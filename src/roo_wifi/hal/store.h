/// @file
/// @brief Persistent Wi-Fi configuration and credential storage contract.

#pragma once

#include <inttypes.h>

#include <string>
#include <vector>

#include "roo_backport.h"
#include "roo_backport/string_view.h"

namespace roo_wifi {

/// @brief Persistent storage abstraction used by `Controller`.
/// @ingroup roo_wifi
///
/// Implementations store interface enablement, the default SSID, and passwords
/// associated with individual SSIDs.
class Store {
 public:
  /// @brief Virtual destructor.
  virtual ~Store() = default;

  /// @brief Returns the persisted Wi-Fi enabled state.
  virtual bool getIsInterfaceEnabled() = 0;
  /// @brief Persists the Wi-Fi enabled state.
  /// @param enabled State to persist.
  virtual void setIsInterfaceEnabled(bool enabled) = 0;
  /// @brief Returns the default SSID, or an empty string when unset.
  virtual std::string getDefaultSSID() = 0;
  /// @brief Persists the default SSID.
  /// @param ssid SSID to make the default connection target.
  virtual void setDefaultSSID(const std::string& ssid) = 0;
  /// @brief Clears the persisted default SSID.
  virtual void clearDefaultSSID() = 0;
  /// @brief Retrieves a password associated with an SSID.
  /// @param ssid SSID whose password should be retrieved.
  /// @param password Destination populated on success.
  /// @return `true` when a password entry exists, including an empty password.
  virtual bool getPassword(const std::string& ssid, std::string& password) = 0;
  /// @brief Stores or replaces the password associated with an SSID.
  /// @param ssid SSID whose profile should be updated.
  /// @param password Password to store; may be empty for an open network.
  virtual void setPassword(const std::string& ssid,
                           roo::string_view password) = 0;
  /// @brief Removes the password entry associated with an SSID.
  /// @param ssid SSID whose password should be removed.
  virtual void clearPassword(const std::string& ssid) = 0;
};

}  // namespace roo_wifi
