#pragma once

#include <stddef.h>
#include <stdint.h>

#include "hkdf-sha256.h"

namespace wattlink {

constexpr size_t kSharedSecretSize = 32;
constexpr size_t kPublicKeySize = 32;
constexpr size_t kMacSize = 6;
constexpr size_t kPairingKeySize = 16;
constexpr size_t kPairingInfoSize = 2 * kPublicKeySize + 2 * kMacSize;

static_assert(kPairingInfoSize <= kMaxHkdfInfoSize, "the info field of the pairing key must fit the limit of HKDF");

// SHA-256 of the text "wattlink-espnow-v1", which is the salt of the derivation. The tests check it against a real SHA-256.
extern const uint8_t kPairingSalt[kSha256Size];

// The key of AES-128-GCM for one node, as in the section Pairing and keys of docs/espnow-protocol.md. The info field is
// the public key of the node, the public key of the hub, the MAC of the node and the MAC of the hub, joined in this order.
// The node and the hub must call it with the same arguments, so both get the same key.
// Returns false for an all zero shared secret, which the protocol refuses, and when the hash fails.
bool derivePairingKey(HmacSha256 hmac, void *context, const uint8_t sharedSecret[kSharedSecretSize],
                      const uint8_t nodePublicKey[kPublicKeySize], const uint8_t hubPublicKey[kPublicKeySize],
                      const uint8_t nodeMac[kMacSize], const uint8_t hubMac[kMacSize], uint8_t key[kPairingKeySize]);

}  // namespace wattlink
