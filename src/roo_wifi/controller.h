#pragma once

#include <vector>

#include "roo_scheduler.h"
#include "roo_wifi/hal/interface.h"
#include "roo_wifi/hal/store.h"

namespace roo_wifi {

/// Coordinates desired station state, scanning, and profile persistence.
/// All calls, reads, listener registration, and destruction use the supplied
/// scheduler context. Interface hands native events off to that context.
/// Listeners may submit requests, whose native execution is deferred. Do not
/// dispatch the scheduler recursively or destroy/register/remove listeners in
/// notification. A faulted controller requires fresh dependencies to recover;
/// synchronous profile operations remain available until shutdown.
class Controller : private Interface::Sink {
 public:
  /// Configures bounded scan capacity and native transition deadlines.
  struct Options {
    /// Maximum number of access-point records retained from one scan.
    /// Additional results are omitted and ScanSnapshot::truncated is set.
    uint16_t max_scan_results = 100;

    /// Maximum time to wait for a scan result, in milliseconds.
    uint32_t scan_timeout_ms = 15000;

    /// Maximum time to wait for connection address readiness, in milliseconds.
    uint32_t connect_timeout_ms = 30000;

    /// Maximum time, in milliseconds, for native teardown or enablement.
    /// Applies to enablement, disconnection, and cancellation. If the native
    /// transition does not settle, the controller enters kFaulted.
    uint32_t transition_timeout_ms = 5000;
  };

  /// Selects the desired station state, independently of physical progress.
  enum class Target { kDisabled, kDisconnected, kConnected };

  /// Reports observed progress, including native teardown and faults.
  enum class StationPhase {
    kDisabled,
    kEnabling,
    kIdle,
    kConnecting,
    kAwaitingIp,
    kConnected,
    kDisconnecting,
    kDisabling,
    kFaulted
  };

  /// Reports whether a scan is queued, active, or awaiting cancellation.
  enum class ScanPhase { kIdle, kQueued, kRunning, kCancelling };

  /// Describes the current station, scan, and profile-persistence state.
  /// This is a snapshot, not an event history. Outcome fields retain the
  /// latest result until a new request. Native identities only correlate link
  /// observations; callers never need an operation ID to issue a request.
  struct State {
    /// Most recent accepted station intent.
    Target desired = Target::kDisabled;

    /// Current physical station progress.
    StationPhase station = StationPhase::kDisabled;

    /// Observed radio enablement, even when persisting it failed.
    bool enabled = false;

    /// Observed association, addresses, and native diagnostics.
    LinkState link;

    /// Saved profile selected by the current intent, or zero for a direct call.
    ProfileId desired_profile = 0;

    /// Saved profile that reached address readiness, or zero otherwise.
    ProfileId connected_profile = 0;

    /// Revision identifying the accepted station intent.
    /// Increments when that intent is replaced or explicitly retried.
    uint64_t revision = 0;

    /// Latest station or enablement-persistence outcome; Ok is not a claim of
    /// address readiness. Read station/link to determine connectivity.
    Status status = Status::kOk;

    /// Platform diagnostic associated with the latest station outcome.
    int32_t native_code = 0;

    /// Whether native_code was supplied by the adapter.
    bool has_native_code = false;

    /// Current scan progress; cancellation stays active until native teardown.
    ScanPhase scan = ScanPhase::kIdle;

    /// Latest scan outcome, reset to Ok when a scan is accepted.
    Status scan_status = Status::kOk;

    /// Revision identifying the most recently accepted scan request.
    /// Increments even for scans that subsequently fail or are cancelled.
    uint64_t scan_revision = 0;

    /// Generation identifying changes to saved-profile storage.
    /// Increments after each write/delete attempt, including partial failures.
    uint64_t profiles_generation = 0;
  };

  /// Provides the access points found by the latest successful scan.
  /// Records are borrowed and remain valid until the next successful
  /// publication or shutdown.
  struct ScanSnapshot {
    /// Identifies the successful scan that produced these records.
    /// Zero means no successful scan has been published. A successful empty
    /// scan also increments the generation; failures and cancellation do not.
    uint64_t generation = 0;

    /// Access-point records published by the scan.
    /// Points to borrowed contiguous storage; only the first @p count entries
    /// are valid. Copy those entries if they must survive a later scan.
    const ScanRecord* records = nullptr;

    /// Number of records available through records.
    size_t count = 0;

    /// Whether the radio found more records than configured capacity.
    bool truncated = false;
  };

