# Backend migration

Include `roo_wifi.h` for portable construction with caller-owned Interface,
Store and Scheduler dependencies. Include `roo_wifi/esp32.h` explicitly for
ESP32 convenience construction. Bazel consumers of that adapter depend on
`@roo_wifi//:esp32`; `@roo_wifi` contains only the portable core.

SDK-free validation (bypass the workspace's default Arduino wrapper):

```
BAZELISK_SKIP_WRAPPER=true bazel test //:portable_api_compile_test //test:controller_test --copt=-DROO_THREADS_USE_CPPSTD --copt=-fno-exceptions --copt=-fno-rtti
bazel build //:esp32
```

The host graph selects standard C++ threading. Platform constraint labels may
appear in `cquery deps(...)`, but no Arduino/ESP-IDF implementation is linked.

## State-machine controller API

All controller calls, listener registration/removal and destruction run on the
supplied scheduler context. Dependencies outlive the controller. Listeners may
submit follow-up commands from callbacks, but must not recursively dispatch the
scheduler or destroy/register/remove listeners during notification. Accepted
station requests copy their input and return `Status`: `kOk` means intent was
accepted, not that the hardware reached it. The latest accepted intent wins.
`Listener::onStationStateChanged()`, `onScanStateChanged()`, and
`onProfilesChanged()` deliver independently coalesced invalidations on the same
scheduler context; read `state()` and `scanSnapshot()` explicitly. Intermediate
states may be skipped. Native events retain ordering internally and are never
delivered on arbitrary threads to application listeners.

`state().desired` and `state().station` describe requested and observed station
state. `revision` changes on replacement or explicit retry, while `status` and
native diagnostic fields describe the latest outcome. Success alone does not
imply connectivity: use `kConnected`/`kAddressReady`. `connected_profile` identifies
a saved profile that reached address readiness. `profiles_generation` invalidates
profile metadata after writes, including potentially partial failures.

Station commands are `setEnabled()`, `connect()`, and `disconnect()`; there is no
`RequestResult` or public `cancel(id)`. A new connection while disconnecting is
retained as the target, and starts after native teardown. Repeating identical
healthy intent is a no-op. Disabling also cancels an active scan. Configuration
and credentials are copied and retained for that desired connection, including
saved-profile retries, then discarded when intent is replaced or shutdown runs.

Scanning uses `startScan()`/`cancelScan()`. Read `state().scan`, `scan_status`, and
`scan_revision` for progress and outcomes. Starting while scanning or during a
station transition returns Busy. Cancellation stays active until native scanning
stops; cancelling an idle scan succeeds without another transition.

```
roo_scheduler::Scheduler scheduler;
MyRadio radio;
MyStore store;
roo_wifi::Controller::Options options;
roo_wifi::Controller wifi(radio, store, scheduler, options);
roo_wifi::Status error = wifi.begin();
```

`begin()` restores persisted enablement asynchronously. A listener observes
physical enablement separately from its operation's persistence result. A
connection succeeds at AddressReady, not association or internet reachability.

For ESP32, include `roo_wifi/esp32.h`, construct
`roo_wifi::Esp32WiFi station(scheduler, options)`, then use
`station.begin()` and its inherited controller API for every operation.
There is only one owner of the process-global physical station.

Use `startScan()` and `scanSnapshot()` instead of SSID-keyed network summaries.
Records include BSSID, security, RSSI/channel and available cipher/radio metadata.
The snapshot is borrowed until the next successful publication or shutdown.
A failed scan retains the previous snapshot; truncation is explicit. Consumer
presentation/grouping is independent of these records.

For provisioning, create `ProfileSettings` with an explicit SSID byte length and
security mode, and a `CredentialUpdate`. Open profiles require Clear; secured
new profiles require Replace; Keep requires a complete existing profile.
`saveProfile(settings, update)` and `removeProfile(ssid)` are synchronous and
return storage outcomes. Saving the same SSID updates its existing configuration.
`connect(ssid)` loads settings and credentials at admission. Later edits do not
change the admitted attempt. Removing a saved entry does not disconnect it.
`forEachProfile()` visits borrowed `const Ssid&` values in unspecified order.
Corrupt settings stop enumeration with `kCorrupt`; credential corruption is
reported by `loadProfile(ssid, out)`. Enumeration is available while radio is off.

The old `ProfileId` API has been removed. Callers no longer allocate keys or
maintain a mapping between SSIDs and IDs. Empty SSIDs represent no saved profile
in controller state and clear the last selection in the store. Other profile
operations require 1–32 bytes. Security policies remain independent of identity.

Faulted and new radio requests fail; profile operations remain available.
Explicit disconnect suppresses automatic reconnect until another
explicit connect or enable cycle. A successful saved-profile connection becomes
the persisted restart choice. Startup and radio re-enable load that profile and
connect only when its `auto_connect` setting is true. Open profiles follow the
same policy as credential-protected profiles; temporary connections are not
remembered.

Each profile uses version-2 settings and secret blobs under `p-` and `s-` keys.
The suffix is the unpadded Base64url encoding of a 64-bit FNV-1a SSID hash in
network byte order (13 characters including the prefix). Both values contain
the full SSID; mismatches return `kHashCollision` before any mutation. Orphaned
secrets are checked before save and delete too. The last successful SSID is
stored under `last-ssid` as its length byte followed by its bytes.
Legacy numeric-ID records are ignored, including their last-selection value;
networks need to be saved again. The radio-enabled preference is retained.

`roo_windows_wifi::Configurator` now takes the controller without a profile key.
`WifiSettingsFlow(context, controller, policies)` optionally borrows an
application policy provider addressed by SSID. The ID allocator has been removed.
