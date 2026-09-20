#include <cstdio>

#ifdef ROO_TESTING
#include <memory>

#include "roo_testing/microcontrollers/esp32/fake_esp32.h"
#include "roo_testing/transducers/wifi/wifi.h"
#endif

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "roo_scheduler.h"
#include "roo_wifi/esp32.h"

namespace {

#ifdef ROO_TESTING
// Statically constructs the host Wi-Fi environment before the application
// globals.
struct Emulator {
  roo_testing_transducers::wifi::Environment wifi;

  Emulator() {
    using namespace roo_testing_transducers::wifi;
    auto workshop =
        std::make_unique<AccessPoint>(MacAddress(2, 0, 0, 0, 0, 1), "Workshop");
    workshop->setAuthMode(AUTH_WPA2_PSK)
        ->setPasswd("example-passphrase")
        ->setChannel(1)
        ->setRSSI(kRssiVeryStrong);
    wifi.addAccessPoint(std::move(workshop));

    auto guest = std::make_unique<AccessPoint>(MacAddress(2, 0, 0, 0, 0, 2),
                                               "Guest Wi-Fi");
    guest->setChannel(6)->setRSSI(kRssiMedium);
    wifi.addAccessPoint(std::move(guest));

    auto sensor = std::make_unique<AccessPoint>(MacAddress(2, 0, 0, 0, 0, 3),
                                                "Sensor setup");
    sensor->setAuthMode(AUTH_WPA_WPA2_PSK)
        ->setPasswd("sensor-passphrase")
        ->setChannel(11)
        ->setRSSI(kRssiWeak);
    wifi.addAccessPoint(std::move(sensor));
    FakeEsp32().setWifiEnvironment(wifi);
  }
} emulator;
#endif

class ScanListener : public roo_wifi::Controller::Listener {
 public:
  explicit ScanListener(roo_wifi::Controller& controller)
      : controller_(controller) {}

  void onOperationFinished(const roo_wifi::OperationResult& result) override {
    if (result.kind != roo_wifi::OperationKind::kEnable) {
      return;
    }
    if (controller_.isEnabled()) {
      StartScan();
      return;
    }
    if (enable_requested_) {
      return;
    }
    enable_requested_ = true;
    if (controller_.setEnabled(true).id == 0) {
      std::printf("Could not enable the Wi-Fi station\n");
    }
  }

  void onScanChanged() override {
    const roo_wifi::Controller::ScanSnapshot snapshot =
        controller_.scanSnapshot();
    std::printf("Found %u%s networks\n", static_cast<unsigned>(snapshot.count),
                snapshot.truncated ? "+" : "");
    for (size_t index = 0; index < snapshot.count; ++index) {
      const roo_wifi::ScanRecord& record = snapshot.records[index];
      std::printf("%.*s  RSSI %d dBm, channel %u\n", record.ssid.size,
                  reinterpret_cast<const char*>(record.ssid.bytes),
                  record.rssi_dbm, record.channel);
    }
  }

 private:
  void StartScan() {
    if (scan_requested_) {
      return;
    }
    const roo_wifi::Controller::RequestResult request = controller_.scan();
    if (request.id == 0) {
      std::printf("Could not start the network scan\n");
      return;
    }
    scan_requested_ = true;
  }
  roo_wifi::Controller& controller_;
  bool enable_requested_ = false;
  bool scan_requested_ = false;
};

roo_scheduler::Scheduler scheduler;
roo_wifi::Esp32Wifi wifi(scheduler);
ScanListener listener(wifi.controller());

}  // namespace

extern "C" void app_main() {
  const esp_err_t nvs_status = nvs_flash_init();
  if (nvs_status != ESP_OK) {
    std::printf("NVS initialization failed: %s\n", esp_err_to_name(nvs_status));
    return;
  }

  wifi.controller().addListener(listener);
  if (wifi.controller().begin() != roo_wifi::Status::kOk) {
    std::printf("Could not initialize roo_wifi\n");
    return;
  }

  for (;;) {
    scheduler.executeEligibleTasks();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
