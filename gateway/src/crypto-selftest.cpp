#include "crypto-selftest.h"

#include <string.h>

#include "frame-codec.h"
#include "hkdf-sha256.h"
#include "mbedtls-crypto.h"
#include "mbedtls-x25519.h"
#include "payloads.h"
#include "secure-frame.h"
#include "secure-wipe.h"

namespace wattlink {

namespace {

// OKM of RFC 5869, appendix A.1, for the input below.
const uint8_t kHkdfExpected[42] = {
    0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a, 0x90, 0x43, 0x4f, 0x64, 0xd0, 0x36, 0x2f, 0x2a, 0x2d, 0x2d, 0x0a, 0x90, 0xcf, 0x1a,
    0x5a, 0x4c, 0x5d, 0xb0, 0x2d, 0x56, 0xec, 0xc4, 0xc5, 0xbf, 0x34, 0x00, 0x72, 0x08, 0xd5, 0xb8, 0x87, 0x18, 0x58, 0x65,
};

// A COMMAND frame with the header 01 30 01 02 03 04 05 06 07 08, the key 00 01 ... 0f and the payload below. It was made with an
// independent AES-GCM (the cryptography package of Python), and common/test/test_secure_frame has the same bytes.
const uint8_t kCommandFrame[37] = {
    0x01, 0x30, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x86, 0x45, 0xe2, 0x4f, 0x35, 0x7f, 0x49, 0x59, 0x63, 0x3b, 0x24, 0x62, 0x5d,
    0x58, 0x17, 0xc2, 0x4c, 0x12, 0x5d, 0x55, 0x14, 0x37, 0xf6, 0xa3, 0xc2, 0x59, 0x8b,
};

const uint8_t kCommandPayload[kCommandSize] = {0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55, 0x0B, 0x0A, 0x01};

// RFC 7748, section 6.1. Alice and Bob each have a private key, and both get the same shared secret.
const uint8_t kAlicePrivate[kPublicKeySize] = {
    0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d, 0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
    0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a, 0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
};
const uint8_t kAlicePublic[kPublicKeySize] = {
    0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54, 0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
    0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4, 0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a,
};
const uint8_t kBobPrivate[kPublicKeySize] = {
    0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b, 0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
    0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd, 0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb,
};
const uint8_t kBobPublic[kPublicKeySize] = {
    0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4, 0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
    0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d, 0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f,
};
const uint8_t kSharedExpected[kSharedSecretSize] = {
    0x4a, 0x5d, 0x9d, 0x5b, 0xa4, 0xce, 0x2d, 0xe1, 0x72, 0x8e, 0x3b, 0xf4, 0x80, 0x35, 0x0f, 0x25,
    0xe0, 0x7e, 0x21, 0xc9, 0x47, 0xd1, 0x9e, 0x33, 0x76, 0xf0, 0x9b, 0x3c, 0x1e, 0x16, 0x17, 0x42,
};

bool x25519Matches() {
  uint8_t publicKey[kPublicKeySize];
  uint8_t shared[kSharedSecretSize];
  const bool ok = mbedtlsX25519PublicKey(kAlicePrivate, publicKey) && memcmp(publicKey, kAlicePublic, sizeof(publicKey)) == 0 &&
                  mbedtlsX25519PublicKey(kBobPrivate, publicKey) && memcmp(publicKey, kBobPublic, sizeof(publicKey)) == 0 &&
                  mbedtlsX25519Agree(nullptr, kAlicePrivate, kBobPublic, shared) && memcmp(shared, kSharedExpected, sizeof(shared)) == 0 &&
                  mbedtlsX25519Agree(nullptr, kBobPrivate, kAlicePublic, shared) && memcmp(shared, kSharedExpected, sizeof(shared)) == 0;
  wipe(shared, sizeof(shared));
  return ok;
}

bool hkdfMatches() {
  uint8_t ikm[22];
  memset(ikm, 0x0b, sizeof(ikm));
  uint8_t salt[13];
  for (size_t i = 0; i < sizeof(salt); i++) {
    salt[i] = static_cast<uint8_t>(i);
  }
  uint8_t info[10];
  for (size_t i = 0; i < sizeof(info); i++) {
    info[i] = static_cast<uint8_t>(0xf0 + i);
  }
  uint8_t okm[sizeof(kHkdfExpected)];
  return hkdfSha256(mbedtlsHmacSha256, nullptr, salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), okm, sizeof(okm)) &&
         memcmp(okm, kHkdfExpected, sizeof(okm)) == 0;
}

}  // namespace

SelfTestResult runCryptoSelfTest() {
  if (!hkdfMatches()) {
    return SelfTestResult::HkdfFailed;
  }

  if (!x25519Matches()) {
    return SelfTestResult::X25519Failed;
  }

  uint8_t key[kAesKeySize];
  for (size_t i = 0; i < sizeof(key); i++) {
    key[i] = static_cast<uint8_t>(i);
  }

  const Header header = {kProtocolVersion, FrameType::Command, 0x04030201, 0x08070605};
  uint8_t sealed[kMaxFrameSize];
  const size_t sealedLength = sealFrame(mbedtlsAesGcmSeal, nullptr, key, header, kCommandPayload, sizeof(kCommandPayload), sealed, sizeof(sealed));
  if (sealedLength != sizeof(kCommandFrame) || memcmp(sealed, kCommandFrame, sizeof(kCommandFrame)) != 0) {
    return SelfTestResult::SealFailed;
  }

  Header opened;
  uint8_t plain[kMaxPayloadSize];
  size_t plainLength = 0;
  if (!openFrame(mbedtlsAesGcmOpen, nullptr, key, Direction::HubToNode, kCommandFrame, sizeof(kCommandFrame), &opened, plain, sizeof(plain),
                 &plainLength) ||
      plainLength != sizeof(kCommandPayload) || memcmp(plain, kCommandPayload, sizeof(kCommandPayload)) != 0) {
    return SelfTestResult::OpenFailed;
  }

  // One changed bit must be refused, or the tag is not checked.
  uint8_t tampered[sizeof(kCommandFrame)];
  memcpy(tampered, kCommandFrame, sizeof(tampered));
  tampered[sizeof(tampered) - 1] ^= 0x01;
  if (openFrame(mbedtlsAesGcmOpen, nullptr, key, Direction::HubToNode, tampered, sizeof(tampered), &opened, plain, sizeof(plain), &plainLength)) {
    return SelfTestResult::TamperAccepted;
  }
  return SelfTestResult::Ok;
}

const char *selfTestName(SelfTestResult result) {
  switch (result) {
    case SelfTestResult::Ok:
      return "ok";
    case SelfTestResult::HkdfFailed:
      return "FAILED hkdf";
    case SelfTestResult::SealFailed:
      return "FAILED seal";
    case SelfTestResult::OpenFailed:
      return "FAILED open";
    case SelfTestResult::TamperAccepted:
      return "FAILED tamper accepted";
    case SelfTestResult::X25519Failed:
      return "FAILED x25519";
  }
  return "FAILED unknown";
}

}  // namespace wattlink
