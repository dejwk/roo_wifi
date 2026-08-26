/// @file
/// @brief Umbrella header for the Roo Wi-Fi module.
///
/// @defgroup roo_wifi Roo Wi-Fi
/// @brief Wi-Fi scanning, connection management, credential storage, and
/// ESP32 Arduino adapters.
///
/// Include this header to use the high-level controller and, when compiling
/// for ESP32, the ready-to-use `roo_wifi::Wifi` controller.
///
/// @note On ESP32 this header must be included after `<Arduino.h>`.

#pragma once

#include "roo_scheduler.h"
#include "roo_wifi/controller.h"
#include "roo_wifi/hal/interface.h"

#ifdef ESP32

#include "roo_wifi/hal/esp32/arduino_preferences_store.h"
#include "roo_wifi/hal/esp32/esp32_arduino_interface.h"

namespace roo_wifi {

/// @brief Ready-to-use ESP32 Wi-Fi controller.
/// @ingroup roo_wifi
///
/// Owns the ESP32 interface and preferences-backed store required by
/// `Controller`.
class Esp32Wifi : public Controller {
 public:
  /// @brief Constructs an ESP32 controller using `scheduler`.
  /// @param scheduler Scheduler used for scans, refreshes, and event dispatch.
  Esp32Wifi(roo_scheduler::Scheduler& scheduler)
      : Controller(store_, interface_, scheduler), store_(), interface_() {}

  /// @brief Shuts down the controller before destroying its owned adapters.
  ~Esp32Wifi() override { shutdown(); }

  /// @brief Copy construction is disabled.
  Esp32Wifi(const Esp32Wifi&) = delete;
  /// @brief Copy assignment is disabled.
  Esp32Wifi& operator=(const Esp32Wifi&) = delete;
  /// @brief Move construction is disabled.
  Esp32Wifi(Esp32Wifi&&) = delete;
  /// @brief Move assignment is disabled.
  Esp32Wifi& operator=(Esp32Wifi&&) = delete;

  /// @brief Initializes storage, the ESP32 interface, and the controller.
  ///
  /// Call once during application startup before using the controller.
  void begin() {
    store_.begin();
    interface_.begin();
    Controller::begin();
  }

 private:
  ArduinoPreferencesStore store_;
  Esp32ArduinoInterface interface_;
};

/// @brief Default platform Wi-Fi controller type for ESP32 builds.
using Wifi = Esp32Wifi;

}  // namespace roo_wifi

#endif
