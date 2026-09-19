#pragma once
#include <vector>

#include "roo_scheduler.h"
#include "roo_wifi/hal/interface.h"
#include "roo_wifi/hal/store.h"
namespace roo_wifi {
/// Public backend facade; owns model/operation state, borrows its dependencies.
/// All calls and destruction run on the supplied scheduler context. Listener
/// callbacks may admit follow-up requests, whose execution is deferred. Do not
/// dispatch the scheduler recursively or destroy/register/remove listeners in
/// notification. Radio, scan and storage slots are independent and bounded.
/// After an unsettled timeout, construct fresh dependencies to recover; native
/// admissions remain closed while profile operations remain available.
class Controller : private Interface::Sink {
 public:
  /// Observes deferred callbacks; registration/removal/destruction occur
  /// outside notification.
  class Listener {
   public:
    /// Destroys a previously unregistered listener.
    virtual ~Listener() = default;

    /// Notifies publication of a new borrowed snapshot.
    virtual void onScanChanged() {}

    /// Notifies scan activity transitions on scheduler context.
    virtual void onScanStateChanged(bool scanning) {}

    /// Notifies actual physical radio state.
    virtual void onEnabledChanged(bool enabled) {}

    /// Notifies association, address readiness or subsequent link changes.
    virtual void onLinkChanged(const LinkState &state) {}

    /// Invalidates loaded profile metadata after a save/remove outcome.
    virtual void onProfilesChanged() {}

    /// Delivers exactly one terminal result per admitted request ID.
    virtual void onOperationFinished(const OperationResult &result) {}
  };

  /// Borrows dependencies, which must outlive this scheduler-context object.
  Controller(Interface &interface, Store &store,
             roo_scheduler::Scheduler &scheduler,
             ControllerOptions options = {});

  /// Borrows dependencies, which must outlive this scheduler-context object.
  ~Controller();

  /// Borrows dependencies, which must outlive this scheduler-context object.
  Controller(const Controller &) = delete;
  Controller &operator=(const Controller &) = delete;

  /// Initializes storage/radio; restores enablement asynchronously before
  /// startup selection.
  Error begin();

  /// Closes admission, cancels public work, and prevents subsequent callbacks.
  void shutdown();

  /// Registers a borrowed listener; call outside notification on scheduler
  /// context.
  void addListener(Listener &listener);

  /// Removes a listener before its destruction; call outside notification.
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

  /// Reads a known key without exposing secrets; failure leaves output
  /// unchanged.
  Error loadProfile(ProfileId id, Profile &out) const;

  /// Admits an enable transition; persistence follows confirmed physical
  /// completion.
  RequestResult setEnabled(bool enabled);

  /// Admits a bounded scan; rejection has ID zero and no completion callback.
  RequestResult scan();

  /// Copies connection input on admission; succeeds only at address readiness.
  RequestResult connect(const ConnectionConfig &config,
                        const Credentials &credential);

  /// Copies connection input on admission; succeeds only at address readiness.
  RequestResult connect(ProfileId id);

  /// Copies connection input on admission; succeeds only at address readiness.
  RequestResult disconnect();

  /// Cancels a live request; its original ID receives one deferred terminal
  /// result.
  Error cancel(OperationId target);

  /// Queues known-key persistence; does not connect or require an enabled
  /// radio.
  RequestResult saveProfile(ProfileId id, const ProfileSettings &settings,
                            const CredentialUpdate &credential);

  /// Queues deletion of a known key without disconnecting its active link.
  RequestResult removeProfile(ProfileId id);

 private:
  struct Slot {
    OperationResult result;
    bool started = false, cancelled = false, timed_out = false;
    roo_time::Uptime deadline;
  };
  RequestResult admit(Slot &, OperationKind, ProfileId = 0);
  Error radioAdmission() const;
  Slot *find(OperationId);
  void execute();
  void checkTimeouts();
  void finish(Slot &, Error, int32_t = 0, bool = false);
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
