#pragma once
#include "roo_wifi/hal/store.h"

namespace roo_wifi {
/// Small-value persistence protocol over ordered, durable key writes.
/// Implementations must serialize access; successful writes survive restart in
/// call order. No key iteration, catalog, or atomic multi-key update is
/// required.
class FieldStore : public Store {
 public:
  /// Reads metadata only; failed reads leave out unchanged.
  Error loadProfile(ProfileId id, Profile &out) const override;
  /// Privileged credential read; failed reads leave out unchanged.
  Error loadCredentials(ProfileId id, Credentials &out) const override;
  /// Saves using incomplete/ready markers, with explicit credential intent.
  SaveResult saveProfile(ProfileId id, const ProfileSettings &settings,
                         const CredentialUpdate &credential) override;
  /// Marks deleted before cleanup; repeated removal retries cleanup.
  Error removeProfile(ProfileId id) override;

 protected:
  /// Reads at most 64 bytes, returning actual length; missing is NotFound.
  virtual Error readField(const char *key, uint8_t *out,
                          size_t &size) const = 0;
  /// Durably writes 1..64 bytes before returning success.
  virtual Error writeField(const char *key, const uint8_t *data,
                           size_t size) = 0;
  /// Durably removes a field; absent is success.
  virtual Error eraseField(const char *key) = 0;

 private:
  Error readStatus(ProfileId id) const;
  Error read(ProfileId id, ProfileSettings &settings,
             Credentials &secret) const;
};
}  // namespace roo_wifi
