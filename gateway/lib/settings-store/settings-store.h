#pragma once

#include <stddef.h>
#include <stdint.h>

#include "key-value-store.h"
#include "setup-code.h"
#include "wifi-form.h"

namespace wattlink {

// The keys in the store. They have at most 15 characters.
namespace key {
constexpr const char *kWifiSsid = "wifi_ssid";
constexpr const char *kWifiPassword = "wifi_pass";
constexpr const char *kApiKey = "api_key";
}  // namespace key

struct WifiSettings {
  char ssid[kMaxSsidLength + 1];
  char password[kMaxPasswordLength + 1];
};

// The backend accepts a key of 32 to 128 characters. The hub makes a key of 43 characters, which is 32 random bytes in base64url.
constexpr size_t kApiKeyMinLength = 32;
constexpr size_t kApiKeyMaxLength = 128;
constexpr size_t kGeneratedApiKeyLength = 43;
constexpr size_t kApiKeySize = kApiKeyMaxLength + 1;
constexpr size_t kRandomKeyBytes = 32;

// base64url as in RFC 4648, with no padding. Writes the text and a terminating zero, and returns the number of characters.
// Returns 0 and writes an empty text when out is too small.
size_t base64UrlEncode(const uint8_t *data, size_t length, char *out, size_t outSize);

// The settings of one hub. The secrets are the Wi-Fi password and the key. They stay in plain flash for now, and no function of this
// class prints them. Use describe() for a log line.
class SettingsStore {
 public:
  explicit SettingsStore(KeyValueStore &store) : store_(store) {}

  // Checks the name and the password with checkWifiForm, and stores them. An empty password is not stored, because the network is open.
  // When the second write fails, the first one is removed again, so the store never holds a name with the password of an older network.
  bool saveWifi(const char *ssid, const char *password);

  // Returns false when no name is stored, or when what is stored does not pass the same checks. That guards against a damaged flash.
  bool loadWifi(WifiSettings *out);

  // Removes both. Returns true when both are gone.
  bool clearWifi();

  // The key is valid when it has 32 to 128 characters of the base64url alphabet.
  bool loadApiKey(char out[kApiKeySize]);

  // Loads the key, or makes a new one from kRandomKeyBytes random bytes and stores it. Sets *created when a new key was made.
  // Returns false when the random source failed (it gave the same number 64 times, see makeSetupCode) or the flash refused the key.
  // The key is made only once. A hub that loses its flash makes a new one and enrolls again, as the firmware guide of the backend says.
  bool ensureApiKey(RandomU32 random, void *context, char out[kApiKeySize], bool *created);

  // Removes the key, for a factory reset.
  bool clearApiKey();

  // Writes a line for the log that says what is stored and never shows a password or a key. The name of the network is not a secret,
  // because the router broadcasts it. Returns the number of characters, cut to fit size.
  size_t describe(char *out, size_t size);

 private:
  KeyValueStore &store_;
};

}  // namespace wattlink
