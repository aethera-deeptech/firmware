#include "frame-codec.h"

#include "byte-order.h"

namespace wattlink {

bool isKnownFrameType(uint8_t value) {
  switch (static_cast<FrameType>(value)) {
    case FrameType::Hello:
    case FrameType::HelloAck:
    case FrameType::PairRequest:
    case FrameType::PairAccept:
    case FrameType::Telemetry:
    case FrameType::Ack:
    case FrameType::Command:
    case FrameType::Result:
      return true;
  }
  return false;
}

bool encodeHeader(const Header &header, uint8_t *out, size_t capacity) {
  if (capacity < kHeaderSize || !isKnownFrameType(static_cast<uint8_t>(header.type))) {
    return false;
  }
  out[0] = header.version;
  out[1] = static_cast<uint8_t>(header.type);
  putU32(out + 2, header.bootId);
  putU32(out + 6, header.counter);
  return true;
}

bool decodeHeader(const uint8_t *in, size_t length, Header *header) {
  if (length < kHeaderSize || in[0] != kProtocolVersion || !isKnownFrameType(in[1])) {
    return false;
  }
  header->version = in[0];
  header->type = static_cast<FrameType>(in[1]);
  header->bootId = getU32(in + 2);
  header->counter = getU32(in + 6);
  return true;
}

void buildNonce(uint32_t bootId, uint32_t counter, Direction direction, uint8_t out[kNonceSize]) {
  putU32(out, bootId);
  putU32(out + 4, counter);
  out[8] = static_cast<uint8_t>(direction);
  out[9] = 0;
  out[10] = 0;
  out[11] = 0;
}

void encodeSample(const Sample &sample, uint8_t out[kSampleSize]) {
  putU32(out, sample.sampleSeq);
  putU32(out + 4, sample.ageMs);
  putU16(out + 8, sample.voltageDeciV);
  putU16(out + 10, sample.currentCentiA);
  putU32(out + 12, sample.powerDeciW);
  putU32(out + 16, sample.energyWh);
  putU16(out + 20, sample.frequencyDeciHz);
  out[22] = sample.powerFactorCenti;
  out[23] = sample.condition;
  out[24] = sample.relay;
}

void decodeSample(const uint8_t in[kSampleSize], Sample *sample) {
  sample->sampleSeq = getU32(in);
  sample->ageMs = getU32(in + 4);
  sample->voltageDeciV = getU16(in + 8);
  sample->currentCentiA = getU16(in + 10);
  sample->powerDeciW = getU32(in + 12);
  sample->energyWh = getU32(in + 16);
  sample->frequencyDeciHz = getU16(in + 20);
  sample->powerFactorCenti = in[22];
  sample->condition = in[23];
  sample->relay = in[24];
}

size_t encodeTelemetryPayload(uint32_t hubBootId, const Sample *samples, size_t count, uint8_t *out, size_t capacity) {
  if (count < 1 || count > kMaxSamplesPerFrame) {
    return 0;
  }
  const size_t size = kTelemetryPrefixSize + count * kSampleSize;
  if (capacity < size) {
    return 0;
  }
  putU32(out, hubBootId);
  out[4] = static_cast<uint8_t>(count);
  for (size_t i = 0; i < count; i++) {
    encodeSample(samples[i], out + kTelemetryPrefixSize + i * kSampleSize);
  }
  return size;
}

bool decodeTelemetryPayload(const uint8_t *in, size_t length, uint32_t *hubBootId, Sample *samples, size_t maxSamples,
                            size_t *count) {
  if (length < kTelemetryPrefixSize) {
    return false;
  }
  const size_t declared = in[4];
  if (declared < 1 || declared > kMaxSamplesPerFrame || declared > maxSamples ||
      length != kTelemetryPrefixSize + declared * kSampleSize) {
    return false;
  }
  *hubBootId = getU32(in);
  for (size_t i = 0; i < declared; i++) {
    decodeSample(in + kTelemetryPrefixSize + i * kSampleSize, &samples[i]);
  }
  *count = declared;
  return true;
}

}  // namespace wattlink
