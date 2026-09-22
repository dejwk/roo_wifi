#include "backend_fakes.h"
#include "gtest/gtest.h"
namespace roo_wifi {
/// Provides an enabled radio and a scheduler-confined notification observer.
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
  /// Completes association and address acquisition for the active attempt.
  void ready() {
    native.associated();
    native.ready();
    Pump(scheduler);
  }
};
// Verifies category isolation and independent coalescing within one delivery.
TEST_F(BackendTest, NotificationsAreCoalescedByCategory) {
  struct Categories : Controller::Listener {
    int station = 0;
    int scan = 0;
    int profiles = 0;
    void onStationStateChanged() override { ++station; }
    void onScanStateChanged() override { ++scan; }
    void onProfilesChanged() override { ++profiles; }
  } listener;
  controller.addListener(listener);
  ProfileSettings settings;
  settings.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(controller.saveProfile(10, settings, update), Status::kOk);
  ASSERT_EQ(controller.removeProfile(10), Status::kOk);
  ASSERT_EQ(controller.startScan(), Status::kOk);
  ASSERT_EQ(controller.cancelScan(), Status::kOk);
  EXPECT_EQ(listener.scan, 0);
  EXPECT_EQ(listener.profiles, 0);
  Pump(scheduler);
  EXPECT_EQ(listener.station, 0);
  EXPECT_EQ(listener.scan, 1);
  EXPECT_EQ(listener.profiles, 1);
  ASSERT_EQ(controller.connect(TestConfig(), {}), Status::kOk);
  Pump(scheduler);
  EXPECT_GT(listener.station, 0);
  EXPECT_EQ(listener.scan, 1);
  EXPECT_EQ(listener.profiles, 1);
  controller.removeListener(listener);
}

