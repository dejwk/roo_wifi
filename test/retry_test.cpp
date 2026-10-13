#include "backend_fakes.h"
#include "gtest/gtest.h"

namespace roo_wifi {
namespace {
// Uses short deterministic delays while exercising the real scheduler and
// ordered adapter, including deferred native cancellation.
Controller::Options RetryOptions() {
  Controller::Options options;
  for (unsigned i = 0; i < 6; ++i) options.retry.delays_ms[i] = 20 * (i + 1);
  options.retry.jitter_percent = 0;
  options.retry.stable_connection_ms = 20;
  options.connect_timeout_ms = 1000;
  return options;
}

class RetryTest : public testing::Test {
 protected:
  roo_scheduler::SchedulingService scheduler;
  TestStation native;
  OrderedInterface radio{native};
  MemoryStore store;
  Controller controller{radio, store, scheduler, RetryOptions()};

  void SetUp() override {
    store.enabled = true;
    ASSERT_EQ(controller.begin(), Status::kOk);
    Pump(scheduler);
    ProfileSettings profile;
    profile.connection = TestConfig();
    CredentialUpdate update;
    update.intent = CredentialIntent::kClear;
    ASSERT_EQ(controller.saveProfile(profile, update), Status::kOk);
    ASSERT_EQ(controller.connect(profile.connection.ssid), Status::kOk);
    Pump(scheduler);
  }

  void fail(ConnectionFailure failure = ConnectionFailure::kTransient,
            int code = 201) {
    NativeStation::Event event{NativeStation::Event::kDisconnected};
    event.link.ssid = native.last_config.ssid;
    event.link.failure = failure;
    event.link.bssid.bytes[5] = 7;
    event.link.rssi_dbm = -65;
    event.link.channel = 11;
    event.link.has_radio_info = true;
    event.status = Status::kConnectionFailed;
    event.native_code = code;
    native.emit(event);
    Pump(scheduler);
  }

  void retry() {
    ASSERT_TRUE(controller.state().retry.pending);
    scheduler.delay(controller.state().retry.next_attempt -
                    roo_time::Uptime::Now() + roo_time::Millis(2));
    Pump(scheduler);
  }