  /// Receives deferred state invalidation on the controller's scheduler
  /// context. Each category is coalesced independently. Callbacks never run
  /// on native event threads; changes made inside callbacks are deferred.
  /// A notification means that the corresponding state should be read again,
  /// not that exactly one transition occurred. Override only the categories
  /// of interest. Several categories may be delivered in the same batch.
  class Listener {
   public:
    /// Destroys a previously unregistered listener.
    virtual ~Listener() = default;

    /// Reports desired station, enablement, link progress, or station errors.
    /// Read state() for the latest values; intermediate states may be skipped.
    virtual void onStationStateChanged() {}

    /// Reports scan progress, completion, cancellation, errors, or new results.
    /// Read state() and scanSnapshot() for the latest values.
    virtual void onScanStateChanged() {}

    /// Reports saved-profile changes, including potentially partial writes.
    /// Reload profiles explicitly to obtain their latest contents. The initial
    /// profile notification is scheduled by begin(); subsequent notifications
    /// follow save/delete attempts, even when the store reports a failure.
    virtual void onProfilesChanged() {}
  };

  /// Creates a controller using borrowed radio @p interface, profile @p store,
  /// and @p scheduler with default scan capacity and timeouts.
  /// All three dependencies must outlive the controller. Construction does
  /// not start the radio; call begin() to initialize it and dispatch the
  /// scheduler to advance asynchronous work.
  /// @param interface Radio adapter used for station and scan operations.
  /// @param store Storage used for profiles and persisted radio enablement.
  /// @param scheduler Context for all controller calls and notifications.
  Controller(Interface& interface, Store& store,
             roo_scheduler::Scheduler& scheduler);

  /// Creates a controller using borrowed radio @p interface, profile @p store,
  /// and @p scheduler, configured by @p options.
  /// All three dependencies must outlive the controller; options are copied.
  /// Construction reserves scan-result storage but does not start the radio.
  /// Call begin() and dispatch the scheduler to advance asynchronous work.
  /// @param interface Radio adapter used for station and scan operations.
  /// @param store Storage used for profiles and persisted radio enablement.
  /// @param scheduler Context for all controller calls and notifications.
  /// @param options Scan capacity and native transition timeout settings.
  Controller(Interface& interface, Store& store,
             roo_scheduler::Scheduler& scheduler, Options options);

  /// Destroys the controller and shuts down its radio adapter.
  /// Detaches native callbacks and cancels notifications without calling users.
  ~Controller();

  /// Prevents copying owned asynchronous state.
  Controller(const Controller&) = delete;

  /// Prevents assigning owned asynchronous state.
  Controller& operator=(const Controller&) = delete;

  /// Initializes the controller and restores its persisted Wi-Fi settings.
  /// Opens the store and attaches the radio adapter synchronously, then
  /// schedules restoration of enablement. If enabled, it also restores the
  /// last successful profile when that profile allows automatic connection.
  /// Register listeners first to observe these asynchronous changes.
  /// @return kOk when initialization succeeds, not when a connection is ready;
  /// kBusy if already running, kNotStarted after shutdown, or a dependency's
  /// initialization error. A shut-down controller cannot be restarted.
  Status begin();

  /// Stops the controller and releases its attachment to the radio adapter.
  /// Cancels queued work and notifications, clears connection credentials and
  /// scan results, and prevents subsequent callbacks. Does not destroy the
  /// borrowed dependencies. Repeated calls have no effect; begin() cannot
  /// restart this instance afterward.
  void shutdown();

  /// Registers @p listener to receive subsequent state-change notifications.
  /// Registration does not immediately report existing state; use state()
  /// and scanSnapshot() to read it. Registering the same listener twice has
  /// no effect. Do not register or remove listeners during notification.
  /// @param listener Borrowed receiver that must remain alive until removed
  /// or until this controller is destroyed.
  void addListener(Listener& listener);

  /// Unregisters @p listener so it receives no further notifications.
  /// Removing an unregistered listener has no effect. Call outside notification
  /// delivery and before destroying a listener while the controller survives.
  /// @param listener Previously registered receiver to detach.
  void removeListener(Listener& listener);

  /// Returns actual hardware support, independently of presentation policy.
  Support support() const { return interface_.support(); }

  /// Returns a snapshot of the desired and observed controller state.
  /// Copies the state without allocating. The returned value is independent of
  /// later updates. Desired intent may differ from physical progress while a
  /// transition is pending; inspect station and link for connection readiness.
  State state() const;

