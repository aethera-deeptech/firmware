#include "wifi-form.h"

#include <string.h>

namespace wattlink {

namespace {

bool isControl(unsigned char c) {
  return c < 0x20 || c == 0x7F;
}

bool isPrintableAscii(unsigned char c) {
  return c >= 0x20 && c <= 0x7E;
}

}  // namespace

FormResult checkWifiForm(const char *ssid, const char *password) {
  if (ssid == nullptr) {
    return FormResult::SsidEmpty;
  }
  const size_t ssidLength = strnlen(ssid, kMaxSsidLength + 1);
  if (ssidLength == 0) {
    return FormResult::SsidEmpty;
  }
  if (ssidLength > kMaxSsidLength) {
    return FormResult::SsidTooLong;
  }
  for (size_t i = 0; i < ssidLength; i++) {
    if (isControl(static_cast<unsigned char>(ssid[i]))) {
      return FormResult::BadCharacters;
    }
  }

  const size_t passwordLength = password == nullptr ? 0 : strnlen(password, kMaxPasswordLength + 1);
  if (passwordLength == 0) {
    return FormResult::Ok;
  }
  if (passwordLength < kMinPasswordLength) {
    return FormResult::PasswordTooShort;
  }
  if (passwordLength > kMaxPasswordLength) {
    return FormResult::PasswordTooLong;
  }
  for (size_t i = 0; i < passwordLength; i++) {
    if (!isPrintableAscii(static_cast<unsigned char>(password[i]))) {
      return FormResult::BadCharacters;
    }
  }
  return FormResult::Ok;
}

}  // namespace wattlink
