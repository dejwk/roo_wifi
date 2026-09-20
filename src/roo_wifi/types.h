#pragma once
#include <stddef.h>
#include <stdint.h>

namespace roo_wifi {
/// Application-assigned key for a saved profile; zero means no profile.
using ProfileId = uint32_t;

/// Nonzero operation identity, never reused during one controller lifetime.
using OperationId = uint64_t;

/// Reports admission, completion, validation, and persistence outcomes.
enum class Status : uint8_t {
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

/// Holds an SSID of up to 32 bytes without requiring a null terminator.
struct Ssid {
  /// Raw SSID bytes; only the first @p size bytes are meaningful.
  uint8_t bytes[32] = {};

  /// Number of meaningful bytes in @p bytes.
  uint8_t size = 0;
};

/// Holds a six-byte IEEE 802 MAC address in network byte order.
struct MacAddress {
  uint8_t bytes[6] = {};
};

/// Holds a four-byte IPv4 address in dotted-quad byte order.
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
/// Selects DHCP or caller-supplied static IPv4 configuration.
enum class IpMode : uint8_t { kDhcp, kStaticIpv4 };

/// Selects the device MAC address or a per-connection randomized address.
enum class MacPolicy : uint8_t { kDevice, kRandomized };

/// Defines static IPv4 settings used when @p IpMode::kStaticIpv4 is selected.
struct StaticIpv4 {
  /// IPv4 address, gateway, primary DNS server, and optional secondary DNS.
  Ipv4Address address;
  Ipv4Address gateway;
  Ipv4Address dns1;
  Ipv4Address dns2;

  /// CIDR prefix length for @p address, from 1 through 30.
  uint8_t prefix_length = 24;

  /// Whether @p dns2 is present and must be configured.
  bool has_dns2 = false;
};

/// Connection parameters without secrets; security is an enforced requirement.
struct ConnectionConfig {
  /// Network name to join.
  Ssid ssid;

  /// Authentication requirement for the selected access point.
  AuthMode security = AuthMode::kUnknown;

  /// Whether the scan and connection should include a hidden network.
  bool hidden = false;

  /// Selects DHCP or the @p static_ipv4 settings.
  IpMode ip_mode = IpMode::kDhcp;

  /// Static address settings when @p ip_mode is @p IpMode::kStaticIpv4.
  StaticIpv4 static_ipv4;

  /// Selects the station MAC address policy for this connection.
  MacPolicy mac_policy = MacPolicy::kDevice;
};

/// Identifies how credential bytes are encoded for the chosen authentication.
enum class CredentialEncoding : uint8_t { kPassphrase, kRawPsk, kWepKey };

/// Holds one connection attempt's secret credential material.
struct Credentials {
  /// Encoding of the bytes stored in @p bytes.
  CredentialEncoding encoding = CredentialEncoding::kPassphrase;

  /// Raw credential bytes; only the first @p size bytes are meaningful.
  uint8_t bytes[64] = {};

  /// Number of meaningful bytes in @p bytes.
  uint8_t size = 0;
};

/// Selects whether a profile keeps, replaces, or clears its credentials.
enum class CredentialIntent : uint8_t { kKeep, kReplace, kClear };

/// Describes the credential change applied when saving a profile.
struct CredentialUpdate {
  /// Requested credential action.
  CredentialIntent intent = CredentialIntent::kKeep;

  /// Replacement credential used only when @p intent is @p kReplace.
  Credentials replacement;
};

/// Contains persisted non-secret settings for one saved profile.
struct ProfileSettings {
  /// Connection settings to persist.
  ConnectionConfig connection;

  /// Whether the controller may reconnect this profile automatically.
  bool auto_connect = true;
};

/// Describes one saved profile without exposing its credentials.
struct Profile {
  /// Application-assigned nonzero profile key.
  ProfileId id = 0;

  /// Persisted non-secret settings.
  ProfileSettings settings;

  /// Whether credentials exist and can be loaded through privileged access.
  bool has_credentials = false;
};

/// Returns the status and key of a profile save or import operation.
struct SaveResult {
  /// Final persistence outcome.
  Status error = Status::kOk;

  /// Profile key supplied to the save operation.
  ProfileId id = 0;
};

/// Observed cipher metadata, independent of native SDK enum values.
enum class CipherType : uint8_t {
  kUnknown,
  kNone,
  kWep40,
  kWep104,
  kTkip,
  kCcmp,
  kTkipCcmp,
  kAesCmac128,
  kSms4,
  kGcmp,
  kGcmp256,
  kAesGmac128,
  kAesGmac256
};

/// One AP; equal SSIDs do not imply equal security or AP identity.
struct ScanRecord {
  /// Ciphers reported by the access point; meaningful only with radio metadata.
  CipherType pairwise_cipher = CipherType::kUnknown;
  CipherType group_cipher = CipherType::kUnknown;

