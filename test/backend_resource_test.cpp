#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "backend_fakes.h"
#include "gtest/gtest.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ROO_WIFI_ADDRESS_SANITIZER 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define ROO_WIFI_ADDRESS_SANITIZER 1
#endif

namespace {
#if !defined(ROO_WIFI_ADDRESS_SANITIZER)
std::atomic<size_t> live{0}, peak{0}, allocations{0};

/// Prefixes each tracked allocation with its requested byte count.
struct alignas(std::max_align_t) Allocation {
  size_t size;
};

/// Exercises the operations that may retain bounded controller resources.
void RunResourceCycle(roo_wifi::Controller& controller,
                      roo_wifi::TestStation& native,
                      roo_scheduler::Scheduler& scheduler) {
  controller.startScan();
  roo_wifi::Pump(scheduler);
  native.emit({roo_wifi::NativeStation::Event::kScanDone});
  roo_wifi::Pump(scheduler);
  controller.connect(roo_wifi::TestConfig(), {});
  controller.disconnect();
  roo_wifi::Pump(scheduler);
  roo_wifi::Profile profile;
  controller.loadProfile(roo_wifi::TestConfig().ssid, profile);
  scheduler.pruneCanceled();
}
#endif
}  // namespace

#if !defined(ROO_WIFI_ADDRESS_SANITIZER)
/// Tracks live heap bytes and allocation count for the resource regression.
void* operator new(size_t size) {
  Allocation* p =
      static_cast<Allocation*>(std::malloc(sizeof(Allocation) + size));
  if (p == nullptr) std::abort();
  p->size = size;
  size_t now = live.fetch_add(size) + size;
  size_t old = peak.load();
  while (now > old && !peak.compare_exchange_weak(old, now)) {
  }
  ++allocations;
  return p + 1;
}

void operator delete(void* data) noexcept {
  if (data == nullptr) return;
  Allocation* p = static_cast<Allocation*>(data) - 1;
  live -= p->size;
  std::free(p);
}

void operator delete(void* data, size_t) noexcept { ::operator delete(data); }

void* operator new[](size_t size) { return ::operator new(size); }

void operator delete[](void* data) noexcept { ::operator delete(data); }

void operator delete[](void* data, size_t) noexcept { ::operator delete(data); }
#endif

namespace roo_wifi {
// Verifies bounded retained allocation across repeated scan/cancel/profile
// work, and that all observation methods allocate zero bytes at N=0,20,40,100.
TEST(BackendResourceTest, RetainedPlateauAndAllocationFreeObservation) {
#if defined(ROO_WIFI_ADDRESS_SANITIZER)
  GTEST_SKIP() << "The allocation counter replaces global new/delete, which "
                  "is incompatible with AddressSanitizer's allocator.";
#else
  {
    for (uint16_t n : {0, 20, 40, 100}) {
      size_t baseline = live.load();
      peak = baseline;
      roo_scheduler::Scheduler scheduler;
      TestStation native;
      OrderedInterface radio(native);
      MemoryStore store;
      Controller::Options options;
      options.max_scan_results = n;
      Controller controller(radio, store, scheduler, options);
      store.enabled = true;
      controller.begin();
      Pump(scheduler);
      native.aps.resize(n + 1);
      ProfileSettings settings;
      settings.connection = TestConfig();
      CredentialUpdate update;
      update.intent = CredentialIntent::kClear;
      store.saveProfile(settings, update);
      for (int i = 0; i < 20; ++i) {
        RunResourceCycle(controller, native, scheduler);
      }
      size_t retained = live.load();
      for (int i = 0; i < 200; ++i) {
        RunResourceCycle(controller, native, scheduler);
      }
      size_t after = live.load();
      EXPECT_EQ(after, retained);
      size_t calls = allocations.load();
      for (int i = 0; i < 100; ++i) {
        controller.scanSnapshot();
        controller.linkState();
        controller.isEnabled();
        controller.isScanning();
        controller.support();
      }
      EXPECT_EQ(allocations.load(), calls);
      EXPECT_EQ(controller.scanSnapshot().count, n);
      EXPECT_TRUE(controller.scanSnapshot().truncated);
      std::printf(
          "N=%u ScanRecord=%zu Controller=%zu OrderedInterface=%zu "
          "retained=%zu "
          "peak=%zu\n",
          n, sizeof(ScanRecord), sizeof(Controller), sizeof(OrderedInterface),
          retained - baseline, peak.load() - baseline);
    }
  }
#endif
}
}  // namespace roo_wifi

