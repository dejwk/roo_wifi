#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

#include "backend_fakes.h"
#include "gtest/gtest.h"

namespace {
std::atomic<size_t> live{0}, peak{0}, allocations{0};

struct alignas(std::max_align_t) Allocation {
  size_t size;
};
}  // namespace

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

namespace roo_wifi {
// Verifies bounded retained allocation across repeated scan/cancel/profile
// work, and that all observation methods allocate zero bytes at N=0,20,40,100.
TEST(BackendResourceTest, RetainedPlateauAndAllocationFreeObservation) {
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
    store.saveProfile(1, settings, update);
    auto cycle = [&] {
      controller.scan();
      Pump(scheduler);
      native.emit({NativeStation::Event::kScanDone});
      Pump(scheduler);
      Controller::RequestResult request = controller.connect(TestConfig(), {});
      controller.cancel(request.id);
      Pump(scheduler);
      Profile profile;
      controller.loadProfile(1, profile);
      scheduler.pruneCanceled();
    };
    for (int i = 0; i < 20; ++i) cycle();
    size_t retained = live.load();
    for (int i = 0; i < 200; ++i) cycle();
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
        "N=%u ScanRecord=%zu Controller=%zu OrderedInterface=%zu retained=%zu "
        "peak=%zu\n",
        n, sizeof(ScanRecord), sizeof(Controller), sizeof(OrderedInterface),
        retained - baseline, peak.load() - baseline);
  }
}
}  // namespace roo_wifi