  /// Returns observed physical radio state, not requested enablement.
  bool isEnabled() const { return state_.enabled; }

  /// Returns true for queued, running, or cancelling scans.
  bool isScanning() const { return state_.scan != ScanPhase::kIdle; }

  /// Returns link diagnostics without claiming internet reachability.
  LinkState linkState() const { return state_.link; }

  /// Returns the access points found by the latest successful scan.
  /// The snapshot borrows its records and is returned without allocating.
  /// Records remain valid until the next successful scan publication or
  /// shutdown(). A failed or cancelled scan preserves the previous snapshot;
  /// inspect state().scan_status for that attempt's outcome. A zero generation
  /// means no scan has succeeded, whereas count == 0 can be a valid result.
  ScanSnapshot scanSnapshot() const { return snapshot_; }

  /// Loads the saved profile identified by @p id.
  /// Returns non-secret settings and credential presence.
  /// @param id Nonzero application-assigned profile key.
  /// @param out Receives the profile on success and is unchanged on failure.
  Status loadProfile(ProfileId id, Profile& out) const;

  /// Visits each saved profile ID in unspecified order. False stops enumeration
  /// with kStopped. Reads are allowed in the visitor; writes are not. Corrupted
  /// profiles remain enumerable, and loadProfile reports their errors.
  /// @param visitor Callable receiving a ProfileId and returning true to
  /// continue or false to stop. Called synchronously on the caller's context.
  /// @return kOk after visiting all IDs, kStopped on early termination,
  /// kNotStarted before begin()/after shutdown(), or a storage error.
  template <typename Visitor>
  Status forEachProfile(Visitor&& visitor) const {
    if (!running_) return Status::kNotStarted;
    return store_.forEachProfile(visitor);
  }

  /// Requests that the Wi-Fi station be enabled or disabled.
  /// Returns admission, not physical completion. Disabling interrupts
  /// scanning/connecting. A new enable cycle restores the last
  /// successful auto-connect profile. Repeating a healthy intent is a no-op.
  /// @param enabled True to enable the station, false to disable it.
  /// @return kOk if the desired state was accepted. Observe
  /// Listener::onStationStateChanged() and state() for progress and failures.
  Status setEnabled(bool enabled);

  /// Requests a connection using @p config and @p credentials.
  /// Copies settings and credentials at admission.
  /// Accepted during disconnection or scanning; native teardown settles first.
  /// Supersedes older intent without queuing a history of requests. Identical
  /// healthy intent is a no-op; repeating failed intent retries. Direct calls
  /// never save a profile and do not automatically reconnect after link loss.
  /// @param config Network, address, and MAC settings to copy.
  /// @param credentials Authentication material to copy; may be discarded by
  /// the caller after this method returns.
  /// @return kOk when intent is accepted, kDisabled when disabled by intent,
  /// or an admission/validation error. Acceptance does not imply association
  /// or an IP address; observe state() through station notifications.
  Status connect(const ConnectionConfig& config,
                 const Credentials& credentials);

  /// Requests a connection using the saved profile identified by @p id.
  /// Loads and copies the profile at admission. Later profile edits do not
  /// change the accepted attempt. Its auto_connect flag controls retries.
  /// @param id Nonzero key of the profile to load, including its credentials.
  /// @return kOk when intent is accepted, a storage error if the profile
  /// cannot be loaded, or the same admission/validation errors as direct
  /// connect(). Completion is reported through station-state notifications.
  Status connect(ProfileId id);

  /// Requests disconnection from the network or cancellation of connection.
  /// Updates intent to idle and suppresses automatic reconnect until another
  /// explicit connection or enable cycle. Repeated requests are idempotent.
  /// The radio remains enabled unless disabling was already requested.
  /// @return kOk when intent is accepted, not when native teardown finishes.
  /// Observe state() through station notifications for physical completion.
  Status disconnect();

  /// Requests a scan for nearby access points.
  /// Retains at most Options::max_scan_results records. Requires an enabled
  /// radio; scanning while connected also requires adapter support.
  /// @return kOk when queued, kBusy during another scan or station transition
  /// (or when scanning while connected is unsupported), kDisabled when the
  /// radio is disabled, or another admission error.
  /// Read state().scan and state().scan_status in
  /// Listener::onScanStateChanged() to follow progress. On success,
  /// scanSnapshot() publishes a new generation; failed or cancelled scans
  /// preserve the previous successful snapshot.
  Status startScan();

