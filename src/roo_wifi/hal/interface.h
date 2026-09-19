#pragma once
#include "roo_wifi/types.h"
namespace roo_scheduler {
class Scheduler;
}
namespace roo_wifi {
/// Radio adapter; all sink delivery is deferred and serialized on the supplied
/// scheduler. begin must not emit callbacks. Commands copy their inputs and
/// return admission only. A successful scan read must copy the complete bounded
/// result; on failure it must leave the caller's buffer and ScanRead unchanged.
/// shutdown detaches producers and neutralizes queued work before returning.
class Interface {
 public:
  /// Receives radio events serialized onto the supplied scheduler context.
  class Sink {
   public:
    /// Destroys a previously detached event sink.
    virtual ~Sink() = default;

    /// Delivers exactly one terminal result per admitted request ID.
    virtual void onOperationFinished(const OperationResult &result) = 0;

    /// Notifies association, address readiness or subsequent link changes.
    virtual void onLinkChanged(const LinkState &state) = 0;

    /// Notifies actual physical radio state.
    virtual void onEnabledChanged(bool enabled) = 0;
  };

  /// Destroys a detached radio adapter.
  virtual ~Interface() = default;

  /// Attaches the sole sink and scheduler without emitting callbacks.
  virtual Error begin(Sink &sink, roo_scheduler::Scheduler &scheduler) = 0;

  /// Returns actual hardware support, independently of consumer presentation.
  virtual Support support() const = 0;
  /// kOk means admitted; completion is deferred and echoes the supplied ID.
  virtual Error setEnabled(OperationId id, bool enabled) = 0;

  /// Admits a bounded scan using the caller-supplied nonzero ID.
  virtual Error scan(OperationId id, uint16_t max_results) = 0;

  /// Copies connection input on admission; succeeds only at address readiness.
  virtual Error connect(OperationId id, const ConnectionConfig &config,
                        const Credentials &credentials) = 0;

  /// Admits physical disconnect; completes from its native lifecycle outcome.
  virtual Error disconnect(OperationId id) = 0;
  /// Accepted cancellation completes the target ID with kCancelled, not a new
  /// ID.
  virtual Error cancel(OperationId target) = 0;
  /// Read during successful scan completion; copies at most capacity records.
  virtual Error readScanResults(ScanRecord *out, size_t capacity,
                                ScanRead &result) const = 0;
  /// Detaches the sink and prevents subsequent delivery, including queued
  /// events.
  virtual void shutdown() = 0;
};

}  // namespace roo_wifi
