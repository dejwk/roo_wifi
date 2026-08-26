#include "esp32_arduino_interface.h"

#include <algorithm>

#include "WiFi.h"
#include "WiFiGeneric.h"

namespace roo_wifi {

namespace {

AuthMode ToAuthMode(wifi_auth_mode_t mode) {
  switch (mode) {
    case ::WIFI_AUTH_OPEN:
      return WIFI_AUTH_OPEN;
    case ::WIFI_AUTH_WEP:
      return WIFI_AUTH_WEP;
    case ::WIFI_AUTH_WPA_PSK:
      return WIFI_AUTH_WPA_PSK;
    case ::WIFI_AUTH_WPA2_PSK:
      return WIFI_AUTH_WPA2_PSK;
    case ::WIFI_AUTH_WPA_WPA2_PSK:
      return WIFI_AUTH_WPA_WPA2_PSK;
    case ::WIFI_AUTH_WPA2_ENTERPRISE:
      return WIFI_AUTH_WPA2_ENTERPRISE;
    case ::WIFI_AUTH_WPA3_PSK:
      return WIFI_AUTH_WPA3_PSK;
    case ::WIFI_AUTH_WPA2_WPA3_PSK:
      return WIFI_AUTH_WPA2_WPA3_PSK;
    case ::WIFI_AUTH_WAPI_PSK:
      return WIFI_AUTH_WAPI_PSK;
    default:
      return WIFI_AUTH_UNKNOWN;
  }
}

roo::mutex interfaces_mutex;
roo_collections::FlatSmallHashSet<Esp32ArduinoInterface*> interfaces;

void Dispatch(arduino_event_id_t event, arduino_event_info_t info) {
  roo::lock_guard<roo::mutex> lock(interfaces_mutex);
  for (Esp32ArduinoInterface* interface : interfaces) {
    interface->dispatchEvent(event, info);
  }
}

void Init() {
  static struct Init {
    Init() { WiFi.onEvent(&Dispatch); }
  } init;
}

}  // namespace

Esp32ArduinoInterface::Esp32ArduinoInterface()
    : listeners_(), listeners_mutex_(), attached_(false) {}

Esp32ArduinoInterface::~Esp32ArduinoInterface() {
  roo::lock_guard<roo::mutex> lock(interfaces_mutex);
  if (attached_) {
    interfaces.erase(this);
    attached_ = false;
  }
}

void Esp32ArduinoInterface::begin() {
  Init();
  WiFi.persistent(false);
  {
    roo::lock_guard<roo::mutex> lock(interfaces_mutex);
    if (!attached_) {
      interfaces.insert(this);
      attached_ = true;
    }
  }
  // // #ifdef ESP32
  // WiFi.onEvent(
  //     [this](arduino_event_id_t event) {
  //       for (const auto& l : listeners_) {
  //         l->scanCompleted();
  //       }
  //     },
  //     arduino_EVENT_SCAN_DONE);
  // // #endif
}

bool Esp32ArduinoInterface::getApInfo(NetworkDetails* info) const {
  const String& ssid = WiFi.SSID();
  if (ssid.length() == 0) return false;
  *info = NetworkDetails{};
  const size_t ssid_length =
      std::min<size_t>(ssid.length(), sizeof(info->ssid) - 1);
  memcpy(info->ssid, ssid.c_str(), ssid_length);
  info->ssid[ssid_length] = 0;
  info->authmode = WIFI_AUTH_UNKNOWN;
  info->rssi = WiFi.RSSI();
  const uint8_t* bssid = WiFi.BSSID();
  if (bssid != nullptr) {
    memcpy(info->bssid, bssid, sizeof(info->bssid));
    const int16_t scan_count = WiFi.scanComplete();
    for (int i = 0; i < scan_count; ++i) {
      const uint8_t* scan_bssid = WiFi.BSSID(i);
      if (scan_bssid != nullptr && ssid == WiFi.SSID(i) &&
          memcmp(bssid, scan_bssid, sizeof(info->bssid)) == 0) {
        info->authmode = ToAuthMode(WiFi.encryptionType(i));
        break;
      }
    }
  }
  info->primary = WiFi.channel();
  info->group_cipher = WIFI_CIPHER_TYPE_UNKNOWN;
  info->pairwise_cipher = WIFI_CIPHER_TYPE_UNKNOWN;
  info->use_11b = false;
  info->use_11g = false;
  info->use_11n = false;
  info->supports_wps = false;

  info->status = (ConnectionStatus)WiFi.status();
  return true;
}

bool Esp32ArduinoInterface::startScan() {
  return WiFi.scanNetworks(true, false) == WIFI_SCAN_RUNNING;
}

bool Esp32ArduinoInterface::scanCompleted() const {
  bool completed = WiFi.scanComplete() >= 0;
  return completed;
}

bool Esp32ArduinoInterface::getScanResults(std::vector<NetworkDetails>* list,
                                           int max_count) const {
  int16_t result = WiFi.scanComplete();
  if (result < 0) return false;
  if (max_count > result) {
    max_count = result;
  }
  list->clear();
  for (int i = 0; i < max_count; ++i) {
    NetworkDetails info = {};
    auto ssid = WiFi.SSID(i);
    const size_t ssid_length =
        std::min<size_t>(ssid.length(), sizeof(info.ssid) - 1);
    memcpy(info.ssid, ssid.c_str(), ssid_length);
    info.ssid[ssid_length] = 0;
    const uint8_t* bssid = WiFi.BSSID(i);
    if (bssid != nullptr) {
      memcpy(info.bssid, bssid, sizeof(info.bssid));
    }
    info.authmode = ToAuthMode(WiFi.encryptionType(i));
    info.rssi = WiFi.RSSI(i);
    info.primary = WiFi.channel(i);
    info.group_cipher = WIFI_CIPHER_TYPE_UNKNOWN;
    info.pairwise_cipher = WIFI_CIPHER_TYPE_UNKNOWN;
    info.use_11b = false;
    info.use_11g = false;
    info.use_11n = false;
    info.supports_wps = false;
    info.status = WL_SCAN_COMPLETED;
    list->push_back(std::move(info));
  }
  return true;
}

void Esp32ArduinoInterface::disconnect() { WiFi.disconnect(); }

void Esp32ArduinoInterface::setEnabled(bool enabled) {
  if (enabled) {
    WiFi.mode(WIFI_STA);
  } else {
    WiFi.disconnect(true, false);
  }
}

void Esp32ArduinoInterface::clearPersistentCredentials() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
}

