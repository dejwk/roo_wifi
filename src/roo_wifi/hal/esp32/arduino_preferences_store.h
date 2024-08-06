#pragma once

#include "roo_prefs.h"
#include "roo_wifi/hal/store.h"

namespace roo_wifi {

class ArduinoPreferencesStore : public Store {
 public:
  ArduinoPreferencesStore() : collection_("roo/t/wifi") {}

  void begin() {}

  bool getIsInterfaceEnabled() override;

  void setIsInterfaceEnabled(bool enabled) override;

  std::string getDefaultSSID() override;

  void clearDefaultSSID() override;

  void setDefaultSSID(const std::string& ssid) override;

  bool getPassword(const std::string& ssid, std::string& password) override;

  void setPassword(const std::string& ssid,
                   const std::string& password) override;

  void clearPassword(const std::string& ssid) override;

 private:
  roo_prefs::Collection collection_;
};

}  // namespace roo_wifi
