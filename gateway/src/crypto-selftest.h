#pragma once

namespace wattlink {

enum class SelfTestResult {
  Ok,
  HkdfFailed,
  SealFailed,
  OpenFailed,
  TamperAccepted,
};

// Runs the known answers of the host tests (RFC 5869 case 1 and a COMMAND frame that an independent AES-GCM made) through the mbedTLS of
// the chip. A failure means that the wrappers or the chip disagree with the host tests, and the hub must not pair with a node. The caller keeps the result for the pairing code to check.
SelfTestResult runCryptoSelfTest();

const char *selfTestName(SelfTestResult result);

}  // namespace wattlink
