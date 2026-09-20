#pragma once
#include <stdint.h>

#include "roo_wifi/network.h"
#include "roo_wifi/operation.h"
#include "roo_wifi/status.h"

namespace roo_wifi {

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

}  // namespace roo_wifi
