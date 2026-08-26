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
      scheduler_(scheduler),
      event_dispatch_state_(std::make_shared<EventDispatchState>(this)),
      enabled_(false),
      current_network_(),
      current_network_index_(-1),
      current_network_status_(WL_NO_SSID_AVAIL),
      all_networks_(),
      wifi_listener_(*this),
      model_listeners_(),
      connecting_(false),
      pending_connection_ssid_(),
      connection_generation_(0),
      listener_attached_(false),
      paused_(true),
      start_scan_(scheduler, [this]() { startScan(); }),
      refresh_current_network_(scheduler,
                               [this]() { periodicRefreshCurrentNetwork(); }) {}

Controller::~Controller() { shutdown(); }

void Controller::shutdown() {
  {
    roo::lock_guard<roo::mutex> lock(event_dispatch_state_->mutex);
    event_dispatch_state_->controller = nullptr;
  }
  start_scan_.cancel();
  refresh_current_network_.cancel();
  if (listener_attached_) {
    interface_.removeEventListener(&wifi_listener_);
    listener_attached_ = false;
  }
}

void Controller::enqueueInterfaceEvent(Interface::EventType type,
                                       roo::string_view event_ssid) {
  std::string ssid(event_ssid.data(), event_ssid.size());
  uint64_t connection_generation;
  {
    roo::lock_guard<roo::mutex> lock(connection_state_mutex_);
    if (ssid.empty()) ssid = pending_connection_ssid_;
    connection_generation = connection_generation_;
  }
  std::shared_ptr<EventDispatchState> state = event_dispatch_state_;
  scheduler_.scheduleNow([state, type, ssid, connection_generation]() {
    roo::lock_guard<roo::mutex> lock(state->mutex);
    if (state->controller != nullptr) {
      state->controller->onInterfaceEvent(type, ssid, connection_generation);
    }
  });
}

void Controller::onInterfaceEvent(Interface::EventType type,
                                  const std::string& ssid,
                                  uint64_t connection_generation) {
  if (paused_ || !enabled_) return;
  switch (type) {
    case Interface::EV_SCAN_COMPLETED:
      onScanCompleted();
      break;
    default:
      onConnectionStateChanged(type, ssid, connection_generation);
      break;
  }
}

