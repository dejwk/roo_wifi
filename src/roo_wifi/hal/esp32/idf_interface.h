#pragma once
#include "esp_event.h"
#include "esp_wifi.h"
#include "roo_wifi/hal/ordered_interface.h"

namespace roo_wifi {

/// Drives the ESP32 station through ESP-IDF APIs.
class Esp32Station : public NativeStation {
 public:
  /// Bounds reuse of the three strongest compatible AP candidates.
  struct SelectionOptions {
    /// Maximum age of scan candidates reused by a subsequent connection.
    uint32_t cache_lifetime_ms = 30000;

    /// Time to avoid an AP after two failures or an explicit absence/rejection.
    uint32_t failure_cooldown_ms = 30000;
  };

  /// Creates an unattached ESP32 station with default candidate lifetimes.
  Esp32Station() = default;

  /// Creates an unattached station with the supplied candidate lifetimes.
  explicit Esp32Station(SelectionOptions options)
      : selection_options_(options) {}

  /// Destroys the station driver after detaching it from ESP event delivery.
  ~Esp32Station() override;

  /// Registers ESP event handlers for the supplied receiver.
  /// @param receiver Recipient of translated station events.
  Status attach(Receiver& receiver) override;

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
  Status connect(const ConnectionConfig& config,
                 const Credentials& credentials) override;

  /// Connects to the access point selected by the preceding scan.
  Status continueConnect() override;

  /// Disconnects the ESP32 station or cancels candidate selection.
  Status disconnect() override;

  /// Copies records retained from the last completed scan.
  /// @param out Destination record array.
  /// @param capacity Number of records that fit in @p out.
  /// @param result Receives count and truncation state on success.
  Status readScan(ScanRecord* out, size_t capacity,
                  ScanRead& result) const override;

 private:
  /// Keeps only connection-selection metadata, never credential material.
  struct Candidate {
    roo_time::Uptime retry_after;
    uint8_t bssid[6] = {};
    uint8_t channel = 0;
    int8_t rssi = -128;
    wifi_auth_mode_t auth = WIFI_AUTH_OPEN;
    uint8_t failures = 0;
  };

  static_assert(sizeof(Candidate) <= 24, "AP candidate RAM budget");

  /// Prepares the strongest cached candidate whose cooldown has expired.
  bool selectCandidate();

  /// Refreshes bounded candidates while retaining recent BSSID penalties.
  void rememberCandidates(const wifi_ap_record_t* aps, size_t count);

  /// Records failure of the selected AP without affecting deliberate teardown.
  void candidateFailed(ConnectionFailure failure);

  /// Forwards an ESP event callback to its station instance.
  static void Dispatch(void*, esp_event_base_t, int32_t, void*);

  /// Translates one ESP event into the portable native-station lifecycle.
  void event(esp_event_base_t, int32_t, void*);

  /// Applies IP, MAC, and AP settings for the selected scan candidate.
  Status startSelected(const wifi_ap_record_t&);
  Receiver* receiver_ = nullptr;
  esp_event_handler_instance_t wifi_handler_ = nullptr;
  esp_event_handler_instance_t ip_handler_ = nullptr;
  mutable roo::mutex mutex_;
  bool selecting_ = false;
  bool scan_active_ = false;
  bool scan_cancelled_ = false;
  bool prepared_ = false;
  bool attempt_active_ = false;
  SelectionOptions selection_options_;
  std::array<Candidate, 3> candidates_;
  size_t candidate_count_ = 0;
  roo_time::Uptime candidates_expire_;

  ConnectionFailure rejected_security_ = ConnectionFailure::kNone;
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
  Esp32IdfInterface() : OrderedInterface(static_cast<Esp32Station&>(*this)) {}

  /// Creates the adapter with explicit AP cache and failure cooldown lifetimes.
  explicit Esp32IdfInterface(Esp32Station::SelectionOptions options)
      : Esp32Station(options),
        OrderedInterface(static_cast<Esp32Station&>(*this)) {}

  /// Destroys the ESP-IDF radio adapter.
  /// Shuts down ordered dispatch before destroying the station driver.
  ~Esp32IdfInterface() override { OrderedInterface::shutdown(); }
};

}  // namespace roo_wifi
