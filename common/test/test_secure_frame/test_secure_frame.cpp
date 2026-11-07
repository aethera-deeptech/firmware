#include <openssl/evp.h>
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "secure-frame.h"

using namespace wattlink;

namespace {

// OpenSSL stands in for mbedTLS on the host, so the tests check the layout of the frame and not the cipher.
bool opensslSeal(void *, const uint8_t key[kAesKeySize], const uint8_t nonce[kNonceSize], const uint8_t *aad, size_t aadLength,
                 const uint8_t *plain, size_t length, uint8_t *cipher, uint8_t tag[kTagSize]) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  int outLength = 0;
  bool ok = ctx != nullptr && EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) == 1 &&
            EVP_EncryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1 && EVP_EncryptUpdate(ctx, nullptr, &outLength, aad, static_cast<int>(aadLength)) == 1 &&
            EVP_EncryptUpdate(ctx, cipher, &outLength, plain, static_cast<int>(length)) == 1;
  if (ok) {
    int finalLength = 0;
    ok = EVP_EncryptFinal_ex(ctx, cipher + outLength, &finalLength) == 1 && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagSize, tag) == 1;
  }
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

bool opensslOpen(void *, const uint8_t key[kAesKeySize], const uint8_t nonce[kNonceSize], const uint8_t *aad, size_t aadLength,
                 const uint8_t *cipher, size_t length, const uint8_t tag[kTagSize], uint8_t *plain) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  int outLength = 0;
  bool ok = ctx != nullptr && EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, nullptr, nullptr) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceSize, nullptr) == 1 &&
            EVP_DecryptInit_ex(ctx, nullptr, nullptr, key, nonce) == 1 && EVP_DecryptUpdate(ctx, nullptr, &outLength, aad, static_cast<int>(aadLength)) == 1 &&
            EVP_DecryptUpdate(ctx, plain, &outLength, cipher, static_cast<int>(length)) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagSize, const_cast<uint8_t *>(tag)) == 1;
  if (ok) {
    int finalLength = 0;
    ok = EVP_DecryptFinal_ex(ctx, plain + outLength, &finalLength) == 1;
  }
  EVP_CIPHER_CTX_free(ctx);
  return ok;
}

// A backend that records what it was given and can be told to fail.
struct Spy {
  bool fail = false;
  int calls = 0;
  uint8_t nonce[kNonceSize] = {0};
  size_t aadLength = 0;
};

bool spySeal(void *context, const uint8_t key[kAesKeySize], const uint8_t nonce[kNonceSize], const uint8_t *aad, size_t aadLength,
             const uint8_t *plain, size_t length, uint8_t *cipher, uint8_t tag[kTagSize]) {
  Spy *spy = static_cast<Spy *>(context);
  spy->calls++;
  memcpy(spy->nonce, nonce, kNonceSize);
  spy->aadLength = aadLength;
  if (spy->fail) {
    return false;
  }
  return opensslSeal(nullptr, key, nonce, aad, aadLength, plain, length, cipher, tag);
}

bool spyOpen(void *context, const uint8_t key[kAesKeySize], const uint8_t nonce[kNonceSize], const uint8_t *aad, size_t aadLength,
             const uint8_t *cipher, size_t length, const uint8_t tag[kTagSize], uint8_t *plain) {
  Spy *spy = static_cast<Spy *>(context);
  spy->calls++;
  if (spy->fail) {
    return false;
  }
  return opensslOpen(nullptr, key, nonce, aad, aadLength, cipher, length, tag, plain);
}

size_t fromHex(const char *hex, uint8_t *out) {
  size_t length = 0;
  for (; hex[0] != '\0' && hex[1] != '\0'; hex += 2) {
    char pair[3] = {hex[0], hex[1], '\0'};
    out[length++] = static_cast<uint8_t>(strtoul(pair, nullptr, 16));
  }
  return length;
}

uint8_t gKey[kAesKeySize];

}  // namespace

void setUp() {
  for (size_t i = 0; i < kAesKeySize; i++) {
    gKey[i] = static_cast<uint8_t>(i);
  }
}
void tearDown() {}

