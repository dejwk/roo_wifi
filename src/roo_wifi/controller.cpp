#include "roo_wifi/controller.h"

#include <algorithm>
#include <limits>

namespace roo_wifi {
Controller::Controller(Interface &interface, Store &store,
                       roo_scheduler::Scheduler &scheduler,
                       ControllerOptions options)
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
  if (closed_) return Status::kNotStarted;
  if (started_) return Status::kBusy;
  Status error = store_.begin();
  if (error != Status::kOk) return error;
  bool enabled = false;
  error = store_.readEnabled(enabled);
  if (error != Status::kOk && error != Status::kNotFound) return error;
  error = interface_.begin(*this, scheduler_);
  if (error != Status::kOk) return error;
  started_ = true;
  return setEnabled(enabled).error;
}

void Controller::close(bool notify) {
  if (closed_) return;
  closed_ = true;
  work_.cancel();
  timer_.cancel();
  reconnect_.cancel();
  if (started_) interface_.shutdown();
  if (notify) {
    for (Slot *slot : {&station_, &scan_, &write_})
      if (slot->result.id) finish(*slot, Status::kCancelled);
  }
  station_ = {};
  scan_ = {};
  write_ = {};
  credentials_ = {};
  update_ = {};
  snapshot_ = {};
  link_ = {};
  enabled_ = false;
  started_ = false;
}

void Controller::shutdown() { close(true); }

void Controller::addListener(Listener &listener) {
  if (std::find(listeners_.begin(), listeners_.end(), &listener) ==
      listeners_.end())
    listeners_.push_back(&listener);
}

void Controller::removeListener(Listener &listener) {
  listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), &listener),
                   listeners_.end());
}

Support Controller::support() const { return interface_.support(); }

bool Controller::isEnabled() const { return enabled_; }

bool Controller::isScanning() const { return scan_.result.id != 0; }

ScanSnapshot Controller::scanSnapshot() const { return snapshot_; }

LinkState Controller::linkState() const { return link_; }

Status Controller::loadProfile(ProfileId id, Profile &out) const {
  if (!started_ || closed_) return Status::kNotStarted;
  if (!id) return Status::kInvalidArgument;
  return store_.loadProfile(id, out);
}

Status Controller::radioAdmission() const {
  if (!started_ || closed_) return Status::kNotStarted;
  if (faulted_) return Status::kNotStarted;
  return Status::kOk;
}

RequestResult Controller::admit(Slot &slot, OperationKind kind,
                                ProfileId profile) {
  if (!started_ || closed_) return {0, Status::kNotStarted};
  if (slot.result.id) return {0, Status::kBusy};
  if (next_id_ == std::numeric_limits<OperationId>::max())
    return {0, Status::kBusy};
  slot = {};
  slot.result = {next_id_++, kind, Status::kOk, profile};
  work_.scheduleNow();
  return {slot.result.id, Status::kOk};
}

RequestResult Controller::setEnabled(bool enabled) {
  Status error = radioAdmission();
  if (error != Status::kOk) return {0, error};
  if (scan_.result.id) return {0, Status::kBusy};
  RequestResult result = admit(station_, OperationKind::kEnable);
  if (result.id) {
    desired_enabled_ = enabled;
    reconnect_.cancel();
    reconnect_profile_ = 0;
  }
  return result;
}

RequestResult Controller::scan() {
  Status error = radioAdmission();
  if (error != Status::kOk) return {0, error};
  if (!enabled_) return {0, Status::kDisabled};
  if (station_.result.id ||
      (link_.phase != LinkPhase::kIdle && !support().scan_while_connected))
    return {0, Status::kBusy};
  return admit(scan_, OperationKind::kScan);
}

RequestResult Controller::connect(const ConnectionConfig &config,
                                  const Credentials &credential) {
  Status error = radioAdmission();
  if (error != Status::kOk) return {0, error};
  if (!enabled_) return {0, Status::kDisabled};
  if (scan_.result.id) return {0, Status::kBusy};
  error = Validate(config, credential);
  if (error != Status::kOk) return {0, error};
  error = ValidateSupport(config, support());
  if (error != Status::kOk) return {0, error};
  RequestResult result = admit(station_, OperationKind::kConnect);
  if (result.id) {
    config_ = config;
    credentials_ = credential;
    reconnect_profile_ = 0;
    reconnect_.cancel();
  }
  return result;
}

