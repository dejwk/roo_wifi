/// Explicit ESP32 construction; dependencies must outlive controller use.
#pragma once

#include "roo_wifi/controller.h"

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
  Esp32Wifi(roo_scheduler::Scheduler &scheduler)
      : Controller(store_, interface_, scheduler), store_(), interface_() {}

  /// @brief Shuts down the controller before destroying its owned adapters.
  ~Esp32Wifi() override { shutdown(); }

  /// @brief Copy construction is disabled.
  Esp32Wifi(const Esp32Wifi &) = delete;
  /// @brief Copy assignment is disabled.
  Esp32Wifi &operator=(const Esp32Wifi &) = delete;
  /// @brief Move construction is disabled.
  Esp32Wifi(Esp32Wifi &&) = delete;
  /// @brief Move assignment is disabled.
  Esp32Wifi &operator=(Esp32Wifi &&) = delete;

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

} // namespace roo_wifi
