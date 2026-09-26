#include "backend_fakes.h"
#include "gtest/gtest.h"

namespace roo_wifi {
/// Provides initialized controller dependencies and a notification observer.
class BackendTest : public testing::Test {
 protected:
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio{native};
  MemoryStore store;
  Controller controller{radio, store, scheduler};
  Observer observer;

  void SetUp() override {
    store.enabled = true;
    controller.addListener(observer);
    ASSERT_EQ(controller.begin(), Status::kOk);
    Pump(scheduler);
    observer.notifications = 0;
  }

  void TearDown() override { controller.removeListener(observer); }
};

// Verifies accepted work owns its input and succeeds only after address
// readiness.
TEST_F(BackendTest, OwnedInputAndAddressReadiness) {
  ConnectionConfig c = TestConfig();
  ASSERT_EQ(controller.connect(c, {}), Status::kOk);
  c.ssid.bytes[0] = 'X';
  EXPECT_EQ(observer.notifications, 0);
  Pump(scheduler);
  EXPECT_EQ(native.last_config.ssid.bytes[0], 'n');
  native.associated();
  Pump(scheduler);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kAwaitingIp);
  native.ready();
  Pump(scheduler);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kConnected);
  auto identity = controller.linkState().connection_id;
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(controller.linkState().connection_id, identity);
  EXPECT_EQ(controller.linkState().phase, LinkPhase::kIdle);
}

// Verifies switching processes A's queued disconnect before starting B.
TEST_F(BackendTest, OrderedSwitchWithDelayedDispatch) {
  controller.connect(TestConfig("A"), {});
  Pump(scheduler);
  native.associated();
  native.ready();
  Pump(scheduler);
  auto a = controller.linkState().connection_id;
  EXPECT_EQ(controller.connect(TestConfig("B"), {}), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 1);
  EXPECT_EQ(native.disconnects, 1);
  native.ready();
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(native.connects, 2);
  EXPECT_NE(controller.linkState().connection_id, a);
  EXPECT_EQ(controller.linkState().phase, LinkPhase::kConnecting);
  native.associated();
  native.ready();
  Pump(scheduler);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kConnected);
}

// Verifies a replacement connection waits for native teardown before starting.
TEST_F(BackendTest, CancelBeforeAssociationAndSameSsidRetry) {
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  auto a = controller.linkState().connection_id;
  EXPECT_EQ(controller.disconnect(), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(controller.connect(TestConfig(), {}), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 1);
  native.disconnected();
  Pump(scheduler);
  EXPECT_NE(controller.linkState().connection_id, a);
  native.associated();
  native.ready();
  Pump(scheduler);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kConnected);
}

// Disconnect can stop an attempt without the caller retaining its ID.
TEST_F(BackendTest, DisconnectCancelsBeforeNativeStart) {
  ASSERT_EQ(controller.connect(TestConfig(), {}), Status::kOk);
  ASSERT_EQ(controller.disconnect(), Status::kOk);
  EXPECT_EQ(observer.notifications, 0);
  EXPECT_EQ(controller.disconnect(), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 0);
  EXPECT_EQ(native.disconnects, 0);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kIdle);
}

TEST_F(BackendTest, DisconnectAfterQueuedCancellation) {
  controller.connect(TestConfig(), {});
  EXPECT_EQ(controller.disconnect(), Status::kOk);
  EXPECT_EQ(controller.disconnect(), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 0);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kIdle);
}

TEST_F(BackendTest, DisconnectRejectionFaultsController) {
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  native.disconnect_rejection = Status::kConnectionFailed;
  EXPECT_EQ(controller.disconnect(), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(controller.state().status, Status::kConnectionFailed);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kFaulted);
  native.associated();
  native.ready();
  Pump(scheduler);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kFaulted);
  EXPECT_EQ(controller.connect(TestConfig(), {}), Status::kNotStarted);
}

TEST_F(BackendTest, DisconnectCancelsBeforeAssociation) {
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  EXPECT_EQ(controller.disconnect(), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.disconnects, 1);
  EXPECT_EQ(controller.disconnect(), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.disconnects, 1);
  EXPECT_EQ(controller.state().station,
            Controller::StationPhase::kDisconnecting);
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kIdle);
  EXPECT_EQ(controller.connect(TestConfig(), {}), Status::kOk);
}

TEST_F(BackendTest, DisconnectWhileWaitingForIpIgnoresQueuedReadiness) {
  ProfileSettings settings;
  settings.connection = TestConfig();
  settings.auto_connect = true;
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(controller.saveProfile(settings, update), Status::kOk);
  ASSERT_EQ(controller.connect(settings.connection.ssid), Status::kOk);
  Pump(scheduler);
  native.associated();
  Pump(scheduler);
  native.ready();
  ASSERT_EQ(controller.disconnect(), Status::kOk);
  Pump(scheduler);
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kIdle);
  Ssid last;
  EXPECT_EQ(store.readLastProfile(last), Status::kNotFound);
  scheduler.delay(roo_time::Seconds(6));
  Pump(scheduler);
  EXPECT_EQ(native.connects, 1);
}

