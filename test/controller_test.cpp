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
    observer.results.clear();
  }

  void TearDown() override { controller.removeListener(observer); }
};

// Verifies accepted work owns its input and succeeds only after address
// readiness.
TEST_F(BackendTest, OwnedInputAndAddressReadiness) {
  ConnectionConfig c = TestConfig();
  Controller::RequestResult r = controller.connect(c, {});
  ASSERT_NE(r.id, 0u);
  c.ssid.bytes[0] = 'X';
  EXPECT_TRUE(observer.results.empty());
  Pump(scheduler);
  EXPECT_EQ(native.last_config.ssid.bytes[0], 'n');
  native.associated();
  Pump(scheduler);
  EXPECT_TRUE(observer.results.empty());
  native.ready();
  Pump(scheduler);
  ASSERT_EQ(observer.results.size(), 1u);
  EXPECT_EQ(observer.results[0].id, r.id);
  EXPECT_EQ(observer.results[0].status, Status::kOk);
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(observer.results.size(), 1u);
  EXPECT_EQ(controller.linkState().connection_id, r.id);
}

// Verifies switching processes A's queued disconnect before starting B.
TEST_F(BackendTest, OrderedSwitchWithDelayedDispatch) {
  Controller::RequestResult a = controller.connect(TestConfig("A"), {});
  Pump(scheduler);
  native.associated();
  native.ready();
  Pump(scheduler);
  Controller::RequestResult b = controller.connect(TestConfig("B"), {});
  Pump(scheduler);
  EXPECT_EQ(native.connects, 1);
  EXPECT_EQ(native.disconnects, 1);
  native.ready();
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(native.connects, 2);
  EXPECT_EQ(controller.linkState().connection_id, b.id);
  EXPECT_EQ(controller.linkState().phase, LinkPhase::kConnecting);
  native.associated();
  native.ready();
  Pump(scheduler);
  ASSERT_EQ(observer.results.size(), 2u);
  EXPECT_EQ(observer.results[0].id, a.id);
  EXPECT_EQ(observer.results[1].id, b.id);
}

// Verifies cancellation waits for the native lifecycle before allowing a retry.
TEST_F(BackendTest, CancelBeforeAssociationAndSameSsidRetry) {
  Controller::RequestResult a = controller.connect(TestConfig(), {});
  Pump(scheduler);
  EXPECT_EQ(controller.cancel(a.id), Status::kOk);
  EXPECT_EQ(controller.connect(TestConfig(), {}).status, Status::kBusy);
  Pump(scheduler);
  EXPECT_TRUE(observer.results.empty());
  native.disconnected();
  Pump(scheduler);
  ASSERT_EQ(observer.results.size(), 1u);
  EXPECT_EQ(observer.results[0].status, Status::kCancelled);
  Controller::RequestResult b = controller.connect(TestConfig(), {});
  EXPECT_NE(b.id, a.id);
  Pump(scheduler);
  native.associated();
  native.ready();
  Pump(scheduler);
  ASSERT_EQ(observer.results.size(), 2u);
  EXPECT_EQ(observer.results.back().id, b.id);
  EXPECT_EQ(controller.cancel(a.id), Status::kNotFound);
}

// Verifies profile work is independent of radio work and cancellation is
// deferred.
TEST_F(BackendTest, IndependentSlotsAndCancelledSave) {
  Controller::RequestResult scan = controller.scan();
  ProfileSettings p;
  p.connection = TestConfig();
  CredentialUpdate u;
  u.intent = CredentialIntent::kClear;
  Controller::RequestResult save = controller.saveProfile(42, p, u);
  ASSERT_NE(save.id, 0u);
  ASSERT_NE(scan.id, 0u);
  EXPECT_EQ(controller.cancel(save.id), Status::kOk);
  EXPECT_TRUE(observer.results.empty());
  Pump(scheduler);
  Profile out;
  EXPECT_EQ(store.loadProfile(42, out), Status::kNotFound);
  ASSERT_EQ(observer.results.size(), 1u);
  EXPECT_EQ(observer.results[0].status, Status::kCancelled);
}

