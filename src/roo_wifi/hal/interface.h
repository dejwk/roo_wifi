#pragma once

#include <inttypes.h>
#include <string>
#include <vector>

namespace roo_wifi {

enum AuthMode {
  WIFI_AUTH_OPEN = 0,        /**< authenticate mode : open */
  WIFI_AUTH_WEP,             /**< authenticate mode : WEP */
  WIFI_AUTH_WPA_PSK,         /**< authenticate mode : WPA_PSK */
  WIFI_AUTH_WPA2_PSK,        /**< authenticate mode : WPA2_PSK */
  WIFI_AUTH_WPA_WPA2_PSK,    /**< authenticate mode : WPA_WPA2_PSK */
  WIFI_AUTH_WPA2_ENTERPRISE, /**< authenticate mode : WPA2_ENTERPRISE */
  WIFI_AUTH_WPA3_PSK,        /**< authenticate mode : WPA3_PSK */
  WIFI_AUTH_WPA2_WPA3_PSK,   /**< authenticate mode : WPA2_WPA3_PSK */
  WIFI_AUTH_WAPI_PSK,        /**< authenticate mode : WAPI_PSK */
  WIFI_AUTH_UNKNOWN
};

enum CipherType {
  WIFI_CIPHER_TYPE_NONE = 0,    /**< the cipher type is none */
  WIFI_CIPHER_TYPE_WEP40,       /**< the cipher type is WEP40 */
  WIFI_CIPHER_TYPE_WEP104,      /**< the cipher type is WEP104 */
  WIFI_CIPHER_TYPE_TKIP,        /**< the cipher type is TKIP */
  WIFI_CIPHER_TYPE_CCMP,        /**< the cipher type is CCMP */
  WIFI_CIPHER_TYPE_TKIP_CCMP,   /**< the cipher type is TKIP and CCMP */
  WIFI_CIPHER_TYPE_AES_CMAC128, /**< the cipher type is AES-CMAC-128 */
  WIFI_CIPHER_TYPE_SMS4,        /**< the cipher type is SMS4 */
  WIFI_CIPHER_TYPE_GCMP,        /**< the cipher type is GCMP */
  WIFI_CIPHER_TYPE_GCMP256,     /**< the cipher type is GCMP-256 */
  WIFI_CIPHER_TYPE_AES_GMAC128, /**< the cipher type is AES-GMAC-128 */
  WIFI_CIPHER_TYPE_AES_GMAC256, /**< the cipher type is AES-GMAC-256 */
  WIFI_CIPHER_TYPE_UNKNOWN,     /**< the cipher type is unknown */
};

enum ConnectionStatus {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6
};

struct NetworkDetails {
  uint8_t bssid[6];           /**< MAC address of AP */
  uint8_t ssid[33];           /**< SSID of AP */
  uint8_t primary;            /**< channel of AP */
  int8_t rssi;                /**< signal strength of AP */
  AuthMode authmode;          /**< authmode of AP */
  CipherType pairwise_cipher; /**< pairwise cipher of AP */
  CipherType group_cipher;    /**< group cipher of AP */
  bool use_11b;
  bool use_11g;
  bool use_11n;
  bool supports_wps;

  ConnectionStatus status;
};

// Abstraction for interacting with the hardware WiFi interface.
class Interface {
 public:
  enum EventType {
    EV_UNKNOWN = 0,
    EV_SCAN_COMPLETED = 1,
    EV_CONNECTED = 2,
    EV_GOT_IP = 3,
    EV_DISCONNECTED = 4,
    EV_CONNECTION_FAILED = 5,
    EV_CONNECTION_LOST = 6,
  };

  class EventListener {
   public:
    virtual ~EventListener() {}
    virtual void onEvent(EventType type) {}
  };

  virtual void addEventListener(EventListener* listener) = 0;
  virtual void removeEventListener(EventListener* listener) = 0;

  virtual bool getApInfo(NetworkDetails* info) const = 0;
  virtual bool startScan() = 0;
  virtual bool scanCompleted() const = 0;

  virtual void disconnect() = 0;
  virtual bool connect(const std::string& ssid, const std::string& passwd) = 0;
  virtual ConnectionStatus getStatus() = 0;

  virtual bool getScanResults(std::vector<NetworkDetails>* list,
                              int max_count) const = 0;
  virtual ~Interface() {}
};

}  // namespace roo_wifi
