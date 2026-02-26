#include "roo_wifi/controller.h"

namespace roo_wifi {

namespace {

ConnectionStatus getConnectionStatus(Interface::EventType type) {
  switch (type) {
    case Interface::EV_CONNECTED:
      return WL_IDLE_STATUS;
    case Interface::EV_GOT_IP:
      return WL_CONNECTED;
    case Interface::EV_DISCONNECTED:
      return WL_DISCONNECTED;
    case Interface::EV_CONNECTION_FAILED:
      return WL_CONNECT_FAILED;
    case Interface::EV_CONNECTION_LOST:
      return WL_CONNECTION_LOST;
    default:
      return WL_CONNECT_FAILED;
  }
}

}  // namespace

Controller::Controller(Store& store, Interface& interface,
                       roo_scheduler::Scheduler& scheduler)
    : store_(store),
      interface_(interface),
      enabled_(false),
      current_network_(),
      current_network_index_(-1),
      current_network_status_(WL_NO_SSID_AVAIL),
      all_networks_(),
      wifi_listener_(*this),
      model_listeners_(),
      connecting_(false),
      start_scan_(scheduler, [this]() { startScan(); }),
      refresh_current_network_(scheduler,
                               [this]() { periodicRefreshCurrentNetwork(); }) {}

Controller::~Controller() { interface_.removeEventListener(&wifi_listener_); }

void Controller::begin() {
  interface_.addEventListener(&wifi_listener_);
  enabled_ = store_.getIsInterfaceEnabled();
  if (enabled_) notifyEnableChanged();
  std::string ssid = store_.getDefaultSSID();
  if (enabled_ && !ssid.empty()) {
    connect();
  }
}

void Controller::addListener(Listener* listener) {
  model_listeners_.insert(listener);
}

void Controller::removeListener(Listener* listener) {
  model_listeners_.erase(listener);
}

int Controller::otherScannedNetworksCount() const {
  int count = all_networks_.size();
  if (current_network_index_ >= 0) --count;
  return count;
}

const Controller::Network& Controller::currentNetwork() const {
  return current_network_;
}

const Controller::Network* Controller::lookupNetwork(
    const std::string& ssid) const {
  for (const Network& net : all_networks_) {
    if (net.ssid == ssid) return &net;
  }
  return nullptr;
}

ConnectionStatus Controller::currentNetworkStatus() const {
  return current_network_status_;
}

const Controller::Network& Controller::otherNetwork(int idx) const {
  if (current_network_index_ >= 0 && idx >= current_network_index_) {
    idx++;
  }
  return all_networks_[idx];
}

bool Controller::startScan() {
  bool started = interface_.startScan();
  if (started) {
    for (auto& l : model_listeners_) {
      l->onScanStarted();
    };
  }
  return started;
}

void Controller::toggleEnabled() {
  enabled_ = !enabled_;
  store_.setIsInterfaceEnabled(enabled_);
  if (!enabled_) {
    interface_.disconnect();
  }
  connecting_ = false;
  notifyEnableChanged();
  if (enabled_) {
    resume();
  } else {
    pause();
  }
}

void Controller::notifyEnableChanged() {
  for (auto& l : model_listeners_) {
    l->onEnableChanged(enabled_);
  };
}

bool Controller::getStoredPassword(const std::string& ssid,
                                   std::string& passwd) const {
  return store_.getPassword(ssid, passwd);
}

void Controller::pause() { start_scan_.cancel(); }

void Controller::resume() {
  if (!enabled_) return;
  refreshCurrentNetwork();
  if (!refresh_current_network_.is_scheduled()) {
    refresh_current_network_.scheduleAfter(roo_time::Seconds(2));
  }
  if (interface_.scanCompleted()) {
    for (auto& l : model_listeners_) {
      l->onScanCompleted();
    };
    start_scan_.scheduleAfter(roo_time::Seconds(15));
  } else {
    startScan();
  }
}

void Controller::setPassword(const std::string& ssid,
                             const std::string& passwd) {
  store_.setPassword(ssid, passwd);
}

bool Controller::connect() {
  std::string ssid = store_.getDefaultSSID();
  std::string password;
  store_.getPassword(ssid, password);
  return connect(ssid, password);
}

bool Controller::connect(const std::string& ssid, const std::string& passwd) {
  std::string default_ssid = store_.getDefaultSSID();
  if (ssid != default_ssid) {
    store_.setDefaultSSID(ssid);
  }
  std::string current_password;
  if (!passwd.empty() && (!store_.getPassword(ssid, current_password) ||
                          current_password != passwd)) {
    store_.setPassword(ssid, passwd);
  }
  if (!interface_.connect(ssid, passwd)) return false;
  connecting_ = true;
  const Network* in_range = lookupNetwork(ssid);
  if (in_range == nullptr) {
    updateCurrentNetwork(ssid, passwd.empty(), -128, WL_DISCONNECTED, true);
  } else {
    updateCurrentNetwork(ssid, in_range->open, in_range->rssi, WL_DISCONNECTED,
                         true);
  }
  return true;
}

void Controller::disconnect() {
  connecting_ = false;
  interface_.disconnect();
}

void Controller::forget(const std::string& ssid) {
  store_.clearPassword(ssid);
  if (ssid == store_.getDefaultSSID()) {
    store_.clearDefaultSSID();
  }
}

void Controller::onConnectionStateChanged(Interface::EventType type) {
  if (type == Interface::EV_UNKNOWN) return;
  if (type == Interface::EV_DISCONNECTED ||
      type == Interface::EV_CONNECTION_FAILED ||
      type == Interface::EV_CONNECTION_LOST) {
    connecting_ = false;
  }
  updateCurrentNetwork(current_network_.ssid, current_network_.open,
                       current_network_.rssi, getConnectionStatus(type), true);
  for (auto& l : model_listeners_) {
    l->onConnectionStateChanged(type);
  }
}

void Controller::periodicRefreshCurrentNetwork() {
  refreshCurrentNetwork();
  if (isEnabled()) {
    refresh_current_network_.scheduleAfter(roo_time::Seconds(2));
  }
}

void Controller::refreshCurrentNetwork() {
  // If we're connected to the network, this is it.
  NetworkDetails current;
  if (interface_.getApInfo(&current)) {
    updateCurrentNetwork(std::string((const char*)current.ssid,
                                     strlen((const char*)current.ssid)),
                         (current.authmode == WIFI_AUTH_OPEN), current.rssi,
                         current.status, false);
  } else {
    // Check if we have a default network.
    std::string default_ssid = store_.getDefaultSSID();
    const Network* default_network_in_range = nullptr;
    if (!default_ssid.empty()) {
      // See if the default network is in range according to the latest
      // scan results.
      default_network_in_range = lookupNetwork(default_ssid);
    }
    // Keep erroneous states sticky. Only update if the network has actually
    // changed.
    if (default_network_in_range == nullptr) {
      ConnectionStatus new_status = (default_ssid == current_network_.ssid)
                                        ? current_network_status_
                                        : WL_NO_SSID_AVAIL;
      updateCurrentNetwork(default_ssid, true, -128, new_status, false);
    } else {
      ConnectionStatus new_status = (default_ssid == current_network_.ssid)
                                        ? current_network_status_
                                        : WL_DISCONNECTED;
      updateCurrentNetwork(default_ssid, default_network_in_range->open,
                           default_network_in_range->rssi, new_status, false);
    }
  }
}

void Controller::updateCurrentNetwork(const std::string& ssid, bool open,
                                      int8_t rssi, ConnectionStatus status,
                                      bool force_notify) {
  if (!force_notify && rssi == current_network_.rssi &&
      ssid == current_network_.ssid && open == current_network_.open &&
      status == current_network_status_) {
    return;
  }
  current_network_.ssid = ssid;
  current_network_.open = open;
  current_network_.rssi = rssi;
  current_network_status_ = status;
  current_network_index_ = -1;
  for (size_t i = 0; i < all_networks_.size(); ++i) {
    if (all_networks_[i].ssid == ssid) {
      current_network_index_ = i;
      break;
    }
  }
  for (auto& l : model_listeners_) {
    l->onCurrentNetworkChanged();
  };
}

void Controller::onScanCompleted() {
  current_network_index_ = -1;
  std::vector<NetworkDetails> raw_data;
  interface_.getScanResults(&raw_data, 100);
  int raw_count = raw_data.size();
  if (raw_count == 0) {
    all_networks_.clear();
    return;
  }
  // De-duplicate SSID, keeping the one with the strongest signal.
  // Start by sorting by (ssid, signal strength).
  std::vector<uint8_t> indices(raw_data.size(), 0);
  for (uint8_t i = 0; i < raw_count; ++i) indices[i] = i;
  std::sort(&indices[0], &indices[raw_count], [&](int a, int b) -> bool {
    int ssid_cmp = strncmp((const char*)raw_data[a].ssid,
                           (const char*)raw_data[b].ssid, 33);
    if (ssid_cmp < 0) return true;
    if (ssid_cmp > 0) return false;
    return raw_data[a].rssi > raw_data[b].rssi;
  });
  // Now, compact the result by keeping the first value for each SSID.
  const char* current_ssid = (const char*)raw_data[indices[0]].ssid;
  uint8_t src = 1;
  uint8_t dst = 1;
  while (src < raw_count) {
    const char* candidate_ssid = (const char*)raw_data[indices[src]].ssid;
    if (strncmp(current_ssid, candidate_ssid, 33) != 0) {
      current_ssid = candidate_ssid;
      indices[dst++] = indices[src];
    }
    ++src;
  }
  // Now sort again, this time by signal strength only.
  // Single-out and remove the default network.
  std::sort(&indices[0], &indices[dst], [&](int a, int b) -> bool {
    return raw_data[a].rssi > raw_data[b].rssi;
  });
  // Finally, copy over the results.
  all_networks_.resize(dst);
  bool found = false;
  for (uint8_t i = 0; i < dst; ++i) {
    NetworkDetails& src = raw_data[indices[i]];
    Network& dst = all_networks_[i];
    dst.ssid =
        std::string((const char*)src.ssid, strlen((const char*)src.ssid));
    dst.open = (src.authmode == WIFI_AUTH_OPEN);
    dst.rssi = src.rssi;
    if (dst.ssid == current_network_.ssid) {
      found = true;
      current_network_index_ = i;
      if (current_network_status_ == WL_NO_SSID_AVAIL) {
        current_network_status_ = WL_DISCONNECTED;
      }
    }
  }
  if (!found && current_network_status_ == WL_DISCONNECTED) {
    current_network_status_ = WL_NO_SSID_AVAIL;
  }
  for (auto& l : model_listeners_) {
    l->onScanCompleted();
  };
  if (enabled_) {
    start_scan_.scheduleAfter(roo_time::Seconds(15));
  }
}

}  // namespace roo_wifi