#include "roo_wifi/hal/ordered_interface.h"

#include <cstring>
namespace roo_wifi {
OrderedInterface::OrderedInterface(NativeStation &native) : native_(native) {}
OrderedInterface::~OrderedInterface() { shutdown(); }
Error OrderedInterface::begin(Sink &sink, roo_scheduler::Scheduler &scheduler) {
  if (attached_) return Error::kBusy;
  dispatch_.reset(
      new roo_scheduler::SingletonTask(scheduler, [this] { drain(); }));
  sink_ = &sink;
  faulted_ = false;
  overflow_ = false;
  head_ = count_ = 0;
  Error error = native_.attach(*this);
  if (error != Error::kOk) {
    sink_ = nullptr;
    dispatch_.reset();
    return error;
  }
  attached_ = true;
  return Error::kOk;
}

Support OrderedInterface::support() const { return native_.support(); }
Error OrderedInterface::stationAdmission(OperationId id) const {
  if (!sink_ || faulted_) return Error::kNotStarted;
  if (!id) return Error::kInvalidArgument;
  if (station_.id || scan_.id) return Error::kBusy;
  return Error::kOk;
}

Error OrderedInterface::setEnabled(OperationId id, bool enabled) {
  Error error = stationAdmission(id);
  if (error != Error::kOk) return error;
  station_ = {id, OperationKind::kEnable};
  desired_enabled_ = enabled;
  error = native_.enable(enabled);
  if (error != Error::kOk) station_ = {};
  return error;
}

Error OrderedInterface::scan(OperationId id, uint16_t capacity) {
  Error error = stationAdmission(id);
  if (error != Error::kOk) return error;
  if (!enabled_) return Error::kDisabled;
  if (link_.phase != LinkPhase::kIdle && !support().scan_while_connected)
    return Error::kBusy;
  scan_ = {id, OperationKind::kScan};
  error = native_.scan(capacity);
  if (error != Error::kOk) scan_ = {};
  return error;
}

Error OrderedInterface::connect(OperationId id, const ConnectionConfig &config,
                                const Credentials &secret) {
  Error error = stationAdmission(id);
  if (error != Error::kOk) return error;
  if (!enabled_) return Error::kDisabled;
  error = Validate(config, secret);
  if (error != Error::kOk) return error;
  error = ValidateSupport(config, support());
  if (error != Error::kOk) return error;
  station_ = {id, OperationKind::kConnect};
  config_ = config;
  credentials_ = secret;
  if (link_.phase != LinkPhase::kIdle) {
    waiting_disconnect_ = true;
    error = native_.disconnect();
    if (error == Error::kNotFound)
      post({NativeStation::Event::kDisconnected, link_});
    else if (error != Error::kOk) {
      station_ = {};
      waiting_disconnect_ = false;
      credentials_ = {};
      return error;
    }
  } else {
    // Commands and sink delivery are deferred even for immediate native
    // failure.
    dispatch_->scheduleNow();
  }
  return Error::kOk;
}

Error OrderedInterface::disconnect(OperationId id) {
  Error error = stationAdmission(id);
  if (error != Error::kOk) return error;
  station_ = {id, OperationKind::kDisconnect};
  waiting_disconnect_ = true;
  if (link_.phase == LinkPhase::kIdle)
    error = Error::kNotFound;
  else
    error = native_.disconnect();
  if (error == Error::kNotFound) {
    post({NativeStation::Event::kDisconnected, link_});
    return Error::kOk;
  }
  if (error != Error::kOk) {
    station_ = {};
    waiting_disconnect_ = false;
  }
  return error;
}

Error OrderedInterface::cancel(OperationId id) {
  if (!id) return Error::kNotFound;
  if (scan_.id == id) {
    if (scan_cancelling_) return Error::kOk;
    Error error = native_.stopScan();
    if (error != Error::kOk) return error;
    scan_cancelling_ = true;
    return Error::kOk;
  }
  if (station_.id != id) return Error::kNotFound;
  if (cancelling_) return Error::kOk;
  if (station_.kind == OperationKind::kEnable) {
    cancelling_ = true;
    return Error::kOk;
  }
  if (!waiting_disconnect_ && link_.phase != LinkPhase::kIdle) {
    Error error = native_.disconnect();
    if (error == Error::kNotFound)
      post({NativeStation::Event::kDisconnected, link_});
    else if (error != Error::kOk)
      return error;
    waiting_disconnect_ = true;
  }
  cancelling_ = true;
  dispatch_->scheduleNow();
  return Error::kOk;
}

Error OrderedInterface::readScanResults(ScanRecord *out, size_t capacity,
                                        ScanRead &result) const {
  return native_.readScan(out, capacity, result);
}

void OrderedInterface::shutdown() {
  if (attached_) {
    native_.detach();
    attached_ = false;
  }
  if (dispatch_) dispatch_->shutdown();
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
  if (count_ == queue_.size())
    overflow_ = true;
  else {
    queue_[(head_ + count_) % queue_.size()] = event;
    ++count_;
  }
  dispatch_->scheduleNow();
}

void OrderedInterface::drain() {
  if (!sink_) return;
  for (;;) {
    NativeStation::Event event{};
    bool overflow;
    {
      roo::lock_guard<roo::mutex> lock(mutex_);
      overflow = overflow_;
      if (overflow) count_ = 0;
      if (!overflow && !count_) break;
      if (!overflow) {
        event = queue_[head_];
        head_ = (head_ + 1) % queue_.size();
        --count_;
      }
    }
    if (overflow) {
      faulted_ = true;
      finishStation(Error::kConnectionFailed);
      finishScan(Error::kConnectionFailed);
      return;
    }
    process(event);
  }
  if (station_.id && station_.kind == OperationKind::kConnect &&
      !waiting_disconnect_ && link_.phase == LinkPhase::kIdle) {
    if (cancelling_)
      finishStation(Error::kCancelled);
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
  Error error = native_.connect(config_, credentials_);
  credentials_ = {};
  if (error != Error::kOk) {
    link_.phase = LinkPhase::kIdle;
    link_.reason = error;
    sink_->onLinkChanged(link_);
    finishStation(error);
  }
}

void OrderedInterface::finishStation(Error error, int32_t code) {
  if (!station_.id) return;
  OperationResult result = station_;
  result.error = cancelling_ ? Error::kCancelled : error;
  result.native_code = code;
  result.has_native_code = code != 0;
  station_ = {};
  cancelling_ = waiting_disconnect_ = false;
  credentials_ = {};
  sink_->onOperationFinished(result);
}

void OrderedInterface::finishScan(Error error, int32_t code) {
  if (!scan_.id) return;
  OperationResult result = scan_;
  result.error = scan_cancelling_ ? Error::kCancelled : error;
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
      if (station_.id && station_.kind == OperationKind::kEnable &&
          enabled_ == desired_enabled_)
        finishStation(event.error, event.native_code);
      break;
    case E::kPrepared:
      if (station_.id && station_.kind == OperationKind::kConnect &&
          !waiting_disconnect_ && !cancelling_) {
        Error error = native_.continueConnect();
        if (error != Error::kOk) {
          link_.phase = LinkPhase::kIdle;
          link_.reason = error;
          sink_->onLinkChanged(link_);
          finishStation(error);
        }
      }
      break;
    case E::kScanDone:
      finishScan(event.error, event.native_code);
      break;
    case E::kAssociated:
      if (link_.phase != LinkPhase::kConnecting ||
          event.link.ssid.size != link_.ssid.size ||
          memcmp(event.link.ssid.bytes, link_.ssid.bytes, link_.ssid.size))
        return;
      {
        OperationId id = link_.connection_id;
        link_ = event.link;
        link_.connection_id = id;
        link_.phase = LinkPhase::kAssociated;
      }
      sink_->onLinkChanged(link_);
      break;
    case E::kAddressReady:
      if (event.link.ssid.size &&
          (event.link.ssid.size != link_.ssid.size ||
           memcmp(event.link.ssid.bytes, link_.ssid.bytes, link_.ssid.size) ||
           memcmp(event.link.bssid.bytes, link_.bssid.bytes, 6)))
        return;
      if ((link_.phase != LinkPhase::kAssociated &&
           link_.phase != LinkPhase::kAddressReady) ||
          waiting_disconnect_ || cancelling_)
        return;
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
          station_.id == link_.connection_id)
        finishStation(Error::kOk);
      break;
    case E::kAddressLost:
      if (link_.phase == LinkPhase::kAddressReady) {
        link_.phase = LinkPhase::kAssociated;
        link_.has_ipv4 = false;
        sink_->onLinkChanged(link_);
      }
      break;
    case E::kDisconnected:
      if (event.link.ssid.size &&
          (event.link.ssid.size != link_.ssid.size ||
           memcmp(event.link.ssid.bytes, link_.ssid.bytes, link_.ssid.size)))
        return;
      link_.phase = LinkPhase::kIdle;
      link_.has_ipv4 = false;
      link_.reason = event.error;
      link_.native_code = event.native_code;
      link_.has_native_code = event.native_code != 0;
      sink_->onLinkChanged(link_);
      if (station_.id) {
        if (waiting_disconnect_ && station_.kind == OperationKind::kConnect &&
            !cancelling_) {
          waiting_disconnect_ = false;
        } else if (station_.kind != OperationKind::kEnable)
          finishStation(station_.kind == OperationKind::kDisconnect
                            ? Error::kOk
                            : Error::kConnectionFailed,
                        event.native_code);
      }
      break;
  }
}
}  // namespace roo_wifi
