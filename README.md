# roo_wifi
WiFi controller library for ESP32, supporting storing persistent configuration in flash, and abstracting away the architecture.

## Arduino use

The library targets ESP32 Arduino projects. Construct the controller once, call
`begin()` from `setup()`, and run the scheduler from `loop()`:

```cpp
#include <Arduino.h>
#include <roo_scheduler.h>
#include <roo_wifi.h>

roo_scheduler::Scheduler scheduler;
roo_wifi::Wifi wifi(scheduler);

void setup() {
  wifi.begin();
  if (!wifi.isEnabled()) wifi.toggleEnabled();
}

void loop() {
  scheduler.executeEligibleTasks();
}
```

Use `connect(ssid, password)` to select a network and `forget(ssid)` to remove
the selected network and its saved credentials.

## Host emulation

Host builds use the roo_testing 2.0 Arduino ESP32 profile. With Bazelisk 1.21
or newer, a plain command defaults to that profile and prints a notice:

    bazel test ...
    bazel test ... --config=asan
    bazel test ... --config=roo_testing_arduino_esp32

The files under .roo_testing/bazelrc/esp32 are vendored from roo_testing;
follow their canonical-source headers when refreshing them.
