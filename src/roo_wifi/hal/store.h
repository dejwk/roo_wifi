#pragma once

#include <inttypes.h>

#include <string>
#include <vector>

#include "roo_backport.h"
#include "roo_backport/string_view.h"

namespace roo_wifi {

/// Abstraction for persistently storing Wi-Fi controller data.
class Store {
 public:
  /// Returns whether the Wi-Fi interface is enabled.
  virtual bool getIsInterfaceEnabled() = 0;
  /// Sets whether the Wi-Fi interface is enabled.
  virtual void setIsInterfaceEnabled(bool enabled) = 0;
  /// Returns the default SSID, if any.
  virtual std::string getDefaultSSID() = 0;
  /// Sets the default SSID.
  virtual void setDefaultSSID(const std::string& ssid) = 0;
  /// Clears the default SSID.
  virtual void clearDefaultSSID() = 0;
  /// Retrieves a stored password for an SSID.
  virtual bool getPassword(const std::string& ssid, std::string& password) = 0;
  /// Stores a password for an SSID.
  virtual void setPassword(const std::string& ssid,
                           roo::string_view password) = 0;
  /// Clears a stored password for an SSID.
  virtual void clearPassword(const std::string& ssid) = 0;
};

}  // namespace roo_wifi
