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
  using ProfileVisitor = bool (*)(void *context, const Ssid &ssid);

  /// Destroys the persistence adapter after its controller.
  virtual ~Store() = default;

  /// Opens/initializes persistence without invoking controller listeners.
  virtual Status begin() = 0;

  /// Loads non-secret profile settings without exposing credentials.
  /// @param ssid Exact SSID of the saved configuration, with 1–32 bytes.
  /// @param out Receives the profile on success and is unchanged on failure.
  virtual Status loadProfile(const Ssid &ssid, Profile &out) const = 0;

  /// Calls `visitor` once for every persisted saved-profile SSID.
  ///
  /// The order is unspecified. Return false to stop early, in which case this
  /// returns Status::kStopped. Corrupt settings that cannot supply a trusted
  /// SSID stop enumeration with kCorrupt; credential errors are reported by
  /// loadProfile(). The SSID reference is borrowed for the callback only.
  /// Do not modify this store while it is being enumerated.
  template <typename Visitor>
  Status forEachProfile(Visitor &&visitor) const {
    using VisitorType = typename std::remove_reference<Visitor>::type;
    // The erased context is mutable; the callback keeps the visitor's cv type.
    struct Context {
      VisitorType *visitor;
    } context{std::addressof(visitor)};
    return enumerateProfiles(
        [](void *opaque, const Ssid &ssid) {
          Context &context = *static_cast<Context *>(opaque);
          return static_cast<bool>((*context.visitor)(ssid));
        },
        &context);
  }

  /// Loads credentials for constructing an admitted connection attempt.
  /// @param ssid Exact SSID of the saved configuration, with 1–32 bytes.
  /// @param out Receives credentials on success and is unchanged on failure.
  virtual Status loadCredentials(const Ssid &ssid, Credentials &out) const = 0;

  /// Creates or replaces the configuration for settings.connection.ssid.
  /// Applies the credential update; a different SSID creates a separate entry.
  /// Hash-based stores must reject conflicting SSIDs with kHashCollision before
  /// changing settings or credentials. Other failures can represent partial
  /// writes.
  /// @param settings Non-secret settings to persist.
  /// @param credential Requested credential action and replacement material.
  virtual Status saveProfile(const ProfileSettings &settings,
                             const CredentialUpdate &credential) = 0;

  /// Removes a profile without disconnecting an active link that used it.
  /// @param ssid Exact SSID of the saved configuration, with 1–32 bytes.
  virtual Status removeProfile(const Ssid &ssid) = 0;

  /// Reads the last successfully connected saved profile.
  /// @param out Receives a nonempty SSID on success.
  virtual Status readLastProfile(Ssid &out) const = 0;

  /// Persists the last successfully connected saved profile.
  /// @param ssid Nonempty SSID, or an empty SSID to clear the selection.
  virtual Status writeLastProfile(const Ssid &ssid) = 0;

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
