#pragma once
#include "roo_wifi/hal/store.h"

namespace roo_wifi {
/// Implements profile persistence using versioned settings and secret blobs.
/// Implementations must serialize access and make successful field writes
/// durable. Field-key iteration discovers profiles without a separate catalog.
class FieldStore : public Store {
 public:
  /// Loads profile metadata and credential presence from persisted blobs.
  /// @param id Nonzero profile key to load.
  /// @param out Receives metadata on success and is unchanged on failure.
  Status loadProfile(ProfileId id, Profile &out) const override;

  /// Loads credentials from a profile's separate secret blob.
  /// @param id Nonzero profile key to load.
  /// @param out Receives credentials on success and is unchanged on failure.
  Status loadCredentials(ProfileId id, Credentials &out) const override;

  /// Serializes settings and applies the requested secret update.
  /// @param id Nonzero profile key to save.
  /// @param settings Non-secret settings to persist.
  /// @param credential Credential action and replacement material.
  Status saveProfile(ProfileId id, const ProfileSettings &settings,
                     const CredentialUpdate &credential) override;

  /// Removes a profile's settings and secret blobs.
  /// @param id Nonzero profile key to remove.
  Status removeProfile(ProfileId id) override;

  /// Loads the persisted last-successful profile ID.
  Status readLastProfile(ProfileId &out) const override;

  /// Persists or clears the last-successful profile ID.
  Status writeLastProfile(ProfileId id) override;

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
  Status enumerateProfiles(ProfileVisitor visitor,
                           void *context) const override;

  /// Reads and validates the settings and corresponding secret.
  Status read(ProfileId id, ProfileSettings &settings,
              Credentials &secret) const;
};
}  // namespace roo_wifi
