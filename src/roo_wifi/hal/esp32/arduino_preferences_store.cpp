#include "roo_wifi/hal/esp32/arduino_preferences_store.h"

#include <cstring>
namespace roo_wifi {
namespace {
Error Read(roo_prefs::ReadResult result) {
  switch (result) {
    case roo_prefs::ReadResult::kOk:
      return Error::kOk;
    case roo_prefs::ReadResult::kNotFound:
      return Error::kNotFound;
    case roo_prefs::ReadResult::kWrongType:
      return Error::kCorrupt;
    default:
      return Error::kStorageFailure;
  }
}

void LegacyKey(const Ssid &ssid, char (&out)[16]) {
  uint64_t hash = 525201411107845655ull;
  for (size_t i = 0; i < ssid.size; ++i) {
    hash ^= static_cast<char>(ssid.bytes[i]);
    hash *= 0x5bd1e9955bd1e995ull;
    hash ^= hash >> 47;
  }
  out[0] = 'p';
  out[1] = 'w';
  out[2] = '-';
  for (int i = 0; i < 11; ++i) {
    out[i + 3] = (hash & 0x3f) + 48;
    hash >>= 6;
  }
  out[14] = 0;
}
}  // namespace
ArduinoPreferencesStore::ArduinoPreferencesStore() : collection_("roo/wifi") {}
Error ArduinoPreferencesStore::begin() {
  roo_prefs::Transaction transaction(collection_);
  return transaction.active() ? Error::kOk : Error::kStorageFailure;
}

Error ArduinoPreferencesStore::readEnabled(bool &out) const {
  roo_prefs::Transaction t(collection_,
                           roo_prefs::Transaction::Mode::kReadOnly);
  if (!t.active()) return Error::kStorageFailure;
  bool value;
  Error error = Read(t.store().readBool("enabled", value));
  if (error == Error::kOk) out = value;
  return error;
}

Error ArduinoPreferencesStore::writeEnabled(bool enabled) {
  roo_prefs::Transaction t(collection_);
  if (!t.active()) return Error::kStorageFailure;
  return t.store().writeBool("enabled", enabled) == roo_prefs::WriteResult::kOk
             ? Error::kOk
             : Error::kStorageFailure;
}

Error ArduinoPreferencesStore::readField(const char *key, uint8_t *out,
                                         size_t &size) const {
  roo_prefs::Transaction t(collection_,
                           roo_prefs::Transaction::Mode::kReadOnly);
  if (!t.active()) return Error::kStorageFailure;
  size_t length = 0;
  Error error = Read(t.store().readBytes(key, out, size, &length));
  if (error == Error::kOk) size = length;
  return error;
}

Error ArduinoPreferencesStore::writeField(const char *key, const uint8_t *data,
                                          size_t size) {
  roo_prefs::Transaction t(collection_);
  if (!t.active()) return Error::kStorageFailure;
  return t.store().writeBytes(key, data, size) == roo_prefs::WriteResult::kOk
             ? Error::kOk
             : Error::kStorageFailure;
}

Error ArduinoPreferencesStore::eraseField(const char *key) {
  roo_prefs::Transaction t(collection_);
  if (!t.active()) return Error::kStorageFailure;
  if (!t.store().isKey(key)) return Error::kOk;
  return t.store().clear(key) == roo_prefs::ClearResult::kOk
             ? Error::kOk
             : Error::kStorageFailure;
}

SaveResult ArduinoPreferencesStore::importLegacy(
    ProfileId id, const ProfileSettings &settings) {
  const Ssid &ssid = settings.connection.ssid;
  if (!ssid.size || ssid.size > 32 || memchr(ssid.bytes, 0, ssid.size))
    return {Error::kInvalidArgument, id};
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  if (settings.connection.security != AuthMode::kOpen) {
    roo_prefs::Transaction t(collection_,
                             roo_prefs::Transaction::Mode::kReadOnly);
    if (!t.active()) return {Error::kStorageFailure, id};
    char key[16];
    LegacyKey(ssid, key);
    std::string password;
    Error error = Read(t.store().readString(key, password));
    if (error != Error::kOk) return {error, id};
    if (password.size() > 64) return {Error::kCorrupt, id};
    update.intent = CredentialIntent::kReplace;
    update.replacement.size = password.size();
    memcpy(update.replacement.bytes, password.data(), password.size());
    update.replacement.encoding = settings.connection.security == AuthMode::kWep
                                      ? CredentialEncoding::kWepKey
                                  : password.size() == 64
                                      ? CredentialEncoding::kRawPsk
                                      : CredentialEncoding::kPassphrase;
  }
  return saveProfile(id, settings, update);
}

Error ArduinoPreferencesStore::readLegacyDefault(Ssid &out) const {
  roo_prefs::Transaction t(collection_,
                           roo_prefs::Transaction::Mode::kReadOnly);
  if (!t.active()) return Error::kStorageFailure;
  std::string ssid;
  Error error = Read(t.store().readString("ssid", ssid));
  if (error != Error::kOk) return error;
  if (ssid.empty() || ssid.size() > 32) return Error::kCorrupt;
  Ssid result;
  result.size = ssid.size();
  memcpy(result.bytes, ssid.data(), ssid.size());
  out = result;
  return Error::kOk;
}
}  // namespace roo_wifi