bool Esp32ArduinoInterface::connect(const std::string& ssid,
                                    const std::string& passwd) {
  if (ssid.empty() || ssid.size() > 32 ||
      ssid.find('\0') != std::string::npos || passwd.size() > 64 ||
      passwd.find('\0') != std::string::npos) {
    return false;
  }
  return WiFi.begin(ssid.c_str(), passwd.c_str()) != ::WL_CONNECT_FAILED;
}

ConnectionStatus Esp32ArduinoInterface::getStatus() {
  return (ConnectionStatus)WiFi.status();
}

void Esp32ArduinoInterface::addEventListener(EventListener* listener) {
  roo::lock_guard<roo::mutex> lock(listeners_mutex_);
  listeners_.insert(listener);
}

void Esp32ArduinoInterface::removeEventListener(EventListener* listener) {
  roo::lock_guard<roo::mutex> lock(listeners_mutex_);
  listeners_.erase(listener);
}

namespace {

Interface::EventType GetEventType(arduino_event_id_t event,
                                  const arduino_event_info_t& info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_SCAN_DONE:
      return Interface::EV_SCAN_COMPLETED;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      return Interface::EV_CONNECTED;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      return Interface::EV_GOT_IP;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      switch (info.wifi_sta_disconnected.reason) {
        case WIFI_REASON_AUTH_FAIL:
          return Interface::EV_CONNECTION_FAILED;
        case WIFI_REASON_BEACON_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
          return Interface::EV_CONNECTION_LOST;
        default:
          return Interface::EV_DISCONNECTED;
      }
    }
    default:
      return Interface::EV_UNKNOWN;
  }
}

roo::string_view GetEventSsid(arduino_event_id_t event,
                              const arduino_event_info_t& info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      return roo::string_view(
          reinterpret_cast<const char*>(info.wifi_sta_connected.ssid),
          info.wifi_sta_connected.ssid_len);
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      return roo::string_view(
          reinterpret_cast<const char*>(info.wifi_sta_disconnected.ssid),
          info.wifi_sta_disconnected.ssid_len);
    default:
      return roo::string_view();
  }
}

}  // namespace

void Esp32ArduinoInterface::dispatchEvent(arduino_event_id_t event,
                                          const arduino_event_info_t& info) {
  EventType type = GetEventType(event, info);
  roo::string_view ssid = GetEventSsid(event, info);
  roo::lock_guard<roo::mutex> lock(listeners_mutex_);
  for (const auto& l : listeners_) {
    l->onEvent(type, ssid);
  }
}

}  // namespace roo_wifi
