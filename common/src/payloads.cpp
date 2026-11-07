#include "payloads.h"

#include <string.h>

#include "byte-order.h"

namespace wattlink {

namespace {

bool validKind(uint8_t value) {
  return value == static_cast<uint8_t>(NodeKind::Plug) || value == static_cast<uint8_t>(NodeKind::Clamp);
}

bool isBit(uint8_t value) {
  return value <= 1;
}

}  // namespace

size_t encodeHello(const Hello &value, uint8_t *out, size_t capacity) {
  if (capacity < kHelloSize || !validKind(static_cast<uint8_t>(value.kind))) {
    return 0;
  }
  out[0] = static_cast<uint8_t>(value.kind);
  out[1] = value.major;
  out[2] = value.minor;
  out[3] = value.patch;
  return kHelloSize;
}

bool decodeHello(const uint8_t *in, size_t length, Hello *value) {
  if (length != kHelloSize || !validKind(in[0])) {
    return false;
  }
  value->kind = static_cast<NodeKind>(in[0]);
  value->major = in[1];
  value->minor = in[2];
  value->patch = in[3];
  return true;
}

size_t encodePairRequest(const PairRequest &value, uint8_t *out, size_t capacity) {
  if (capacity < kPairRequestSize || !validKind(static_cast<uint8_t>(value.kind))) {
    return 0;
  }
  out[0] = static_cast<uint8_t>(value.kind);
  memcpy(out + 1, value.nodePublicKey, kPublicKeyBytes);
  return kPairRequestSize;
}

bool decodePairRequest(const uint8_t *in, size_t length, PairRequest *value) {
  if (length != kPairRequestSize || !validKind(in[0])) {
    return false;
  }
  value->kind = static_cast<NodeKind>(in[0]);
  memcpy(value->nodePublicKey, in + 1, kPublicKeyBytes);
  return true;
}

size_t encodePairAccept(const PairAccept &value, uint8_t *out, size_t capacity) {
  if (capacity < kPairAcceptSize) {
    return 0;
  }
  memcpy(out, value.hubPublicKey, kPublicKeyBytes);
  memcpy(out + kPublicKeyBytes, value.nodePublicKey, kPublicKeyBytes);
  return kPairAcceptSize;
}

bool decodePairAccept(const uint8_t *in, size_t length, PairAccept *value) {
  if (length != kPairAcceptSize) {
    return false;
  }
  memcpy(value->hubPublicKey, in, kPublicKeyBytes);
  memcpy(value->nodePublicKey, in + kPublicKeyBytes, kPublicKeyBytes);
  return true;
}

size_t encodeAck(const Ack &value, uint8_t *out, size_t capacity) {
  if (capacity < kAckSize) {
    return 0;
  }
  putU32(out, value.nodeBootId);
  putU32(out + 4, value.highestSampleSeq);
  return kAckSize;
}

bool decodeAck(const uint8_t *in, size_t length, Ack *value) {
  if (length != kAckSize) {
    return false;
  }
  value->nodeBootId = getU32(in);
  value->highestSampleSeq = getU32(in + 4);
  return true;
}

size_t encodeCommand(const Command &value, uint8_t *out, size_t capacity) {
  if (capacity < kCommandSize || !isBit(value.action)) {
    return 0;
  }
  putU32(out, value.echoBootId);
  putU32(out + 4, value.echoCounter);
  putU16(out + 8, value.number);
  out[10] = value.action;
  return kCommandSize;
}

bool decodeCommand(const uint8_t *in, size_t length, Command *value) {
  if (length != kCommandSize || !isBit(in[10])) {
    return false;
  }
  value->echoBootId = getU32(in);
  value->echoCounter = getU32(in + 4);
  value->number = getU16(in + 8);
  value->action = in[10];
  return true;
}

size_t encodeResult(const Result &value, uint8_t *out, size_t capacity) {
  if (capacity < kResultSize || !isBit(value.ok)) {
    return 0;
  }
  putU32(out, value.hubBootId);
  putU16(out + 4, value.number);
  out[6] = value.ok;
  return kResultSize;
}

bool decodeResult(const uint8_t *in, size_t length, Result *value) {
  if (length != kResultSize || !isBit(in[6])) {
    return false;
  }
  value->hubBootId = getU32(in);
  value->number = getU16(in + 4);
  value->ok = in[6];
  return true;
}

bool payloadLengthFits(FrameType type, size_t length) {
  switch (type) {
    case FrameType::Hello:
      return length == kHelloSize;
    case FrameType::HelloAck:
      return length == kHelloAckSize;
    case FrameType::PairRequest:
      return length == kPairRequestSize;
    case FrameType::PairAccept:
      return length == kPairAcceptSize;
    case FrameType::Ack:
      return length == kAckSize;
    case FrameType::Command:
      return length == kCommandSize;
    case FrameType::Result:
      return length == kResultSize;
    case FrameType::Telemetry:
      if (length < kTelemetryPrefixSize + kSampleSize) {
        return false;
      }
      return (length - kTelemetryPrefixSize) % kSampleSize == 0 && (length - kTelemetryPrefixSize) / kSampleSize <= kMaxSamplesPerFrame;
  }
  return false;
}

bool isEncryptedType(FrameType type) {
  return type == FrameType::Telemetry || type == FrameType::Ack || type == FrameType::Command || type == FrameType::Result;
}

Direction directionOf(FrameType type) {
  switch (type) {
    case FrameType::Hello:
    case FrameType::PairRequest:
    case FrameType::Telemetry:
    case FrameType::Result:
      return Direction::NodeToHub;
    case FrameType::HelloAck:
    case FrameType::PairAccept:
    case FrameType::Ack:
    case FrameType::Command:
      return Direction::HubToNode;
  }
  return Direction::NodeToHub;
}

}  // namespace wattlink
