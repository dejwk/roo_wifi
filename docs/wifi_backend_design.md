# Roo Wi-Fi Backend Foundation Design

## Implementation status

**Proposed.** The existing controller and ESP32 adapter provide the starting
point; this document's portable contracts and extensions are not implemented.
The [Material 3 Wi-Fi UI design](../../roo_windows/docs/design/proposed/material3_wifi_configuration_design.md)
is a separate consumer design, not the specification for this library.

## Objective

Provide a capable, platform-independent Wi-Fi backend for embedded applications:
AP discovery, connection control, persistent profiles, supported link
configuration, and precise asynchronous outcomes. Implement the first production
adapter on ESP32 while allowing future platforms to implement the same public
contracts without importing ESP32, Arduino, or UI types.

## Motivation

Headless devices need reliable provisioning, saved connection configurations,
and diagnostics just as interactive devices do. Today's SSID/password-oriented
controller loses useful AP metadata and offers incomplete persistence and
operation outcomes. Extending it with reusable domain primitives serves all of
these consumers and keeps presentation decisions outside the backend.

## Background

The current [Controller](../src/roo_wifi/controller.h) coordinates an
[Interface](../src/roo_wifi/hal/interface.h), [Store](../src/roo_wifi/hal/store.h),
and scheduler. It serializes native notifications onto that scheduler. Its
scan model primarily exposes SSID, open/secured state, and RSSI; the HAL already
has richer AP authentication and radio metadata. The legacy store persists
radio enablement, default SSID, and passwords rather than enumerable profiles.

The [ESP32 adapter](../src/roo_wifi/hal/esp32/esp32_arduino_interface.h) uses Arduino
Wi-Fi and process-global station events. The [umbrella header](../src/roo_wifi.h)
conditionally exposes an ESP32 convenience controller, but the current
[build target](../BUILD) includes both core and platform implementation with an
ESP32 Wi-Fi dependency. Portable class names alone do not establish build or
behavioral independence from that platform.

The current [roo_prefs storage API](../../roo_prefs/src/roo_prefs/store/preferences_store.h)
provides named-key reads/writes, blobs, and explicit status codes, but no key
iteration. Its [Transaction](../../roo_prefs/src/roo_prefs/transaction.h) opens
and closes collection access; it is not an atomic multi-key commit. The current
[Wi-Fi preferences adapter](../src/roo_wifi/hal/esp32/arduino_preferences_store.cpp)
stores passwords under hashes of SSIDs and stores only the default SSID in
recoverable text. This constrains both enumeration and legacy migration.

## Requirements

1. Expose AP discovery and connection state without UI summaries, row identities,
   display strings, or action-availability flags.
2. Support headless observation, provisioning, temporary connections, and stored
   profiles through one public API; radio-off profile management must work.
3. Report full authentication metadata and enforce requested security without
   silently downgrading or conflating same-SSID networks.
4. Give accepted operations distinct identities and terminal outcomes; distinguish
   rejection, completion, cancellation, timeout, and subsequent link changes.
5. Preserve native event identity/order and safe listener/input lifetimes through
   asynchronous dispatch, including cancellation and destruction.
6. Support lookup and updates of known saved configurations, explicit credential
   intent, reuse of recoverable legacy data, and reported storage failures.
   Listing every saved configuration is not required.
7. Apply supported hidden-network, reconnect, DHCP/static IPv4, and station MAC
   configuration; report actual support and available link diagnostics.
8. Keep public domain and HAL contracts free of ESP32/Arduino/RTOS-specific types.
   Core code and conformance tests must build without an ESP32 SDK or driver.
9. Allow another platform to supply radio and storage implementations without
   changing the controller's domain model or introducing a dependency on the UI.
10. Keep memory bounded by configured scan capacity, live operations, and loaded
    profile data. Support exception- and RTTI-disabled embedded builds.

Out of scope: UI widgets, application proxy/metered policies, internet probing,
enterprise credential provisioning, static IPv6 editing, and a second production
platform adapter. These exclusions do not justify dropping observed metadata
or making the portable API dependent on ESP32 limitations.

## Design Overview

The controller owns Wi-Fi policy and observable state. A platform `Interface`
owns native station operations and translates native events. An independent
`Store` owns profile persistence; a platform can pair its radio with an in-memory,
flash-backed, or application-supplied store.

| Concept | Ownership and purpose |
| --- | --- |
| AP record / scan snapshot | Controller-owned scan data; consumers borrow it until the documented snapshot mutation. A BSSID identifies an AP, not a saved profile. |
| Connection configuration | Caller-owned input copied as needed on admission; describes a connection independently of persistence. |
| Profile / profile ID | Application-assigned lookup key for durable configuration; known without a scan or store listing. |
| Operation ID / result | Correlates one admitted command and its terminal result; a connection identity remains useful for subsequent link events. |
| Backend support | Reports real radio/store operations and diagnostics, independent of what any caller chooses to expose. |

These concepts separate observation, execution, and persistence: scan indices
cannot become profile IDs; saving is not connecting; association is not address
readiness or internet availability. A provisioning service can save configuration,
save under a known profile key, then request a connection by that key. A temporary diagnostic tool
can connect to explicit configuration without writing credentials to flash.

The dependency direction is application -> controller -> portable Interface/Store
contracts. Concrete adapters implement those contracts; the core does not depend
on their headers or SDKs. The ESP32 state machine is an implementation of the
portable event/result contract, not a universal event sequence imposed on every
future OS or radio.

## Design Details

### Platform Boundary and Build Structure

Use portable fixed-width values, byte-oriented SSIDs, BSSIDs, authentication
requirements, typed IPv4 settings, and portable result categories in public
headers. Preserve SSID byte length rather than assuming native C-string layout.
Portable enums and errors are library-defined; they do not require matching
Arduino or ESP-IDF numeric constants. Native errors can accompany a portable category as optional adapter-specific
numeric diagnostics; portable callers must not need ESP error constants to
handle ordinary outcomes. Scheduling uses the library's existing portable
scheduler abstraction; platform event threads marshal owned payloads into it.

