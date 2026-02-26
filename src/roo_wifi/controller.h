#pragma once

#include <inttypes.h>

#include <algorithm>
#include <string>
#include <vector>

#include "roo_collections/flat_small_hash_set.h"
#include "roo_scheduler.h"
#include "roo_wifi/hal/interface.h"
#include "roo_wifi/hal/store.h"

namespace roo_wifi {

/// High-level Wi-Fi controller that manages scanning and connections.
class Controller {
 public:
  /// Summary of a scanned network.
  struct Network {
    Network() : ssid(), open(false), rssi(-128) {}

    std::string ssid;
    bool open;
    int8_t rssi;
  };

  /// Listener for controller events.
  class Listener {
   public:
    Listener() = default;
    virtual ~Listener() = default;

    virtual void onEnableChanged(bool enabled) {}
    virtual void onScanStarted() {}
    virtual void onScanCompleted() {}
    virtual void onCurrentNetworkChanged() {}
    virtual void onConnectionStateChanged(Interface::EventType type) {}

   private:
    friend class Controller;
  };

  /// Creates a controller using the provided store, interface, and scheduler.
  Controller(Store& store, Interface& interface,
             roo_scheduler::Scheduler& scheduler);

  /// Destroys the controller and detaches listeners.
  ~Controller();

  /// Initializes the controller and registers for interface events.
  void begin();

  /// Adds a listener for controller events.
  void addListener(Listener* listener);

  /// Removes a previously added listener.
  void removeListener(Listener* listener);

  /// Returns the number of non-current networks in the scan list.
  int otherScannedNetworksCount() const;

  /// Returns the current network (may be empty if disconnected).
  const Network& currentNetwork() const;

  /// Returns a network by SSID, or nullptr if not found.
  const Network* lookupNetwork(const std::string& ssid) const;

  /// Returns the connection status of the current network.
  ConnectionStatus currentNetworkStatus() const;

  /// Returns the ith non-current network in the scan list.
  const Network& otherNetwork(int idx) const;

  /// Starts a scan. Returns false if a scan could not be started.
  bool startScan();

  /// Returns true when the current scan has completed.
  bool isScanCompleted() const { return interface_.scanCompleted(); }
  /// Returns true when the interface is enabled.
  bool isEnabled() const { return enabled_; }

  /// Returns true when a connection is in progress.
  bool isConnecting() const { return connecting_; }

  /// Toggles the enabled/disabled state and persists it in the store.
  void toggleEnabled();

  /// Notifies listeners that enable state changed.
  void notifyEnableChanged();

  /// Looks up a stored password for the given SSID.
  bool getStoredPassword(const std::string& ssid, std::string& passwd) const;

  /// Temporarily disables periodic refresh and event processing.
  void pause();

  /// Resumes periodic refresh and event processing.
  void resume();

  /// Stores a password for the given SSID.
  void setPassword(const std::string& ssid, const std::string& passwd);

  /// Connects using stored SSID/password values.
  bool connect();

  /// Connects to the specified SSID/password.
  bool connect(const std::string& ssid, const std::string& passwd);

  /// Disconnects the current connection.
  void disconnect();

  /// Forgets the password and SSID association.
  void forget(const std::string& ssid);

 private:
  class WifiListener : public Interface::EventListener {
   public:
    WifiListener(Controller& wifi) : wifi_(wifi) {}

    void onEvent(Interface::EventType type) {
      switch (type) {
        case Interface::EV_SCAN_COMPLETED: {
          wifi_.onScanCompleted();
          break;
        }
        default: {
          wifi_.onConnectionStateChanged(type);
          break;
        }
      }
    }

   private:
    Controller& wifi_;
  };

  friend class WifiListener;

  void onConnectionStateChanged(Interface::EventType type);

  void periodicRefreshCurrentNetwork();

  void refreshCurrentNetwork();

  void updateCurrentNetwork(const std::string& ssid, bool open, int8_t rssi,
                            ConnectionStatus status, bool force_notify);

  void onScanCompleted();

  Store& store_;
  Interface& interface_;
  bool enabled_;
  Network current_network_;
  int16_t current_network_index_;
  ConnectionStatus current_network_status_;
  std::vector<Network> all_networks_;
  WifiListener wifi_listener_;
  roo_collections::FlatSmallHashSet<Listener*> model_listeners_;
  bool connecting_;

  roo_scheduler::SingletonTask start_scan_;
  roo_scheduler::SingletonTask refresh_current_network_;
};

}  // namespace roo_wifi
