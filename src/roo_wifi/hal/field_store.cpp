#include "roo_wifi/hal/field_store.h"

#include <cstring>

namespace roo_wifi {
namespace {

constexpr uint32_t kProfileMagic = 0x52575050;  // "RWPP"
constexpr uint32_t kSecretMagic = 0x52575053;   // "RWPS"
constexpr uint8_t kFormatVersion = 2;
constexpr size_t kMaxProfileSize = 61;
constexpr size_t kMaxSecretSize = 104;
constexpr char kBase64UrlAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/// Writes bounded fields in the portable storage format.
class Writer {
 public:
  /// Creates a writer borrowing a buffer of @p capacity bytes.
  Writer(uint8_t *begin, size_t capacity)
      : begin_(begin), current_(begin), end_(begin + capacity) {}

  /// Appends one byte, recording an overflow if the buffer is full.
  void u8(uint8_t value) {
    if (current_ == end_) {
      ok_ = false;
      return;
    }
    *current_++ = value;
  }

  /// Appends a 32-bit integer in network byte order.
  void be32(uint32_t value) {
    u8(value >> 24);
    u8(value >> 16);
    u8(value >> 8);
    u8(value);
  }

  /// Appends bytes only when the complete range fits.
  void bytes(const uint8_t *data, size_t size) {
    if (size > static_cast<size_t>(end_ - current_)) {
      ok_ = false;
      return;
    }
    memcpy(current_, data, size);
    current_ += size;
  }

  /// Reports whether every write fitted in the buffer.
  bool ok() const { return ok_; }

  /// Returns the number of bytes written.
  size_t size() const { return current_ - begin_; }

 private:
  uint8_t *begin_;
  uint8_t *current_;
  uint8_t *end_;
  bool ok_ = true;
};

/// Reads bounded fields and records any truncated input.
class Reader {
 public:
  /// Creates a reader borrowing @p size encoded bytes.
  Reader(const uint8_t *begin, size_t size)
      : current_(begin), end_(begin + size) {}

  /// Reads one byte, recording truncation if none remains.
  uint8_t u8() {
    if (current_ == end_) {
      ok_ = false;
      return 0;
    }
    return *current_++;
  }

  /// Reads a 32-bit integer in network byte order.
  uint32_t be32() {
    uint32_t value = static_cast<uint32_t>(u8()) << 24;
    value |= static_cast<uint32_t>(u8()) << 16;
    value |= static_cast<uint32_t>(u8()) << 8;
    value |= u8();
    return value;
  }

  /// Copies bytes only when the complete encoded range is available.
  void bytes(uint8_t *out, size_t size) {
    if (size > static_cast<size_t>(end_ - current_)) {
      ok_ = false;
      return;
    }
    memcpy(out, current_, size);
    current_ += size;
  }

  /// Reports successful decoding with no trailing bytes.
  bool complete() const { return ok_ && current_ == end_; }