// Verifies a category changed by a callback is queued for a later batch.
TEST_F(BackendTest, NotificationCallbacksCanQueueAnotherCategory) {
  struct Follow : Controller::Listener {
    Controller* controller = nullptr;
    bool in_callback = false;
    int profiles = 0;
    int scans = 0;
    void onProfilesChanged() override {
      in_callback = true;
      ++profiles;
      EXPECT_EQ(controller->startScan(), Status::kOk);
      EXPECT_EQ(controller->cancelScan(), Status::kOk);
      EXPECT_EQ(scans, 0);
      in_callback = false;
    }
    void onScanStateChanged() override {
      EXPECT_FALSE(in_callback);
      ++scans;
    }
  } listener;
  listener.controller = &controller;
  controller.addListener(listener);
  ProfileSettings settings;
  settings.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(controller.saveProfile(10, settings, update), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(listener.profiles, 1);
  EXPECT_EQ(listener.scans, 1);
  controller.removeListener(listener);
}

// Verifies shutdown stops subsequent categories within the same listener.
TEST_F(BackendTest, ShutdownStopsRemainingNotificationCategories) {
  struct Stop : Controller::Listener {
    Controller* controller = nullptr;
    int scans = 0;
    int profiles = 0;
    void onScanStateChanged() override {
      ++scans;
      controller->shutdown();
    }
    void onProfilesChanged() override { ++profiles; }
  } listener;
  listener.controller = &controller;
  controller.addListener(listener);
  ProfileSettings settings;
  settings.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(controller.saveProfile(10, settings, update), Status::kOk);
  ASSERT_EQ(controller.startScan(), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(listener.scans, 1);
  EXPECT_EQ(listener.profiles, 0);
  controller.removeListener(listener);
}

// Verifies compact FIFO slots preserve association/address metadata and stale
// event rejection while wrapping and reusing slots for different payload kinds.
TEST_F(BackendTest, CompactQueuePreservesPayloadsAndIdentityChecks) {
  using E = NativeStation::Event;
  for (int iteration = 0; iteration < 12; ++iteration) {
    ASSERT_EQ(controller.connect(TestConfig("A"), {}), Status::kOk);
    Pump(scheduler);
    E associated{E::kAssociated};
    associated.link.ssid = TestConfig("A").ssid;
    associated.link.bssid.bytes[5] = 42;
    associated.link.station_mac.bytes[5] = 17;
    associated.link.security = AuthMode::kOpen;
    associated.link.channel = 233;
    associated.link.rssi_dbm = -51;
    associated.link.has_radio_info = true;
    associated.link.has_station_mac = true;
    E stale = associated;
    stale.link.ssid = TestConfig("B").ssid;
    native.emit(stale);
    Pump(scheduler);
    EXPECT_EQ(controller.linkState().phase, LinkPhase::kConnecting);
    native.emit(associated);
    E address{E::kAddressReady};
    address.link.ssid = associated.link.ssid;
    address.link.bssid = associated.link.bssid;
    address.link.address = {{192, 168, 1, 5}};
    address.link.gateway = {{192, 168, 1, 1}};
    address.link.dns1 = {{1, 1, 1, 1}};
    address.link.dns2 = {{8, 8, 4, 4}};
    address.link.has_ipv4 = true;
    address.link.has_dns1 = true;
    address.link.has_dns2 = (iteration % 2) == 0;
    stale = address;
    stale.link.bssid.bytes[5] = 43;
    native.emit(stale);
    stale = address;
    stale.link.ssid = TestConfig("B").ssid;
    native.emit(stale);
    Pump(scheduler);
    EXPECT_EQ(controller.linkState().phase, LinkPhase::kAssociated);
    native.emit(address);
    Pump(scheduler);
    LinkState link = controller.linkState();
    EXPECT_EQ(link.phase, LinkPhase::kAddressReady);
    EXPECT_EQ(link.bssid.bytes[5], 42);
    EXPECT_EQ(link.station_mac.bytes[5], 17);
    EXPECT_EQ(link.security, AuthMode::kOpen);
    EXPECT_EQ(link.channel, 233);
    EXPECT_EQ(link.rssi_dbm, -51);
    EXPECT_TRUE(link.has_radio_info);
    EXPECT_TRUE(link.has_station_mac);
    EXPECT_TRUE(link.has_ipv4);
    EXPECT_TRUE(link.has_dns1);
    EXPECT_EQ(link.has_dns2, (iteration % 2) == 0);
    EXPECT_EQ(memcmp(link.address.bytes, address.link.address.bytes, 4), 0);
    EXPECT_EQ(memcmp(link.gateway.bytes, address.link.gateway.bytes, 4), 0);
    EXPECT_EQ(memcmp(link.dns1.bytes, address.link.dns1.bytes, 4), 0);
    EXPECT_EQ(memcmp(link.dns2.bytes, address.link.dns2.bytes, 4), 0);
    native.emit({E::kAddressLost});
    Pump(scheduler);
    EXPECT_FALSE(controller.linkState().has_ipv4);
    ASSERT_EQ(controller.disconnect(), Status::kOk);
    Pump(scheduler);
    E disconnected{E::kDisconnected};
    disconnected.link.ssid = TestConfig("B").ssid;
    disconnected.status = Status::kConnectionFailed;
    disconnected.native_code = 123;
    native.emit(disconnected);
    Pump(scheduler);
    EXPECT_EQ(controller.state().station,
              Controller::StationPhase::kDisconnecting);
    disconnected.link.ssid = TestConfig("A").ssid;
    native.emit(disconnected);
    Pump(scheduler);
    EXPECT_EQ(controller.linkState().phase, LinkPhase::kIdle);
    EXPECT_EQ(controller.linkState().reason, Status::kConnectionFailed);
    EXPECT_EQ(controller.linkState().native_code, 123);
    EXPECT_TRUE(controller.linkState().has_native_code);
  }
}

// Verifies latest intent before dispatch wins.
TEST_F(BackendTest, LatestIntentBeforeDispatchWins) {
  ASSERT_EQ(controller.connect(TestConfig("A"), {}), Status::kOk);
  ASSERT_EQ(controller.connect(TestConfig("B"), {}), Status::kOk);
  ASSERT_EQ(controller.disconnect(), Status::kOk);
  EXPECT_EQ(observer.notifications, 0);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 0);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kIdle);
  EXPECT_GT(observer.notifications, 0);
}
// Verifies connect disconnect connect waits for teardown.
TEST_F(BackendTest, ConnectDisconnectConnectWaitsForTeardown) {
  ASSERT_EQ(controller.connect(TestConfig("A"), {}), Status::kOk);
  Pump(scheduler);
  native.associated();
  Pump(scheduler);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kAwaitingIp);
  ASSERT_EQ(controller.disconnect(), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.disconnects, 1);
  ASSERT_EQ(controller.connect(TestConfig("B"), {}), Status::kOk);
  ASSERT_EQ(controller.connect(TestConfig("C"), {}), Status::kOk);
  native.ready();
  Pump(scheduler);
  EXPECT_EQ(native.connects, 1);
  EXPECT_EQ(controller.state().station,
            Controller::StationPhase::kDisconnecting);
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(native.connects, 2);
  EXPECT_EQ(native.last_config.ssid.bytes[0], 'C');
  ready();
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kConnected);
  EXPECT_EQ(controller.state().status, Status::kOk);
}
// Verifies switching established link and repeated intent.
TEST_F(BackendTest, SwitchingEstablishedLinkAndRepeatedIntent) {
  auto a = TestConfig("A");
  ASSERT_EQ(controller.connect(a, {}), Status::kOk);
  a.ssid.bytes[0] = 'X';
  Pump(scheduler);
  ready();
  EXPECT_EQ(native.last_config.ssid.bytes[0], 'A');
  uint64_t revision = controller.state().revision;
  EXPECT_EQ(controller.connect(TestConfig("A"), {}), Status::kOk);
  EXPECT_EQ(controller.state().revision, revision);
  EXPECT_EQ(controller.connect(TestConfig("B"), {}), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 1);
  native.disconnected();
  Pump(scheduler);
  ready();
  EXPECT_EQ(native.connects, 2);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kConnected);
}
// Verifies disable interrupts connection and can be reversed.
TEST_F(BackendTest, DisableInterruptsConnectionAndCanBeReversed) {
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  EXPECT_EQ(controller.setEnabled(false), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(controller.state().station,
            Controller::StationPhase::kDisconnecting);
  EXPECT_EQ(controller.setEnabled(true), Status::kOk);
  EXPECT_EQ(controller.connect(TestConfig("B"), {}), Status::kOk);
  native.disconnected();
  Pump(scheduler);
  ready();
  EXPECT_TRUE(controller.isEnabled());
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kConnected);
}
// Verifies scan cancellation and snapshot lifetime.
TEST_F(BackendTest, ScanCancellationAndSnapshotLifetime) {
  native.aps.resize(1);
  native.aps[0].ssid = TestConfig().ssid;
  ASSERT_EQ(controller.startScan(), Status::kOk);
  EXPECT_EQ(controller.startScan(), Status::kBusy);
  Pump(scheduler);
  native.emit({NativeStation::Event::kScanDone});
  Pump(scheduler);
  Controller::ScanSnapshot snapshot = controller.scanSnapshot();
  ASSERT_EQ(snapshot.count, 1u);
  ASSERT_EQ(controller.startScan(), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(controller.cancelScan(), Status::kOk);
  EXPECT_EQ(controller.cancelScan(), Status::kOk);
  EXPECT_EQ(native.scan_stops, 1);
  EXPECT_TRUE(controller.isScanning());
  native.emit({NativeStation::Event::kScanDone});
  Pump(scheduler);
  EXPECT_EQ(controller.state().scan_status, Status::kCancelled);
  EXPECT_EQ(controller.scanSnapshot().generation, snapshot.generation);
  EXPECT_EQ(controller.scanSnapshot().records[0].ssid.bytes[0], 'n');
  EXPECT_EQ(controller.cancelScan(), Status::kOk);
}
// Verifies cancel queued scan and connect during scan.
TEST_F(BackendTest, CancelQueuedScanAndConnectDuringScan) {
  controller.startScan();
  controller.cancelScan();
  Pump(scheduler);
  EXPECT_EQ(native.scans, 0);
  EXPECT_EQ(controller.state().scan_status, Status::kCancelled);
  controller.startScan();
  Pump(scheduler);
  EXPECT_EQ(controller.connect(TestConfig(), {}), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.scan_stops, 1);
  EXPECT_EQ(native.connects, 0);
  native.emit({NativeStation::Event::kScanDone});
  Pump(scheduler);
  EXPECT_EQ(native.connects, 1);
}
// Verifies writes are synchronous and notify asynchronously.
TEST_F(BackendTest, WritesAreSynchronousAndNotifyAsynchronously) {
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  observer.notifications = 0;
  ProfileSettings settings;
  settings.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  EXPECT_EQ(controller.saveProfile(42, settings, update), Status::kOk);
  Profile p;
  EXPECT_EQ(controller.loadProfile(42, p), Status::kOk);
  EXPECT_EQ(observer.notifications, 0);
  EXPECT_EQ(controller.removeProfile(42), Status::kOk);
  EXPECT_EQ(controller.loadProfile(42, p), Status::kNotFound);
  Pump(scheduler);
  EXPECT_EQ(observer.notifications, 1);
  EXPECT_EQ(controller.state().profiles_generation, 2u);
}
// Verifies listener can submit new intent without recursive notification.
TEST_F(BackendTest, ListenerCanSubmitNewIntentWithoutRecursiveNotification) {
  struct Follow : Controller::Listener {
    Controller* controller = nullptr;
    int calls = 0;
    void onStationStateChanged() override {
      ++calls;
      if (calls == 1) {
        EXPECT_EQ(controller->disconnect(), Status::kOk);
        EXPECT_EQ(calls, 1);
      }
    }
  } follow;
  follow.controller = &controller;
  controller.addListener(follow);
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(controller.state().desired, Controller::Target::kDisconnected);
  controller.removeListener(follow);
}
// Verifies shutdown neutralizes queued events.
TEST_F(BackendTest, ShutdownNeutralizesQueuedEvents) {
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  native.associated();
  native.ready();
  int count = observer.notifications;
  controller.shutdown();
  Pump(scheduler);
  EXPECT_EQ(observer.notifications, count);
  EXPECT_EQ(controller.disconnect(), Status::kNotStarted);
}
// Verifies validation does not replace intent.
TEST_F(BackendTest, ValidationDoesNotReplaceIntent) {
  controller.connect(TestConfig(), {});
  uint64_t revision = controller.state().revision;
  EXPECT_EQ(controller.connect(ConnectionConfig{}, {}),
            Status::kInvalidArgument);
  EXPECT_EQ(controller.state().revision, revision);
}
// Verifies saved profile success and disconnect suppress retry.
TEST_F(BackendTest, SavedProfileSuccessAndDisconnectSuppressRetry) {
  ProfileSettings p;
  p.connection = TestConfig();
  p.auto_connect = true;
  CredentialUpdate u;
  u.intent = CredentialIntent::kClear;
  ASSERT_EQ(controller.saveProfile(7, p, u), Status::kOk);
  ASSERT_EQ(controller.connect(7), Status::kOk);
  Pump(scheduler);
  ready();
  EXPECT_EQ(controller.state().connected_profile, 7u);
  ProfileId last = 0;
  EXPECT_EQ(store.readLastProfile(last), Status::kOk);
  EXPECT_EQ(last, 7u);
  controller.disconnect();
  Pump(scheduler);
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(scheduler.getNearestExecutionTime(), roo_time::Uptime::Max());
}
// Verifies failed attempt can be retried explicitly.
TEST_F(BackendTest, FailedAttemptCanBeRetriedExplicitly) {
  native.rejection = Status::kConnectionFailed;
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  EXPECT_EQ(controller.state().status, Status::kConnectionFailed);
  EXPECT_EQ(native.connects, 1);
  native.rejection = Status::kOk;
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  ready();
  EXPECT_EQ(native.connects, 2);
  EXPECT_EQ(controller.state().status, Status::kOk);
}
// Verifies unsettled cancellation faults and preserves writes.
TEST(TimeoutTest, UnsettledCancellationFaultsAndPreservesWrites) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  store.enabled = true;
  Controller::Options options;
  options.transition_timeout_ms = 1;
  Controller controller(radio, store, scheduler, options);
  controller.begin();
  Pump(scheduler);
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  controller.disconnect();
  Pump(scheduler);
  controller.connect(TestConfig("B"), {});
  scheduler.delay(roo_time::Millis(6));
  Pump(scheduler);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kFaulted);
  EXPECT_EQ(controller.state().status, Status::kTimeout);
  EXPECT_EQ(native.connects, 1);
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(controller.connect(TestConfig(), {}), Status::kNotStarted);
  ProfileSettings p;
  p.connection = TestConfig();
  CredentialUpdate u;
  u.intent = CredentialIntent::kClear;
  EXPECT_EQ(controller.saveProfile(1, p, u), Status::kOk);
}
// Verifies connection timeout settles without fault.
TEST(TimeoutTest, ConnectionTimeoutSettlesWithoutFault) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  store.enabled = true;
  Controller::Options options;
  options.connect_timeout_ms = 1;
  Controller controller(radio, store, scheduler, options);
  controller.begin();
  Pump(scheduler);
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  scheduler.delay(roo_time::Millis(3));
  EXPECT_EQ(native.disconnects, 1);
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(controller.state().status, Status::kTimeout);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kIdle);
  EXPECT_EQ(controller.connect(TestConfig(), {}), Status::kOk);
}
// Verifies restores last profile unless explicitly superseded.
TEST(StartupTest, RestoresLastProfileUnlessExplicitlySuperseded) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  store.enabled = true;
  ProfileSettings p;
  p.connection = TestConfig("saved");
  p.auto_connect = true;
  CredentialUpdate u;
  u.intent = CredentialIntent::kClear;
  store.saveProfile(1, p, u);
  store.writeLastProfile(1);
  Controller controller(radio, store, scheduler);
  controller.begin();
  controller.disconnect();
  Pump(scheduler);
  EXPECT_EQ(native.connects, 0);
  controller.setEnabled(false);
  Pump(scheduler);
  controller.setEnabled(true);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 1);
  EXPECT_EQ(native.last_config.ssid.bytes[0], 's');
}
// Verifies failed begin does not shutdown existing owner.
TEST(OwnershipTest, FailedBeginDoesNotShutdownExistingOwner) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  Controller first(radio, store, scheduler);
  EXPECT_EQ(first.begin(), Status::kOk);
  {
    Controller second(radio, store, scheduler);
    EXPECT_EQ(second.begin(), Status::kBusy);
  }
  Pump(scheduler);
  EXPECT_EQ(first.setEnabled(true), Status::kOk);
  Pump(scheduler);
  EXPECT_TRUE(first.isEnabled());
}

