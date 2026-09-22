#include <type_traits>

#include "backend_fakes.h"
#include "esp_netif.h"
#include "gtest/gtest.h"
#include "roo_testing/microcontrollers/esp32/fake_esp32.h"
#include "roo_testing/transducers/wifi/wifi.h"
#include "roo_wifi.h"
#include "roo_wifi/hal/esp32/idf_interface.h"
#include "roo_wifi/hal/prefs/prefs_store.h"

namespace roo_wifi {
namespace {
static_assert(std::is_same<WiFi, Esp32WiFi>::value,
              "ESP32 builds select Esp32WiFi as roo_wifi::WiFi");
static_assert(std::is_constructible<Esp32WiFi, roo_scheduler::Scheduler&,
                                    roo_prefs::Store&>::value,
              "Esp32WiFi accepts a caller-owned roo_prefs backend");

esp_ip4_addr_t Ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  esp_ip4_addr_t result = {};
  result.addr = static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
                (static_cast<uint32_t>(c) << 16) |
                (static_cast<uint32_t>(d) << 24);
  return result;
}

/// Runs scheduler work while allowing the ESP32 simulation to advance.
void RunBackend(roo_scheduler::Scheduler& scheduler) {
  for (int i = 0; i < 1000; ++i) {
    scheduler.executeEligibleTasks();
    delay(1);
  }
}
}  // namespace

// Exercise event delivery while the application sleeps inside its scheduler.
TEST(Esp32BackendTest, EnableWhileSchedulerWaits) {
  roo_scheduler::Scheduler scheduler;
  Esp32IdfInterface radio;
  MemoryStore store;
  Controller controller(radio, store, scheduler);
  Observer observer;
  controller.addListener(observer);
  ASSERT_EQ(controller.begin(), Status::kOk);
  scheduler.delay(roo_time::Millis(50));
  ASSERT_FALSE(controller.isEnabled());
  auto request = controller.setEnabled(true);
  ASSERT_EQ(request, Status::kOk);
  scheduler.delay(roo_time::Millis(500));
  EXPECT_TRUE(controller.isEnabled());
  for (bool enabled : {false, true, true, false}) {
    request = controller.setEnabled(enabled);
    ASSERT_EQ(request, Status::kOk);
    scheduler.delay(roo_time::Millis(50));
    EXPECT_EQ(controller.isEnabled(), enabled);
  }
  controller.shutdown();
}

// Verifies the production radio selects exact security among same-SSID APs,
// completes only with an address, and switches through the old disconnect.
TEST(Esp32BackendTest, SecuritySelectionAndSwitch) {
  using namespace roo_testing_transducers::wifi;
  static Environment environment;
  environment.setScanDurationMs(20);
  auto open = std::make_unique<AccessPoint>(
      roo_testing_transducers::wifi::MacAddress(2, 0, 0, 0, 0, 1), "same");
  auto secure = std::make_unique<AccessPoint>(
      roo_testing_transducers::wifi::MacAddress(2, 0, 0, 0, 0, 2), "same");
  secure->setAuthMode(AUTH_WPA2_PSK);
  secure->setPasswd("password");
  environment.addAccessPoint(std::move(open));
  environment.addAccessPoint(std::move(secure));
  FakeEsp32().setWifiEnvironment(environment);
  roo_scheduler::Scheduler scheduler;
  Esp32IdfInterface radio;
  MemoryStore store;
  store.enabled = true;
  Controller controller(radio, store, scheduler);
  Observer observer;
  controller.addListener(observer);
  ASSERT_EQ(controller.begin(), Status::kOk);
  RunBackend(scheduler);
  ASSERT_TRUE(controller.isEnabled());
  ConnectionConfig config = TestConfig("same");
  config.security = AuthMode::kWpa2Personal;
  config.hidden = true;
  config.mac_policy = MacPolicy::kRandomized;
  config.ip_mode = IpMode::kStaticIpv4;
  config.static_ipv4.address = {{192, 168, 7, 12}};
  config.static_ipv4.gateway = {{192, 168, 7, 1}};
  config.static_ipv4.dns1 = {{1, 1, 1, 1}};
  uint8_t original_mac[6];
  ASSERT_EQ(esp_wifi_get_mac(WIFI_IF_STA, original_mac), ESP_OK);
  Credentials secret;
  secret.size = 8;
  memcpy(secret.bytes, "password", 8);
  Status secure_request = controller.connect(config, secret);
  ASSERT_EQ(secure_request, Status::kOk);
  RunBackend(scheduler);
  EXPECT_EQ(controller.linkState().phase, LinkPhase::kAddressReady);
  EXPECT_EQ(controller.linkState().bssid.bytes[5], 2);
  EXPECT_EQ(controller.linkState().address.bytes[2], 7);
  EXPECT_EQ(controller.linkState().station_mac.bytes[0] & 3, 2);
  config.security = AuthMode::kOpen;
  config.ip_mode = IpMode::kDhcp;
  config.mac_policy = MacPolicy::kDevice;
  Status open_request = controller.connect(config, {});
  ASSERT_EQ(open_request, Status::kOk);
  RunBackend(scheduler);
  // The emulator supplies a DHCP lease; switching must replace the static
  // address and complete through the real native IP event.
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t leased = {};
  ASSERT_EQ(esp_netif_get_ip_info(netif, &leased), ESP_OK);
  EXPECT_EQ(leased.ip.addr, Ip(192, 168, 1, 100).addr);
  esp_netif_dhcp_status_t dhcp;
  ASSERT_EQ(esp_netif_dhcpc_get_status(netif, &dhcp), ESP_OK);
  EXPECT_EQ(dhcp, ESP_NETIF_DHCP_STARTED);
  EXPECT_EQ(controller.linkState().phase, LinkPhase::kAddressReady);
  EXPECT_EQ(controller.linkState().address.bytes[2], 1);
  EXPECT_EQ(memcmp(controller.linkState().station_mac.bytes, original_mac, 6),
            0);
  EXPECT_EQ(controller.linkState().bssid.bytes[5], 1);
  controller.removeListener(observer);
}

