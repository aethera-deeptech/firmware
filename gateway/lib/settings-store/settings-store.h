#pragma once

#include <stddef.h>
#include <stdint.h>

#include "key-value-store.h"
#include "setup-code.h"
#include "wifi-form.h"

namespace wattlink {

// The keys in the store. They have at most 15 characters.
namespace key {
// The name and the password of the Wi-Fi are one value, the name, a line feed and the password. One write cannot leave the name of a new
// network next to the password of an old one. The name has no control characters, so the first line feed is always the separator.
constexpr const char *kWifi = "wifi";
constexpr const char *kApiKey = "api_key";
// The value is "1" when the owner claimed the hub with the current key, and the key is missing otherwise.
constexpr const char *kEnrolled = "enrolled";
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
constexpr size_t kWifiValueSize = kMaxSsidLength + 1 + kMaxPasswordLength + 1;

static_assert(kRandomKeyBytes % 4 == 0, "the key is made from whole 32 bit words");
static_assert((kRandomKeyBytes * 8 + 5) / 6 == kGeneratedApiKeyLength, "32 bytes give 43 base64url characters");
static_assert(kGeneratedApiKeyLength >= kApiKeyMinLength && kGeneratedApiKeyLength <= kApiKeyMaxLength, "the made key must pass the shape check");

// base64url as in RFC 4648, with no padding. Writes the text and a terminating zero, and returns the number of characters.
// Returns 0 and writes an empty text when out is too small.
size_t base64UrlEncode(const uint8_t *data, size_t length, char *out, size_t outSize);

// The settings of one hub. The secrets are the Wi-Fi password and the key. They stay in plain flash for now, and no function of this
// class prints them. Use describe() for a log line.
class SettingsStore {
 public:
  explicit SettingsStore(KeyValueStore &store) : store_(store) {}

  // Checks the name and the password with checkWifiForm, and stores them in one write. A failed write leaves the older setting as it was.
  bool saveWifi(const char *ssid, const char *password);

  // Returns false when no name is stored, or when what is stored does not pass the same checks. That guards against a damaged flash.
  bool loadWifi(WifiSettings *out);

  // Removes the setting. Returns true when it is gone. The flash may keep the old bytes until its page is erased.
  bool clearWifi();

  // The key is valid when it has 32 to 128 characters of the base64url alphabet.
  bool loadApiKey(char out[kApiKeySize]);

  // Loads the key, or makes a new one from kRandomKeyBytes random bytes and stores it. Sets *created when a new key was made.
  // Returns false when the random source is stuck (all 8 numbers equal) or the flash refused the key.
  // The key is made only once. A hub that loses its flash makes a new one and enrolls again, as the firmware guide of the backend says.
  bool ensureApiKey(RandomU32 random, void *context, char out[kApiKeySize], bool *created);

  // Makes a new key and stores it over the old one. A write that fails leaves the old key as it was. Returns false when the random source is stuck or the
  // flash refused the key.
  bool replaceApiKey(RandomU32 random, void *context);

  // Removes the key, for a factory reset.
  bool clearApiKey();

  // The flag that says that the owner claimed the hub with the current key. It is false when it is missing or damaged.
  bool loadEnrolled();
  // Returns true when the flag holds the value afterwards.
  bool saveEnrolled(bool enrolled);

  // Writes a line for the log that says what is stored and never shows a password or a key. The name of the network is not a secret,
  // because the router broadcasts it. Returns the number of characters, cut to fit size.
  size_t describe(char *out, size_t size);

 private:
  // Makes the text of a new key, without storing it. Returns false when the random source is stuck.
  static bool makeApiKey(RandomU32 random, void *context, char out[kApiKeySize]);

  KeyValueStore &store_;
};

}  // namespace wattlink
