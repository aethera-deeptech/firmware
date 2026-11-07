#include "setup-code.h"

namespace wattlink {

const char kSetupCodeAlphabet[kSetupCodeAlphabetSize + 1] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";

namespace {

// The largest multiple of 31 that is not above 2^32. A number below it gives every symbol the same chance.
constexpr uint32_t kDrawLimit = 4294967292u;

static_assert(kDrawLimit % kSetupCodeAlphabetSize == 0, "the draw limit must be a multiple of the alphabet size");
static_assert(kDrawLimit + kSetupCodeAlphabetSize > 4294967295u, "the draw limit must be the largest such multiple");

}  // namespace

bool makeSetupCode(RandomU32 random, void *context, char out[kSetupCodeSize]) {
  for (size_t i = 0; i < kSetupCodeLength; i++) {
    uint32_t value = random(context);
    unsigned rejected = 0;
    while (value >= kDrawLimit) {
      if (++rejected >= kMaxRejectedDraws) {
        out[0] = '\0';
        return false;
      }
      value = random(context);
    }
    out[i] = kSetupCodeAlphabet[value % kSetupCodeAlphabetSize];
  }
  out[kSetupCodeLength] = '\0';
  return true;
}

void formatSetupCode(const char *code, char out[kSetupCodeShownSize]) {
  size_t length = 0;
  if (code != nullptr) {
    for (size_t i = 0; i < kSetupCodeLength && code[i] != '\0'; i++) {
      if (i > 0 && i % 4 == 0) {
        out[length++] = '-';
      }
      out[length++] = code[i];
    }
  }
  out[length] = '\0';
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
