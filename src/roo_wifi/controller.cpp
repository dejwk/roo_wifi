#include "roo_wifi/controller.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace roo_wifi {
namespace {

// Compares meaningful configuration fields, excluding padding and unused DNS.
bool SameConfig(const ConnectionConfig& a, const ConnectionConfig& b) {
  return a.ssid.size == b.ssid.size &&
         memcmp(a.ssid.bytes, b.ssid.bytes, a.ssid.size) == 0 &&
         a.security == b.security && a.hidden == b.hidden &&
         a.ip_mode == b.ip_mode && a.mac_policy == b.mac_policy &&
         (a.ip_mode != IpMode::kStaticIpv4 ||
          (memcmp(a.static_ipv4.address.bytes, b.static_ipv4.address.bytes,
                  4) == 0 &&
           memcmp(a.static_ipv4.gateway.bytes, b.static_ipv4.gateway.bytes,
                  4) == 0 &&
           memcmp(a.static_ipv4.dns1.bytes, b.static_ipv4.dns1.bytes, 4) == 0 &&
           a.static_ipv4.prefix_length == b.static_ipv4.prefix_length &&
           a.static_ipv4.has_dns2 == b.static_ipv4.has_dns2 &&
           (!a.static_ipv4.has_dns2 ||
            memcmp(a.static_ipv4.dns2.bytes, b.static_ipv4.dns2.bytes, 4) ==
                0)));
}

}  // namespace

Controller::Controller(Interface& interface, Store& store,
                       roo_scheduler::SchedulerClient& scheduler)
    : Controller(interface, store, scheduler, Options{}) {}

Controller::Controller(Interface& interface, Store& store,
                       roo_scheduler::SchedulerClient& scheduler,
                       Options options)
    : interface_(interface),
      store_(store),
      scheduler_(scheduler),
      options_(options),
      work_(scheduler, [this] { execute(); }),
      timer_(scheduler, [this] { checkTimeouts(); }),
      reconnect_(scheduler, [this] { retry(); }),
      notify_(scheduler, [this] { notifyListeners(); }) {
  records_.resize(options.max_scan_results);
  jitter_state_ = static_cast<uint32_t>(roo_time::Uptime::Now().inMicros()) ^
                  static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this));
  if (jitter_state_ == 0) jitter_state_ = 1;
}

Controller::~Controller() { shutdown(); }

Status Controller::begin() {
  if (closed_) return Status::kNotStarted;
  if (running_) return Status::kBusy;
  Status status = store_.begin();
  if (status != Status::kOk) return status;
  bool enabled = false;
  status = store_.readEnabled(enabled);
  if (status != Status::kOk && status != Status::kNotFound) return status;
  status = interface_.begin(*this, scheduler_);
  if (status != Status::kOk) return status;
  running_ = true;
  state_.desired = enabled ? Target::kDisconnected : Target::kDisabled;
  restore_profile_ = enabled;
  changed(Change::kProfiles);
  newIntent();
  return Status::kOk;
}

void Controller::shutdown() {
  if (closed_) return;
  closed_ = true;
  work_.cancel();
  timer_.cancel();
  reconnect_.cancel();
  notify_.cancel();
  pending_changes_ = 0;
  if (running_) interface_.shutdown();
  running_ = false;
  credentials_ = {};
  snapshot_ = {};
  state_ = {};
  station_id_ = scan_id_ = 0;
  transition_ = Transition::kNone;
}

void Controller::addListener(Listener& listener) {
  if (std::find(listeners_.begin(), listeners_.end(), &listener) ==
      listeners_.end()) {
    listeners_.push_back(&listener);
  }
}

void Controller::removeListener(Listener& listener) {
  listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), &listener),
                   listeners_.end());
}

Controller::State Controller::state() const {
  State result = state_;
  if (!running_) return result;
  if (faulted_) {
    result.station = StationPhase::kFaulted;
  } else if (transition_ == Transition::kEnable) {
    result.station = transition_enabled_ ? StationPhase::kEnabling
                                         : StationPhase::kDisabling;
  } else if (transition_ == Transition::kDisconnect || cancelling_connect_) {
    result.station = StationPhase::kDisconnecting;
  } else if (!state_.enabled) {
    result.station = StationPhase::kDisabled;
  } else if (state_.link.phase == LinkPhase::kAddressReady) {
    result.station = StationPhase::kConnected;
  } else if (state_.link.phase == LinkPhase::kAssociated) {
    result.station = StationPhase::kAwaitingIp;
  } else if (transition_ == Transition::kConnect) {
    result.station = StationPhase::kConnecting;
  } else {
    result.station = StationPhase::kIdle;
  }
  return result;
}

