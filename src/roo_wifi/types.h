#pragma once
#include <stddef.h>
#include <stdint.h>
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
  CipherType pairwise_cipher = CipherType::kUnknown;
  CipherType group_cipher = CipherType::kUnknown;
  bool has_radio_metadata = false;
  bool use_11b = false, use_11g = false, use_11n = false, supports_wps = false;
  Ssid ssid;
  MacAddress bssid;
  AuthMode security = AuthMode::kUnknown;
  int8_t rssi_dbm = -128;
  uint16_t channel = 0;
};
/// Borrowed until next successful scan publication or controller shutdown.
struct ScanSnapshot {
  uint64_t generation = 0;
  const ScanRecord *records = nullptr;
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
  int32_t native_code = 0;
  bool has_native_code = false;
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

struct ControllerOptions {
  ProfileId startup_profile =
      0;  // Application-known key; zero disables selection.
  uint16_t max_scan_results = 100;
  uint32_t scan_timeout_ms = 15000;
  uint32_t connect_timeout_ms = 30000;
  uint32_t transition_timeout_ms = 5000;
};

/// Validates portable settings and credential encoding.
Error Validate(const ConnectionConfig &, const Credentials &);
/// Checks requested features against the selected radio.
Error ValidateSupport(const ConnectionConfig &, const Support &);
}  // namespace roo_wifi
