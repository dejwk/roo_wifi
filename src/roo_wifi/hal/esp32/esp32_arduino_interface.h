#pragma once

#include "WiFi.h"
#include "roo_collections.h"
#include "roo_scheduler.h"
#include "roo_wifi/hal/esp32/arduino_preferences_store.h"
#include "roo_wifi/hal/interface.h"
#include "roo_wifi/hal/store.h"

namespace roo_wifi {

namespace internal {

/// Linked list node storing a native ESP32 event callback.
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

/// ESP32 Arduino Wi-Fi interface implementation.
class Esp32ArduinoInterface : public Interface {
 public:
  Esp32ArduinoInterface();

  ~Esp32ArduinoInterface();

  /// Initializes the underlying Wi-Fi stack and registers callbacks.
  void begin();

  /// Returns current AP information; false if not connected.
  bool getApInfo(NetworkDetails* info) const override;

  /// Starts a scan.
  bool startScan() override;

  /// Returns true if the scan has completed.
  bool scanCompleted() const override;

  /// Returns scan results, up to max_count entries.
  bool getScanResults(std::vector<NetworkDetails>* list,
                      int max_count) const override;

  /// Disconnects from the current network.
  void disconnect() override;

  /// Connects to the specified SSID/password.
  bool connect(const std::string& ssid, const std::string& passwd) override;

  /// Returns the current connection status.
  ConnectionStatus getStatus() override;

  /// Registers an interface event listener.
  void addEventListener(EventListener* listener) override;

  /// Unregisters an interface event listener.
  void removeEventListener(EventListener* listener) override;

 private:
  void dispatchEvent(WiFiEvent_t event, WiFiEventInfo_t info);

  internal::Esp32ListenerListNode event_relay_;
  roo_collections::FlatSmallHashSet<EventListener*> listeners_;

  bool scanning_;
};

}  // namespace roo_wifi