Status Controller::admission() const {
  if (!running_ || faulted_) return Status::kNotStarted;
  if (next_id_ == std::numeric_limits<OperationId>::max()) return Status::kBusy;
  return Status::kOk;
}

void Controller::notifyListeners() {
  const uint8_t changes = pending_changes_;
  pending_changes_ = 0;
  if (!running_) return;
  for (Listener* listener : listeners_) {
    if (!running_) return;
    if ((changes & static_cast<uint8_t>(Change::kStation)) != 0) {
      listener->onStationStateChanged();
    }
    if (!running_) return;
    if ((changes & static_cast<uint8_t>(Change::kScan)) != 0) {
      listener->onScanStateChanged();
    }
    if (!running_) return;
    if ((changes & static_cast<uint8_t>(Change::kProfiles)) != 0) {
      listener->onProfilesChanged();
    }
  }
}

void Controller::changed(Change change) {
  if (!running_) return;
  pending_changes_ |= static_cast<uint8_t>(change);
  notify_.scheduleNow();
}

void Controller::newIntent() {
  ++state_.revision;
  state_.status = Status::kOk;
  state_.native_code = 0;
  state_.has_native_code = false;
  cancelRetry();
  state_.retry = {};
  retry_level_ = 0;
  stable_deadline_ = address_deadline_ = roo_time::Uptime::Max();
  changed(Change::kStation);
  work_.scheduleNow();
}

Status Controller::setEnabled(bool enabled) {
  Status status = admission();
  if (status != Status::kOk) return status;
  if (enabled == (state_.desired != Target::kDisabled) &&
      state_.status == Status::kOk) {
    return Status::kOk;
  }
  state_.desired = enabled ? Target::kDisconnected : Target::kDisabled;
  state_.desired_profile = {};
  credentials_ = {};
  auto_connect_ = false;
  restore_profile_ = enabled;
  newIntent();
  return Status::kOk;
}

Status Controller::requestConnection(const ConnectionConfig& config,
                                     const Credentials& credentials,
                                     const Ssid& profile, bool automatic) {
  Status status = admission();
  if (status != Status::kOk) return status;
  if (state_.desired == Target::kDisabled) return Status::kDisabled;
  status = Validate(config, credentials);
  if (status != Status::kOk) return status;
  status = ValidateSupport(config, support());
  if (status != Status::kOk) return status;
  if (state_.desired == Target::kConnected && state_.status == Status::kOk &&
      state_.desired_profile == profile && SameConfig(config_, config) &&
      credentials_.encoding == credentials.encoding &&
      credentials_.size == credentials.size &&
      memcmp(credentials_.bytes, credentials.bytes, credentials.size) == 0) {
    auto_connect_ = automatic;
    return Status::kOk;
  }
  config_ = config;
  credentials_ = credentials;
  state_.desired = Target::kConnected;
  state_.desired_profile = profile;
  auto_connect_ = automatic;
  restore_profile_ = false;
  newIntent();
  return Status::kOk;
}

Status Controller::connect(const ConnectionConfig& config,
                           const Credentials& credentials) {
  return requestConnection(config, credentials, {}, false);
}

Status Controller::connect(const Ssid& ssid) {
  Profile profile;
  Credentials credentials;
  Status status = loadProfile(ssid, profile);
  if (status != Status::kOk) return status;
  status = store_.loadCredentials(ssid, credentials);
  if (status != Status::kOk) return status;
  return requestConnection(profile.settings.connection, credentials, ssid,
                           profile.settings.auto_connect);
}

Status Controller::disconnect() {
  Status status = admission();
  if (status != Status::kOk) return status;
  restore_profile_ = false;
  auto_connect_ = false;
  if (state_.desired != Target::kConnected && state_.status == Status::kOk) {
    return Status::kOk;
  }
  if (state_.desired != Target::kDisabled) {
    state_.desired = Target::kDisconnected;
  }
  state_.desired_profile = {};
  credentials_ = {};
  newIntent();
  return Status::kOk;
}