// The expected frames were made with an independent implementation (Python, cryptography) from the layout of the protocol document.
void test_a_command_frame_matches_the_reference_bytes() {
  const Header header = {kProtocolVersion, FrameType::Command, 0x04030201, 0x08070605};
  const Command command = {0x11223344, 0x55667788, 0x0A0B, 1};
  uint8_t payload[kCommandSize];
  encodeCommand(command, payload, sizeof(payload));
  uint8_t expected[64];
  const size_t expectedLength = fromHex("013001020304050607088645e24f357f4959633b24625d5817c24c125d551437f6a3c2598b", expected);
  TEST_ASSERT_EQUAL_UINT(37, expectedLength);

  uint8_t out[kMaxFrameSize];
  TEST_ASSERT_EQUAL_UINT(37, sealFrame(opensslSeal, nullptr, gKey, header, payload, sizeof(payload), out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, 37);

  Header opened;
  uint8_t plain[kMaxPayloadSize];
  size_t plainLength = 0;
  TEST_ASSERT_TRUE(openFrame(opensslOpen, nullptr, gKey, expected, expectedLength, &opened, plain, sizeof(plain), &plainLength));
  TEST_ASSERT_EQUAL_UINT(kCommandSize, plainLength);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, plain, kCommandSize);
  TEST_ASSERT_EQUAL_UINT32(0x04030201, opened.bootId);
  TEST_ASSERT_EQUAL_UINT32(0x08070605, opened.counter);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FrameType::Command), static_cast<uint8_t>(opened.type));
}