RequestResult Controller::connect(ProfileId id) {
  Profile profile;
  Credentials credential;
  Status error = loadProfile(id, profile);
  if (error != Status::kOk) return {0, error};
  error = store_.loadCredentials(id, credential);
  if (error != Status::kOk) return {0, error};
  RequestResult result = connect(profile.settings.connection, credential);
  if (result.id) {
    station_.result.profile_id = id;
    reconnect_profile_ = profile.settings.auto_connect ? id : 0;
  }
  return result;
}

RequestResult Controller::disconnect() {
  Status error = radioAdmission();
  if (error != Status::kOk) return {0, error};
  RequestResult result = admit(station_, OperationKind::kDisconnect);
  if (result.id) {
    reconnect_profile_ = 0;
    reconnect_.cancel();
  }
  return result;
}

RequestResult Controller::saveProfile(ProfileId id,
                                      const ProfileSettings &settings,
                                      const CredentialUpdate &credential) {
  if (!id) return {0, Status::kInvalidArgument};
  RequestResult result = admit(write_, OperationKind::kSave, id);
  if (result.id) {
    settings_ = settings;
    update_ = credential;
  }
  return result;
}

RequestResult Controller::removeProfile(ProfileId id) {
  if (!id) return {0, Status::kInvalidArgument};
  return admit(write_, OperationKind::kRemove, id);
}

Controller::Slot *Controller::find(OperationId id) {
  if (!id) return nullptr;
  for (Slot *slot : {&station_, &scan_, &write_})
    if (slot->result.id == id) return slot;
  return nullptr;
}

Status Controller::cancel(OperationId id) {
  Slot *slot = find(id);
  if (!slot) return Status::kNotFound;
  if (slot == &write_ && slot->started) return Status::kBusy;
  if (slot->cancelled || slot->timed_out) return Status::kOk;
  if (slot->started) {
    Status error = interface_.cancel(id);
    if (error != Status::kOk) return error;
    slot->deadline = roo_time::Uptime::Now() +
                     roo_time::Millis(options_.transition_timeout_ms);
  }
  slot->cancelled = true;
  if (slot == &station_) {
    reconnect_profile_ = 0;
    reconnect_.cancel();
  }
  work_.scheduleNow();
  return Status::kOk;
}

void Controller::execute() {
  if (closed_) return;
  // Capture IDs: a listener's follow-up admission must wait for the next task.
  OperationId ids[] = {station_.result.id, scan_.result.id, write_.result.id};
  for (OperationId id : ids) {
    Slot *slot = find(id);
    if (!slot || slot->started) continue;
    if (slot->cancelled) {
      finish(*slot, Status::kCancelled);
      continue;
    }
    slot->started = true;
    Status error = Status::kOk;
    switch (slot->result.kind) {
      case OperationKind::kSave:
        error = store_.saveProfile(slot->result.profile_id, settings_, update_)
                    .error;
        update_ = {};
        break;
      case OperationKind::kRemove:
        error = store_.removeProfile(slot->result.profile_id);
        break;
      case OperationKind::kEnable:
        error = interface_.setEnabled(id, desired_enabled_);
        break;
      case OperationKind::kConnect:
        error = interface_.connect(id, config_, credentials_);
        credentials_ = {};
        break;
      case OperationKind::kDisconnect:
        error = interface_.disconnect(id);
        break;
      case OperationKind::kScan:
        for (Listener *listener : listeners_)
          listener->onScanStateChanged(true);
        error = interface_.scan(id, options_.max_scan_results);
        break;
    }
    if (slot == &write_ || error != Status::kOk) {
      finish(*slot, error);
      continue;
    }
    uint32_t timeout = slot == &scan_ ? options_.scan_timeout_ms
                       : slot->result.kind == OperationKind::kConnect
                           ? options_.connect_timeout_ms
                           : options_.transition_timeout_ms;
    slot->deadline = roo_time::Uptime::Now() + roo_time::Millis(timeout);
  }
  if (station_.result.id || scan_.result.id)
    timer_.scheduleAfter(roo_time::Millis(1));
}

