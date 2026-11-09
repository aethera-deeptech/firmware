#pragma once

#include <stdint.h>

#include "pairing-key.h"

namespace wattlink {

// X25519 (RFC 7748) on top of the mbedTLS of the chip, for the key exchange of the pairing. The functions match X25519Generate and
// X25519Agree in gateway/lib/hub-pairing/hub-pairing.h. They keep no state, they ignore the context pointer, and nothing in them prints a key.
// Keys are 32 bytes in the little endian form of the RFC.

// The public key for a private key, which is the multiplication of the base point. The private key is clamped as the RFC says before it is used,
// so the vectors of the RFC work as they are. Returns false when mbedTLS fails, and then the output is zero.
bool mbedtlsX25519PublicKey(const uint8_t privateKey[kPublicKeySize], uint8_t publicKey[kPublicKeySize]);

// Makes a new clamped private key from the random number generator of the chip and its public key. The private key is wiped on a failure.
// The chip makes true random numbers only while the radio is on, and a pairing runs only then.
bool mbedtlsX25519Generate(void *context, uint8_t privateKey[kPublicKeySize], uint8_t publicKey[kPublicKeySize]);

// The shared secret of our private key and the public key of the peer. The top bit of the peer key is ignored and a value of the prime or above is
// reduced, as RFC 7748 says. Returns false for a null peer key and when mbedTLS refuses the peer key (the points of small order such as 0, 1 and the
// prime minus 1, or a point that gives no result), and then the output is zero. derivePairingKey also refuses an all zero secret.
bool mbedtlsX25519Agree(void *context, const uint8_t privateKey[kPublicKeySize], const uint8_t peerPublicKey[kPublicKeySize],
                        uint8_t shared[kSharedSecretSize]);

}  // namespace wattlink
