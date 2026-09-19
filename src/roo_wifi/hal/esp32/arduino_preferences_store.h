#pragma once
#include "roo_prefs.h"
#include "roo_wifi/hal/field_store.h"
namespace roo_wifi {
/// Known-key Wi-Fi profiles using small, individually committed preferences.
class ArduinoPreferencesStore : public FieldStore {
 public:
  /// Initializes the adapter and its bounded state.
  ArduinoPreferencesStore();

  /// Implements the inherited begin contract.
  Error begin() override;

  /// Implements the inherited readEnabled contract.
  Error readEnabled(bool &) const override;

  /// Implements the inherited writeEnabled contract.
  Error writeEnabled(bool) override;
  /// Imports only the supplied legacy SSID into a supplied application key.
  /// Security/settings are explicit; legacy preferences remain untouched.
  SaveResult importLegacy(ProfileId, const ProfileSettings &);
  /// Reads the legacy default SSID without importing it.
  Error readLegacyDefault(Ssid &) const;

 protected:
  Error readField(const char *, uint8_t *, size_t &) const override;
  Error writeField(const char *, const uint8_t *, size_t) override;
  Error eraseField(const char *) override;

 private:
  mutable roo_prefs::Collection collection_;
};
}  // namespace roo_wifi
