#include "nvs-record-store.h"

#include <Preferences.h>
#include <stdio.h>

#include "node-registry.h"

namespace wattlink {

namespace {

// A namespace has at most 15 characters.
constexpr const char *kNamespace = "nodes";

// A key is n00 to n63.
bool slotKey(size_t slot, char key[4]) {
  if (slot >= kMaxNodes) {
    return false;
  }
  snprintf(key, 4, "n%02u", static_cast<unsigned>(slot));
  return true;
}

}  // namespace

ReadResult NvsRecordStore::read(size_t slot, uint8_t out[kRecordSize]) {
  char key[4];
  if (!slotKey(slot, key)) {
    return ReadResult::Failed;
  }
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) {
    return ReadResult::Failed;
  }
  // getBytesLength logs an error for a key that is missing, so the key is checked first.
  ReadResult result = ReadResult::Missing;
  if (preferences.isKey(key)) {
    // A record of another size is damaged and not a missing one, so it is never written over.
    const bool good = preferences.getBytesLength(key) == kRecordSize && preferences.getBytes(key, out, kRecordSize) == kRecordSize;
    result = good ? ReadResult::Ok : ReadResult::Failed;
  }
  preferences.end();
  return result;
}

bool NvsRecordStore::write(size_t slot, const uint8_t in[kRecordSize]) {
  char key[4];
  if (!slotKey(slot, key)) {
    return false;
  }
  Preferences preferences;
  if (!preferences.begin(kNamespace, false)) {
    return false;
  }
  const bool stored = preferences.putBytes(key, in, kRecordSize) == kRecordSize;
  preferences.end();
  return stored;
}

}  // namespace wattlink
