#include "pairing-key.h"

#include <string.h>

namespace wattlink {

const uint8_t kPairingSalt[kSha256Size] = {0x7b, 0xe1, 0xd9, 0xc7, 0xdf, 0xf3, 0x93, 0xcd, 0x6f, 0xbc, 0x5a,
                                           0x76, 0xa0, 0x35, 0xbc, 0xf9, 0xe4, 0x30, 0xe8, 0x5c, 0x51, 0xdc,
                                           0x4b, 0xbf, 0x80, 0x44, 0x6a, 0x2c, 0x49, 0xb5, 0x12, 0x90};

bool derivePairingKey(HmacSha256 hmac, void *context, const uint8_t sharedSecret[kSharedSecretSize],
                      const uint8_t nodePublicKey[kPublicKeySize], const uint8_t hubPublicKey[kPublicKeySize],
                      const uint8_t nodeMac[kMacSize], const uint8_t hubMac[kMacSize], uint8_t key[kPairingKeySize]) {
  uint8_t any = 0;
  for (size_t i = 0; i < kSharedSecretSize; i++) {
    any |= sharedSecret[i];
  }
  if (any == 0) {
    return false;
  }

  uint8_t info[kPairingInfoSize];
  uint8_t *cursor = info;
  memcpy(cursor, nodePublicKey, kPublicKeySize);
  cursor += kPublicKeySize;
  memcpy(cursor, hubPublicKey, kPublicKeySize);
  cursor += kPublicKeySize;
  memcpy(cursor, nodeMac, kMacSize);
  cursor += kMacSize;
  memcpy(cursor, hubMac, kMacSize);

  return hkdfSha256(hmac, context, kPairingSalt, kSha256Size, sharedSecret, kSharedSecretSize, info, kPairingInfoSize, key,
                    kPairingKeySize);
}

}  // namespace wattlink
