#include <string.h>
#include <unity.h>

#include "frame-codec.h"

using namespace wattlink;

namespace {

Sample exampleSample() {
  Sample sample;
  sample.sampleSeq = 0x04030201;
  sample.ageMs = 0x08070605;
  sample.voltageDeciV = 0x0A09;
  sample.currentCentiA = 0x0C0B;
  sample.powerDeciW = 0x100F0E0D;
  sample.energyWh = 0x14131211;
  sample.frequencyDeciHz = 0x1615;
  sample.powerFactorCenti = 0x17;
  sample.condition = 0x18;
  sample.relay = 0x19;
  return sample;
}

void assertSampleEqual(const Sample &expected, const Sample &actual) {
  TEST_ASSERT_EQUAL_UINT32(expected.sampleSeq, actual.sampleSeq);
  TEST_ASSERT_EQUAL_UINT32(expected.ageMs, actual.ageMs);
  TEST_ASSERT_EQUAL_UINT16(expected.voltageDeciV, actual.voltageDeciV);
  TEST_ASSERT_EQUAL_UINT16(expected.currentCentiA, actual.currentCentiA);
  TEST_ASSERT_EQUAL_UINT32(expected.powerDeciW, actual.powerDeciW);
  TEST_ASSERT_EQUAL_UINT32(expected.energyWh, actual.energyWh);
  TEST_ASSERT_EQUAL_UINT16(expected.frequencyDeciHz, actual.frequencyDeciHz);
  TEST_ASSERT_EQUAL_UINT8(expected.powerFactorCenti, actual.powerFactorCenti);
  TEST_ASSERT_EQUAL_UINT8(expected.condition, actual.condition);
  TEST_ASSERT_EQUAL_UINT8(expected.relay, actual.relay);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_header_bytes_are_little_endian() {
  const Header header = {kProtocolVersion, FrameType::Telemetry, 0x04030201, 0x08070605};
  uint8_t out[kHeaderSize];
  TEST_ASSERT_TRUE(encodeHeader(header, out, sizeof(out)));
  const uint8_t expected[kHeaderSize] = {0x01, 0x20, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, kHeaderSize);
}

void test_header_round_trip() {
  const Header header = {kProtocolVersion, FrameType::Command, 0xA1B2C3D4u, 0x0000FFFFu};
  uint8_t out[kHeaderSize];
  TEST_ASSERT_TRUE(encodeHeader(header, out, sizeof(out)));
  Header decoded;
  TEST_ASSERT_TRUE(decodeHeader(out, sizeof(out), &decoded));
  TEST_ASSERT_EQUAL_UINT8(kProtocolVersion, decoded.version);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(FrameType::Command), static_cast<uint8_t>(decoded.type));
  TEST_ASSERT_EQUAL_UINT32(0xA1B2C3D4u, decoded.bootId);
  TEST_ASSERT_EQUAL_UINT32(0x0000FFFFu, decoded.counter);
}

void test_every_frame_type_of_the_table_is_known() {
  const uint8_t known[] = {0x01, 0x02, 0x10, 0x11, 0x20, 0x21, 0x30, 0x31};
  for (uint8_t value : known) {
    TEST_ASSERT_TRUE(isKnownFrameType(value));
  }
  TEST_ASSERT_FALSE(isKnownFrameType(0x00));
  TEST_ASSERT_FALSE(isKnownFrameType(0x03));
  TEST_ASSERT_FALSE(isKnownFrameType(0xFF));
}

void test_encode_header_refuses_a_small_buffer() {
  const Header header = {kProtocolVersion, FrameType::Hello, 1, 1};
  uint8_t out[kHeaderSize];
  TEST_ASSERT_FALSE(encodeHeader(header, out, kHeaderSize - 1));
}

void test_encode_header_refuses_an_unknown_type() {
  const Header header = {kProtocolVersion, static_cast<FrameType>(0x99), 1, 1};
  uint8_t out[kHeaderSize];
  TEST_ASSERT_FALSE(encodeHeader(header, out, sizeof(out)));
}

void test_decode_header_refuses_bad_input() {
  const uint8_t good[kHeaderSize] = {0x01, 0x20, 0, 0, 0, 0, 0, 0, 0, 0};
  Header header;
  TEST_ASSERT_TRUE(decodeHeader(good, sizeof(good), &header));
  TEST_ASSERT_FALSE(decodeHeader(good, kHeaderSize - 1, &header));

  uint8_t wrongVersion[kHeaderSize];
  memcpy(wrongVersion, good, sizeof(good));
  wrongVersion[0] = 2;
  TEST_ASSERT_FALSE(decodeHeader(wrongVersion, sizeof(wrongVersion), &header));

  uint8_t unknownType[kHeaderSize];
  memcpy(unknownType, good, sizeof(good));
  unknownType[1] = 0x99;
  TEST_ASSERT_FALSE(decodeHeader(unknownType, sizeof(unknownType), &header));

  unknownType[1] = 0x00;
  TEST_ASSERT_FALSE(decodeHeader(unknownType, sizeof(unknownType), &header));
}

void test_nonce_layout() {
  uint8_t nonce[kNonceSize];
  buildNonce(0x04030201, 0x08070605, Direction::HubToNode, nonce);
  const uint8_t expected[kNonceSize] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x01, 0x00, 0x00, 0x00};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, nonce, kNonceSize);
}