 private:
  const uint8_t *current_;
  const uint8_t *end_;
  bool ok_ = true;
};

/// Hashes the exact SSID bytes with stable 64-bit FNV-1a, then encodes
/// the hash in network byte order using unpadded Base64url (13-byte key).
void Key(char kind, const Ssid &ssid, char (&out)[14]) {
  uint64_t hash = UINT64_C(14695981039346656037);
  for (size_t i = 0; i < ssid.size; ++i) {
    hash ^= ssid.bytes[i];
    hash *= UINT64_C(1099511628211);
  }
  out[0] = kind;
  out[1] = '-';
  for (size_t i = 0; i < 10; ++i) {
    out[2 + i] = kBase64UrlAlphabet[(hash >> (58 - 6 * i)) & 63];
  }
  out[12] = kBase64UrlAlphabet[(hash & 15) << 2];
  out[13] = '\0';
}

/// Recognizes only canonical keys in the SSID-based storage layout.
bool ProfileKey(const char *key, size_t size) {
  if (size != 13 || key[0] != 'p' || key[1] != '-') return false;
  for (size_t i = 2; i < size; ++i) {
    const char *digit =
        key[i] == '\0' ? nullptr : strchr(kBase64UrlAlphabet, key[i]);
    if (digit == nullptr ||
        (i == 12 && (digit - kBase64UrlAlphabet) % 4 != 0)) {
      return false;
    }
  }
  return true;
}

/// Encodes length-delimited settings into their bounded persistent
/// representation.
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

/// Decodes settings and rejects malformed lengths, versions, and flags.
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

/// Encodes credentials together with the SSID that owns them.
size_t EncodeSecret(const Ssid &ssid, const Credentials &secret,
                    uint8_t (&data)[kMaxSecretSize]) {
  Writer out(data, sizeof(data));
  out.be32(kSecretMagic);
  out.u8(kFormatVersion);
  out.u8(ssid.size);
  out.bytes(ssid.bytes, ssid.size);
  out.u8(static_cast<uint8_t>(secret.encoding));
  out.u8(secret.size);
  out.bytes(secret.bytes, secret.size);
  return out.ok() ? out.size() : 0;
}

/// Decodes a credential record without changing outputs on malformed input.
Status DecodeSecret(const uint8_t *data, size_t size, Ssid &ssid,
                    Credentials &secret) {
  Reader in(data, size);
  if (in.be32() != kSecretMagic || in.u8() != kFormatVersion) {
    return Status::kCorrupt;
  }
  Ssid decoded_ssid;
  decoded_ssid.size = in.u8();
  if (decoded_ssid.size == 0 ||
      decoded_ssid.size > sizeof(decoded_ssid.bytes)) {
    return Status::kCorrupt;
  }
  in.bytes(decoded_ssid.bytes, decoded_ssid.size);
  Credentials decoded;
  decoded.encoding = static_cast<CredentialEncoding>(in.u8());
  uint8_t secret_size = in.u8();
  if (secret_size > sizeof(decoded.bytes)) return Status::kCorrupt;
  in.bytes(decoded.bytes, secret_size);
  if (!in.complete()) return Status::kCorrupt;
  decoded.size = secret_size;
  ssid = decoded_ssid;
  secret = decoded;
  return Status::kOk;
}

}  // namespace

Status FieldStore::read(const Ssid &ssid, ProfileSettings &settings,
                        Credentials &secret) const {
  if (ssid.size == 0 || ssid.size > sizeof(ssid.bytes)) {
    return Status::kInvalidArgument;
  }
  char key[14];
  Key('p', ssid, key);
  uint8_t profile_data[kMaxProfileSize];
  size_t profile_size = sizeof(profile_data);
  Status status = readField(key, profile_data, profile_size);
  if (status != Status::kOk) return status;

  ProfileSettings decoded_settings;
  status = DecodeProfile(profile_data, profile_size, decoded_settings);
  if (status != Status::kOk) return status;

  if (decoded_settings.connection.ssid != ssid) return Status::kHashCollision;

  Credentials decoded_secret;
  if (decoded_settings.connection.security != AuthMode::kOpen) {
    Key('s', ssid, key);
    uint8_t secret_data[kMaxSecretSize];
    size_t secret_size = sizeof(secret_data);
    status = readField(key, secret_data, secret_size);
    if (status != Status::kOk) {
      return status == Status::kNotFound ? Status::kCorrupt : status;
    }
    Ssid secret_ssid;
    status =
        DecodeSecret(secret_data, secret_size, secret_ssid, decoded_secret);
    if (status == Status::kOk && secret_ssid != ssid) {
      return Status::kHashCollision;
    }
    if (status != Status::kOk) return status;
  }
  if (Validate(decoded_settings.connection, decoded_secret) != Status::kOk) {
    return Status::kCorrupt;
  }
  settings = decoded_settings;
  secret = decoded_secret;
  return Status::kOk;
}

Status FieldStore::loadProfile(const Ssid &ssid, Profile &out) const {
  Profile result;
  Credentials secret;
  Status status = read(ssid, result.settings, secret);
  if (status != Status::kOk) return status;
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
    Status status = Status::kOk;
  } state = {this, visitor, context};
  Status result = enumerateFields(
      [](void *opaque, const char *key, size_t size) {
        Context &state = *static_cast<Context *>(opaque);
        state.status = state.store->visitProfile(key, size, state.visitor,
                                                 state.visitor_context);
        return state.status == Status::kOk;
      },
      &state);
  return state.status == Status::kOk ? result : state.status;
}

Status FieldStore::visitProfile(const char *key, size_t size,
                                ProfileVisitor visitor, void *context) const {
  if (!ProfileKey(key, size)) return Status::kOk;
  char terminated[14];
  memcpy(terminated, key, size);
  terminated[size] = '\0';
  uint8_t data[kMaxProfileSize];
  size_t length = sizeof(data);
  Status status = readField(terminated, data, length);
  if (status != Status::kOk) return status;
  ProfileSettings settings;
  status = DecodeProfile(data, length, settings);
  if (status != Status::kOk) return status;
  char expected[14];
  Key('p', settings.connection.ssid, expected);
  if (memcmp(expected, key, size) != 0) return Status::kCorrupt;
  return visitor(context, settings.connection.ssid) ? Status::kOk
                                                    : Status::kStopped;
}

/// Checks both blobs before mutation, including secrets orphaned by a failed
/// save/delete. Never trusts a hash alone to establish ownership.
Status FieldStore::checkIdentity(const Ssid &ssid) const {
  if (ssid.size == 0 || ssid.size > sizeof(ssid.bytes)) {
    return Status::kInvalidArgument;
  }
  char key[14];
  uint8_t data[kMaxSecretSize];
  size_t size = sizeof(data);
  Key('p', ssid, key);
  Status status = readField(key, data, size);
  if (status == Status::kOk) {
    ProfileSettings settings;
    status = DecodeProfile(data, size, settings);
    if (status != Status::kOk) return status;
    if (settings.connection.ssid != ssid) return Status::kHashCollision;
  } else if (status != Status::kNotFound) {
    return status;
  }
  Key('s', ssid, key);
  size = sizeof(data);
  status = readField(key, data, size);
  if (status == Status::kNotFound) return Status::kOk;
  if (status != Status::kOk) return status;
  Ssid stored;
  Credentials secret;
  status = DecodeSecret(data, size, stored, secret);
  if (status != Status::kOk) return status;
  return stored == ssid ? Status::kOk : Status::kHashCollision;
}

