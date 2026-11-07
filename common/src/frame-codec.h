#pragma once

#include <stddef.h>
#include <stdint.h>

namespace wattlink {

// The layout is described in docs/espnow-protocol.md, in the sections Frame layout and Sample.
constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kHeaderSize = 10;
constexpr size_t kTagSize = 16;
constexpr size_t kSampleSize = 25;
constexpr size_t kMaxSamplesPerFrame = 8;
constexpr size_t kTelemetryPrefixSize = 5;
constexpr size_t kNonceSize = 12;
constexpr size_t kMaxFrameSize = 250;

static_assert(kHeaderSize + kTelemetryPrefixSize + kMaxSamplesPerFrame * kSampleSize + kTagSize <= kMaxFrameSize,
              "a full telemetry frame must fit in one ESP-NOW frame");

// A field that the node cannot measure is sent with every bit set.
constexpr uint8_t kNoU8 = 0xFF;
constexpr uint16_t kNoU16 = 0xFFFF;
constexpr uint32_t kNoU32 = 0xFFFFFFFFu;

enum class FrameType : uint8_t {
  Hello = 0x01,
  HelloAck = 0x02,
  PairRequest = 0x10,
  PairAccept = 0x11,
  Telemetry = 0x20,
  Ack = 0x21,
  Command = 0x30,
  Result = 0x31,
};

enum class Direction : uint8_t {
  NodeToHub = 0,
  HubToNode = 1,
};

struct Header {
  uint8_t version;
  FrameType type;
  uint32_t bootId;
  uint32_t counter;
};

struct Sample {
  uint32_t sampleSeq;
  uint32_t ageMs;
  uint16_t voltageDeciV;
  uint16_t currentCentiA;
  uint32_t powerDeciW;
  uint32_t energyWh;
  uint16_t frequencyDeciHz;
  uint8_t powerFactorCenti;
  uint8_t condition;
  uint8_t relay;
};

bool isKnownFrameType(uint8_t value);

// Writes kHeaderSize bytes. Returns false when the buffer is too small or the type is unknown.
bool encodeHeader(const Header &header, uint8_t *out, size_t capacity);

// Returns false for a short buffer, a version other than kProtocolVersion or an unknown type.
bool decodeHeader(const uint8_t *in, size_t length, Header *header);

// The nonce of AES-GCM is the boot number, the counter, the direction and three zero bytes.
void buildNonce(uint32_t bootId, uint32_t counter, Direction direction, uint8_t out[kNonceSize]);

void encodeSample(const Sample &sample, uint8_t out[kSampleSize]);
void decodeSample(const uint8_t in[kSampleSize], Sample *sample);

// The payload of a TELEMETRY frame before it is encrypted. It holds the boot number of the hub as the node last saw it,
// the number of samples and the samples. Returns the size, or 0 for a count outside 1 to kMaxSamplesPerFrame or a small buffer.
size_t encodeTelemetryPayload(uint32_t hubBootId, const Sample *samples, size_t count, uint8_t *out, size_t capacity);

// Returns false when the length does not fit the count, or the count is outside 1 to kMaxSamplesPerFrame or above maxSamples.
bool decodeTelemetryPayload(const uint8_t *in, size_t length, uint32_t *hubBootId, Sample *samples, size_t maxSamples,
                            size_t *count);

}  // namespace wattlink