Split the Bazel library into a portable core and an ESP32 adapter target. Keep
SDK includes, Arduino preferences, native global station registration, and any
platform convenience construction under the adapter. The generic umbrella
header exposes the portable controller; an explicit platform header exposes
ESP32 convenience construction. Breaking source changes are allowed, and
migration documentation replaces compatibility layers with weaker semantics.

Add a second, non-ESP32 **test implementation** of Interface and Store and a host
compile target with no Arduino/ESP-IDF dependencies or ESP32 feature defines.
Reuse a contract suite across this implementation and the ESP32 adapter harness.
A future production adapter implements these same interfaces and reports its
own supported features; its implementation does not have to mimic ESP-IDF's
callback names, event-loop structure, or disconnect sequencing.

There is no platform plugin registry or runtime adapter discovery. The caller
supplies concrete dependencies at construction. Maintain one owner per physical
station, while allowing separate controller instances for distinct interfaces.

### Backend Contract: Wi-Fi State and Operations

The following are proposed backend additions, not an API facade tailored to a particular consumer. Reuse and extend existing `roo_wifi` domain types and listeners;
change or remove existing APIs where necessary. Backward compatibility with
1.1.x is not a constraint; migrate callers instead of retaining a second legacy
execution path. Do not introduce `NetworkSummary`, `ConfigurationDetails`, `can_connect`,
`can_edit`, row selection, or per-screen capability flags into `roo_wifi`.

| Backend concept | Meaning independent of a UI |
| --- | --- |
| Scan result | AP information: SSID, BSSID, full `AuthMode`, RSSI, channel, and available radio metadata. Each result describes an AP; grouping and presentation order are consumer choices. |
| Scan snapshot | Results of one scan, with documented lifetime and completion/failure notification. Indices are valid only within that snapshot; they are not persistent network identities. |
| Connection state | Selected connection parameters, association/address-acquisition state, typed failure information, and available effective link diagnostics. No labels, badges, internet claims, or allowed UI actions. |
| Saved profile and `ProfileId` | Persistent connection configuration and credential reference, addressed by an application-assigned key across restart. There is no required collection or generated identity service. |
| Connection configuration | SSID/security requirements, explicit credential intent, hidden-network handling, auto-connect policy, DHCP/static IPv4, and supported station MAC policy. No form text, proxy policy, or metered treatment. |
| Backend support | Supported authentication/configuration operations and available diagnostics from the selected HAL/store. It does not describe whether a settings control is visible or editable. |
| Request/result | Admission, operation identity, completion, and errors for backend work. Results refer to the operation and, when relevant, its profile/connection target, never a UI row handle. |

Scan results preserve the existing `AuthMode` values, including transition
modes. A caller can inspect enterprise/unknown modes independently of any consumer's configuration facilities. Do not filter backend data to the personal modes offered by
a particular consumer. Likewise, a platform-supported operation is not disabled
in the backend merely because one consumer does not expose it.

Backend operations cover:

- enabling/disabling the interface and requesting scans,
- observing current connection and scan state,
- loading, saving, and deleting profiles by a caller-known key,
- connecting using a saved profile ID or explicit connection configuration,
- disconnecting, cancelling supported operations, and reporting outcomes,
- validating and applying supported Wi-Fi/IP settings.

These are independent primitives. A provisioning workflow can compose save and connect: a save-and-connect action waits for successful
profile persistence, then issues a connection request. The backend does not
need a combined save-and-connect operation. A headless provisioning caller
can use this sequence, while a device with temporary credentials can
connect without saving a profile. Saving works with the radio disabled;
connecting while disabled reports an explicit error.

For example, a headless provisioning service saves an SSID/security/IP profile,
uses its known key to request a connection. The Wi-Fi UI
uses the same operations and translates their results into inline feedback.
Neither caller needs a display summary, scan-row handle, or activity state in
the backend.

#### Ownership, Concurrency, and Results

Keep the existing scheduler dispatch and listener lifetime contract. Document
which calls require the scheduler context and exactly when borrowed scan or
profile views expire. Accepted asynchronous operations must own their required
inputs; queued work cannot borrow a caller's temporary strings. Detach listeners
and cancel or neutralize queued callbacks before their owners are destroyed.

Admission is distinct from completion. Preserve each accepted operation's
identity through its terminal result. Reject conflicting operations with a
typed busy result according to the HAL's actual constraints; do not impose a
single global operation slot for unrelated work. Each consumer can track its
own requests or observe all backend events.

Cancellation has a defined terminal outcome in the adapter's connection state
machine. Preserve native event order and identity through scheduler dispatch;
sequence a new connection after the previous attempt's disconnect/failure
outcome. A request ID identifies backend work, but does not replace native
SSID/BSSID information or make overlapping attempts distinguishable by itself.
Test same-SSID retries, cross-SSID switches, and successive scans using native
lifecycle sequences, including delayed application processing. Treat IP events
and cancellation before association explicitly, as described below. Immediate
HAL acceptance must not be presented as confirmed physical completion.

#### Persistence and Platform Work in `roo_wifi`

`Store` provides direct access by a caller-known profile key. It does not expose
iteration, allocate profile IDs, or maintain a catalog. An application can use a
single fixed key for its provisioned network, or retain its own mapping for
multiple configurations. Listing saved networks is a consumer feature, not a
prerequisite for Wi-Fi persistence.

Extend `Interface` and the ESP32 implementation to apply authentication,
hidden-network, DHCP/static IPv4 (address, prefix, gateway, primary and optional
secondary DNS), and supported station MAC settings before connection. Changing
back to DHCP clears earlier static settings. Report effective IP/MAC/link data
with availability, without inventing unsupported diagnostics. Auto-connect
policy governs reconnect behavior after restart and explicit disconnect;
explicit disconnect suppresses automatic reconnect until the next explicit
connect or enable cycle.

Credential updates have explicit keep/replace/clear intent. Reading profile
metadata need not expose the old secret. Validate domain values in the backend
for every caller; the UI additionally validates field text for useful feedback.
A failed multi-field save can leave that profile unavailable; it must never
silently expose a mixture of old and new settings as a valid configuration.
Connection failure does not implicitly delete saved settings. Unsupported
non-default configuration is rejected rather than dropped.