  void ready() {
    native.associated();
    native.ready();
    Pump(scheduler);
  }
};

// Verifies escalating bounded delays, retained error/AP metadata during a new
// attempt, and reset only after a sustained successful connection.
TEST_F(RetryTest, BackoffRetainsDiagnosticsAndResetsAfterStableSuccess) {
  for (int i = 0; i < 7; ++i) {
    fail();
    const Controller::RetryState state = controller.state().retry;
    EXPECT_EQ(state.attempt_count, static_cast<unsigned>(i + 1));
    EXPECT_EQ(state.last_status, Status::kConnectionFailed);
    EXPECT_EQ(state.native_code, 201);
    EXPECT_EQ(state.bssid.bytes[5], 7);
    EXPECT_EQ(state.channel, 11);
    EXPECT_EQ(state.rssi_dbm, -65);
    const int64_t remaining =
        (state.next_attempt - roo_time::Uptime::Now()).inMillis();
    const int64_t expected = 20 * (std::min(i, 5) + 1);
    EXPECT_LE(remaining, expected);
    EXPECT_GE(remaining, expected - 10);
    retry();
    EXPECT_EQ(native.connects, i + 2);
    EXPECT_EQ(controller.state().retry.last_status, Status::kConnectionFailed);
    EXPECT_EQ(controller.state().status, Status::kOk);
    EXPECT_FALSE(controller.state().has_native_code);
  }
  ready();
  EXPECT_GT(controller.state().retry.attempt_count, 1u);
  scheduler.delay(roo_time::Millis(30));
  EXPECT_EQ(controller.state().retry.attempt_count, 0u);
  EXPECT_EQ(controller.state().retry.last_status, Status::kOk);
  fail();
  EXPECT_LE((controller.state().retry.next_attempt - roo_time::Uptime::Now())
                .inMillis(),
            20);
}

// Verifies ambiguous authentication gets a small burst before cooldown, while
// incompatible security avoids rapid repetition without changing credentials.
TEST_F(RetryTest, AuthenticationAndSecurityCooldown) {
  for (int i = 0; i < 3; ++i) {
    fail(ConnectionFailure::kAuthentication, 202);
    const int64_t remaining =
        (controller.state().retry.next_attempt - roo_time::Uptime::Now())
            .inMillis();
    EXPECT_GE(remaining, (i == 2 ? 120 : 20 * (i + 1)) - 10);
    retry();
  }
  ASSERT_EQ(controller.connect(TestConfig().ssid), Status::kOk);
  // This is an idempotent call while the attempt is active.
  fail(ConnectionFailure::kIncompatibleSecurity, 211);
  EXPECT_GE((controller.state().retry.next_attempt - roo_time::Uptime::Now())
                .inMillis(),
            110);
  EXPECT_EQ(native.last_config.security, AuthMode::kOpen);
}

// Verifies deliberate disconnect and disable cancel scheduled retries, and
// an explicit same-profile retry starts immediately.
TEST_F(RetryTest, IntentChangesCancelPendingRetries) {
  fail();
  ASSERT_EQ(controller.connect(TestConfig().ssid), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.connects, 2);
  EXPECT_EQ(controller.state().retry.attempt_count, 1u);
  fail();
  ASSERT_EQ(controller.disconnect(), Status::kOk);
  Pump(scheduler);
  scheduler.delay(roo_time::Millis(30));
  EXPECT_EQ(native.connects, 2);
  EXPECT_FALSE(controller.state().retry.pending);
  ASSERT_EQ(controller.connect(TestConfig().ssid), Status::kOk);
  Pump(scheduler);
  fail();
  ASSERT_EQ(controller.setEnabled(false), Status::kOk);
  Pump(scheduler);
  scheduler.delay(roo_time::Millis(30));
  EXPECT_EQ(native.connects, 3);
  EXPECT_FALSE(controller.state().retry.pending);
}

// Verifies changing a saved profile's retry policy takes effect at retry time,
// without replacing the already accepted connection settings snapshot.
TEST_F(RetryTest, AutoConnectOptOutSuppressesScheduledRetry) {
  fail();
  ProfileSettings profile;
  profile.connection = TestConfig();
  profile.auto_connect = false;
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  ASSERT_EQ(controller.saveProfile(profile, update), Status::kOk);
  retry();
  EXPECT_EQ(native.connects, 1);
  EXPECT_FALSE(controller.state().retry.pending);
}

// Verifies association/address loss starts a bounded recovery timer and that
// late DHCP recovery during backoff cancels the pending reconnect.
TEST_F(RetryTest, AddressRecoveryCancelsRetry) {
  ready();
  native.emit({NativeStation::Event::kAddressLost});
  Pump(scheduler);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kAwaitingIp);
  scheduler.delay(roo_time::Millis(1010));
  ASSERT_TRUE(controller.state().retry.pending);
  EXPECT_EQ(controller.state().retry.failure,
            ConnectionFailure::kAddressTimeout);
  native.ready();
  Pump(scheduler);
  EXPECT_FALSE(controller.state().retry.pending);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kConnected);
  scheduler.delay(roo_time::Millis(30));
  EXPECT_EQ(native.connects, 1);
  EXPECT_EQ(native.disconnects, 0);
}

// Verifies a timeout is retried only after native cancellation settles, and
// stale address events cannot complete or overlap the replacement attempt.
TEST(RetryTimeoutTest, ConnectionTimeoutWaitsForTeardown) {
  roo_scheduler::SchedulingService scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  store.enabled = true;
  Controller::Options options = RetryOptions();
  options.connect_timeout_ms = 15;
  Controller controller(radio, store, scheduler, options);
  controller.begin();
  Pump(scheduler);
  ProfileSettings profile;
  profile.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  controller.saveProfile(profile, update);
  controller.connect(profile.connection.ssid);
  Pump(scheduler);
  native.associated();
  Pump(scheduler);
  scheduler.delay(roo_time::Millis(20));
  EXPECT_EQ(native.disconnects, 1);
  EXPECT_FALSE(controller.state().retry.pending);
  native.ready();
  Pump(scheduler);
  EXPECT_NE(controller.state().station, Controller::StationPhase::kConnected);
  native.disconnected();
  Pump(scheduler);
  ASSERT_TRUE(controller.state().retry.pending);
  EXPECT_EQ(controller.state().retry.failure,
            ConnectionFailure::kAddressTimeout);
  scheduler.delay(roo_time::Millis(24));
  EXPECT_EQ(native.connects, 2);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kConnecting);
}

