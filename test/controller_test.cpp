#include "roo_wifi/controller.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
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
  bool getPassword(const std::string& ssid, std::string& password) override {
    auto itr = passwords_.find(ssid);
    if (itr == passwords_.end()) return false;
    password = itr->second;
    return true;
  }
  void setPassword(const std::string& ssid, roo::string_view password) override {
    passwords_[ssid] = std::string(password.data(), password.size());
  }
  void clearPassword(const std::string& ssid) override { passwords_.erase(ssid); }

 private:
  bool enabled_ = false;
  std::string default_ssid_;
  std::unordered_map<std::string, std::string> passwords_;
};

class FakeInterface : public roo_wifi::Interface {
 public:
  void addEventListener(EventListener* listener) override {
    listener_ = listener;
  }
  void removeEventListener(EventListener* listener) override {
    if (listener_ == listener) listener_ = nullptr;
  }
  bool getApInfo(roo_wifi::NetworkDetails* info) const override {
    if (!has_ap_info_) return false;
    *info = ap_info_;
    return true;
  }
  bool startScan() override {
    ++start_scan_calls;
    scan_completed_ = false;
    return true;
  }
  bool scanCompleted() const override { return scan_completed_; }
  void disconnect() override {}
  bool connect(const std::string& ssid, const std::string& password) override {
    ++connect_calls;
    last_ssid = ssid;
    last_password = password;
    if (connect_event != EV_UNKNOWN) listener_->onEvent(connect_event, ssid);
    return connect_result;
  }
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

  void setApInfo(const char* ssid, int8_t rssi,
                 roo_wifi::ConnectionStatus status) {
    ap_info_ = {};
    std::strncpy(reinterpret_cast<char*>(ap_info_.ssid), ssid,
                 sizeof(ap_info_.ssid) - 1);
    ap_info_.rssi = rssi;
    ap_info_.status = status;
    has_ap_info_ = true;
  }

  void completeScan() {
    scan_completed_ = true;
    listener_->onEvent(EV_SCAN_COMPLETED, roo::string_view());
  }

  void emit(EventType type, roo::string_view ssid = roo::string_view()) {
    listener_->onEvent(type, ssid);
  }

  bool connect_result = true;
  EventType connect_event = EV_UNKNOWN;
  int start_scan_calls = 0;
  int connect_calls = 0;
  std::string last_ssid;
  std::string last_password;

 private:
  EventListener* listener_ = nullptr;
  bool scan_completed_ = false;
  bool has_ap_info_ = false;
  roo_wifi::NetworkDetails ap_info_ = {};
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
  scheduler.executeEligibleTasks();

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
  scheduler.executeEligibleTasks();

  ASSERT_EQ(controller.otherScannedNetworksCount(), 2);
  EXPECT_EQ(controller.otherNetwork(0).ssid, "Roo Guest");
  EXPECT_EQ(controller.otherNetwork(0).rssi, -50);
  EXPECT_TRUE(controller.otherNetwork(0).open);
  EXPECT_EQ(controller.otherNetwork(1).ssid, "Roo Secure");
  EXPECT_EQ(controller.otherNetwork(1).rssi, -70);
  EXPECT_FALSE(controller.otherNetwork(1).open);
}

TEST(ControllerTest, EnabledControllerStartsScanAndReconnectsAtBoot) {
  FakeStore store;
  store.setIsInterfaceEnabled(true);
  store.setDefaultSSID("Roo Secure");
  store.setPassword("Roo Secure", "secret");
  FakeInterface interface;
  roo_scheduler::Scheduler scheduler;
  roo_wifi::Controller controller(store, interface, scheduler);

  controller.begin();

  EXPECT_EQ(interface.connect_calls, 1);
  EXPECT_EQ(interface.last_ssid, "Roo Secure");
  EXPECT_EQ(interface.last_password, "secret");
  EXPECT_EQ(interface.start_scan_calls, 1);
}

TEST(ControllerTest, FailedConnectionDoesNotReplaceStoredProfile) {
  FakeStore store;
  FakeInterface interface;
  roo_scheduler::Scheduler scheduler;
  roo_wifi::Controller controller(store, interface, scheduler);
  controller.begin();
  controller.toggleEnabled();
  interface.connect_result = false;

  EXPECT_FALSE(controller.connect("Roo Secure", "secret"));
  EXPECT_TRUE(store.getDefaultSSID().empty());
  std::string password;
  EXPECT_FALSE(store.getPassword("Roo Secure", password));
}

