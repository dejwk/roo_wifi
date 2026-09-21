#pragma once
#include "roo_prefs.h"
#include "roo_wifi/hal/field_store.h"

namespace roo_wifi {

/// Stores known-key Wi-Fi profiles through the roo_prefs backend.
class PrefsStore : public FieldStore {
 public:
  /// Creates a profile store backed by roo_prefs' platform-default store.
  PrefsStore();

  /// Creates a profile store backed by a caller-owned roo_prefs store.
  /// @param store Backend that must outlive this adapter.
  explicit PrefsStore(roo_prefs::Store& store);

  /// Opens the roo_prefs collection for subsequent operations.
  Status begin() override;

  /// Reads persisted radio enablement from roo_prefs.
  /// @param enabled Receives the stored value on success.
  Status readEnabled(bool &enabled) const override;

  /// Persists radio enablement through roo_prefs.
  /// @param enabled Value to persist.
  Status writeEnabled(bool enabled) override;

 protected:
  Status readField(const char *, uint8_t *, size_t &) const override;
  Status writeField(const char *, const uint8_t *, size_t) override;
  Status eraseField(const char *) override;
  Status enumerateFields(FieldVisitor visitor, void *context) const override;

 private:
  mutable roo_prefs::Collection collection_;
};

}  // namespace roo_wifi
