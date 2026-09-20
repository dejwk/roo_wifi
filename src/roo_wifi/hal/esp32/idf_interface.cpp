#include "roo_wifi/hal/esp32/idf_interface.h"

#include <algorithm>
#include <cstring>

#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_wifi_default.h"

namespace roo_wifi {
namespace {

roo::mutex owner_mutex;
Esp32Station *owner = nullptr;

/// Maps an ESP authentication mode to its portable equivalent.
AuthMode Auth(wifi_auth_mode_t mode) {
  switch (mode) {
    case WIFI_AUTH_OPEN:
      return AuthMode::kOpen;
    case WIFI_AUTH_WEP:
      return AuthMode::kWep;
    case WIFI_AUTH_WPA_PSK:
      return AuthMode::kWpaPersonal;
    case WIFI_AUTH_WPA2_PSK:
      return AuthMode::kWpa2Personal;
    case WIFI_AUTH_WPA_WPA2_PSK:
      return AuthMode::kWpaWpa2Personal;
    case WIFI_AUTH_WPA2_ENTERPRISE:
      return AuthMode::kEnterprise;
    case WIFI_AUTH_WPA3_PSK:
      return AuthMode::kWpa3Personal;
    case WIFI_AUTH_WPA2_WPA3_PSK:
      return AuthMode::kWpa2Wpa3Personal;
    case WIFI_AUTH_WAPI_PSK:
      return AuthMode::kWapiPersonal;
    default:
      return AuthMode::kUnknown;
  }
}

/// Maps an ESP cipher type to its portable equivalent.
CipherType Cipher(wifi_cipher_type_t c) {
  switch (c) {
    case WIFI_CIPHER_TYPE_NONE:
      return CipherType::kNone;
    case WIFI_CIPHER_TYPE_WEP40:
      return CipherType::kWep40;
    case WIFI_CIPHER_TYPE_WEP104:
      return CipherType::kWep104;
    case WIFI_CIPHER_TYPE_TKIP:
      return CipherType::kTkip;
    case WIFI_CIPHER_TYPE_CCMP:
      return CipherType::kCcmp;
    case WIFI_CIPHER_TYPE_TKIP_CCMP:
      return CipherType::kTkipCcmp;
    case WIFI_CIPHER_TYPE_AES_CMAC128:
      return CipherType::kAesCmac128;
    case WIFI_CIPHER_TYPE_SMS4:
      return CipherType::kSms4;
    case WIFI_CIPHER_TYPE_GCMP:
      return CipherType::kGcmp;
    case WIFI_CIPHER_TYPE_GCMP256:
      return CipherType::kGcmp256;
    default:
      return CipherType::kUnknown;
  }
}

/// Translates an ESP access-point record into a portable scan record.
ScanRecord Record(const wifi_ap_record_t &ap) {
  ScanRecord r;
  r.ssid.size = strnlen(reinterpret_cast<const char *>(ap.ssid), 32);
  memcpy(r.ssid.bytes, ap.ssid, r.ssid.size);
  memcpy(r.bssid.bytes, ap.bssid, 6);
  r.security = Auth(ap.authmode);
  r.pairwise_cipher = Cipher(ap.pairwise_cipher);
  r.group_cipher = Cipher(ap.group_cipher);
  r.has_radio_metadata = true;
  r.use_11b = ap.phy_11b != 0;
  r.use_11g = ap.phy_11g != 0;
  r.use_11n = ap.phy_11n != 0;
  r.supports_wps = ap.wps != 0;
  r.rssi_dbm = ap.rssi;
  r.channel = ap.primary;
  return r;
}

/// Translates an ESP-IDF IPv4 address into its portable representation.
Ipv4Address Address(const esp_ip4_addr_t &ip) {
  const uint32_t raw = ip.addr;
  return {{static_cast<uint8_t>(raw), static_cast<uint8_t>(raw >> 8),
           static_cast<uint8_t>(raw >> 16), static_cast<uint8_t>(raw >> 24)}};
}

/// Translates a portable IPv4 address into ESP-IDF's native representation.
esp_ip4_addr_t Address(const Ipv4Address &ip) {
  esp_ip4_addr_t result = {};
  result.addr = static_cast<uint32_t>(ip.bytes[0]) |
                (static_cast<uint32_t>(ip.bytes[1]) << 8) |
                (static_cast<uint32_t>(ip.bytes[2]) << 16) |
                (static_cast<uint32_t>(ip.bytes[3]) << 24);
  return result;
}

Ipv4Address PrefixMask(uint8_t prefix_length) {
  const uint32_t mask =
      prefix_length == 0 ? 0 : 0xffffffffu << (32 - prefix_length);
  return {{static_cast<uint8_t>(mask >> 24), static_cast<uint8_t>(mask >> 16),
           static_cast<uint8_t>(mask >> 8), static_cast<uint8_t>(mask)}};
}

/// Maps an ESP result code to the portable connection outcome.
Status Result(esp_err_t code) {
  return code == ESP_OK ? Status::kOk : Status::kConnectionFailed;
}

}  // namespace

Esp32Station::~Esp32Station() { detach(); }

Status Esp32Station::attach(Receiver &receiver) {
  roo::lock_guard<roo::mutex> lock(owner_mutex);
  if (owner != nullptr) return Status::kBusy;
  esp_err_t error = esp_netif_init();
  if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
    return Status::kConnectionFailed;
  }
  error = esp_event_loop_create_default();
  if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
    return Status::kConnectionFailed;
  }
  if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == nullptr &&
      esp_netif_create_default_wifi_sta() == nullptr) {
    return Status::kConnectionFailed;
  }
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  error = esp_wifi_init(&init);
  if (error != ESP_OK && error != ESP_ERR_WIFI_INIT_STATE) {
    return Status::kConnectionFailed;
  }
  if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
    return Status::kConnectionFailed;
  }
  receiver_ = &receiver;
  error = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              &Dispatch, this, &wifi_handler_);
  if (error == ESP_OK) {
    error = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                &Dispatch, this, &ip_handler_);
  }
  if (error != ESP_OK) {
    if (wifi_handler_ != nullptr) {
      esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            wifi_handler_);
    }
    wifi_handler_ = nullptr;
    receiver_ = nullptr;
    return Status::kConnectionFailed;
  }
  owner = this;
  return Status::kOk;
}

