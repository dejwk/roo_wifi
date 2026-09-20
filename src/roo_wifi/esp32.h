#pragma once
#include "roo_wifi/hal/esp32/idf_interface.h"
#include "roo_wifi/hal/prefs/prefs_store.h"
#include "roo_wifi/wifi_specialization.h"

namespace roo_wifi {

/// Ready-to-use ESP32 controller backed by roo_prefs and ESP-IDF.
using Esp32WiFi = WiFiSpecialization<PrefsStore, Esp32IdfInterface>;

}  // namespace roo_wifi
