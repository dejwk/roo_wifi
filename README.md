# roo_wifi
Wi-Fi controller library for ESP32, supporting persistent configuration in flash
and a portable controller API. Its radio adapter uses ESP-IDF directly.

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

The same radio adapter can be used from a raw ESP-IDF application. It creates
the default event loop, station netif, and Wi-Fi driver only when they have not
already been initialized; applications must still leave the station exclusively
owned by `roo_wifi`.

## Host emulation

Host builds use the roo_testing 2.0 Arduino ESP32 profile. With Bazelisk 1.21
or newer, a plain command defaults to that profile and prints a notice:

    bazel test ...
    bazel test ... --config=asan
    bazel test ... --config=roo_testing_arduino_esp32

The files under .roo_testing/bazelrc/esp32 are vendored from roo_testing;
follow their canonical-source headers when refreshing them.

## Examples

Runnable Arduino and native ESP-IDF scan examples are in
[examples](examples/README.md).

## Design

The proposed [Wi-Fi backend foundation](docs/wifi_backend_design.md) defines the
platform-independent API, HAL evolution, profile persistence, and backend
validation plan. Its [design authoring rules](.github/instructions/embedded-design-doc-authoring.instructions.md)
apply repository-wide.