void Esp32Station::detach() {
  roo::lock_guard<roo::mutex> owner_lock(owner_mutex);
  if (owner != this) return;
  if (wifi_handler_ != nullptr) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          wifi_handler_);
  }
  if (ip_handler_ != nullptr) {
    esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID,
                                          ip_handler_);
  }
  roo::unique_lock<roo::mutex> lock(mutex_);
  receiver_ = nullptr;
  wifi_handler_ = ip_handler_ = nullptr;
  owner = nullptr;
  selecting_ = scan_active_ = false;
  secret_ = {};
  esp_wifi_disconnect();
}

Support Esp32Station::support() const {
  Support s;
  for (AuthMode mode : {AuthMode::kOpen, AuthMode::kWep, AuthMode::kWpaPersonal,
                        AuthMode::kWpa2Personal, AuthMode::kWpaWpa2Personal}) {
    s.authentication_modes |= 1u << static_cast<unsigned>(mode);
  }
#if (defined(CONFIG_ESP_WIFI_ENABLE_WPA3_SAE) &&   \
     CONFIG_ESP_WIFI_ENABLE_WPA3_SAE) ||           \
    (defined(CONFIG_ESP32_WIFI_ENABLE_WPA3_SAE) && \
     CONFIG_ESP32_WIFI_ENABLE_WPA3_SAE)
  s.authentication_modes |=
      (1u << static_cast<unsigned>(AuthMode::kWpa3Personal)) |
      (1u << static_cast<unsigned>(AuthMode::kWpa2Wpa3Personal));
#endif
  s.hidden_networks = true;
  s.static_ipv4 = true;
  s.randomized_mac = true;
  s.scan_while_connected = true;
  return s;
}

