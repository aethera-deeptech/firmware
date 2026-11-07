#pragma once

#include <stddef.h>

namespace wattlink {

// The storage that the settings use. On the chip it is the flash (NVS), and in the tests it is a fake in memory.
// A key has at most 15 characters, which is the limit of NVS. A value is a non-empty text, because the flash library reports an empty
// text as a failure. A setting that is empty is removed instead.
class KeyValueStore {
 public:
  virtual ~KeyValueStore() = default;

  // Copies the text of the key, with its terminating zero, into out. Returns false when the key is missing or the text does not fit.
  // Then out holds an empty text.
  virtual bool get(const char *key, char *out, size_t size) = 0;

  // Stores a non-empty text. Returns false when it could not be stored.
  virtual bool put(const char *key, const char *value) = 0;

  // Returns true when the key is gone afterwards, also when it was missing before.
  virtual bool remove(const char *key) = 0;
};

}  // namespace wattlink
