#pragma once
#include "esp_event.h"
#include "esp_wifi.h"
#include "roo_wifi/hal/ordered_interface.h"

namespace roo_wifi {
/// ESP-IDF station driver with Arduino initialization and exact AP selection.
class Esp32Station : public NativeStation {
 public:
  /// Initializes the adapter and its bounded state.
  Esp32Station() = default;

  /// Implements the inherited Esp32Station contract.
  ~Esp32Station() override;

  /// Implements the inherited attach contract.
  Error attach(Receiver &) override;

  /// Implements the inherited detach contract.
  void detach() override;

  /// Implements the inherited support contract.
  Support support() const override;

  /// Implements the inherited enable contract.
  Error enable(bool) override;

  /// Implements the inherited scan contract.
  Error scan(uint16_t) override;

  /// Implements the inherited stopScan contract.
  Error stopScan() override;

  /// Implements the inherited connect contract.
  Error connect(const ConnectionConfig &, const Credentials &) override;

  /// Implements the inherited continueConnect contract.
  Error continueConnect() override;

  /// Implements the inherited disconnect contract.
  Error disconnect() override;

  /// Implements the inherited readScan contract.
  Error readScan(ScanRecord *, size_t, ScanRead &) const override;

 private:
  static void Dispatch(void *, esp_event_base_t, int32_t, void *);
  void event(esp_event_base_t, int32_t, void *);
  Error startSelected(const wifi_ap_record_t &);
  Receiver *receiver_ = nullptr;
  esp_event_handler_instance_t wifi_handler_ = nullptr, ip_handler_ = nullptr;
  mutable roo::mutex mutex_;
  bool selecting_ = false, scan_active_ = false, scan_cancelled_ = false;
  bool prepared_ = false;
  wifi_ap_record_t selected_ = {};
  ConnectionConfig config_;
  Credentials secret_;
  std::vector<ScanRecord> records_;
  uint16_t capacity_ = 100;
  bool truncated_ = false;
  uint8_t device_mac_[6] = {};
};

/// Owns the driver before constructing its portable ordered interface.
class Esp32ArduinoInterface : private Esp32Station, public OrderedInterface {
 public:
  /// Initializes the adapter and its bounded state.
  Esp32ArduinoInterface()
      : OrderedInterface(static_cast<Esp32Station &>(*this)) {}

  /// Implements the inherited Esp32ArduinoInterface contract.
  ~Esp32ArduinoInterface() override { OrderedInterface::shutdown(); }
};
}  // namespace roo_wifi