TEST_F(BackendTest, FollowUpIntentDuringDisconnectNotification) {
  class FollowUp : public Controller::Listener {
   public:
    Controller* controller;
    bool requested = false;
    void onStationStateChanged() override {
      if (!requested && controller->state().station ==
                            Controller::StationPhase::kDisconnecting) {
        requested = true;
        EXPECT_EQ(controller->connect(TestConfig("B"), {}), Status::kOk);
      }
    }
  } listener;
  listener.controller = &controller;
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  controller.addListener(listener);
  controller.disconnect();
  Pump(scheduler);
  EXPECT_TRUE(listener.requested);
  EXPECT_EQ(native.connects, 1);
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(native.connects, 2);
  EXPECT_EQ(native.last_config.ssid.bytes[0], 'B');
  controller.removeListener(listener);
}

TEST_F(BackendTest, SupersedingPendingConnectionDoesNotReviveIt) {
  controller.connect(TestConfig("A"), {});
  Pump(scheduler);
  controller.disconnect();
  Pump(scheduler);
  controller.connect(TestConfig("B"), {});
  controller.disconnect();
  Pump(scheduler);
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(native.connects, 1);
  EXPECT_EQ(native.disconnects, 1);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kIdle);
}

TEST_F(BackendTest, ShutdownStopsConnectionAndNotifications) {
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  controller.disconnect();
  Pump(scheduler);
  auto count = observer.notifications;
  controller.shutdown();
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(observer.notifications, count);
  EXPECT_EQ(controller.disconnect(), Status::kNotStarted);
}

// Verifies profile writes settle synchronously during an asynchronous scan.
TEST_F(BackendTest, SynchronousSaveDuringScan) {
  ASSERT_EQ(controller.startScan(), Status::kOk);
  ProfileSettings p;
  p.connection = TestConfig();
  CredentialUpdate u;
  u.intent = CredentialIntent::kClear;
  ASSERT_EQ(controller.saveProfile(p, u), Status::kOk);
  EXPECT_EQ(observer.notifications, 0);
  Profile out;
  EXPECT_EQ(store.loadProfile(p.connection.ssid, out), Status::kOk);
  Pump(scheduler);
  EXPECT_TRUE(controller.isScanning());
  EXPECT_EQ(controller.state().profiles_generation, 1u);
}

// Verifies failed scans retain the old snapshot and metadata.
TEST_F(BackendTest, SnapshotLifetimeAndMetadata) {
  ScanRecord record;
  record.ssid = TestConfig().ssid;
  record.security = AuthMode::kEnterprise;
  record.bssid.bytes[5] = 42;
  native.aps.push_back(record);
  Status a = controller.startScan();
  Pump(scheduler);
  native.emit({NativeStation::Event::kScanDone});
  Pump(scheduler);
  Controller::ScanSnapshot snapshot = controller.scanSnapshot();
  ASSERT_EQ(snapshot.count, 1u);
  EXPECT_EQ(snapshot.records[0].security, AuthMode::kEnterprise);
  Status b = controller.startScan();
  Pump(scheduler);
  NativeStation::Event event{};
  event.kind = NativeStation::Event::kScanDone;
  event.status = Status::kConnectionFailed;
  native.emit(event);
  Pump(scheduler);
  EXPECT_EQ(a, Status::kOk);
  EXPECT_EQ(b, Status::kOk);
  EXPECT_EQ(controller.scanSnapshot().generation, snapshot.generation);
  EXPECT_EQ(snapshot.records[0].bssid.bytes[5], 42);
}

// Verifies shutdown cancels pending notifications and ignores queued native
// events.
TEST_F(BackendTest, ShutdownNeutralizesQueuedEvents) {
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  native.associated();
  auto count = observer.notifications;
  controller.shutdown();
  Pump(scheduler);
  EXPECT_EQ(observer.notifications, count);
  EXPECT_EQ(controller.startScan(), Status::kNotStarted);
}

// Verifies actual physical state remains observable after persistence failure.
TEST_F(BackendTest, EnablePersistenceFailure) {
  store.enabled_error = Status::kStorageFailure;
  EXPECT_EQ(controller.setEnabled(false), Status::kOk);
  Pump(scheduler);
  EXPECT_FALSE(controller.isEnabled());
  EXPECT_EQ(controller.state().status, Status::kStorageFailure);
}

// Verifies direct temporary connections leave persistence untouched.
TEST_F(BackendTest, TemporaryConnectionDoesNotPersist) {
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  native.associated();
  native.ready();
  Pump(scheduler);
  EXPECT_TRUE(store.values.empty());
}

