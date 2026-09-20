#pragma once
#include "roo_wifi/hal/store.h"

namespace roo_wifi {
/// Implements profile persistence using ordered, durable small-value fields.
/// Implementations must serialize access; successful writes survive restart in
/// call order. No key iteration, catalog, or atomic multi-key update is
/// required.
class FieldStore : public Store {
 public:
  /// Loads profile metadata from its committed fields.
  /// @param id Nonzero profile key to load.
  /// @param out Receives metadata on success and is unchanged on failure.
  Status loadProfile(ProfileId id, Profile &out) const override;
  /// Loads credentials from a profile's committed fields.
  /// @param id Nonzero profile key to load.
  /// @param out Receives credentials on success and is unchanged on failure.
  Status loadCredentials(ProfileId id, Credentials &out) const override;
  /// Saves a profile with incomplete/ready commit markers.
  /// @param id Nonzero profile key to save.
  /// @param settings Non-secret settings to persist.
  /// @param credential Credential action and replacement material.
  Status saveProfile(ProfileId id, const ProfileSettings &settings,
                     const CredentialUpdate &credential) override;
  /// Removes a profile by marking it deleted before field cleanup.
  /// @param id Nonzero profile key to remove.
  Status removeProfile(ProfileId id) override;

 protected:
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

 private:
  Status readStatus(ProfileId id) const;
  Status read(ProfileId id, ProfileSettings &settings,
              Credentials &secret) const;
};
}  // namespace roo_wifi
