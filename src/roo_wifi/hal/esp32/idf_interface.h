#pragma once
#include "esp_event.h"
#include "esp_wifi.h"
#include "roo_wifi/hal/ordered_interface.h"

namespace roo_wifi {

/// Drives the ESP32 station through ESP-IDF APIs.
class Esp32Station : public NativeStation {
 public:
  /// Creates an unattached ESP32 station driver.
  Esp32Station() = default;

  /// Detaches the driver from ESP event delivery.
  ~Esp32Station() override;

  /// Registers ESP event handlers for the supplied receiver.
  /// @param receiver Recipient of translated station events.
  Status attach(Receiver &receiver) override;

  /// Unregisters ESP event handlers and clears active state.
  void detach() override;

  /// Returns features available from this ESP32 station implementation.
  Support support() const override;

  /// Enables or disables ESP32 station mode.
  /// @param enabled Desired station mode.
  Status enable(bool enabled) override;

  /// Starts an ESP32 scan and bounds retained results.
  /// @param max_results Maximum records to retain.
  Status scan(uint16_t max_results) override;

  /// Requests cancellation of the active ESP32 scan.
  Status stopScan() override;

  /// Selects an exact access point and begins connection.
  /// @param config Network settings to apply.
  /// @param credentials Credential material for the attempt.
  Status connect(const ConnectionConfig &config,
                 const Credentials &credentials) override;

  /// Connects to the access point selected by the preceding scan.
  Status continueConnect() override;

  /// Disconnects the ESP32 station or cancels candidate selection.
  Status disconnect() override;

  /// Copies records retained from the last completed scan.
  /// @param out Destination record array.
  /// @param capacity Number of records that fit in @p out.
  /// @param result Receives count and truncation state on success.
  Status readScan(ScanRecord *out, size_t capacity,
                  ScanRead &result) const override;

 private:
  /// Forwards an ESP event callback to its station instance.
  static void Dispatch(void *, esp_event_base_t, int32_t, void *);

  /// Translates one ESP event into the portable native-station lifecycle.
  void event(esp_event_base_t, int32_t, void *);

  /// Applies IP, MAC, and AP settings for the selected scan candidate.
  Status startSelected(const wifi_ap_record_t &);
  Receiver *receiver_ = nullptr;
  esp_event_handler_instance_t wifi_handler_ = nullptr;
  esp_event_handler_instance_t ip_handler_ = nullptr;
  mutable roo::mutex mutex_;
  bool selecting_ = false;
  bool scan_active_ = false;
  bool scan_cancelled_ = false;
  bool prepared_ = false;
  wifi_ap_record_t selected_ = {};
  ConnectionConfig config_;
  Credentials secret_;
  std::vector<ScanRecord> records_;
  uint16_t capacity_ = 100;
  bool truncated_ = false;
  uint8_t device_mac_[6] = {};
};

/// Combines an ESP-IDF station driver with the portable ordered interface.
class Esp32IdfInterface : private Esp32Station, public OrderedInterface {
 public:
  /// Creates the station driver and its ordered interface.
  Esp32IdfInterface() : OrderedInterface(static_cast<Esp32Station &>(*this)) {}

  /// Shuts down ordered dispatch before destroying the station driver.
  ~Esp32IdfInterface() override { OrderedInterface::shutdown(); }
};

}  // namespace roo_wifi
