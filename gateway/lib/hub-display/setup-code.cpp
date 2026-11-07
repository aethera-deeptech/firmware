#include "setup-code.h"

namespace wattlink {

namespace {

// The largest multiple of 10 that is not above 2^32. A number below it gives every digit the same chance.
constexpr uint32_t kDrawLimit = 4294967290u;

}  // namespace

void makeSetupCode(RandomU32 random, void *context, char out[kSetupCodeDigits + 1]) {
  for (size_t i = 0; i < kSetupCodeDigits; i++) {
    uint32_t value = random(context);
    while (value >= kDrawLimit) {
      value = random(context);
    }
    out[i] = static_cast<char>('0' + value % 10);
  }
  out[kSetupCodeDigits] = '\0';
}

void makeApName(const uint8_t mac[6], char out[kApNameSize]) {
  static const char kHex[] = "0123456789ABCDEF";
  const char prefix[] = "WATTLINK-";
  size_t length = 0;
  for (; prefix[length] != '\0'; length++) {
    out[length] = prefix[length];
  }
  out[length++] = kHex[mac[4] >> 4];
  out[length++] = kHex[mac[4] & 0x0F];
  out[length++] = kHex[mac[5] >> 4];
  out[length++] = kHex[mac[5] & 0x0F];
  out[length] = '\0';
}

}  // namespace wattlink
