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

class Controller {
 public:
  struct Network {
    Network() : ssid(), open(false), rssi(-128) {}

    std::string ssid;
    bool open;
    int8_t rssi;
  };

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

  Controller(Store& store, Interface& interface,
             roo_scheduler::Scheduler& scheduler);

  ~Controller();

  void begin();

  void addListener(Listener* listener);

  void removeListener(Listener* listener);

  int otherScannedNetworksCount() const;

  const Network& currentNetwork() const;

  const Network* lookupNetwork(const std::string& ssid) const;

  ConnectionStatus currentNetworkStatus() const;

  const Network& otherNetwork(int idx) const;

  bool startScan();

  bool isScanCompleted() const { return interface_.scanCompleted(); }
  bool isEnabled() const { return enabled_; }

  bool isConnecting() const { return connecting_; }

  void toggleEnabled();

  void notifyEnableChanged();

  bool getStoredPassword(const std::string& ssid, std::string& passwd) const;

  void pause();

  void resume();

  void setPassword(const std::string& ssid, const std::string& passwd);

  bool connect();

  bool connect(const std::string& ssid, const std::string& passwd);

  void disconnect();

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
