#include "hkdf-sha256.h"

#include <string.h>

namespace wattlink {

namespace {

// A plain memset on a buffer that is not read again can be removed by the optimizer, a write through a volatile pointer cannot.
void wipe(void *data, size_t length) {
  volatile uint8_t *bytes = static_cast<volatile uint8_t *>(data);
  while (length-- > 0) {
    *bytes++ = 0;
  }
}

}  // namespace

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

  // The pseudo random key and the blocks are as secret as the output, so they are wiped before the function returns.
  uint8_t prk[kSha256Size];
  if (!hmac(context, salt, saltLength, ikm, ikmLength, prk)) {
    wipe(prk, sizeof(prk));
    return false;
  }

  uint8_t block[kSha256Size + kMaxHkdfInfoSize + 1];
  uint8_t previous[kSha256Size];
  size_t previousLength = 0;
  size_t produced = 0;
  bool ok = true;

  for (uint8_t counter = 1; ok && produced < okmLength; counter++) {
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
      ok = false;
      break;
    }
    previousLength = kSha256Size;

    const size_t take = okmLength - produced < kSha256Size ? okmLength - produced : kSha256Size;
    memcpy(okm + produced, previous, take);
    produced += take;
  }

  wipe(prk, sizeof(prk));
  wipe(block, sizeof(block));
  wipe(previous, sizeof(previous));
  return ok;
}

}  // namespace wattlink