// Verifies a rejected settings replacement leaves the prior profile readable.
TEST(StoreTest, FailedBlobWritePreservesPreviousProfile) {
  ProfileSettings p;
  p.connection = TestConfig();
  CredentialUpdate u;
  u.intent = CredentialIntent::kClear;
  MemoryStore store;
  ASSERT_EQ(store.saveProfile(p, u), Status::kOk);
  store.fail_at = store.writes + 1;
  p.connection.hidden = true;
  EXPECT_EQ(store.saveProfile(p, u), Status::kStorageFailure);
  Profile out;
  ASSERT_EQ(store.loadProfile(p.connection.ssid, out), Status::kOk);
  EXPECT_FALSE(out.settings.connection.hidden);
  store.fail_at = -1;
  EXPECT_EQ(store.saveProfile(p, u), Status::kOk);
  ASSERT_EQ(store.loadProfile(p.connection.ssid, out), Status::kOk);
  EXPECT_TRUE(out.settings.connection.hidden);
}

// Verifies settings and credentials use separate compact versioned values.
TEST(StoreTest, ProfileAndSecretBlobsRoundTripAllFields) {
  MemoryStore store;
  ProfileSettings settings;
  settings.connection = TestConfig("complete-profile");
  settings.connection.security = AuthMode::kWpa2Personal;
  settings.connection.hidden = true;
  settings.connection.ip_mode = IpMode::kStaticIpv4;
  settings.connection.static_ipv4.address = {{192, 168, 7, 12}};
  settings.connection.static_ipv4.gateway = {{192, 168, 7, 1}};
  settings.connection.static_ipv4.dns1 = {{1, 1, 1, 1}};
  settings.connection.static_ipv4.dns2 = {{8, 8, 8, 8}};
  settings.connection.static_ipv4.prefix_length = 24;
  settings.connection.static_ipv4.has_dns2 = true;
  settings.connection.mac_policy = MacPolicy::kRandomized;
  settings.auto_connect = false;
  CredentialUpdate update;
  update.intent = CredentialIntent::kReplace;
  update.replacement.size = 8;
  memcpy(update.replacement.bytes, "password", 8);

  ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);
  ASSERT_EQ(store.values.size(), 2u);
  EXPECT_NE(store.values.find("p-Qq2GTETDNQY"), store.values.end());
  EXPECT_NE(store.values.find("s-Qq2GTETDNQY"), store.values.end());

  Profile profile;
  ASSERT_EQ(store.loadProfile(settings.connection.ssid, profile), Status::kOk);
  EXPECT_EQ(profile.settings.connection.ssid, settings.connection.ssid);
  EXPECT_TRUE(profile.has_credentials);
  EXPECT_EQ(profile.settings.connection.ssid.size,
            settings.connection.ssid.size);
  EXPECT_EQ(
      memcmp(profile.settings.connection.ssid.bytes,
             settings.connection.ssid.bytes, settings.connection.ssid.size),
      0);
  EXPECT_EQ(profile.settings.connection.security, AuthMode::kWpa2Personal);
  EXPECT_TRUE(profile.settings.connection.hidden);
  EXPECT_EQ(profile.settings.connection.ip_mode, IpMode::kStaticIpv4);
  EXPECT_EQ(profile.settings.connection.static_ipv4.address.bytes[3], 12);
  EXPECT_EQ(profile.settings.connection.static_ipv4.gateway.bytes[3], 1);
  EXPECT_EQ(profile.settings.connection.static_ipv4.dns1.bytes[0], 1);
  EXPECT_EQ(profile.settings.connection.static_ipv4.dns2.bytes[0], 8);
  EXPECT_EQ(profile.settings.connection.static_ipv4.prefix_length, 24);
  EXPECT_TRUE(profile.settings.connection.static_ipv4.has_dns2);
  EXPECT_EQ(profile.settings.connection.mac_policy, MacPolicy::kRandomized);
  EXPECT_FALSE(profile.settings.auto_connect);
  Credentials credentials;
  ASSERT_EQ(store.loadCredentials(settings.connection.ssid, credentials),
            Status::kOk);
  EXPECT_EQ(credentials.size, 8);
  EXPECT_EQ(memcmp(credentials.bytes, "password", 8), 0);

  std::vector<uint8_t> profile_data = store.values["p-Qq2GTETDNQY"];
  int writes = store.writes;
  memcpy(update.replacement.bytes, "new-pass", 8);
  ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);
  EXPECT_EQ(store.writes, writes + 1);
  EXPECT_EQ(store.values["p-Qq2GTETDNQY"], profile_data);
  ASSERT_EQ(store.loadCredentials(settings.connection.ssid, credentials),
            Status::kOk);
  EXPECT_EQ(memcmp(credentials.bytes, "new-pass", 8), 0);

  store.values["p-Qq2GTETDNQY"].push_back(0);
  EXPECT_EQ(store.loadProfile(settings.connection.ssid, profile),
            Status::kCorrupt);
}