Status Esp32Station::enable(bool enabled) {
  wifi_mode_t mode;
  if (esp_wifi_get_mode(&mode) != ESP_OK) {
    return Status::kConnectionFailed;
  }
  if ((mode == WIFI_MODE_STA && enabled) ||
      (mode == WIFI_MODE_NULL && !enabled)) {
    receiver_->post({enabled ? Event::kEnabled : Event::kDisabled});
    return Status::kOk;
  }
  if (enabled) {
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
      return Status::kConnectionFailed;
    }
    if (device_mac_[0] == 0 && device_mac_[1] == 0) {
      esp_wifi_get_mac(WIFI_IF_STA, device_mac_);
    }
  } else {
    if (esp_wifi_stop() != ESP_OK ||
        esp_wifi_set_mode(WIFI_MODE_NULL) != ESP_OK) {
      return Status::kConnectionFailed;
    }
  }
  return Status::kOk;
}

Status Esp32Station::scan(uint16_t capacity) {
  roo::unique_lock<roo::mutex> lock(mutex_);
  if (selecting_ || scan_active_) return Status::kBusy;
  capacity_ = capacity;
  records_.reserve(capacity);
  scan_cancelled_ = false;
  wifi_scan_config_t scan = {};
  scan.show_hidden = true;
  scan_active_ = true;
  lock.unlock();
  esp_err_t error = esp_wifi_scan_start(&scan, false);
  if (error != ESP_OK) {
    lock.lock();
    scan_active_ = false;
  }
  return Result(error);
}

Status Esp32Station::stopScan() {
  roo::unique_lock<roo::mutex> lock(mutex_);
  scan_cancelled_ = true;
  lock.unlock();
  Status status = Result(esp_wifi_scan_stop());
  return status;
}

Status Esp32Station::connect(const ConnectionConfig &config,
                             const Credentials &secret) {
  roo::unique_lock<roo::mutex> lock(mutex_);
  if (selecting_ || scan_active_) return Status::kBusy;
  // IDF's SSID filter is a C string, so reject unrepresentable byte SSIDs.
  if (memchr(config.ssid.bytes, 0, config.ssid.size) != nullptr)
    return Status::kUnsupported;
  config_ = config;
  secret_ = secret;
  uint8_t ssid[33] = {};
  memcpy(ssid, config.ssid.bytes, config.ssid.size);
  wifi_scan_config_t scan = {};
  scan.ssid = ssid;
  scan.show_hidden = true;
  selecting_ = true;
  scan_cancelled_ = false;
  lock.unlock();
  esp_err_t error = esp_wifi_scan_start(&scan, false);
  if (error != ESP_OK) {
    lock.lock();
    selecting_ = false;
    secret_ = {};
  }
  return Result(error);
}

Status Esp32Station::continueConnect() {
  {
    roo::lock_guard<roo::mutex> lock(mutex_);
    if (!prepared_) return Status::kNotFound;
    prepared_ = false;
  }
  return startSelected(selected_);
}

Status Esp32Station::disconnect() {
  roo::unique_lock<roo::mutex> lock(mutex_);
  if (prepared_) {
    prepared_ = false;
    secret_ = {};
    return Status::kNotFound;
  }
  if (selecting_) {
    scan_cancelled_ = true;
    lock.unlock();
    Status status = Result(esp_wifi_scan_stop());
    return status;
  }
  // The ordered layer synthesizes idle only when it already knows no attempt
  // exists. ESP_OK during pre-association cancellation is not proof of idle.
  lock.unlock();
  return Result(esp_wifi_disconnect());
}

Status Esp32Station::readScan(ScanRecord *out, size_t capacity,
                              ScanRead &result) const {
  roo::unique_lock<roo::mutex> lock(mutex_);
  size_t count = std::min(capacity, records_.size());
  std::copy_n(records_.begin(), count, out);
  result = {count, truncated_ || count < records_.size()};
  return Status::kOk;
}