TEST(ControllerTest, SuccessfulConnectionIsNoLongerInProgressAfterGotIp) {
  FakeStore store;
  FakeInterface interface;
  roo_scheduler::Scheduler scheduler;
  roo_wifi::Controller controller(store, interface, scheduler);
  controller.begin();
  controller.toggleEnabled();

  ASSERT_TRUE(controller.connect("Roo Secure", "secret"));
  ASSERT_TRUE(controller.isConnecting());
  interface.emit(roo_wifi::Interface::EV_GOT_IP);
  scheduler.executeEligibleTasks();

  EXPECT_FALSE(controller.isConnecting());
  EXPECT_EQ(controller.currentNetworkStatus(), roo_wifi::WL_CONNECTED);
}

TEST(ControllerTest, SynchronousAuthenticationFailureKeepsAttemptedNetwork) {
  FakeStore store;
  FakeInterface interface;
  interface.addScanResult("Roo Guest", -50, roo_wifi::WIFI_AUTH_OPEN);
  interface.addScanResult("Roo Secure", -70, roo_wifi::WIFI_AUTH_WPA2_PSK);
  roo_scheduler::Scheduler scheduler;
  roo_wifi::Controller controller(store, interface, scheduler);
  controller.begin();
  controller.toggleEnabled();
  interface.completeScan();
  scheduler.executeEligibleTasks();

  interface.connect_event = roo_wifi::Interface::EV_CONNECTION_FAILED;
  ASSERT_TRUE(controller.connect("Roo Secure", "wrong"));
  scheduler.executeEligibleTasks();

  EXPECT_EQ("Roo Secure", controller.currentNetwork().ssid);
  EXPECT_EQ(roo_wifi::WL_CONNECT_FAILED, controller.currentNetworkStatus());
}

TEST(ControllerTest, ConnectionFailureStaysWithPendingNetwork) {
  FakeStore store;
  FakeInterface interface;
  interface.addScanResult("Roo Guest", -50, roo_wifi::WIFI_AUTH_OPEN);
  interface.addScanResult("Roo Secure", -70, roo_wifi::WIFI_AUTH_WPA2_PSK);
  roo_scheduler::Scheduler scheduler;
  roo_wifi::Controller controller(store, interface, scheduler);
  controller.begin();
  controller.toggleEnabled();
  interface.completeScan();
  scheduler.executeEligibleTasks();

  ASSERT_TRUE(controller.connect("Roo Guest", ""));
  interface.emit(roo_wifi::Interface::EV_GOT_IP);
  scheduler.executeEligibleTasks();
  ASSERT_EQ("Roo Guest", controller.currentNetwork().ssid);

  ASSERT_TRUE(controller.connect("Roo Secure", "wrong"));
  // A refresh can still observe the previous access point while the new
  // authentication attempt is in flight.
  interface.emit(roo_wifi::Interface::EV_CONNECTION_FAILED);
  scheduler.executeEligibleTasks();

  EXPECT_EQ("Roo Secure", controller.currentNetwork().ssid);
  EXPECT_EQ(roo_wifi::WL_CONNECT_FAILED, controller.currentNetworkStatus());
}

TEST(ControllerTest, ConnectionFailureClearsOnlyRejectedPassword) {
  FakeStore store;
  store.setPassword("Roo Guest", "guest-password");
  FakeInterface interface;
  roo_scheduler::Scheduler scheduler;
  roo_wifi::Controller controller(store, interface, scheduler);
  controller.begin();
  controller.toggleEnabled();

  ASSERT_TRUE(controller.connect("Roo Secure", "wrong"));
  interface.emit(roo_wifi::Interface::EV_CONNECTION_FAILED);
  scheduler.executeEligibleTasks();

  std::string password;
  EXPECT_FALSE(controller.getStoredPassword("Roo Secure", password));
  ASSERT_TRUE(controller.getStoredPassword("Roo Guest", password));
  EXPECT_EQ("guest-password", password);
}

