#pragma once
#include <stdint.h>

namespace roo_wifi {

/// Holds an SSID of up to 32 bytes without requiring a null terminator.
struct Ssid {
  /// Raw SSID bytes; only the first @p size bytes are meaningful.
  uint8_t bytes[32] = {};

  /// Number of meaningful bytes in @p bytes.
  uint8_t size = 0;
};

/// Holds a six-byte IEEE 802 MAC address in network byte order.
struct MacAddress {
  /// Six address octets in network byte order.
  uint8_t bytes[6] = {};
};

/// Holds a four-byte IPv4 address in dotted-quad byte order.
struct Ipv4Address {
  /// Four address octets in dotted-quad order.
  uint8_t bytes[4] = {};
};

/// Identifies a network's authentication mode.
/// Values are library-defined and independent of native SDK enum numbering.
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

}  // namespace roo_wifi