void Controller::begin() {
  if (!listener_attached_) {
    interface_.addEventListener(&wifi_listener_);
    listener_attached_ = true;
  }
  enabled_ = store_.getIsInterfaceEnabled();
  interface_.setEnabled(enabled_);
  if (!enabled_) return;
  notifyEnableChanged();
  std::string ssid = store_.getDefaultSSID();
  if (!ssid.empty()) {
    connect();
  }
  resume();
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
  if (!enabled_ || paused_) return false;
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
    roo::lock_guard<roo::mutex> lock(connection_state_mutex_);
    pending_connection_ssid_.clear();
    ++connection_generation_;
  }
  interface_.setEnabled(enabled_);
  connecting_ = false;
  notifyEnableChanged();
  if (enabled_) {
    if (!store_.getDefaultSSID().empty()) {
      connect();
    }
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

void Controller::pause() {
  paused_ = true;
  start_scan_.cancel();
  refresh_current_network_.cancel();
}

void Controller::resume() {
  paused_ = false;
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
  if (ssid.empty()) return false;
  std::string password;
  store_.getPassword(ssid, password);
  return connect(ssid, password);
}

bool Controller::connect(const std::string& ssid, const std::string& passwd) {
  if (!enabled_ || ssid.empty()) return false;
  std::string default_ssid = store_.getDefaultSSID();
  std::string current_password;
  // The adapter can synchronously publish an event while connect() is in
  // progress. Set the target first so that event has an unambiguous owner.
  uint64_t connection_generation;
  {
    roo::lock_guard<roo::mutex> lock(connection_state_mutex_);
    pending_connection_ssid_ = ssid;
    connection_generation = ++connection_generation_;
  }
  if (!interface_.connect(ssid, passwd)) {
    roo::lock_guard<roo::mutex> lock(connection_state_mutex_);
    if (connection_generation_ == connection_generation) {
      pending_connection_ssid_.clear();
    }
    return false;
  }
  if (ssid != default_ssid) {
    store_.setDefaultSSID(ssid);
  }
  if (!store_.getPassword(ssid, current_password) || current_password != passwd) {
    store_.setPassword(ssid, passwd);
  }
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
  {
    roo::lock_guard<roo::mutex> lock(connection_state_mutex_);
    pending_connection_ssid_.clear();
    ++connection_generation_;
  }
  interface_.disconnect();
}

void Controller::forget(const std::string& ssid) {
  store_.clearPassword(ssid);
  if (ssid == store_.getDefaultSSID()) {
    store_.clearDefaultSSID();
    interface_.clearPersistentCredentials();
  }
}

void Controller::onConnectionStateChanged(Interface::EventType type,
                                          const std::string& event_ssid,
                                          uint64_t connection_generation) {
  if (type == Interface::EV_UNKNOWN) return;
  {
    roo::lock_guard<roo::mutex> lock(connection_state_mutex_);
    if (connection_generation != connection_generation_ ||
        (!event_ssid.empty() && !pending_connection_ssid_.empty() &&
         event_ssid != pending_connection_ssid_)) {
      if (type == Interface::EV_CONNECTION_FAILED && !event_ssid.empty()) {
        store_.clearPassword(event_ssid);
      }
      return;
    }
  }
  if (type == Interface::EV_DISCONNECTED ||
      type == Interface::EV_CONNECTION_FAILED ||
      type == Interface::EV_CONNECTION_LOST) {
    connecting_ = false;
  }
  if (type == Interface::EV_CONNECTED || type == Interface::EV_GOT_IP) {
    connecting_ = false;
  }
  const std::string& ssid =
      event_ssid.empty() ? current_network_.ssid : event_ssid;
  if (type == Interface::EV_CONNECTION_FAILED && !ssid.empty()) {
    // An authentication failure proves that the credential for this network
    // is unusable. Do not silently retry it the next time the network is
    // selected; let the UI ask for a replacement password instead.
    store_.clearPassword(ssid);
  }
  const Network* network = lookupNetwork(ssid);
  updateCurrentNetwork(ssid, network == nullptr ? current_network_.open
                                                 : network->open,
                       network == nullptr ? current_network_.rssi
                                          : network->rssi,
                       getConnectionStatus(type), true);
  if (type == Interface::EV_GOT_IP || type == Interface::EV_DISCONNECTED ||
      type == Interface::EV_CONNECTION_FAILED ||
      type == Interface::EV_CONNECTION_LOST) {
    roo::lock_guard<roo::mutex> lock(connection_state_mutex_);
    if (connection_generation_ == connection_generation) {
      pending_connection_ssid_.clear();
    }
  }
  for (auto& l : model_listeners_) {
    l->onConnectionStateChanged(type);
  }
}

void Controller::periodicRefreshCurrentNetwork() {
  refreshCurrentNetwork();
  if (isEnabled() && !paused_) {
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
      current_network_index_ = static_cast<int16_t>(i);
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
  if (!interface_.getScanResults(&raw_data, 100)) {
    if (enabled_ && !paused_) {
      start_scan_.scheduleAfter(roo_time::Seconds(15));
    }
    return;
  }
  auto notify_scan_completed = [this]() {
    for (auto& listener : model_listeners_) {
      listener->onScanCompleted();
    }
    if (enabled_ && !paused_) {
      start_scan_.scheduleAfter(roo_time::Seconds(15));
    }
  };
  size_t raw_count = raw_data.size();
  if (raw_count == 0) {
    all_networks_.clear();
    if (current_network_status_ == WL_DISCONNECTED) {
      current_network_status_ = WL_NO_SSID_AVAIL;
    }
    notify_scan_completed();
    return;
  }
  // De-duplicate SSID, keeping the one with the strongest signal.
  // Start by sorting by (ssid, signal strength).
  std::vector<size_t> indices(raw_count, 0);
  for (size_t i = 0; i < raw_count; ++i) indices[i] = i;
  std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) -> bool {
    int ssid_cmp = strncmp((const char*)raw_data[a].ssid,
                           (const char*)raw_data[b].ssid, 33);
    if (ssid_cmp < 0) return true;
    if (ssid_cmp > 0) return false;
    return raw_data[a].rssi > raw_data[b].rssi;
  });
  // Now, compact the result by keeping the first value for each SSID.
  const char* current_ssid = (const char*)raw_data[indices[0]].ssid;
  size_t src = 1;
  size_t dst = 1;
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
  std::sort(indices.begin(), indices.begin() + dst,
            [&](size_t a, size_t b) -> bool {
              return raw_data[a].rssi > raw_data[b].rssi;
            });
  // Finally, copy over the results.
  all_networks_.resize(dst);
  bool found = false;
  for (size_t i = 0; i < dst; ++i) {
    NetworkDetails& src = raw_data[indices[i]];
    Network& dst = all_networks_[i];
    dst.ssid =
        std::string((const char*)src.ssid, strlen((const char*)src.ssid));
    dst.open = (src.authmode == WIFI_AUTH_OPEN);
    dst.rssi = src.rssi;
    if (dst.ssid == current_network_.ssid) {
      found = true;
      current_network_index_ = static_cast<int16_t>(i);
      if (current_network_status_ == WL_NO_SSID_AVAIL) {
        current_network_status_ = WL_DISCONNECTED;
      }
    }
  }
  if (!found && current_network_status_ == WL_DISCONNECTED) {
    current_network_status_ = WL_NO_SSID_AVAIL;
  }
  notify_scan_completed();
}

}  // namespace roo_wifi
