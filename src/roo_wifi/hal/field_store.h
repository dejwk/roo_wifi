#pragma once
#include "roo_wifi/hal/store.h"

namespace roo_wifi {
/// Implements profile persistence using versioned settings and secret blobs.
/// Implementations must serialize access and make successful field writes
/// durable. Field-key iteration discovers profiles without a separate catalog.
class FieldStore : public Store {
 public:
  /// Loads profile metadata and credential presence from persisted blobs.
  /// @param ssid Exact SSID of the saved configuration.
  /// @param out Receives metadata on success and is unchanged on failure.
  Status loadProfile(const Ssid &ssid, Profile &out) const override;

  /// Loads credentials from a profile's separate secret blob.
  /// @param ssid Exact SSID of the saved configuration.
  /// @param out Receives credentials on success and is unchanged on failure.
  Status loadCredentials(const Ssid &ssid, Credentials &out) const override;

  /// Serializes settings and applies the requested secret update.
  /// @param settings Non-secret settings to persist.
  /// @param credential Credential action and replacement material.
  Status saveProfile(const ProfileSettings &settings,
                     const CredentialUpdate &credential) override;

  /// Removes a profile's settings and secret blobs.
  /// @param ssid Exact SSID of the saved configuration.
  Status removeProfile(const Ssid &ssid) override;

  /// Loads the persisted last-successful profile SSID.
  Status readLastProfile(Ssid &out) const override;

  /// Persists or clears the last-successful profile SSID.
  Status writeLastProfile(const Ssid &ssid) override;

 protected:
  /// Callback used internally to enumerate persisted field keys.
  using FieldVisitor = bool (*)(void *context, const char *key, size_t size);

  /// Reads one persisted field.
  /// @param key Field key to read.
  /// @param out Buffer of at least @p size bytes.
  /// @param size On input buffer capacity; on success actual byte count.
  virtual Status readField(const char *key, uint8_t *out,
                           size_t &size) const = 0;

  /// Durably writes one field before reporting success.
  /// @param key Field key to write.
  /// @param data Bytes to persist.
  /// @param size Number of bytes in @p data.
  virtual Status writeField(const char *key, const uint8_t *data,
                            size_t size) = 0;

  /// Durably removes one field; an absent field is successful.
  /// @param key Field key to remove.
  virtual Status eraseField(const char *key) = 0;

  /// Calls the visitor for each persisted field key in unspecified order.
  /// Returns kStopped when the visitor returns false.
  virtual Status enumerateFields(FieldVisitor visitor, void *context) const = 0;

 private:
  /// Writes changed bytes and rereads failures to resolve the commit outcome.
  /// @param capacity Maximum size of the existing value, bounded by the largest
  /// supported blob; a mismatch still permits replacement after identity
  /// checks.
  Status writeVerified(const char *key, const uint8_t *expected,
                       size_t expected_size, size_t capacity);

  /// Decodes and visits one canonical profile key, checking its SSID hash.
  Status visitProfile(const char *key, size_t size, ProfileVisitor visitor,
                      void *context) const;

  /// Verifies ownership of all existing blobs before changing either.
  Status checkIdentity(const Ssid &ssid) const;

  /// Visits decoded SSIDs and propagates enumeration or corruption failures.
  Status enumerateProfiles(ProfileVisitor visitor,
                           void *context) const override;

  /// Reads and validates the settings and corresponding secret.
  Status read(const Ssid &ssid, ProfileSettings &settings,
              Credentials &secret) const;
};
}  // namespace roo_wifi
