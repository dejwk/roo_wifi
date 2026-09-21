#pragma once
#include <vector>

#include "roo_scheduler.h"
#include "roo_wifi/hal/interface.h"
#include "roo_wifi/hal/store.h"

namespace roo_wifi {
/// Coordinates Wi-Fi radio operations, profile persistence, and state
/// notifications.
/// All calls and destruction run on the supplied scheduler context. Listener
/// callbacks may admit follow-up requests, whose execution is deferred. Do not
/// dispatch the scheduler recursively or destroy/register/remove listeners in
/// notification. Radio, scan and storage slots are independent and bounded.
/// After an unsettled timeout, construct fresh dependencies to recover; native
/// admissions remain closed while profile operations remain available.
class Controller : private Interface::Sink {
 public:
  /// Configures controller capacity, timeouts, and optional startup connection.
  struct Options {
    /// Profile to connect after startup; zero disables automatic selection.
    ProfileId startup_profile = 0;

    /// Maximum records retained from one scan.
    uint16_t max_scan_results = 100;

    /// Deadlines in milliseconds for scan, connection, and state transitions.
    uint32_t scan_timeout_ms = 15000;
    uint32_t connect_timeout_ms = 30000;
    uint32_t transition_timeout_ms = 5000;
  };

  /// Returns immediate operation admission status and its deferred-completion
  /// ID.
  struct RequestResult {
    /// Nonzero admitted operation ID; zero means no completion callback
    /// follows.
    OperationId id = 0;

    /// Immediate admission or validation result.
    Status status = Status::kOk;
  };

  /// Borrows records from the controller's latest successful scan publication.
  /// The records remain valid until the next successful publication or
  /// shutdown.
  struct ScanSnapshot {
    /// Monotonically increasing publication generation.
    uint64_t generation = 0;

    /// Borrowed contiguous scan records.
    const ScanRecord *records = nullptr;

    /// Number of records available through @p records.
    size_t count = 0;

    /// Whether the radio found more records than the configured capacity.
    bool truncated = false;
  };

  /// Receives deferred controller state and operation notifications.
  class Listener {
   public:
    /// Destroys a previously unregistered listener.
    virtual ~Listener() = default;

    /// Notifies publication of a new borrowed snapshot.
    virtual void onScanChanged() {}

    /// Reports whether scanning has started or stopped.
    /// @param scanning True while a scan is pending.
    virtual void onScanStateChanged(bool scanning) {}

    /// Reports the observed physical radio state.
    /// @param enabled True when the radio is enabled.
    virtual void onEnabledChanged(bool enabled) {}

    /// Reports association, address readiness, or a later link-state change.
    /// @param state The current observed link diagnostics.
    virtual void onLinkChanged(const LinkState &state) {}

    /// Invalidates loaded profile metadata after a save/remove outcome.
    virtual void onProfilesChanged() {}

    /// Delivers the terminal result for an admitted operation exactly once.
    /// @param result The completed operation and its outcome.
    virtual void onOperationFinished(const OperationResult &result) {}
  };

  /// Creates a controller using the supplied radio, persistence, and scheduler.
  /// @param interface Radio adapter that outlives this controller.
  /// @param store Persistence adapter that outlives this controller.
  /// @param scheduler Context on which calls and callbacks run.
  Controller(Interface &interface, Store &store,
             roo_scheduler::Scheduler &scheduler);

  /// Creates a controller with explicit capacity, timeout, and startup options.
  /// @param options Capacity, timeout, and startup behavior.
  Controller(Interface &interface, Store &store,
             roo_scheduler::Scheduler &scheduler, Options options);

  /// Shuts down the controller without notifying listeners.
  ~Controller();

  /// Prevents copying a controller because it owns active operation state.
  Controller(const Controller &) = delete;

  /// Prevents assignment because a controller owns active operation state.
  Controller &operator=(const Controller &) = delete;

  /// Initializes storage/radio; restores enablement asynchronously before
  /// startup selection.
  Status begin();

  /// Closes admission, cancels public work, and prevents subsequent callbacks.
  void shutdown();

  /// Registers a listener for subsequent controller notifications.
  /// @param listener Listener that remains alive until removed.
  void addListener(Listener &listener);

  /// Stops delivering notifications to a registered listener.
  /// @param listener Listener to remove before its destruction.
  void removeListener(Listener &listener);

  /// Returns actual hardware support, independently of consumer presentation.
  Support support() const;

  /// Returns observed physical radio state, independently of persistence
  /// outcomes.
  bool isEnabled() const;

  /// Returns whether a scan request is pending.
  bool isScanning() const;

  /// Borrows records until the next successful scan or shutdown; does not
  /// allocate.
  ScanSnapshot scanSnapshot() const;