Status Controller::startScan() {
  Status status = admission();
  if (status != Status::kOk) return status;
  if (state_.desired == Target::kDisabled || !state_.enabled) {
    return Status::kDisabled;
  }
  if (isScanning() || transition_ != Transition::kNone ||
      attempted_revision_ != state_.revision ||
      (state_.link.phase != LinkPhase::kIdle &&
       !support().scan_while_connected)) {
    return Status::kBusy;
  }
  state_.scan = ScanPhase::kQueued;
  state_.scan_status = Status::kOk;
  ++state_.scan_revision;
  scan_timed_out_ = false;
  changed(Change::kScan);
  work_.scheduleNow();
  return Status::kOk;
}

Status Controller::cancelScan() {
  if (!running_) return Status::kNotStarted;
  if (state_.scan == ScanPhase::kIdle ||
      state_.scan == ScanPhase::kCancelling) {
    return Status::kOk;
  }
  if (state_.scan == ScanPhase::kQueued) {
    state_.scan = ScanPhase::kIdle;
    state_.scan_status = Status::kCancelled;
  } else {
    Status status = interface_.cancelScan();
    if (status != Status::kOk) return status;
    state_.scan = ScanPhase::kCancelling;
    scan_deadline_ = roo_time::Uptime::Now() +
                     roo_time::Millis(options_.transition_timeout_ms);
  }
  changed(Change::kScan);
  work_.scheduleNow();
  scheduleTimeouts();
  return Status::kOk;
}

Status Controller::loadProfile(const Ssid& ssid, Profile& out) const {
  if (!running_) return Status::kNotStarted;
  return (ssid.size == 0 || ssid.size > sizeof(ssid.bytes))
             ? Status::kInvalidArgument
             : store_.loadProfile(ssid, out);
}

Status Controller::saveProfile(const ProfileSettings& settings,
                               const CredentialUpdate& credentials) {
  const Ssid& ssid = settings.connection.ssid;
  if (!running_) return Status::kNotStarted;
  if (ssid.size == 0 || ssid.size > sizeof(ssid.bytes)) {
    return Status::kInvalidArgument;
  }
  Status status = store_.saveProfile(settings, credentials);
  ++state_.profiles_generation;
  changed(Change::kProfiles);
  return status;
}

Status Controller::removeProfile(const Ssid& ssid) {
  if (!running_) return Status::kNotStarted;
  if (ssid.size == 0 || ssid.size > sizeof(ssid.bytes)) {
    return Status::kInvalidArgument;
  }
  Status status = store_.removeProfile(ssid);
  ++state_.profiles_generation;
  changed(Change::kProfiles);
  return status;
}

void Controller::restoreProfile() {
  restore_profile_ = false;
  Ssid ssid;
  Status status = store_.readLastProfile(ssid);
  if (status == Status::kNotFound) return;
  Profile profile;
  if (status == Status::kOk && ssid.size != 0) {
    status = loadProfile(ssid, profile);
  }
  if (status == Status::kOk && ssid.size != 0 &&
      profile.settings.auto_connect) {
    status = connect(ssid);
  }
  if (status != Status::kOk) {
    state_.status = status;
    changed(Change::kStation);
  }
}

void Controller::cancelRetry() {
  reconnect_.cancel();
  state_.retry.pending = false;
  state_.retry.next_attempt = roo_time::Uptime::Max();
}

uint32_t Controller::retryDelay() {
  const ConnectionFailure failure = state_.retry.failure;
  unsigned level = retry_level_;
  if (failure == ConnectionFailure::kIncompatibleSecurity ||
      (failure == ConnectionFailure::kAuthentication && level >= 2)) {
    level = 5;
  }
  const uint32_t base = std::max<uint32_t>(1, options_.retry.delays_ms[level]);
  const uint32_t spread = static_cast<uint64_t>(base) *
                          std::min<uint8_t>(50, options_.retry.jitter_percent) /
                          100;
  // A small per-controller generator avoids platform dependencies and global
  // random state. Retry timing is not a source of security-sensitive entropy.
  jitter_state_ ^= jitter_state_ << 13;
  jitter_state_ ^= jitter_state_ >> 17;
  jitter_state_ ^= jitter_state_ << 5;
  const int64_t delay =
      static_cast<int64_t>(base) - spread +
      (static_cast<uint64_t>(jitter_state_) % (2ULL * spread + 1));
  return static_cast<uint32_t>(std::min<int64_t>(
      std::numeric_limits<uint32_t>::max(), std::max<int64_t>(1, delay)));
}

