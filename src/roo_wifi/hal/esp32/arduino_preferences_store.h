#pragma once

#include "roo_prefs.h"
#include "roo_wifi/hal/store.h"

namespace roo_wifi {

/// Store implementation backed by roo_prefs (ESP32 Arduino preferences).
class ArduinoPreferencesStore : public Store {
 public:
  ArduinoPreferencesStore();

  /// Initializes the preferences store (no-op placeholder).
  void begin() {}

  /// Returns whether the Wi-Fi interface is enabled.
  bool getIsInterfaceEnabled() override;

  /// Sets whether the Wi-Fi interface is enabled.
  void setIsInterfaceEnabled(bool enabled) override;

  /// Returns the default SSID, if any.
  std::string getDefaultSSID() override;

  /// Clears the default SSID.
  void clearDefaultSSID() override;

  /// Sets the default SSID.
  void setDefaultSSID(const std::string& ssid) override;

  /// Retrieves a stored password for an SSID.
  bool getPassword(const std::string& ssid, std::string& password) override;

  /// Stores a password for an SSID.
  void setPassword(const std::string& ssid, roo::string_view password) override;

  /// Clears a stored password for an SSID.
  void clearPassword(const std::string& ssid) override;

 private:
  roo_prefs::Collection collection_;
  roo_prefs::Bool is_interface_enabled_;
  roo_prefs::String default_ssid_;
};

}  // namespace roo_wifi
