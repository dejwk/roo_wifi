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
static_assert(std::is_constructible<Esp32WiFi, roo_scheduler::SchedulerClient&,
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
void RunBackend(roo_scheduler::SchedulingService& scheduler) {
  for (int i = 0; i < 1000; ++i) {
    scheduler.executeEligibleTasks();
    delay(1);
  }
}
}  // namespace

// Exercise event delivery while the application sleeps inside its scheduler.
TEST(Esp32BackendTest, EnableWhileSchedulerWaits) {
  roo_scheduler::SchedulingService scheduler;
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

// Verifies enabling Wi-Fi repairs inherited LR-only mode while preserving
// protocol selections that already support ordinary access points.
TEST(Esp32BackendTest, EnableReplacesLongRangeOnlyProtocol) {
  roo_scheduler::SchedulingService scheduler;
  Esp32IdfInterface radio;
  MemoryStore store;
  Controller controller(radio, store, scheduler);
  ASSERT_EQ(controller.begin(), Status::kOk);
  scheduler.delay(roo_time::Millis(50));
  const uint8_t standard =
      WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;
  for (uint8_t initial :
       {static_cast<uint8_t>(WIFI_PROTOCOL_LR),
        static_cast<uint8_t>(WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N),
        static_cast<uint8_t>(standard | WIFI_PROTOCOL_LR)}) {
    // Model the protocol bitmap restored by IDF from an earlier application.
    ASSERT_EQ(esp_wifi_set_protocol(WIFI_IF_STA, initial), ESP_OK);
    ASSERT_EQ(controller.setEnabled(true), Status::kOk);
    scheduler.delay(roo_time::Millis(50));
    ASSERT_TRUE(controller.isEnabled());
    uint8_t actual = 0;
    ASSERT_EQ(esp_wifi_get_protocol(WIFI_IF_STA, &actual), ESP_OK);
    EXPECT_EQ(actual, initial == WIFI_PROTOCOL_LR ? standard : initial);
    ASSERT_EQ(controller.setEnabled(false), Status::kOk);
    scheduler.delay(roo_time::Millis(50));
    ASSERT_FALSE(controller.isEnabled());
  }
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
  roo_scheduler::SchedulingService scheduler;
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
  roo_scheduler::SchedulingService scheduler;
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

struct NegotiationCase {
  AuthMode configured;
  wifi_auth_mode_t advertised;
  wifi_auth_mode_t negotiated;
  bool accepted;
  wifi_auth_mode_t minimum;
};
class Esp32NegotiationTest : public ::testing::TestWithParam<NegotiationCase> {
};

// Verifies native thresholds permit every allowed constituent of a mixed
// profile while the backend still rejects incompatible negotiated security.
TEST_P(Esp32NegotiationTest, AcceptsOnlyCompatibleNegotiation) {
  const auto c = GetParam();
  using namespace roo_testing_transducers::wifi;
  auto environment = std::make_shared<Environment>();
  environment->setScanDurationMs(20);
  ConnectionAttempt attempt;
  attempt.negotiated_auth_mode =
      static_cast<roo_testing_transducers::wifi::AuthMode>(c.negotiated);
  environment->queueConnectionAttempt(attempt);
  auto ap = std::make_unique<AccessPoint>(
      roo_testing_transducers::wifi::MacAddress(2, 0, 0, 0, 1, 1), "mixed");
  ap->setAuthMode(
      static_cast<roo_testing_transducers::wifi::AuthMode>(c.advertised));
  ap->setPasswd("password");
  environment->addAccessPoint(std::move(ap));
  FakeEsp32().setWifiEnvironment(environment);
  roo_scheduler::SchedulingService scheduler;
  Esp32IdfInterface radio;
  MemoryStore store;
  store.enabled = true;
  Controller controller(radio, store, scheduler);
  ASSERT_EQ(controller.begin(), Status::kOk);
  RunBackend(scheduler);
  auto config = TestConfig("mixed");
  config.security = c.configured;
  Credentials secret;
  secret.size = 8;
  memcpy(secret.bytes, "password", 8);
  ProfileSettings settings;
  settings.connection = config;
  CredentialUpdate update;
  update.intent = CredentialIntent::kReplace;
  update.replacement = secret;
  ASSERT_EQ(controller.saveProfile(settings, update), Status::kOk);
  ASSERT_EQ(controller.connect(settings.connection.ssid), Status::kOk);
  RunBackend(scheduler);
  wifi_config_t native_config = {};
  ASSERT_EQ(esp_wifi_get_config(WIFI_IF_STA, &native_config), ESP_OK);
  EXPECT_EQ(native_config.sta.sae_pwe_h2e, WPA3_SAE_PWE_BOTH);
  EXPECT_EQ(native_config.sta.threshold.authmode, c.minimum);
  Profile persisted;
  ASSERT_EQ(controller.loadProfile(settings.connection.ssid, persisted),
            Status::kOk);
  EXPECT_EQ(persisted.settings.connection.security, c.configured);
  if (c.accepted) {
    EXPECT_EQ(controller.linkState().phase, LinkPhase::kAddressReady);
    auto expected = c.configured;
    if (c.negotiated == WIFI_AUTH_WPA3_PSK) {
      expected = roo_wifi::AuthMode::kWpa3Personal;
    }
    if (c.negotiated == WIFI_AUTH_WPA2_PSK) {
      expected = roo_wifi::AuthMode::kWpa2Personal;
    }
    if (c.negotiated == WIFI_AUTH_WPA_PSK) {
      expected = roo_wifi::AuthMode::kWpaPersonal;
    }
    EXPECT_EQ(controller.linkState().security, expected);
  } else {
    EXPECT_EQ(controller.linkState().phase, LinkPhase::kIdle);
    EXPECT_EQ(controller.state().status, Status::kConnectionFailed);
  }
}

INSTANTIATE_TEST_SUITE_P(
    MixedAndStrictModes, Esp32NegotiationTest,
    ::testing::Values(
        NegotiationCase{AuthMode::kWpa2Wpa3Personal, WIFI_AUTH_WPA3_PSK,
                        WIFI_AUTH_WPA3_PSK, true, WIFI_AUTH_WPA3_PSK},
        NegotiationCase{AuthMode::kWpa2Wpa3Personal, WIFI_AUTH_WPA2_PSK,
                        WIFI_AUTH_WPA2_PSK, true, WIFI_AUTH_WPA2_PSK},
        NegotiationCase{AuthMode::kWpaWpa2Personal, WIFI_AUTH_WPA2_PSK,
                        WIFI_AUTH_WPA2_PSK, true, WIFI_AUTH_WPA2_PSK},
        NegotiationCase{AuthMode::kWpa2Wpa3Personal, WIFI_AUTH_WPA2_WPA3_PSK,
                        WIFI_AUTH_WPA3_PSK, true, WIFI_AUTH_WPA2_PSK},
        NegotiationCase{AuthMode::kWpa2Wpa3Personal, WIFI_AUTH_WPA2_WPA3_PSK,
                        WIFI_AUTH_WPA2_PSK, true, WIFI_AUTH_WPA2_PSK},
        NegotiationCase{AuthMode::kWpa2Wpa3Personal, WIFI_AUTH_WPA2_WPA3_PSK,
                        WIFI_AUTH_WPA2_WPA3_PSK, true, WIFI_AUTH_WPA2_PSK},
        NegotiationCase{AuthMode::kWpaWpa2Personal, WIFI_AUTH_WPA_WPA2_PSK,
                        WIFI_AUTH_WPA_PSK, true, WIFI_AUTH_WPA_PSK},
        NegotiationCase{AuthMode::kWpaWpa2Personal, WIFI_AUTH_WPA_WPA2_PSK,
                        WIFI_AUTH_WPA2_PSK, true, WIFI_AUTH_WPA_PSK},
        NegotiationCase{AuthMode::kWpa2Wpa3Personal, WIFI_AUTH_WPA2_WPA3_PSK,
                        WIFI_AUTH_OPEN, false, WIFI_AUTH_WPA2_PSK},
        NegotiationCase{AuthMode::kWpa2Wpa3Personal, WIFI_AUTH_WPA2_WPA3_PSK,
                        WIFI_AUTH_WPA_PSK, false, WIFI_AUTH_WPA2_PSK},
        NegotiationCase{AuthMode::kWpa3Personal, WIFI_AUTH_WPA3_PSK,
                        WIFI_AUTH_WPA2_PSK, false, WIFI_AUTH_WPA3_PSK}));

// Verifies a second owner cannot attach to the process-global station.
TEST(Esp32BackendTest, ExclusiveOwnership) {
  roo_scheduler::SchedulingService scheduler;
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
  ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);
  PrefsStore reopened;
  ASSERT_EQ(reopened.begin(), Status::kOk);
  std::vector<Ssid> ids;
  ASSERT_EQ(reopened.forEachProfile([&](Ssid id) {
    ids.push_back(id);
    return true;
  }),
            Status::kOk);
  EXPECT_NE(std::find(ids.begin(), ids.end(), settings.connection.ssid),
            ids.end());
  Profile out;
  ASSERT_EQ(reopened.loadProfile(settings.connection.ssid, out), Status::kOk);
  EXPECT_EQ(out.settings.connection.ssid.size, 9u);
  EXPECT_EQ(reopened.removeProfile(settings.connection.ssid), Status::kOk);
  EXPECT_EQ(store.loadProfile(settings.connection.ssid, out),
            Status::kNotFound);
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
  ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);
  Profile out;
  ASSERT_EQ(store.loadProfile(settings.connection.ssid, out), Status::kOk);
  EXPECT_EQ(out.settings.connection.ssid.size, 12u);
  EXPECT_EQ(store.removeProfile(settings.connection.ssid), Status::kOk);
}
// Verifies actual IDF event translation distinguishes missing networks,
// ambiguous authentication failures, incompatible security, and transient loss.
TEST(Esp32BackendTest, DisconnectFailureCategories) {
  class Receiver : public NativeStation::Receiver {
   public:
    void post(const NativeStation::Event& event) override {
      roo::lock_guard<roo::mutex> lock(mutex);
      last = event;
    }
    NativeStation::Event snapshot() {
      roo::lock_guard<roo::mutex> lock(mutex);
      return last;
    }
    roo::mutex mutex;
    NativeStation::Event last{NativeStation::Event::kEnabled};
  } receiver;
  Esp32Station station;
  ASSERT_EQ(station.attach(receiver), Status::kOk);
  struct Case {
    uint16_t reason;
    ConnectionFailure failure;
  };
  const Case cases[] = {
      {WIFI_REASON_NO_AP_FOUND, ConnectionFailure::kNetworkNotFound},
      {WIFI_REASON_AUTH_FAIL, ConnectionFailure::kAuthentication},
      {WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, ConnectionFailure::kAuthentication},
      {WIFI_REASON_HANDSHAKE_TIMEOUT, ConnectionFailure::kAuthentication},
      {WIFI_REASON_INVALID_PMKID, ConnectionFailure::kAuthentication},
      {WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY,
       ConnectionFailure::kIncompatibleSecurity},
      {WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD,
       ConnectionFailure::kIncompatibleSecurity},
      {WIFI_REASON_CIPHER_SUITE_REJECTED,
       ConnectionFailure::kIncompatibleSecurity},
      {WIFI_REASON_BEACON_TIMEOUT, ConnectionFailure::kTransient},
      {WIFI_REASON_AUTH_EXPIRE, ConnectionFailure::kTransient},
      {WIFI_REASON_ASSOC_FAIL, ConnectionFailure::kTransient}};
  for (const Case& c : cases) {
    wifi_event_sta_disconnected_t event = {};
    event.reason = c.reason;
    event.rssi = -61;
    ASSERT_EQ(esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event,
                             sizeof(event), portMAX_DELAY),
              ESP_OK);
    delay(30);
    const NativeStation::Event actual = receiver.snapshot();
    EXPECT_EQ(actual.link.failure, c.failure) << c.reason;
    EXPECT_EQ(actual.native_code, c.reason);
    EXPECT_EQ(actual.link.rssi_dbm, -61);
  }
  station.detach();
}

}  // namespace roo_wifi
