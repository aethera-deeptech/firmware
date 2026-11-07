#include "crypto-selftest.h"

#include <string.h>

#include "frame-codec.h"
#include "hkdf-sha256.h"
#include "mbedtls-crypto.h"
#include "payloads.h"
#include "secure-frame.h"

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
  }
  return "FAILED unknown";
}

}  // namespace wattlink
