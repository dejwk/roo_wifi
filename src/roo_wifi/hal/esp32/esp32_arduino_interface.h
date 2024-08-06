#pragma once

#include "WiFi.h"
#include "roo_collections.h"
#include "roo_scheduler.h"
#include "roo_wifi/hal/esp32/arduino_preferences_store.h"
#include "roo_wifi/hal/interface.h"
#include "roo_wifi/hal/store.h"

namespace roo_wifi {

namespace internal {

struct Esp32ListenerListNode {
  std::function<void(arduino_event_id_t event, arduino_event_info_t info)>
      notify_fn;
  Esp32ListenerListNode* next;
  Esp32ListenerListNode* prev;

  Esp32ListenerListNode(
      std::function<void(arduino_event_id_t event, arduino_event_info_t info)>
          notify_fn)
      : notify_fn(notify_fn), next(nullptr), prev(nullptr) {}
};

}  // namespace internal

class Esp32ArduinoInterface : public Interface {
 public:
  Esp32ArduinoInterface();

  ~Esp32ArduinoInterface();

  void begin();

  bool getApInfo(NetworkDetails* info) const override;

  bool startScan() override;

  bool scanCompleted() const override;

  bool getScanResults(std::vector<NetworkDetails>* list,
                      int max_count) const override;

  void disconnect() override;

  bool connect(const std::string& ssid, const std::string& passwd) override;

  ConnectionStatus getStatus() override;

  void addEventListener(EventListener* listener) override;

  void removeEventListener(EventListener* listener) override;

 private:
  void dispatchEvent(WiFiEvent_t event, WiFiEventInfo_t info);

  internal::Esp32ListenerListNode event_relay_;
  roo_collections::FlatSmallHashSet<EventListener*> listeners_;

  bool scanning_;
};

}  // namespace roo_wifi