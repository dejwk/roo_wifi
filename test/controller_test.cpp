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

// Verifies all write interruptions leave either the previous ready record or
// Incomplete.
TEST(StoreTest, EveryInterruptedWriteAndRepair) {
  ProfileSettings p;
  p.connection = TestConfig();
  CredentialUpdate u;
  u.intent = CredentialIntent::kClear;
  for (int failure = 1; failure <= 16; ++failure) {
    MemoryStore store;
    ASSERT_EQ(store.saveProfile(1, p, u), Status::kOk);
    store.fail_at = store.writes + failure;
    p.connection.hidden = true;
    Status result = store.saveProfile(1, p, u);
    EXPECT_NE(result, Status::kOk);
    Profile out;
    Status read = store.loadProfile(1, out);
    EXPECT_EQ(read, failure == 1 ? Status::kOk : Status::kIncomplete);
    if (failure != 1) {
      CredentialUpdate keep;
      EXPECT_NE(store.saveProfile(1, p, keep), Status::kOk);
    }
    store.fail_at = -1;
    EXPECT_EQ(store.saveProfile(1, p, u), Status::kOk);
    EXPECT_EQ(store.loadProfile(1, out), Status::kOk);
    EXPECT_TRUE(out.settings.connection.hidden);
  }
}

// Verifies deleted markers prevent resurrection when cleanup fails and can be
// retried.
TEST(StoreTest, FailedDeleteCleanupAndRetry) {
  MemoryStore store;
  ProfileSettings p;
  p.connection = TestConfig();
  CredentialUpdate u;
  u.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(7, p, u), Status::kOk);
  store.fail_at = store.writes + 2;
  EXPECT_EQ(store.removeProfile(7), Status::kStorageFailure);
  Profile out;
  EXPECT_EQ(store.loadProfile(7, out), Status::kNotFound);
  store.fail_at = -1;
  EXPECT_EQ(store.removeProfile(7), Status::kOk);
  EXPECT_EQ(store.values.size(), 1u);
}

// Verifies enumeration exposes only committed profiles and can stop early.
TEST(StoreTest, EnumeratesCommittedProfiles) {
  MemoryStore store;
  ProfileSettings settings;
  settings.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(store.saveProfile(42, settings, update), Status::kOk);
  ASSERT_EQ(store.saveProfile(7, settings, update), Status::kOk);
  ASSERT_EQ(store.saveProfile(9, settings, update), Status::kOk);
  ASSERT_EQ(store.removeProfile(9), Status::kOk);
  store.values["0000000bstate"] = {0x10};  // Interrupted save.
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

// Verifies disabled provisioning, startup selection, and owned profile input.
TEST(StartupTest, KnownProfileAndAdmissionSnapshot) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  ProfileSettings settings;
  settings.connection = TestConfig("saved");
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  Controller::Options options;
  options.startup_profile = 1;
  Controller controller(radio, store, scheduler, options);
  ASSERT_EQ(controller.begin(), Status::kOk);
  Pump(scheduler);
  ASSERT_FALSE(controller.isEnabled());
  EXPECT_NE(controller.saveProfile(1, settings, update).id, 0u);
  Pump(scheduler);
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
// Verifies a failed ready write is reread, distinguishing confirmed completion
// from an unreadable commit outcome without claiming atomic replacement.
TEST(StoreTest, FinalCommitVerification) {
  /// Simulates a final commit whose write result is ambiguous.
  class AmbiguousStore : public MemoryStore {
   public:
    /// Writes the field but reports failure for the final ready marker.
    Status writeField(const char* key, const uint8_t* data,
                      size_t size) override {
      Status status = MemoryStore::writeField(key, data, size);
      if (std::string(key) == "00000001state" && data[0] == 0x11) {
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
