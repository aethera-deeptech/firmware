#pragma once

#include <stddef.h>
#include <stdint.h>

namespace wattlink {

constexpr size_t kSha256Size = 32;
constexpr size_t kMaxHkdfInfoSize = 96;
constexpr size_t kMaxHkdfOutputSize = 255 * kSha256Size;

// The hash is supplied by the caller. On the chip it wraps mbedTLS, and in the tests it wraps OpenSSL, so this code needs no library.
using HmacSha256 = void (*)(void *context, const uint8_t *key, size_t keyLength, const uint8_t *data, size_t dataLength,
                            uint8_t out[kSha256Size]);

// HKDF with SHA-256 as in RFC 5869. An empty salt means a salt of 32 zero bytes, as the RFC says.
// Returns false when the info or the output is longer than the limits above.
bool hkdfSha256(HmacSha256 hmac, void *context, const uint8_t *salt, size_t saltLength, const uint8_t *ikm, size_t ikmLength,
                const uint8_t *info, size_t infoLength, uint8_t *okm, size_t okmLength);

}  // namespace wattlink