// Verifies a failed blob erase leaves the profile intact and can be retried.
TEST(StoreTest, FailedDeleteCleanupAndRetry) {
  MemoryStore store;
  ProfileSettings p;
  p.connection = TestConfig();
  CredentialUpdate u;
  u.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(p, u), Status::kOk);
  store.fail_at = store.writes + 1;
  EXPECT_EQ(store.removeProfile(p.connection.ssid), Status::kStorageFailure);
  Profile out;
  EXPECT_EQ(store.loadProfile(p.connection.ssid, out), Status::kOk);
  store.fail_at = -1;
  ASSERT_EQ(store.writeLastProfile(p.connection.ssid), Status::kOk);
  EXPECT_EQ(store.removeProfile(p.connection.ssid), Status::kOk);
  EXPECT_TRUE(store.values.empty());
  Ssid last;
  EXPECT_EQ(store.readLastProfile(last), Status::kNotFound);
}

// Verifies one configuration per exact SSID, enumeration and early stopping.
TEST(StoreTest, EnumeratesProfiles) {
  MemoryStore store;
  ProfileSettings settings;
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  for (const char* name : {"first", "second", "removed"}) {
    settings.connection = TestConfig(name);
    ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);
  }
  ASSERT_EQ(store.removeProfile(TestConfig("removed").ssid), Status::kOk);
  store.values["s-orphan"] = {0x10};
  store.values["p-00000002"] = {0xff};  // Legacy IDs are ignored.
  std::vector<Ssid> ssids;
  EXPECT_EQ(store.forEachProfile([&](const Ssid& ssid) {
    ssids.push_back(ssid);
    return true;
  }),
            Status::kOk);
  ASSERT_EQ(ssids.size(), 2u);
  EXPECT_NE(std::find(ssids.begin(), ssids.end(), TestConfig("first").ssid),
            ssids.end());
  EXPECT_NE(std::find(ssids.begin(), ssids.end(), TestConfig("second").ssid),
            ssids.end());
  int visits = 0;
  EXPECT_EQ(store.forEachProfile([&](const Ssid&) {
    ++visits;
    return false;
  }),
            Status::kStopped);
  EXPECT_EQ(visits, 1);
  store.enumeration_error = Status::kStorageFailure;
  EXPECT_EQ(store.forEachProfile([](const Ssid&) { return true; }),
            Status::kStorageFailure);
}

// Verifies corrupt settings cannot supply a trusted SSID during enumeration.
TEST(StoreTest, EnumerationAndCorruptProfiles) {
  MemoryStore store;
  ProfileSettings settings;
  settings.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);
  store.values.begin()->second = {0xff};
  EXPECT_EQ(store.forEachProfile([](const Ssid&) { return true; }),
            Status::kCorrupt);
}

// Verifies the controller gates enumeration on its lifecycle and permits
// profile reads from the visitor.
TEST(ProfileEnumerationTest, ControllerFacade) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  Controller controller(radio, store, scheduler);
  EXPECT_EQ(controller.forEachProfile([](Ssid) { return true; }),
            Status::kNotStarted);
  ASSERT_EQ(controller.begin(), Status::kOk);
  Pump(scheduler);

  ProfileSettings settings;
  settings.connection = TestConfig("enumerated");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);
  EXPECT_EQ(controller.forEachProfile([&](Ssid id) {
    Profile profile;
    EXPECT_EQ(controller.loadProfile(id, profile), Status::kOk);
    EXPECT_EQ(profile.settings.connection.ssid, settings.connection.ssid);
    return true;
  }),
            Status::kOk);
}

// Verifies profile invalidation is delivered after persistence has settled, so
// a listener can immediately rebuild its enumeration-derived model.
TEST(ProfileEnumerationTest, ReloadsFromProfilesChanged) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  Controller controller(radio, store, scheduler);
  ASSERT_EQ(controller.begin(), Status::kOk);
  Pump(scheduler);

  class ReloadingListener : public Controller::Listener {
   public:
    explicit ReloadingListener(Controller& controller)
        : controller(controller) {}

    void onProfilesChanged() override {
      ++notifications;
      ids.clear();
      status = controller.forEachProfile([&](Ssid id) {
        ids.push_back(id);
        return true;
      });
    }

    Controller& controller;
    int notifications = 0;
    Status status = Status::kNotStarted;
    std::vector<Ssid> ids;
  } listener(controller);
  controller.addListener(listener);

  ProfileSettings settings;
  settings.connection = TestConfig("notified");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(controller.saveProfile(settings, update), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(listener.notifications, 1);
  EXPECT_EQ(listener.status, Status::kOk);
  EXPECT_EQ(listener.ids, (std::vector<Ssid>{settings.connection.ssid}));

  controller.removeListener(listener);
}

