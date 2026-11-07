#pragma once

#include "key-value-store.h"

namespace wattlink {

// The flash storage (NVS) of the chip, behind the interface that the settings use. Each call opens the namespace, does one thing, and
// closes it. The settings change rarely, so this costs nothing that matters, and it keeps a failed call from leaving the namespace open.
// Nothing in this class prints a value.
class NvsKeyValueStore : public KeyValueStore {
 public:
  bool get(const char *key, char *out, size_t size) override;
  bool put(const char *key, const char *value) override;
  bool remove(const char *key) override;
};

}  // namespace wattlink
