#pragma once

#include "backend-client.h"
#include "settings-store.h"

namespace wattlink {

// The key and the enrolled flag of the client, kept in the settings store (the flash). The random source is the one of the chip, and it is given by the caller.
class FlashClientStore : public ClientStore {
 public:
  FlashClientStore(SettingsStore &settings, RandomU32 random, void *randomContext) : settings_(settings), random_(random), randomContext_(randomContext) {}

  bool loadKey(char out[kApiKeySize]) override { return settings_.loadApiKey(out); }

  bool ensureKey(char out[kApiKeySize]) override {
    bool created = false;
    return settings_.ensureApiKey(random_, randomContext_, out, &created);
  }

  // The new key replaces the old one in one write, so a failed write leaves the old key.
  bool renewKey() override { return settings_.replaceApiKey(random_, randomContext_); }

  bool enrolled() override { return settings_.loadEnrolled(); }
  bool setEnrolled(bool value) override { return settings_.saveEnrolled(value); }

 private:
  SettingsStore &settings_;
  RandomU32 random_;
  void *randomContext_;
};

}  // namespace wattlink
