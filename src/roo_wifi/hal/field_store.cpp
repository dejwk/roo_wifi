#include "roo_wifi/hal/field_store.h"

#include <cstdio>
#include <cstring>

namespace roo_wifi {
namespace {
constexpr uint8_t kIncomplete = 0x10, kReady = 0x11, kDeleted = 0x12;
constexpr const char *kFields[] = {
    "ssid", "auth",   "hidden",  "ip",  "addr", "gw",  "dns1",
    "dns2", "prefix", "dns2set", "mac", "auto", "enc", "secret"};

void Key(ProfileId id, const char *field, char (&out)[16]) {
  snprintf(out, sizeof(out), "%08lx%s", static_cast<unsigned long>(id), field);
}

// Each field has a stable byte representation independent of C++ padding/ABI.
size_t Encode(size_t f, const ProfileSettings &s, const Credentials &c,
              uint8_t *out) {
  const ConnectionConfig &p = s.connection;
  switch (f) {
    case 0:
      memcpy(out, p.ssid.bytes, p.ssid.size);
      return p.ssid.size;
    case 1:
      out[0] = static_cast<uint8_t>(p.security);
      break;
    case 2:
      out[0] = p.hidden;
      break;
    case 3:
      out[0] = static_cast<uint8_t>(p.ip_mode);
      break;
    case 4:
      memcpy(out, p.static_ipv4.address.bytes, 4);
      return 4;
    case 5:
      memcpy(out, p.static_ipv4.gateway.bytes, 4);
      return 4;
    case 6:
      memcpy(out, p.static_ipv4.dns1.bytes, 4);
      return 4;
    case 7:
      memcpy(out, p.static_ipv4.dns2.bytes, 4);
      return 4;
    case 8:
      out[0] = p.static_ipv4.prefix_length;
      break;
    case 9:
      out[0] = p.static_ipv4.has_dns2;
      break;
    case 10:
      out[0] = static_cast<uint8_t>(p.mac_policy);
      break;
    case 11:
      out[0] = s.auto_connect;
      break;
    case 12:
      out[0] = static_cast<uint8_t>(c.encoding);
      break;
    case 13:
      memcpy(out, c.bytes, c.size);
      return c.size;
  }
  return 1;
}

Status Decode(size_t f, const uint8_t *data, size_t n, ProfileSettings &s,
              Credentials &c) {
  ConnectionConfig &p = s.connection;
  if (f == 0) {
    if (!n || n > 32) return Status::kCorrupt;
    memcpy(p.ssid.bytes, data, n);
    p.ssid.size = n;
    return Status::kOk;
  }
  if (f == 13) {
    if (n > 64) return Status::kCorrupt;
    memcpy(c.bytes, data, n);
    c.size = n;
    return Status::kOk;
  }
  if (f >= 4 && f <= 7) {
    if (n != 4) return Status::kCorrupt;
    Ipv4Address *addresses[] = {&p.static_ipv4.address, &p.static_ipv4.gateway,
                                &p.static_ipv4.dns1, &p.static_ipv4.dns2};
    memcpy(addresses[f - 4]->bytes, data, 4);
    return Status::kOk;
  }
  if (n != 1) return Status::kCorrupt;
  uint8_t v = data[0];
  if ((f == 2 || f == 9 || f == 11) && v > 1) return Status::kCorrupt;
  switch (f) {
    case 1:
      p.security = static_cast<AuthMode>(v);
      break;
    case 2:
      p.hidden = v;
      break;
    case 3:
      p.ip_mode = static_cast<IpMode>(v);
      break;
    case 8:
      p.static_ipv4.prefix_length = v;
      break;
    case 9:
      p.static_ipv4.has_dns2 = v;
      break;
    case 10:
      p.mac_policy = static_cast<MacPolicy>(v);
      break;
    case 11:
      s.auto_connect = v;
      break;
    case 12:
      c.encoding = static_cast<CredentialEncoding>(v);
      break;
  }
  return Status::kOk;
}
}  // namespace

Status FieldStore::readStatus(ProfileId id) const {
  if (!id) return Status::kInvalidArgument;
  char key[16];
  Key(id, "state", key);
  uint8_t data[64];
  size_t n = sizeof(data);
  Status error = readField(key, data, n);
  if (error != Status::kOk) return error;
  if (n != 1) return Status::kCorrupt;
  if (data[0] == kIncomplete) return Status::kIncomplete;
  if (data[0] == kDeleted) return Status::kNotFound;
  return data[0] == kReady ? Status::kOk : Status::kCorrupt;
}

Status FieldStore::read(ProfileId id, ProfileSettings &settings,
                        Credentials &secret) const {
  Status error = readStatus(id);
  if (error != Status::kOk) return error;
  for (size_t f = 0; f < 14; ++f) {
    char key[16];
    Key(id, kFields[f], key);
    uint8_t data[64];
    size_t n = sizeof(data);
    error = readField(key, data, n);
    if (f == 13 && error == Status::kNotFound &&
        settings.connection.security == AuthMode::kOpen) {
      n = 0;
      error = Status::kOk;
    }
    if (error != Status::kOk)
      return error == Status::kNotFound ? Status::kCorrupt : error;
    error = Decode(f, data, n, settings, secret);
    if (error != Status::kOk) return error;
  }
  return Validate(settings.connection, secret) == Status::kOk
             ? Status::kOk
             : Status::kCorrupt;
}

Status FieldStore::loadProfile(ProfileId id, Profile &out) const {
  Profile result;
  Credentials secret;
  Status error = read(id, result.settings, secret);
  if (error != Status::kOk) return error;
  result.id = id;
  result.has_credentials = secret.size != 0;
  out = result;
  return Status::kOk;
}

Status FieldStore::loadCredentials(ProfileId id, Credentials &out) const {
  ProfileSettings settings;
  Credentials secret;
  Status error = read(id, settings, secret);
  if (error == Status::kOk) out = secret;
  return error;
}

SaveResult FieldStore::saveProfile(ProfileId id,
                                   const ProfileSettings &settings,
                                   const CredentialUpdate &update) {
  if (!id) return {Status::kInvalidArgument, 0};
  Credentials secret;
  if (settings.connection.security == AuthMode::kOpen &&
      (update.intent != CredentialIntent::kClear || update.replacement.size))
    return {Status::kInvalidArgument, id};
  switch (update.intent) {
    case CredentialIntent::kKeep: {
      Status error = loadCredentials(id, secret);
      if (error != Status::kOk) return {error, id};
      break;
    }
    case CredentialIntent::kReplace:
      secret = update.replacement;
      break;
    case CredentialIntent::kClear:
      if (update.replacement.size) return {Status::kInvalidArgument, id};
      break;
    default:
      return {Status::kInvalidArgument, id};
  }
  Status error = Validate(settings.connection, secret);
  if (error != Status::kOk) return {error, id};
  char key[16];
  Key(id, "state", key);
  error = writeField(key, &kIncomplete, 1);
  if (error != Status::kOk) return {Status::kStorageFailure, id};
  for (size_t f = 0; f < 14; ++f) {
    uint8_t data[64];
    size_t n = Encode(f, settings, secret, data);
    Key(id, kFields[f], key);
    error = n ? writeField(key, data, n) : eraseField(key);
    if (error != Status::kOk) return {Status::kIncomplete, id};
  }
  Key(id, "state", key);
  if (writeField(key, &kReady, 1) == Status::kOk) return {Status::kOk, id};
  // A failed final commit can still have reached storage. Verify every field.
  error = readStatus(id);
  if (error == Status::kIncomplete) return {error, id};
  if (error != Status::kOk) return {Status::kCommitUnknown, id};
  ProfileSettings stored;
  Credentials stored_secret;
  error = read(id, stored, stored_secret);
  if (error != Status::kOk) return {Status::kCommitUnknown, id};
  for (size_t f = 0; f < 14; ++f) {
    uint8_t expected[64], actual[64];
    size_t a = Encode(f, settings, secret, expected),
           b = Encode(f, stored, stored_secret, actual);
    if (a != b || memcmp(expected, actual, a))
      return {Status::kCommitUnknown, id};
  }
  return {Status::kOk, id};
}

Status FieldStore::removeProfile(ProfileId id) {
  if (!id) return Status::kInvalidArgument;
  char key[16];
  Key(id, "state", key);
  if (writeField(key, &kDeleted, 1) != Status::kOk)
    return Status::kStorageFailure;
  Status result = Status::kOk;
  for (const char *field : kFields) {
    Key(id, field, key);
    if (eraseField(key) != Status::kOk) result = Status::kStorageFailure;
  }
  return result;
}
}  // namespace roo_wifi