// Verifies Keep retains credentials, and metadata reads never return secret
// bytes.
TEST(StoreTest, ExplicitCredentialIntent) {
  MemoryStore store;
  ProfileSettings p;
  p.connection = TestConfig();
  p.connection.security = AuthMode::kWpa2Personal;
  CredentialUpdate u;
  u.intent = CredentialIntent::kReplace;
  u.replacement.size = 8;
  memcpy(u.replacement.bytes, "password", 8);
  ASSERT_EQ(store.saveProfile(p, u), Status::kOk);
  u.intent = CredentialIntent::kKeep;
  p.connection.hidden = true;
  EXPECT_EQ(store.saveProfile(p, u), Status::kOk);
  Credentials c;
  EXPECT_EQ(store.loadCredentials(p.connection.ssid, c), Status::kOk);
  EXPECT_EQ(c.size, 8u);
  u.intent = CredentialIntent::kClear;
  u.replacement = {};
  EXPECT_EQ(store.saveProfile(p, u), Status::kInvalidArgument);
}

// Verifies static IPv4 and security values are validated independently of a UI.
TEST(ConfigurationTest, InvalidIpAndCredentialEncoding) {
  ConnectionConfig c = TestConfig();
  c.ip_mode = IpMode::kStaticIpv4;
  EXPECT_EQ(Validate(c, {}), Status::kInvalidArgument);
  c.static_ipv4.address = {{192, 168, 1, 2}};
  c.static_ipv4.gateway = {{192, 168, 1, 1}};
  c.static_ipv4.dns1 = {{1, 1, 1, 1}};
  EXPECT_EQ(Validate(c, {}), Status::kOk);
  c.static_ipv4.gateway = {{10, 0, 0, 1}};
  EXPECT_EQ(Validate(c, {}), Status::kInvalidArgument);
  c = TestConfig();
  c.security = AuthMode::kUnknown;
  EXPECT_EQ(Validate(c, {}), Status::kUnsupported);
}
}  // namespace roo_wifi

namespace roo_wifi {
// Verifies an unsettled cancellation produces one Timeout and permanently
// closes radio admission while radio-off profile management remains available.
TEST(TimeoutTest, UnsettledNativeWorkCannotOverlapNewAttempt) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  store.enabled = true;
  Controller::Options options;
  options.connect_timeout_ms = 1;
  options.transition_timeout_ms = 1;
  Controller controller(radio, store, scheduler, options);
  Observer observer;
  controller.addListener(observer);
  controller.begin();
  Pump(scheduler);
  observer.notifications = 0;
  ASSERT_EQ(controller.connect(TestConfig(), {}), Status::kOk);
  Pump(scheduler);
  scheduler.delay(roo_time::Millis(6));
  Pump(scheduler);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kFaulted);

  EXPECT_EQ(controller.state().status, Status::kTimeout);
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kFaulted);
  EXPECT_EQ(controller.connect(TestConfig(), {}), Status::kNotStarted);
  ProfileSettings settings;
  settings.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  EXPECT_EQ(controller.saveProfile(settings, update), Status::kOk);
  Pump(scheduler);
  Profile out;
  EXPECT_EQ(controller.loadProfile(settings.connection.ssid, out), Status::kOk);
  controller.removeListener(observer);
}

TEST(TimeoutTest, DisconnectCancellationHasTransitionDeadline) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  store.enabled = true;
  Controller::Options options;
  options.connect_timeout_ms = 30000;
  options.transition_timeout_ms = 1;
  Controller controller(radio, store, scheduler, options);
  Observer observer;
  controller.addListener(observer);
  ASSERT_EQ(controller.begin(), Status::kOk);
  Pump(scheduler);
  observer.notifications = 0;
  EXPECT_EQ(controller.connect(TestConfig(), {}), Status::kOk);
  Pump(scheduler);
  auto disconnect = controller.disconnect();
  ASSERT_EQ(disconnect, Status::kOk);
  scheduler.delay(roo_time::Millis(6));
  Pump(scheduler);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kFaulted);

  EXPECT_EQ(controller.state().status, Status::kTimeout);

  EXPECT_EQ(controller.state().status, Status::kTimeout);
  EXPECT_EQ(native.disconnects, 1);
  EXPECT_EQ(controller.connect(TestConfig(), {}), Status::kNotStarted);
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kFaulted);
  controller.removeListener(observer);
}

// Verifies timeout monitoring sleeps until the pending operation's deadline.
TEST(TimeoutTest, MonitorsAtDeadlineRatherThanPolling) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  store.enabled = true;
  Controller::Options options;
  options.connect_timeout_ms = 1000;
  Controller controller(radio, store, scheduler, options);
  controller.begin();
  Pump(scheduler);

  ASSERT_EQ(controller.connect(TestConfig(), {}), Status::kOk);
  Pump(scheduler);

  EXPECT_GT(scheduler.getNearestExecutionDelay(), roo_time::Millis(500));
}

