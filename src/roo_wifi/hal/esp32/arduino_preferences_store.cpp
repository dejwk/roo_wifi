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

bool ArduinoPreferencesStore::getIsInterfaceEnabled() {
  roo_prefs::Transaction t(collection_, true);
  return t.store().getBool("enabled", false);
}

void ArduinoPreferencesStore::setIsInterfaceEnabled(bool enabled) {
  roo_prefs::Transaction t(collection_);
  t.store().putBool("enabled", enabled);
}

std::string ArduinoPreferencesStore::getDefaultSSID() {
  roo_prefs::Transaction t(collection_, true);
  if (!t.store().isKey("ssid")) {
    return "";
  }
  char buf[33];
  t.store().getString("ssid", buf, 33);
  return std::string(buf);
}

void ArduinoPreferencesStore::setDefaultSSID(const std::string& ssid) {
  roo_prefs::Transaction t(collection_);
  t.store().putString("ssid", ssid.c_str());
}

void ArduinoPreferencesStore::clearDefaultSSID() {
  roo_prefs::Transaction t(collection_);
  t.store().remove("ssid");
}

bool ArduinoPreferencesStore::getPassword(const std::string& ssid, std::string& password) {
  roo_prefs::Transaction t(collection_, true);
  char pwkey[16];
  ToSsiPwdKey(ssid, pwkey);
  if (!t.store().isKey(pwkey)) return false;
  char pwd[128];
  size_t len = t.store().getString(pwkey, pwd, 128);
  password = std::string(pwd, len);
  return true;
}

void ArduinoPreferencesStore::setPassword(const std::string& ssid,
                               const std::string& password) {
  roo_prefs::Transaction t(collection_);
  char pwkey[16];
  ToSsiPwdKey(ssid, pwkey);
  t.store().putString(pwkey, password.c_str());
}

void ArduinoPreferencesStore::clearPassword(const std::string& ssid) {
  roo_prefs::Transaction t(collection_);
  char pwkey[16];
  ToSsiPwdKey(ssid, pwkey);
  t.store().remove(pwkey);
}

}  // namespace roo_wifi
