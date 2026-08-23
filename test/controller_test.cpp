#include "roo_wifi/controller.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "roo_scheduler.h"
#include "roo_wifi/hal/interface.h"
#include "roo_wifi/hal/store.h"

namespace {

class FakeStore : public roo_wifi::Store {
 public:
  bool getIsInterfaceEnabled() override { return enabled_; }
  void setIsInterfaceEnabled(bool enabled) override { enabled_ = enabled; }
  std::string getDefaultSSID() override { return default_ssid_; }
  void setDefaultSSID(const std::string& ssid) override {
    default_ssid_ = ssid;
  }
  void clearDefaultSSID() override { default_ssid_.clear(); }
  bool getPassword(const std::string&, std::string&) override { return false; }
  void setPassword(const std::string&, roo::string_view) override {}
  void clearPassword(const std::string&) override {}

 private:
  bool enabled_ = false;
  std::string default_ssid_;
};

class FakeInterface : public roo_wifi::Interface {
 public:
  void addEventListener(EventListener* listener) override {
    listener_ = listener;
  }
  void removeEventListener(EventListener* listener) override {
    if (listener_ == listener) listener_ = nullptr;
  }
  bool getApInfo(roo_wifi::NetworkDetails*) const override { return false; }
  bool startScan() override {
    scan_completed_ = false;
    return true;
  }
  bool scanCompleted() const override { return scan_completed_; }
  void disconnect() override {}
  bool connect(const std::string&, const std::string&) override { return true; }
  roo_wifi::ConnectionStatus getStatus() override {
    return roo_wifi::WL_DISCONNECTED;
  }
  bool getScanResults(std::vector<roo_wifi::NetworkDetails>* results,
                      int max_count) const override {
    const size_t count =
        std::min(scan_results_.size(), static_cast<size_t>(max_count));
    results->assign(scan_results_.begin(), scan_results_.begin() + count);
    return true;
  }

  void addScanResult(const char* ssid, int8_t rssi,
                     roo_wifi::AuthMode auth_mode) {
    roo_wifi::NetworkDetails result = {};
    std::strncpy(reinterpret_cast<char*>(result.ssid), ssid,
                 sizeof(result.ssid) - 1);
    result.rssi = rssi;
    result.authmode = auth_mode;
    scan_results_.push_back(result);
  }

  void completeScan() {
    scan_completed_ = true;
    listener_->onEvent(EV_SCAN_COMPLETED);
  }

 private:
  EventListener* listener_ = nullptr;
  bool scan_completed_ = false;
  std::vector<roo_wifi::NetworkDetails> scan_results_;
};

class RecordingListener : public roo_wifi::Controller::Listener {
 public:
  void onScanStarted() override { ++scan_started; }
  void onScanCompleted() override { ++scan_completed; }

  int scan_started = 0;
  int scan_completed = 0;
};

TEST(ControllerTest, EmptyScanStillNotifiesCompletion) {
  FakeStore store;
  FakeInterface interface;
  roo_scheduler::Scheduler scheduler;
  roo_wifi::Controller controller(store, interface, scheduler);
  RecordingListener listener;
  controller.addListener(&listener);
  controller.begin();

  controller.toggleEnabled();
  ASSERT_EQ(listener.scan_started, 1);
  interface.completeScan();

  EXPECT_EQ(listener.scan_completed, 1);
  EXPECT_EQ(controller.otherScannedNetworksCount(), 0);
}

TEST(ControllerTest, NonEmptyScanSortsAndDeduplicatesNetworks) {
  FakeStore store;
  FakeInterface interface;
  interface.addScanResult("Roo Secure", -70, roo_wifi::WIFI_AUTH_WPA2_PSK);
  interface.addScanResult("Roo Guest", -50, roo_wifi::WIFI_AUTH_OPEN);
  interface.addScanResult("Roo Guest", -80, roo_wifi::WIFI_AUTH_OPEN);
  roo_scheduler::Scheduler scheduler;
  roo_wifi::Controller controller(store, interface, scheduler);
  controller.begin();

  controller.toggleEnabled();
  interface.completeScan();

  ASSERT_EQ(controller.otherScannedNetworksCount(), 2);
  EXPECT_EQ(controller.otherNetwork(0).ssid, "Roo Guest");
  EXPECT_EQ(controller.otherNetwork(0).rssi, -50);
  EXPECT_TRUE(controller.otherNetwork(0).open);
  EXPECT_EQ(controller.otherNetwork(1).ssid, "Roo Secure");
  EXPECT_EQ(controller.otherNetwork(1).rssi, -70);
  EXPECT_FALSE(controller.otherNetwork(1).open);
}

}  // namespace
