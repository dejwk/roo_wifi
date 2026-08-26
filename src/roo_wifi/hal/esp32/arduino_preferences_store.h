/// @file
/// @brief ESP32 preferences-backed Wi-Fi store.

#pragma once

#include "roo_prefs.h"
#include "roo_wifi/hal/store.h"

namespace roo_wifi {

/// @brief Stores Wi-Fi configuration in ESP32 non-volatile preferences.
/// @ingroup roo_wifi
///
/// Password keys are derived from SSIDs so multiple network profiles can be
/// stored without exposing SSIDs as preferences keys.
class ArduinoPreferencesStore : public Store {
 public:
  /// @brief Constructs a store bound to the Roo Wi-Fi preference collection.
  ArduinoPreferencesStore();

  /// @brief Initializes the preferences store.
  ///
  /// Present for a uniform adapter lifecycle; currently performs no work.
  void begin() {}

  /// @copydoc Store::getIsInterfaceEnabled()
  bool getIsInterfaceEnabled() override;

  /// @copydoc Store::setIsInterfaceEnabled()
  void setIsInterfaceEnabled(bool enabled) override;

  /// @copydoc Store::getDefaultSSID()
  std::string getDefaultSSID() override;

  /// @copydoc Store::clearDefaultSSID()
  void clearDefaultSSID() override;

  /// @copydoc Store::setDefaultSSID()
  void setDefaultSSID(const std::string& ssid) override;

  /// @copydoc Store::getPassword()
  bool getPassword(const std::string& ssid, std::string& password) override;

  /// @copydoc Store::setPassword()
  void setPassword(const std::string& ssid, roo::string_view password) override;

  /// @copydoc Store::clearPassword()
  void clearPassword(const std::string& ssid) override;

 private:
  roo_prefs::Collection collection_;
  roo_prefs::Bool is_interface_enabled_;
  roo_prefs::String default_ssid_;
};

}  // namespace roo_wifi
