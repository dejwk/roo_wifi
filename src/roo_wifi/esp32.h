#pragma once
#include "roo_wifi/controller.h"
#include "roo_wifi/hal/esp32/arduino_preferences_store.h"
#include "roo_wifi/hal/esp32/esp32_arduino_interface.h"

namespace roo_wifi {
/// Creates a ready-to-use ESP32-backed Wi-Fi controller.
class Esp32Wifi {
 public:
  /// Creates the ESP32 radio and preferences-backed controller.
  /// @param scheduler Context on which controller calls and callbacks run.
  /// @param options Capacity, timeout, and startup behavior.
  explicit Esp32Wifi(roo_scheduler::Scheduler &scheduler,
                     Controller::Options options = {})
      : controller_(interface_, store_, scheduler, options) {}

  /// Returns the portable controller facade.
  Controller &controller() { return controller_; }

 private:
  ArduinoPreferencesStore store_;
  Esp32IdfInterface interface_;
  Controller controller_;
};
}  // namespace roo_wifi