// Verifies stopping discovery or connection selection leaves the controller
// usable, including after the cancellation deadline has elapsed.
TEST(Esp32BackendTest, CancelScansWithoutFaulting) {
  using namespace roo_testing_transducers::wifi;
  auto environment = std::make_shared<Environment>();
  environment->setScanDurationMs(500);
  FakeEsp32().setWifiEnvironment(environment);
  roo_scheduler::Scheduler scheduler;
  Esp32IdfInterface radio;
  MemoryStore store;
  store.enabled = true;
  Controller::Options options;
  options.transition_timeout_ms = 100;
  Controller controller(radio, store, scheduler, options);
  ASSERT_EQ(controller.begin(), Status::kOk);
  scheduler.delay(roo_time::Millis(50));
  ASSERT_EQ(controller.startScan(), Status::kOk);
  scheduler.delay(roo_time::Millis(20));
  ASSERT_EQ(controller.state().scan, Controller::ScanPhase::kRunning);
  ASSERT_EQ(controller.setEnabled(false), Status::kOk);
  scheduler.delay(roo_time::Millis(200));
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kDisabled);
  ASSERT_EQ(controller.setEnabled(true), Status::kOk);
  scheduler.delay(roo_time::Millis(50));
  ASSERT_EQ(controller.connect(TestConfig("cancel-selection"), {}),
            Status::kOk);
  scheduler.delay(roo_time::Millis(20));
  ASSERT_EQ(controller.state().station, Controller::StationPhase::kConnecting);
  ASSERT_EQ(controller.disconnect(), Status::kOk);
  scheduler.delay(roo_time::Millis(200));
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kIdle);
  EXPECT_EQ(controller.startScan(), Status::kOk);
  scheduler.delay(roo_time::Millis(600));
  EXPECT_EQ(controller.state().scan, Controller::ScanPhase::kIdle);
  EXPECT_EQ(controller.state().scan_status, Status::kOk);
}

// Verifies a second owner cannot attach to the process-global station.
TEST(Esp32BackendTest, ExclusiveOwnership) {
  roo_scheduler::Scheduler scheduler;
  Esp32IdfInterface a, b;
  MemoryStore sa;
  MemoryStore sb;
  Controller first(a, sa, scheduler), second(b, sb, scheduler);
  EXPECT_EQ(first.begin(), Status::kOk);
  EXPECT_EQ(second.begin(), Status::kBusy);
}

// Verifies actual preferences survive close/reopen with small known-key fields.
TEST(Esp32BackendTest, PreferencesReopen) {
  PrefsStore store;
  ASSERT_EQ(store.begin(), Status::kOk);
  ProfileSettings settings;
  settings.connection = TestConfig("persisted");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(0x1234, settings, update), Status::kOk);
  PrefsStore reopened;
  ASSERT_EQ(reopened.begin(), Status::kOk);
  std::vector<ProfileId> ids;
  ASSERT_EQ(reopened.forEachProfile([&](ProfileId id) {
    ids.push_back(id);
    return true;
  }),
            Status::kOk);
  EXPECT_NE(std::find(ids.begin(), ids.end(), 0x1234), ids.end());
  Profile out;
  ASSERT_EQ(reopened.loadProfile(0x1234, out), Status::kOk);
  EXPECT_EQ(out.settings.connection.ssid.size, 9u);
  EXPECT_EQ(reopened.removeProfile(0x1234), Status::kOk);
  EXPECT_EQ(store.loadProfile(0x1234, out), Status::kNotFound);
}

// Verifies profile persistence can use a caller-owned roo_prefs backend.
TEST(Esp32BackendTest, CustomPreferencesBackend) {
  roo_prefs::PreferencesStore backend;
  PrefsStore store(backend);
  ASSERT_EQ(store.begin(), Status::kOk);
  ProfileSettings settings;
  settings.connection = TestConfig("custom-store");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(0x5678, settings, update), Status::kOk);
  Profile out;
  ASSERT_EQ(store.loadProfile(0x5678, out), Status::kOk);
  EXPECT_EQ(out.settings.connection.ssid.size, 12u);
  EXPECT_EQ(store.removeProfile(0x5678), Status::kOk);
}
}  // namespace roo_wifi
