#include "roo_wifi/hal/prefs/prefs_store.h"

namespace roo_wifi {
namespace {

/// Maps a roo_prefs read outcome to its portable equivalent.
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

/// Maps a roo_prefs enumeration outcome to its portable equivalent.
Status Enumerate(roo_prefs::EnumerateResult result) {
  switch (result) {
    case roo_prefs::EnumerateResult::kOk:
      return Status::kOk;
    case roo_prefs::EnumerateResult::kStopped:
      return Status::kStopped;
    case roo_prefs::EnumerateResult::kUnsupported:
      return Status::kUnsupported;
    default:
      return Status::kStorageFailure;
  }
}

}  // namespace

PrefsStore::PrefsStore() : collection_("roo/wifi") {}

PrefsStore::PrefsStore(roo_prefs::Store& store)
    : collection_("roo/wifi", store) {}

Status PrefsStore::begin() {
  roo_prefs::Transaction transaction(collection_);
  return transaction.active() ? Status::kOk : Status::kStorageFailure;
}

Status PrefsStore::readEnabled(bool &out) const {
  roo_prefs::Transaction t(collection_,
                           roo_prefs::Transaction::Mode::kReadOnly);
  if (!t.active()) return Status::kStorageFailure;
  bool value;
  Status status = Read(t.store().readBool("enabled", value));
  if (status == Status::kOk) out = value;
  return status;
}

Status PrefsStore::writeEnabled(bool enabled) {
  roo_prefs::Transaction t(collection_);
  if (!t.active()) return Status::kStorageFailure;
  return t.store().writeBool("enabled", enabled) == roo_prefs::WriteResult::kOk
             ? Status::kOk
             : Status::kStorageFailure;
}

Status PrefsStore::readField(const char *key, uint8_t *out,
                             size_t &size) const {
  roo_prefs::Transaction t(collection_,
                           roo_prefs::Transaction::Mode::kReadOnly);
  if (!t.active()) return Status::kStorageFailure;
  size_t length = 0;
  Status status = Read(t.store().readBytes(key, out, size, &length));
  if (status == Status::kOk) size = length;
  return status;
}

Status PrefsStore::writeField(const char *key, const uint8_t *data,
                              size_t size) {
  roo_prefs::Transaction t(collection_);
  if (!t.active()) return Status::kStorageFailure;
  return t.store().writeBytes(key, data, size) == roo_prefs::WriteResult::kOk
             ? Status::kOk
             : Status::kStorageFailure;
}

Status PrefsStore::eraseField(const char *key) {
  roo_prefs::Transaction t(collection_);
  if (!t.active()) return Status::kStorageFailure;
  if (!t.store().isKey(key)) return Status::kOk;
  return t.store().clear(key) == roo_prefs::ClearResult::kOk
             ? Status::kOk
             : Status::kStorageFailure;
}

Status PrefsStore::enumerateFields(FieldVisitor visitor, void *context) const {
  if (visitor == nullptr) return Status::kInvalidArgument;
  return Enumerate(
      collection_.forEachKey([visitor, context](roo::string_view key) {
        return visitor(context, key.data(), key.size());
      }));
}

}  // namespace roo_wifi
