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
    /// Identifies the native lifecycle event represented by this payload.
    enum Kind : uint8_t {
      kEnabled,
      kDisabled,
      kAssociated,
      kAddressReady,
      kAddressLost,
      kDisconnected,
      kScanDone,
      kPrepared
    };

    /// Native lifecycle event kind.
    Kind kind;

    /// Link-state data carried by the event.
    LinkState link;

    /// Portable outcome carried by the event.
    Status status = Status::kOk;

    /// Optional native platform diagnostic.
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

/// Adapts a native station to scheduler-delivered radio operations.
/// Preserves native event order and sequences network switches through
/// disconnect outcomes. Owns bounded handoff storage; overflow faults
/// admissions instead of dropping an event and reusing its operation identity.
/// All public calls use scheduler context.
class OrderedInterface : public Interface, private NativeStation::Receiver {
 public:
  /// Creates a radio adapter that serializes commands for borrowed @p native.
  /// @param native Station that outlives this adapter.
  explicit OrderedInterface(NativeStation &native);

  /// Destroys the radio adapter after shutting down native event dispatch.
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
  Status startScan(OperationId id, uint16_t max_results) override;

  /// Starts a connection operation.
  /// @param id Operation ID to complete.
  /// @param config Network settings to copy.
  /// @param credentials Credential material to copy.
  Status connect(OperationId id, const ConnectionConfig &config,
                 const Credentials &credentials) override;

  /// Starts a disconnect operation.
  /// @param id Operation ID to complete.
  Status disconnect(OperationId id) override;

  /// Requests native scan cancellation without cancelling station work.
  Status cancelScan() override;

  /// Requests cancellation of the active connection attempt.
  /// Completion is delivered after the native teardown event.
  Status cancelConnect() override;

  /// Copies results from the most recent completed scan.
  /// @param out Destination record array.
  /// @param capacity Number of records that fit in @p out.
  /// @param result Receives count and truncation state on success.
  Status readScanResults(ScanRecord *out, size_t capacity,
                         ScanRead &result) const override;

  /// Detaches native callbacks and clears queued work.
  void shutdown() override;

 private:
  /// Stores only the payload consumed for an event's kind in the native FIFO.
  /// SSID/BSSID remain available for rejecting stale connection events. Full
  /// link state is assembled only when an event is processed, not per slot.
  struct QueuedEvent {
    /// Association metadata, excluding addresses learned by later IP events.
    struct Association {
      MacAddress station_mac;
      AuthMode security;
      int8_t rssi_dbm;
      uint8_t channel;
      bool has_radio_info : 1;
      bool has_station_mac : 1;
    };

    /// Address data published by an IP-ready event.
    struct Addresses {
      Ipv4Address address;
      Ipv4Address gateway;
      Ipv4Address dns1;
      Ipv4Address dns2;
      bool has_ipv4 : 1;
      bool has_dns1 : 1;
      bool has_dns2 : 1;
    };

    /// Shares payload storage between mutually exclusive event kinds.
    union Payload {
      /// Creates an empty address payload for an unused queue slot.
      Payload() : addresses{} {}

      Association association;
      Addresses addresses;
    };

    /// Creates an empty queue slot; no event is pending until it is assigned.
    QueuedEvent() = default;

    /// Copies the fields needed to process @p event into compact storage.
    explicit QueuedEvent(const NativeStation::Event &event);

    /// Reconstructs the event fields used by the station state machine.
    NativeStation::Event expand() const;

    int32_t native_code = 0;
    NativeStation::Event::Kind kind = NativeStation::Event::kEnabled;
    Status status = Status::kOk;
    Ssid ssid;
    MacAddress bssid;
    Payload payload;
  };

  static_assert(sizeof(QueuedEvent) <= 64, "Native event queue entry budget");

  /// Enqueues a native event for scheduler-context processing.
  void post(const NativeStation::Event &) override;

  /// Drains the bounded native-event queue in posting order.
  void drain();

  /// Applies one native event to the portable state machine.
  void process(const NativeStation::Event &);

  /// Starts a prepared connection after any old link has disconnected.
  void startConnection();

  /// Settles the active station operation.
  void finishStation(Status, int32_t = 0);

  /// Settles the active scan operation.
  void finishScan(Status, int32_t = 0);

  /// Reports whether the station transition can admit the requested operation
  /// ID.
  Status stationAdmission(OperationId) const;
  NativeStation &native_;
  Sink *sink_ = nullptr;
  std::unique_ptr<roo_scheduler::SingletonTask> dispatch_;
  roo::mutex mutex_;
  std::array<QueuedEvent, 16> queue_;
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
