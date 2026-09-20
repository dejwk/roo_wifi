#pragma once
#include <stdint.h>

#include "roo_wifi/configuration.h"

namespace roo_wifi {

/// Application-assigned key for a saved profile; zero means no profile.
using ProfileId = uint32_t;

/// Selects whether a profile keeps, replaces, or clears its credentials.
enum class CredentialIntent : uint8_t { kKeep, kReplace, kClear };

/// Describes the credential change applied when saving a profile.
struct CredentialUpdate {
  /// Requested credential action.
  CredentialIntent intent = CredentialIntent::kKeep;

  /// Replacement credential used only when @p intent is @p kReplace.
  Credentials replacement;
};

/// Contains persisted non-secret settings for one saved profile.
struct ProfileSettings {
  /// Connection settings to persist.
  ConnectionConfig connection;

  /// Whether the controller may reconnect this profile automatically.
  bool auto_connect = true;
};

/// Describes one saved profile without exposing its credentials.
struct Profile {
  /// Application-assigned nonzero profile key.
  ProfileId id = 0;

  /// Persisted non-secret settings.
  ProfileSettings settings;

  /// Whether credentials exist and can be loaded through privileged access.
  bool has_credentials = false;
};

}  // namespace roo_wifi
