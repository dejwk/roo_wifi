#pragma once
#include "roo_wifi/types.h"

namespace roo_scheduler {
class Scheduler;
}

namespace roo_wifi {
/// Persists Wi-Fi profiles and radio enablement on the controller context.
class Store {
 public:
  /// Destroys the persistence adapter after its controller.
  virtual ~Store() = default;

  /// Opens/initializes persistence without invoking controller listeners.
  virtual Status begin() = 0;

  /// Loads non-secret profile settings without exposing credentials.
  /// @param id Nonzero application-assigned profile key.
  /// @param out Receives the profile on success and is unchanged on failure.
  virtual Status loadProfile(ProfileId id, Profile &out) const = 0;

  /// Loads credentials for constructing an admitted connection attempt.
  /// @param id Nonzero application-assigned profile key.
  /// @param out Receives credentials on success and is unchanged on failure.
  virtual Status loadCredentials(ProfileId id, Credentials &out) const = 0;

  /// Creates or replaces a profile and applies its credential update.
  /// @param id Nonzero profile key to save.
  /// @param settings Non-secret settings to persist.
  /// @param credential Requested credential action and replacement material.
  virtual SaveResult saveProfile(ProfileId id, const ProfileSettings &settings,
                                 const CredentialUpdate &credential) = 0;

  /// Removes a profile without disconnecting an active link that used it.
  /// @param id Nonzero profile key to remove.
  virtual Status removeProfile(ProfileId id) = 0;

  /// Reads persisted radio enablement.
  /// @param out Receives the stored value on success.
  virtual Status readEnabled(bool &out) const = 0;

  /// Persists the observed physical radio enablement.
  /// @param enabled Value to persist.
  virtual Status writeEnabled(bool enabled) = 0;
};

}  // namespace roo_wifi
