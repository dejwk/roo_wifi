#include "roo_wifi/types.h"

namespace roo_wifi {
namespace {
uint32_t Address(const Ipv4Address &ip) {
  return (uint32_t(ip.bytes[0]) << 24) | (uint32_t(ip.bytes[1]) << 16) |
         (uint32_t(ip.bytes[2]) << 8) | ip.bytes[3];
}

bool Unicast(uint32_t ip) {
  return ip != 0 && (ip >> 24) < 224 && (ip >> 24) != 127;
}

bool Hex(const Credentials &c) {
  for (size_t i = 0; i < c.size; ++i) {
    uint8_t b = c.bytes[i];
    if (!(b >= '0' && b <= '9') && !(b >= 'a' && b <= 'f') &&
        !(b >= 'A' && b <= 'F'))
      return false;
  }
  return true;
}
}  // namespace

Status Validate(const ConnectionConfig &c, const Credentials &secret) {
  if ((secret.encoding != CredentialEncoding::kPassphrase &&
       secret.encoding != CredentialEncoding::kRawPsk &&
       secret.encoding != CredentialEncoding::kWepKey) ||
      c.ssid.size == 0 || c.ssid.size > 32 || secret.size > 64 ||
      (c.mac_policy != MacPolicy::kDevice &&
       c.mac_policy != MacPolicy::kRandomized) ||
      (c.ip_mode != IpMode::kDhcp && c.ip_mode != IpMode::kStaticIpv4))
    return Status::kInvalidArgument;
  if (c.ip_mode == IpMode::kStaticIpv4) {
    const StaticIpv4 &s = c.static_ipv4;
    if (s.prefix_length == 0 || s.prefix_length > 30)
      return Status::kInvalidArgument;
    uint32_t mask = 0xffffffffu << (32 - s.prefix_length);
    uint32_t ip = Address(s.address), gw = Address(s.gateway);
    if (!Unicast(ip) || (ip & ~mask) == 0 || (ip & ~mask) == ~mask ||
        (gw && (!Unicast(gw) || (gw & mask) != (ip & mask) ||
                (gw & ~mask) == 0 || (gw & ~mask) == ~mask || gw == ip)) ||
        !Unicast(Address(s.dns1)) || (s.has_dns2 && !Unicast(Address(s.dns2))))
      return Status::kInvalidArgument;
  }
  if (c.security == AuthMode::kOpen)
    return secret.size == 0 ? Status::kOk : Status::kInvalidArgument;
  if (c.security == AuthMode::kWep) {
    if (secret.encoding != CredentialEncoding::kWepKey)
      return Status::kInvalidArgument;
    return (secret.size == 5 || secret.size == 13 ||
            ((secret.size == 10 || secret.size == 26) && Hex(secret)))
               ? Status::kOk
               : Status::kInvalidArgument;
  }
  if (c.security != AuthMode::kWpaPersonal &&
      c.security != AuthMode::kWpa2Personal &&
      c.security != AuthMode::kWpaWpa2Personal &&
      c.security != AuthMode::kWpa3Personal &&
      c.security != AuthMode::kWpa2Wpa3Personal)
    return Status::kUnsupported;
  if (secret.encoding == CredentialEncoding::kRawPsk) {
    if (c.security == AuthMode::kWpa3Personal ||
        c.security == AuthMode::kWpa2Wpa3Personal)
      return Status::kUnsupported;
    return secret.size == 64 && Hex(secret) ? Status::kOk
                                            : Status::kInvalidArgument;
  }
  if (secret.encoding != CredentialEncoding::kPassphrase || secret.size < 8 ||
      secret.size > 63)
    return Status::kInvalidArgument;
  for (size_t i = 0; i < secret.size; ++i)
    if (secret.bytes[i] < 32 || secret.bytes[i] > 126)
      return Status::kInvalidArgument;
  return Status::kOk;
}

Status ValidateSupport(const ConnectionConfig &c, const Support &s) {
  if (static_cast<unsigned>(c.security) >= 32 ||
      !(s.authentication_modes & (1u << static_cast<unsigned>(c.security))) ||
      (c.hidden && !s.hidden_networks) ||
      (c.ip_mode == IpMode::kStaticIpv4 && !s.static_ipv4) ||
      (c.mac_policy == MacPolicy::kRandomized && !s.randomized_mac))
    return Status::kUnsupported;
  return Status::kOk;
}
}  // namespace roo_wifi