Status Esp32Station::startSelected(const wifi_ap_record_t &ap) {
  uint8_t mac[6];
  memcpy(mac, device_mac_, 6);
  if (config_.mac_policy == MacPolicy::kRandomized) {
    esp_fill_random(mac, 6);
    mac[0] = (mac[0] & 0xfe) | 0x02;
  }
  if (esp_wifi_set_mac(WIFI_IF_STA, mac) != ESP_OK)
    return Status::kConnectionFailed;
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif == nullptr) return Status::kConnectionFailed;
  esp_err_t native_status = esp_netif_dhcpc_stop(netif);
  if (native_status != ESP_OK &&
      native_status != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED)
    return Status::kConnectionFailed;
  esp_netif_ip_info_t ip = {};
  esp_netif_dns_info_t dns1 = {};
  esp_netif_dns_info_t dns2 = {};
  dns1.ip.type = dns2.ip.type = ESP_IPADDR_TYPE_V4;
  if (config_.ip_mode == IpMode::kStaticIpv4) {
    const StaticIpv4 &settings = config_.static_ipv4;
    ip.ip = Address(settings.address);
    ip.gw = Address(settings.gateway);
    ip.netmask = Address(PrefixMask(settings.prefix_length));
    dns1.ip.u_addr.ip4 = Address(settings.dns1);
    if (settings.has_dns2) {
      dns2.ip.u_addr.ip4 = Address(settings.dns2);
    }
  }
  if (esp_netif_set_ip_info(netif, &ip) != ESP_OK ||
      esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns1) != ESP_OK ||
      esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns2) != ESP_OK) {
    return Status::kConnectionFailed;
  }
  if (config_.ip_mode == IpMode::kDhcp) {
    native_status = esp_netif_dhcpc_start(netif);
    if (native_status != ESP_OK &&
        native_status != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
      return Status::kConnectionFailed;
  }
  wifi_config_t config = {};
  memcpy(config.sta.ssid, config_.ssid.bytes, config_.ssid.size);
  memcpy(config.sta.password, secret_.bytes, secret_.size);
  config.sta.bssid_set = true;
  memcpy(config.sta.bssid, ap.bssid, 6);
  config.sta.channel = ap.primary;
  config.sta.threshold.authmode = ap.authmode;
  config.sta.pmf_cfg.capable = true;
  config.sta.pmf_cfg.required = config_.security == AuthMode::kWpa3Personal;
  Status status = Result(esp_wifi_set_config(WIFI_IF_STA, &config));
  secret_ = {};
  return status == Status::kOk ? Result(esp_wifi_connect()) : status;
}

void Esp32Station::Dispatch(void *context, esp_event_base_t base, int32_t id,
                            void *data) {
  static_cast<Esp32Station *>(context)->event(base, id, data);
}