TEST(ControllerTest, QueuedFailureCannotMoveToNewConnectionAttempt) {
  FakeStore store;
  FakeInterface interface;
  interface.addScanResult("Roo Guest", -50, roo_wifi::WIFI_AUTH_OPEN);
  interface.addScanResult("Roo Secure", -70, roo_wifi::WIFI_AUTH_WPA2_PSK);
  roo_scheduler::Scheduler scheduler;
  roo_wifi::Controller controller(store, interface, scheduler);
  controller.begin();
  controller.toggleEnabled();
  interface.completeScan();
  scheduler.executeEligibleTasks();

  ASSERT_TRUE(controller.connect("Roo Secure", "wrong"));
  ASSERT_TRUE(controller.connect("Roo Guest", ""));
  // The old hardware attempt reports its failure only after Guest has become
  // the pending target.
  interface.emit(roo_wifi::Interface::EV_CONNECTION_FAILED, "Roo Secure");
  scheduler.executeEligibleTasks();

  EXPECT_EQ("Roo Guest", controller.currentNetwork().ssid);
  EXPECT_EQ(roo_wifi::WL_DISCONNECTED, controller.currentNetworkStatus());
  EXPECT_TRUE(controller.isConnecting());
  std::string password;
  EXPECT_FALSE(controller.getStoredPassword("Roo Secure", password));

  interface.emit(roo_wifi::Interface::EV_GOT_IP);
  scheduler.executeEligibleTasks();
  EXPECT_EQ("Roo Guest", controller.currentNetwork().ssid);
  EXPECT_EQ(roo_wifi::WL_CONNECTED, controller.currentNetworkStatus());
}

TEST(ControllerTest, DelayedPreviousNetworkEventCannotReplaceFailedTarget) {
  FakeStore store;
  FakeInterface interface;
  interface.addScanResult("Roo Guest", -50, roo_wifi::WIFI_AUTH_OPEN);
  interface.addScanResult("Roo Secure", -70, roo_wifi::WIFI_AUTH_WPA2_PSK);
  roo_scheduler::Scheduler scheduler;
  roo_wifi::Controller controller(store, interface, scheduler);
  controller.begin();
  controller.toggleEnabled();
  interface.completeScan();
  scheduler.executeEligibleTasks();

  ASSERT_TRUE(controller.connect("Roo Guest", ""));
  interface.emit(roo_wifi::Interface::EV_GOT_IP);
  scheduler.executeEligibleTasks();
  ASSERT_EQ("Roo Guest", controller.currentNetwork().ssid);

  ASSERT_TRUE(controller.connect("Roo Secure", "wrong"));
  interface.emit(roo_wifi::Interface::EV_CONNECTION_FAILED, "Roo Secure");
  scheduler.executeEligibleTasks();
  ASSERT_EQ("Roo Secure", controller.currentNetwork().ssid);
  ASSERT_EQ(roo_wifi::WL_CONNECT_FAILED, controller.currentNetworkStatus());

  // Disconnecting the old AP can be reported after the new attempt's
  // authentication failure. It must not replace the failed target.
  interface.emit(roo_wifi::Interface::EV_DISCONNECTED, "Roo Guest");
  scheduler.executeEligibleTasks();

  EXPECT_EQ("Roo Secure", controller.currentNetwork().ssid);
  EXPECT_EQ(roo_wifi::WL_CONNECT_FAILED, controller.currentNetworkStatus());
}

TEST(ControllerTest, RefreshCannotReplaceFailedTargetWithPreviousAccessPoint) {
  FakeStore store;
  FakeInterface interface;
  interface.addScanResult("Roo Guest", -50, roo_wifi::WIFI_AUTH_OPEN);
  interface.addScanResult("Roo Secure", -70, roo_wifi::WIFI_AUTH_WPA2_PSK);
  roo_scheduler::Scheduler scheduler;
  roo_wifi::Controller controller(store, interface, scheduler);
  controller.begin();
  controller.toggleEnabled();
  interface.completeScan();
  scheduler.executeEligibleTasks();

  ASSERT_TRUE(controller.connect("Roo Secure", "wrong"));
  interface.emit(roo_wifi::Interface::EV_CONNECTION_FAILED, "Roo Secure");
  scheduler.executeEligibleTasks();
  ASSERT_EQ(roo_wifi::WL_CONNECT_FAILED, controller.currentNetworkStatus());

  // A periodic refresh can briefly observe the AP from before this attempt.
  interface.setApInfo("Roo Guest", -50, roo_wifi::WL_CONNECTED);
  controller.resume();

  EXPECT_EQ("Roo Secure", controller.currentNetwork().ssid);
  EXPECT_EQ(roo_wifi::WL_CONNECT_FAILED, controller.currentNetworkStatus());
}

}  // namespace