// Verifies the last successful open profile is selected after radio enablement.
TEST(StartupTest, LastProfileAndAdmissionSnapshot) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  ProfileSettings settings;
  settings.connection = TestConfig("saved");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);
  ASSERT_EQ(store.writeLastProfile(settings.connection.ssid), Status::kOk);
  Controller controller(radio, store, scheduler);
  ASSERT_EQ(controller.begin(), Status::kOk);
  Pump(scheduler);
  ASSERT_FALSE(controller.isEnabled());
  EXPECT_EQ(native.connects, 0);
  controller.setEnabled(true);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 1);
  EXPECT_EQ(native.last_config.ssid.bytes[0], 's');
  settings.connection = TestConfig("changed");
  controller.saveProfile(settings, update);
  Pump(scheduler);
  EXPECT_EQ(native.last_config.ssid.bytes[0], 's');
}

// Verifies a successful saved-profile connection becomes the restart choice,
// while a temporary connection does not replace it.
TEST_F(BackendTest, RemembersLastSuccessfulSavedProfile) {
  ProfileSettings settings;
  settings.connection = TestConfig("remembered-open");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);

  ASSERT_EQ(controller.connect(settings.connection.ssid), Status::kOk);
  Pump(scheduler);
  native.associated();
  native.ready();
  Pump(scheduler);
  Ssid last;
  EXPECT_EQ(store.readLastProfile(last), Status::kOk);
  EXPECT_EQ(last, settings.connection.ssid);

  ASSERT_EQ(controller.connect(TestConfig("temporary"), {}), Status::kOk);
  Pump(scheduler);
  native.disconnected();
  Pump(scheduler);
  native.associated();
  native.ready();
  Pump(scheduler);
  ASSERT_EQ(store.readLastProfile(last), Status::kOk);
  EXPECT_EQ(last, settings.connection.ssid);
}

// A remembered profile with auto-connect disabled is not started.
TEST(StartupTest, AutoConnectOptOutIncludesOpenProfiles) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  ProfileSettings settings;
  settings.connection = TestConfig("manual-open");
  settings.auto_connect = false;
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);
  ASSERT_EQ(store.writeLastProfile(settings.connection.ssid), Status::kOk);
  store.enabled = true;

  Controller controller(radio, store, scheduler);
  ASSERT_EQ(controller.begin(), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 0);
}

// Verifies successful persistence remains saved after a connection fails.
TEST_F(BackendTest, SavedProfileSurvivesNativeRejection) {
  ProfileSettings settings;
  settings.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  controller.saveProfile(settings, update);
  Pump(scheduler);
  native.rejection = Status::kConnectionFailed;
  Status request = controller.connect(settings.connection.ssid);
  Pump(scheduler);
  EXPECT_EQ(request, Status::kOk);
  EXPECT_EQ(controller.state().status, Status::kConnectionFailed);
  Profile out;
  EXPECT_EQ(controller.loadProfile(settings.connection.ssid, out), Status::kOk);
}

// An auto-connect profile remains eligible when it is temporarily unavailable.
TEST_F(BackendTest, RetriesUnavailableSavedProfile) {
  ProfileSettings settings;
  settings.connection = TestConfig("later-open");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);
  native.rejection = Status::kConnectionFailed;
  ASSERT_EQ(controller.connect(settings.connection.ssid), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 1);

  native.rejection = Status::kOk;
  scheduler.delay(roo_time::Seconds(6));
  Pump(scheduler);
  EXPECT_EQ(native.connects, 2);
}

// Verifies disconnect supersedes a queued connection before native execution.
TEST_F(BackendTest, CancelBeforeNativeStart) {
  ASSERT_EQ(controller.connect(TestConfig(), {}), Status::kOk);
  EXPECT_EQ(controller.disconnect(), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 0);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kIdle);
}

// Verifies bounded event overflow faults radio admission instead of reusing
// lost identity.
TEST_F(BackendTest, NativeHandoffOverflowFailsClosed) {
  ASSERT_EQ(controller.startScan(), Status::kOk);
  Pump(scheduler);
  for (int i = 0; i < 20; ++i) native.emit({NativeStation::Event::kScanDone});
  Pump(scheduler);
  EXPECT_EQ(controller.state().scan_status, Status::kConnectionFailed);
  controller.startScan();
  Pump(scheduler);
  EXPECT_EQ(controller.state().scan_status, Status::kNotStarted);
}
}  // namespace roo_wifi

namespace roo_wifi {
// Verifies a failed settings write is reread to resolve an ambiguous outcome.
TEST(StoreTest, SettingsCommitVerification) {
  /// Persists the settings but reports their write as failed.
  class AmbiguousStore : public MemoryStore {
   public:
    /// Writes the blob but reports failure to its caller.
    Status writeField(const char* key, const uint8_t* data,
                      size_t size) override {
      Status status = MemoryStore::writeField(key, data, size);
      if (std::string(key).substr(0, 2) == "p-") {
        final_written = true;
        return Status::kStorageFailure;
      }
      return status;
    }

    /// Optionally makes final-commit verification unreadable.
    Status readField(const char* key, uint8_t* out,
                     size_t& size) const override {
      if (final_written && unreadable) return Status::kStorageFailure;
      return MemoryStore::readField(key, out, size);
    }

    bool final_written = false;
    bool unreadable = false;
  } store;

  ProfileSettings settings;
  settings.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  EXPECT_EQ(store.saveProfile(settings, update), Status::kOk);
  store.final_written = false;
  store.unreadable = true;
  settings.connection.hidden = true;
  EXPECT_EQ(store.saveProfile(settings, update), Status::kCommitUnknown);
  Profile untouched;
  untouched.settings.connection = TestConfig("untouched");
  EXPECT_EQ(store.loadProfile(settings.connection.ssid, untouched),
            Status::kStorageFailure);
  EXPECT_EQ(untouched.settings.connection.ssid, TestConfig("untouched").ssid);
}
}  // namespace roo_wifi

