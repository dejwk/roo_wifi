#include "roo_wifi/configuration.h"

#include "roo_wifi/radio.h"

namespace roo_wifi {
namespace {
/// Packs an IPv4 address into a host integer while preserving network order.
uint32_t Address(const Ipv4Address &ip) {
  return (uint32_t(ip.bytes[0]) << 24) | (uint32_t(ip.bytes[1]) << 16) |
         (uint32_t(ip.bytes[2]) << 8) | ip.bytes[3];
}

/// Returns whether an address can identify an ordinary unicast host.
bool IsUnicast(uint32_t ip) {
  return ip != 0 && (ip >> 24) < 224 && (ip >> 24) != 127;
}

/// Returns whether every populated credential byte is an ASCII hex digit.
bool IsHex(const Credentials &c) {
  for (size_t i = 0; i < c.size; ++i) {
    uint8_t b = c.bytes[i];
    if (!(b >= '0' && b <= '9') && !(b >= 'a' && b <= 'f') &&
        !(b >= 'A' && b <= 'F')) {
      return false;
    }
  }
  return true;
}
}  // namespace

Status Validate(const ConnectionConfig &c, const Credentials &secret) {
  // Reject malformed discriminators and lengths before interpreting dependent
  // fields. This also makes invalid enum values safe to receive from storage.
  if ((secret.encoding != CredentialEncoding::kPassphrase &&
       secret.encoding != CredentialEncoding::kRawPsk &&
       secret.encoding != CredentialEncoding::kWepKey) ||
      c.ssid.size == 0 || c.ssid.size > 32 || secret.size > 64 ||
      (c.mac_policy != MacPolicy::kDevice &&
       c.mac_policy != MacPolicy::kRandomized) ||
      (c.ip_mode != IpMode::kDhcp && c.ip_mode != IpMode::kStaticIpv4)) {
    return Status::kInvalidArgument;
  }

  if (c.ip_mode == IpMode::kStaticIpv4) {
    const StaticIpv4 &s = c.static_ipv4;

    // Exclude prefixes without usable host addresses. The ESP32 adapter and
    // profile format intentionally support ordinary subnets, not host routes.
    if (s.prefix_length == 0 || s.prefix_length > 30)
      return Status::kInvalidArgument;

    uint32_t mask = 0xffffffffu << (32 - s.prefix_length);
    uint32_t ip = Address(s.address);
    uint32_t gw = Address(s.gateway);

    // The station address must be a usable host. A gateway is optional, but
    // when present it must be a distinct usable host in the same subnet. DNS
    // addresses need only be unicast; the secondary address is optional.
    if (!IsUnicast(ip) || (ip & ~mask) == 0 || (ip & ~mask) == ~mask ||
        (gw != 0 && (!IsUnicast(gw) || (gw & mask) != (ip & mask) ||
                     (gw & ~mask) == 0 || (gw & ~mask) == ~mask || gw == ip)) ||
        !IsUnicast(Address(s.dns1)) ||
        (s.has_dns2 && !IsUnicast(Address(s.dns2)))) {
      return Status::kInvalidArgument;
    }
  }

  // Open networks must not carry credential material of any encoding.
  if (c.security == AuthMode::kOpen) {
    return secret.size == 0 ? Status::kOk : Status::kInvalidArgument;
  }

  // WEP accepts its two raw key sizes or their exact hexadecimal encodings.
  if (c.security == AuthMode::kWep) {
    if (secret.encoding != CredentialEncoding::kWepKey)
      return Status::kInvalidArgument;
    return (secret.size == 5 || secret.size == 13 ||
            ((secret.size == 10 || secret.size == 26) && IsHex(secret)))
               ? Status::kOk
               : Status::kInvalidArgument;
  }

  // Enterprise, unknown, and observation-only authentication modes cannot be
  // configured by this personal-credential API.
  if (c.security != AuthMode::kWpaPersonal &&
      c.security != AuthMode::kWpa2Personal &&
      c.security != AuthMode::kWpaWpa2Personal &&
      c.security != AuthMode::kWpa3Personal &&
      c.security != AuthMode::kWpa2Wpa3Personal) {
    return Status::kUnsupported;
  }

  // A raw WPA PSK is exactly 32 bytes represented as 64 hex digits. WPA3 uses
  // SAE rather than a raw PSK, including in WPA2/WPA3 transition mode.
  if (secret.encoding == CredentialEncoding::kRawPsk) {
    if (c.security == AuthMode::kWpa3Personal ||
        c.security == AuthMode::kWpa2Wpa3Personal) {
      return Status::kUnsupported;
    }
    return secret.size == 64 && IsHex(secret) ? Status::kOk
                                              : Status::kInvalidArgument;
  }

  // Personal-mode passphrases follow the portable WPA length and printable
  // ASCII constraints accepted consistently by supported adapters.
  if (secret.encoding != CredentialEncoding::kPassphrase || secret.size < 8 ||
      secret.size > 63) {
    return Status::kInvalidArgument;
  }
  for (size_t i = 0; i < secret.size; ++i) {
    if (secret.bytes[i] < 32 || secret.bytes[i] > 126)
      return Status::kInvalidArgument;
  }
  return Status::kOk;
}

Status ValidateSupport(const ConnectionConfig &c, const Support &s) {
  // Authentication uses a bit indexed by the portable enum. Configuration
  // features are checked independently so adapters never silently ignore a
  // requested hidden-network, static-address, or randomized-MAC policy.
  if (static_cast<unsigned>(c.security) >= 32 ||
      (s.authentication_modes & (1u << static_cast<unsigned>(c.security))) ==
          0 ||
      (c.hidden && !s.hidden_networks) ||
      (c.ip_mode == IpMode::kStaticIpv4 && !s.static_ipv4) ||
      (c.mac_policy == MacPolicy::kRandomized && !s.randomized_mac)) {
    return Status::kUnsupported;
  }
  return Status::kOk;
}
}  // namespace roo_wifi
