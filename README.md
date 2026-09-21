# roo_wifi
Wi-Fi controller library for ESP32, supporting persistent configuration in flash
and a portable controller API. Its radio adapter uses ESP-IDF directly.

## ESP32 use

The ESP32 adapter uses ESP-IDF directly and works in both Arduino-ESP32
sketches and native ESP-IDF applications. Construct the controller once, call
`begin()` from the application startup path, and run the scheduler regularly.

In an Arduino sketch:

```cpp
#include <Arduino.h>
#include <roo_scheduler.h>
#include <roo_wifi.h>

roo_scheduler::Scheduler scheduler;
roo_wifi::WiFi wifi(scheduler);

void setup() {
  wifi.begin();
}

void loop() {
  scheduler.executeEligibleTasks();
}
```

Register a `roo_wifi::Listener` before `begin()` to observe deferred
enablement, scan, connection, and profile-operation results. The runnable scan
examples show how to enable the station and request a scan once the preceding
enable operation has completed.

Saved profiles can be discovered without a separate application catalog:

```cpp
wifi.forEachProfile([&](roo_wifi::ProfileId id) {
  roo_wifi::Profile profile;
  if (wifi.loadProfile(id, profile) == roo_wifi::Status::kOk) {
    // Use the non-secret profile metadata.
  }
  return true;  // Return false to stop early.
});
```

Enumeration order is unspecified. Each profile uses compact, versioned settings
and secret values; an ID is still visited if either is corrupt so
`loadProfile()` can report the error. Enumeration works independently of radio
enablement after `begin()`.

In a raw ESP-IDF application, initialize the default NVS partition before
constructing `WiFi`; the adapter creates the default event loop, station
netif, and Wi-Fi driver only when they are not already initialized. In either
environment, leave the physical station exclusively owned by `roo_wifi`.

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
