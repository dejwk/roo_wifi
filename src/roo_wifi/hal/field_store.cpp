#include "roo_wifi/hal/field_store.h"

#include <cstdio>
#include <cstring>

namespace roo_wifi {
namespace {

constexpr uint8_t kIncomplete = 0x10;
constexpr uint8_t kReady = 0x11;
constexpr uint8_t kDeleted = 0x12;
constexpr const char *kFields[] = {
    "ssid", "auth",   "hidden",  "ip",  "addr", "gw",  "dns1",
    "dns2", "prefix", "dns2set", "mac", "auto", "enc", "secret"};

/// Parses the status-field key that identifies one profile record. Only keys
/// shaped as eight lowercase hex digits followed by `state` are accepted, so
/// radio enablement, legacy SSID/password entries, and every non-status profile
/// field in the same collection are ignored.
bool ProfileStateKey(const char *key, size_t size, ProfileId &id) {
  if (size != 13 || memcmp(key + 8, "state", 5) != 0) return false;
  ProfileId value = 0;
  for (size_t i = 0; i < 8; ++i) {
    char c = key[i];
    uint8_t digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10;
    } else {
      return false;
    }
    value = (value << 4) | digit;
  }
  if (value == 0) return false;
  id = value;
  return true;
}

/// Formats a stable field key for the supplied profile.
void Key(ProfileId id, const char *field, char (&out)[16]) {
  snprintf(out, sizeof(out), "%08lx%s", static_cast<unsigned long>(id), field);
}

/// Encodes one field independently of C++ padding and ABI details.
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

/// Decodes and validates one field from its stable byte representation.
Status Decode(size_t f, const uint8_t *data, size_t n, ProfileSettings &s,
              Credentials &c) {
  ConnectionConfig &p = s.connection;
  if (f == 0) {
    if (n == 0 || n > 32) return Status::kCorrupt;
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
      p.hidden = v != 0;
      break;
    case 3:
      p.ip_mode = static_cast<IpMode>(v);
      break;
    case 8:
      p.static_ipv4.prefix_length = v;
      break;
    case 9:
      p.static_ipv4.has_dns2 = v != 0;
      break;
    case 10:
      p.mac_policy = static_cast<MacPolicy>(v);
      break;
    case 11:
      s.auto_connect = v != 0;
      break;
    case 12:
      c.encoding = static_cast<CredentialEncoding>(v);
      break;
  }
  return Status::kOk;
}

}  // namespace

Status FieldStore::readStatus(ProfileId id) const {
  if (id == 0) return Status::kInvalidArgument;
  char key[16];
  Key(id, "state", key);
  uint8_t data[64];
  size_t n = sizeof(data);
  Status status = readField(key, data, n);
  if (status != Status::kOk) return status;
  if (n != 1) return Status::kCorrupt;
  if (data[0] == kIncomplete) return Status::kIncomplete;
  if (data[0] == kDeleted) return Status::kNotFound;
  return data[0] == kReady ? Status::kOk : Status::kCorrupt;
}

Status FieldStore::read(ProfileId id, ProfileSettings &settings,
                        Credentials &secret) const {
  Status status = readStatus(id);
  if (status != Status::kOk) return status;
  for (size_t f = 0; f < 14; ++f) {
    char key[16];
    Key(id, kFields[f], key);
    uint8_t data[64];
    size_t n = sizeof(data);
    status = readField(key, data, n);
    if (f == 13 && status == Status::kNotFound &&
        settings.connection.security == AuthMode::kOpen) {
      n = 0;
      status = Status::kOk;
    }
    if (status != Status::kOk)
      return status == Status::kNotFound ? Status::kCorrupt : status;
    status = Decode(f, data, n, settings, secret);
    if (status != Status::kOk) return status;
  }
  return Validate(settings.connection, secret) == Status::kOk
             ? Status::kOk
             : Status::kCorrupt;
}

Status FieldStore::loadProfile(ProfileId id, Profile &out) const {
  Profile result;
  Credentials secret;
  Status status = read(id, result.settings, secret);
  if (status != Status::kOk) return status;
  result.id = id;
  result.has_credentials = secret.size != 0;
  out = result;
  return Status::kOk;
}

Status FieldStore::enumerateProfiles(ProfileVisitor visitor,
                                     void *context) const {
  if (visitor == nullptr) return Status::kInvalidArgument;
  struct Context {
    const FieldStore *store;
    ProfileVisitor visitor;
    void *visitor_context;
    Status status;
  } state = {this, visitor, context, Status::kOk};
  Status result = enumerateFields(
      [](void *opaque, const char *key, size_t size) {
        Context &state = *static_cast<Context *>(opaque);
        ProfileId id;
        if (!ProfileStateKey(key, size, id)) return true;
        Status status = state.store->readStatus(id);
        if (status == Status::kNotFound || status == Status::kIncomplete) {
          return true;
        }
        if (status != Status::kOk) {
          state.status = status;
          return false;
        }
        return state.visitor(state.visitor_context, id);
      },
      &state);
  return state.status == Status::kOk ? result : state.status;
}

Status FieldStore::loadCredentials(ProfileId id, Credentials &out) const {
  ProfileSettings settings;
  Credentials secret;
  Status status = read(id, settings, secret);
  if (status == Status::kOk) out = secret;
  return status;
}

Status FieldStore::saveProfile(ProfileId id, const ProfileSettings &settings,
                               const CredentialUpdate &update) {
  if (id == 0) return Status::kInvalidArgument;
  Credentials secret;
  if (settings.connection.security == AuthMode::kOpen &&
      (update.intent != CredentialIntent::kClear ||
       update.replacement.size != 0)) {
    return Status::kInvalidArgument;
  }
  switch (update.intent) {
    case CredentialIntent::kKeep: {
      Status status = loadCredentials(id, secret);
      if (status != Status::kOk) return status;
      break;
    }
    case CredentialIntent::kReplace: {
      secret = update.replacement;
      break;
    }
    case CredentialIntent::kClear: {
      if (update.replacement.size != 0) return Status::kInvalidArgument;
      break;
    }
    default: {
      return Status::kInvalidArgument;
    }
  }
  Status status = Validate(settings.connection, secret);
  if (status != Status::kOk) return status;
  char key[16];
  Key(id, "state", key);
  status = writeField(key, &kIncomplete, 1);
  if (status != Status::kOk) return Status::kStorageFailure;
  for (size_t f = 0; f < 14; ++f) {
    uint8_t data[64];
    size_t n = Encode(f, settings, secret, data);
    Key(id, kFields[f], key);
    status = n != 0 ? writeField(key, data, n) : eraseField(key);
    if (status != Status::kOk) return Status::kIncomplete;
  }
  Key(id, "state", key);
  if (writeField(key, &kReady, 1) == Status::kOk) return Status::kOk;
  // A failed final commit can still have reached storage. Verify every field.
  status = readStatus(id);
  if (status == Status::kIncomplete) return status;
  if (status != Status::kOk) return Status::kCommitUnknown;
  ProfileSettings stored;
  Credentials stored_secret;
  status = read(id, stored, stored_secret);
  if (status != Status::kOk) return Status::kCommitUnknown;
  for (size_t f = 0; f < 14; ++f) {
    uint8_t expected[64];
    uint8_t actual[64];
    size_t a = Encode(f, settings, secret, expected);
    size_t b = Encode(f, stored, stored_secret, actual);
    if (a != b || memcmp(expected, actual, a) != 0)
      return Status::kCommitUnknown;
  }
  return Status::kOk;
}

Status FieldStore::removeProfile(ProfileId id) {
  if (id == 0) return Status::kInvalidArgument;
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
