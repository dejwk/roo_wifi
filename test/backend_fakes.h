#pragma once
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "roo_wifi.h"
#include "roo_wifi/hal/field_store.h"
#include "roo_wifi/hal/ordered_interface.h"

namespace roo_wifi {
/// Creates a portable open-network configuration for tests.
inline ConnectionConfig TestConfig(const char *name = "network") {
  ConnectionConfig config;
  config.ssid.size = strlen(name);
  memcpy(config.ssid.bytes, name, config.ssid.size);
  config.security = AuthMode::kOpen;
  return config;
}

/// Provides deterministic in-memory field persistence for controller tests.
class MemoryStore : public FieldStore {
 public:
  /// Opens the always-available in-memory store.
  Status begin() override { return Status::kOk; }

  /// Returns the configured persisted enablement value.
  Status readEnabled(bool &out) const override {
    out = enabled;
    return Status::kOk;
  }

  /// Stores enablement unless the configured failure is active.
  Status writeEnabled(bool value) override {
    if (enabled_error != Status::kOk) return enabled_error;
    enabled = value;
    return Status::kOk;
  }

  /// Copies a named field from the in-memory map.
  Status readField(const char *key, uint8_t *out, size_t &size) const override {
    auto it = values.find(key);
    if (it == values.end()) return Status::kNotFound;
    if (it->second.size() > size) return Status::kCorrupt;
    size = it->second.size();
    memcpy(out, it->second.data(), size);
    return Status::kOk;
  }

  /// Copies a named field into the in-memory map.
  Status writeField(const char *key, const uint8_t *data,
                    size_t size) override {
    if (++writes == fail_at) return Status::kStorageFailure;
    if (size > 64 || strlen(key) > 15) return Status::kInvalidArgument;
    values[key] = std::vector<uint8_t>(data, data + size);
    return Status::kOk;
  }

  /// Removes a named field from the in-memory map.
  Status eraseField(const char *key) override {
    if (++writes == fail_at) return Status::kStorageFailure;
    values.erase(key);
    return Status::kOk;
  }

  /// Visits every field key in deterministic map order.
  Status enumerateFields(FieldVisitor visitor, void *context) const override {
    for (const auto &entry : values) {
      if (!visitor(context, entry.first.data(), entry.first.size())) {
        return Status::kStopped;
      }
    }
    return Status::kOk;
  }

  std::map<std::string, std::vector<uint8_t>> values;
  int writes = 0;
  int fail_at = -1;
  bool enabled = false;
  Status enabled_error = Status::kOk;
};

/// Provides a manually driven native station for ordered-interface tests.
class TestStation : public NativeStation {
 public:
  /// Attaches the receiver that accepts manually emitted events.
  Status attach(Receiver &receiver) override {
    receiver_ = &receiver;
    return Status::kOk;
  }

  /// Detaches the current event receiver.
  void detach() override { receiver_ = nullptr; }

  /// Reports support for every portable feature used by the tests.
  Support support() const override {
    return {0xffffffffu, true, true, true, true};
  }

  /// Emits the requested physical enablement state.
  Status enable(bool enabled) override {
    emit({enabled ? Event::kEnabled : Event::kDisabled});
    return Status::kOk;
  }

  /// Records admission of a native scan.
  Status scan(uint16_t) override {
    ++scans;
    return Status::kOk;
  }

  /// Records cancellation of a native scan.
  Status stopScan() override {
    ++scan_stops;
    return Status::kOk;
  }

  /// Records connection inputs and returns the configured outcome.
  Status connect(const ConnectionConfig &config,
                 const Credentials &secret) override {
    ++connects;
    last_config = config;
    last_secret = secret;
    return rejection;
  }

  /// Accepts continuation of a prepared connection.
  Status continueConnect() override { return Status::kOk; }

  /// Records admission of a native disconnect.
  Status disconnect() override {
    ++disconnects;
    return Status::kOk;
  }

  /// Copies the bounded set of configured scan records.
  Status readScan(ScanRecord *out, size_t capacity,
                  ScanRead &result) const override {
    if (read_error != Status::kOk) return read_error;
    size_t n = std::min(capacity, aps.size());
    std::copy_n(aps.begin(), n, out);
    result = {n, n < aps.size()};
    return Status::kOk;
  }

  /// Delivers one native event to the attached receiver, when present.
  void emit(Event event) {
    if (receiver_ != nullptr) receiver_->post(event);
  }

  /// Emits association using the most recent connection configuration.
  void associated() {
    Event e{};
    e.kind = Event::kAssociated;
    e.link.ssid = last_config.ssid;
    e.link.security = last_config.security;
    emit(e);
  }

  /// Emits IPv4 address readiness for the active connection.
  void ready() {
    Event e{};
    e.kind = Event::kAddressReady;
    e.link.has_ipv4 = true;
    e.link.address = {{192, 168, 1, 2}};
    emit(e);
  }

  /// Emits disconnection from the most recently configured network.
  void disconnected() {
    Event e{};
    e.kind = Event::kDisconnected;
    e.link.ssid = last_config.ssid;
    emit(e);
  }

  Receiver *receiver_ = nullptr;
  int scans = 0;
  int scan_stops = 0;
  int connects = 0;
  int disconnects = 0;
  ConnectionConfig last_config;
  Credentials last_secret;
  Status rejection = Status::kOk;
  Status read_error = Status::kOk;
  std::vector<ScanRecord> aps;
};

/// Collects controller operation and link notifications for assertions.
class Observer : public Controller::Listener {
 public:
  /// Retains a delivered terminal operation result.
  void onOperationFinished(const OperationResult &result) override {
    results.push_back(result);
  }

  /// Retains a delivered link-state publication.
  void onLinkChanged(const LinkState &link) override { links.push_back(link); }

  std::vector<OperationResult> results;
  std::vector<LinkState> links;
};

/// Executes enough eligible tasks to settle the test fakes' deferred work.
inline void Pump(roo_scheduler::Scheduler &scheduler) {
  for (int i = 0; i < 12; ++i) scheduler.executeEligibleTasks();
}
}  // namespace roo_wifi
