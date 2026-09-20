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
  /// @param options Capacity, timeout, and startup behavior.
  Controller(Interface &interface, Store &store,
             roo_scheduler::Scheduler &scheduler,
             ControllerOptions options = {});

  /// Shuts down the controller without notifying listeners.
  ~Controller();

  /// Prevents copying a controller because it owns active operation state.
  Controller(const Controller &) = delete;
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
  struct Slot {
    OperationResult result;
    bool started = false, cancelled = false, timed_out = false;
    roo_time::Uptime deadline;
  };

  RequestResult admit(Slot &, OperationKind, ProfileId = 0);
  Status radioAdmission() const;
  Slot *find(OperationId);
  void execute();
  void checkTimeouts();
  void finish(Slot &, Status, int32_t = 0, bool = false);
  void close(bool notify);
  void startProfile();
  void onOperationFinished(const OperationResult &) override;
  void onLinkChanged(const LinkState &) override;
  void onEnabledChanged(bool) override;

  Interface &interface_;
  Store &store_;
  roo_scheduler::Scheduler &scheduler_;
  ControllerOptions options_;
  roo_scheduler::SingletonTask work_, timer_, reconnect_;
  std::vector<Listener *> listeners_;
  std::vector<ScanRecord> records_;
  ScanSnapshot snapshot_;
  LinkState link_;
  Slot station_, scan_, write_;
  ConnectionConfig config_;
  Credentials credentials_;
  ProfileSettings settings_;
  CredentialUpdate update_;
  OperationId next_id_ = 1;
  ProfileId reconnect_profile_ = 0;
  bool started_ = false, closed_ = false, enabled_ = false;
  bool faulted_ = false, desired_enabled_ = false;
};

}  // namespace roo_wifi
