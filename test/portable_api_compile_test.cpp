#include <type_traits>
#include <utility>

#include "roo_wifi.h"
#if defined(ESP32) || defined(ARDUINO) || defined(ESP_PLATFORM)
#error "Compile this target on the host without native platform defines."
#endif
// Verifies station and scan requests expose Status without public operation
// IDs.
static_assert(
    std::is_same<decltype(std::declval<roo_wifi::Controller&>().disconnect()),
                 roo_wifi::Status>::value,
    "disconnect returns admission");
static_assert(
    std::is_same<decltype(std::declval<roo_wifi::Controller&>().startScan()),
                 roo_wifi::Status>::value,
    "startScan returns admission");
static_assert(
    std::is_same<decltype(std::declval<roo_wifi::Controller&>().cancelScan()),
                 roo_wifi::Status>::value,
    "cancelScan is scan-specific");
static_assert(std::is_same<decltype(std::declval<roo_wifi::Controller&>()
                                        .removeProfile(roo_wifi::Ssid{})),
                           roo_wifi::Status>::value,
              "writes return their outcome");

int main() {
  roo_scheduler::Scheduler scheduler;
  return scheduler.empty() ? 0 : 1;
}
