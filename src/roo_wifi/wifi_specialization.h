#pragma once

#include "roo_wifi/controller.h"

namespace roo_wifi {
namespace internal {

/// Owns platform dependencies that must outlive a controller base class.
template <typename Store, typename PlatformInterface>
class WiFiSpecializationResources {
 protected:
  Store platform_store_;
  PlatformInterface platform_interface_;
};

}  // namespace internal

/// Combines default-constructible platform storage and radio adapters with the
/// portable controller API.
template <typename Store, typename PlatformInterface>
class WiFiSpecialization
    : private internal::WiFiSpecializationResources<Store, PlatformInterface>,
      public Controller {
 public:
  /// Creates the platform dependencies and their controller facade.
  /// @param scheduler Context on which controller calls and callbacks run.
  /// @param options Capacity, timeout, and startup behavior.
  explicit WiFiSpecialization(roo_scheduler::Scheduler &scheduler,
                              Controller::Options options = {})
      : Controller(this->platform_interface_, this->platform_store_, scheduler,
                   options) {}
};

}  // namespace roo_wifi
