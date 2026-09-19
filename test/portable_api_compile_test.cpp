#include "roo_wifi.h"
#if defined(ESP32) || defined(ARDUINO) || defined(ESP_PLATFORM)
#error "Compile this target on the host without native platform defines."
#endif
int main() {
  roo_scheduler::Scheduler scheduler;
  return scheduler.empty() ? 0 : 1;
}