  /// Whether cipher and PHY fields were supplied by the native radio.
  bool has_radio_metadata = false;

  /// Advertised 802.11 PHY capabilities and WPS support.
  bool use_11b = false;
  bool use_11g = false;
  bool use_11n = false;
  bool supports_wps = false;

  /// Network identity and authentication observed during the scan.
  Ssid ssid;
  MacAddress bssid;
  AuthMode security = AuthMode::kUnknown;
  /// Received signal strength in dBm and the primary radio channel.
  int8_t rssi_dbm = -128;
  uint16_t channel = 0;
};

/// Borrows records from the controller's latest successful scan publication.
/// The records remain valid until the next successful publication or shutdown.
struct ScanSnapshot {
  /// Monotonically increasing publication generation.
  uint64_t generation = 0;

  /// Borrowed contiguous scan records.
  const ScanRecord *records = nullptr;

  /// Number of records available through @p records.
  size_t count = 0;

  /// Whether the radio found more records than the configured capacity.
  bool truncated = false;
};

/// Reports the number of scan records copied by a radio adapter.
struct ScanRead {
  /// Number of records copied into caller storage.
  size_t count = 0;

  /// Whether records were omitted because storage was bounded.
  bool truncated = false;
};

/// Describes the portable features supported by a selected radio adapter.
struct Support {
  /// Bitset of supported @p AuthMode values, indexed by their numeric value.
  uint32_t authentication_modes = 0;

  /// Features accepted by the adapter in addition to authentication modes.
  bool hidden_networks = false;
  bool static_ipv4 = false;
  bool randomized_mac = false;
  bool scan_while_connected = false;
};

/// Identifies the current stage of a station connection lifecycle.
enum class LinkPhase : uint8_t {
  kIdle,
  kConnecting,
  kAssociated,
  kAddressReady
};

/// Reports observed station identity, connectivity, and network diagnostics.
struct LinkState {
  /// Connect operation that established this link, or zero while idle.
  OperationId connection_id = 0;

  /// Current connection lifecycle stage.
  LinkPhase phase = LinkPhase::kIdle;

  /// Identity and radio properties last reported by the access point.
  Ssid ssid;
  MacAddress bssid;
  MacAddress station_mac;
  AuthMode security = AuthMode::kUnknown;
  int8_t rssi_dbm = -128;
  uint16_t channel = 0;
  /// IPv4 configuration observed after address readiness.
  Ipv4Address address;
  Ipv4Address gateway;
  Ipv4Address dns1;
  Ipv4Address dns2;

  /// Indicates which optional radio, MAC, address, and DNS fields are valid.
  bool has_radio_info = false;
  bool has_station_mac = false;
  bool has_ipv4 = false;
  bool has_dns1 = false;
  bool has_dns2 = false;

  /// Terminal disconnect reason, with an optional native diagnostic code.
  Status reason = Status::kOk;
  int32_t native_code = 0;
  bool has_native_code = false;
};

/// Identifies the controller action associated with an operation result.
enum class OperationKind : uint8_t {
  kEnable,
  kScan,
  kConnect,
  kDisconnect,
  kSave,
  kRemove
};

/// Returns immediate operation admission status and its deferred-completion ID.
struct RequestResult {
  /// Nonzero admitted operation ID; zero means no completion callback follows.
  OperationId id = 0;

  /// Immediate admission or validation result.
  Status error = Status::kOk;
};

/// Reports the terminal result of one admitted controller operation.
struct OperationResult {
  /// ID allocated when the controller admitted this operation.
  OperationId id = 0;

  /// Action that completed.
  OperationKind kind = OperationKind::kScan;
  /// Successful completion or one terminal failure/cancellation.
  Status error = Status::kOk;

  /// Created, saved, removed, or connected profile key when applicable.
  ProfileId profile_id = 0;
  /// Platform diagnostic code when @p has_native_code is true.
  int32_t native_code = 0;

  /// Whether @p native_code contains a platform diagnostic.
  bool has_native_code = false;
};

/// Configures controller capacity, timeouts, and optional startup connection.
struct ControllerOptions {
  /// Profile to connect after startup; zero disables automatic selection.
  ProfileId startup_profile = 0;

  /// Maximum records retained from one scan.
  uint16_t max_scan_results = 100;

  /// Deadlines in milliseconds for scan, connection, and state transitions.
  uint32_t scan_timeout_ms = 15000;
  uint32_t connect_timeout_ms = 30000;
  uint32_t transition_timeout_ms = 5000;
};

/// Validates portable connection settings and credential encoding.
/// @param config Network and IP settings to validate.
/// @param credentials Credential material and encoding to validate.
Status Validate(const ConnectionConfig &config, const Credentials &credentials);

/// Checks that configuration features are available from the selected radio.
/// @param config Network and IP settings to check.
/// @param support Supported radio features.
Status ValidateSupport(const ConnectionConfig &config, const Support &support);
}  // namespace roo_wifi
