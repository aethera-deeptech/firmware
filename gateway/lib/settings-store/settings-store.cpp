#include "settings-store.h"

#include <stdio.h>
#include <string.h>

namespace wattlink {

namespace {

const char kBase64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// A plain memset on a buffer that is not read again can be removed by the optimizer, a write through a volatile pointer cannot.
void wipe(void *data, size_t length) {
  volatile uint8_t *bytes = static_cast<volatile uint8_t *>(data);
  while (length-- > 0) {
    *bytes++ = 0;
  }
}

bool isBase64UrlChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

bool validApiKey(const char *text) {
  const size_t length = strnlen(text, kApiKeyMaxLength + 1);
  if (length < kApiKeyMinLength || length > kApiKeyMaxLength) {
    return false;
  }
  for (size_t i = 0; i < length; i++) {
    if (!isBase64UrlChar(text[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace

size_t base64UrlEncode(const uint8_t *data, size_t length, char *out, size_t outSize) {
  const size_t chars = (length * 8 + 5) / 6;
  if (outSize < chars + 1) {
    if (outSize > 0) {
      out[0] = '\0';
    }
    return 0;
  }
  size_t o = 0;
  size_t i = 0;
  for (; i + 2 < length; i += 3) {
    const uint32_t group = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
    out[o++] = kBase64Url[(group >> 18) & 0x3F];
    out[o++] = kBase64Url[(group >> 12) & 0x3F];
    out[o++] = kBase64Url[(group >> 6) & 0x3F];
    out[o++] = kBase64Url[group & 0x3F];
  }
  if (length - i == 1) {
    const uint32_t group = static_cast<uint32_t>(data[i]) << 16;
    out[o++] = kBase64Url[(group >> 18) & 0x3F];
    out[o++] = kBase64Url[(group >> 12) & 0x3F];
  } else if (length - i == 2) {
    const uint32_t group = (static_cast<uint32_t>(data[i]) << 16) | (static_cast<uint32_t>(data[i + 1]) << 8);
    out[o++] = kBase64Url[(group >> 18) & 0x3F];
    out[o++] = kBase64Url[(group >> 12) & 0x3F];
    out[o++] = kBase64Url[(group >> 6) & 0x3F];
  }
  out[o] = '\0';
  return o;
}

bool SettingsStore::saveWifi(const char *ssid, const char *password) {
  if (checkWifiForm(ssid, password) != FormResult::Ok) {
    return false;
  }
  char value[kWifiValueSize];
  snprintf(value, sizeof(value), "%s\n%s", ssid, password != nullptr ? password : "");
  const bool stored = store_.put(key::kWifi, value);
  wipe(value, sizeof(value));
  return stored;
}

bool SettingsStore::loadWifi(WifiSettings *out) {
  char value[kWifiValueSize] = {0};
  if (!store_.get(key::kWifi, value, sizeof(value))) {
    wipe(value, sizeof(value));
    return false;
  }
  // The name is everything before the first line feed. A value with no line feed is damaged.
  char *separator = strchr(value, '\n');
  if (separator == nullptr) {
    wipe(value, sizeof(value));
    return false;
  }
  *separator = '\0';
  const FormResult result = checkWifiForm(value, separator + 1);
  if (result == FormResult::Ok) {
    memset(out, 0, sizeof(*out));
    strncpy(out->ssid, value, sizeof(out->ssid) - 1);
    strncpy(out->password, separator + 1, sizeof(out->password) - 1);
  }
  wipe(value, sizeof(value));
  return result == FormResult::Ok;
}

bool SettingsStore::clearWifi() {
  return store_.remove(key::kWifi);
}

bool SettingsStore::loadApiKey(char out[kApiKeySize]) {
  char found[kApiKeySize] = {0};
  if (!store_.get(key::kApiKey, found, sizeof(found)) || !validApiKey(found)) {
    out[0] = '\0';
    wipe(found, sizeof(found));
    return false;
  }
  memcpy(out, found, sizeof(found));
  wipe(found, sizeof(found));
  return true;
}

bool SettingsStore::ensureApiKey(RandomU32 random, void *context, char out[kApiKeySize], bool *created) {
  *created = false;
  if (loadApiKey(out)) {
    return true;
  }

  uint8_t bytes[kRandomKeyBytes];
  uint32_t firstWord = 0;
  bool allSame = true;
  for (size_t i = 0; i < kRandomKeyBytes; i += 4) {
    const uint32_t word = random(context);
    if (i == 0) {
      firstWord = word;
    } else if (word != firstWord) {
      allSame = false;
    }
    bytes[i] = static_cast<uint8_t>(word);
    bytes[i + 1] = static_cast<uint8_t>(word >> 8);
    bytes[i + 2] = static_cast<uint8_t>(word >> 16);
    bytes[i + 3] = static_cast<uint8_t>(word >> 24);
  }

  // Eight equal numbers in a row mean that the source is stuck, and a key made from them would be easy to guess.
  char newKey[kApiKeySize] = {0};
  const bool encoded = !allSame && base64UrlEncode(bytes, sizeof(bytes), newKey, sizeof(newKey)) == kGeneratedApiKeyLength;
  wipe(bytes, sizeof(bytes));
  if (!encoded || !store_.put(key::kApiKey, newKey)) {
    wipe(newKey, sizeof(newKey));
    out[0] = '\0';
    return false;
  }
  memcpy(out, newKey, sizeof(newKey));
  wipe(newKey, sizeof(newKey));
  *created = true;
  return true;
}

bool SettingsStore::clearApiKey() {
  return store_.remove(key::kApiKey);
}

size_t SettingsStore::describe(char *out, size_t size) {
  if (size == 0) {
    return 0;
  }
  WifiSettings wifi;
  const bool hasWifi = loadWifi(&wifi);
  char apiKey[kApiKeySize];
  const bool hasKey = loadApiKey(apiKey);

  int written;
  if (hasWifi) {
    written = snprintf(out, size, "wifi=%s password=%s api_key=%s", wifi.ssid, wifi.password[0] != '\0' ? "set" : "none",
                       hasKey ? "set" : "none");
  } else {
    written = snprintf(out, size, "wifi=none api_key=%s", hasKey ? "set" : "none");
  }
  wipe(&wifi, sizeof(wifi));
  wipe(apiKey, sizeof(apiKey));
  if (written < 0) {
    out[0] = '\0';
    return 0;
  }
  return strlen(out);
}

}  // namespace wattlink
