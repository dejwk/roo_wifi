#include "roo_wifi/hal/field_store.h"

#include <cstdio>
#include <cstring>

namespace roo_wifi {
namespace {

constexpr uint32_t kProfileMagic = 0x52575050;  // "RWPP"
constexpr uint32_t kSecretMagic = 0x52575053;   // "RWPS"
constexpr uint8_t kFormatVersion = 1;
constexpr size_t kMaxProfileSize = 61;
constexpr size_t kMaxSecretSize = 71;

class Writer {
 public:
  Writer(uint8_t *begin, size_t capacity)
      : begin_(begin), current_(begin), end_(begin + capacity) {}

  void u8(uint8_t value) {
    if (current_ == end_) {
      ok_ = false;
      return;
    }
    *current_++ = value;
  }

  void be32(uint32_t value) {
    u8(value >> 24);
    u8(value >> 16);
    u8(value >> 8);
    u8(value);
  }

  void bytes(const uint8_t *data, size_t size) {
    if (size > static_cast<size_t>(end_ - current_)) {
      ok_ = false;
      return;
    }
    memcpy(current_, data, size);
    current_ += size;
  }

  bool ok() const { return ok_; }
  size_t size() const { return current_ - begin_; }

 private:
  uint8_t *begin_;
  uint8_t *current_;
  uint8_t *end_;
  bool ok_ = true;
};

class Reader {
 public:
  Reader(const uint8_t *begin, size_t size)
      : current_(begin), end_(begin + size) {}

  uint8_t u8() {
    if (current_ == end_) {
      ok_ = false;
      return 0;
    }
    return *current_++;
  }

  uint32_t be32() {
    uint32_t value = static_cast<uint32_t>(u8()) << 24;
    value |= static_cast<uint32_t>(u8()) << 16;
    value |= static_cast<uint32_t>(u8()) << 8;
    value |= u8();
    return value;
  }

  void bytes(uint8_t *out, size_t size) {
    if (size > static_cast<size_t>(end_ - current_)) {
      ok_ = false;
      return;
    }
    memcpy(out, current_, size);
    current_ += size;
  }

  bool complete() const { return ok_ && current_ == end_; }

