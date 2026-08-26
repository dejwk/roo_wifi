/// @file
/// @brief Platform-independent Wi-Fi hardware interface contract.

#pragma once

#include <inttypes.h>

#include <string>
#include <vector>

#include "roo_backport/string_view.h"

namespace roo_wifi {

/// @brief Authentication modes reported by scanned access points.
/// @ingroup roo_wifi
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

/// @brief Pairwise and group cipher types reported by Wi-Fi hardware.
/// @ingroup roo_wifi
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

/// @brief Current Wi-Fi connection or scan status.
/// @ingroup roo_wifi
enum ConnectionStatus {
  WL_IDLE_STATUS = 0,      ///< Associated, but an IP address is not ready.
  WL_NO_SSID_AVAIL = 1,    ///< The requested network is unavailable.
  WL_SCAN_COMPLETED = 2,   ///< A network scan completed.
  WL_CONNECTED = 3,        ///< Connected and an IP address is available.
  WL_CONNECT_FAILED = 4,   ///< Authentication or connection failed.
  WL_CONNECTION_LOST = 5,  ///< An established connection was lost.
  WL_DISCONNECTED = 6      ///< Not connected.
};

/// @brief Detailed access-point information reported by `Interface`.
/// @ingroup roo_wifi
struct NetworkDetails {
  uint8_t bssid[6];            ///< MAC address of AP.
  uint8_t ssid[33];            ///< SSID of AP.
  uint8_t primary;             ///< Channel of AP.
  int8_t rssi;                 ///< Signal strength of AP.
  AuthMode authmode;           ///< Auth mode of AP.
  CipherType pairwise_cipher;  ///< Pairwise cipher of AP.
  CipherType group_cipher;     ///< Group cipher of AP.
  bool use_11b;                ///< Whether the AP supports IEEE 802.11b.
  bool use_11g;                ///< Whether the AP supports IEEE 802.11g.
  bool use_11n;                ///< Whether the AP supports IEEE 802.11n.
  /// Whether the AP supports Wi-Fi Protected Setup.
  bool supports_wps;

  ConnectionStatus status;  ///< Status associated with this network record.
};

/// @brief Platform abstraction for Wi-Fi hardware operations and events.
/// @ingroup roo_wifi
///
/// Connection requests are asynchronous: `connect()` starts an attempt and
/// completion is delivered to registered `EventListener` instances.
class Interface {
 public:
  /// @brief Events emitted by a Wi-Fi interface.
  enum EventType {
    EV_UNKNOWN = 0,            ///< Unrecognized or unsupported native event.
    EV_SCAN_COMPLETED = 1,     ///< A requested scan completed.
    EV_CONNECTED = 2,          ///< Associated with an access point.
    EV_GOT_IP = 3,             ///< Network address configuration completed.
    EV_DISCONNECTED = 4,       ///< Disconnected without a specific failure.
    EV_CONNECTION_FAILED = 5,  ///< Authentication or association failed.
    EV_CONNECTION_LOST = 6,    ///< A previously established link was lost.
  };

  /// @brief Receives synchronous dispatch of asynchronous interface events.
  class EventListener {
   public:
    /// @brief Virtual destructor.
    virtual ~EventListener() {}

    /// @brief Receives an interface event and its associated SSID.
    ///
    /// The listener is invoked synchronously as part of interface event
    /// dispatch, but the event itself is generally produced asynchronously
    /// relative to `connect()`. In particular, `connect()` may return and a
    /// new connection attempt may start before an earlier attempt reports its
    /// result. Consumers must therefore use `ssid`, rather than the current
    /// connection target, to associate delayed events with a network.
    ///
    /// `ssid` may be empty for events whose native representation does not
    /// identify a network. The view is valid only for the duration of this
    /// call; consumers that defer processing must make an owning copy.
    ///
    /// @param type The kind of event being reported.
    /// @param ssid The event's network SSID, if provided by the interface.
    virtual void onEvent(EventType type, roo::string_view ssid) = 0;
  };

  /// @brief Registers an interface event listener.
  /// @param listener Non-null listener that remains alive until removed.
  virtual void addEventListener(EventListener* listener) = 0;
  /// @brief Unregisters a previously registered event listener.
  /// @param listener Listener to remove.
  virtual void removeEventListener(EventListener* listener) = 0;

  /// @brief Enables or disables the physical Wi-Fi interface.
  /// @param enabled Whether the interface should be enabled.
  virtual void setEnabled(bool enabled) { (void)enabled; }

  /// @brief Clears credentials persisted by the platform Wi-Fi stack.
  virtual void clearPersistentCredentials() {}

  /// @brief Retrieves information about the currently connected AP.
  /// @param info Destination populated on success.
  /// @return `true` when connected AP information was available.
  virtual bool getApInfo(NetworkDetails* info) const = 0;
  /// @brief Starts an asynchronous network scan.
  /// @return `true` when the scan request was accepted.
  virtual bool startScan() = 0;
  /// @brief Reports whether the most recently requested scan has completed.
  virtual bool scanCompleted() const = 0;

  /// @brief Requests disconnection from the current network.
  virtual void disconnect() = 0;
  /// @brief Starts connecting to the specified SSID/password.
  ///
  /// A true return value means that the connection attempt was accepted, not
  /// that authentication or address acquisition succeeded. Completion is
  /// reported later through `EventListener`.
  ///
  /// @param ssid SSID to connect to.
  /// @param passwd Password, or an empty string for an open network.
  /// @return `true` when the connection request was accepted.
  virtual bool connect(const std::string& ssid, const std::string& passwd) = 0;
  /// @brief Returns the platform's current connection status.
  virtual ConnectionStatus getStatus() = 0;

  /// @brief Retrieves results from the most recently completed scan.
  /// @param list Destination replaced with up to `max_count` results.
  /// @param max_count Maximum number of results to return.
  /// @return `true` when scan results were available.
  virtual bool getScanResults(std::vector<NetworkDetails>* list,
                              int max_count) const = 0;
  /// @brief Virtual destructor.
  virtual ~Interface() {}
};

}  // namespace roo_wifi