void Controller::connectionFailed(Status status, ConnectionFailure failure,
                                  int32_t code, bool has_code) {
  state_.status = status;
  state_.native_code = code;
  state_.has_native_code = has_code;
  RetryState& retry = state_.retry;
  retry.last_status = status;
  retry.failure = failure == ConnectionFailure::kNone
                      ? ConnectionFailure::kTransient
                      : failure;
  retry.native_code = code;
  retry.has_native_code = has_code;
  retry.bssid = state_.link.bssid;
  retry.channel = state_.link.channel;
  retry.rssi_dbm = state_.link.rssi_dbm;
  retry.has_radio_info = state_.link.has_radio_info;
  stable_deadline_ = address_deadline_ = roo_time::Uptime::Max();
  if (auto_connect_ && state_.desired == Target::kConnected &&
      (status == Status::kConnectionFailed || status == Status::kTimeout)) {
    retry.pending = true;
    retry.next_attempt =
        roo_time::Uptime::Now() + roo_time::Millis(retryDelay());
    if (retry_level_ < 5) ++retry_level_;
    reconnect_.scheduleOn(retry.next_attempt);
  }
}

void Controller::retry() {
  cancelRetry();
  if (!running_ || faulted_ || state_.desired != Target::kConnected ||
      !auto_connect_) {
    return;
  }
  Profile profile;
  Status status = loadProfile(state_.desired_profile, profile);
  if (status != Status::kOk || !profile.settings.auto_connect) {
    auto_connect_ = false;
    if (status != Status::kOk) state_.status = status;
    changed(Change::kStation);
    return;
  }
  // Keep the accepted configuration snapshot; edits apply on explicit connect.
  attempted_revision_ = 0;
  state_.status = Status::kOk;
  state_.native_code = 0;
  state_.has_native_code = false;
  changed(Change::kStation);
  work_.scheduleNow();
}

void Controller::execute() {
  if (!running_ || faulted_) return;
  if (next_id_ == std::numeric_limits<OperationId>::max()) {
    fault(Status::kBusy);
    return;
  }
  if (transition_ == Transition::kConnect &&
      active_revision_ != state_.revision && !cancelling_connect_) {
    Status status = interface_.cancelConnect();
    if (status != Status::kOk) {
      fault(status);
      return;
    }
    cancelling_connect_ = true;
    station_deadline_ = roo_time::Uptime::Now() +
                        roo_time::Millis(options_.transition_timeout_ms);
    changed(Change::kStation);
  }
  if (transition_ != Transition::kNone) {
    scheduleTimeouts();
    return;
  }
  // Intent is a single replaceable target. Finish native teardown before
  // taking another configuration snapshot into the adapter.
  bool needs_station = !initialized_ || attempted_revision_ != state_.revision;
  if (needs_station && isScanning()) {
    Status status = cancelScan();
    if (status != Status::kOk) fault(status);
    if (isScanning() || faulted_) return;
  }
  if (needs_station && !state_.retry.pending) {
    active_revision_ = state_.revision;
    station_timed_out_ = false;
    if (state_.link.phase != LinkPhase::kIdle) {
      transition_ = Transition::kDisconnect;
    } else if (!initialized_ ||
               state_.enabled != (state_.desired != Target::kDisabled)) {
      transition_ = Transition::kEnable;
      transition_enabled_ = state_.desired != Target::kDisabled;
    } else if (restore_profile_) {
      restoreProfile();
      work_.scheduleNow();
      return;
    } else if (state_.desired == Target::kConnected) {
      transition_ = Transition::kConnect;
      if (state_.retry.attempt_count != std::numeric_limits<uint32_t>::max()) {
        ++state_.retry.attempt_count;
      }
    } else {
      attempted_revision_ = state_.revision;
      changed(Change::kStation);
    }
    if (transition_ != Transition::kNone) {
      station_id_ = next_id_++;
      station_deadline_ =
          roo_time::Uptime::Now() +
          roo_time::Millis(transition_ == Transition::kConnect
                               ? options_.connect_timeout_ms
                               : options_.transition_timeout_ms);
      Status status;
      OperationKind kind;
      if (transition_ == Transition::kEnable) {
        kind = OperationKind::kEnable;
        status = interface_.setEnabled(station_id_, transition_enabled_);
      } else if (transition_ == Transition::kConnect) {
        kind = OperationKind::kConnect;
        status = interface_.connect(station_id_, config_, credentials_);
      } else {
        kind = OperationKind::kDisconnect;
        status = interface_.disconnect(station_id_);
      }
      changed(Change::kStation);
      if (status != Status::kOk) {
        onOperationFinished({station_id_, kind, status});
      }
    }
  }
  if (transition_ == Transition::kNone && state_.scan == ScanPhase::kQueued) {
    scan_id_ = next_id_++;
    state_.scan = ScanPhase::kRunning;
    scan_deadline_ =
        roo_time::Uptime::Now() + roo_time::Millis(options_.scan_timeout_ms);
    Status status = interface_.startScan(scan_id_, options_.max_scan_results);
    changed(Change::kScan);
    if (status != Status::kOk) {
      onOperationFinished({scan_id_, OperationKind::kScan, status});
    }
  }
  scheduleTimeouts();
}

