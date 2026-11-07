#pragma once

#include <stddef.h>

namespace wattlink {

constexpr size_t kMaxSsidLength = 32;
constexpr size_t kMinPasswordLength = 8;
constexpr size_t kMaxPasswordLength = 63;

enum class FormResult {
  Ok,
  SsidEmpty,
  SsidTooLong,
  PasswordTooShort,
  PasswordTooLong,
  BadCharacters,
};

// Checks what the owner typed before the hub tries to join the network.
// The name is 1 to 32 bytes and has no control characters. Letters outside ASCII are allowed in the name, because a shop may use them.
// The password is empty for an open network, or 8 to 63 printable ASCII characters as WPA2 allows. A null text counts as empty.
// The length is counted with a bound, so a text that has no end cannot make the check read on without limit.
FormResult checkWifiForm(const char *ssid, const char *password);

}  // namespace wattlink