 private:
  const uint8_t *current_;
  const uint8_t *end_;
  bool ok_ = true;
};

/// Parses a compact profile key shaped as `p-` plus eight lowercase hex digits.
bool ProfileKey(const char *key, size_t size, ProfileId &id) {
  if (size != 10 || key[0] != 'p' || key[1] != '-') return false;
  ProfileId value = 0;
  for (size_t i = 2; i < 10; ++i) {
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

void Key(char kind, ProfileId id, char (&out)[11]) {
  snprintf(out, sizeof(out), "%c-%08lx", kind,
           static_cast<unsigned long>(id));
}

size_t EncodeProfile(const ProfileSettings &settings,
                     uint8_t (&data)[kMaxProfileSize]) {
  const ConnectionConfig &config = settings.connection;
  Writer out(data, sizeof(data));
  out.be32(kProfileMagic);
  out.u8(kFormatVersion);
  out.u8(config.ssid.size);
  out.bytes(config.ssid.bytes, config.ssid.size);
  out.u8(static_cast<uint8_t>(config.security));
  out.u8(config.hidden);
  out.u8(static_cast<uint8_t>(config.ip_mode));
  out.bytes(config.static_ipv4.address.bytes, 4);
  out.bytes(config.static_ipv4.gateway.bytes, 4);
  out.bytes(config.static_ipv4.dns1.bytes, 4);
  out.bytes(config.static_ipv4.dns2.bytes, 4);
  out.u8(config.static_ipv4.prefix_length);
  out.u8(config.static_ipv4.has_dns2);
  out.u8(static_cast<uint8_t>(config.mac_policy));
  out.u8(settings.auto_connect);
  return out.ok() ? out.size() : 0;
}

Status DecodeProfile(const uint8_t *data, size_t size,
                     ProfileSettings &settings) {
  Reader in(data, size);
  if (in.be32() != kProfileMagic || in.u8() != kFormatVersion) {
    return Status::kCorrupt;
  }
  ProfileSettings decoded;
  ConnectionConfig &config = decoded.connection;
  uint8_t ssid_size = in.u8();
  if (ssid_size == 0 || ssid_size > sizeof(config.ssid.bytes)) {
    return Status::kCorrupt;
  }
  in.bytes(config.ssid.bytes, ssid_size);
  config.ssid.size = ssid_size;
  config.security = static_cast<AuthMode>(in.u8());
  uint8_t hidden = in.u8();
  config.ip_mode = static_cast<IpMode>(in.u8());
  in.bytes(config.static_ipv4.address.bytes, 4);
  in.bytes(config.static_ipv4.gateway.bytes, 4);
  in.bytes(config.static_ipv4.dns1.bytes, 4);
  in.bytes(config.static_ipv4.dns2.bytes, 4);
  config.static_ipv4.prefix_length = in.u8();
  uint8_t has_dns2 = in.u8();
  config.mac_policy = static_cast<MacPolicy>(in.u8());
  uint8_t auto_connect = in.u8();
  if (hidden > 1 || has_dns2 > 1 || auto_connect > 1 || !in.complete()) {
    return Status::kCorrupt;
  }
  config.hidden = hidden != 0;
  config.static_ipv4.has_dns2 = has_dns2 != 0;
  decoded.auto_connect = auto_connect != 0;
  settings = decoded;
  return Status::kOk;
}

size_t EncodeSecret(const Credentials &secret,
                    uint8_t (&data)[kMaxSecretSize]) {
  Writer out(data, sizeof(data));
  out.be32(kSecretMagic);
  out.u8(kFormatVersion);
  out.u8(static_cast<uint8_t>(secret.encoding));
  out.u8(secret.size);
  out.bytes(secret.bytes, secret.size);
  return out.ok() ? out.size() : 0;
}

Status DecodeSecret(const uint8_t *data, size_t size, Credentials &secret) {
  Reader in(data, size);
  if (in.be32() != kSecretMagic || in.u8() != kFormatVersion) {
    return Status::kCorrupt;
  }
  Credentials decoded;
  decoded.encoding = static_cast<CredentialEncoding>(in.u8());
  uint8_t secret_size = in.u8();
  if (secret_size > sizeof(decoded.bytes)) return Status::kCorrupt;
  in.bytes(decoded.bytes, secret_size);
  if (!in.complete()) return Status::kCorrupt;
  decoded.size = secret_size;
  secret = decoded;
  return Status::kOk;
}

}  // namespace

Status FieldStore::read(ProfileId id, ProfileSettings &settings,
                        Credentials &secret) const {
  if (id == 0) return Status::kInvalidArgument;
  char key[11];
  Key('p', id, key);
  uint8_t profile_data[kMaxProfileSize];
  size_t profile_size = sizeof(profile_data);
  Status status = readField(key, profile_data, profile_size);
  if (status != Status::kOk) return status;

  ProfileSettings decoded_settings;
  status = DecodeProfile(profile_data, profile_size, decoded_settings);
  if (status != Status::kOk) return status;

  Credentials decoded_secret;
  if (decoded_settings.connection.security != AuthMode::kOpen) {
    Key('s', id, key);
    uint8_t secret_data[kMaxSecretSize];
    size_t secret_size = sizeof(secret_data);
    status = readField(key, secret_data, secret_size);
    if (status != Status::kOk) {
      return status == Status::kNotFound ? Status::kCorrupt : status;
    }
    status = DecodeSecret(secret_data, secret_size, decoded_secret);
    if (status != Status::kOk) return status;
  }
  if (Validate(decoded_settings.connection, decoded_secret) != Status::kOk) {
    return Status::kCorrupt;
  }
  settings = decoded_settings;
  secret = decoded_secret;
  return Status::kOk;
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
    ProfileVisitor visitor;
    void *visitor_context;
  } state = {visitor, context};
  return enumerateFields(
      [](void *opaque, const char *key, size_t size) {
        Context &state = *static_cast<Context *>(opaque);
        ProfileId id;
        return !ProfileKey(key, size, id) ||
               state.visitor(state.visitor_context, id);
      },
      &state);
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
    case CredentialIntent::kReplace:
      secret = update.replacement;
      break;
    case CredentialIntent::kClear:
      if (update.replacement.size != 0) return Status::kInvalidArgument;
      break;
    default:
      return Status::kInvalidArgument;
  }
  Status status = Validate(settings.connection, secret);
  if (status != Status::kOk) return status;