namespace roo_wifi {
// Verifies a failed second owner cannot detach an already-owned Interface.
TEST(OwnershipTest, FailedBeginDoesNotShutdownExistingOwner) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  store.enabled = true;
  Controller first(radio, store, scheduler), second(radio, store, scheduler);
  ASSERT_EQ(first.begin(), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(second.begin(), Status::kBusy);
  EXPECT_EQ(first.connect(TestConfig(), {}), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 1);
}
}  // namespace roo_wifi

namespace roo_wifi {
// Verifies hash keys have a stable encoding and distinguish case, length,
// embedded zero bytes, and the maximum SSID length without truncation.
TEST(StoreTest, ExactSsidIdentityAndStableKeys) {
  MemoryStore store;
  ProfileSettings settings;
  settings.connection = TestConfig("hello");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);
  EXPECT_EQ(store.values.count("p-pDDYRoCqvQs"), 1u);
  std::vector<Ssid> names = {TestConfig("Hello").ssid, TestConfig("hello").ssid,
                             TestConfig("hello!").ssid};
  Ssid binary = TestConfig("hello").ssid;
  binary.bytes[binary.size++] = 0;
  names.push_back(binary);
  Ssid longest;
  longest.size = 32;
  memset(longest.bytes, 0xff, 32);
  names.push_back(longest);
  for (const Ssid& ssid : names) {
    settings.connection.ssid = ssid;
    ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);
    Profile profile;
    ASSERT_EQ(store.loadProfile(ssid, profile), Status::kOk);
    EXPECT_EQ(profile.settings.connection.ssid, ssid);
    ASSERT_EQ(store.writeLastProfile(ssid), Status::kOk);
    Ssid last;
    ASSERT_EQ(store.readLastProfile(last), Status::kOk);
    EXPECT_EQ(last, ssid);
  }
  size_t count = 0;
  EXPECT_EQ(store.forEachProfile([&](const Ssid&) {
    ++count;
    return true;
  }),
            Status::kOk);
  EXPECT_EQ(count, names.size());
  for (const auto& entry : store.values) {
    if (entry.first == "last-ssid") continue;
    EXPECT_EQ(entry.first.size(), 13u);
  }
  settings.connection.ssid.size = 33;
  EXPECT_EQ(store.saveProfile(settings, update), Status::kInvalidArgument);
  Profile profile;
  EXPECT_EQ(store.loadProfile(settings.connection.ssid, profile),
            Status::kInvalidArgument);
  EXPECT_EQ(store.removeProfile(settings.connection.ssid),
            Status::kInvalidArgument);
  EXPECT_EQ(store.writeLastProfile(settings.connection.ssid),
            Status::kInvalidArgument);
  EXPECT_EQ(store.loadProfile({}, profile), Status::kInvalidArgument);
}

// Verifies colliding settings and orphaned secrets cannot be read, overwritten,
// or deleted under another SSID. Injecting a blob at another key simulates a
// hash collision without weakening the production hashing algorithm.
TEST(StoreTest, CollisionGuardsAllOperations) {
  MemoryStore source;
  ProfileSettings settings;
  settings.connection = TestConfig("owner");
  settings.connection.security = AuthMode::kWpa2Personal;
  CredentialUpdate update;
  update.intent = CredentialIntent::kReplace;
  update.replacement.size = 8;
  memcpy(update.replacement.bytes, "password", 8);
  ASSERT_EQ(source.saveProfile(settings, update), Status::kOk);
  const auto owner = source.values;
  settings.connection.ssid = TestConfig("other").ssid;
  MemoryStore target;
  ASSERT_EQ(target.saveProfile(settings, update), Status::kOk);
  const auto target_keys = target.values;
  for (bool orphan : {false, true}) {
    target.values.clear();
    for (const auto& entry : target_keys) {
      if (orphan && entry.first[0] == 'p') continue;
      for (const auto& stored : owner) {
        if (stored.first[0] == entry.first[0]) {
          target.values[entry.first] = stored.second;
        }
      }
    }
    const auto before = target.values;
    const int writes = target.writes;
    Profile profile;
    profile.settings.connection = TestConfig("untouched");
    Credentials secret;
    secret.size = 1;
    EXPECT_EQ(target.loadProfile(settings.connection.ssid, profile),
              orphan ? Status::kNotFound : Status::kHashCollision);
    EXPECT_EQ(profile.settings.connection.ssid, TestConfig("untouched").ssid);
    EXPECT_EQ(target.loadCredentials(settings.connection.ssid, secret),
              orphan ? Status::kNotFound : Status::kHashCollision);
    EXPECT_EQ(secret.size, 1u);
    for (CredentialIntent intent :
         {CredentialIntent::kReplace, CredentialIntent::kKeep,
          CredentialIntent::kClear}) {
      update.intent = intent;
      EXPECT_EQ(target.saveProfile(settings, update), Status::kHashCollision);
    }
    EXPECT_EQ(target.removeProfile(settings.connection.ssid),
              Status::kHashCollision);
    EXPECT_EQ(target.values, before);
    EXPECT_EQ(target.writes, writes);
  }
  // A matching settings record must not authorize a mismatched secret.
  target.values = target_keys;
  for (const auto& entry : target_keys) {
    if (entry.first[0] != 's') continue;
    for (const auto& stored : owner) {
      if (stored.first[0] == 's') target.values[entry.first] = stored.second;
    }
  }
  Credentials secret;
  EXPECT_EQ(target.loadCredentials(settings.connection.ssid, secret),
            Status::kHashCollision);
}
}  // namespace roo_wifi

