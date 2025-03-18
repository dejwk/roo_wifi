#include "arduino_preferences_store.h"

namespace roo_wifi {

namespace {

uint64_t inline MurmurOAAT64(const char* key) {
  uint64_t h(525201411107845655ull);
  for (; *key != '\0'; ++key) {
    h ^= *key;
    h *= 0x5bd1e9955bd1e995;
    h ^= h >> 47;
  }
  return h;
}

void ToSsiPwdKey(const std::string& ssid, char* result) {
  uint64_t hash = MurmurOAAT64(ssid.c_str());
  *result++ = 'p';
  *result++ = 'w';
  *result++ = '-';
  // We break 64 bits into 11 groups of 6 bits; then to ASCII.
  for (int i = 0; i < 11; i++) {
    *result++ = (hash & 0x3F) + 48;  // ASCII from '0' to 'o'.
    hash >>= 6;
  }
  *result = '\0';
}

}  // namespace

ArduinoPreferencesStore::ArduinoPreferencesStore()
    : collection_("roo/wifi"),
      is_interface_enabled_(collection_, "enabled", false),
      default_ssid_(collection_, "ssid", "") {}

bool ArduinoPreferencesStore::getIsInterfaceEnabled() {
  return is_interface_enabled_.get();
}

void ArduinoPreferencesStore::setIsInterfaceEnabled(bool enabled) {
  is_interface_enabled_.set(enabled);
}

std::string ArduinoPreferencesStore::getDefaultSSID() {
  return default_ssid_.get();
}

void ArduinoPreferencesStore::setDefaultSSID(const std::string& ssid) {
  default_ssid_.set(ssid);
}

void ArduinoPreferencesStore::clearDefaultSSID() {
  roo_prefs::Transaction t(collection_);
  t.store().clear("ssid");
}

bool ArduinoPreferencesStore::getPassword(const std::string& ssid,
                                          std::string& password) {
  roo_prefs::Transaction t(collection_, true);
  char pwkey[16];
  ToSsiPwdKey(ssid, pwkey);
  return (t.store().readString(pwkey, password) == roo_prefs::READ_OK);
}

void ArduinoPreferencesStore::setPassword(const std::string& ssid,
                                          roo::string_view password) {
  roo_prefs::Transaction t(collection_);
  char pwkey[16];
  ToSsiPwdKey(ssid, pwkey);
  t.store().writeString(pwkey, password);
}

void ArduinoPreferencesStore::clearPassword(const std::string& ssid) {
  roo_prefs::Transaction t(collection_);
  char pwkey[16];
  ToSsiPwdKey(ssid, pwkey);
  t.store().clear(pwkey);
}

}  // namespace roo_wifi