  auto write_verified = [this](const char *key, const uint8_t *expected,
                               size_t expected_size, size_t capacity) {
    uint8_t actual[kMaxSecretSize];
    size_t actual_size = capacity;
    Status read_status = readField(key, actual, actual_size);
    if (read_status == Status::kOk && actual_size == expected_size &&
        memcmp(actual, expected, expected_size) == 0) {
      return Status::kOk;
    }
    if (writeField(key, expected, expected_size) == Status::kOk) {
      return Status::kOk;
    }
    actual_size = capacity;
    read_status = readField(key, actual, actual_size);
    if (read_status == Status::kOk && actual_size == expected_size &&
        memcmp(actual, expected, expected_size) == 0) {
      return Status::kOk;
    }
    return read_status == Status::kNotFound || read_status == Status::kOk
               ? Status::kStorageFailure
               : Status::kCommitUnknown;
  };

  char key[11];
  if (update.intent == CredentialIntent::kReplace) {
    uint8_t secret_data[kMaxSecretSize];
    size_t secret_size = EncodeSecret(secret, secret_data);
    Key('s', id, key);
    status = write_verified(key, secret_data, secret_size, sizeof(secret_data));
    if (status != Status::kOk) return status;
  }

  uint8_t profile_data[kMaxProfileSize];
  size_t profile_size = EncodeProfile(settings, profile_data);
  Key('p', id, key);
  status =
      write_verified(key, profile_data, profile_size, sizeof(profile_data));
  if (status != Status::kOk) return status;

  if (update.intent == CredentialIntent::kClear) {
    Key('s', id, key);
    uint8_t existing[kMaxSecretSize];
    size_t existing_size = sizeof(existing);
    if (readField(key, existing, existing_size) != Status::kNotFound &&
        eraseField(key) != Status::kOk) {
      return Status::kStorageFailure;
    }
  }
  return Status::kOk;
}

Status FieldStore::removeProfile(ProfileId id) {
  if (id == 0) return Status::kInvalidArgument;
  char key[11];
  Key('p', id, key);
  if (eraseField(key) != Status::kOk) return Status::kStorageFailure;
  Key('s', id, key);
  if (eraseField(key) != Status::kOk) return Status::kStorageFailure;
  ProfileId last;
  Status status = readLastProfile(last);
  if (status == Status::kNotFound || (status == Status::kOk && last != id)) {
    return Status::kOk;
  }
  if (status != Status::kOk) return status;
  return writeLastProfile(0);
}

Status FieldStore::readLastProfile(ProfileId &out) const {
  uint8_t data[4];
  size_t size = sizeof(data);
  Status status = readField("last", data, size);
  if (status != Status::kOk) return status;
  if (size != sizeof(data)) return Status::kCorrupt;
  Reader in(data, size);
  ProfileId id = in.be32();
  if (!in.complete() || id == 0) return Status::kCorrupt;
  out = id;
  return Status::kOk;
}

Status FieldStore::writeLastProfile(ProfileId id) {
  if (id == 0) {
    return eraseField("last") == Status::kOk ? Status::kOk
                                              : Status::kStorageFailure;
  }
  uint8_t expected[4];
  Writer out(expected, sizeof(expected));
  out.be32(id);
  uint8_t actual[4];
  size_t size = sizeof(actual);
  Status status = readField("last", actual, size);
  if (status == Status::kOk && size == sizeof(actual) &&
      memcmp(actual, expected, sizeof(actual)) == 0) {
    return Status::kOk;
  }
  if (writeField("last", expected, sizeof(expected)) == Status::kOk) {
    return Status::kOk;
  }
  size = sizeof(actual);
  status = readField("last", actual, size);
  if (status == Status::kOk && size == sizeof(actual) &&
      memcmp(actual, expected, sizeof(actual)) == 0) {
    return Status::kOk;
  }
  return status == Status::kNotFound || status == Status::kOk
             ? Status::kStorageFailure
             : Status::kCommitUnknown;
}

}  // namespace roo_wifi