// Verifies failed scans retain the old snapshot and repeated scans get new IDs.
TEST_F(BackendTest, SnapshotLifetimeAndMetadata) {
  ScanRecord record;
  record.ssid = TestConfig().ssid;
  record.security = AuthMode::kEnterprise;
  record.bssid.bytes[5] = 42;
  native.aps.push_back(record);
  Controller::RequestResult a = controller.scan();
  Pump(scheduler);
  native.emit({NativeStation::Event::kScanDone});
  Pump(scheduler);
  Controller::ScanSnapshot snapshot = controller.scanSnapshot();
  ASSERT_EQ(snapshot.count, 1u);
  EXPECT_EQ(snapshot.records[0].security, AuthMode::kEnterprise);
  Controller::RequestResult b = controller.scan();
  Pump(scheduler);
  NativeStation::Event event{};
  event.kind = NativeStation::Event::kScanDone;
  event.status = Status::kConnectionFailed;
  native.emit(event);
  Pump(scheduler);
  EXPECT_NE(a.id, b.id);
  EXPECT_EQ(controller.scanSnapshot().generation, snapshot.generation);
  EXPECT_EQ(snapshot.records[0].bssid.bytes[5], 42);
}

// Verifies explicit shutdown settles public work and queued native delivery is
// inert.
TEST_F(BackendTest, ShutdownNeutralizesQueuedEvents) {
  Controller::RequestResult r = controller.connect(TestConfig(), {});
  Pump(scheduler);
  native.associated();
  controller.shutdown();
  Pump(scheduler);
  ASSERT_EQ(observer.results.size(), 1u);
  EXPECT_EQ(observer.results[0].id, r.id);
  EXPECT_EQ(observer.results[0].status, Status::kCancelled);
  EXPECT_EQ(controller.scan().status, Status::kNotStarted);
}

