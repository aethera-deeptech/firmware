#include "nvs-counter-store.h"

#include <nvs.h>

namespace wattlink {

namespace {

// A namespace and a key have at most 15 characters.
constexpr const char *kNamespace = "hub";
constexpr const char *kKey = "boot";

}  // namespace

// The raw NVS calls are used here and not Preferences, because Preferences turns a read error into "no such key". A flash error would then look
// like a first start, and the counter would repeat numbers.
CounterRead NvsCounterStore::read(uint32_t *value) {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return CounterRead::Failed;
  }
  uint32_t stored = 0;
  const esp_err_t found = nvs_get_u32(handle, kKey, &stored);
  nvs_close(handle);
  if (found == ESP_ERR_NVS_NOT_FOUND) {
    return CounterRead::Missing;
  }
  if (found != ESP_OK) {
    return CounterRead::Failed;
  }
  *value = stored;
  return CounterRead::Ok;
}

bool NvsCounterStore::write(uint32_t value) {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return false;
  }
  esp_err_t err = nvs_set_u32(handle, kKey, value);
  if (err == ESP_OK) {
    err = nvs_commit(handle);
  }
  nvs_close(handle);
  return err == ESP_OK;
}

}  // namespace wattlink
