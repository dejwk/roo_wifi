#include "roo_wifi/hal/ordered_interface.h"

#include <cstring>

namespace roo_wifi {
OrderedInterface::OrderedInterface(NativeStation &native) : native_(native) {}

OrderedInterface::~OrderedInterface() { shutdown(); }

Status OrderedInterface::begin(Sink &sink,
                               roo_scheduler::Scheduler &scheduler) {
  if (attached_) return Status::kBusy;
  dispatch_.reset(
      new roo_scheduler::SingletonTask(scheduler, [this] { drain(); }));
  sink_ = &sink;
  faulted_ = false;
  overflow_ = false;
  head_ = count_ = 0;
  Status status = native_.attach(*this);
  if (status != Status::kOk) {
    sink_ = nullptr;
    dispatch_.reset();
    return status;
  }
  attached_ = true;
  return Status::kOk;
}

Support OrderedInterface::support() const { return native_.support(); }

Status OrderedInterface::stationAdmission(OperationId id) const {
  if (sink_ == nullptr || faulted_) return Status::kNotStarted;
  if (id == 0) return Status::kInvalidArgument;
  if (station_.id != 0 || scan_.id != 0) return Status::kBusy;
  return Status::kOk;
}

Status OrderedInterface::setEnabled(OperationId id, bool enabled) {
  Status status = stationAdmission(id);
  if (status != Status::kOk) return status;
  station_ = {id, OperationKind::kEnable};
  desired_enabled_ = enabled;
  status = native_.enable(enabled);
  if (status != Status::kOk) station_ = {};
  return status;
}

Status OrderedInterface::scan(OperationId id, uint16_t capacity) {
  Status status = stationAdmission(id);
  if (status != Status::kOk) return status;
  if (!enabled_) return Status::kDisabled;
  if (link_.phase != LinkPhase::kIdle && !support().scan_while_connected)
    return Status::kBusy;
  scan_ = {id, OperationKind::kScan};
  status = native_.scan(capacity);
  if (status != Status::kOk) scan_ = {};
  return status;
}

Status OrderedInterface::connect(OperationId id, const ConnectionConfig &config,
                                 const Credentials &secret) {
  Status status = stationAdmission(id);
  if (status != Status::kOk) return status;
  if (!enabled_) return Status::kDisabled;
  status = Validate(config, secret);
  if (status != Status::kOk) return status;
  status = ValidateSupport(config, support());
  if (status != Status::kOk) return status;
  station_ = {id, OperationKind::kConnect};
  config_ = config;
  credentials_ = secret;
  if (link_.phase != LinkPhase::kIdle) {
    waiting_disconnect_ = true;
    status = native_.disconnect();
    if (status == Status::kNotFound) {
      post({NativeStation::Event::kDisconnected, link_});
    } else if (status != Status::kOk) {
      station_ = {};
      waiting_disconnect_ = false;
      credentials_ = {};
      return status;
    }
  } else {
    // Commands and sink delivery are deferred even for immediate native
    // failure.
    dispatch_->scheduleNow();
  }
  return Status::kOk;
}

Status OrderedInterface::disconnect(OperationId id) {
  Status status = stationAdmission(id);
  if (status != Status::kOk) return status;
  station_ = {id, OperationKind::kDisconnect};
  waiting_disconnect_ = true;
  if (link_.phase == LinkPhase::kIdle)
    status = Status::kNotFound;
  else
    status = native_.disconnect();
  if (status == Status::kNotFound) {
    post({NativeStation::Event::kDisconnected, link_});
    return Status::kOk;
  }
  if (status != Status::kOk) {
    station_ = {};
    waiting_disconnect_ = false;
  }
  return status;
}

Status OrderedInterface::cancel(OperationId id) {
  if (id == 0) return Status::kNotFound;
  if (scan_.id == id) {
    if (scan_cancelling_) return Status::kOk;
    Status status = native_.stopScan();
    if (status != Status::kOk) return status;
    scan_cancelling_ = true;
    return Status::kOk;
  }
  if (station_.id != id) return Status::kNotFound;
  if (cancelling_) return Status::kOk;
  if (station_.kind == OperationKind::kEnable) {
    cancelling_ = true;
    return Status::kOk;
  }
  if (!waiting_disconnect_ && link_.phase != LinkPhase::kIdle) {
    Status status = native_.disconnect();
    if (status == Status::kNotFound)
      post({NativeStation::Event::kDisconnected, link_});
    else if (status != Status::kOk)
      return status;
    waiting_disconnect_ = true;
  }
  cancelling_ = true;
  dispatch_->scheduleNow();
  return Status::kOk;
}

Status OrderedInterface::readScanResults(ScanRecord *out, size_t capacity,
                                         ScanRead &result) const {
  return native_.readScan(out, capacity, result);
}

void OrderedInterface::shutdown() {
  if (attached_) {
    native_.detach();
    attached_ = false;
  }
  if (dispatch_ != nullptr) dispatch_->shutdown();
  sink_ = nullptr;
  station_ = {};
  scan_ = {};
  credentials_ = {};
  link_ = {};
  roo::lock_guard<roo::mutex> lock(mutex_);
  count_ = 0;
}

void OrderedInterface::post(const NativeStation::Event &event) {
  roo::lock_guard<roo::mutex> lock(mutex_);
  if (count_ == queue_.size()) {
    overflow_ = true;
  } else {
    queue_[(head_ + count_) % queue_.size()] = event;
    ++count_;
  }
  dispatch_->scheduleNow();
}

void OrderedInterface::drain() {
  if (sink_ == nullptr) return;
  for (;;) {
    NativeStation::Event event{};
    bool overflow;
    {
      roo::lock_guard<roo::mutex> lock(mutex_);
      overflow = overflow_;
      if (overflow) count_ = 0;
      if (!overflow && count_ == 0) break;
      if (!overflow) {
        event = queue_[head_];
        head_ = (head_ + 1) % queue_.size();
        --count_;
      }
    }
    if (overflow) {
      faulted_ = true;
      finishStation(Status::kConnectionFailed);
      finishScan(Status::kConnectionFailed);
      return;
    }
    process(event);
  }
  if (station_.id != 0 && station_.kind == OperationKind::kConnect &&
      !waiting_disconnect_ && link_.phase == LinkPhase::kIdle) {
    if (cancelling_)
      finishStation(Status::kCancelled);
    else
      startConnection();
  }
}

void OrderedInterface::startConnection() {
  link_ = {};
  link_.connection_id = station_.id;
  link_.ssid = config_.ssid;
  link_.security = config_.security;
  link_.phase = LinkPhase::kConnecting;
  sink_->onLinkChanged(link_);
  Status status = native_.connect(config_, credentials_);
  credentials_ = {};
  if (status != Status::kOk) {
    link_.phase = LinkPhase::kIdle;
    link_.reason = status;
    sink_->onLinkChanged(link_);
    finishStation(status);
  }
}

void OrderedInterface::finishStation(Status error, int32_t code) {
  if (station_.id == 0) return;
  OperationResult result = station_;
  result.error = cancelling_ ? Status::kCancelled : error;
  result.native_code = code;
  result.has_native_code = code != 0;
  station_ = {};
  cancelling_ = waiting_disconnect_ = false;
  credentials_ = {};
  sink_->onOperationFinished(result);
}

void OrderedInterface::finishScan(Status error, int32_t code) {
  if (scan_.id == 0) return;
  OperationResult result = scan_;
  result.error = scan_cancelling_ ? Status::kCancelled : error;
  result.native_code = code;
  result.has_native_code = code != 0;
  scan_ = {};
  scan_cancelling_ = false;
  sink_->onOperationFinished(result);
}

void OrderedInterface::process(const NativeStation::Event &event) {
  using E = NativeStation::Event;
  if (faulted_) return;
  switch (event.kind) {
    case E::kEnabled:
    case E::kDisabled:
      enabled_ = event.kind == E::kEnabled;
      sink_->onEnabledChanged(enabled_);
      if (!enabled_ && link_.phase != LinkPhase::kIdle) {
        link_.phase = LinkPhase::kIdle;
        link_.has_ipv4 = false;
        sink_->onLinkChanged(link_);
      }
      if (station_.id != 0 && station_.kind == OperationKind::kEnable &&
          enabled_ == desired_enabled_) {
        finishStation(event.error, event.native_code);
      }
      break;
    case E::kPrepared:
      if (station_.id != 0 && station_.kind == OperationKind::kConnect &&
          !waiting_disconnect_ && !cancelling_) {
        Status status = native_.continueConnect();
        if (status != Status::kOk) {
          link_.phase = LinkPhase::kIdle;
          link_.reason = status;
          sink_->onLinkChanged(link_);
          finishStation(status);
        }
      }
      break;
    case E::kScanDone:
      finishScan(event.error, event.native_code);
      break;
    case E::kAssociated:
      if (link_.phase != LinkPhase::kConnecting ||
          event.link.ssid.size != link_.ssid.size ||
          memcmp(event.link.ssid.bytes, link_.ssid.bytes, link_.ssid.size)) {
        return;
      }
      {
        OperationId id = link_.connection_id;
        link_ = event.link;
        link_.connection_id = id;
        link_.phase = LinkPhase::kAssociated;
      }
      sink_->onLinkChanged(link_);
      break;
    case E::kAddressReady:
      if (event.link.ssid.size != 0 &&
          (event.link.ssid.size != link_.ssid.size ||
           memcmp(event.link.ssid.bytes, link_.ssid.bytes, link_.ssid.size) ||
           memcmp(event.link.bssid.bytes, link_.bssid.bytes, 6))) {
        return;
      }
      if ((link_.phase != LinkPhase::kAssociated &&
           link_.phase != LinkPhase::kAddressReady) ||
          waiting_disconnect_ || cancelling_) {
        return;
      }
      link_.address = event.link.address;
      link_.gateway = event.link.gateway;
      link_.dns1 = event.link.dns1;
      link_.dns2 = event.link.dns2;
      link_.has_ipv4 = event.link.has_ipv4;
      link_.has_dns1 = event.link.has_dns1;
      link_.has_dns2 = event.link.has_dns2;
      if (!link_.has_ipv4) return;
      link_.phase = LinkPhase::kAddressReady;
      sink_->onLinkChanged(link_);
      if (station_.kind == OperationKind::kConnect &&
          station_.id == link_.connection_id) {
        finishStation(Status::kOk);
      }
      break;
    case E::kAddressLost:
      if (link_.phase == LinkPhase::kAddressReady) {
        link_.phase = LinkPhase::kAssociated;
        link_.has_ipv4 = false;
        sink_->onLinkChanged(link_);
      }
      break;
    case E::kDisconnected:
      if (event.link.ssid.size != 0 &&
          (event.link.ssid.size != link_.ssid.size ||
           memcmp(event.link.ssid.bytes, link_.ssid.bytes, link_.ssid.size))) {
        return;
      }
      link_.phase = LinkPhase::kIdle;
      link_.has_ipv4 = false;
      link_.reason = event.error;
      link_.native_code = event.native_code;
      link_.has_native_code = event.native_code != 0;
      sink_->onLinkChanged(link_);
      if (station_.id != 0) {
        if (waiting_disconnect_ && station_.kind == OperationKind::kConnect &&
            !cancelling_) {
          waiting_disconnect_ = false;
        } else if (station_.kind != OperationKind::kEnable) {
          finishStation(station_.kind == OperationKind::kDisconnect
                            ? Status::kOk
                            : Status::kConnectionFailed,
                        event.native_code);
        }
      }
      break;
  }
}
}  // namespace roo_wifi