void Esp32Station::event(esp_event_base_t base, int32_t id, void *data) {
  roo::unique_lock<roo::mutex> lock(mutex_);
  if (receiver_ == nullptr) return;
  Event event{};
  if (base == WIFI_EVENT) {
    switch (id) {
      case WIFI_EVENT_STA_START: {
        event.kind = Event::kEnabled;
        break;
      }
      case WIFI_EVENT_STA_STOP: {
        event.kind = Event::kDisabled;
        break;
      }
      case WIFI_EVENT_SCAN_DONE: {
        if (!selecting_ && !scan_active_) return;
        const wifi_event_sta_scan_done_t &done =
            *static_cast<wifi_event_sta_scan_done_t *>(data);
        uint16_t total = 0;
        esp_wifi_scan_get_ap_num(&total);
        // Bound both user scans and internal candidate selection.
        uint16_t count = std::min<uint16_t>(
            total, selecting_ ? 100 : std::max<uint16_t>(1, capacity_));
        std::vector<wifi_ap_record_t> aps(std::max<uint16_t>(count, 1));
        uint16_t fetched = std::max<uint16_t>(count, 1);
        esp_err_t error = esp_wifi_scan_get_ap_records(&fetched, aps.data());
        event.status = done.status == 0 && error == ESP_OK
                           ? Status::kOk
                           : Status::kConnectionFailed;
        event.native_code = done.status != 0 ? done.status : error;
        if (selecting_) {
          selecting_ = false;
          if (!scan_cancelled_ && event.status == Status::kOk) {
            for (size_t i = 0; i < fetched; ++i) {
              if (Auth(aps[i].authmode) != config_.security) continue;
              selected_ = aps[i];
              prepared_ = true;
              receiver_->post({Event::kPrepared});
              return;
            }
          }
          secret_ = {};
          event.kind = Event::kDisconnected;
          event.link.ssid = config_.ssid;
          event.status = Status::kConnectionFailed;
        } else {
          scan_active_ = false;
          event.kind = Event::kScanDone;
          if (event.status == Status::kOk && !scan_cancelled_) {
            records_.clear();
            for (size_t i = 0; i < std::min<size_t>(fetched, capacity_); ++i)
              records_.push_back(Record(aps[i]));
            truncated_ = total > records_.size();
          }
        }
        scan_cancelled_ = false;
        break;
      }
      case WIFI_EVENT_STA_CONNECTED: {
        const wifi_event_sta_connected_t &info =
            *static_cast<wifi_event_sta_connected_t *>(data);
        if (Auth(info.authmode) != config_.security) {
          lock.unlock();
          esp_wifi_disconnect();
          return;
        }
        event.kind = Event::kAssociated;
        event.link.ssid.size = std::min<uint8_t>(info.ssid_len, 32);
        memcpy(event.link.ssid.bytes, info.ssid, event.link.ssid.size);
        memcpy(event.link.bssid.bytes, info.bssid, 6);
        event.link.security = Auth(info.authmode);
        event.link.channel = info.channel;
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
          event.link.rssi_dbm = ap.rssi;
          event.link.has_radio_info = true;
        }
        event.link.has_station_mac =
            esp_wifi_get_mac(WIFI_IF_STA, event.link.station_mac.bytes) ==
            ESP_OK;
        break;
      }
      case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t &info =
            *static_cast<wifi_event_sta_disconnected_t *>(data);
        event.kind = Event::kDisconnected;
        event.link.ssid.size = std::min<uint8_t>(info.ssid_len, 32);
        memcpy(event.link.ssid.bytes, info.ssid, event.link.ssid.size);
        memcpy(event.link.bssid.bytes, info.bssid, 6);
        event.native_code = info.reason;
        event.status = Status::kConnectionFailed;
        break;
      }
      default: {
        return;
      }
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t &info = *static_cast<ip_event_got_ip_t *>(data);
    if (info.esp_netif != esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"))
      return;
    wifi_ap_record_t current_ap = {};
    esp_netif_ip_info_t current_ip = {};
    if (esp_wifi_sta_get_ap_info(&current_ap) != ESP_OK ||
        esp_netif_get_ip_info(info.esp_netif, &current_ip) != ESP_OK ||
        current_ip.ip.addr == 0 || current_ip.ip.addr == 0xffffffffu ||
        current_ip.ip.addr != info.ip_info.ip.addr ||
        memcmp(current_ap.bssid, selected_.bssid, 6) != 0) {
      return;
    }
    event.link.ssid = Record(current_ap).ssid;
    memcpy(event.link.bssid.bytes, current_ap.bssid, 6);
    event.kind = Event::kAddressReady;
    event.link.address = Address(info.ip_info.ip);
    event.link.gateway = Address(info.ip_info.gw);
    event.link.has_ipv4 = true;
    esp_netif_dns_info_t dns = {};
    if (esp_netif_get_dns_info(info.esp_netif, ESP_NETIF_DNS_MAIN, &dns) ==
            ESP_OK &&
        dns.ip.u_addr.ip4.addr != 0) {
      event.link.dns1 = Address(dns.ip.u_addr.ip4);
      event.link.has_dns1 = true;
    }
    if (esp_netif_get_dns_info(info.esp_netif, ESP_NETIF_DNS_BACKUP, &dns) ==
            ESP_OK &&
        dns.ip.u_addr.ip4.addr != 0) {
      event.link.dns2 = Address(dns.ip.u_addr.ip4);
      event.link.has_dns2 = true;
    }
  } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
    // Loss has no connection identity. Verify current netif state; never clear
    // a newly acquired address merely because an old loss timer expired.
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t current_ip = {};
    if (netif != nullptr &&
        esp_netif_get_ip_info(netif, &current_ip) == ESP_OK &&
        current_ip.ip.addr != 0) {
      return;
    }
    event.kind = Event::kAddressLost;
  } else {
    return;
  }
  receiver_->post(event);
}

}  // namespace roo_wifi