void test_nonce_differs_by_direction() {
  uint8_t toHub[kNonceSize];
  uint8_t toNode[kNonceSize];
  buildNonce(7, 9, Direction::NodeToHub, toHub);
  buildNonce(7, 9, Direction::HubToNode, toNode);
  TEST_ASSERT_NOT_EQUAL(0, memcmp(toHub, toNode, kNonceSize));
  TEST_ASSERT_EQUAL_UINT8(0, toHub[8]);
  TEST_ASSERT_EQUAL_UINT8(1, toNode[8]);
}

void test_nonce_differs_by_boot_number_and_counter() {
  uint8_t a[kNonceSize];
  uint8_t b[kNonceSize];
  uint8_t c[kNonceSize];
  buildNonce(1, 5, Direction::NodeToHub, a);
  buildNonce(2, 5, Direction::NodeToHub, b);
  buildNonce(1, 6, Direction::NodeToHub, c);
  TEST_ASSERT_NOT_EQUAL(0, memcmp(a, b, kNonceSize));
  TEST_ASSERT_NOT_EQUAL(0, memcmp(a, c, kNonceSize));
}

void test_sample_bytes_follow_the_table() {
  uint8_t out[kSampleSize];
  encodeSample(exampleSample(), out);
  uint8_t expected[kSampleSize];
  for (size_t i = 0; i < kSampleSize; i++) {
    expected[i] = static_cast<uint8_t>(i + 1);
  }
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, out, kSampleSize);
}

void test_sample_round_trip_keeps_the_markers_of_missing_fields() {
  Sample sample = exampleSample();
  sample.voltageDeciV = kNoU16;
  sample.currentCentiA = kNoU16;
  sample.energyWh = kNoU32;
  sample.frequencyDeciHz = kNoU16;
  sample.powerFactorCenti = kNoU8;
  sample.relay = kNoU8;
  uint8_t out[kSampleSize];
  encodeSample(sample, out);
  Sample decoded;
  decodeSample(out, &decoded);
  assertSampleEqual(sample, decoded);
}

void test_the_largest_values_survive() {
  Sample sample = {kNoU32, kNoU32, kNoU16, kNoU16, kNoU32, kNoU32, kNoU16, kNoU8, 3, 1};
  uint8_t out[kSampleSize];
  encodeSample(sample, out);
  Sample decoded;
  decodeSample(out, &decoded);
  assertSampleEqual(sample, decoded);
}

void test_telemetry_payload_layout() {
  const Sample samples[1] = {exampleSample()};
  uint8_t out[kTelemetryPrefixSize + kSampleSize];
  TEST_ASSERT_EQUAL_UINT(kTelemetryPrefixSize + kSampleSize, encodeTelemetryPayload(0x44332211, samples, 1, out, sizeof(out)));
  const uint8_t prefix[kTelemetryPrefixSize] = {0x11, 0x22, 0x33, 0x44, 0x01};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(prefix, out, kTelemetryPrefixSize);
  uint8_t sampleBytes[kSampleSize];
  encodeSample(samples[0], sampleBytes);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(sampleBytes, out + kTelemetryPrefixSize, kSampleSize);
}

