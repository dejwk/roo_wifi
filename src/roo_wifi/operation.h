#pragma once
#include <stdint.h>

#include "roo_wifi/profile.h"
#include "roo_wifi/status.h"

namespace roo_wifi {

/// Nonzero operation identity, never reused during one controller lifetime.
using OperationId = uint64_t;

/// Identifies the controller action associated with an operation result.
enum class OperationKind : uint8_t {
  kEnable,
  kScan,
  kConnect,
  kDisconnect,
  kSave,
  kRemove
};

/// Reports the terminal result of one admitted controller operation.
struct OperationResult {
  /// ID allocated when the controller admitted this operation.
  OperationId id = 0;

  /// Action that completed.
  OperationKind kind = OperationKind::kScan;

  /// Successful completion or one terminal failure/cancellation.
  Status error = Status::kOk;

  /// Created, saved, removed, or connected profile key when applicable.
  ProfileId profile_id = 0;

  /// Platform diagnostic code when @p has_native_code is true.
  int32_t native_code = 0;

  /// Whether @p native_code contains a platform diagnostic.
  bool has_native_code = false;
};

}  // namespace roo_wifi