void test_an_ack_frame_uses_the_hub_to_node_direction() {
  const Header header = {kProtocolVersion, FrameType::Ack, 9, 3};
  const Ack ack = {0xDDCCBBAA, 0x10};
  uint8_t payload[kAckSize];
  encodeAck(ack, payload, sizeof(payload));
  uint8_t expected[64];
  const size_t expectedLength = fromHex("012109000000030000003d6f724120ff179669d0566a96ebba7a9bec105e69e9b11c", expected);
  uint8_t out[kMaxFrameSize];
  TEST_ASSERT_EQUAL_UINT(expectedLength, sealFrame(opensslSeal, nullptr, gKey, header, payload, sizeof(payload), out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, expectedLength);
}

void test_a_result_frame_uses_the_node_to_hub_direction() {
  const Header header = {kProtocolVersion, FrameType::Result, 7, 2};
  const Result result = {0x12345678, 0x0A0B, 1};
  uint8_t payload[kResultSize];
  encodeResult(result, payload, sizeof(payload));
  uint8_t expected[64];
  const size_t expectedLength = fromHex("0131070000000200000042c21e1d6d431932b371d1c21b13ba41d947887a632b12", expected);
  uint8_t out[kMaxFrameSize];
  TEST_ASSERT_EQUAL_UINT(expectedLength, sealFrame(opensslSeal, nullptr, gKey, header, payload, sizeof(payload), out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, expectedLength);
}

void test_the_nonce_and_the_associated_data_follow_the_header() {
  Spy spy;
  uint8_t out[kMaxFrameSize];
  const Header toNode = {kProtocolVersion, FrameType::Command, 0x04030201, 0x08070605};
  uint8_t payload[kCommandSize] = {0};
  TEST_ASSERT_TRUE(sealFrame(spySeal, &spy, gKey, toNode, payload, sizeof(payload), out, sizeof(out)) > 0);
  const uint8_t expectedToNode[kNonceSize] = {1, 2, 3, 4, 5, 6, 7, 8, 1, 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedToNode, spy.nonce, kNonceSize);
  TEST_ASSERT_EQUAL_UINT(kHeaderSize, spy.aadLength);

  const Header toHub = {kProtocolVersion, FrameType::Result, 0x04030201, 0x08070605};
  uint8_t resultPayload[kResultSize] = {0};
  TEST_ASSERT_TRUE(sealFrame(spySeal, &spy, gKey, toHub, resultPayload, sizeof(resultPayload), out, sizeof(out)) > 0);
  const uint8_t expectedToHub[kNonceSize] = {1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedToHub, spy.nonce, kNonceSize);
  TEST_ASSERT_EQUAL_INT(2, spy.calls);
}

void test_a_full_telemetry_frame_round_trips_at_the_size_limit() {
  Sample samples[kMaxSamplesPerFrame];
  for (size_t i = 0; i < kMaxSamplesPerFrame; i++) {
    memset(&samples[i], 0, sizeof(samples[i]));
    samples[i].sampleSeq = static_cast<uint32_t>(100 + i);
    samples[i].powerDeciW = static_cast<uint32_t>(2000 + i);
  }
  uint8_t payload[kTelemetryPrefixSize + kMaxSamplesPerFrame * kSampleSize];
  const size_t payloadLength = encodeTelemetryPayload(55, samples, kMaxSamplesPerFrame, payload, sizeof(payload));
  const Header header = {kProtocolVersion, FrameType::Telemetry, 3, 1};
  uint8_t frame[kMaxFrameSize];
  const size_t frameLength = sealFrame(opensslSeal, nullptr, gKey, header, payload, payloadLength, frame, sizeof(frame));
  TEST_ASSERT_EQUAL_UINT(231, frameLength);

  Header opened;
  uint8_t plain[kMaxPayloadSize];
  size_t plainLength = 0;
  TEST_ASSERT_TRUE(openFrame(opensslOpen, nullptr, gKey, frame, frameLength, &opened, plain, sizeof(plain), &plainLength));
  TEST_ASSERT_EQUAL_UINT(payloadLength, plainLength);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, plain, payloadLength);
  Sample decoded[kMaxSamplesPerFrame];
  uint32_t hubBootId = 0;
  size_t count = 0;
  TEST_ASSERT_TRUE(decodeTelemetryPayload(plain, plainLength, &hubBootId, decoded, kMaxSamplesPerFrame, &count));
  TEST_ASSERT_EQUAL_UINT32(55, hubBootId);
  TEST_ASSERT_EQUAL_UINT(8, count);
  TEST_ASSERT_EQUAL_UINT32(107, decoded[7].sampleSeq);
}

void test_every_changed_byte_of_a_frame_is_refused() {
  const Header header = {kProtocolVersion, FrameType::Command, 5, 6};
  const Command command = {1, 2, 3, 0};
  uint8_t payload[kCommandSize];
  encodeCommand(command, payload, sizeof(payload));
  uint8_t frame[kMaxFrameSize];
  const size_t length = sealFrame(opensslSeal, nullptr, gKey, header, payload, sizeof(payload), frame, sizeof(frame));
  TEST_ASSERT_EQUAL_UINT(37, length);

  for (size_t i = 0; i < length; i++) {
    uint8_t changed[kMaxFrameSize];
    memcpy(changed, frame, length);
    changed[i] ^= 0x01;
    Header opened;
    uint8_t plain[kMaxPayloadSize];
    size_t plainLength = 0;
    TEST_ASSERT_FALSE_MESSAGE(openFrame(opensslOpen, nullptr, gKey, changed, length, &opened, plain, sizeof(plain), &plainLength), "a changed byte was accepted");
  }
}

void test_a_failed_open_leaves_zeros_in_the_payload_and_the_header_alone() {
  const Header header = {kProtocolVersion, FrameType::Ack, 5, 6};
  const Ack ack = {1, 2};
  uint8_t payload[kAckSize];
  encodeAck(ack, payload, sizeof(payload));
  uint8_t frame[kMaxFrameSize];
  const size_t length = sealFrame(opensslSeal, nullptr, gKey, header, payload, sizeof(payload), frame, sizeof(frame));
  frame[length - 1] ^= 0x80;

  Header opened = {9, FrameType::Hello, 9, 9};
  uint8_t plain[kMaxPayloadSize];
  memset(plain, 0x5A, sizeof(plain));
  size_t plainLength = 77;
  TEST_ASSERT_FALSE(openFrame(opensslOpen, nullptr, gKey, frame, length, &opened, plain, sizeof(plain), &plainLength));
  for (size_t i = 0; i < kAckSize; i++) {
    TEST_ASSERT_EQUAL_UINT8(0, plain[i]);
  }
  TEST_ASSERT_EQUAL_UINT8(0x5A, plain[kAckSize]);
  TEST_ASSERT_EQUAL_UINT8(9, opened.version);
  TEST_ASSERT_EQUAL_UINT(77, plainLength);
}

void test_another_key_is_refused() {
  const Header header = {kProtocolVersion, FrameType::Ack, 1, 1};
  const Ack ack = {1, 2};
  uint8_t payload[kAckSize];
  encodeAck(ack, payload, sizeof(payload));
  uint8_t frame[kMaxFrameSize];
  const size_t length = sealFrame(opensslSeal, nullptr, gKey, header, payload, sizeof(payload), frame, sizeof(frame));
  uint8_t other[kAesKeySize];
  memcpy(other, gKey, sizeof(other));
  other[15] ^= 1;
  Header opened;
  uint8_t plain[kMaxPayloadSize];
  size_t plainLength = 0;
  TEST_ASSERT_FALSE(openFrame(opensslOpen, nullptr, other, frame, length, &opened, plain, sizeof(plain), &plainLength));
}

void test_a_frame_with_a_valid_tag_but_a_wrong_payload_length_is_refused() {
  // Sealed with the raw cipher, so the tag is good and only the length of the payload is wrong for the type.
  const Header header = {kProtocolVersion, FrameType::Ack, 1, 1};
  uint8_t frame[kMaxFrameSize];
  encodeHeader(header, frame, sizeof(frame));
  uint8_t nonce[kNonceSize];
  buildNonce(1, 1, Direction::HubToNode, nonce);
  const uint8_t payload[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  TEST_ASSERT_TRUE(opensslSeal(nullptr, gKey, nonce, frame, kHeaderSize, payload, sizeof(payload), frame + kHeaderSize, frame + kHeaderSize + sizeof(payload)));
  Header opened;
  uint8_t plain[kMaxPayloadSize];
  size_t plainLength = 0;
  TEST_ASSERT_FALSE(openFrame(opensslOpen, nullptr, gKey, frame, kHeaderSize + sizeof(payload) + kTagSize, &opened, plain, sizeof(plain), &plainLength));
}

void test_open_refuses_short_long_and_unencrypted_frames() {
  Header opened;
  uint8_t plain[kMaxPayloadSize];
  size_t plainLength = 0;
  uint8_t frame[kMaxFrameSize + 8];
  memset(frame, 0, sizeof(frame));
  frame[0] = kProtocolVersion;
  frame[1] = static_cast<uint8_t>(FrameType::Ack);
  TEST_ASSERT_FALSE(openFrame(opensslOpen, nullptr, gKey, frame, 0, &opened, plain, sizeof(plain), &plainLength));
  TEST_ASSERT_FALSE(openFrame(opensslOpen, nullptr, gKey, frame, kHeaderSize + kTagSize - 1, &opened, plain, sizeof(plain), &plainLength));
  TEST_ASSERT_FALSE(openFrame(opensslOpen, nullptr, gKey, frame, kMaxFrameSize + 1, &opened, plain, sizeof(plain), &plainLength));
  frame[1] = static_cast<uint8_t>(FrameType::Hello);
  TEST_ASSERT_FALSE(openFrame(opensslOpen, nullptr, gKey, frame, kHeaderSize + 4 + kTagSize, &opened, plain, sizeof(plain), &plainLength));
  frame[1] = static_cast<uint8_t>(FrameType::Ack);
  frame[0] = 2;
  TEST_ASSERT_FALSE(openFrame(opensslOpen, nullptr, gKey, frame, kHeaderSize + kAckSize + kTagSize, &opened, plain, sizeof(plain), &plainLength));
}

void test_open_refuses_a_payload_buffer_that_is_too_small() {
  const Header header = {kProtocolVersion, FrameType::Ack, 1, 1};
  const Ack ack = {1, 2};
  uint8_t payload[kAckSize];
  encodeAck(ack, payload, sizeof(payload));
  uint8_t frame[kMaxFrameSize];
  const size_t length = sealFrame(opensslSeal, nullptr, gKey, header, payload, sizeof(payload), frame, sizeof(frame));
  Header opened;
  uint8_t plain[kAckSize];
  size_t plainLength = 0;
  TEST_ASSERT_FALSE(openFrame(opensslOpen, nullptr, gKey, frame, length, &opened, plain, kAckSize - 1, &plainLength));
  TEST_ASSERT_TRUE(openFrame(opensslOpen, nullptr, gKey, frame, length, &opened, plain, kAckSize, &plainLength));
}

void test_seal_refuses_bad_headers_lengths_and_buffers() {
  uint8_t out[kMaxFrameSize];
  uint8_t payload[kMaxPayloadSize] = {0};
  Header header = {kProtocolVersion, FrameType::Ack, 1, 1};
  TEST_ASSERT_EQUAL_UINT(0, sealFrame(opensslSeal, nullptr, gKey, header, payload, kAckSize - 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT(0, sealFrame(opensslSeal, nullptr, gKey, header, payload, kAckSize, out, kHeaderSize + kAckSize + kTagSize - 1));
  header.version = 2;
  TEST_ASSERT_EQUAL_UINT(0, sealFrame(opensslSeal, nullptr, gKey, header, payload, kAckSize, out, sizeof(out)));
  header.version = kProtocolVersion;
  header.type = FrameType::Hello;
  TEST_ASSERT_EQUAL_UINT(0, sealFrame(opensslSeal, nullptr, gKey, header, payload, kHelloSize, out, sizeof(out)));
  header.type = static_cast<FrameType>(0x99);
  TEST_ASSERT_EQUAL_UINT(0, sealFrame(opensslSeal, nullptr, gKey, header, payload, 0, out, sizeof(out)));
}

void test_a_failing_cipher_gives_no_frame() {
  Spy spy;
  spy.fail = true;
  uint8_t out[kMaxFrameSize];
  memset(out, 0x77, sizeof(out));
  const Header header = {kProtocolVersion, FrameType::Ack, 1, 1};
  uint8_t payload[kAckSize] = {0};
  TEST_ASSERT_EQUAL_UINT(0, sealFrame(spySeal, &spy, gKey, header, payload, sizeof(payload), out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT8(0, out[0]);
  TEST_ASSERT_EQUAL_INT(1, spy.calls);

  spy.fail = false;
  const size_t length = sealFrame(spySeal, &spy, gKey, header, payload, sizeof(payload), out, sizeof(out));
  spy.fail = true;
  Header opened;
  uint8_t plain[kMaxPayloadSize];
  size_t plainLength = 0;
  TEST_ASSERT_FALSE(openFrame(spyOpen, &spy, gKey, out, length, &opened, plain, sizeof(plain), &plainLength));
}

void test_plain_frames_are_the_header_and_the_payload() {
  const Header header = {kProtocolVersion, FrameType::Hello, 4, 0};
  const Hello hello = {NodeKind::Plug, 0, 1, 2};
  uint8_t payload[kHelloSize];
  encodeHello(hello, payload, sizeof(payload));
  uint8_t frame[kMaxFrameSize];
  const size_t length = buildPlainFrame(header, payload, sizeof(payload), frame, sizeof(frame));
  TEST_ASSERT_EQUAL_UINT(14, length);
  const uint8_t expected[14] = {1, 0x01, 4, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 2};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, frame, 14);

  Header parsed;
  const uint8_t *body = nullptr;
  size_t bodyLength = 0;
  TEST_ASSERT_TRUE(parsePlainFrame(frame, length, &parsed, &body, &bodyLength));
  TEST_ASSERT_EQUAL_UINT(4, bodyLength);
  TEST_ASSERT_EQUAL_PTR(frame + kHeaderSize, body);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FrameType::Hello), static_cast<uint8_t>(parsed.type));
}

void test_a_hello_ack_is_only_a_header() {
  const Header header = {kProtocolVersion, FrameType::HelloAck, 1, 1};
  uint8_t frame[kMaxFrameSize];
  TEST_ASSERT_EQUAL_UINT(kHeaderSize, buildPlainFrame(header, nullptr, 0, frame, sizeof(frame)));
  Header parsed;
  const uint8_t *body = nullptr;
  size_t bodyLength = 99;
  TEST_ASSERT_TRUE(parsePlainFrame(frame, kHeaderSize, &parsed, &body, &bodyLength));
  TEST_ASSERT_EQUAL_UINT(0, bodyLength);
}

void test_plain_frames_refuse_encrypted_types_wrong_lengths_and_small_buffers() {
  uint8_t frame[kMaxFrameSize];
  uint8_t payload[kMaxPayloadSize] = {0};
  Header header = {kProtocolVersion, FrameType::Telemetry, 1, 1};
  TEST_ASSERT_EQUAL_UINT(0, buildPlainFrame(header, payload, kTelemetryPrefixSize + kSampleSize, frame, sizeof(frame)));
  header.type = FrameType::PairRequest;
  TEST_ASSERT_EQUAL_UINT(0, buildPlainFrame(header, payload, kPairRequestSize - 1, frame, sizeof(frame)));
  TEST_ASSERT_EQUAL_UINT(0, buildPlainFrame(header, payload, kPairRequestSize, frame, kHeaderSize + kPairRequestSize - 1));
  TEST_ASSERT_EQUAL_UINT(kHeaderSize + kPairRequestSize, buildPlainFrame(header, payload, kPairRequestSize, frame, kHeaderSize + kPairRequestSize));
  header.version = 2;
  TEST_ASSERT_EQUAL_UINT(0, buildPlainFrame(header, payload, kPairRequestSize, frame, sizeof(frame)));

  Header parsed;
  const uint8_t *body = nullptr;
  size_t bodyLength = 0;
  uint8_t ack[kHeaderSize + kAckSize] = {kProtocolVersion, static_cast<uint8_t>(FrameType::Ack)};
  TEST_ASSERT_FALSE(parsePlainFrame(ack, sizeof(ack), &parsed, &body, &bodyLength));
  uint8_t hello[kHeaderSize + 5] = {kProtocolVersion, static_cast<uint8_t>(FrameType::Hello)};
  TEST_ASSERT_FALSE(parsePlainFrame(hello, sizeof(hello), &parsed, &body, &bodyLength));
  TEST_ASSERT_FALSE(parsePlainFrame(hello, kHeaderSize - 1, &parsed, &body, &bodyLength));
}

void test_the_largest_payload_matches_the_frame_limit() {
  TEST_ASSERT_EQUAL_UINT(224, kMaxPayloadSize);
  TEST_ASSERT_TRUE(kTelemetryPrefixSize + kMaxSamplesPerFrame * kSampleSize <= kMaxPayloadSize);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_a_command_frame_matches_the_reference_bytes);
  RUN_TEST(test_an_ack_frame_uses_the_hub_to_node_direction);
  RUN_TEST(test_a_result_frame_uses_the_node_to_hub_direction);
  RUN_TEST(test_the_nonce_and_the_associated_data_follow_the_header);
  RUN_TEST(test_a_full_telemetry_frame_round_trips_at_the_size_limit);
  RUN_TEST(test_every_changed_byte_of_a_frame_is_refused);
  RUN_TEST(test_a_failed_open_leaves_zeros_in_the_payload_and_the_header_alone);
  RUN_TEST(test_another_key_is_refused);
  RUN_TEST(test_a_frame_with_a_valid_tag_but_a_wrong_payload_length_is_refused);
  RUN_TEST(test_open_refuses_short_long_and_unencrypted_frames);
  RUN_TEST(test_open_refuses_a_payload_buffer_that_is_too_small);
  RUN_TEST(test_seal_refuses_bad_headers_lengths_and_buffers);
  RUN_TEST(test_a_failing_cipher_gives_no_frame);
  RUN_TEST(test_plain_frames_are_the_header_and_the_payload);
  RUN_TEST(test_a_hello_ack_is_only_a_header);
  RUN_TEST(test_plain_frames_refuse_encrypted_types_wrong_lengths_and_small_buffers);
  RUN_TEST(test_the_largest_payload_matches_the_frame_limit);
  return UNITY_END();
}
