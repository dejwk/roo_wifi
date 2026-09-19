# Backend validation and release status

The software backend and the existing Material 2 consumer are implemented.
Version 2.0.0 is prepared locally because the Controller, Interface, Store and
ESP32 convenience APIs break 1.1.x source compatibility. Publication and full
hardware acceptance remain pending. The original design document is preserved
as supplied; this file records implementation evidence rather than changing its
proposed status without completing its hardware acceptance gates.

## Reproduce

Run in `roo_wifi`, serially, using the persistent default Bazel output root and
configured global disk cache:

```
BAZELISK_SKIP_WRAPPER=true bazel test \
  //:portable_api_compile_test //:configuration_controller_test \
  //:configuration_store_test //:interface_lifecycle_test \
  //:backend_resource_test \
  --copt=-DROO_THREADS_USE_CPPSTD --copt=-fno-exceptions --copt=-fno-rtti
bazel test //:configuration_interface_test
bazel build //:esp32
```

The first command builds the real portable core without ESP32/Arduino defines
or SDK implementation dependencies. `:interface_conformance_test` aggregates
portable controller cases and the production ESP32 harness in the native build.
The named host test suites partition the shared controller test cases by contract.

In `roo_windows_wifi`, use the local backend until 2.0.0 is published:

```
BAZELISK_SKIP_WRAPPER=true bazel test //:model_test \
  --override_module=roo_wifi=/home/dawidk/Documents/Arduino/roo/roo_wifi \
  --copt=-DROO_THREADS_USE_CPPSTD --copt=-fno-exceptions --copt=-fno-rtti
bazel build //:roo_windows_wifi //examples/simple:simple \
  --override_module=roo_wifi=/home/dawidk/Documents/Arduino/roo/roo_wifi
```

## Tested behavior

- Admission owns input; connection completion requires address readiness.
- Switching waits for the old disconnect; delayed events retain their identity.
- Cancellation before execution and before association, same-SSID retry,
  repeated scans, snapshot retention, exclusive ownership and shutdown cleanup.
- Timeout cancellation holds the radio slot; an unsettled transition closes
  native admission. Late completions cannot finish a new request.
- FIFO handoff is bounded to 16 owned native events. Overflow faults the
  interface rather than dropping correlation and permitting further commands.
- Radio-off provisioning, known startup keys, explicit credential intent,
  saved profiles surviving connection failure, and temporary unsaved connections.
- Every marker/field interruption point, delete-cleanup retry, Keep failure on
  incomplete data, readable ambiguous final commits, unreadable commits, and
  unchanged outputs on read failure.
- Production ESP32 harness selects exact security between same-SSID APs,
  switches connections, verifies static-to-DHCP reset and randomized/device MAC
  restoration, rejects zero/broadcast address readiness, rejects competing station
  owners, and reloads small
  profile fields through the actual preferences adapter.
- The migrated UI model saves before connecting, reserves a caller-known key,
  and refuses ambiguous SSID-only security selection. The backend itself can
  connect to an explicitly requested security mode among same-SSID APs.

## Memory measurements

Linux x86-64, exceptions/RTTI disabled. ScanRecord is 50 bytes; Controller is
880 bytes; OrderedInterface is 2032 bytes (including its fixed event handoff).
Heap counters include scheduler allocation, the fake native scan vector,
controller records and one fake stored profile. They exclude stack objects,
allocator bookkeeping and hardware/SDK allocations.

| AP limit | Retained heap bytes | Peak heap bytes |
| ---: | ---: | ---: |
| 0 | 1690 | 1691 |
| 20 | 3690 | 3691 |
| 40 | 5690 | 5691 |
| 100 | 11690 | 11691 |

After warm-up, 200 more scan/cancel/profile-load cycles retain the same allocation
count. Snapshot, link, support and activity observation allocate nothing. These
are host measurements, not ESP32 ABI or driver heap measurements. Production
ESP32 additionally retains up to N converted records, transiently reads up to N
native AP records (at least one for draining an empty-capacity scan), and bounds
its internal authentication candidate scan to 100 records. Driver-owned scan
storage is SDK-managed and requires physical-device measurement.

## Native assumptions and unresolved acceptance

The IDF event loop serializes native posted events. The adapter copies complete
payloads into its own FIFO; application operation IDs are assigned by the
ordered lifecycle, never inferred from the currently selected SSID in a delayed
callback. AP selection prepares native configuration on the event source and
continues the connection on the scheduler, so cancellation cannot race a new
connect issued independently from an event callback.

References: [IDF event loop](https://github.com/espressif/esp-idf/blob/v4.4/components/esp_event/esp_event.c),
[Wi-Fi native API](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/api-reference/network/esp_wifi.html).
The pinned roo_testing 2.1.2 harness uses its current Arduino/IDF compatibility
implementation behind the legacy 2.0.4 Bazel label; it is not a hardware test of
Arduino 2.0.4 or IDF 4.4 binary-driver behavior.

Arduino Preferences performs `nvs_set_blob` followed by `nvs_commit` before
returning success. Each FieldStore field is a separate committed value; the
Transaction object only opens/closes access. This establishes the intended
ordered-write API dependency, not a measured power-cut durability result.

No physical ESP32 serial device was available during implementation. Before
publication, check ordinary switching, pre-association cancellation, cancellation
of the internal discovery scan, hidden SSIDs, supported WPA3/transition modes,
static-to-DHCP reset, randomized/device MAC restoration, NVS power interruption,
and actual target retained/peak driver memory. Do not substitute emulator success
for these checks. In particular, the harness scan-stop shim suppresses completion
instead of emitting a scan-done lifecycle event; the backend times out and
quarantines such an unsettled operation rather than fabricating cancellation.
Native event-loop teardown quiescence also needs validation on each supported SDK.

The radio rejects embedded-NUL connection SSIDs because its directed-scan filter
uses native C-string input. Observed scan SSIDs have the native record's length
limitation. Public portable SSIDs preserve their explicit byte length. Enterprise
and unknown authentication remain observable but provisioning is unsupported.
Recovery from a fault requires shutdown and fresh controller/adapter instances;
normal switching never restarts the driver.
