#include "roo_wifi/hal/ordered_interface.h"

#include <cstring>
#include <new>
#include <type_traits>

namespace roo_wifi {
OrderedInterface::QueuedEvent::QueuedEvent(const NativeStation::Event &event)
    : native_code(event.native_code), kind(event.kind), status(event.status) {
  static_assert(std::is_trivially_copyable<QueuedEvent>::value,
                "Queue assignment must preserve the active union member");
  using E = NativeStation::Event;
  if (kind == E::kAssociated || kind == E::kAddressReady ||
      kind == E::kDisconnected) {
    ssid = event.link.ssid;
  }
  if (kind == E::kAssociated || kind == E::kAddressReady) {
    bssid = event.link.bssid;
  }
  if (kind == E::kAssociated) {
    // Explicitly begin the association member's lifetime before filling it.
    new (&payload.association) Association{};
    payload.association.station_mac = event.link.station_mac;
    payload.association.security = event.link.security;
    payload.association.rssi_dbm = event.link.rssi_dbm;
    payload.association.channel = event.link.channel;
    payload.association.has_radio_info = event.link.has_radio_info;
    payload.association.has_station_mac = event.link.has_station_mac;
  } else if (kind == E::kAddressReady) {
    payload.addresses.address = event.link.address;
    payload.addresses.gateway = event.link.gateway;
    payload.addresses.dns1 = event.link.dns1;
    payload.addresses.dns2 = event.link.dns2;
    payload.addresses.has_ipv4 = event.link.has_ipv4;
    payload.addresses.has_dns1 = event.link.has_dns1;
    payload.addresses.has_dns2 = event.link.has_dns2;
  }
}

NativeStation::Event OrderedInterface::QueuedEvent::expand() const {
  NativeStation::Event event{kind};
  event.status = status;
  event.native_code = native_code;
  using E = NativeStation::Event;
  if (kind == E::kAssociated || kind == E::kAddressReady ||
      kind == E::kDisconnected) {
    event.link.ssid = ssid;
  }
  if (kind == E::kAssociated || kind == E::kAddressReady) {
    event.link.bssid = bssid;
  }
  if (kind == E::kAssociated) {
    event.link.station_mac = payload.association.station_mac;
    event.link.security = payload.association.security;
    event.link.rssi_dbm = payload.association.rssi_dbm;
    event.link.channel = payload.association.channel;
    event.link.has_radio_info = payload.association.has_radio_info;
    event.link.has_station_mac = payload.association.has_station_mac;
  } else if (kind == E::kAddressReady) {
    event.link.address = payload.addresses.address;
    event.link.gateway = payload.addresses.gateway;
    event.link.dns1 = payload.addresses.dns1;
    event.link.dns2 = payload.addresses.dns2;
    event.link.has_ipv4 = payload.addresses.has_ipv4;
    event.link.has_dns1 = payload.addresses.has_dns1;
    event.link.has_dns2 = payload.addresses.has_dns2;
  }
  return event;
}

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

Status OrderedInterface::startScan(OperationId id, uint16_t capacity) {
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

Status OrderedInterface::cancelScan() {
  if (scan_.id == 0 || scan_cancelling_) return Status::kOk;
  Status status = native_.stopScan();
  if (status != Status::kOk) return status;
  scan_cancelling_ = true;
  return Status::kOk;
}

Status OrderedInterface::cancelConnect() {
  if (station_.id == 0 || station_.kind != OperationKind::kConnect)
    return Status::kNotFound;
  if (cancelling_) return Status::kOk;
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
    queue_[(head_ + count_) % queue_.size()] = QueuedEvent(event);
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
        event = queue_[head_].expand();
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
  result.status = cancelling_ ? Status::kCancelled : error;
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
  result.status = scan_cancelling_ ? Status::kCancelled : error;
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
        finishStation(event.status, event.native_code);
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
      finishScan(event.status, event.native_code);
      break;
    case E::kAssociated:
      if (link_.phase != LinkPhase::kConnecting ||
          event.link.ssid.size != link_.ssid.size ||
          memcmp(event.link.ssid.bytes, link_.ssid.bytes, link_.ssid.size) !=
              0) {
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
           memcmp(event.link.ssid.bytes, link_.ssid.bytes, link_.ssid.size) !=
               0 ||
           memcmp(event.link.bssid.bytes, link_.bssid.bytes, 6) != 0)) {
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
           memcmp(event.link.ssid.bytes, link_.ssid.bytes, link_.ssid.size) !=
               0)) {
        return;
      }
      link_.phase = LinkPhase::kIdle;
      link_.has_ipv4 = false;
      link_.reason = event.status;
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
