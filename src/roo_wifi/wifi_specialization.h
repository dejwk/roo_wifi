#pragma once

#include <type_traits>
#include <utility>

#include "roo_wifi/controller.h"

namespace roo_wifi {
namespace internal {

/// Owns platform dependencies that must outlive a controller base class.
template <typename Store, typename PlatformInterface>
class WiFiSpecializationResources {
 protected:
  WiFiSpecializationResources() = default;

  template <typename StoreInitializer>
  explicit WiFiSpecializationResources(StoreInitializer&& initializer)
      : platform_store_(std::forward<StoreInitializer>(initializer)) {}

  Store platform_store_;
  PlatformInterface platform_interface_;
};

}  // namespace internal

/// Combines platform storage and radio adapters with the portable controller
/// API.
template <typename Store, typename PlatformInterface>
class WiFiSpecialization
    : private internal::WiFiSpecializationResources<Store, PlatformInterface>,
      public Controller {
 public:
  /// Creates a Wi-Fi controller using @p scheduler and @p options.
  /// Owns the platform radio and storage adapters; borrows the scheduler.
  /// @param scheduler Context on which controller calls and callbacks run.
  /// @param options Scan capacity and native transition timeout settings.
  explicit WiFiSpecialization(roo_scheduler::Scheduler& scheduler,
                              Controller::Options options = {})
      : Resources(),
        Controller(this->platform_interface_, this->platform_store_, scheduler,
                   options) {}

  /// Creates a Wi-Fi controller using @p scheduler, @p initializer, and
  /// @p options. Constructs its storage adapter from the supplied backend.
  /// The backend and borrowed scheduler must outlive this controller.
  /// @param scheduler Context on which controller calls and callbacks run.
  /// @param initializer Backend used to construct the platform storage adapter.
  /// @param options Scan capacity and native transition timeout settings.
  template <typename StoreInitializer,
            typename std::enable_if<
                std::is_constructible<Store, StoreInitializer&&>::value,
                int>::type = 0>
  WiFiSpecialization(roo_scheduler::Scheduler& scheduler,
                     StoreInitializer&& initializer,
                     Controller::Options options = {})
      : Resources(std::forward<StoreInitializer>(initializer)),
        Controller(this->platform_interface_, this->platform_store_, scheduler,
                   options) {}

 private:
  using Resources =
      internal::WiFiSpecializationResources<Store, PlatformInterface>;
};

}  // namespace roo_wifi
