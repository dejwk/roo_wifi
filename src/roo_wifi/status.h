#pragma once
#include <stdint.h>

namespace roo_wifi {

/// Reports admission, completion, validation, and persistence outcomes.
enum class Status : uint8_t {
  kOk,
  kNotFound,
  kInvalidArgument,
  kUnsupported,
  kBusy,
  kDisabled,
  kNotStarted,
  kCancelled,
  kTimeout,
  kConnectionFailed,
  kStorageFailure,
  kCommitUnknown,
  kCorrupt,
  /// Enumeration stopped because its visitor requested it.
  kStopped,
  /// A storage hash belongs to a different SSID; no mutation was attempted.
  kHashCollision
};

}  // namespace roo_wifi
