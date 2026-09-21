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

## Version 2.0 backend API

All controller calls, listener registration/removal and destruction run on the
supplied scheduler context. Dependencies outlive the controller. Listeners may
submit follow-up commands from callbacks, but must not recursively dispatch the
scheduler or destroy/register/remove listeners during notification. Accepted
requests copy their input and return a nonzero ID; completion is deferred.
Rejection returns ID zero and has no completion event.

```
roo_scheduler::Scheduler scheduler;
MyRadio radio;
MyStore store;
roo_wifi::Controller::Options options;
options.startup_profile = 1;  // Zero disables startup selection.
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

Use `scan()` and `scanSnapshot()` instead of SSID-keyed network summaries.
Records include BSSID, security, RSSI/channel and available cipher/radio metadata.
The snapshot is borrowed until the next successful publication or shutdown.
A failed scan retains the previous snapshot; truncation is explicit. Consumer
presentation/grouping is independent of these records.

For provisioning, create `ProfileSettings` with an explicit SSID byte length and
security mode, and a `CredentialUpdate`. Open profiles require Clear; secured
new profiles require Replace; Keep requires a complete existing profile.
Call `saveProfile(known_key, settings, update)`, remember its request ID, and
call `connect(result.profile_id)` only from its successful result callback.
The radio may be off while saving. A subsequent Disabled or connection failure
does not undo persistence. `forEachProfile()` discovers committed profile keys
without a separate catalog; profile identity remains application-assigned.

`connect(config, credentials)` makes a temporary connection without writing
credentials. `connect(key)` copies the saved input before returning, so later
profile edits cannot change an admitted attempt. `removeProfile(key)` does not
disconnect. Explicit disconnect suppresses automatic reconnect until another
explicit connect or enable cycle. Startup reads only the configured key and
honors that profile's `auto_connect` setting.

An incomplete save requires replacement with complete input, never Keep.
`CommitUnknown` requires rereading before assuming success. Deleted profiles
remain absent even if physical field cleanup failed; retry deletion to clean up.
`PrefsStore::importLegacy(key, settings)` explicitly imports only
the supplied SSID's hashed legacy password. It never infers authentication from
password presence or automatically migrates scans; old preferences remain.

The existing `roo_windows_wifi::Configurator` takes the new Controller and an
optional caller-known profile key (default 1). It stores one provisioned profile
at that key, keeps its own display model, and no longer reads secrets for display.
Its example uses the same key as Controller::Options::startup_profile. UI-owned
SSID-only selection rejects ambiguity; explicit-security backend selection
remains supported. Applications needing multiple remembered configurations can
enumerate their assigned keys and load the corresponding non-secret metadata.

See [validation and release status](backend_validation.md) for capabilities,
measured resource bounds and physical-device acceptance still required before
publishing the prepared 2.0.0 release.
