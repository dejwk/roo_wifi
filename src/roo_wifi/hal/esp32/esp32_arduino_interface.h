#pragma once

#include "WiFi.h"
#include "roo_collections.h"
#include "roo_scheduler.h"
#include "roo_threads.h"
#include "roo_wifi/hal/esp32/arduino_preferences_store.h"
#include "roo_wifi/hal/interface.h"
#include "roo_wifi/hal/store.h"

namespace roo_wifi {

/// ESP32 Arduino Wi-Fi interface implementation.
class Esp32ArduinoInterface : public Interface {
 public:
  Esp32ArduinoInterface();

  ~Esp32ArduinoInterface();

  Esp32ArduinoInterface(const Esp32ArduinoInterface&) = delete;
  Esp32ArduinoInterface& operator=(const Esp32ArduinoInterface&) = delete;
  Esp32ArduinoInterface(Esp32ArduinoInterface&&) = delete;
  Esp32ArduinoInterface& operator=(Esp32ArduinoInterface&&) = delete;

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

  void setEnabled(bool enabled) override;
  void clearPersistentCredentials() override;

  /// Dispatches a native Arduino Wi-Fi event to registered listeners.
  ///
  /// This is public solely for the process-wide Arduino callback.
  void dispatchEvent(WiFiEvent_t event, WiFiEventInfo_t info);

 private:
  roo_collections::FlatSmallHashSet<EventListener*> listeners_;
  roo::mutex listeners_mutex_;

  bool attached_;
};

}  // namespace roo_wifi