  /// Returns current link diagnostics, without internet reachability claims.
  LinkState linkState() const;

  /// Loads non-secret settings for a known profile key.
  /// @param id Nonzero application-assigned profile key.
  /// @param out Receives settings on success and is unchanged on failure.
  Status loadProfile(ProfileId id, Profile &out) const;

  /// Calls `visitor` once for every persisted saved-profile ID.
  ///
  /// The order is unspecified. Return false to stop early, in which case this
  /// returns Status::kStopped. Profile reads are allowed from the visitor, but
  /// the store must not be modified during the call. A profile whose data was
  /// later corrupted is still enumerated; loadProfile() reports
  /// that read failure independently.
  template <typename Visitor>
  Status forEachProfile(Visitor &&visitor) const {
    if (lifecycle_ != Lifecycle::kRunning) return Status::kNotStarted;
    return store_.forEachProfile(visitor);
  }

  /// Requests a physical radio enablement transition and persists its outcome.
  /// @param enabled Desired physical radio state.
  RequestResult setEnabled(bool enabled);

  /// Admits a bounded scan; rejection has ID zero and no completion callback.
  RequestResult scan();

  /// Starts a connection using copied configuration and credentials.
  /// @param config Network and IP configuration to copy.
  /// @param credential Credential material to copy for this attempt.
  RequestResult connect(const ConnectionConfig &config,
                        const Credentials &credential);

  /// Starts a connection from a saved profile.
  /// @param id Nonzero profile key whose settings and credentials are loaded.
  RequestResult connect(ProfileId id);

  /// Disconnects the active link through its native lifecycle.
  RequestResult disconnect();

  /// Cancels a live request and delivers its original ID a deferred result.
  /// @param target ID of the operation to cancel.
  Status cancel(OperationId target);

  /// Saves settings and a credential update under a known profile key.
  /// @param id Nonzero profile key to create or replace.
  /// @param settings Non-secret settings to copy.
  /// @param credential Credential action and replacement material to copy.
  RequestResult saveProfile(ProfileId id, const ProfileSettings &settings,
                            const CredentialUpdate &credential);

  /// Deletes a saved profile without disconnecting its active link.
  /// @param id Nonzero profile key to remove.
  RequestResult removeProfile(ProfileId id);

 private:
  enum class Lifecycle { kNew, kRunning, kClosed };

  struct Slot {
    enum class State {
      kQueued,
      kCancelledBeforeStart,
      kRunning,
      kCancelling,
      kTimingOut
    };

    OperationResult result;
    State state = State::kQueued;
    roo_time::Uptime deadline;
  };

  /// Admits an operation into an idle slot and schedules its execution.
  RequestResult admit(Slot &, OperationKind, ProfileId = 0);

  /// Reports whether the radio can admit a new operation.
  Status radioAdmission() const;

  /// Finds the live operation slot with the requested ID.
  Slot *find(OperationId);

  /// Executes operations that have been admitted but not started.
  void execute();

  /// Cancels expired native operations and settles expired cancellations.
  void checkTimeouts();

  /// Schedules timeout processing for the earliest active radio deadline.
  void scheduleTimeoutCheck();

  /// Settles a slot and notifies listeners of its terminal result.
  void finish(Slot &, Status, int32_t = 0, bool = false);

  /// Closes the controller and optionally settles public work.
  void close(bool notify);

  /// Starts the configured automatic profile connection when eligible.
  void startProfile();

  /// Handles a terminal operation result from the radio adapter.
  void onOperationFinished(const OperationResult &) override;

  /// Publishes an observed link-state change from the radio adapter.
  void onLinkChanged(const LinkState &) override;

  /// Publishes and persists an observed radio enablement change.
  void onEnabledChanged(bool) override;

  Interface &interface_;
  Store &store_;
  roo_scheduler::Scheduler &scheduler_;
  Options options_;
  roo_scheduler::SingletonTask work_;
  roo_scheduler::SingletonTask timer_;
  roo_scheduler::SingletonTask reconnect_;
  std::vector<Listener *> listeners_;
  std::vector<ScanRecord> records_;
  ScanSnapshot snapshot_;
  LinkState link_;
  Slot station_;
  Slot scan_;
  Slot write_;
  ConnectionConfig config_;
  Credentials credentials_;
  ProfileSettings settings_;
  CredentialUpdate update_;
  OperationId next_id_ = 1;
  ProfileId reconnect_profile_ = 0;
  Lifecycle lifecycle_ = Lifecycle::kNew;
  bool enabled_ = false;
  bool faulted_ = false;
  bool desired_enabled_ = false;
};

using Listener = Controller::Listener;
using RequestResult = Controller::RequestResult;
using ScanSnapshot = Controller::ScanSnapshot;

}  // namespace roo_wifi