### Small Preferences Values and Known Keys

Use `roo_prefs` as a small-value store, without building a profile database on
top of it. `ProfileId` is a nonzero application-assigned 32-bit key; saving that
key creates or replaces its configuration. Zero means no profile. The application
owns key selection and reuse; deleting and recreating a key intentionally refers
to the same application location, not a new generated identity.

The preferences adapter addresses each field directly. A key consists of eight
hexadecimal profile-ID digits plus a short field suffix, within the native key
length limit. Store SSID bytes (at most 32), credential bytes (at most 64), and
individual booleans/enums/IPv4 values separately. A small format/status value
marks a profile as ready, incomplete, or deleted. No field exceeds 64 bytes;
there are no slot limits, profile-list blobs, bank buffers, revision counters,
or index writes. Native storage exhaustion remains an explicit storage failure.

A save loads the old credential only when Keep is requested, validates all input,
and then performs these ordered writes:

1. Persist an incomplete status before changing any field. If this fails, stop
   without changing fields; report storage failure.
2. Write each required field, checking every result. Clear removes stored secret
   material logically. Omitted/default fields have explicit defaults in the
   versioned schema; do not leave a stale static-IP setting active.
3. Write ready status last, only after all field writes succeeded.

Reads return Incomplete for an interrupted update and never return such data as
usable configuration. A failed final status write requires rereading status and
fields: confirm success only when the requested configuration is readable;
otherwise report Incomplete, StorageFailure, or CommitUnknown when the outcome
cannot be determined. A failed save does not promise preservation of the previous
configuration. Recovery is an explicit replacement using complete input; Keep
cannot recover an incomplete profile. This is a deliberate weaker contract than
atomic replacement, adequate for provisioning without a custom transaction layer.

Deletion writes deleted status first; after that, cleanup of individual fields
can be retried without resurrecting the profile. A cleanup failure is reported,
although lookup can already return NotFound. Logical deletion does not guarantee
physical secure erasure. Repeated removal of a deleted key retries cleanup.

This protocol requires ordered, durable successful writes from the concrete
preferences adapter, verified in Phase 4. Namespace access through
`roo_prefs::Transaction` does not establish that guarantee. Tests must prove that
a completed incomplete marker survives before subsequent field changes can
survive; otherwise this adapter cannot claim safe interrupted-update detection.
No claim of cross-key atomicity is made.

Radio enablement remains a separate boolean. The application supplies a known
startup profile key through ControllerOptions (zero disables startup selection).
When enabled, the controller loads only that profile and honors its auto-connect
setting. Missing/incomplete startup data produces an explicit failure and no
connection attempt; it does not trigger a search through saved profiles.

#### Legacy Data and Optional Catalogs

Retain legacy preferences. At explicit migration time, the application supplies a
destination key and SSID (or reads the legacy default SSID), and the adapter reads
the existing hashed password key directly. Security must be supplied or resolved
before saving a usable profile; do not guess from password presence. Import uses
the same save protocol and does not run automatically on scans or after deletion.
There is no full-store migration or requirement to discover legacy SSIDs.

A consumer that needs a saved-networks page can provide a separate catalog of
known keys using storage appropriate to that application. The core Store does
not acquire optional iterator stubs solely for that UI. Without a catalog,
provisioning, known-profile editing, and direct connection still work. Neither
scanning nor loading one profile implicitly builds a complete saved list.

### HAL Evolution and Native Event Correlation

Fix the HAL contract and its concrete adapter together. The current
SSID/password-only connect call, unqualified native events, and void setters
are not architectural constraints. Breaking changes to `roo_wifi::Interface`,
`Controller`, `Store`, and their implementations are allowed. A caller migration
and appropriate new release are part of this work; compatibility overloads,
default no-op methods, or a legacy adapter are not required.

The new HAL must provide these backend guarantees:

1. A connection request carries explicit authentication requirements, credentials,
   and supported station/IP configuration. The adapter applies those requirements
   through the native SDK and rejects settings it cannot enforce. Same-SSID APs
   are not grounds for blanket rejection: select candidates meeting the requested
   security constraints, using BSSID selection internally when needed. Do not
   treat a native minimum-authentication threshold as exact-mode enforcement.
2. Each accepted scan or connection attempt has an operation ID supplied at
   admission. Completion echoes that ID. Link events after connection establishment
   carry the established connection's ID; later attempts cannot adopt it.
   A result's ID is assigned from known ownership, not from whichever request is
   current when an unqualified callback happens to arrive.
3. Immediate rejection, asynchronous success/failure, cancellation, and timeout
   are distinct typed results. Each accepted operation gets one terminal result;
   an established connection can subsequently emit separate link-state events.
   Preserve native failure detail alongside portable categories where useful.
4. Cancel/disconnect/disable have asynchronous completion when the native
   transition is asynchronous. Complete them from the matching lifecycle
   outcome, or an explicit already-idle result, before starting a conflicting
   transition. An immediate return means admission, not physical disconnection
   or radio shutdown.
5. Supported configurations and diagnostics are explicit and fail closed for
   genuinely unavailable platform features. Extend concrete implementations
   rather than reporting everything unsupported to preserve the old interface.
6. One adapter owns the native station and its event stream. Multiple independent
   controllers must not mutate the same process-global ESP32 station. Observers
   subscribe to its backend state; they do not create competing hardware owners.

#### ESP32 Implementation Strategy