  /// Requests scan cancellation; native completion remains asynchronous.
  /// Idempotent while cancelling or idle. A queued scan never reaches hardware.
  /// An active scan remains in kCancelling until the adapter confirms teardown;
  /// scan-state notifications report completion. The previous snapshot remains
  /// available.
  /// @return kOk if cancellation was accepted or unnecessary, kNotStarted
  /// when not running, or the adapter's cancellation error.
  Status cancelScan();

  /// Creates or updates profile @p id with @p settings and @p credentials.
  /// Saves synchronously and returns the storage outcome; never cancellable.
  /// Invalidates profile state asynchronously, including partial failures.
  /// @param id Nonzero application-assigned profile key.
  /// @param settings Non-secret profile settings to persist.
  /// @param credentials Explicit credential action and replacement material.
  /// @return The completed storage outcome. A failure can represent a partial
  /// write; reload the profile after onProfilesChanged() rather than assuming
  /// the old contents survived unchanged.
  Status saveProfile(ProfileId id, const ProfileSettings& settings,
                     const CredentialUpdate& credentials);

  /// Removes the saved profile identified by @p id.
  /// Deletes synchronously without disconnecting a link using that profile.
  /// Returns the storage outcome and invalidates profile state asynchronously.
  /// @param id Nonzero key of the profile to remove.
  /// @return The completed storage outcome, or an argument/lifecycle error.
  /// A failed delete may be partial; reload after onProfilesChanged().
  Status removeProfile(ProfileId id);

 private:
  enum class Change : uint8_t { kStation = 1, kScan = 2, kProfiles = 4 };

  enum class Transition { kNone, kEnable, kConnect, kDisconnect };

  /// Rejects native admission after shutdown, faults, or identity exhaustion.
  Status admission() const;

  /// Schedules one coalesced public invalidation.
  void changed(Change change);

  /// Delivers the invalidation after the current scheduler task returns.
  void notifyListeners();

  /// Replaces intent, cancels any retry timer, and schedules reconciliation.
  void newIntent();

  /// Advances native work toward current intent, never overlapping teardown.
  void execute();

  /// Cancels timed-out work or faults an unsettled native transition.
  void checkTimeouts();

  /// Arms the earliest active native deadline, without periodic polling.
  void scheduleTimeouts();

  /// Closes native admission when physical state cannot safely be reconciled.
  void fault(Status status);

  /// Restores the last successful profile only when auto-connect is enabled.
  void restoreProfile();

  /// Retries the accepted profile snapshot after checking current retry policy.
  void retry();

  /// Validates and replaces connection intent with an owned input snapshot.
  Status requestConnection(const ConnectionConfig& config,
                           const Credentials& credentials, ProfileId profile,
                           bool automatic);

  /// Retires one internal native transition and schedules reconciliation.
  void onOperationFinished(const OperationResult& result) override;

  /// Accepts observed link updates with a current native identity.
  void onLinkChanged(const LinkState& link) override;

  /// Records physical enablement independently of persistence success.
  void onEnabledChanged(bool enabled) override;

  Interface& interface_;
  Store& store_;
  roo_scheduler::Scheduler& scheduler_;
  Options options_;

  roo_scheduler::SingletonTask work_;
  roo_scheduler::SingletonTask timer_;
  roo_scheduler::SingletonTask reconnect_;
  roo_scheduler::SingletonTask notify_;
  std::vector<Listener*> listeners_;
  uint8_t pending_changes_ = 0;

  std::vector<ScanRecord> records_;
  State state_;
  ScanSnapshot snapshot_;
  ConnectionConfig config_;
  Credentials credentials_;

  Transition transition_ = Transition::kNone;
  OperationId next_id_ = 1;
  OperationId station_id_ = 0;
  OperationId scan_id_ = 0;
  uint64_t active_revision_ = 0;
  uint64_t attempted_revision_ = 0;
  roo_time::Uptime station_deadline_;
  roo_time::Uptime scan_deadline_;

  bool running_ = false;
  bool closed_ = false;
  bool initialized_ = false;
  bool faulted_ = false;
  bool cancelling_connect_ = false;
  bool station_timed_out_ = false;
  bool scan_timed_out_ = false;
  bool transition_enabled_ = false;
  bool restore_profile_ = false;
  bool auto_connect_ = false;
  bool retry_waiting_ = false;
};

using Listener = Controller::Listener;
using ScanSnapshot = Controller::ScanSnapshot;

}  // namespace roo_wifi