void Controller::fault(Status status) {
  faulted_ = true;
  state_.status = status;
  if (isScanning()) {
    state_.scan_status = status;
    state_.scan = ScanPhase::kIdle;
    changed(Change::kScan);
  }
  scan_id_ = station_id_ = 0;
  transition_ = Transition::kNone;
  credentials_ = {};
  timer_.cancel();
  cancelRetry();
  stable_deadline_ = address_deadline_ = roo_time::Uptime::Max();
  changed(Change::kStation);
}

void Controller::checkTimeouts() {
  if (!running_ || faulted_) return;
  const roo_time::Uptime now = roo_time::Uptime::Now();
  if (now >= stable_deadline_) {
    stable_deadline_ = roo_time::Uptime::Max();
    state_.retry = {};
    retry_level_ = 0;
    changed(Change::kStation);
  }
  if (now >= address_deadline_) {
    address_deadline_ = roo_time::Uptime::Max();
    connectionFailed(Status::kTimeout, ConnectionFailure::kAddressTimeout, 0,
                     false);
    changed(Change::kStation);
  }
  if (station_id_ != 0 && now >= station_deadline_) {
    if (transition_ != Transition::kConnect || cancelling_connect_) {
      fault(Status::kTimeout);
      return;
    }
    station_timed_out_ = true;
    timeout_failure_ = state_.link.phase == LinkPhase::kAssociated
                           ? ConnectionFailure::kAddressTimeout
                           : ConnectionFailure::kConnectionTimeout;
    Status status = interface_.cancelConnect();
    if (status != Status::kOk) {
      fault(Status::kTimeout);
      return;
    }
    cancelling_connect_ = true;
    station_deadline_ = roo_time::Uptime::Now() +
                        roo_time::Millis(options_.transition_timeout_ms);
    changed(Change::kStation);
  }
  if (scan_id_ != 0 && roo_time::Uptime::Now() >= scan_deadline_) {
    if (state_.scan == ScanPhase::kCancelling) {
      fault(Status::kTimeout);
      return;
    }
    scan_timed_out_ = true;
    if (cancelScan() != Status::kOk) {
      fault(Status::kTimeout);
      return;
    }
  }
  scheduleTimeouts();
}

void Controller::scheduleTimeouts() {
  roo_time::Uptime deadline = std::min(stable_deadline_, address_deadline_);
  if (station_id_ != 0 && station_deadline_ < deadline)
    deadline = station_deadline_;
  if (scan_id_ != 0 && scan_deadline_ < deadline) deadline = scan_deadline_;
  if (deadline == roo_time::Uptime::Max()) {
    timer_.cancel();
  } else {
    timer_.scheduleOn(deadline);
  }
}