Status FieldStore::loadCredentials(const Ssid &ssid, Credentials &out) const {
  ProfileSettings settings;
  Credentials secret;
  Status status = read(ssid, settings, secret);
  if (status == Status::kOk) out = secret;
  return status;
}

Status FieldStore::writeVerified(const char *key, const uint8_t *expected,
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
}

Status FieldStore::saveProfile(const ProfileSettings &settings,
                               const CredentialUpdate &update) {
  const Ssid &ssid = settings.connection.ssid;
  Status identity = checkIdentity(ssid);
  if (identity != Status::kOk) return identity;
  Credentials secret;
  if (settings.connection.security == AuthMode::kOpen &&
      (update.intent != CredentialIntent::kClear ||
       update.replacement.size != 0)) {
    return Status::kInvalidArgument;
  }
  switch (update.intent) {
    case CredentialIntent::kKeep: {
      Status status = loadCredentials(ssid, secret);
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

  char key[14];
  if (update.intent == CredentialIntent::kReplace) {
    uint8_t secret_data[kMaxSecretSize];
    size_t secret_size = EncodeSecret(ssid, secret, secret_data);
    Key('s', ssid, key);
    status = writeVerified(key, secret_data, secret_size, sizeof(secret_data));
    if (status != Status::kOk) return status;
  }

  uint8_t profile_data[kMaxProfileSize];
  size_t profile_size = EncodeProfile(settings, profile_data);
  Key('p', ssid, key);
  status = writeVerified(key, profile_data, profile_size, sizeof(profile_data));
  if (status != Status::kOk) return status;

  if (update.intent == CredentialIntent::kClear) {
    Key('s', ssid, key);
    uint8_t existing[kMaxSecretSize];
    size_t existing_size = sizeof(existing);
    if (readField(key, existing, existing_size) != Status::kNotFound &&
        eraseField(key) != Status::kOk) {
      return Status::kStorageFailure;
    }
  }
  return Status::kOk;
}

Status FieldStore::removeProfile(const Ssid &ssid) {
  Status identity = checkIdentity(ssid);
  if (identity != Status::kOk) return identity;
  char key[14];
  Key('p', ssid, key);
  if (eraseField(key) != Status::kOk) return Status::kStorageFailure;
  Key('s', ssid, key);
  if (eraseField(key) != Status::kOk) return Status::kStorageFailure;
  Ssid last;
  Status status = readLastProfile(last);
  if (status == Status::kNotFound || (status == Status::kOk && last != ssid)) {
    return Status::kOk;
  }
  if (status != Status::kOk) return status;
  return writeLastProfile({});
}

Status FieldStore::readLastProfile(Ssid &out) const {
  uint8_t data[33];
  size_t size = sizeof(data);
  Status status = readField("last-ssid", data, size);
  if (status != Status::kOk) return status;
  if (size < 2 || data[0] > 32 || size != static_cast<size_t>(data[0]) + 1) {
    return Status::kCorrupt;
  }
  Ssid ssid;
  ssid.size = data[0];
  memcpy(ssid.bytes, data + 1, ssid.size);
  out = ssid;
  return Status::kOk;
}

Status FieldStore::writeLastProfile(const Ssid &ssid) {
  if (ssid.size > sizeof(ssid.bytes)) return Status::kInvalidArgument;
  if (ssid.size == 0) {
    return eraseField("last-ssid") == Status::kOk ? Status::kOk
                                                  : Status::kStorageFailure;
  }
  uint8_t expected[33];
  expected[0] = ssid.size;
  memcpy(expected + 1, ssid.bytes, ssid.size);
  const size_t expected_size = ssid.size + 1;
  uint8_t actual[33];
  size_t size = sizeof(actual);
  Status status = readField("last-ssid", actual, size);
  if (status == Status::kOk && size == expected_size &&
      memcmp(actual, expected, size) == 0) {
    return Status::kOk;
  }
  if (writeField("last-ssid", expected, expected_size) == Status::kOk) {
    return Status::kOk;
  }
  size = sizeof(actual);
  status = readField("last-ssid", actual, size);
  if (status == Status::kOk && size == expected_size &&
      memcmp(actual, expected, size) == 0) {
    return Status::kOk;
  }
  return status == Status::kNotFound || status == Status::kOk
             ? Status::kStorageFailure
             : Status::kCommitUnknown;
}

}  // namespace roo_wifi
