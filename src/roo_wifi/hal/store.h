#pragma once

#include <inttypes.h>

#include <string>
#include <vector>

namespace roo_wifi {

// Abstraction for persistently storing the data used by the WiFi controller.
class Store {
 public:
  virtual bool getIsInterfaceEnabled() = 0;
  virtual void setIsInterfaceEnabled(bool enabled) = 0;
  virtual std::string getDefaultSSID() = 0;
  virtual void setDefaultSSID(const std::string& ssid) = 0;
  virtual void clearDefaultSSID() = 0;
  virtual bool getPassword(const std::string& ssid, std::string& password) = 0;
  virtual void setPassword(const std::string& ssid,
                           const std::string& password) = 0;
  virtual void clearPassword(const std::string& ssid) = 0;
};

}  // namespace roo_wifi
