#pragma once
#include <memory>
#include <type_traits>

#include "roo_wifi/profile.h"
#include "roo_wifi/status.h"

namespace roo_wifi {

/// Persists Wi-Fi profiles and radio enablement on the controller context.
class Store {
 public:
  /// Callback used internally by allocation-free profile enumeration.
  using ProfileVisitor = bool (*)(void *context, ProfileId id);

  /// Destroys the persistence adapter after its controller.
  virtual ~Store() = default;

  /// Opens/initializes persistence without invoking controller listeners.
  virtual Status begin() = 0;

  /// Loads non-secret profile settings without exposing credentials.
  /// @param id Nonzero application-assigned profile key.
  /// @param out Receives the profile on success and is unchanged on failure.
  virtual Status loadProfile(ProfileId id, Profile &out) const = 0;

  /// Calls `visitor` once for every committed saved-profile ID.
  ///
  /// The order is unspecified. Return false to stop early, in which case this
  /// returns Status::kStopped. Incomplete and deleted profiles are not visited.
  /// A committed profile whose metadata was later corrupted is still visited;
  /// loadProfile() reports that read failure independently. Do not modify this
  /// store while it is being enumerated.
  template <typename Visitor>
  Status forEachProfile(Visitor &&visitor) const {
    using VisitorType = typename std::remove_reference<Visitor>::type;
    return enumerateProfiles(
        [](void *context, ProfileId id) {
          return static_cast<bool>((*static_cast<VisitorType *>(context))(id));
        },
        const_cast<void *>(static_cast<const void *>(std::addressof(visitor))));
  }

  /// Loads credentials for constructing an admitted connection attempt.
  /// @param id Nonzero application-assigned profile key.
  /// @param out Receives credentials on success and is unchanged on failure.
  virtual Status loadCredentials(ProfileId id, Credentials &out) const = 0;

  /// Creates or replaces a profile and applies its credential update.
  /// @param id Nonzero profile key to save.
  /// @param settings Non-secret settings to persist.
  /// @param credential Requested credential action and replacement material.
  virtual Status saveProfile(ProfileId id, const ProfileSettings &settings,
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

 protected:
  /// Implements allocation-free profile enumeration for concrete stores.
  virtual Status enumerateProfiles(ProfileVisitor visitor,
                                   void *context) const = 0;
};

}  // namespace roo_wifi
