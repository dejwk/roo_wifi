#pragma once
#include <stdint.h>

#include "roo_wifi/profile.h"
#include "roo_wifi/status.h"

namespace roo_wifi {

/// Identifies a native radio operation and its completion.
/// Values are nonzero and never reused during one controller lifetime.
using OperationId = uint64_t;

/// Identifies the native adapter action associated with an operation result.
enum class OperationKind : uint8_t { kEnable, kScan, kConnect, kDisconnect };

/// Reports the terminal result of one admitted native adapter transition.
struct OperationResult {
  /// ID allocated when the controller started this native transition.
  OperationId id = 0;

  /// Action that completed.
  OperationKind kind = OperationKind::kScan;

  /// Successful completion or one terminal failure/cancellation.
  Status status = Status::kOk;

  /// Platform diagnostic code when @p has_native_code is true.
  int32_t native_code = 0;

  /// Whether @p native_code contains a platform diagnostic.
  bool has_native_code = false;
};

}  // namespace roo_wifi
