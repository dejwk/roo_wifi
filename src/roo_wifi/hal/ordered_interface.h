#pragma once
#include <array>
#include <vector>

#include "roo_scheduler.h"
#include "roo_wifi/hal/interface.h"

namespace roo_wifi {

/// Defines native station commands consumed by the ordered radio adapter.
/// A successful asynchronous command posts a corresponding event. Already-idle
/// disconnect returns NotFound. A driver must not independently reconnect.
class NativeStation {
 public:
  using ScanRead = Interface::ScanRead;

  /// Carries a copied native station event to the ordered adapter.
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
    Status error = Status::kOk;
    int32_t native_code = 0;
  };

  /// Receives native events in posting order, potentially from another thread.
  class Receiver {
   public:
    /// Destroys a receiver after the station has detached it.
    virtual ~Receiver() = default;

    /// Queues a native event for ordered processing.
    /// @param event Native event payload to copy.
    virtual void post(const Event &event) = 0;
  };

  /// Destroys a detached native station.
  virtual ~NativeStation() = default;

  /// Attaches the sole event receiver without starting a connection.
  /// @param receiver Receiver that remains valid until detach returns.
  virtual Status attach(Receiver &receiver) = 0;

  /// Detaches the receiver and waits for in-flight callbacks to return.
  virtual void detach() = 0;

  /// Reports the features supported by the native station.
  virtual Support support() const = 0;

  /// Changes the physical station enablement.
  /// @param enabled Desired physical station state.
  virtual Status enable(bool enabled) = 0;

  /// Starts a network scan with bounded retained results.
  /// @param max_results Maximum records to retain.
  virtual Status scan(uint16_t max_results) = 0;

  /// Requests cancellation of the native scan.
  virtual Status stopScan() = 0;

  /// Begins selection and connection to a network.
  /// @param config Network settings to apply.
  /// @param credentials Credential material for the attempt.
  virtual Status connect(const ConnectionConfig &config,
                         const Credentials &credentials) = 0;

  /// Continues an asynchronously prepared connection on scheduler context.
  virtual Status continueConnect() = 0;

  /// Starts physical disconnection from the active network.
  virtual Status disconnect() = 0;

  /// Copies records from a completed scan.
  /// @param out Destination record array.
  /// @param capacity Number of records that fit in @p out.
  /// @param result Receives count and truncation state on success.
  virtual Status readScan(ScanRecord *out, size_t capacity,
                          ScanRead &result) const = 0;
};

/// Preserves one native FIFO and sequences switches through disconnect
/// outcomes. Owns bounded handoff storage; overflow faults admissions instead
/// of dropping an event and reusing its operation identity. All public calls
/// use scheduler context.
class OrderedInterface : public Interface, private NativeStation::Receiver {
 public:
  /// Creates an adapter that serializes commands for one native station.
  /// @param native Station that outlives this adapter.
  explicit OrderedInterface(NativeStation &native);

  /// Shuts down the adapter and detaches its native station.
  ~OrderedInterface() override;

  /// Attaches an event sink and creates deferred dispatch work.
  /// @param sink Controller event recipient.
  /// @param scheduler Context used for deferred dispatch.
  Status begin(Sink &sink, roo_scheduler::Scheduler &scheduler) override;

  /// Returns the native station's supported features.
  Support support() const override;

  /// Starts a radio enablement operation.
  /// @param id Operation ID to complete.
  /// @param enabled Desired physical radio state.
  Status setEnabled(OperationId id, bool enabled) override;

  /// Starts a bounded scan operation.
  /// @param id Operation ID to complete.
  /// @param max_results Maximum records to retain.
  Status scan(OperationId id, uint16_t max_results) override;

  /// Starts a connection operation.
  /// @param id Operation ID to complete.
  /// @param config Network settings to copy.
  /// @param credentials Credential material to copy.
  Status connect(OperationId id, const ConnectionConfig &config,
                 const Credentials &credentials) override;

  /// Starts a disconnect operation.
  /// @param id Operation ID to complete.
  Status disconnect(OperationId id) override;

  /// Cancels a pending station or scan operation.
  /// @param id Operation ID to cancel.
  Status cancel(OperationId id) override;

  /// Copies results from the most recent completed scan.
  /// @param out Destination record array.
  /// @param capacity Number of records that fit in @p out.
  /// @param result Receives count and truncation state on success.
  Status readScanResults(ScanRecord *out, size_t capacity,
                         ScanRead &result) const override;

  /// Detaches native callbacks and clears queued work.
  void shutdown() override;

 private:
  void post(const NativeStation::Event &) override;
  void drain();
  void process(const NativeStation::Event &);
  void startConnection();
  void finishStation(Status, int32_t = 0);
  void finishScan(Status, int32_t = 0);
  Status stationAdmission(OperationId) const;
  NativeStation &native_;
  Sink *sink_ = nullptr;
  std::unique_ptr<roo_scheduler::SingletonTask> dispatch_;
  roo::mutex mutex_;
  std::array<NativeStation::Event, 16> queue_;
  size_t head_ = 0;
  size_t count_ = 0;
  bool overflow_ = false;
  bool attached_ = false;
  bool faulted_ = false;
  bool enabled_ = false;
  bool cancelling_ = false;
  bool scan_cancelling_ = false;
  bool waiting_disconnect_ = false;
  bool desired_enabled_ = false;
  OperationResult station_;
  OperationResult scan_;
  LinkState link_;
  ConnectionConfig config_;
  Credentials credentials_;
};

}  // namespace roo_wifi
