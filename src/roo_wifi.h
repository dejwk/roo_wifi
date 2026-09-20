/// Portable Wi-Fi controller and hardware/storage contracts.
#pragma once

#include "roo_wifi/controller.h"

#if defined(ESP_PLATFORM) || (defined(ARDUINO) && defined(ARDUINO_ARCH_ESP32))
#include "roo_wifi/esp32.h"

namespace roo_wifi {

/// Default hardware-backed controller selected for ESP32 builds.
using WiFi = Esp32WiFi;

}  // namespace roo_wifi
#endif
