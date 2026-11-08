#include "nvs-record-store.h"

#include <nvs.h>
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

// The raw NVS calls are used here and not Preferences, because Preferences::isKey turns every read error into "no such key". A flash error
// would then look like an empty slot, and the registry could write over a record that holds a key or an id.
ReadResult NvsRecordStore::read(size_t slot, uint8_t out[kRecordSize]) {
  char key[4];
  if (!slotKey(slot, key)) {
    return ReadResult::Failed;
  }
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return ReadResult::Failed;
  }
  size_t length = 0;
  const esp_err_t found = nvs_get_blob(handle, key, nullptr, &length);
  ReadResult result = ReadResult::Failed;
  if (found == ESP_ERR_NVS_NOT_FOUND) {
    result = ReadResult::Missing;
  } else if (found == ESP_OK && length == kRecordSize) {
    // A record of another size is damaged and not a missing one, so it is never written over.
    result = nvs_get_blob(handle, key, out, &length) == ESP_OK && length == kRecordSize ? ReadResult::Ok : ReadResult::Failed;
  }
  nvs_close(handle);
  return result;
}

bool NvsRecordStore::write(size_t slot, const uint8_t in[kRecordSize]) {
  char key[4];
  if (!slotKey(slot, key)) {
    return false;
  }
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  esp_err_t err = nvs_set_blob(handle, key, in, kRecordSize);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  return err == ESP_OK;
}

}  // namespace wattlink
