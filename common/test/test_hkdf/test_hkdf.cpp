#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "hkdf-sha256.h"
#include "pairing-key.h"

using namespace wattlink;

namespace {

// OpenSSL stands in for mbedTLS on the host, so the tests check the derivation and not the hash.
void opensslHmac(void *, const uint8_t *key, size_t keyLength, const uint8_t *data, size_t dataLength, uint8_t out[kSha256Size]) {
  unsigned int outLength = 0;
  HMAC(EVP_sha256(), key, static_cast<int>(keyLength), data, dataLength, out, &outLength);
}

size_t fromHex(const char *hex, uint8_t *out) {
  size_t length = 0;
  for (; hex[0] != '\0' && hex[1] != '\0'; hex += 2) {
    char pair[3] = {hex[0], hex[1], '\0'};
    out[length++] = static_cast<uint8_t>(strtoul(pair, nullptr, 16));
  }
  return length;
}

void fillRange(uint8_t *out, size_t length, uint8_t first) {
  for (size_t i = 0; i < length; i++) {
    out[i] = static_cast<uint8_t>(first + i);
  }
}

}  // namespace

void setUp() {}
void tearDown() {}

// RFC 5869, appendix A.1
void test_rfc5869_case_1() {
  uint8_t ikm[22];
  memset(ikm, 0x0b, sizeof(ikm));
  uint8_t salt[13];
  fromHex("000102030405060708090a0b0c", salt);
  uint8_t info[10];
  fromHex("f0f1f2f3f4f5f6f7f8f9", info);
  uint8_t expected[42];
  fromHex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", expected);

  uint8_t okm[42];
  TEST_ASSERT_TRUE(hkdfSha256(opensslHmac, nullptr, salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), okm, sizeof(okm)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, okm, sizeof(okm));
}

// RFC 5869, appendix A.2, with inputs and output longer than one hash block
void test_rfc5869_case_2() {
  uint8_t ikm[80];
  uint8_t salt[80];
  uint8_t info[80];
  fillRange(ikm, sizeof(ikm), 0x00);
  fillRange(salt, sizeof(salt), 0x60);
  fillRange(info, sizeof(info), 0xb0);
  uint8_t expected[82];
  fromHex(
      "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71cc30c"
      "58179ec3e87c14c01d5c1f3434f1d87",
      expected);

  uint8_t okm[82];
  TEST_ASSERT_TRUE(hkdfSha256(opensslHmac, nullptr, salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), okm, sizeof(okm)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, okm, sizeof(okm));
}

