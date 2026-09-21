# roo_wifi

`roo_wifi` combines Arduino-style ease of use and cross-platform portability
with advanced Wi-Fi configuration. It provides a portable controller API for
network discovery, connection management, and saved profiles, while its ESP32
radio adapter uses ESP-IDF directly.

Built-in integration with `roo_prefs` persists network settings and passwords
in NVS flash by default, while allowing applications to supply another storage
backend. After a saved connection succeeds, `roo_wifi` remembers that network
and can reconnect seamlessly after a reboot or radio re-enable, subject to the
profile's auto-connect setting. Open and password-protected networks behave the
same way.

## ESP32 use

The ESP32 adapter uses ESP-IDF directly and works in both Arduino-ESP32
sketches and native ESP-IDF applications. Construct the controller once, call
`begin()` from the application startup path, and run the scheduler regularly.

Here is a basic Arduino discovery-and-connect flow. Controller operations are
asynchronous, so each step starts after the preceding operation completes:

```cpp
#include <Arduino.h>
#include <roo_scheduler.h>
#include <roo_wifi.h>

roo_scheduler::Scheduler scheduler;
roo_wifi::WiFi wifi(scheduler);

class WifiListener : public roo_wifi::Listener {
 public:
  void onOperationFinished(
      const roo_wifi::OperationResult& result) override {
    if (result.status != roo_wifi::Status::kOk) {
      Serial.println("Wi-Fi operation failed");
      return;
    }
    if (result.kind == roo_wifi::OperationKind::kEnable) {
      // begin() restores the persisted radio state. Enable it if necessary,
      // then discover nearby access points.
      if (wifi.isEnabled()) {
        wifi.scan();
      } else {
        wifi.setEnabled(true);
      }
    } else if (result.kind == roo_wifi::OperationKind::kScan) {
      auto networks = wifi.scanSnapshot();
      for (size_t i = 0; i < networks.count; ++i) {
        const auto& network = networks.records[i];
        if (network.security != roo_wifi::AuthMode::kOpen) continue;
        roo_wifi::ConnectionConfig config;
        config.ssid = network.ssid;
        config.security = network.security;
        wifi.connect(config, {});  // Connect to the first open network.
        return;
      }
    } else if (result.kind == roo_wifi::OperationKind::kConnect) {
      Serial.println("Connected");
    }
  }
} listener;

void setup() {
  Serial.begin(115200);
  wifi.addListener(listener);
  if (wifi.begin() != roo_wifi::Status::kOk) {
    Serial.println("Could not initialize Wi-Fi");
  }
}

void loop() {
  scheduler.executeEligibleTasks();
}
```

For a protected network, populate `roo_wifi::Credentials` and pass it instead
of `{}`. For a connection that should be remembered, save a profile with
`saveProfile()` and connect using its profile ID.

## Custom persistence

The default ESP32 controller stores its `roo_prefs` collection in NVS. You can
instead pass any caller-owned `roo_prefs::Store` to the controller. For
example, the optional `roo_prefs::FilesystemStore` can place Wi-Fi settings and
passwords on an SD-backed `roo_io::Filesystem`:

```cpp
#include <roo_scheduler.h>
#include <roo_wifi.h>
#include "roo_io/fs/arduino/sdfs.h"
#include "roo_prefs/store/filesystem_store.h"

roo_scheduler::Scheduler scheduler;
roo_prefs::FilesystemStore preferences(roo_io::SD, "/prefs");
roo_wifi::WiFi wifi(scheduler, preferences);

void setup() {
  roo_io::SD.setCsPin(10);  // Configure the filesystem before wifi.begin().
  wifi.begin();
}
```

The supplied store must outlive `wifi`. Filesystem persistence is optional and
does not add a `roo_io` dependency to `roo_wifi`; applications opt into the
`roo_prefs` filesystem-store target and their chosen `roo_io` filesystem.

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

After a saved-profile connection succeeds, its ID is remembered. On restart or
radio re-enable, that profile reconnects when its `auto_connect` setting is true.
This applies equally to open and credential-protected profiles. Temporary
connections are never remembered, and explicitly disabling auto-connect keeps a
remembered profile from being started automatically.

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
