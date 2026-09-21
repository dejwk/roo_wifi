#include "roo_wifi/controller.h"

#include <algorithm>
#include <limits>

namespace roo_wifi {
Controller::Controller(Interface& interface, Store& store,
                       roo_scheduler::Scheduler& scheduler)
    : Controller(interface, store, scheduler, Options{}) {}

Controller::Controller(Interface& interface, Store& store,
                       roo_scheduler::Scheduler& scheduler, Options options)
    : interface_(interface),
      store_(store),
      scheduler_(scheduler),
      options_(options),
      work_(scheduler, [this] { execute(); }),
      timer_(scheduler, [this] { checkTimeouts(); }),
      reconnect_(scheduler, [this] { startProfile(); }) {
  records_.resize(options.max_scan_results);
}

Controller::~Controller() { close(false); }

Status Controller::begin() {
  if (lifecycle_ == Lifecycle::kClosed) return Status::kNotStarted;
  if (lifecycle_ == Lifecycle::kRunning) return Status::kBusy;
  Status status = store_.begin();
  if (status != Status::kOk) return status;
  bool enabled = false;
  status = store_.readEnabled(enabled);
  if (status != Status::kOk && status != Status::kNotFound) return status;
  status = interface_.begin(*this, scheduler_);
  if (status != Status::kOk) return status;
  lifecycle_ = Lifecycle::kRunning;
  return setEnabled(enabled).status;
}

void Controller::close(bool notify) {
  if (lifecycle_ == Lifecycle::kClosed) return;
  bool running = lifecycle_ == Lifecycle::kRunning;
  lifecycle_ = Lifecycle::kClosed;
  work_.cancel();
  timer_.cancel();
  reconnect_.cancel();
  if (running) interface_.shutdown();
  if (notify) {
    for (Slot* slot : {&station_, &scan_, &write_}) {
      if (slot->result.id != 0) finish(*slot, Status::kCancelled);
    }
  }
  station_ = {};
  scan_ = {};
  write_ = {};
  credentials_ = {};
  update_ = {};
  snapshot_ = {};
  link_ = {};
  enabled_ = false;
}

void Controller::shutdown() { close(true); }

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

Support Controller::support() const { return interface_.support(); }

bool Controller::isEnabled() const { return enabled_; }

bool Controller::isScanning() const { return scan_.result.id != 0; }

Controller::ScanSnapshot Controller::scanSnapshot() const { return snapshot_; }

LinkState Controller::linkState() const { return link_; }

Status Controller::loadProfile(ProfileId id, Profile& out) const {
  if (lifecycle_ != Lifecycle::kRunning) return Status::kNotStarted;
  if (id == 0) return Status::kInvalidArgument;
  return store_.loadProfile(id, out);
}

Status Controller::radioAdmission() const {
  if (lifecycle_ != Lifecycle::kRunning) return Status::kNotStarted;
  if (faulted_) return Status::kNotStarted;
  return Status::kOk;
}

Controller::RequestResult Controller::admit(Slot& slot, OperationKind kind,
                                            ProfileId profile) {
  if (lifecycle_ != Lifecycle::kRunning) return {0, Status::kNotStarted};
  if (slot.result.id != 0) return {0, Status::kBusy};
  if (next_id_ == std::numeric_limits<OperationId>::max()) {
    return {0, Status::kBusy};
  }
  slot = {};
  slot.result = {next_id_++, kind, Status::kOk, profile};
  work_.scheduleNow();
  return {slot.result.id, Status::kOk};
}

Controller::RequestResult Controller::setEnabled(bool enabled) {
  Status status = radioAdmission();
  if (status != Status::kOk) return {0, status};
  if (scan_.result.id != 0) return {0, Status::kBusy};
  RequestResult result = admit(station_, OperationKind::kEnable);
  if (result.id != 0) {
    desired_enabled_ = enabled;
    reconnect_.cancel();
    reconnect_profile_ = 0;
  }
  return result;
}

Controller::RequestResult Controller::scan() {
  Status status = radioAdmission();
  if (status != Status::kOk) return {0, status};
  if (!enabled_) return {0, Status::kDisabled};
  if (station_.result.id != 0 ||
      (link_.phase != LinkPhase::kIdle && !support().scan_while_connected)) {
    return {0, Status::kBusy};
  }
  return admit(scan_, OperationKind::kScan);
}

Controller::RequestResult Controller::connect(const ConnectionConfig& config,
                                              const Credentials& credential) {
  Status status = radioAdmission();
  if (status != Status::kOk) return {0, status};
  if (!enabled_) return {0, Status::kDisabled};
  if (scan_.result.id != 0) return {0, Status::kBusy};
  status = Validate(config, credential);
  if (status != Status::kOk) return {0, status};
  status = ValidateSupport(config, support());
  if (status != Status::kOk) return {0, status};
  RequestResult result = admit(station_, OperationKind::kConnect);
  if (result.id != 0) {
    config_ = config;
    credentials_ = credential;
    reconnect_profile_ = 0;
    reconnect_.cancel();
  }
  return result;
}

Controller::RequestResult Controller::connect(ProfileId id) {
  Profile profile;
  Credentials credential;
  Status status = loadProfile(id, profile);
  if (status != Status::kOk) return {0, status};
  status = store_.loadCredentials(id, credential);
  if (status != Status::kOk) return {0, status};
  RequestResult result = connect(profile.settings.connection, credential);
  if (result.id != 0) {
    station_.result.profile_id = id;
    reconnect_profile_ = profile.settings.auto_connect ? id : 0;
  }
  return result;
}

Controller::RequestResult Controller::disconnect() {
  Status status = radioAdmission();
  if (status != Status::kOk) return {0, status};
  RequestResult result = admit(station_, OperationKind::kDisconnect);
  if (result.id != 0) {
    reconnect_profile_ = 0;
    reconnect_.cancel();
  }
  return result;
}

Controller::RequestResult Controller::saveProfile(
    ProfileId id, const ProfileSettings& settings,
    const CredentialUpdate& credential) {
  if (id == 0) return {0, Status::kInvalidArgument};
  RequestResult result = admit(write_, OperationKind::kSave, id);
  if (result.id != 0) {
    settings_ = settings;
    update_ = credential;
  }
  return result;
}

Controller::RequestResult Controller::removeProfile(ProfileId id) {
  if (id == 0) return {0, Status::kInvalidArgument};
  return admit(write_, OperationKind::kRemove, id);
}

Controller::Slot* Controller::find(OperationId id) {
  if (id == 0) return nullptr;
  for (Slot* slot : {&station_, &scan_, &write_})
    if (slot->result.id == id) return slot;
  return nullptr;
}

Status Controller::cancel(OperationId id) {
  Slot* slot = find(id);
  if (slot == nullptr) return Status::kNotFound;
  if (slot == &write_ && slot->state != Slot::State::kQueued)
    return Status::kBusy;
  if (slot->state == Slot::State::kCancelling ||
      slot->state == Slot::State::kTimingOut)
    return Status::kOk;
  if (slot->state == Slot::State::kRunning) {
    Status status = interface_.cancel(id);
    if (status != Status::kOk) return status;
    slot->deadline = roo_time::Uptime::Now() +
                     roo_time::Millis(options_.transition_timeout_ms);
  }
  slot->state = slot->state == Slot::State::kQueued
                    ? Slot::State::kCancelledBeforeStart
                    : Slot::State::kCancelling;
  if (slot == &station_) {
    reconnect_profile_ = 0;
    reconnect_.cancel();
  }
  work_.scheduleNow();
  return Status::kOk;
}

void Controller::execute() {
  if (lifecycle_ == Lifecycle::kClosed) return;
  // Capture IDs: a listener's follow-up admission must wait for the next task.
  OperationId ids[] = {station_.result.id, scan_.result.id, write_.result.id};
  for (OperationId id : ids) {
    Slot* slot = find(id);
    if (slot == nullptr || slot->state == Slot::State::kRunning ||
        slot->state == Slot::State::kCancelling ||
        slot->state == Slot::State::kTimingOut) {
      continue;
    }
    if (slot->state == Slot::State::kCancelledBeforeStart) {
      finish(*slot, Status::kCancelled);
      continue;
    }
    slot->state = Slot::State::kRunning;
    Status status = Status::kOk;
    switch (slot->result.kind) {
      case OperationKind::kSave: {
        status =
            store_.saveProfile(slot->result.profile_id, settings_, update_);
        update_ = {};
        break;
      }
      case OperationKind::kRemove: {
        status = store_.removeProfile(slot->result.profile_id);
        break;
      }
      case OperationKind::kEnable: {
        status = interface_.setEnabled(id, desired_enabled_);
        break;
      }
      case OperationKind::kConnect: {
        status = interface_.connect(id, config_, credentials_);
        credentials_ = {};
        break;
      }
      case OperationKind::kDisconnect: {
        status = interface_.disconnect(id);
        break;
      }
      case OperationKind::kScan: {
        for (Listener* listener : listeners_) {
          listener->onScanStateChanged(true);
        }
        status = interface_.scan(id, options_.max_scan_results);
        break;
      }
    }
    if (slot == &write_ || status != Status::kOk) {
      finish(*slot, status);
      continue;
    }
    uint32_t timeout = slot == &scan_ ? options_.scan_timeout_ms
                       : slot->result.kind == OperationKind::kConnect
                           ? options_.connect_timeout_ms
                           : options_.transition_timeout_ms;
    slot->deadline = roo_time::Uptime::Now() + roo_time::Millis(timeout);
  }
  scheduleTimeoutCheck();
}

void Controller::checkTimeouts() {
  if (lifecycle_ == Lifecycle::kClosed) return;
  for (Slot* slot : {&station_, &scan_}) {
    if (slot->result.id == 0 || slot->state == Slot::State::kQueued ||
        roo_time::Uptime::Now() < slot->deadline) {
      continue;
    }
    if (slot->state == Slot::State::kTimingOut ||
        slot->state == Slot::State::kCancelling) {
      faulted_ = true;
      reconnect_profile_ = 0;
      finish(*slot, Status::kTimeout);
    } else {
      slot->state = Slot::State::kTimingOut;
      slot->deadline = roo_time::Uptime::Now() +
                       roo_time::Millis(options_.transition_timeout_ms);
      interface_.cancel(slot->result.id);
    }
  }
  scheduleTimeoutCheck();
}

void Controller::scheduleTimeoutCheck() {
  roo_time::Uptime deadline = roo_time::Uptime::Max();
  for (Slot* slot : {&station_, &scan_}) {
    if (slot->result.id != 0 && slot->state != Slot::State::kQueued &&
        slot->state != Slot::State::kCancelledBeforeStart &&
        slot->deadline < deadline) {
      deadline = slot->deadline;
    }
  }
  if (deadline == roo_time::Uptime::Max()) {
    timer_.cancel();
  } else {
    timer_.scheduleOn(deadline);
  }
}

void Controller::finish(Slot& slot, Status status, int32_t native,
                        bool has_native) {
  OperationResult result = slot.result;
  result.status =
      slot.state == Slot::State::kTimingOut ? Status::kTimeout : status;
  result.native_code = native;
  result.has_native_code = has_native;
  slot = {};
  if (result.kind == OperationKind::kConnect && result.status != Status::kOk) {
    if (result.status == Status::kConnectionFailed &&
        reconnect_profile_ != 0 && enabled_ && !faulted_) {
      reconnect_.scheduleAfter(roo_time::Seconds(5));
    } else {
      reconnect_profile_ = 0;
    }
  }
  if (result.kind == OperationKind::kScan) {
    for (Listener* listener : listeners_) listener->onScanStateChanged(false);
  }
  if (result.kind == OperationKind::kSave ||
      result.kind == OperationKind::kRemove) {
    for (Listener* listener : listeners_) listener->onProfilesChanged();
  }
  for (Listener* listener : listeners_) listener->onOperationFinished(result);
}

void Controller::onOperationFinished(const OperationResult& result) {
  Slot* slot = find(result.id);
  if (slot == nullptr || lifecycle_ == Lifecycle::kClosed ||
      result.kind != slot->result.kind) {
    return;
  }
  Status status = result.status;
  ProfileId last_profile = 0;
  if (status == Status::kOk && slot->state == Slot::State::kRunning) {
    if (result.kind == OperationKind::kScan) {
      Interface::ScanRead read;
      status =
          interface_.readScanResults(records_.data(), records_.size(), read);
      if (status == Status::kOk && read.count <= records_.size()) {
        snapshot_ = {snapshot_.generation + 1, records_.data(), read.count,
                     read.truncated};
        for (Listener* listener : listeners_) listener->onScanChanged();
      } else if (status == Status::kOk) {
        status = Status::kCorrupt;
      }
    } else if (result.kind == OperationKind::kEnable) {
      status = store_.writeEnabled(enabled_);
      if (status == Status::kOk && enabled_) {
        status = store_.readLastProfile(last_profile);
        if (status == Status::kNotFound) status = Status::kOk;
      }
    } else if (result.kind == OperationKind::kConnect &&
               slot->result.profile_id != 0) {
      status = store_.writeLastProfile(slot->result.profile_id);
    }
  }
  finish(*slot, status, result.native_code, result.has_native_code);
  if (last_profile != 0 && station_.result.id == 0 &&
      lifecycle_ != Lifecycle::kClosed) {
    reconnect_profile_ = last_profile;
    reconnect_.scheduleNow();
  }
}

void Controller::onEnabledChanged(bool enabled) {
  if (lifecycle_ == Lifecycle::kClosed) return;
  enabled_ = enabled;
  for (Listener* listener : listeners_) listener->onEnabledChanged(enabled);
}

void Controller::onLinkChanged(const LinkState& state) {
  if (lifecycle_ == Lifecycle::kClosed || faulted_) return;
  if (state.connection_id != link_.connection_id &&
      state.connection_id != station_.result.id) {
    return;
  }
  link_ = state;
  for (Listener* listener : listeners_) listener->onLinkChanged(state);
  if (state.phase == LinkPhase::kIdle && reconnect_profile_ != 0 &&
      station_.result.id == 0) {
    reconnect_.scheduleAfter(roo_time::Seconds(1));
  }
}

void Controller::startProfile() {
  if (reconnect_profile_ == 0 || lifecycle_ == Lifecycle::kClosed ||
      !enabled_ || faulted_)
    return;
  ProfileId id = reconnect_profile_;
  Profile profile;
  Status status = loadProfile(id, profile);
  if (status == Status::kOk && !profile.settings.auto_connect) {
    reconnect_profile_ = 0;
    return;
  }
  if (status == Status::kOk) {
    RequestResult result = connect(id);
    if (result.id != 0) return;
    status = result.status;
    if (status == Status::kBusy) {
      reconnect_.scheduleAfter(roo_time::Seconds(1));
      return;
    }
  }
  RequestResult request = admit(station_, OperationKind::kConnect, id);
  if (request.id != 0) finish(station_, status);
}
}  // namespace roo_wifi