namespace roo_wifi {
// Verifies maximum-size secrets and SSIDs fit the field buffers, and an
// interrupted delete can clean up the remaining owned secret on retry.
TEST(StoreTest, MaximumBlobsAndPartialDelete) {
  MemoryStore store;
  ProfileSettings settings;
  settings.connection = TestConfig("12345678901234567890123456789012");
  settings.connection.security = AuthMode::kWpa2Personal;
  CredentialUpdate update;
  update.intent = CredentialIntent::kReplace;
  update.replacement.encoding = CredentialEncoding::kRawPsk;
  update.replacement.size = 64;
  memset(update.replacement.bytes, 'a', 64);
  ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);
  ASSERT_EQ(store.values.size(), 2u);
  for (const auto& entry : store.values) {
    EXPECT_EQ(entry.second.size(), entry.first[0] == 'p' ? 61u : 104u);
  }
  Credentials secret;
  ASSERT_EQ(store.loadCredentials(settings.connection.ssid, secret),
            Status::kOk);
  EXPECT_EQ(secret.size, 64u);
  EXPECT_EQ(memcmp(secret.bytes, update.replacement.bytes, 64), 0);
  store.fail_at = store.writes + 2;
  EXPECT_EQ(store.removeProfile(settings.connection.ssid),
            Status::kStorageFailure);
  ASSERT_EQ(store.values.size(), 1u);
  EXPECT_EQ(store.values.begin()->first[0], 's');
  EXPECT_EQ(store.removeProfile(settings.connection.ssid), Status::kOk);
  EXPECT_TRUE(store.values.empty());
}

// Verifies the last selection rejects malformed lengths without changing the
// caller's output, and legacy ID selections are ignored.
TEST(StoreTest, LastSsidValidationAndLegacySelection) {
  MemoryStore store;
  store.values["last"] = {0, 0, 0, 7};
  Ssid out = TestConfig("unchanged").ssid;
  EXPECT_EQ(store.readLastProfile(out), Status::kNotFound);
  for (const auto& bytes : std::vector<std::vector<uint8_t>>{
           {}, {0}, {2, 'a'}, {1, 'a', 'b'}, {33}}) {
    store.values["last-ssid"] = bytes;
    EXPECT_EQ(store.readLastProfile(out), Status::kCorrupt);
    EXPECT_EQ(out, TestConfig("unchanged").ssid);
  }
  EXPECT_EQ(store.writeLastProfile({}), Status::kOk);
  EXPECT_EQ(store.readLastProfile(out), Status::kNotFound);
}
}  // namespace roo_wifi

namespace roo_wifi {
// Verifies enumeration preserves const visitors and invokes mutable visitors
// without copying their state or removing their const qualification.
TEST(StoreTest, EnumerationPreservesVisitorQualification) {
  MemoryStore store;
  ProfileSettings settings;
  settings.connection = TestConfig("callback");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(settings, update), Status::kOk);
  int visits = 0;
  const auto immutable = [&visits](const Ssid&) {
    ++visits;
    return true;
  };
  EXPECT_EQ(store.forEachProfile(immutable), Status::kOk);
  auto mutable_visitor = [count = 0, &visits](const Ssid&) mutable {
    visits = ++count;
    return true;
  };
  EXPECT_EQ(store.forEachProfile(mutable_visitor), Status::kOk);
  EXPECT_EQ(visits, 1);
  EXPECT_EQ(store.forEachProfile(mutable_visitor), Status::kOk);
  EXPECT_EQ(visits, 2);
}
}  // namespace roo_wifi