// Verifies actual physical state remains observable after persistence failure.
TEST_F(BackendTest, EnablePersistenceFailure) {
  store.enabled_error = Status::kStorageFailure;
  Controller::RequestResult r = controller.setEnabled(false);
  Pump(scheduler);
  EXPECT_FALSE(controller.isEnabled());
  ASSERT_EQ(observer.results.size(), 1u);
  EXPECT_EQ(observer.results[0].id, r.id);
  EXPECT_EQ(observer.results[0].status, Status::kStorageFailure);
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

// A rejected settings replacement leaves the previous profile readable.
TEST(StoreTest, FailedBlobWritePreservesPreviousProfile) {
  ProfileSettings p;
  p.connection = TestConfig();
  CredentialUpdate u;
  u.intent = CredentialIntent::kClear;
  MemoryStore store;
  ASSERT_EQ(store.saveProfile(1, p, u), Status::kOk);
  store.fail_at = store.writes + 1;
  p.connection.hidden = true;
  EXPECT_EQ(store.saveProfile(1, p, u), Status::kStorageFailure);
  Profile out;
  ASSERT_EQ(store.loadProfile(1, out), Status::kOk);
  EXPECT_FALSE(out.settings.connection.hidden);
  store.fail_at = -1;
  EXPECT_EQ(store.saveProfile(1, p, u), Status::kOk);
  ASSERT_EQ(store.loadProfile(1, out), Status::kOk);
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

  ASSERT_EQ(store.saveProfile(0x1234, settings, update), Status::kOk);
  ASSERT_EQ(store.values.size(), 2u);
  EXPECT_NE(store.values.find("p-00001234"), store.values.end());
  EXPECT_NE(store.values.find("s-00001234"), store.values.end());

  Profile profile;
  ASSERT_EQ(store.loadProfile(0x1234, profile), Status::kOk);
  EXPECT_EQ(profile.id, 0x1234u);
  EXPECT_TRUE(profile.has_credentials);
  EXPECT_EQ(profile.settings.connection.ssid.size,
            settings.connection.ssid.size);
  EXPECT_EQ(memcmp(profile.settings.connection.ssid.bytes,
                   settings.connection.ssid.bytes,
                   settings.connection.ssid.size),
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
  ASSERT_EQ(store.loadCredentials(0x1234, credentials), Status::kOk);
  EXPECT_EQ(credentials.size, 8);
  EXPECT_EQ(memcmp(credentials.bytes, "password", 8), 0);

  std::vector<uint8_t> profile_data = store.values["p-00001234"];
  int writes = store.writes;
  memcpy(update.replacement.bytes, "new-pass", 8);
  ASSERT_EQ(store.saveProfile(0x1234, settings, update), Status::kOk);
  EXPECT_EQ(store.writes, writes + 1);
  EXPECT_EQ(store.values["p-00001234"], profile_data);
  ASSERT_EQ(store.loadCredentials(0x1234, credentials), Status::kOk);
  EXPECT_EQ(memcmp(credentials.bytes, "new-pass", 8), 0);

  store.values["p-00001234"].push_back(0);
  EXPECT_EQ(store.loadProfile(0x1234, profile), Status::kCorrupt);
}

// Verifies a failed blob erase leaves the profile intact and can be retried.
TEST(StoreTest, FailedDeleteCleanupAndRetry) {
  MemoryStore store;
  ProfileSettings p;
  p.connection = TestConfig();
  CredentialUpdate u;
  u.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(7, p, u), Status::kOk);
  store.fail_at = store.writes + 1;
  EXPECT_EQ(store.removeProfile(7), Status::kStorageFailure);
  Profile out;
  EXPECT_EQ(store.loadProfile(7, out), Status::kOk);
  store.fail_at = -1;
  ASSERT_EQ(store.writeLastProfile(7), Status::kOk);
  EXPECT_EQ(store.removeProfile(7), Status::kOk);
  EXPECT_TRUE(store.values.empty());
  ProfileId last;
  EXPECT_EQ(store.readLastProfile(last), Status::kNotFound);
}

// Verifies enumeration exposes only settings blobs and can stop early.
TEST(StoreTest, EnumeratesProfiles) {
  MemoryStore store;
  ProfileSettings settings;
  settings.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(42, settings, update), Status::kOk);
  ASSERT_EQ(store.saveProfile(7, settings, update), Status::kOk);
  ASSERT_EQ(store.saveProfile(9, settings, update), Status::kOk);
  ASSERT_EQ(store.removeProfile(9), Status::kOk);
  store.values["s-0000000b"] = {0x10};  // Orphaned secret is not a profile.
  store.values["not-a-profile"] = {0x11};

  std::vector<ProfileId> ids;
  EXPECT_EQ(store.forEachProfile([&](ProfileId id) {
    ids.push_back(id);
    return true;
  }),
            Status::kOk);
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids, (std::vector<ProfileId>{7, 42}));

  int visits = 0;
  EXPECT_EQ(store.forEachProfile([&](ProfileId) {
    ++visits;
    return false;
  }),
            Status::kStopped);
  EXPECT_EQ(visits, 1);
}

// Verifies empty enumeration, boundary IDs, and underlying failures.
TEST(StoreTest, EnumerationBoundariesAndFailures) {
  MemoryStore store;
  int visits = 0;
  EXPECT_EQ(store.forEachProfile([&](ProfileId) {
    ++visits;
    return true;
  }),
            Status::kOk);
  EXPECT_EQ(visits, 0);

  ProfileSettings settings;
  settings.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(1, settings, update), Status::kOk);
  ASSERT_EQ(store.saveProfile(UINT32_MAX, settings, update), Status::kOk);
  std::vector<ProfileId> ids;
  ASSERT_EQ(store.forEachProfile([&](ProfileId id) {
    ids.push_back(id);
    return true;
  }),
            Status::kOk);
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids, (std::vector<ProfileId>{1, UINT32_MAX}));

  store.enumeration_error = Status::kUnsupported;
  EXPECT_EQ(store.forEachProfile([](ProfileId) { return true; }),
            Status::kUnsupported);
  store.enumeration_error = Status::kStorageFailure;
  EXPECT_EQ(store.forEachProfile([](ProfileId) { return true; }),
            Status::kStorageFailure);
}

