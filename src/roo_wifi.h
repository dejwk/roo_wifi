#pragma once

/// Umbrella header for the roo_wifi module.
///
/// Provides Wi-Fi controller interfaces and platform adapters.

#include "roo_scheduler.h"
#include "roo_wifi/controller.h"
#include "roo_wifi/hal/interface.h"

/// Must be included after `<Arduino.h>`.

#ifdef ESP32

#include "roo_wifi/hal/esp32/arduino_preferences_store.h"
#include "roo_wifi/hal/esp32/esp32_arduino_interface.h"

namespace roo_wifi {

/// ESP32 Wi-Fi controller convenience wrapper.
class Esp32Wifi : public Controller {
 public:
  Esp32Wifi(roo_scheduler::Scheduler& scheduler)
      : Controller(store_, interface_, scheduler), store_(), interface_() {}

  void begin() {
    store_.begin();
    interface_.begin();
    Controller::begin();
  }

 private:
  ArduinoPreferencesStore store_;
  Esp32ArduinoInterface interface_;
};

using Wifi = Esp32Wifi;

}  // namespace roo_wifi

#endif
