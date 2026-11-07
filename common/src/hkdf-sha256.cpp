#include "hkdf-sha256.h"

#include <string.h>

namespace wattlink {

bool hkdfSha256(HmacSha256 hmac, void *context, const uint8_t *salt, size_t saltLength, const uint8_t *ikm, size_t ikmLength,
                const uint8_t *info, size_t infoLength, uint8_t *okm, size_t okmLength) {
  if (infoLength > kMaxHkdfInfoSize || okmLength > kMaxHkdfOutputSize) {
    return false;
  }

  const uint8_t zeroSalt[kSha256Size] = {0};
  if (saltLength == 0) {
    salt = zeroSalt;
    saltLength = kSha256Size;
  }

  uint8_t prk[kSha256Size];
  if (!hmac(context, salt, saltLength, ikm, ikmLength, prk)) {
    return false;
  }

  uint8_t block[kSha256Size + kMaxHkdfInfoSize + 1];
  uint8_t previous[kSha256Size];
  size_t previousLength = 0;
  size_t produced = 0;

  for (uint8_t counter = 1; produced < okmLength; counter++) {
    size_t length = 0;
    if (previousLength > 0) {
      memcpy(block, previous, previousLength);
      length = previousLength;
    }
    if (infoLength > 0) {
      memcpy(block + length, info, infoLength);
      length += infoLength;
    }
    block[length++] = counter;

    if (!hmac(context, prk, kSha256Size, block, length, previous)) {
      return false;
    }
    previousLength = kSha256Size;

    const size_t take = okmLength - produced < kSha256Size ? okmLength - produced : kSha256Size;
    memcpy(okm + produced, previous, take);
    produced += take;
  }
  return true;
}

}  // namespace wattlink
