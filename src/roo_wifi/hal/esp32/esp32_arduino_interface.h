/// @file
/// @brief ESP32 Arduino implementation of the Wi-Fi hardware interface.

#pragma once

#include "WiFi.h"
#include "roo_collections.h"
#include "roo_scheduler.h"
#include "roo_threads.h"
#include "roo_wifi/hal/esp32/arduino_preferences_store.h"
#include "roo_wifi/hal/interface.h"
#include "roo_wifi/hal/store.h"

namespace roo_wifi {

/// @brief Adapts the ESP32 Arduino `WiFi` API to `Interface`.
/// @ingroup roo_wifi
///
/// Native Arduino events are dispatched synchronously to registered
/// `EventListener` instances. Creating more than one adapter is supported;
/// each attached instance receives the process-wide Arduino Wi-Fi events.
class Esp32ArduinoInterface : public Interface {
 public:
  /// @brief Constructs a detached ESP32 interface adapter.
  Esp32ArduinoInterface();

  /// @brief Detaches the adapter from process-wide Arduino event dispatch.
  ~Esp32ArduinoInterface();

  /// @brief Copy construction is disabled.
  Esp32ArduinoInterface(const Esp32ArduinoInterface&) = delete;
  /// @brief Copy assignment is disabled.
  Esp32ArduinoInterface& operator=(const Esp32ArduinoInterface&) = delete;
  /// @brief Move construction is disabled.
  Esp32ArduinoInterface(Esp32ArduinoInterface&&) = delete;
  /// @brief Move assignment is disabled.
  Esp32ArduinoInterface& operator=(Esp32ArduinoInterface&&) = delete;

  /// @brief Initializes Arduino Wi-Fi and attaches global event dispatch.
  void begin();

  /// @copydoc Interface::getApInfo()
  bool getApInfo(NetworkDetails* info) const override;

  /// @copydoc Interface::startScan()
  bool startScan() override;

  /// @copydoc Interface::scanCompleted()
  bool scanCompleted() const override;

  /// @copydoc Interface::getScanResults()
  bool getScanResults(std::vector<NetworkDetails>* list,
                      int max_count) const override;

  /// @copydoc Interface::disconnect()
  void disconnect() override;

  /// @copydoc Interface::connect()
  bool connect(const std::string& ssid, const std::string& passwd) override;

  /// @copydoc Interface::getStatus()
  ConnectionStatus getStatus() override;

  /// @copydoc Interface::addEventListener()
  void addEventListener(EventListener* listener) override;

  /// @copydoc Interface::removeEventListener()
  void removeEventListener(EventListener* listener) override;

  /// @copydoc Interface::setEnabled()
  void setEnabled(bool enabled) override;
  /// @copydoc Interface::clearPersistentCredentials()
  void clearPersistentCredentials() override;

  /// @brief Dispatches a native Arduino event to registered listeners.
  ///
  /// This is public solely for the process-wide Arduino callback.
  ///
  /// @param event Native Arduino event identifier.
  /// @param info Native event payload, valid for the duration of dispatch.
  void dispatchEvent(WiFiEvent_t event, const WiFiEventInfo_t& info);

 private:
  roo_collections::FlatSmallHashSet<EventListener*> listeners_;
  roo::mutex listeners_mutex_;

  bool attached_;
};

}  // namespace roo_wifi
