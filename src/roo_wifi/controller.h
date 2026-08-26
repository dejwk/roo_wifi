/// @file
/// @brief High-level Wi-Fi controller and observable network model.

#pragma once

#include <inttypes.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "roo_collections/flat_small_hash_set.h"
#include "roo_scheduler.h"
#include "roo_threads.h"
#include "roo_wifi/hal/interface.h"
#include "roo_wifi/hal/store.h"

namespace roo_wifi {

/// @brief Manages Wi-Fi enablement, scans, profiles, and connections.
/// @ingroup roo_wifi
///
/// `Controller` coordinates a platform `Interface`, persistent `Store`, and
/// scheduler. It maintains a scan model for user interfaces and serializes
/// native interface callbacks onto the supplied scheduler.
class Controller {
 public:
  /// @brief Summary of a scanned or currently selected network.
  struct Network {
    /// @brief Constructs an empty network with the minimum RSSI value.
    Network() : ssid(), open(false), rssi(-128) {}

    std::string ssid;  ///< Service set identifier.
    bool open;         ///< Whether the network requires no password.
    int8_t rssi;       ///< Received signal strength in dBm.
  };

  /// @brief Observes changes to the controller model.
  ///
  /// Callbacks run on the controller's scheduler context. A listener must
  /// remain alive from `addListener()` until `removeListener()`.
  class Listener {
   public:
    /// @brief Constructs a listener.
    Listener() = default;
    /// @brief Virtual destructor.
    virtual ~Listener() = default;

    /// @brief Called after the enabled state changes.
    /// @param enabled The new enabled state.
    virtual void onEnableChanged(bool enabled) {}
    /// @brief Called after an asynchronous scan starts.
    virtual void onScanStarted() {}
    /// @brief Called after scan results have been incorporated into the model.
    virtual void onScanCompleted() {}
    /// @brief Called after the selected network or its status changes.
    virtual void onCurrentNetworkChanged() {}
    /// @brief Called after a native connection event updates the model.
    /// @param type The interface event that was processed.
    virtual void onConnectionStateChanged(Interface::EventType type) {}

   private:
    friend class Controller;
  };

  /// @brief Creates a controller over caller-owned dependencies.
  /// @param store Persistent configuration and credential store.
  /// @param interface Platform Wi-Fi interface.
  /// @param scheduler Scheduler used for refreshes and event dispatch.
  ///
  /// All dependencies must outlive the controller.
  Controller(Store& store, Interface& interface,
             roo_scheduler::Scheduler& scheduler);

  /// @brief Cancels scheduled work and detaches from the interface.
  virtual ~Controller();

  /// @brief Copy construction is disabled.
  Controller(const Controller&) = delete;
  /// @brief Copy assignment is disabled.
  Controller& operator=(const Controller&) = delete;
  /// @brief Move construction is disabled.
  Controller(Controller&&) = delete;
  /// @brief Move assignment is disabled.
  Controller& operator=(Controller&&) = delete;

  /// @brief Initializes persisted state and registers for interface events.
  void begin();

  /// @brief Adds a listener for controller model events.
  /// @param listener Non-null listener that remains alive until removed.
  void addListener(Listener* listener);

  /// @brief Removes a previously added listener.
  /// @param listener Listener to remove.
  void removeListener(Listener* listener);

  /// @brief Returns the number of scanned networks other than the current one.
  int otherScannedNetworksCount() const;

  /// @brief Returns the current connection target and its scan metadata.
  ///
  /// The SSID may be empty when no default or selected network exists.
  const Network& currentNetwork() const;

  /// @brief Finds a network in the latest scan results.
  /// @param ssid SSID to find.
  /// @return Network metadata, or `nullptr` when not present.
  const Network* lookupNetwork(const std::string& ssid) const;

  /// @brief Returns the status associated with `currentNetwork()`.
  ConnectionStatus currentNetworkStatus() const;

  /// @brief Returns a scanned network other than the current network.
  /// @param idx Zero-based index less than `otherScannedNetworksCount()`.
  const Network& otherNetwork(int idx) const;

  /// @brief Starts an asynchronous network scan.
  /// @return `true` when the scan request was accepted.
  bool startScan();

