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
  /// Pairwise cipher reported by the access point when metadata is available.
  CipherType pairwise_cipher = CipherType::kUnknown;

  /// Group cipher reported by the access point when metadata is available.
  CipherType group_cipher = CipherType::kUnknown;

  /// Whether cipher and PHY fields were supplied by the native radio.
  bool has_radio_metadata = false;

  /// Whether the access point advertises 802.11b support.
  bool use_11b = false;

  /// Whether the access point advertises 802.11g support.
  bool use_11g = false;

  /// Whether the access point advertises 802.11n support.
  bool use_11n = false;

  /// Whether the access point advertises WPS support.
  bool supports_wps = false;

  /// Network name observed during the scan.
  Ssid ssid;

  /// Access-point MAC address observed during the scan.
  MacAddress bssid;

  /// Authentication mode observed during the scan.
  AuthMode security = AuthMode::kUnknown;

  /// Received signal strength in dBm.
  int8_t rssi_dbm = -128;

  /// Primary radio channel.
  uint16_t channel = 0;
};

/// Describes the portable features supported by a selected radio adapter.
struct Support {
  /// Bitset of supported @p AuthMode values, indexed by their numeric value.
  uint32_t authentication_modes = 0;

  /// Whether hidden-network connections are supported.
  bool hidden_networks = false;

  /// Whether static IPv4 configuration is supported.
  bool static_ipv4 = false;

  /// Whether randomized station MAC addresses are supported.
  bool randomized_mac = false;

  /// Whether scanning while connected is supported.
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

  /// Network name last reported by the access point.
  Ssid ssid;

  /// Access-point MAC address last reported by the radio.
  MacAddress bssid;

  /// Station MAC address last reported by the radio.
  MacAddress station_mac;

  /// Authentication mode last reported by the access point.
  AuthMode security = AuthMode::kUnknown;

  /// Received signal strength in dBm.
  int8_t rssi_dbm = -128;

  /// Primary radio channel.
  uint16_t channel = 0;

  /// IPv4 station address observed after address readiness.
  Ipv4Address address;

  /// IPv4 gateway observed after address readiness.
  Ipv4Address gateway;

  /// Primary DNS server observed after address readiness.
  Ipv4Address dns1;

  /// Secondary DNS server observed after address readiness.
  Ipv4Address dns2;

  /// Whether radio metadata fields are valid.
  bool has_radio_info = false;

  /// Whether @p station_mac is valid.
  bool has_station_mac = false;

  /// Whether the IPv4 address and gateway are valid.
  bool has_ipv4 = false;

  /// Whether @p dns1 is valid.
  bool has_dns1 = false;

  /// Whether @p dns2 is valid.
  bool has_dns2 = false;

  /// Terminal disconnect reason.
  Status reason = Status::kOk;

  /// Platform diagnostic code when @p has_native_code is true.
  int32_t native_code = 0;

  /// Whether @p native_code contains a platform diagnostic.
  bool has_native_code = false;
};

}  // namespace roo_wifi
