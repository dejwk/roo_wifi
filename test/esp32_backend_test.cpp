#include "WiFi.h"
#include "backend_fakes.h"
#include "esp_netif.h"
#include "gtest/gtest.h"
#include "roo_testing/microcontrollers/esp32/fake_esp32.h"
#include "roo_testing/transducers/wifi/wifi.h"
#include "roo_wifi/hal/esp32/arduino_preferences_store.h"
#include "roo_wifi/hal/esp32/esp32_arduino_interface.h"

namespace roo_wifi {
namespace {
void RunBackend(roo_scheduler::Scheduler& scheduler) {
  for (int i = 0; i < 1000; ++i) {
    scheduler.executeEligibleTasks();
    delay(1);
  }
}
}  // namespace

// Verifies the production radio selects exact security among same-SSID APs,
// completes only with an address, and switches through the old disconnect.
TEST(Esp32BackendTest, SecuritySelectionAndSwitch) {
  using namespace roo_testing_transducers::wifi;
  static Environment environment;
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
  Esp32ArduinoInterface radio;
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
  Controller::RequestResult secure_request = controller.connect(config, secret);
  ASSERT_NE(secure_request.id, 0u);
  RunBackend(scheduler);
  ASSERT_FALSE(observer.results.empty());
  EXPECT_EQ(observer.results.back().id, secure_request.id);
  EXPECT_EQ(observer.results.back().error, Status::kOk);
  EXPECT_EQ(controller.linkState().phase, LinkPhase::kAddressReady);
  EXPECT_EQ(controller.linkState().bssid.bytes[5], 2);
  EXPECT_EQ(controller.linkState().address.bytes[2], 7);
  EXPECT_EQ(controller.linkState().station_mac.bytes[0] & 3, 2);
  config.security = AuthMode::kOpen;
  config.ip_mode = IpMode::kDhcp;
  config.mac_policy = MacPolicy::kDevice;
  Controller::RequestResult open_request = controller.connect(config, {});
  ASSERT_NE(open_request.id, 0u);
  RunBackend(scheduler);
  // The shim does not implement a DHCP server. Verify cleared static settings,
  // then deliver an explicit lease event through the real native event source.
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t cleared = {};
  ASSERT_EQ(esp_netif_get_ip_info(netif, &cleared), ESP_OK);
  EXPECT_EQ(cleared.ip.addr, 0u);
  esp_netif_dhcp_status_t dhcp;
  ASSERT_EQ(esp_netif_dhcpc_get_status(netif, &dhcp), ESP_OK);
  EXPECT_EQ(dhcp, ESP_NETIF_DHCP_STARTED);
  EXPECT_EQ(controller.linkState().phase, LinkPhase::kAssociated);
  ip_event_got_ip_t lease = {};
  lease.esp_netif = netif;
  lease.ip_info.ip.addr = uint32_t(IPAddress(192, 168, 1, 100));
  lease.ip_info.gw.addr = uint32_t(IPAddress(192, 168, 1, 1));
  lease.ip_info.netmask.addr = uint32_t(IPAddress(255, 255, 255, 0));
  ASSERT_EQ(esp_netif_set_ip_info(netif, &lease.ip_info), ESP_OK);
  ASSERT_EQ(esp_event_post(IP_EVENT, IP_EVENT_STA_GOT_IP, &lease, sizeof(lease),
                           portMAX_DELAY),
            ESP_OK);
  Pump(scheduler);
  EXPECT_EQ(memcmp(controller.linkState().station_mac.bytes, original_mac, 6),
            0);
  EXPECT_EQ(observer.results.back().id, open_request.id);
  EXPECT_EQ(observer.results.back().error, Status::kOk);
  EXPECT_EQ(controller.linkState().bssid.bytes[5], 1);
  controller.removeListener(observer);
}

// Verifies a second owner cannot attach to the process-global station.
TEST(Esp32BackendTest, ExclusiveOwnership) {
  roo_scheduler::Scheduler scheduler;
  Esp32ArduinoInterface a, b;
  MemoryStore sa;
  MemoryStore sb;
  Controller first(a, sa, scheduler), second(b, sb, scheduler);
  EXPECT_EQ(first.begin(), Status::kOk);
  EXPECT_EQ(second.begin(), Status::kBusy);
}

// Verifies actual preferences survive close/reopen with small known-key fields.
TEST(Esp32BackendTest, PreferencesReopen) {
  ArduinoPreferencesStore store;
  ASSERT_EQ(store.begin(), Status::kOk);
  ProfileSettings settings;
  settings.connection = TestConfig("persisted");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(0x1234, settings, update), Status::kOk);
  ArduinoPreferencesStore reopened;
  ASSERT_EQ(reopened.begin(), Status::kOk);
  Profile out;
  ASSERT_EQ(reopened.loadProfile(0x1234, out), Status::kOk);
  EXPECT_EQ(out.settings.connection.ssid.size, 9u);
  EXPECT_EQ(reopened.removeProfile(0x1234), Status::kOk);
  EXPECT_EQ(store.loadProfile(0x1234, out), Status::kNotFound);
}
}  // namespace roo_wifi
