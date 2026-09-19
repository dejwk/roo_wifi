#pragma once
#include "roo_wifi/types.h"
namespace roo_scheduler {
class Scheduler;
}
namespace roo_wifi {
/// Persistence adapter: synchronous operations executed on the controller
/// context.
class Store {
 public:
  /// Destroys the persistence adapter after its controller.
  virtual ~Store() = default;

  /// Opens/initializes persistence without invoking controller listeners.
  virtual Error begin() = 0;

  /// Reads a known key without exposing secrets; failure leaves output
  /// unchanged.
  virtual Error loadProfile(ProfileId id, Profile &out) const = 0;
  /// Privileged backend access, used only to construct connection input.
  virtual Error loadCredentials(ProfileId id, Credentials &out) const = 0;
  /// Create or replace a known nonzero key; kKeep requires a valid old profile.
  virtual SaveResult saveProfile(ProfileId id, const ProfileSettings &settings,
                                 const CredentialUpdate &credential) = 0;

  /// Queues deletion of a known key without disconnecting its active link.
  virtual Error removeProfile(ProfileId id) = 0;

  /// Reads the independent persisted enablement value.
  virtual Error readEnabled(bool &out) const = 0;

  /// Persists enablement, returning explicit storage failure.
  virtual Error writeEnabled(bool enabled) = 0;
};

}  // namespace roo_wifi
