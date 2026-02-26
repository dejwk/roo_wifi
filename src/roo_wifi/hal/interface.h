#pragma once

#include <inttypes.h>

#include <string>
#include <vector>

namespace roo_wifi {

/// Wi-Fi authentication modes.
enum AuthMode {
  WIFI_AUTH_OPEN = 0,         ///< Open.
  WIFI_AUTH_WEP,              ///< WEP.
  WIFI_AUTH_WPA_PSK,          ///< WPA-PSK.
  WIFI_AUTH_WPA2_PSK,         ///< WPA2-PSK.
  WIFI_AUTH_WPA_WPA2_PSK,     ///< WPA/WPA2-PSK.
  WIFI_AUTH_WPA2_ENTERPRISE,  ///< WPA2-Enterprise.
  WIFI_AUTH_WPA3_PSK,         ///< WPA3-PSK.
  WIFI_AUTH_WPA2_WPA3_PSK,    ///< WPA2/WPA3-PSK.
  WIFI_AUTH_WAPI_PSK,         ///< WAPI-PSK.
  WIFI_AUTH_UNKNOWN           ///< Unknown.
};

/// Wi-Fi cipher types.
enum CipherType {
  WIFI_CIPHER_TYPE_NONE = 0,     ///< None.
  WIFI_CIPHER_TYPE_WEP40,        ///< WEP40.
  WIFI_CIPHER_TYPE_WEP104,       ///< WEP104.
  WIFI_CIPHER_TYPE_TKIP,         ///< TKIP.
  WIFI_CIPHER_TYPE_CCMP,         ///< CCMP.
  WIFI_CIPHER_TYPE_TKIP_CCMP,    ///< TKIP+CCMP.
  WIFI_CIPHER_TYPE_AES_CMAC128,  ///< AES-CMAC-128.
  WIFI_CIPHER_TYPE_SMS4,         ///< SMS4.
  WIFI_CIPHER_TYPE_GCMP,         ///< GCMP.
  WIFI_CIPHER_TYPE_GCMP256,      ///< GCMP-256.
  WIFI_CIPHER_TYPE_AES_GMAC128,  ///< AES-GMAC-128.
  WIFI_CIPHER_TYPE_AES_GMAC256,  ///< AES-GMAC-256.
  WIFI_CIPHER_TYPE_UNKNOWN,      ///< Unknown.
};

/// Wi-Fi connection status.
enum ConnectionStatus {
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6
};

/// Detailed network information reported by the interface.
struct NetworkDetails {
  uint8_t bssid[6];            ///< MAC address of AP.
  uint8_t ssid[33];            ///< SSID of AP.
  uint8_t primary;             ///< Channel of AP.
  int8_t rssi;                 ///< Signal strength of AP.
  AuthMode authmode;           ///< Auth mode of AP.
  CipherType pairwise_cipher;  ///< Pairwise cipher of AP.
  CipherType group_cipher;     ///< Group cipher of AP.
  bool use_11b;
  bool use_11g;
  bool use_11n;
  bool supports_wps;

  ConnectionStatus status;
};

/// Abstraction for interacting with the hardware Wi-Fi interface.
class Interface {
 public:
  /// Interface event types.
  enum EventType {
    EV_UNKNOWN = 0,
    EV_SCAN_COMPLETED = 1,
    EV_CONNECTED = 2,
    EV_GOT_IP = 3,
    EV_DISCONNECTED = 4,
    EV_CONNECTION_FAILED = 5,
    EV_CONNECTION_LOST = 6,
  };

  /// Listener for interface events.
  class EventListener {
   public:
    virtual ~EventListener() {}
    virtual void onEvent(EventType type) {}
  };

  /// Registers an interface event listener.
  virtual void addEventListener(EventListener* listener) = 0;
  /// Unregisters an interface event listener.
  virtual void removeEventListener(EventListener* listener) = 0;

  /// Returns current AP information; false if not connected.
  virtual bool getApInfo(NetworkDetails* info) const = 0;
  /// Starts a scan.
  virtual bool startScan() = 0;
  /// Returns true if the last scan has completed.
  virtual bool scanCompleted() const = 0;

  /// Disconnects from the current network.
  virtual void disconnect() = 0;
  /// Connects to the specified SSID/password.
  virtual bool connect(const std::string& ssid, const std::string& passwd) = 0;
  /// Returns the current connection status.
  virtual ConnectionStatus getStatus() = 0;

  /// Returns scan results, up to max_count entries.
  virtual bool getScanResults(std::vector<NetworkDetails>* list,
                              int max_count) const = 0;
  /// Virtual destructor.
  virtual ~Interface() {}
};

}  // namespace roo_wifi
