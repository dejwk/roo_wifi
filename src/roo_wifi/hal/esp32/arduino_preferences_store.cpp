#include "roo_wifi/hal/esp32/arduino_preferences_store.h"

#include <cstring>

namespace roo_wifi {
namespace {
/// Maps a Preferences read outcome to its portable equivalent.
Status Read(roo_prefs::ReadResult result) {
  switch (result) {
    case roo_prefs::ReadResult::kOk:
      return Status::kOk;
    case roo_prefs::ReadResult::kNotFound:
      return Status::kNotFound;
    case roo_prefs::ReadResult::kWrongType:
      return Status::kCorrupt;
    default:
      return Status::kStorageFailure;
  }
}

/// Derives the legacy credential key associated with an SSID.
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

Status ArduinoPreferencesStore::begin() {
  roo_prefs::Transaction transaction(collection_);
  return transaction.active() ? Status::kOk : Status::kStorageFailure;
}

Status ArduinoPreferencesStore::readEnabled(bool &out) const {
  roo_prefs::Transaction t(collection_,
                           roo_prefs::Transaction::Mode::kReadOnly);
  if (!t.active()) return Status::kStorageFailure;
  bool value;
  Status status = Read(t.store().readBool("enabled", value));
  if (status == Status::kOk) out = value;
  return status;
}

Status ArduinoPreferencesStore::writeEnabled(bool enabled) {
  roo_prefs::Transaction t(collection_);
  if (!t.active()) return Status::kStorageFailure;
  return t.store().writeBool("enabled", enabled) == roo_prefs::WriteResult::kOk
             ? Status::kOk
             : Status::kStorageFailure;
}

Status ArduinoPreferencesStore::readField(const char *key, uint8_t *out,
                                          size_t &size) const {
  roo_prefs::Transaction t(collection_,
                           roo_prefs::Transaction::Mode::kReadOnly);
  if (!t.active()) return Status::kStorageFailure;
  size_t length = 0;
  Status status = Read(t.store().readBytes(key, out, size, &length));
  if (status == Status::kOk) size = length;
  return status;
}

Status ArduinoPreferencesStore::writeField(const char *key, const uint8_t *data,
                                           size_t size) {
  roo_prefs::Transaction t(collection_);
  if (!t.active()) return Status::kStorageFailure;
  return t.store().writeBytes(key, data, size) == roo_prefs::WriteResult::kOk
             ? Status::kOk
             : Status::kStorageFailure;
}

Status ArduinoPreferencesStore::eraseField(const char *key) {
  roo_prefs::Transaction t(collection_);
  if (!t.active()) return Status::kStorageFailure;
  if (!t.store().isKey(key)) return Status::kOk;
  return t.store().clear(key) == roo_prefs::ClearResult::kOk
             ? Status::kOk
             : Status::kStorageFailure;
}

Status ArduinoPreferencesStore::importLegacy(ProfileId id,
                                             const ProfileSettings &settings) {
  const Ssid &ssid = settings.connection.ssid;
  if (ssid.size == 0 || ssid.size > 32 ||
      memchr(ssid.bytes, 0, ssid.size) != nullptr)
    return Status::kInvalidArgument;
  CredentialUpdate update;
  update.intent = CredentialIntent::kClear;
  if (settings.connection.security != AuthMode::kOpen) {
    roo_prefs::Transaction t(collection_,
                             roo_prefs::Transaction::Mode::kReadOnly);
    if (!t.active()) return Status::kStorageFailure;
    char key[16];
    LegacyKey(ssid, key);
    std::string password;
    Status status = Read(t.store().readString(key, password));
    if (status != Status::kOk) return status;
    if (password.size() > 64) return Status::kCorrupt;
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

Status ArduinoPreferencesStore::readLegacyDefault(Ssid &out) const {
  roo_prefs::Transaction t(collection_,
                           roo_prefs::Transaction::Mode::kReadOnly);
  if (!t.active()) return Status::kStorageFailure;
  std::string ssid;
  Status status = Read(t.store().readString("ssid", ssid));
  if (status != Status::kOk) return status;
  if (ssid.empty() || ssid.size() > 32) return Status::kCorrupt;
  Ssid result;
  result.size = ssid.size();
  memcpy(result.bytes, ssid.data(), ssid.size());
  out = result;
  return Status::kOk;
}
}  // namespace roo_wifi