// RFC 5869, appendix A.3, with no salt and no info
void test_rfc5869_case_3() {
  uint8_t ikm[22];
  memset(ikm, 0x0b, sizeof(ikm));
  uint8_t expected[42];
  fromHex("8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8", expected);

  uint8_t okm[42];
  TEST_ASSERT_TRUE(hkdfSha256(opensslHmac, nullptr, nullptr, 0, ikm, sizeof(ikm), nullptr, 0, okm, sizeof(okm)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, okm, sizeof(okm));
}

void test_a_shorter_output_is_the_start_of_a_longer_one() {
  uint8_t ikm[22];
  memset(ikm, 0x0b, sizeof(ikm));
  uint8_t longOutput[42];
  uint8_t shortOutput[16];
  TEST_ASSERT_TRUE(hkdfSha256(opensslHmac, nullptr, nullptr, 0, ikm, sizeof(ikm), nullptr, 0, longOutput, sizeof(longOutput)));
  TEST_ASSERT_TRUE(hkdfSha256(opensslHmac, nullptr, nullptr, 0, ikm, sizeof(ikm), nullptr, 0, shortOutput, sizeof(shortOutput)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(longOutput, shortOutput, sizeof(shortOutput));
}

void test_an_empty_output_succeeds_and_writes_nothing() {
  uint8_t ikm[1] = {1};
  uint8_t okm[1] = {0xAA};
  TEST_ASSERT_TRUE(hkdfSha256(opensslHmac, nullptr, nullptr, 0, ikm, sizeof(ikm), nullptr, 0, okm, 0));
  TEST_ASSERT_EQUAL_UINT8(0xAA, okm[0]);
}

void test_the_limits_are_refused() {
  uint8_t ikm[1] = {1};
  uint8_t info[kMaxHkdfInfoSize + 1] = {0};
  uint8_t okm[1];
  TEST_ASSERT_FALSE(hkdfSha256(opensslHmac, nullptr, nullptr, 0, ikm, sizeof(ikm), info, kMaxHkdfInfoSize + 1, okm, sizeof(okm)));
  TEST_ASSERT_FALSE(hkdfSha256(opensslHmac, nullptr, nullptr, 0, ikm, sizeof(ikm), nullptr, 0, okm, kMaxHkdfOutputSize + 1));
  TEST_ASSERT_TRUE(hkdfSha256(opensslHmac, nullptr, nullptr, 0, ikm, sizeof(ikm), info, kMaxHkdfInfoSize, okm, sizeof(okm)));
}

void test_the_pairing_salt_is_the_sha256_of_the_protocol_text() {
  const char text[] = "wattlink-espnow-v1";
  uint8_t digest[kSha256Size];
  SHA256(reinterpret_cast<const uint8_t *>(text), strlen(text), digest);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(digest, kPairingSalt, kSha256Size);
}

namespace {

struct PairingInputs {
  uint8_t secret[kSharedSecretSize];
  uint8_t nodeKey[kPublicKeySize];
  uint8_t hubKey[kPublicKeySize];
  uint8_t nodeMac[kMacSize];
  uint8_t hubMac[kMacSize];

  PairingInputs() {
    fillRange(secret, sizeof(secret), 0x01);
    fillRange(nodeKey, sizeof(nodeKey), 0x40);
    fillRange(hubKey, sizeof(hubKey), 0x80);
    fillRange(nodeMac, sizeof(nodeMac), 0xa0);
    fillRange(hubMac, sizeof(hubMac), 0xb0);
  }
};

}  // namespace

// The expected key was computed once with an independent implementation in Python from the layout of the protocol document.
void test_the_pairing_key_matches_the_reference_value() {
  const PairingInputs in;
  uint8_t expected[kPairingKeySize];
  fromHex("0f086e6ee650031857b42ed129b3696b", expected);

  uint8_t key[kPairingKeySize];
  TEST_ASSERT_TRUE(derivePairingKey(opensslHmac, nullptr, in.secret, in.nodeKey, in.hubKey, in.nodeMac, in.hubMac, key));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, key, sizeof(key));
}

void test_the_order_of_the_keys_and_the_macs_matters() {
  const PairingInputs in;
  uint8_t swapped[kPairingKeySize];
  TEST_ASSERT_TRUE(derivePairingKey(opensslHmac, nullptr, in.secret, in.hubKey, in.nodeKey, in.hubMac, in.nodeMac, swapped));
  uint8_t expectedSwapped[kPairingKeySize];
  fromHex("29dba17ea5aec96b45940aaef59dae70", expectedSwapped);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedSwapped, swapped, sizeof(swapped));

  uint8_t normal[kPairingKeySize];
  TEST_ASSERT_TRUE(derivePairingKey(opensslHmac, nullptr, in.secret, in.nodeKey, in.hubKey, in.nodeMac, in.hubMac, normal));
  TEST_ASSERT_NOT_EQUAL(0, memcmp(normal, swapped, sizeof(normal)));
}

void test_an_all_zero_shared_secret_is_refused() {
  PairingInputs in;
  memset(in.secret, 0, sizeof(in.secret));
  uint8_t key[kPairingKeySize] = {0};
  TEST_ASSERT_FALSE(derivePairingKey(opensslHmac, nullptr, in.secret, in.nodeKey, in.hubKey, in.nodeMac, in.hubMac, key));
}

void test_one_set_bit_in_the_secret_is_enough() {
  PairingInputs in;
  memset(in.secret, 0, sizeof(in.secret));
  in.secret[kSharedSecretSize - 1] = 1;
  uint8_t key[kPairingKeySize];
  TEST_ASSERT_TRUE(derivePairingKey(opensslHmac, nullptr, in.secret, in.nodeKey, in.hubKey, in.nodeMac, in.hubMac, key));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_rfc5869_case_1);
  RUN_TEST(test_rfc5869_case_2);
  RUN_TEST(test_rfc5869_case_3);
  RUN_TEST(test_a_shorter_output_is_the_start_of_a_longer_one);
  RUN_TEST(test_an_empty_output_succeeds_and_writes_nothing);
  RUN_TEST(test_the_limits_are_refused);
  RUN_TEST(test_the_pairing_salt_is_the_sha256_of_the_protocol_text);
  RUN_TEST(test_the_pairing_key_matches_the_reference_value);
  RUN_TEST(test_the_order_of_the_keys_and_the_macs_matters);
  RUN_TEST(test_an_all_zero_shared_secret_is_refused);
  RUN_TEST(test_one_set_bit_in_the_secret_is_enough);
  return UNITY_END();
}
