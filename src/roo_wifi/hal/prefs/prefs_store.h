#pragma once
#include "roo_prefs.h"
#include "roo_wifi/hal/field_store.h"

namespace roo_wifi {

/// Stores known-key Wi-Fi profiles through the roo_prefs backend.
class PrefsStore : public FieldStore {
 public:
  /// Creates a roo_prefs-backed profile store.
  PrefsStore();

  /// Opens the roo_prefs collection for subsequent operations.
  Status begin() override;

  /// Reads persisted radio enablement from roo_prefs.
  /// @param enabled Receives the stored value on success.
  Status readEnabled(bool &enabled) const override;

  /// Persists radio enablement through roo_prefs.
  /// @param enabled Value to persist.
  Status writeEnabled(bool enabled) override;

  /// Imports one legacy SSID/profile into an application profile key.
  /// Security/settings are explicit; legacy preferences remain untouched.
  /// @param id Nonzero destination profile key.
  /// @param settings Settings paired with the legacy credentials.
  Status importLegacy(ProfileId id, const ProfileSettings &settings);

  /// Reads the legacy default SSID without importing it.
  /// @param out Receives the SSID on success and is unchanged on failure.
  Status readLegacyDefault(Ssid &out) const;

 protected:
  Status readField(const char *, uint8_t *, size_t &) const override;
  Status writeField(const char *, const uint8_t *, size_t) override;
  Status eraseField(const char *) override;

 private:
  mutable roo_prefs::Collection collection_;
};

}  // namespace roo_wifi
