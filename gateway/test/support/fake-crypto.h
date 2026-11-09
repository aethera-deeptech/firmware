#pragma once

// Fake crypto and a fake flash for the host tests of the hub. The crypto is deterministic and it depends on every input, which is all that the
// logic needs. The real HKDF, AES-GCM and key derivation are tested against known answers in common/ and on the chip at boot.

#include <string.h>

#include <stddef.h>
#include <stdint.h>

#include "frame-codec.h"
#include "hkdf-sha256.h"
#include "node-registry.h"
#include "pairing-key.h"
#include "record-store.h"
#include "secure-frame.h"

namespace wattlink {
namespace testsupport {

struct Knobs {
  bool failGenerate = false;
  bool failAgree = false;
  bool failHmac = false;
  bool zeroShared = false;
  uint8_t next = 1;
};

inline uint32_t mix(uint32_t h, const uint8_t *data, size_t length) {
  for (size_t i = 0; i < length; i++) {
    h = (h ^ data[i]) * 16777619u;
  }
  return h;
}

inline void spread(uint32_t h, uint8_t *out, size_t length) {
  for (size_t i = 0; i < length; i++) {
    h = h * 1664525u + 1013904223u;
    out[i] = static_cast<uint8_t>(h >> 24);
  }
}

inline bool fakeHmac(void *context, const uint8_t *key, size_t keyLength, const uint8_t *data, size_t dataLength, uint8_t out[kSha256Size]) {
  if (static_cast<Knobs *>(context)->failHmac) {
    return false;
  }
  spread(mix(mix(2166136261u, key, keyLength), data, dataLength), out, kSha256Size);
  return true;
}

inline void fakeTag(const uint8_t key[kAesKeySize], const uint8_t nonce[kNonceSize], const uint8_t *aad, size_t aadLength, const uint8_t *cipher,
             size_t length, uint8_t tag[kTagSize]) {
  uint32_t h = mix(2166136261u, key, kAesKeySize);
  h = mix(h, nonce, kNonceSize);
  h = mix(h, aad, aadLength);
  h = mix(h, cipher, length);
  spread(h, tag, kTagSize);
}

inline void fakeStream(const uint8_t key[kAesKeySize], const uint8_t nonce[kNonceSize], const uint8_t *in, size_t length, uint8_t *out) {
  for (size_t i = 0; i < length; i++) {
    out[i] = in[i] ^ key[i % kAesKeySize] ^ nonce[i % kNonceSize] ^ static_cast<uint8_t>(i * 7);
  }
}

inline bool fakeSeal(void *, const uint8_t key[kAesKeySize], const uint8_t nonce[kNonceSize], const uint8_t *aad, size_t aadLength,
              const uint8_t *plain, size_t length, uint8_t *cipher, uint8_t tag[kTagSize]) {
  fakeStream(key, nonce, plain, length, cipher);
  fakeTag(key, nonce, aad, aadLength, cipher, length, tag);
  return true;
}

inline bool fakeOpen(void *, const uint8_t key[kAesKeySize], const uint8_t nonce[kNonceSize], const uint8_t *aad, size_t aadLength,
              const uint8_t *cipher, size_t length, const uint8_t tag[kTagSize], uint8_t *plain) {
  uint8_t expected[kTagSize];
  fakeTag(key, nonce, aad, aadLength, cipher, length, expected);
  if (memcmp(expected, tag, kTagSize) != 0) {
    return false;
  }
  fakeStream(key, nonce, cipher, length, plain);
  return true;
}

// A toy exchange. The public key is the private key with every byte xored by 0x5A, and the shared secret is the xor of both private keys, so
// both sides get the same secret.
constexpr uint8_t kMask = 0x5A;

inline bool fakeGenerate(void *context, uint8_t privateKey[kPublicKeySize], uint8_t publicKey[kPublicKeySize]) {
  Knobs *knobs = static_cast<Knobs *>(context);
  if (knobs->failGenerate) {
    return false;
  }
  for (size_t i = 0; i < kPublicKeySize; i++) {
    privateKey[i] = static_cast<uint8_t>(knobs->next + i);
    publicKey[i] = privateKey[i] ^ kMask;
  }
  knobs->next++;
  return true;
}

inline bool fakeAgree(void *context, const uint8_t privateKey[kPublicKeySize], const uint8_t peerPublicKey[kPublicKeySize],
               uint8_t shared[kSharedSecretSize]) {
  const Knobs *knobs = static_cast<Knobs *>(context);
  if (knobs->failAgree) {
    return false;
  }
  for (size_t i = 0; i < kSharedSecretSize; i++) {
    shared[i] = knobs->zeroShared ? 0 : static_cast<uint8_t>(privateKey[i] ^ peerPublicKey[i] ^ kMask);
  }
  return true;
}

// A store in memory. It can fail every write, or only the write with a given number.
class FakeStore : public RecordStore {
 public:
  ReadResult read(size_t slot, uint8_t out[kRecordSize]) override {
    if (!present[slot]) {
      return ReadResult::Missing;
    }
    memcpy(out, data[slot], kRecordSize);
    return ReadResult::Ok;
  }
  bool write(size_t slot, const uint8_t in[kRecordSize]) override {
    writes++;
    if (failWrites || writes == failWriteNumber) {
      return false;
    }
    memcpy(data[slot], in, kRecordSize);
    present[slot] = true;
    return true;
  }

  uint8_t data[kMaxNodes][kRecordSize] = {};
  bool present[kMaxNodes] = {};
  bool failWrites = false;
  int failWriteNumber = 0;
  int writes = 0;
};

}  // namespace testsupport
}  // namespace wattlink
