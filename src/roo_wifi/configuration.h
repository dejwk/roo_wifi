#pragma once
#include <stddef.h>
#include <stdint.h>

#include "roo_wifi/network.h"
#include "roo_wifi/status.h"

namespace roo_wifi {

struct Support;

/// Selects DHCP or caller-supplied static IPv4 configuration.
enum class IpMode : uint8_t { kDhcp, kStaticIpv4 };

/// Selects the device MAC address or a per-connection randomized address.
enum class MacPolicy : uint8_t { kDevice, kRandomized };

/// Defines static IPv4 settings used when @p IpMode::kStaticIpv4 is selected.
struct StaticIpv4 {
  /// IPv4 address to assign to the station.
  Ipv4Address address;

  /// IPv4 address of the default gateway.
  Ipv4Address gateway;

  /// IPv4 address of the primary DNS server.
  Ipv4Address dns1;

  /// IPv4 address of the secondary DNS server, used when @p has_dns2 is true.
  Ipv4Address dns2;

  /// CIDR prefix length for @p address, from 1 through 30.
  uint8_t prefix_length = 24;

  /// Whether @p dns2 is present and must be configured.
  bool has_dns2 = false;
};

/// Defines the network, addressing, and MAC settings for a connection.
/// Contains no secrets; security is an enforced requirement.
struct ConnectionConfig {
  /// Network name to join.
  Ssid ssid;

  /// Allowed authentication modes. Mixed modes also allow either constituent
  /// mode when a router changes configuration; strict modes remain strict.
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

/// Tests whether an advertised or negotiated mode is allowed by a policy.
/// Mixed policies allow either constituent mode as well as the mixed mode.
/// Unknown modes are never compatible.
bool SecurityAllows(AuthMode policy, AuthMode mode);

/// Validates portable connection settings and credential encoding.
/// @param config Network and IP settings to validate.
/// @param credentials Credential material and encoding to validate.
Status Validate(const ConnectionConfig &config, const Credentials &credentials);

/// Checks that configuration features are available from the selected radio.
/// @param config Network and IP settings to check.
/// @param support Supported radio features.
Status ValidateSupport(const ConnectionConfig &config, const Support &support);

}  // namespace roo_wifi
