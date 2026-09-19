#pragma once
#include "roo_wifi/controller.h"
#include "roo_wifi/hal/esp32/arduino_preferences_store.h"
#include "roo_wifi/hal/esp32/esp32_arduino_interface.h"
namespace roo_wifi {
/// Owns dependencies before the controller and destroys the controller first.
class Esp32Wifi {
 public:
  /// Creates the station owner; begin through controller() on scheduler
  /// context.
  explicit Esp32Wifi(roo_scheduler::Scheduler &scheduler,
                     ControllerOptions options = {})
      : controller_(interface_, store_, scheduler, options) {}
  /// Portable facade for all operations and listener registration.
  Controller &controller() { return controller_; }

 private:
  ArduinoPreferencesStore store_;
  Esp32ArduinoInterface interface_;
  Controller controller_;
};
}  // namespace roo_wifi