// Verifies blob corruption is reported by loading without hiding the ID.
TEST(StoreTest, EnumerationAndCorruptProfiles) {
  MemoryStore store;
  store.values["p-00000002"] = {0xff};
  int visits = 0;
  EXPECT_EQ(store.forEachProfile([&](ProfileId id) {
    ++visits;
    EXPECT_EQ(id, 2u);
    Profile profile;
    EXPECT_EQ(store.loadProfile(id, profile), Status::kCorrupt);
    return true;
  }),
            Status::kOk);
  EXPECT_EQ(visits, 1);
}

// Verifies the controller gates enumeration on its lifecycle and permits
// profile reads from the visitor.
TEST(ProfileEnumerationTest, ControllerFacade) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  Controller controller(radio, store, scheduler);
  EXPECT_EQ(controller.forEachProfile([](ProfileId) { return true; }),
            Status::kNotStarted);
  ASSERT_EQ(controller.begin(), Status::kOk);
  Pump(scheduler);

  ProfileSettings settings;
  settings.connection = TestConfig("enumerated");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(17, settings, update), Status::kOk);
  EXPECT_EQ(controller.forEachProfile([&](ProfileId id) {
    Profile profile;
    EXPECT_EQ(controller.loadProfile(id, profile), Status::kOk);
    EXPECT_EQ(profile.id, 17u);
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
      status = controller.forEachProfile([&](ProfileId id) {
        ids.push_back(id);
        return true;
      });
    }

    Controller& controller;
    int notifications = 0;
    Status status = Status::kNotStarted;
    std::vector<ProfileId> ids;
  } listener(controller);
  controller.addListener(listener);

  ProfileSettings settings;
  settings.connection = TestConfig("notified");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_NE(controller.saveProfile(23, settings, update).id, 0u);
  Pump(scheduler);
  EXPECT_EQ(listener.notifications, 1);
  EXPECT_EQ(listener.status, Status::kOk);
  EXPECT_EQ(listener.ids, (std::vector<ProfileId>{23}));

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
  ASSERT_EQ(store.saveProfile(9, p, u), Status::kOk);
  u.intent = CredentialIntent::kKeep;
  p.connection.hidden = true;
  EXPECT_EQ(store.saveProfile(9, p, u), Status::kOk);
  Credentials c;
  EXPECT_EQ(store.loadCredentials(9, c), Status::kOk);
  EXPECT_EQ(c.size, 8u);
  u.intent = CredentialIntent::kClear;
  u.replacement = {};
  EXPECT_EQ(store.saveProfile(9, p, u), Status::kInvalidArgument);
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
  observer.results.clear();
  Controller::RequestResult request = controller.connect(TestConfig(), {});
  Pump(scheduler);
  scheduler.delay(roo_time::Millis(6));
  Pump(scheduler);
  ASSERT_EQ(observer.results.size(), 1u);
  EXPECT_EQ(observer.results[0].id, request.id);
  EXPECT_EQ(observer.results[0].status, Status::kTimeout);
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(observer.results.size(), 1u);
  EXPECT_EQ(controller.connect(TestConfig(), {}).status, Status::kNotStarted);
  ProfileSettings settings;
  settings.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  EXPECT_NE(controller.saveProfile(1, settings, update).id, 0u);
  Pump(scheduler);
  EXPECT_EQ(observer.results.back().status, Status::kOk);
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

  ASSERT_NE(controller.connect(TestConfig(), {}).id, 0u);
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
  ASSERT_EQ(store.saveProfile(1, settings, update), Status::kOk);
  ASSERT_EQ(store.writeLastProfile(1), Status::kOk);
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
  controller.saveProfile(1, settings, update);
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
  ASSERT_EQ(store.saveProfile(31, settings, update), Status::kOk);

  ASSERT_NE(controller.connect(31).id, 0u);
  Pump(scheduler);
  native.associated();
  native.ready();
  Pump(scheduler);
  ProfileId last = 0;
  EXPECT_EQ(store.readLastProfile(last), Status::kOk);
  EXPECT_EQ(last, 31u);

  ASSERT_NE(controller.connect(TestConfig("temporary"), {}).id, 0u);
  Pump(scheduler);
  native.disconnected();
  Pump(scheduler);
  native.associated();
  native.ready();
  Pump(scheduler);
  ASSERT_EQ(store.readLastProfile(last), Status::kOk);
  EXPECT_EQ(last, 31u);
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
  ASSERT_EQ(store.saveProfile(9, settings, update), Status::kOk);
  ASSERT_EQ(store.writeLastProfile(9), Status::kOk);
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
  controller.saveProfile(1, settings, update);
  Pump(scheduler);
  native.rejection = Status::kConnectionFailed;
  Controller::RequestResult request = controller.connect(1);
  Pump(scheduler);
  EXPECT_EQ(observer.results.back().id, request.id);
  EXPECT_EQ(observer.results.back().status, Status::kConnectionFailed);
  Profile out;
  EXPECT_EQ(controller.loadProfile(1, out), Status::kOk);
}