  /// @brief Reports whether the interface's current scan has completed.
  bool isScanCompleted() const { return interface_.scanCompleted(); }
  /// @brief Reports whether Wi-Fi is enabled.
  bool isEnabled() const { return enabled_; }

  /// @brief Reports whether a connection attempt is in progress.
  bool isConnecting() const { return connecting_; }

  /// @brief Toggles Wi-Fi and persists the resulting enabled state.
  void toggleEnabled();

  /// @brief Notifies listeners of the current enabled state.
  void notifyEnableChanged();

  /// @brief Retrieves a stored password for an SSID.
  /// @param ssid SSID whose password should be retrieved.
  /// @param passwd Destination populated on success.
  /// @return `true` when a stored password exists.
  bool getStoredPassword(const std::string& ssid, std::string& passwd) const;

  /// @brief Suspends scans, periodic refresh, and interface event processing.
  void pause();

  /// @brief Resumes refresh and scanning when Wi-Fi is enabled.
  void resume();

  /// @brief Stores or replaces a password for an SSID.
  /// @param ssid SSID whose profile should be updated.
  /// @param passwd Password to store.
  void setPassword(const std::string& ssid, const std::string& passwd);

  /// @brief Starts connecting using stored SSID/password values.
  ///
  /// Returns true when the attempt was accepted. Its eventual result is
  /// delivered asynchronously through the controller listeners.
  /// @return `true` when a default SSID existed and the request was accepted.
  bool connect();

  /// @brief Starts connecting to the specified SSID/password.
  ///
  /// Returns true when the attempt was accepted. Its eventual result is
  /// delivered asynchronously through the controller listeners.
  /// @param ssid SSID to connect to.
  /// @param passwd Password, or an empty string for an open network.
  /// @return `true` when the request was accepted.
  bool connect(const std::string& ssid, const std::string& passwd);

  /// @brief Cancels any pending attempt and requests disconnection.
  void disconnect();

  /// @brief Removes a network's stored password and default association.
  /// @param ssid SSID whose saved profile should be forgotten.
  void forget(const std::string& ssid);

 protected:
  /// @brief Stops controller activity and detaches the interface listener.
  ///
  /// Safe to call more than once. Derived classes that own the interface must
  /// call this before destroying it.
  void shutdown();

 private:
  class WifiListener : public Interface::EventListener {
   public:
    WifiListener(Controller& wifi) : wifi_(wifi) {}

    void onEvent(Interface::EventType type, roo::string_view ssid) override {
      wifi_.enqueueInterfaceEvent(type, ssid);
    }

   private:
    Controller& wifi_;
  };

  friend class WifiListener;

  struct EventDispatchState {
    explicit EventDispatchState(Controller* controller)
        : controller(controller) {}

    roo::mutex mutex;
    Controller* controller;
  };

  void enqueueInterfaceEvent(Interface::EventType type, roo::string_view ssid);
  void onInterfaceEvent(Interface::EventType type, const std::string& ssid,
                        uint64_t connection_generation);

  void onConnectionStateChanged(Interface::EventType type,
                                const std::string& ssid,
                                uint64_t connection_generation);

  void periodicRefreshCurrentNetwork();

  void refreshCurrentNetwork();

  void updateCurrentNetwork(const std::string& ssid, bool open, int8_t rssi,
                            ConnectionStatus status, bool force_notify);

  void onScanCompleted();

  Store& store_;
  Interface& interface_;
  roo_scheduler::Scheduler& scheduler_;
  std::shared_ptr<EventDispatchState> event_dispatch_state_;
  bool enabled_;
  Network current_network_;
  int16_t current_network_index_;
  ConnectionStatus current_network_status_;
  std::vector<Network> all_networks_;
  WifiListener wifi_listener_;
  roo_collections::FlatSmallHashSet<Listener*> model_listeners_;
  bool connecting_;
  // Snapshot the target and generation with each queued event. The target is
  // retained after terminal events so later events from the previous AP do not
  // replace the result of the most recent connection attempt.
  std::string connection_target_ssid_;
  uint64_t connection_generation_;
  roo::mutex connection_state_mutex_;
  bool listener_attached_;
  bool paused_;

  roo_scheduler::SingletonTask start_scan_;
  roo_scheduler::SingletonTask refresh_current_network_;
};

}  // namespace roo_wifi
