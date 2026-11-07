#pragma once

#include "hkdf-sha256.h"
#include "secure-frame.h"

namespace wattlink {

// The hash and the cipher that common/ asks for, on top of the mbedTLS of the chip. Each one matches a function pointer type of common/.
// They keep no state, they ignore the context pointer, and nothing in them prints a key or a message.

bool mbedtlsHmacSha256(void *context, const uint8_t *key, size_t keyLength, const uint8_t *data, size_t dataLength,
                       uint8_t out[kSha256Size]);

bool mbedtlsAesGcmSeal(void *context, const uint8_t key[kAesKeySize], const uint8_t nonce[kNonceSize], const uint8_t *aad, size_t aadLength,
                       const uint8_t *plain, size_t length, uint8_t *cipher, uint8_t tag[kTagSize]);

bool mbedtlsAesGcmOpen(void *context, const uint8_t key[kAesKeySize], const uint8_t nonce[kNonceSize], const uint8_t *aad, size_t aadLength,
                       const uint8_t *cipher, size_t length, const uint8_t tag[kTagSize], uint8_t *plain);

}  // namespace wattlink