// Verifies direct requests remain single-attempt and synchronous permanent
// errors do not produce an automatic retry even for saved profiles.
TEST(RetryTimeoutTest, DirectRequestsAndPermanentErrorsDoNotRetry) {
  roo_scheduler::SchedulingService scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  store.enabled = true;
  Controller controller(radio, store, scheduler, RetryOptions());
  controller.begin();
  Pump(scheduler);
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  native.disconnected();
  Pump(scheduler);
  EXPECT_FALSE(controller.state().retry.pending);
  ProfileSettings profile;
  profile.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  controller.saveProfile(profile, update);
  native.rejection = Status::kUnsupported;
  controller.connect(profile.connection.ssid);
  Pump(scheduler);
  EXPECT_EQ(controller.state().status, Status::kUnsupported);
  EXPECT_FALSE(controller.state().retry.pending);
  controller.shutdown();
  scheduler.delay(roo_time::Millis(30));
  EXPECT_EQ(native.connects, 2);
}
// Verifies changing networks supersedes pending retries, and shutdown prevents
// a scheduled callback from starting more native work.
TEST_F(RetryTest, ReplacementAndShutdownSuppressOldRetry) {
  fail();
  ASSERT_EQ(controller.connect(TestConfig("replacement"), {}), Status::kOk);
  Pump(scheduler);
  EXPECT_EQ(native.last_config.ssid, TestConfig("replacement").ssid);
  scheduler.delay(roo_time::Millis(30));
  EXPECT_EQ(native.connects, 2);
  fail();
  ASSERT_EQ(controller.connect(TestConfig().ssid), Status::kOk);
  Pump(scheduler);
  fail();
  ASSERT_TRUE(controller.state().retry.pending);
  controller.shutdown();
  scheduler.delay(roo_time::Millis(30));
  EXPECT_EQ(native.connects, 3);
}

// Verifies lost-address recovery disconnects before a replacement connection,
// and ignores a late IP event once that teardown has begun.
TEST_F(RetryTest, LostAddressReconnectWaitsForDisconnect) {
  ready();
  native.emit({NativeStation::Event::kAddressLost});
  Pump(scheduler);
  scheduler.delay(roo_time::Millis(1010));
  retry();
  EXPECT_EQ(native.disconnects, 1);
  EXPECT_EQ(native.connects, 1);
  native.ready();
  Pump(scheduler);
  EXPECT_NE(controller.state().station, Controller::StationPhase::kConnected);
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(native.connects, 2);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kConnecting);
}

// Verifies repeated idle notifications cannot postpone a pending retry or
// advance the backoff twice for one failed connection.
TEST_F(RetryTest, DuplicateDisconnectDoesNotRestartBackoff) {
  fail();
  const roo_time::Uptime deadline = controller.state().retry.next_attempt;
  fail();
  EXPECT_EQ(controller.state().retry.next_attempt, deadline);
  retry();
  EXPECT_EQ(native.connects, 2);
}

// Verifies default jitter is bounded and a connection timeout before
// association remains distinct from a DHCP timeout.
TEST(RetryTimeoutTest, JitterAndPreAssociationTimeout) {
  roo_scheduler::SchedulingService scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  store.enabled = true;
  Controller::Options options;
  options.connect_timeout_ms = 5;
  Controller controller(radio, store, scheduler, options);
  controller.begin();
  Pump(scheduler);
  ProfileSettings profile;
  profile.connection = TestConfig();
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  controller.saveProfile(profile, update);
  controller.connect(profile.connection.ssid);
  Pump(scheduler);
  scheduler.delay(roo_time::Millis(10));
  native.disconnected();
  Pump(scheduler);
  EXPECT_EQ(controller.state().retry.failure,
            ConnectionFailure::kConnectionTimeout);
  const int64_t delay =
      (controller.state().retry.next_attempt - roo_time::Uptime::Now())
          .inMillis();
  EXPECT_GE(delay, 390);
  EXPECT_LE(delay, 600);
  controller.shutdown();
}

// Verifies DHCP recovery clears a timeout on an existing direct connection
// without scheduling or starting an additional connection attempt.
TEST(RetryTimeoutTest, DirectAddressRecoveryClearsTimeout) {
  roo_scheduler::SchedulingService scheduler;
  TestStation native;
  OrderedInterface radio(native);
  MemoryStore store;
  store.enabled = true;
  Controller::Options options = RetryOptions();
  options.connect_timeout_ms = 15;
  Controller controller(radio, store, scheduler, options);
  controller.begin();
  Pump(scheduler);
  controller.connect(TestConfig(), {});
  Pump(scheduler);
  native.associated();
  native.ready();
  Pump(scheduler);
  native.emit({NativeStation::Event::kAddressLost});
  Pump(scheduler);
  scheduler.delay(roo_time::Millis(20));
  EXPECT_EQ(controller.state().status, Status::kTimeout);
  EXPECT_FALSE(controller.state().retry.pending);
  native.ready();
  Pump(scheduler);
  EXPECT_EQ(controller.state().status, Status::kOk);
  EXPECT_EQ(controller.state().station, Controller::StationPhase::kConnected);
  EXPECT_EQ(native.connects, 1);
}

}  // namespace
}  // namespace roo_wifi
