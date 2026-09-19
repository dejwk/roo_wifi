#pragma once
#include <array>
#include <vector>

#include "roo_scheduler.h"
#include "roo_wifi/hal/interface.h"

namespace roo_wifi {
/// Native station commands used by the ordered adapter and its test harness.
/// A successful asynchronous command posts a corresponding event. Already-idle
/// disconnect returns NotFound. A driver must not independently reconnect.
class NativeStation {
 public:
  /// Copied native event payload; interface/IP identity is checked by the
  /// driver.
  struct Event {
    enum Kind {
      kEnabled,
      kDisabled,
      kAssociated,
      kAddressReady,
      kAddressLost,
      kDisconnected,
      kScanDone,
      kPrepared
    } kind;
    LinkState link;
    Error error = Error::kOk;
    int32_t native_code = 0;
  };
  /// Receives native events in posting order, potentially from another thread.
  class Receiver {
   public:
    /// Initializes the adapter and its bounded state.
    virtual ~Receiver() = default;

    /// Initializes the adapter and its bounded state.
    virtual void post(const Event &event) = 0;
  };

  /// Initializes the adapter and its bounded state.
  virtual ~NativeStation() = default;
  /// Attaches the sole station owner, without starting a connection.
  virtual Error attach(Receiver &) = 0;
  /// Detaches and waits for callbacks to return.
  virtual void detach() = 0;

  /// Initializes the adapter and its bounded state.
  virtual Support support() const = 0;

  /// Initializes the adapter and its bounded state.
  virtual Error enable(bool) = 0;

  /// Initializes the adapter and its bounded state.
  virtual Error scan(uint16_t) = 0;

  /// Initializes the adapter and its bounded state.
  virtual Error stopScan() = 0;

  /// Initializes the adapter and its bounded state.
  virtual Error connect(const ConnectionConfig &, const Credentials &) = 0;
  /// Continues an asynchronously prepared connection on scheduler context.
  virtual Error continueConnect() = 0;

  /// Initializes the adapter and its bounded state.
  virtual Error disconnect() = 0;
  /// Called after scan completion; leaves out unchanged on failure.
  virtual Error readScan(ScanRecord *, size_t, ScanRead &) const = 0;
};

/// Preserves one native FIFO and sequences switches through disconnect
/// outcomes. Owns bounded handoff storage; overflow faults admissions instead
/// of dropping an event and reusing its operation identity. All public calls
/// use scheduler context.
class OrderedInterface : public Interface, private NativeStation::Receiver {
 public:
  explicit OrderedInterface(NativeStation &native);

  /// Implements the inherited OrderedInterface contract.
  ~OrderedInterface() override;

  /// Implements the inherited begin contract.
  Error begin(Sink &, roo_scheduler::Scheduler &) override;

  /// Implements the inherited support contract.
  Support support() const override;

  /// Implements the inherited setEnabled contract.
  Error setEnabled(OperationId, bool) override;

  /// Implements the inherited scan contract.
  Error scan(OperationId, uint16_t) override;
  Error connect(OperationId, const ConnectionConfig &,
                const Credentials &) override;

  /// Implements the inherited disconnect contract.
  Error disconnect(OperationId) override;

  /// Implements the inherited cancel contract.
  Error cancel(OperationId) override;

  /// Implements the inherited readScanResults contract.
  Error readScanResults(ScanRecord *, size_t, ScanRead &) const override;

  /// Implements the inherited shutdown contract.
  void shutdown() override;

 private:
  void post(const NativeStation::Event &) override;
  void drain();
  void process(const NativeStation::Event &);
  void startConnection();
  void finishStation(Error, int32_t = 0);
  void finishScan(Error, int32_t = 0);
  Error stationAdmission(OperationId) const;
  NativeStation &native_;
  Sink *sink_ = nullptr;
  std::unique_ptr<roo_scheduler::SingletonTask> dispatch_;
  roo::mutex mutex_;
  std::array<NativeStation::Event, 16> queue_;
  size_t head_ = 0, count_ = 0;
  bool overflow_ = false, attached_ = false, faulted_ = false, enabled_ = false;
  bool cancelling_ = false, scan_cancelling_ = false,
       waiting_disconnect_ = false;
  bool desired_enabled_ = false;
  OperationResult station_, scan_;
  LinkState link_;
  ConnectionConfig config_;
  Credentials credentials_;
};
}  // namespace roo_wifi
