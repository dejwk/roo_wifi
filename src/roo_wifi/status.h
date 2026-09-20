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
  kIncomplete
};

}  // namespace roo_wifi