Use one adapter connection state machine and preserve the native event stream.
ESP-IDF's event loop queues events in FIFO order; this is ordering of posted
events, not a blanket guarantee about when independent producers generate them.
The adapter must preserve that order when copying events to the controller's
scheduler rather than introducing reordering or reinterpreting old events using
a newly selected target. See the [ESP-IDF 4.4 event-loop implementation](https://github.com/espressif/esp-idf/blob/v4.4/components/esp_event/esp_event.c).

Station connected/disconnected payloads carry SSID and BSSID. Retain that data
and the native reason code when delivering backend events. ESP-IDF documents
reconnection from `WIFI_EVENT_STA_DISCONNECTED`; use that ordinary lifecycle
instead of assuming arbitrarily reordered events across SSIDs. See the
[Wi-Fi event payloads](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/api-reference/network/esp_wifi.html)
and [disconnect/reconnect guidance](https://docs.espressif.com/projects/esp-idf/en/v4.4/esp32/api-guides/wifi.html#wifi-event-sta-disconnected).

The normal switch from network A to B follows this sequence:

1. Retain A as the active connection/attempt and B as the requested next target.
   Disable independent Arduino auto-reconnect so one backend owns retry policy.
2. Request disconnection/cancellation of A. Process already queued A events in
   their original order without completing B or starting independent retries.
3. On A's disconnect/failure outcome, settle A's operation and apply B's
   connection configuration. If the station was already idle, use the native
   API's documented idle outcome instead of waiting for an event that cannot
   occur.
4. Start B, associate its connected event with B using the state and native
   identity, and report address readiness separately from association.

Use the same lifecycle sequencing for retries of the same SSID: SSID equality
alone cannot distinguish attempts, so the state transition between them matters.
Assign backend operation IDs at admission and associate native events with the
operation established by that state machine. Do not overwrite the active target
with B merely because the caller has requested B.

IP events require their own handling. They describe interface/address state
rather than necessarily identifying an SSID or connection attempt. Only report
address readiness for the active association, preserving event order and
checking the relevant interface/state. A delayed IP-loss notification must not
be treated as failure of a newly requested Wi-Fi connection. Cancellation before
association also needs a defined native outcome: inspect the pinned SDK's return
and event behavior rather than unconditionally waiting for a connected-station
disconnect event. Cover these cases in focused adapter tests.

Scans use their own native completion/failure lifecycle. Complete or cancel the
active scan and release its result resources before starting a conflicting scan;
do not treat a queued result from the previous scan as a new scan's completion.
Preserve any native scan identity available, without assuming it is a universal
application request token.

Use ESP-IDF Wi-Fi/netif/event APIs from the Arduino build when the wrapper loses
required payloads, error codes, configuration, or lifecycle information. This
permits a correct backend implementation without imposing compatibility with
the current narrow HAL. It does not require replacing the entire wrapper when
its existing behavior already satisfies the contract.

Station stop/restart or netif reinitialization is **fault recovery**, not the
normal switching or cancellation path. Add such recovery only for a demonstrated
timeout or native-state failure, document its trigger, and test it separately.
Validate the native lifecycle guarantees actually used, preservation of those
guarantees through the adapter, and concrete edge cases. Check resource bounds
with repeated operations and use targeted hardware smoke tests for SDK-sensitive
behavior.

### Memory and Work Bounds

Let N be the configured maximum AP records in a scan and K the bounded number
of in-flight native operations supported by an adapter. Scan payload is O(N);
observation does not copy the snapshot per listener. Load only the selected
profile and owned input for active work. Preferences scratch is at most one
64-byte field buffer plus one decoded configuration/credential; there is no
memory reserved per saved network. Each lookup/update performs a bounded number
of field accesses independent of the number of saved networks. The core retains
O(K) operation state, with no lifetime-growing history or per-scan handle map.

For scale, a scan record containing a 32-byte SSID plus length, 6-byte BSSID,
security, RSSI, and channel starts around 42 bytes before optional metadata and
ABI padding. N=100 therefore means roughly 4.2 KB of payload, not a guaranteed
object size. Holding old and replacement snapshots doubles that portion during
publication; measure peak as well as retained capacity. Keep limits caller-set
and report truncation explicitly. Snapshot bounds and borrowed lifetimes are
part of the contract; no allocation is permitted merely to observe a record.

Include private state in measurements: scheduler tasks, listener storage, native
event handoff, the active configuration/credential copy, and adapter command
state. Repeated scan/connect/cancel cycles must reach a stable retained-memory
plateau. Record target-ABI sizes and native buffer costs during implementation;
formatting strings and display caches belong to consumers.

## Proposed API

These are concrete proposed C++17 declarations, syntax-checked together with the
consumer below; they are not yet library implementation. Bodies and controller
private state are omitted from declarations, with retained state specified below.
Types arrive alongside their implementing phase rather than as unused stubs.

### Domain Types, Store, HAL, and Controller

The fixed-size domain records deliberately avoid heap ownership ambiguity;
implementation headers will provide full method documentation and validation.

```cpp
#include <stddef.h>
#include <stdint.h>

namespace roo_scheduler {
class Scheduler;
}

namespace roo_wifi {

using ProfileId = uint32_t;  // Caller-assigned key; zero means no profile.
using OperationId =
    uint64_t;  // Nonzero, never reused within a controller lifetime.

enum class Error : uint8_t {
  kOk,
  kNotFound,
  kInvalidArgument,
  kUnsupported,
  kBusy,
  kDisabled,
  kNotStarted,
  kCancelled,
  kTimeout,
  kConnectionFailed,
  kStorageFailure,
  kCommitUnknown,
  kCorrupt,
  kIncomplete
};

/// An SSID is up to 32 bytes, not necessarily a null-terminated string.
struct Ssid {
  uint8_t bytes[32] = {};
  uint8_t size = 0;
};
struct MacAddress {
  uint8_t bytes[6] = {};
};
struct Ipv4Address {
  uint8_t bytes[4] = {};
};

/// Library-defined values; no dependency on native SDK enum numbering.
enum class AuthMode : uint8_t {
  kUnknown,
  kOpen,
  kWep,
  kWpaPersonal,
  kWpa2Personal,
  kWpaWpa2Personal,
  kWpa3Personal,
  kWpa2Wpa3Personal,
  kEnterprise,
  kWapiPersonal,
  kOther
};
enum class IpMode : uint8_t { kDhcp, kStaticIpv4 };
enum class MacPolicy : uint8_t { kDevice, kRandomized };

struct StaticIpv4 {
  Ipv4Address address, gateway, dns1, dns2;
  uint8_t prefix_length = 24;
  bool has_dns2 = false;
};

/// Connection parameters without secrets; security is an enforced requirement.
struct ConnectionConfig {
  Ssid ssid;
  AuthMode security = AuthMode::kUnknown;
  bool hidden = false;
  IpMode ip_mode = IpMode::kDhcp;
  StaticIpv4 static_ipv4;
  MacPolicy mac_policy = MacPolicy::kDevice;
};

/// Secret material for one admitted attempt, never part of profile metadata.
enum class CredentialEncoding : uint8_t { kPassphrase, kRawPsk, kWepKey };
struct Credentials {
  CredentialEncoding encoding = CredentialEncoding::kPassphrase;
  uint8_t bytes[64] = {};
  uint8_t size = 0;
};
enum class CredentialIntent : uint8_t { kKeep, kReplace, kClear };
struct CredentialUpdate {
  CredentialIntent intent = CredentialIntent::kKeep;
  Credentials replacement;  // Used only for kReplace.
};

struct ProfileSettings {
  ConnectionConfig connection;
  bool auto_connect = true;
};
struct Profile {
  ProfileId id = 0;
  ProfileSettings settings;
  bool has_credentials = false;
};

struct SaveResult {
  Error error = Error::kOk;
  ProfileId id = 0;
};

/// Persistence adapter: synchronous operations executed on the controller
/// context.
class Store {
 public:
  virtual ~Store() = default;
  virtual Error begin() = 0;
  virtual Error loadProfile(ProfileId id, Profile& out) const = 0;
  /// Privileged backend access, used only to construct connection input.
  virtual Error loadCredentials(ProfileId id, Credentials& out) const = 0;
  /// Create or replace a known nonzero key; kKeep requires a valid old profile.
  virtual SaveResult saveProfile(ProfileId id, const ProfileSettings& settings,
                                 const CredentialUpdate& credential) = 0;
  virtual Error removeProfile(ProfileId id) = 0;
  virtual Error readEnabled(bool& out) const = 0;
  virtual Error writeEnabled(bool enabled) = 0;
};

struct ScanRecord {
  Ssid ssid;
  MacAddress bssid;
  AuthMode security = AuthMode::kUnknown;
  int8_t rssi_dbm = -128;
  uint16_t channel = 0;
};
/// Borrowed until next successful scan publication or controller shutdown.
struct ScanSnapshot {
  uint64_t generation = 0;
  const ScanRecord* records = nullptr;
  size_t count = 0;
  bool truncated = false;
};
struct ScanRead {
  size_t count = 0;
  bool truncated = false;
};
struct Support {
  uint32_t authentication_modes = 0;  // Bit positions are AuthMode values.
  bool hidden_networks = false;
  bool static_ipv4 = false;
  bool randomized_mac = false;
  bool scan_while_connected = false;
};

enum class LinkPhase : uint8_t {
  kIdle,
  kConnecting,
  kAssociated,
  kAddressReady
};
struct LinkState {
  OperationId connection_id = 0;  // The connect operation that established it.
  LinkPhase phase = LinkPhase::kIdle;
  Ssid ssid;
  MacAddress bssid, station_mac;
  AuthMode security = AuthMode::kUnknown;
  int8_t rssi_dbm = -128;
  uint16_t channel = 0;
  Ipv4Address address, gateway, dns1, dns2;
  bool has_radio_info = false, has_station_mac = false, has_ipv4 = false;
  bool has_dns1 = false, has_dns2 = false;
  Error reason = Error::kOk;
};
enum class OperationKind : uint8_t {
  kEnable,
  kScan,
  kConnect,
  kDisconnect,
  kSave,
  kRemove
};
struct RequestResult {
  OperationId id = 0;  // Zero: rejected, no completion callback.
  Error error = Error::kOk;
};
struct OperationResult {
  OperationId id = 0;
  OperationKind kind = OperationKind::kScan;
  Error error = Error::kOk;  // kOk or one terminal failure/cancellation.
  ProfileId profile_id = 0;  // Created/saved/removed/connected profile, if any.
  int32_t native_code = 0;
  bool has_native_code = false;
};

/// Radio adapter; all sink delivery is serialized on the supplied scheduler.
class Interface {
 public:
  class Sink {
   public:
    virtual ~Sink() = default;
    virtual void onOperationFinished(const OperationResult& result) = 0;
    virtual void onLinkChanged(const LinkState& state) = 0;
    virtual void onEnabledChanged(bool enabled) = 0;
  };
  virtual ~Interface() = default;
  virtual Error begin(Sink& sink, roo_scheduler::Scheduler& scheduler) = 0;
  virtual Support support() const = 0;
  /// kOk means admitted; completion is deferred and echoes the supplied ID.
  virtual Error setEnabled(OperationId id, bool enabled) = 0;
  virtual Error scan(OperationId id, uint16_t max_results) = 0;
  virtual Error connect(OperationId id, const ConnectionConfig& config,
                        const Credentials& credentials) = 0;
  virtual Error disconnect(OperationId id) = 0;
  /// Accepted cancellation completes the target ID with kCancelled, not a new
  /// ID.
  virtual Error cancel(OperationId target) = 0;
  /// Read during successful scan completion; copies at most capacity records.
  virtual Error readScanResults(ScanRecord* out, size_t capacity,
                                ScanRead& result) const = 0;
  /// Detaches the sink and prevents subsequent delivery, including queued
  /// events.
  virtual void shutdown() = 0;
};

struct ControllerOptions {
  ProfileId startup_profile = 0;  // Application-known key; zero disables selection.
  uint16_t max_scan_results = 100;
  uint32_t scan_timeout_ms = 15000;
  uint32_t connect_timeout_ms = 30000;
  uint32_t transition_timeout_ms = 5000;
};

/// Public backend facade; owns model/operation state, borrows its dependencies.
class Controller {
 public:
  class Listener {
   public:
    virtual ~Listener() = default;
    virtual void onScanChanged() {}
    virtual void onScanStateChanged(bool scanning) {}
    virtual void onEnabledChanged(bool enabled) {}
    virtual void onLinkChanged(const LinkState& state) {}
    virtual void onProfilesChanged() {}
    virtual void onOperationFinished(const OperationResult& result) {}
  };
  Controller(Interface& interface, Store& store,
             roo_scheduler::Scheduler& scheduler,
             ControllerOptions options = {});
  ~Controller();
  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;
  Error begin();
  void shutdown();
  void addListener(Listener& listener);
  void removeListener(Listener& listener);
  Support support() const;
  bool isEnabled() const;
  bool isScanning() const;
  ScanSnapshot scanSnapshot() const;
  LinkState linkState() const;
  Error loadProfile(ProfileId id, Profile& out) const;
  RequestResult setEnabled(bool enabled);
  RequestResult scan();
  RequestResult connect(const ConnectionConfig& config,
                        const Credentials& credential);
  RequestResult connect(ProfileId id);
  RequestResult disconnect();
  Error cancel(OperationId target);
  RequestResult saveProfile(ProfileId id,
                            const ProfileSettings& settings,
                            const CredentialUpdate& credential);
  RequestResult removeProfile(ProfileId id);
};

}  // namespace roo_wifi
```

### Behavioral Contracts Behind the Declarations

- `begin()` initializes dependencies/state and returns explicit storage/HAL
  failure. The application runs all public calls and destruction
  on the supplied scheduler context. Native adapters copy event data and deliver
  sink callbacks there in order. No public type contains Arduino/ESP-IDF objects.
- Request ID zero means rejection and produces no completion. A nonzero ID means
  owned input has been accepted. Execution and terminal notification are deferred
  until after the admission call returns, including locally executed Store work.
  Admission does not imply persistence, association, or address readiness.
- Listener callbacks may submit follow-up requests; those requests are enqueued
  for execution after notification returns. They cannot trigger recursive model
  mutation or completion. Listener registration/removal and controller destruction
  occur outside notification. Removing a listener prevents later delivery to it.
- The core has separate bounded pending slots for station transitions, scans,
  and a profile write (maximum three), rather than an unbounded request queue.
  HAL conflicts can further restrict admission. Queries and profile reads do
  not consume radio slots. A completed connection keeps only its live LinkState;
  no completed-operation history accumulates in the controller.
- `connect(profile_id)` copies metadata/credentials at admission; a subsequent
  profile edit cannot change an admitted attempt. A direct `connect(config,
  credentials)` never persists anything. Saving a profile never connects or
  changes the active association. Removing one never implicitly disconnects.
- The connection result succeeds at AddressReady, not merely Associated. Later
  link changes retain that connection ID even after its initial result. Native
  scan results are copied into the controller snapshot before the scan result
  notification; failed scans retain the prior snapshot. Truncation is explicit.
- `cancel(id)` requests cancellation of that pending operation; accepted
  cancellation yields one Cancelled result for the original ID. Cancelling a
  completed ID returns NotFound; disconnect tears down an established link.
  A queued profile write can be cancelled before it begins. Once synchronous
  storage execution starts, it runs to a resolved result rather than pretending
  that a committed write was cancelled.
- Configurable deadlines start when native work begins. Timeout initiates native
  cancellation; the command slot cannot be reused until the adapter settles the
  old work. If cancellation does not settle within `transition_timeout_ms`, emit the one
  Timeout result, mark the adapter faulted and reject new native admissions
  until recovery. Retain only the retired operation identity needed to discard
  late callbacks; a Timeout result must not permit overlapping native attempts.
  Ignore a late duplicate completion after an operation's terminal outcome.
- Explicit `shutdown()` closes admission, settles outstanding public operations
  as Cancelled, detaches the HAL sink and neutralizes queued work before returning.
  Destruction performs the same lifetime cleanup without calling application
  listeners; terminal notification is not promised after owner destruction.
- `Store` methods are synchronous and never invoke controller listeners. Failed
  reads leave output unchanged. `loadCredentials()` is privileged backend access;
  ordinary `loadProfile()` does not expose secrets.
- Keep requires an existing, complete profile; new secured profiles require
  Replace. Open profiles require Clear with no credential. Validate credential
  lengths/encoding for the selected mode. Unknown security cannot be saved as a
  usable profile or used to connect.
- `saveProfile(id, ...)` creates or replaces the caller's nonzero key. Zero is
  InvalidArgument. Incomplete means replacement input is needed; CommitUnknown
  requires rereading before assuming success. A storage failure does not imply
  the previous settings survived. Snapshot generation only versions scan data.
- Persisted enablement is a separate Store update. `setEnabled()` first performs
  the HAL transition and then writes the preference; on persistence failure its
  operation fails but the physical state may already have changed. Report actual
  state, rather than claiming a transaction spans hardware and flash. Do not
  auto-connect before enablement handling completes. Use `isEnabled()` and `onEnabledChanged()` to observe physical state
  independently of the operation's persistence result.

The omitted controller state is bounded: one scan buffer with retained capacity
`max_scan_results`, one LinkState, one slot per admitted work category, listener
registrations, and scheduler/timeout handles. The profile-write slot owns one
ProfileSettings/CredentialUpdate; the connect slot owns one resolved
ConnectionConfig/Credentials. It does not retain one full Profile per saved
network. Stores own bounded field scratch independently.

### Headless Consumer Using a Known Profile Key

For a device with one provisioned network, the application can reserve key 1
and call `start(1, settings, credential)` repeatedly to replace that configuration.
The following observer saves that known key, then connects on success.
Wi-Fi must already be enabled for the connect step; a saved profile still survives
a Disabled or connection failure result. Create one provisioning operation at a
time. `ready_` records initial address acquisition; continuous service readiness
also follows `onLinkChanged()`.

```cpp
// The owner keeps this listener alive until its operations are finished, or
// removes it before destruction. All methods run on the controller context.
class Provisioner final : public roo_wifi::Controller::Listener {
 public:
  explicit Provisioner(roo_wifi::Controller& wifi) : wifi_(wifi) {
    wifi_.addListener(*this);
  }
  ~Provisioner() override { wifi_.removeListener(*this); }

  roo_wifi::RequestResult start(roo_wifi::ProfileId key,
                                const roo_wifi::ProfileSettings& settings,
                                const roo_wifi::CredentialUpdate& credential) {
    roo_wifi::RequestResult request =
        wifi_.saveProfile(key, settings, credential);
    save_id_ = request.id;
    error_ = request.error;
    return request;
  }

  void onOperationFinished(const roo_wifi::OperationResult& result) override {
    if (result.id == save_id_) {
      save_id_ = 0;
      error_ = result.error;
      if (result.error != roo_wifi::Error::kOk) return;
      profile_id_ = result.profile_id;
      // Admission is safe in a callback: execution and completion are deferred.
      roo_wifi::RequestResult connect = wifi_.connect(profile_id_);
      connect_id_ = connect.id;
      error_ = connect.error;
    } else if (result.id == connect_id_) {
      connect_id_ = 0;
      error_ = result.error;
      ready_ = result.error == roo_wifi::Error::kOk;
    }
  }

 private:
  roo_wifi::Controller& wifi_;
  roo_wifi::OperationId save_id_ = 0, connect_id_ = 0;
  roo_wifi::ProfileId profile_id_ = 0;
  roo_wifi::Error error_ = roo_wifi::Error::kOk;
  bool ready_ = false;  // Initial address readiness, not internet reachability.
};

```

### Requirement Coverage

| Requirements | Concrete API and validation |
| --- | --- |
| 1, 3: AP data and security | ScanRecord, ConnectionConfig.security, Support.authentication_modes, and HAL security-enforcement tests. |
| 2, 6: headless provisioning and persistence | Store load/save/remove by caller-known ProfileId, CredentialUpdate, incomplete-write errors, and the consumer above. |
| 4, 5: precise asynchronous outcomes and lifetime | RequestResult, OperationResult, Interface::Sink, cancel/shutdown, deferred listeners, and lifecycle tests. |
| 7: connection settings and diagnostics | StaticIpv4, MacPolicy, ProfileSettings.auto_connect, LinkState availability flags, and native configuration tests. |
| 8, 9: portable implementations | Interface/Store virtual contracts with standard fixed-size types, SDK-free compile test and two test adapters. |
| 10: bounded costs | Fixed credential/SSID buffers, bounded snapshots/work slots, bounded per-profile field access, and resource tests. |

Unsupported features fail with Unsupported; their flags stay false. The portable
contract does not promise every platform can apply every setting.

## Implementation Plan

Authoring reference: [embedded C++ authoring](../.github/instructions/embedded-cpp-code-authoring.instructions.md).
Designs follow the [repo-wide design authoring instructions](../.github/instructions/embedded-design-doc-authoring.instructions.md).
All new targets below belong to `roo_wifi`. Run narrow tests before broader
checks; the separate UI design owns visual examples and rendering validation.

### Phase 1: Separate Portable Core and Platform Construction

Split core/platform targets and public headers. Specify portable observation,
inputs, results and dependency ownership using current useful domain types;
introduce the SDK-free fake platform and compile coverage. Document headless
construction and adapter implementation. Do not add UI types or advanced stubs.

Proposed commit message:

> Wi-Fi Backend Foundation Phase 1: separate portable core and platform adapters.
>
> Isolate native dependencies and construction, add an SDK-free host target and
> fake platform, and document the backend ownership and portability contract.

Validation: add `//:portable_api_compile_test` and build the ESP32 adapter target;
inspect the portable target's transitive dependencies for native SDK leakage.

### Phase 2: Specify and Test Ordered Native Lifecycle Handling

Document the supported Arduino/ESP-IDF event ordering, payload identity, and
connection/disconnection outcomes used by the adapter. Add a small headless
adapter harness with a deterministic native event source. Model ordinary
switching as disconnect-outcome processing followed by configuration and connect
of the next target. Preserve FIFO delivery through the scheduler and retain
SSID/BSSID/reason metadata. Determine the native outcomes for already-idle and
pre-association cancellation, and distinguish IP events from Wi-Fi attempt
completion. Use native SDK calls where the Arduino wrapper loses required data.

Acceptance: cover A-to-B switching, same-SSID retry, connection failure,
already-idle disconnect, cancellation before association, delayed scheduler
processing, IP readiness/loss, repeated scans, and owner destruction with work
queued. Every admitted request has one terminal result; delayed processing must
not attribute A's events to B or call a destroyed owner. Fake-native sequences
must respect documented ordering; test wrapper/scheduler reordering as a bug to
prevent, not as assumed native behavior. Keep retained state bounded across
repeated operations. Hardware smoke checks confirm normal switching and the
specific SDK-sensitive cancellation cases; driver restart is not a prerequisite.

Proposed commit message:

> Wi-Fi Backend Foundation Phase 2: specify ordered Wi-Fi lifecycle handling.
>
> Add a headless adapter harness and focused tests for disconnect-driven
> switching, native event identity, scheduler ordering and cancellation outcomes.

Validation: add and run `//:interface_lifecycle_test` in `roo_wifi`; document
which SDK guarantees each transition uses and record relevant hardware smoke
checks. Add recovery-path tests only when a concrete native failure requires
recovery, rather than making teardown/drain machinery the baseline design.

### Phase 3: Implement Reusable Observation and Operation Contracts

Replace the narrow HAL/event API with typed connection inputs, backend support
reporting, operation IDs, outcomes, and the ordered asynchronous cancellation
state machine. Update the ESP32 adapter and controller together. Expose full AP and
authentication metadata, connection state, and documented snapshot lifetimes.
Report actual platform support independently of any consumer's feature subset.
Migrate existing callers and tests; remove superseded paths instead of keeping
compatibility branches whose semantics are weaker than the new contract.

Do not add UI summaries/details, allowed-action flags, selection handles, a
single UI busy slot, proxy/metered/reachability policy, or speculative advanced
configuration stubs. Profile schema and persistence arrive together in Phase 4;
additional platform settings arrive with implementation in Phase 5. Connection
input in this phase includes the security requirements needed for correct
selection; unsupported hardware features report explicit errors.

Proposed commit message:

> Wi-Fi Backend Foundation Phase 3: implement reusable Wi-Fi backend contracts.
>
> Add typed connection and operation APIs, AP/security metadata and verified
> cancellation/correlation in the controller and native adapter, with migrated
> callers and headless usage documentation.

Validation: add and run `//:configuration_controller_test` in `roo_wifi`, then
its migrated controller regressions and Phase 2 lifecycle tests. Cover metadata
without UI grouping, borrowed-view lifetime, native rejection versus completion,
same-SSID security selection, cancellation, retry correlation, and teardown.
Build known downstream callers after migration; UI redesign is not required to
validate the backend. No UI presentation
model or legacy API preservation is a condition of backend tests passing.

### Phase 4: Persist Connection Profiles and Credential Intent

Implement direct known-key load/save/remove using small preference values and
the incomplete/ready/deleted status protocol above. Add caller-assigned profile
keys, startup selection, keep/replace/clear intent, and explicit incomplete/error
results. Preserve old preferences and provide explicit import from a supplied
SSID to a supplied profile key. No enumeration or full-store migration is needed.
Support persistence with the radio off and temporary unsaved connections. Add
headless usage documentation covering both single-key provisioning and repair
of an interrupted save. Do not persist application proxy/metered policies here.

Proposed commit message:

> Wi-Fi Backend Foundation Phase 4: persist known Wi-Fi configurations.
>
> Add small-value preferences, explicit credential intent and interrupted-save
> detection, with known-key provisioning and legacy credential import.

Validation: add and run `//:configuration_store_test` and affected controller
tests. Use a preferences fake exposing known-key operations only. Cover every
interrupted-write point, marker/write/read errors, create/replace/remove/retry,
Keep after incomplete save, storage exhaustion, legacy lookup, startup selection,
and radio-off use. Verify ordered durability on the native preferences backend
before claiming incomplete-update detection. Check that no stored value exceeds
64 bytes and that loading one key neither scans nor allocates a catalog.

### Phase 5: Implement Platform Connection Configuration

Extend the HAL and ESP32 implementation for supported authentication modes,
hidden networks, DHCP/static IPv4, effective link diagnostics, and supported
station MAC settings. Wire auto-connect behavior and backend validation of
Wi-Fi/IP values. Cover static-to-DHCP reset, reconnect, unsupported platform
features, and connection failure after independently successful persistence.
Report platform support honestly; neither metered/proxy consumers nor internet
reachability monitors belong in this phase or package.

Proposed commit message:

> Wi-Fi Backend Foundation Phase 5: apply platform connection configuration.
>
> Extend Wi-Fi/IP configuration, link diagnostics and reconnect behavior through
> the platform interface, with validation and asynchronous failure handling.

Validation: add and run `//:configuration_interface_test` in `roo_wifi`, run
backend regressions, and build the ESP32 implementation. Confirm static/DHCP
and supported MAC behavior on hardware before claiming those capabilities.

### Phase 6: Verify Portability, Resource Bounds, and Release

Run the same portable contracts against the non-ESP32 fake and the ESP32 harness.
Cover radio-off provisioning and temporary unsaved connections in backend docs.
Measure retained/peak memory at N=0, 20, 40, and 100 APs, with repeated cancellation
and profile loads; include K native command slots and all handoff storage. Verify
bounded retained memory and no allocation in observation paths. Build core code
with exceptions and RTTI disabled. Document platform support and breaking API
migration, then publish a new backend release with updated package metadata.
Do not choose a release number until preparing that release.

Proposed commit message:

> Wi-Fi Backend Foundation Phase 6: validate portable contracts and release migration.
>
> Add cross-adapter conformance and resource acceptance, document supported
> features and headless migration, and prepare the backend release.

Validation: run `//:interface_conformance_test`, `//:backend_resource_test`, the
portable compile target, and affected backend regressions; build the ESP32
adapter and record targeted hardware smoke checks. No second production
platform or Material 3 UI is required for backend acceptance.

## Testing Plan

- Portable header/build checks enforce absence of ESP32/Arduino/UI dependencies.
- Shared conformance tests cover scan data, supported features, operation results,
  lifetime rules, and platform-independent error handling.
- Native adapter tests cover ordered switching, cancellation, same-SSID retry,
  IP-state interpretation and teardown; hardware checks address SDK-sensitive cases.
- Store tests cover known-key access, small values, interrupted-save detection,
  error reporting, legacy lookup, credentials, startup selection, and radio-off use.
- Configuration tests cover authentication enforcement, hidden networks,
  reconnect policy, static-to-DHCP transitions and available diagnostics.
- Resource tests distinguish retained capacity from peak transient allocations
  and ensure repeated operation history does not grow without bound.

## Caveats

### Rejected Alternatives

#### Put UI Summaries and Actions in the Backend

A combined settings summary simplifies one caller but imposes its joins, identity,
and action policy on headless consumers. Keep scan, profile, and connection data
as reusable domain state; consumers derive their own presentation.

#### Make ESP32 Event Details the Portable API

Exposing Arduino/ESP-IDF structs is convenient for the first adapter but prevents
SDK-free consumers and alternative implementations. Translate at the adapter
boundary and retain native codes only as supplemental diagnostics.

#### Require Enumerable Profiles and Atomic Record Replacement

A complete catalog and atomic replacement can help products that manage many
saved networks. They are not backend requirements here. Implementing them over
preferences introduced fixed slots, dual banks, cursors, generated IDs, and
migration machinery without a demonstrated need. Keep direct small-value access
and explicit interrupted-update outcomes; applications with stronger persistence
needs can supply a different Store and their own catalog.

#### Preserve the Narrow HAL Through Compatibility Shims

Preserving void setters and SSID-only event attribution limits reliable outcomes
and configuration. Breaking changes and caller migration are authorized; update
the concrete adapter with the contract rather than keeping weaker legacy paths.

#### Restart the Driver on Every Network Switch

This adds disruption and complexity to ordinary operation. Use the ordered native
disconnect lifecycle on ESP32; reserve restart for a demonstrated fault and test
that recovery separately. Other platforms implement the same logical outcomes
using their own native guarantees.

#### Add Application Proxy, Metering, or Reachability Services

These serve application traffic rather than Wi-Fi link management. Keeping them
outside the backend avoids client-stack dependencies and costs for applications
that do not use those policies.

## Future Work

- Implement a production adapter for another radio or operating system against
  the same contracts, with its own support matrix and conformance results.
- Add enterprise credential provisioning and certificate handling as a distinct
  domain extension, preserving existing observed authentication metadata.
- Extend diagnostics to IPv6 before considering static IPv6 configuration.