// Verifies explicit reconnect remains possible after a temporary link drops.
TEST_F(BackendTest, TemporaryLinkLossCanBeRetried) {
  ASSERT_EQ(controller.connect(TestConfig(), {}), Status::kOk);
  Pump(scheduler);
  ready();
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(controller.state().status, Status::kConnectionFailed);
  EXPECT_EQ(controller.connect(TestConfig(), {}), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 2);
}

// Verifies enable/disable reversals settle the active physical transition
// first.
TEST_F(BackendTest, EnablementReversalDuringNativeTransition) {
  EXPECT_EQ(controller.setEnabled(false), Status::kOk);
  scheduler.executeEligibleTasks();
  EXPECT_EQ(controller.setEnabled(true), Status::kOk);
  Pump(scheduler);
  EXPECT_TRUE(controller.isEnabled());
  EXPECT_EQ(controller.state().desired, Controller::Target::kDisconnected);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kIdle);
}

// Verifies shutdown inside notification prevents subsequent listener delivery.
TEST_F(BackendTest, ShutdownInsideListenerStopsNotification) {
  struct Stopper : Controller::Listener {
    Controller* controller = nullptr;
    void onStationStateChanged() override { controller->shutdown(); }
  } stopper;
  stopper.controller = &controller;
  Observer after;
  controller.addListener(stopper);
  controller.addListener(after);
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  EXPECT_EQ(after.notifications, 0);
  EXPECT_EQ(native.connects, 0);
  controller.removeListener(after);
  controller.removeListener(stopper);
}

// Verifies a timed-out scan retains published results while teardown settles.
TEST(TimeoutTest, ScanTimeoutWaitsForNativeCancellation) {
  roo_scheduler::Scheduler scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  store.enabled = true;
  Controller::Options options;
  options.scan_timeout_ms = 1;
  Controller controller(radio, store, scheduler, options);
  controller.begin();
  Pump(scheduler);
  ASSERT_EQ(controller.startScan(), Status::kOk);
  Pump(scheduler);
  scheduler.delay(roo_time::Millis(3));
  EXPECT_EQ(controller.state().scan, Controller::ScanPhase::kCancelling);
  EXPECT_EQ(native.scan_stops, 1);
  EXPECT_EQ(controller.connect(TestConfig(), {}), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 0);
  native.emit({NativeStation::Event::kScanDone});
  Pump(scheduler);
  EXPECT_EQ(controller.state().scan_status, Status::kTimeout);
  EXPECT_EQ(native.connects, 1);
}

}  // namespace roo_wifi