void Controller::onOperationFinished(const OperationResult& result) {
  if (!running_ || faulted_ || result.id == 0) return;
  if (result.id == scan_id_ && result.kind == OperationKind::kScan) {
    Status status = scan_timed_out_ ? Status::kTimeout : result.status;
    if (state_.scan == ScanPhase::kCancelling && !scan_timed_out_) {
      status = Status::kCancelled;
    }
    if (status == Status::kOk) {
      Interface::ScanRead read;
      status =
          interface_.readScanResults(records_.data(), records_.size(), read);
      if (status == Status::kOk && read.count > records_.size()) {
        status = Status::kCorrupt;
      }
      if (status == Status::kOk) {
        snapshot_ = {snapshot_.generation + 1, records_.data(), read.count,
                     read.truncated};
      }
    }
    scan_id_ = 0;
    state_.scan = ScanPhase::kIdle;
    state_.scan_status = status;
    changed(Change::kScan);
    work_.scheduleNow();
    scheduleTimeouts();
    return;
  }
  if (result.id != station_id_) return;
  OperationKind expected =
      transition_ == Transition::kEnable    ? OperationKind::kEnable
      : transition_ == Transition::kConnect ? OperationKind::kConnect
                                            : OperationKind::kDisconnect;
  if (result.kind != expected) return;
  Transition completed = transition_;
  bool superseded = active_revision_ != state_.revision;
  Status status = station_timed_out_ ? Status::kTimeout : result.status;
  station_id_ = 0;
  transition_ = Transition::kNone;
  cancelling_connect_ = false;
  if (completed == Transition::kEnable && status == Status::kOk) {
    initialized_ = true;
    status = store_.writeEnabled(state_.enabled);
  }
  if (!superseded) {
    if (completed == Transition::kConnect && status == Status::kOk) {
      state_.connected_profile = state_.desired_profile;
      if (state_.desired_profile.size != 0) {
        status = store_.writeLastProfile(state_.desired_profile);
      }
    }
    state_.status = status;
    state_.native_code = result.native_code;
    state_.has_native_code = result.has_native_code;
    if (completed == Transition::kConnect || status != Status::kOk) {
      attempted_revision_ = state_.revision;
    }
    if (completed == Transition::kConnect && status != Status::kOk &&
        status != Status::kCancelled) {
      connectionFailed(status,
                       station_timed_out_ ? timeout_failure_ : result.failure,
                       result.native_code, result.has_native_code);
    }
  }
  changed(Change::kStation);
  work_.scheduleNow();
  scheduleTimeouts();
}

void Controller::onEnabledChanged(bool enabled) {
  if (!running_ || faulted_) return;
  state_.enabled = enabled;
  changed(Change::kStation);
}

void Controller::onLinkChanged(const LinkState& link) {
  if (!running_ || faulted_) return;
  if (link.connection_id != state_.link.connection_id &&
      !(transition_ == Transition::kConnect &&
        link.connection_id == station_id_)) {
    return;
  }
  const LinkPhase previous = state_.link.phase;
  state_.link = link;
  const roo_time::Uptime now = roo_time::Uptime::Now();
  if (link.phase == LinkPhase::kAddressReady) {
    address_deadline_ = roo_time::Uptime::Max();
    if (previous != LinkPhase::kAddressReady) {
      stable_deadline_ =
          now + roo_time::Millis(options_.retry.stable_connection_ms);
    }
    // DHCP can recover while waiting to retry. Keep that recovered link.
    if (transition_ == Transition::kNone &&
        (state_.retry.pending ||
         (previous != LinkPhase::kAddressReady &&
          state_.retry.failure == ConnectionFailure::kAddressTimeout))) {
      cancelRetry();
      attempted_revision_ = state_.revision;
      state_.status = Status::kOk;
      state_.has_native_code = false;
      state_.native_code = 0;
    }
  } else {
    stable_deadline_ = roo_time::Uptime::Max();
    if (previous == LinkPhase::kAddressReady &&
        link.phase == LinkPhase::kAssociated &&
        transition_ == Transition::kNone) {
      address_deadline_ = now + roo_time::Millis(options_.connect_timeout_ms);
    }
  }
  if (link.phase == LinkPhase::kIdle) {
    address_deadline_ = roo_time::Uptime::Max();
    state_.connected_profile = {};
    if (transition_ == Transition::kNone && previous != LinkPhase::kIdle &&
        state_.desired == Target::kConnected) {
      const Status status =
          link.reason == Status::kOk ? Status::kConnectionFailed : link.reason;
      connectionFailed(status, link.failure, link.native_code,
                       link.has_native_code);
    }
  }
  changed(Change::kStation);
  scheduleTimeouts();
}

}  // namespace roo_wifi
