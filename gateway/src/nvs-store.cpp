#include "nvs-store.h"

#include <Preferences.h>
#include <string.h>

namespace wattlink {

namespace {

// A namespace has at most 15 characters.
constexpr const char *kNamespace = "wattlink";

}  // namespace

bool NvsKeyValueStore::get(const char *key, char *out, size_t size) {
  if (size == 0) {
    return false;
  }
  out[0] = '\0';
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) {
    return false;
  }
  // getString logs an error for a key that is missing, so the key is checked first.
  bool found = false;
  if (preferences.isKey(key)) {
    // It returns the number of bytes with the terminating zero, and 0 when the text does not fit or the read failed.
    found = preferences.getString(key, out, size) > 0;
  }
  preferences.end();
  if (!found) {
    out[0] = '\0';
  }
  return found;
}

bool NvsKeyValueStore::put(const char *key, const char *value) {
  if (value == nullptr || value[0] == '\0') {
    return false;
  }
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) {
    return false;
  }
  const bool stored = preferences.putString(key, value) == strlen(value);
  preferences.end();
  return stored;
}

bool NvsKeyValueStore::remove(const char *key) {
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) {
    return false;
  }
  const bool gone = !preferences.isKey(key) || preferences.remove(key);
  preferences.end();
  return gone;
}

}  // namespace wattlink
