#include "nvs-command-blob-store.h"

#include <nvs.h>
#include <stdio.h>

namespace wattlink {

namespace {

// A namespace has at most 15 characters.
constexpr const char *kNamespace = "cmds";

// A key is c00 to c79.
bool slotKey(size_t slot, char key[4]) {
  if (slot >= kCommandLogSlots) {
    return false;
  }
  snprintf(key, 4, "c%02u", static_cast<unsigned>(slot));
  return true;
}

}  // namespace

// The raw NVS calls are used here and not Preferences, for the reason that nvs-record-store.cpp gives. A flash error must not look like an empty slot.
ReadResult NvsCommandBlobStore::read(size_t slot, uint8_t out[kCommandBlobSize]) {
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
  } else if (found == ESP_OK && length == kCommandBlobSize) {
    result = nvs_get_blob(handle, key, out, &length) == ESP_OK && length == kCommandBlobSize ? ReadResult::Ok : ReadResult::Failed;
  }
  nvs_close(handle);
  return result;
}

bool NvsCommandBlobStore::write(size_t slot, const uint8_t in[kCommandBlobSize]) {
  char key[4];
  if (!slotKey(slot, key)) {
    return false;
  }
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  esp_err_t err = nvs_set_blob(handle, key, in, kCommandBlobSize);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  return err == ESP_OK;
}

}  // namespace wattlink
