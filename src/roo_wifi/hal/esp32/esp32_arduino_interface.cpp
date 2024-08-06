#include "esp32_arduino_interface.h"

#include "WiFiGeneric.h"
#include "WiFi.h"

namespace roo_wifi {

namespace {

static AuthMode authMode(wifi_auth_mode_t mode) {
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
    default:
      return WIFI_AUTH_UNKNOWN;
  }
}

internal::Esp32ListenerListNode* head = nullptr;

void attach(internal::Esp32ListenerListNode* n) {
  if (head == nullptr) {
    n->next = n->prev = n;
  } else {
    n->next = head->next;
    n->prev = head;
    head->next->prev = n;
    head->next = n;
  }
  head = n;
}

void detach(internal::Esp32ListenerListNode* n) {
  internal::Esp32ListenerListNode* new_head = nullptr;
  if (n->next != n->prev) {
    n->next->prev = n->prev;
    n->prev->next = n->next;
    new_head = n->next;
  }
  n->next = n->prev = nullptr;
  head = new_head;
}

void dispatch(arduino_event_id_t event, arduino_event_info_t info) {
  if (head == nullptr) return;
  auto n = head;
  do {
    n->notify_fn(event, info);
    n = n->next;
  } while (n != head);
}

void init() {
  static struct Init {
    Init() { WiFi.onEvent(&dispatch); }
  } init;
}

}  // namespace

Esp32ArduinoInterface::Esp32ArduinoInterface()
    : event_relay_([&](arduino_event_id_t event, arduino_event_info_t info) {
        dispatchEvent(event, info);
      }),
      scanning_(false) {}

Esp32ArduinoInterface::~Esp32ArduinoInterface() {
  detach(&event_relay_);
}

void Esp32ArduinoInterface::begin() {
  init();
  attach(&event_relay_);
  WiFi.mode(WIFI_STA);
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
  memcpy(info->ssid, ssid.c_str(), ssid.length());
  info->ssid[ssid.length()] = 0;
  info->authmode = WIFI_AUTH_UNKNOWN;  // authMode(WiFi.encryptionType());
  info->rssi = WiFi.RSSI();
  auto mac = WiFi.macAddress();
  memcpy(info->bssid, mac.c_str(), 6);
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
  scanning_ = (WiFi.scanNetworks(true, false) == WIFI_SCAN_RUNNING);
  return scanning_;
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
    NetworkDetails info;
    auto ssid = WiFi.SSID(i);
    memcpy(info.ssid, ssid.c_str(), ssid.length());
    info.ssid[ssid.length()] = 0;
    info.authmode = authMode(WiFi.encryptionType(i));
    info.rssi = WiFi.RSSI(i);
    info.primary = WiFi.channel(i);
    info.group_cipher = WIFI_CIPHER_TYPE_UNKNOWN;
    info.pairwise_cipher = WIFI_CIPHER_TYPE_UNKNOWN;
    info.use_11b = false;
    info.use_11g = false;
    info.use_11n = false;
    info.supports_wps = false;
    list->push_back(std::move(info));
  }
  return true;
}

void Esp32ArduinoInterface::disconnect() { WiFi.disconnect(); }

bool Esp32ArduinoInterface::connect(const std::string& ssid,
                                    const std::string& passwd) {
  WiFi.begin(ssid.c_str(), passwd.c_str());
  return true;
}

ConnectionStatus Esp32ArduinoInterface::getStatus() {
  return (ConnectionStatus)WiFi.status();
}

void Esp32ArduinoInterface::addEventListener(EventListener* listener) {
  listeners_.insert(listener);
}

void Esp32ArduinoInterface::removeEventListener(EventListener* listener) {
  listeners_.erase(listener);
}

namespace {

Interface::EventType getEventType(arduino_event_id_t event, arduino_event_info_t info) {
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

}  // namespace

void Esp32ArduinoInterface::dispatchEvent(arduino_event_id_t event, arduino_event_info_t info) {
  EventType type = getEventType(event, info);
  if (type == Interface::EV_SCAN_COMPLETED) {
    scanning_ = false;
  }
  for (const auto& l : listeners_) {
    l->onEvent(type);
  }
}

}  // namespace roo_wifi