// Verifies compact records retain defaults, flag independence, all cipher
// values, and byte-wide channel numbers when copied through the native queue.
TEST(RadioStorageTest, CompactRecordsPreserveValues) {
  using namespace roo_wifi;
  EXPECT_LE(sizeof(LinkState), 80u);
  EXPECT_EQ(sizeof(ScanRecord), 44u);
  EXPECT_LE(sizeof(NativeStation::Event), 96u);
  EXPECT_EQ(sizeof(NativeStation::Event::Kind), 1u);
  static_assert(static_cast<unsigned>(CipherType::kAesGmac256) < 16,
                "CipherType must fit in its four-bit fields");
  ScanRecord scan;
  EXPECT_EQ(scan.pairwise_cipher, CipherType::kUnknown);
  EXPECT_EQ(scan.group_cipher, CipherType::kUnknown);
  EXPECT_FALSE(scan.has_radio_metadata);
  EXPECT_FALSE(scan.use_11b);
  EXPECT_FALSE(scan.use_11g);
  EXPECT_FALSE(scan.use_11n);
  EXPECT_FALSE(scan.supports_wps);
  for (unsigned value = 0;
       value <= static_cast<unsigned>(CipherType::kAesGmac256); ++value) {
    scan.pairwise_cipher = static_cast<CipherType>(value);
    scan.group_cipher = CipherType::kAesGmac256;
    scan.has_radio_metadata = true;
    scan.use_11b = true;
    scan.use_11g = false;
    scan.use_11n = true;
    scan.supports_wps = true;
    scan.channel = 233;
    ScanRecord copy = scan;
    EXPECT_EQ(copy.pairwise_cipher, static_cast<CipherType>(value));
    EXPECT_EQ(copy.group_cipher, CipherType::kAesGmac256);
    EXPECT_TRUE(copy.has_radio_metadata);
    EXPECT_TRUE(copy.use_11b);
    EXPECT_FALSE(copy.use_11g);
    EXPECT_TRUE(copy.use_11n);
    EXPECT_TRUE(copy.supports_wps);
    EXPECT_EQ(copy.channel, 233);
  }
  LinkState link;
  EXPECT_EQ(link.phase, LinkPhase::kIdle);
  EXPECT_FALSE(link.has_radio_info);
  EXPECT_FALSE(link.has_station_mac);
  EXPECT_FALSE(link.has_ipv4);
  EXPECT_FALSE(link.has_dns1);
  EXPECT_FALSE(link.has_dns2);
  EXPECT_FALSE(link.has_native_code);
  for (unsigned flags = 0; flags < 64; ++flags) {
    link.has_radio_info = (flags & 1) != 0;
    link.has_station_mac = (flags & 2) != 0;
    link.has_ipv4 = (flags & 4) != 0;
    link.has_dns1 = (flags & 8) != 0;
    link.has_dns2 = (flags & 16) != 0;
    link.has_native_code = (flags & 32) != 0;
    link.channel = 255;
    link.native_code = -123;
    NativeStation::Event event{NativeStation::Event::kAssociated, link};
    EXPECT_EQ(event.link.has_radio_info, (flags & 1) != 0);
    EXPECT_EQ(event.link.has_station_mac, (flags & 2) != 0);
    EXPECT_EQ(event.link.has_ipv4, (flags & 4) != 0);
    EXPECT_EQ(event.link.has_dns1, (flags & 8) != 0);
    EXPECT_EQ(event.link.has_dns2, (flags & 16) != 0);
    EXPECT_EQ(event.link.has_native_code, (flags & 32) != 0);
    EXPECT_EQ(event.link.native_code, -123);
    EXPECT_EQ(event.link.channel, 255);
  }
}
