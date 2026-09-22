#pragma once
#include <stddef.h>

#include "roo_wifi/configuration.h"
#include "roo_wifi/operation.h"
#include "roo_wifi/radio.h"

namespace roo_scheduler {
class Scheduler;
}

namespace roo_wifi {

/// Adapts a platform radio to the controller's asynchronous operation model.
/// All sink delivery is deferred and serialized on the supplied scheduler.
/// begin() must not emit callbacks. Commands carrying an operation ID copy
/// their inputs and return admission only: kOk promises a deferred
/// Sink::onOperationFinished() result,
/// while a rejected command must not later emit a completion for that ID.
/// Cancellation completes the original operation rather than introducing a
/// second operation ID. shutdown() may discard pending completions. A
/// successful scan read must copy the complete bounded result; on failure it
/// must leave the caller's buffer and ScanRead unchanged. shutdown detaches
/// producers and neutralizes queued work before returning.
class Interface {
 public:
  /// Reports the number of scan records copied by a radio adapter.
  struct ScanRead {
    /// Number of records copied into caller storage.
    size_t count = 0;

    /// Whether records were omitted because storage was bounded.
    bool truncated = false;
  };

  /// Receives radio events serialized onto the supplied scheduler context.
  class Sink {
   public:
    /// Destroys a previously detached event sink.
    virtual ~Sink() = default;

    /// Delivers the terminal result for an admitted operation exactly once.
    /// @param result Completed operation and its outcome.
    virtual void onOperationFinished(const OperationResult &result) = 0;

    /// Reports association, address readiness, or another link-state change.
    /// @param state Current observed link diagnostics.
    virtual void onLinkChanged(const LinkState &state) = 0;

    /// Reports the observed physical radio state.
    /// @param enabled True when the radio is enabled.
    virtual void onEnabledChanged(bool enabled) = 0;
  };

  /// Destroys a detached radio adapter.
  virtual ~Interface() = default;

  /// Attaches the controller sink and scheduler without emitting callbacks.
  /// The adapter borrows both dependencies until shutdown(). Native producers
  /// may run on other threads, but must hand events off to this scheduler.
  /// @param sink Receiver of deferred radio events.
  /// @param scheduler Context on which events are delivered.
  virtual Status begin(Sink &sink, roo_scheduler::Scheduler &scheduler) = 0;

  /// Returns actual hardware support, independently of consumer presentation.
  virtual Support support() const = 0;

  /// Requests a physical radio enablement transition.
  /// @param id Nonzero operation ID echoed in the deferred completion.
  /// @param enabled Desired physical radio state.
  virtual Status setEnabled(OperationId id, bool enabled) = 0;

  /// Starts a bounded network scan.
  /// @param id Nonzero operation ID echoed in the deferred completion.
  /// @param max_results Maximum records to retain.
  virtual Status startScan(OperationId id, uint16_t max_results) = 0;

  /// Starts a connection and reports success only after address readiness.
  /// @param id Nonzero operation ID echoed in the deferred completion.
  /// @param config Connection settings to copy.
  /// @param credentials Credential material to copy.
  virtual Status connect(OperationId id, const ConnectionConfig &config,
                         const Credentials &credentials) = 0;

  /// Starts a physical disconnect and reports its lifecycle outcome.
  /// @param id Nonzero operation ID echoed in the deferred completion.
  virtual Status disconnect(OperationId id) = 0;

  /// Requests cancellation of the active scan.
  /// Returning kOk does not mean teardown is finished: the original scan
  /// operation completes asynchronously with its original ID.
  virtual Status cancelScan() = 0;

  /// Requests cancellation of the active connection attempt.
  /// Completion is delivered after native teardown; the call does not wait.
  virtual Status cancelConnect() = 0;

  /// Copies records from a completed scan into caller-owned storage.
  /// A successful read copies the complete retained result into @p out.
  /// Failure leaves both the destination array and @p result unchanged.
  /// @param out Destination record array.
  /// @param capacity Number of records that fit in @p out.
  /// @param result Receives count and truncation state on success.
  virtual Status readScanResults(ScanRecord *out, size_t capacity,
                                 ScanRead &result) const = 0;

  /// Detaches the sink and prevents all subsequent event delivery.
  virtual void shutdown() = 0;
};

}  // namespace roo_wifi
