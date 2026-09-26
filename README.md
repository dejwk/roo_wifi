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

Station requests replace the desired state and return admission status. Native
work and coalesced state notifications run asynchronously on the scheduler.
Profile saves and deletes return their storage outcome synchronously.

```cpp
#include <Arduino.h>
#include <roo_scheduler.h>
#include <roo_wifi.h>

roo_scheduler::Scheduler scheduler;
roo_wifi::WiFi wifi(scheduler);

class WifiListener : public roo_wifi::Listener {
 public:
  void onStationStateChanged() override {
    const roo_wifi::Controller::State state = wifi.state();
    if (state.status != roo_wifi::Status::kOk) {
      Serial.println("Wi-Fi transition failed");
    }
    if (state.station == roo_wifi::Controller::StationPhase::kIdle &&
        !scan_requested_) {
      scan_requested_ = wifi.startScan() == roo_wifi::Status::kOk;
    }
  }

  void onScanStateChanged() override {
    const roo_wifi::ScanSnapshot networks = wifi.scanSnapshot();
    if (networks.generation == generation_) return;
    generation_ = networks.generation;
    for (size_t i = 0; i < networks.count; ++i) {
      const roo_wifi::ScanRecord& network = networks.records[i];
      if (network.security != roo_wifi::AuthMode::kOpen) continue;
      roo_wifi::ConnectionConfig config;
      config.ssid = network.ssid;
      config.security = network.security;
      wifi.connect(config, {});
      break;
    }
  }

 private:
  bool scan_requested_ = false;
  uint64_t generation_ = 0;
} listener;

void setup() {
  Serial.begin(115200);
  wifi.addListener(listener);
  if (wifi.begin() == roo_wifi::Status::kOk) wifi.setEnabled(true);
}

void loop() { scheduler.executeEligibleTasks(); }
```

`disconnect()` can interrupt a connection, including its wait for an IP address.
`connect()` can replace a pending connection or be submitted during teardown;
only the latest accepted intent starts after teardown finishes. `cancelScan()`
is scan-specific and idempotent. There is no public generic operation slot or
cancellation ID. See [API migration](docs/backend_migration.md) for threading,
state observation, persistence, and timeout details.

For a protected network, populate `roo_wifi::Credentials` and pass it instead
of `{}`. For a connection that should be remembered, save a profile with
`saveProfile()` and connect using its SSID.

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
category-specific invalidations: `onStationStateChanged()`,
`onScanStateChanged()`, and `onProfilesChanged()`. Read the latest state, scan
results, or profiles explicitly. The runnable scan
examples show how to enable the station and request a scan once the preceding
enable operation has completed.

There is one saved configuration per exact SSID (bytes and length, case-sensitive).
`saveProfile(settings, update)` creates or replaces the entry identified by
`settings.connection.ssid`. `loadProfile(ssid, out)`, `connect(ssid)`, and
`removeProfile(ssid)` use that same identity. Security remains a stored,
enforced policy. Changing the SSID creates another entry; remove the old entry
explicitly if it is no longer needed.

The store hashes SSID bytes with 64-bit FNV-1a and encodes the hash in network
byte order as unpadded Base64url. Keys are `p-` or `s-` plus 11 characters.
Both blobs retain the full SSID. A conflicting stored SSID returns
`kHashCollision` without exposing credentials or mutating records; there is no
collision-resolution catalog. Hashing is an addressing mechanism, not encryption.
Legacy numeric-ID records and their last-selection value are ignored. Re-save
networks after upgrading; there is no automatic migration.

Saved profiles can be discovered without a separate application catalog:

```cpp
wifi.forEachProfile([&](const roo_wifi::Ssid& ssid) {
  roo_wifi::Profile profile;
  if (wifi.loadProfile(ssid, profile) == roo_wifi::Status::kOk) {
    // Use the non-secret profile metadata.
  }
  return true;  // Return false to stop early.
});
```

Enumeration order is unspecified. Each profile uses compact, versioned settings
and secret values. Enumeration reads settings to recover each SSID; damaged
settings stop enumeration with `kCorrupt`. Credential errors are reported by
`loadProfile()`. Callback SSID references are borrowed for the callback only. Enumeration works independently of radio
enablement after `begin()`.

After a saved-profile connection succeeds, its SSID is remembered. On restart or
radio re-enable, that profile reconnects when its `auto_connect` setting is true.
This applies equally to open and credential-protected profiles. Temporary
connections are never remembered, and explicitly disabling auto-connect keeps a
remembered profile from being started automatically.

In a raw ESP-IDF application, initialize the default NVS partition before
constructing `WiFi`; the adapter creates the default event loop, station
netif, and Wi-Fi driver only when they are not already initialized. In either
environment, leave the physical station exclusively owned by `roo_wifi`.

## Saved security policy

A profile's `connection.security` is a persistent policy. A
`kWpa2Wpa3Personal` profile accepts WPA2/WPA3, WPA2-only, or WPA3-only APs
with the same SSID. Likewise, `kWpaWpa2Personal` accepts either constituent
mode. Single-mode profiles remain strict, and secured profiles never match
open APs. Existing mixed-mode profiles need no migration or re-saving.

Scan records report advertised security. After association,
`linkState().security` reports the mode supplied by the station's connection
event (normally the negotiated protocol). Connecting does not rewrite the
profile's policy. `SecurityAllows(policy, mode)` exposes the same compatibility
rule used for AP selection and association validation.

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
