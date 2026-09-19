#include "WiFi.h"
#include "backend_fakes.h"
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
  ASSERT_EQ(controller.begin(), Error::kOk);
  RunBackend(scheduler);
  ASSERT_TRUE(controller.isEnabled());
  ConnectionConfig config = TestConfig("same");
  config.security = AuthMode::kWpa2Personal;
  Credentials secret;
  secret.size = 8;
  memcpy(secret.bytes, "password", 8);
  RequestResult secure_request = controller.connect(config, secret);
  ASSERT_NE(secure_request.id, 0u);
  RunBackend(scheduler);
  ASSERT_FALSE(observer.results.empty());
  EXPECT_EQ(observer.results.back().id, secure_request.id);
  EXPECT_EQ(observer.results.back().error, Error::kOk);
  EXPECT_EQ(controller.linkState().phase, LinkPhase::kAddressReady);
  EXPECT_EQ(controller.linkState().bssid.bytes[5], 2);
  config.security = AuthMode::kOpen;
  RequestResult open_request = controller.connect(config, {});
  ASSERT_NE(open_request.id, 0u);
  RunBackend(scheduler);
  EXPECT_EQ(observer.results.back().id, open_request.id);
  EXPECT_EQ(observer.results.back().error, Error::kOk);
  EXPECT_EQ(controller.linkState().bssid.bytes[5], 1);
  controller.removeListener(observer);
}
// Verifies a second owner cannot attach to the process-global station.
TEST(Esp32BackendTest, ExclusiveOwnership) {
  roo_scheduler::Scheduler scheduler;
  Esp32ArduinoInterface a, b;
  MemoryStore sa, sb;
  Controller first(a, sa, scheduler), second(b, sb, scheduler);
  EXPECT_EQ(first.begin(), Error::kOk);
  EXPECT_EQ(second.begin(), Error::kBusy);
}
// Verifies actual preferences survive close/reopen with small known-key fields.
TEST(Esp32BackendTest, PreferencesReopen) {
  ArduinoPreferencesStore store;
  ASSERT_EQ(store.begin(), Error::kOk);
  ProfileSettings settings;
  settings.connection = TestConfig("persisted");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(0x1234, settings, update).error, Error::kOk);
  ArduinoPreferencesStore reopened;
  ASSERT_EQ(reopened.begin(), Error::kOk);
  Profile out;
  ASSERT_EQ(reopened.loadProfile(0x1234, out), Error::kOk);
  EXPECT_EQ(out.settings.connection.ssid.size, 9);
  EXPECT_EQ(reopened.removeProfile(0x1234), Error::kOk);
  EXPECT_EQ(store.loadProfile(0x1234, out), Error::kNotFound);
}
}  // namespace roo_wifi