void Controller::checkTimeouts() {
  if (closed_) return;
  for (Slot *slot : {&station_, &scan_}) {
    if (!slot->result.id || !slot->started ||
        roo_time::Uptime::Now() < slot->deadline)
      continue;
    if (slot->timed_out || slot->cancelled) {
      faulted_ = true;
      reconnect_profile_ = 0;
      finish(*slot, Status::kTimeout);
    } else {
      slot->timed_out = true;
      slot->deadline = roo_time::Uptime::Now() +
                       roo_time::Millis(options_.transition_timeout_ms);
      interface_.cancel(slot->result.id);
    }
  }
  if (station_.result.id || scan_.result.id)
    timer_.scheduleAfter(roo_time::Millis(1));
}

void Controller::finish(Slot &slot, Status error, int32_t native,
                        bool has_native) {
  OperationResult result = slot.result;
  result.error = slot.timed_out ? Status::kTimeout : error;
  result.native_code = native;
  result.has_native_code = has_native;
  slot = {};
  if (result.kind == OperationKind::kConnect && result.error != Status::kOk)
    reconnect_profile_ = 0;
  if (result.kind == OperationKind::kScan)
    for (Listener *listener : listeners_) listener->onScanStateChanged(false);
  if (result.kind == OperationKind::kSave ||
      result.kind == OperationKind::kRemove)
    for (Listener *listener : listeners_) listener->onProfilesChanged();
  for (Listener *listener : listeners_) listener->onOperationFinished(result);
}

void Controller::onOperationFinished(const OperationResult &result) {
  Slot *slot = find(result.id);
  if (!slot || closed_ || result.kind != slot->result.kind) return;
  Status error = result.error;
  bool startup = false;
  if (error == Status::kOk && !slot->timed_out && !slot->cancelled) {
    if (result.kind == OperationKind::kScan) {
      ScanRead read;
      error =
          interface_.readScanResults(records_.data(), records_.size(), read);
      if (error == Status::kOk && read.count <= records_.size()) {
        snapshot_ = {snapshot_.generation + 1, records_.data(), read.count,
                     read.truncated};
        for (Listener *listener : listeners_) listener->onScanChanged();
      } else if (error == Status::kOk)
        error = Status::kCorrupt;
    } else if (result.kind == OperationKind::kEnable) {
      error = store_.writeEnabled(enabled_);
      startup = error == Status::kOk && enabled_;
    }
  }
  finish(*slot, error, result.native_code, result.has_native_code);
  if (startup && !station_.result.id && !closed_) {
    reconnect_profile_ = options_.startup_profile;
    reconnect_.scheduleNow();
  }
}

void Controller::onEnabledChanged(bool enabled) {
  if (closed_) return;
  enabled_ = enabled;
  for (Listener *listener : listeners_) listener->onEnabledChanged(enabled);
}

void Controller::onLinkChanged(const LinkState &state) {
  if (closed_ || faulted_) return;
  if (state.connection_id != link_.connection_id &&
      state.connection_id != station_.result.id)
    return;
  link_ = state;
  for (Listener *listener : listeners_) listener->onLinkChanged(state);
  if (state.phase == LinkPhase::kIdle && reconnect_profile_ &&
      !station_.result.id)
    reconnect_.scheduleAfter(roo_time::Seconds(1));
}

void Controller::startProfile() {
  if (!reconnect_profile_ || closed_ || !enabled_ || faulted_) return;
  ProfileId id = reconnect_profile_;
  Profile profile;
  Status error = loadProfile(id, profile);
  if (error == Status::kOk && !profile.settings.auto_connect) {
    reconnect_profile_ = 0;
    return;
  }
  if (error == Status::kOk) {
    RequestResult result = connect(id);
    if (result.id) return;
    error = result.error;
    if (error == Status::kBusy) {
      reconnect_.scheduleAfter(roo_time::Seconds(1));
      return;
    }
  }
  RequestResult request = admit(station_, OperationKind::kConnect, id);
  if (request.id) finish(station_, error);
}
}  // namespace roo_wifi