void test_telemetry_payload_round_trip() {
  Sample samples[3];
  for (size_t i = 0; i < 3; i++) {
    samples[i] = exampleSample();
    samples[i].sampleSeq = static_cast<uint32_t>(100 + i);
    samples[i].powerDeciW = static_cast<uint32_t>(2000 + i);
  }
  uint8_t out[kTelemetryPrefixSize + 3 * kSampleSize];
  const size_t size = encodeTelemetryPayload(77, samples, 3, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT(sizeof(out), size);

  Sample decoded[kMaxSamplesPerFrame];
  uint32_t hubBootId = 0;
  size_t count = 0;
  TEST_ASSERT_TRUE(decodeTelemetryPayload(out, size, &hubBootId, decoded, kMaxSamplesPerFrame, &count));
  TEST_ASSERT_EQUAL_UINT32(77, hubBootId);
  TEST_ASSERT_EQUAL_UINT(3, count);
  for (size_t i = 0; i < 3; i++) {
    assertSampleEqual(samples[i], decoded[i]);
  }
}

void test_a_full_telemetry_frame_fits_one_esp_now_frame() {
  Sample samples[kMaxSamplesPerFrame];
  for (size_t i = 0; i < kMaxSamplesPerFrame; i++) {
    samples[i] = exampleSample();
  }
  uint8_t out[kTelemetryPrefixSize + kMaxSamplesPerFrame * kSampleSize];
  const size_t size = encodeTelemetryPayload(1, samples, kMaxSamplesPerFrame, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT(205, size);
  TEST_ASSERT_EQUAL_UINT(231, kHeaderSize + size + kTagSize);
  TEST_ASSERT_TRUE(kHeaderSize + size + kTagSize <= kMaxFrameSize);
}

void test_encode_telemetry_refuses_a_bad_count_and_a_small_buffer() {
  Sample samples[kMaxSamplesPerFrame + 1];
  for (size_t i = 0; i < kMaxSamplesPerFrame + 1; i++) {
    samples[i] = exampleSample();
  }
  uint8_t out[kTelemetryPrefixSize + (kMaxSamplesPerFrame + 1) * kSampleSize];
  TEST_ASSERT_EQUAL_UINT(0, encodeTelemetryPayload(1, samples, 0, out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT(0, encodeTelemetryPayload(1, samples, kMaxSamplesPerFrame + 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_UINT(0, encodeTelemetryPayload(1, samples, 2, out, kTelemetryPrefixSize + kSampleSize));
}

void test_decode_telemetry_refuses_bad_input() {
  Sample samples[2] = {exampleSample(), exampleSample()};
  uint8_t out[kTelemetryPrefixSize + 2 * kSampleSize];
  const size_t size = encodeTelemetryPayload(5, samples, 2, out, sizeof(out));
  TEST_ASSERT_EQUAL_UINT(sizeof(out), size);

  Sample decoded[kMaxSamplesPerFrame];
  uint32_t hubBootId = 0;
  size_t count = 0;
  TEST_ASSERT_TRUE(decodeTelemetryPayload(out, size, &hubBootId, decoded, kMaxSamplesPerFrame, &count));

  TEST_ASSERT_FALSE(decodeTelemetryPayload(out, size - 1, &hubBootId, decoded, kMaxSamplesPerFrame, &count));
  TEST_ASSERT_FALSE(decodeTelemetryPayload(out, kTelemetryPrefixSize - 1, &hubBootId, decoded, kMaxSamplesPerFrame, &count));
  TEST_ASSERT_FALSE(decodeTelemetryPayload(out, size, &hubBootId, decoded, 1, &count));

  uint8_t zeroCount[sizeof(out)];
  memcpy(zeroCount, out, sizeof(out));
  zeroCount[4] = 0;
  TEST_ASSERT_FALSE(decodeTelemetryPayload(zeroCount, size, &hubBootId, decoded, kMaxSamplesPerFrame, &count));

  uint8_t nineCount[sizeof(out)];
  memcpy(nineCount, out, sizeof(out));
  nineCount[4] = kMaxSamplesPerFrame + 1;
  TEST_ASSERT_FALSE(decodeTelemetryPayload(nineCount, size, &hubBootId, decoded, kMaxSamplesPerFrame, &count));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_header_bytes_are_little_endian);
  RUN_TEST(test_header_round_trip);
  RUN_TEST(test_every_frame_type_of_the_table_is_known);
  RUN_TEST(test_encode_header_refuses_a_small_buffer);
  RUN_TEST(test_encode_header_refuses_an_unknown_type);
  RUN_TEST(test_decode_header_refuses_bad_input);
  RUN_TEST(test_nonce_layout);
  RUN_TEST(test_nonce_differs_by_direction);
  RUN_TEST(test_nonce_differs_by_boot_number_and_counter);
  RUN_TEST(test_sample_bytes_follow_the_table);
  RUN_TEST(test_sample_round_trip_keeps_the_markers_of_missing_fields);
  RUN_TEST(test_the_largest_values_survive);
  RUN_TEST(test_telemetry_payload_layout);
  RUN_TEST(test_telemetry_payload_round_trip);
  RUN_TEST(test_a_full_telemetry_frame_fits_one_esp_now_frame);
  RUN_TEST(test_encode_telemetry_refuses_a_bad_count_and_a_small_buffer);
  RUN_TEST(test_decode_telemetry_refuses_bad_input);
  return UNITY_END();
}
