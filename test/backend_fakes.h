#pragma once
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "roo_wifi.h"
#include "roo_wifi/hal/field_store.h"
#include "roo_wifi/hal/ordered_interface.h"
namespace roo_wifi {
inline ConnectionConfig TestConfig(const char *name = "network") {
  ConnectionConfig config;
  config.ssid.size = strlen(name);
  memcpy(config.ssid.bytes, name, config.ssid.size);
  config.security = AuthMode::kOpen;
  return config;
}
class MemoryStore : public FieldStore {
 public:
  Error begin() override { return Error::kOk; }
  Error readEnabled(bool &out) const override {
    out = enabled;
    return Error::kOk;
  }
  Error writeEnabled(bool value) override {
    if (enabled_error != Error::kOk) return enabled_error;
    enabled = value;
    return Error::kOk;
  }
  Error readField(const char *key, uint8_t *out, size_t &size) const override {
    auto it = values.find(key);
    if (it == values.end()) return Error::kNotFound;
    if (it->second.size() > size) return Error::kCorrupt;
    size = it->second.size();
    memcpy(out, it->second.data(), size);
    return Error::kOk;
  }
  Error writeField(const char *key, const uint8_t *data, size_t size) override {
    if (++writes == fail_at) return Error::kStorageFailure;
    if (size > 64 || strlen(key) > 15) return Error::kInvalidArgument;
    values[key] = std::vector<uint8_t>(data, data + size);
    return Error::kOk;
  }
  Error eraseField(const char *key) override {
    if (++writes == fail_at) return Error::kStorageFailure;
    values.erase(key);
    return Error::kOk;
  }
  std::map<std::string, std::vector<uint8_t>> values;
  int writes = 0, fail_at = -1;
  bool enabled = false;
  Error enabled_error = Error::kOk;
};
class TestStation : public NativeStation {
 public:
  Error attach(Receiver &receiver) override {
    receiver_ = &receiver;
    return Error::kOk;
  }
  void detach() override { receiver_ = nullptr; }
  Support support() const override {
    return {0xffffffffu, true, true, true, true};
  }
  Error enable(bool enabled) override {
    emit({enabled ? Event::kEnabled : Event::kDisabled});
    return Error::kOk;
  }
  Error scan(uint16_t) override {
    ++scans;
    return Error::kOk;
  }
  Error stopScan() override {
    ++scan_stops;
    return Error::kOk;
  }
  Error connect(const ConnectionConfig &config,
                const Credentials &secret) override {
    ++connects;
    last_config = config;
    last_secret = secret;
    return rejection;
  }
  Error continueConnect() override { return Error::kOk; }
  Error disconnect() override {
    ++disconnects;
    return Error::kOk;
  }
  Error readScan(ScanRecord *out, size_t capacity,
                 ScanRead &result) const override {
    if (read_error != Error::kOk) return read_error;
    size_t n = std::min(capacity, aps.size());
    std::copy_n(aps.begin(), n, out);
    result = {n, n < aps.size()};
    return Error::kOk;
  }
  void emit(Event event) {
    if (receiver_) receiver_->post(event);
  }
  void associated() {
    Event e{};
    e.kind = Event::kAssociated;
    e.link.ssid = last_config.ssid;
    e.link.security = last_config.security;
    emit(e);
  }
  void ready() {
    Event e{};
    e.kind = Event::kAddressReady;
    e.link.has_ipv4 = true;
    e.link.address = {{192, 168, 1, 2}};
    emit(e);
  }
  void disconnected() {
    Event e{};
    e.kind = Event::kDisconnected;
    e.link.ssid = last_config.ssid;
    emit(e);
  }
  Receiver *receiver_ = nullptr;
  int scans = 0, scan_stops = 0, connects = 0, disconnects = 0;
  ConnectionConfig last_config;
  Credentials last_secret;
  Error rejection = Error::kOk, read_error = Error::kOk;
  std::vector<ScanRecord> aps;
};
class Observer : public Controller::Listener {
 public:
  void onOperationFinished(const OperationResult &result) override {
    results.push_back(result);
  }
  void onLinkChanged(const LinkState &link) override { links.push_back(link); }
  std::vector<OperationResult> results;
  std::vector<LinkState> links;
};
inline void Pump(roo_scheduler::Scheduler &scheduler) {
  for (int i = 0; i < 12; ++i) scheduler.executeEligibleTasks();
}
}  // namespace roo_wifi
