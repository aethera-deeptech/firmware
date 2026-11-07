#include <string.h>
#include <unity.h>

#include "payloads.h"

using namespace wattlink;

namespace {

void fillRange(uint8_t *out, size_t length, uint8_t first) {
  for (size_t i = 0; i < length; i++) {
    out[i] = static_cast<uint8_t>(first + i);
  }
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_hello_layout_and_round_trip() {
  const Hello hello = {NodeKind::Clamp, 1, 2, 3};
  uint8_t out[kHelloSize];
  TEST_ASSERT_EQUAL_UINT(kHelloSize, encodeHello(hello, out, sizeof(out)));
  const uint8_t expected[kHelloSize] = {2, 1, 2, 3};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, kHelloSize);
  Hello decoded = {NodeKind::Plug, 0, 0, 0};
  TEST_ASSERT_TRUE(decodeHello(out, sizeof(out), &decoded));
  TEST_ASSERT_EQUAL_UINT8(2, static_cast<uint8_t>(decoded.kind));
  TEST_ASSERT_EQUAL_UINT8(1, decoded.major);
  TEST_ASSERT_EQUAL_UINT8(2, decoded.minor);
  TEST_ASSERT_EQUAL_UINT8(3, decoded.patch);
}

void test_hello_refuses_bad_input() {
  uint8_t out[8];
  const Hello bad = {static_cast<NodeKind>(3), 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT(0, encodeHello(bad, out, sizeof(out)));
  const Hello good = {NodeKind::Plug, 0, 0, 0};
  TEST_ASSERT_EQUAL_UINT(0, encodeHello(good, out, kHelloSize - 1));
  Hello value = {NodeKind::Plug, 9, 9, 9};
  const uint8_t zeroKind[kHelloSize] = {0, 1, 2, 3};
  const uint8_t thirdKind[kHelloSize] = {3, 1, 2, 3};
  TEST_ASSERT_FALSE(decodeHello(zeroKind, kHelloSize, &value));
  TEST_ASSERT_FALSE(decodeHello(thirdKind, kHelloSize, &value));
  const uint8_t plug[kHelloSize] = {1, 1, 2, 3};
  TEST_ASSERT_FALSE(decodeHello(plug, kHelloSize - 1, &value));
  TEST_ASSERT_FALSE(decodeHello(plug, kHelloSize + 1, &value));
  TEST_ASSERT_EQUAL_UINT8(9, value.major);
}

void test_pair_request_layout_and_refusals() {
  PairRequest request;
  request.kind = NodeKind::Plug;
  fillRange(request.nodePublicKey, kPublicKeyBytes, 0x40);
  uint8_t out[kPairRequestSize];
  TEST_ASSERT_EQUAL_UINT(kPairRequestSize, encodePairRequest(request, out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT8(1, out[0]);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(request.nodePublicKey, out + 1, kPublicKeyBytes);
  PairRequest decoded;
  memset(&decoded, 0, sizeof(decoded));
  TEST_ASSERT_TRUE(decodePairRequest(out, sizeof(out), &decoded));
  TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(decoded.kind));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(request.nodePublicKey, decoded.nodePublicKey, kPublicKeyBytes);
  TEST_ASSERT_FALSE(decodePairRequest(out, sizeof(out) - 1, &decoded));
  out[0] = 0;
  TEST_ASSERT_FALSE(decodePairRequest(out, sizeof(out), &decoded));
  TEST_ASSERT_EQUAL_UINT(0, encodePairRequest(request, out, kPairRequestSize - 1));
}

void test_pair_accept_holds_the_hub_key_then_the_node_key() {
  PairAccept accept;
  fillRange(accept.hubPublicKey, kPublicKeyBytes, 0x80);
  fillRange(accept.nodePublicKey, kPublicKeyBytes, 0x40);
  uint8_t out[kPairAcceptSize];
  TEST_ASSERT_EQUAL_UINT(64, encodePairAccept(accept, out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(accept.hubPublicKey, out, kPublicKeyBytes);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(accept.nodePublicKey, out + kPublicKeyBytes, kPublicKeyBytes);
  PairAccept decoded;
  TEST_ASSERT_TRUE(decodePairAccept(out, sizeof(out), &decoded));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(accept.hubPublicKey, decoded.hubPublicKey, kPublicKeyBytes);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(accept.nodePublicKey, decoded.nodePublicKey, kPublicKeyBytes);
  TEST_ASSERT_FALSE(decodePairAccept(out, sizeof(out) + 1, &decoded));
  TEST_ASSERT_EQUAL_UINT(0, encodePairAccept(accept, out, sizeof(out) - 1));
}

void test_ack_layout() {
  const Ack ack = {0x04030201, 0x08070605};
  uint8_t out[kAckSize];
  TEST_ASSERT_EQUAL_UINT(8, encodeAck(ack, out, sizeof(out)));
  const uint8_t expected[kAckSize] = {1, 2, 3, 4, 5, 6, 7, 8};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, kAckSize);
  Ack decoded = {0, 0};
  TEST_ASSERT_TRUE(decodeAck(out, sizeof(out), &decoded));
  TEST_ASSERT_EQUAL_UINT32(0x04030201, decoded.nodeBootId);
  TEST_ASSERT_EQUAL_UINT32(0x08070605, decoded.highestSampleSeq);
  TEST_ASSERT_FALSE(decodeAck(out, 7, &decoded));
  TEST_ASSERT_FALSE(decodeAck(out, 9, &decoded));
  TEST_ASSERT_EQUAL_UINT(0, encodeAck(ack, out, 7));
}

void test_command_layout_and_action_values() {
  const Command command = {0x11223344, 0x55667788, 0x0A0B, 1};
  uint8_t out[kCommandSize];
  TEST_ASSERT_EQUAL_UINT(11, encodeCommand(command, out, sizeof(out)));
  const uint8_t expected[kCommandSize] = {0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55, 0x0B, 0x0A, 1};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, kCommandSize);
  Command decoded;
  memset(&decoded, 0, sizeof(decoded));
  TEST_ASSERT_TRUE(decodeCommand(out, sizeof(out), &decoded));
  TEST_ASSERT_EQUAL_UINT32(0x11223344, decoded.echoBootId);
  TEST_ASSERT_EQUAL_UINT32(0x55667788, decoded.echoCounter);
  TEST_ASSERT_EQUAL_UINT16(0x0A0B, decoded.number);
  TEST_ASSERT_EQUAL_UINT8(1, decoded.action);

  Command off = command;
  off.action = 0;
  TEST_ASSERT_EQUAL_UINT(kCommandSize, encodeCommand(off, out, sizeof(out)));
  TEST_ASSERT_TRUE(decodeCommand(out, sizeof(out), &decoded));
  TEST_ASSERT_EQUAL_UINT8(0, decoded.action);
}

void test_command_refuses_a_toggle_or_any_other_action() {
  Command command = {1, 2, 3, 2};
  uint8_t out[kCommandSize];
  TEST_ASSERT_EQUAL_UINT(0, encodeCommand(command, out, sizeof(out)));
  command.action = 1;
  encodeCommand(command, out, sizeof(out));
  out[10] = 2;
  Command decoded = {7, 7, 7, 7};
  TEST_ASSERT_FALSE(decodeCommand(out, sizeof(out), &decoded));
  out[10] = 0xFF;
  TEST_ASSERT_FALSE(decodeCommand(out, sizeof(out), &decoded));
  TEST_ASSERT_EQUAL_UINT8(7, decoded.action);
  TEST_ASSERT_FALSE(decodeCommand(out, kCommandSize - 1, &decoded));
  TEST_ASSERT_EQUAL_UINT(0, encodeCommand(command, out, kCommandSize - 1));
}

void test_result_layout_and_ok_values() {
  const Result result = {0x12345678, 0x0A0B, 1};
  uint8_t out[kResultSize];
  TEST_ASSERT_EQUAL_UINT(7, encodeResult(result, out, sizeof(out)));
  const uint8_t expected[kResultSize] = {0x78, 0x56, 0x34, 0x12, 0x0B, 0x0A, 1};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, kResultSize);
  Result decoded = {0, 0, 0};
  TEST_ASSERT_TRUE(decodeResult(out, sizeof(out), &decoded));
  TEST_ASSERT_EQUAL_UINT32(0x12345678, decoded.hubBootId);
  TEST_ASSERT_EQUAL_UINT16(0x0A0B, decoded.number);
  TEST_ASSERT_EQUAL_UINT8(1, decoded.ok);
  out[6] = 2;
  TEST_ASSERT_FALSE(decodeResult(out, sizeof(out), &decoded));
  TEST_ASSERT_FALSE(decodeResult(out, 6, &decoded));
  const Result bad = {1, 1, 2};
  TEST_ASSERT_EQUAL_UINT(0, encodeResult(bad, out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT(0, encodeResult(result, out, 6));
}

void test_the_payload_length_of_every_type() {
  TEST_ASSERT_TRUE(payloadLengthFits(FrameType::Hello, 4));
  TEST_ASSERT_FALSE(payloadLengthFits(FrameType::Hello, 3));
  TEST_ASSERT_TRUE(payloadLengthFits(FrameType::HelloAck, 0));
  TEST_ASSERT_FALSE(payloadLengthFits(FrameType::HelloAck, 1));
  TEST_ASSERT_TRUE(payloadLengthFits(FrameType::PairRequest, 33));
  TEST_ASSERT_FALSE(payloadLengthFits(FrameType::PairRequest, 32));
  TEST_ASSERT_TRUE(payloadLengthFits(FrameType::PairAccept, 64));
  TEST_ASSERT_FALSE(payloadLengthFits(FrameType::PairAccept, 65));
  TEST_ASSERT_TRUE(payloadLengthFits(FrameType::Ack, 8));
  TEST_ASSERT_FALSE(payloadLengthFits(FrameType::Ack, 0));
  TEST_ASSERT_TRUE(payloadLengthFits(FrameType::Command, 11));
  TEST_ASSERT_FALSE(payloadLengthFits(FrameType::Command, 10));
  TEST_ASSERT_TRUE(payloadLengthFits(FrameType::Result, 7));
  TEST_ASSERT_FALSE(payloadLengthFits(FrameType::Result, 8));
}

void test_the_telemetry_length_needs_one_to_eight_whole_samples() {
  TEST_ASSERT_FALSE(payloadLengthFits(FrameType::Telemetry, 0));
  TEST_ASSERT_FALSE(payloadLengthFits(FrameType::Telemetry, 4));
  TEST_ASSERT_FALSE(payloadLengthFits(FrameType::Telemetry, 5));
  TEST_ASSERT_FALSE(payloadLengthFits(FrameType::Telemetry, 29));
  for (size_t samples = 1; samples <= kMaxSamplesPerFrame; samples++) {
    TEST_ASSERT_TRUE(payloadLengthFits(FrameType::Telemetry, kTelemetryPrefixSize + samples * kSampleSize));
    TEST_ASSERT_FALSE(payloadLengthFits(FrameType::Telemetry, kTelemetryPrefixSize + samples * kSampleSize + 1));
  }
  TEST_ASSERT_FALSE(payloadLengthFits(FrameType::Telemetry, kTelemetryPrefixSize + 9 * kSampleSize));
  TEST_ASSERT_FALSE(payloadLengthFits(FrameType::Telemetry, 100000));
}

void test_an_unknown_type_has_no_valid_length() {
  TEST_ASSERT_FALSE(payloadLengthFits(static_cast<FrameType>(0x99), 0));
}

void test_which_types_are_encrypted_and_which_way_they_travel() {
  TEST_ASSERT_FALSE(isEncryptedType(FrameType::Hello));
  TEST_ASSERT_FALSE(isEncryptedType(FrameType::HelloAck));
  TEST_ASSERT_FALSE(isEncryptedType(FrameType::PairRequest));
  TEST_ASSERT_FALSE(isEncryptedType(FrameType::PairAccept));
  TEST_ASSERT_TRUE(isEncryptedType(FrameType::Telemetry));
  TEST_ASSERT_TRUE(isEncryptedType(FrameType::Ack));
  TEST_ASSERT_TRUE(isEncryptedType(FrameType::Command));
  TEST_ASSERT_TRUE(isEncryptedType(FrameType::Result));

  const FrameType toHub[] = {FrameType::Hello, FrameType::PairRequest, FrameType::Telemetry, FrameType::Result};
  const FrameType toNode[] = {FrameType::HelloAck, FrameType::PairAccept, FrameType::Ack, FrameType::Command};
  for (FrameType type : toHub) {
    TEST_ASSERT_EQUAL_UINT8(0, static_cast<uint8_t>(directionOf(type)));
  }
  for (FrameType type : toNode) {
    TEST_ASSERT_EQUAL_UINT8(1, static_cast<uint8_t>(directionOf(type)));
  }
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_hello_layout_and_round_trip);
  RUN_TEST(test_hello_refuses_bad_input);
  RUN_TEST(test_pair_request_layout_and_refusals);
  RUN_TEST(test_pair_accept_holds_the_hub_key_then_the_node_key);
  RUN_TEST(test_ack_layout);
  RUN_TEST(test_command_layout_and_action_values);
  RUN_TEST(test_command_refuses_a_toggle_or_any_other_action);
  RUN_TEST(test_result_layout_and_ok_values);
  RUN_TEST(test_the_payload_length_of_every_type);
  RUN_TEST(test_the_telemetry_length_needs_one_to_eight_whole_samples);
  RUN_TEST(test_an_unknown_type_has_no_valid_length);
  RUN_TEST(test_which_types_are_encrypted_and_which_way_they_travel);
  return UNITY_END();
}
