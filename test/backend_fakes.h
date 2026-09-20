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
  Status begin() override { return Status::kOk; }

  Status readEnabled(bool &out) const override {
    out = enabled;
    return Status::kOk;
  }

  Status writeEnabled(bool value) override {
    if (enabled_error != Status::kOk) return enabled_error;
    enabled = value;
    return Status::kOk;
  }

  Status readField(const char *key, uint8_t *out, size_t &size) const override {
    auto it = values.find(key);
    if (it == values.end()) return Status::kNotFound;
    if (it->second.size() > size) return Status::kCorrupt;
    size = it->second.size();
    memcpy(out, it->second.data(), size);
    return Status::kOk;
  }

  Status writeField(const char *key, const uint8_t *data,
                    size_t size) override {
    if (++writes == fail_at) return Status::kStorageFailure;
    if (size > 64 || strlen(key) > 15) return Status::kInvalidArgument;
    values[key] = std::vector<uint8_t>(data, data + size);
    return Status::kOk;
  }

  Status eraseField(const char *key) override {
    if (++writes == fail_at) return Status::kStorageFailure;
    values.erase(key);
    return Status::kOk;
  }

  std::map<std::string, std::vector<uint8_t>> values;
  int writes = 0;
  int fail_at = -1;
  bool enabled = false;
  Status enabled_error = Status::kOk;
};

class TestStation : public NativeStation {
 public:
  Status attach(Receiver &receiver) override {
    receiver_ = &receiver;
    return Status::kOk;
  }

  void detach() override { receiver_ = nullptr; }

  Support support() const override {
    return {0xffffffffu, true, true, true, true};
  }

  Status enable(bool enabled) override {
    emit({enabled ? Event::kEnabled : Event::kDisabled});
    return Status::kOk;
  }

  Status scan(uint16_t) override {
    ++scans;
    return Status::kOk;
  }

  Status stopScan() override {
    ++scan_stops;
    return Status::kOk;
  }

  Status connect(const ConnectionConfig &config,
                 const Credentials &secret) override {
    ++connects;
    last_config = config;
    last_secret = secret;
    return rejection;
  }

  Status continueConnect() override { return Status::kOk; }

  Status disconnect() override {
    ++disconnects;
    return Status::kOk;
  }

  Status readScan(ScanRecord *out, size_t capacity,
                  ScanRead &result) const override {
    if (read_error != Status::kOk) return read_error;
    size_t n = std::min(capacity, aps.size());
    std::copy_n(aps.begin(), n, out);
    result = {n, n < aps.size()};
    return Status::kOk;
  }

  void emit(Event event) {
    if (receiver_ != nullptr) receiver_->post(event);
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