// An auto-connect profile remains eligible when it is temporarily unavailable.
TEST_F(BackendTest, RetriesUnavailableSavedProfile) {
  ProfileSettings settings;
  settings.connection = TestConfig("later-open");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(5, settings, update), Status::kOk);
  native.rejection = Status::kConnectionFailed;
  ASSERT_NE(controller.connect(5).id, 0u);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 1);

  native.rejection = Status::kOk;
  scheduler.delay(roo_time::Seconds(6));
  Pump(scheduler);
  EXPECT_EQ(native.connects, 2);
}

// Verifies cancellation before queued native execution emits one result and no
// connection.
TEST_F(BackendTest, CancelBeforeNativeStart) {
  Controller::RequestResult request = controller.connect(TestConfig(), {});
  EXPECT_EQ(controller.cancel(request.id), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 0);
  ASSERT_EQ(observer.results.size(), 1u);
  EXPECT_EQ(observer.results[0].status, Status::kCancelled);
}

// Verifies bounded event overflow faults radio admission instead of reusing
// lost identity.
TEST_F(BackendTest, NativeHandoffOverflowFailsClosed) {
  Controller::RequestResult request = controller.scan();
  Pump(scheduler);
  for (int i = 0; i < 20; ++i) native.emit({NativeStation::Event::kScanDone});
  Pump(scheduler);
  ASSERT_EQ(observer.results.size(), 1u);
  EXPECT_EQ(observer.results[0].id, request.id);
  EXPECT_EQ(observer.results[0].status, Status::kConnectionFailed);
  controller.scan();
  Pump(scheduler);
  EXPECT_EQ(observer.results.back().status, Status::kNotStarted);
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
      if (std::string(key) == "p-00000001") {
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
  EXPECT_EQ(store.saveProfile(1, settings, update), Status::kOk);
  store.unreadable = true;
  EXPECT_EQ(store.saveProfile(1, settings, update), Status::kCommitUnknown);
  Profile untouched;
  untouched.id = 99;
  EXPECT_EQ(store.loadProfile(1, untouched), Status::kStorageFailure);
  EXPECT_EQ(untouched.id, 99u);
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
  EXPECT_NE(first.connect(TestConfig(), {}).id, 0u);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 1);
}
}  // namespace roo_wifi
