#pragma once

#include <stddef.h>
#include <stdint.h>

#include "frame-codec.h"

namespace wattlink {

// The payloads of the frame types, as docs/espnow-protocol.md describes them in the table of frame types. The TELEMETRY payload is in
// frame-codec.h. Every decoder takes the exact length of the payload and refuses any other length, and it refuses a value outside the
// table (a kind that is not 1 or 2, an action or an ok that is not 0 or 1), so a damaged or hostile frame never reaches the logic.

enum class NodeKind : uint8_t {
  Plug = 1,
  Clamp = 2,
};

constexpr size_t kPublicKeyBytes = 32;

constexpr size_t kHelloSize = 4;
constexpr size_t kHelloAckSize = 0;
constexpr size_t kPairRequestSize = 1 + kPublicKeyBytes;
constexpr size_t kPairAcceptSize = 2 * kPublicKeyBytes;
constexpr size_t kAckSize = 8;
constexpr size_t kCommandSize = 11;
constexpr size_t kResultSize = 7;

struct Hello {
  NodeKind kind;
  uint8_t major;
  uint8_t minor;
  uint8_t patch;
};

struct PairRequest {
  NodeKind kind;
  uint8_t nodePublicKey[kPublicKeyBytes];
};

struct PairAccept {
  uint8_t hubPublicKey[kPublicKeyBytes];
  uint8_t nodePublicKey[kPublicKeyBytes];  // the key of the request that this answers
};

struct Ack {
  uint32_t nodeBootId;
  uint32_t highestSampleSeq;
};

struct Command {
  uint32_t echoBootId;  // the bootId and the counter of a TELEMETRY frame of the node
  uint32_t echoCounter;
  uint16_t number;
  uint8_t action;  // 1 on, 0 off, never a toggle
};

struct Result {
  uint32_t hubBootId;
  uint16_t number;
  uint8_t ok;  // 1 when the relay switched
};

// Each encoder returns the size it wrote, or 0 when the buffer is too small or a value is outside the table.
size_t encodeHello(const Hello &value, uint8_t *out, size_t capacity);
size_t encodePairRequest(const PairRequest &value, uint8_t *out, size_t capacity);
size_t encodePairAccept(const PairAccept &value, uint8_t *out, size_t capacity);
size_t encodeAck(const Ack &value, uint8_t *out, size_t capacity);
size_t encodeCommand(const Command &value, uint8_t *out, size_t capacity);
size_t encodeResult(const Result &value, uint8_t *out, size_t capacity);

// Each decoder returns false for a length that is not exact or a value outside the table, and then leaves the output as it was.
bool decodeHello(const uint8_t *in, size_t length, Hello *value);
bool decodePairRequest(const uint8_t *in, size_t length, PairRequest *value);
bool decodePairAccept(const uint8_t *in, size_t length, PairAccept *value);
bool decodeAck(const uint8_t *in, size_t length, Ack *value);
bool decodeCommand(const uint8_t *in, size_t length, Command *value);
bool decodeResult(const uint8_t *in, size_t length, Result *value);

// True when a payload of this length fits the type. TELEMETRY needs the prefix and 1 to kMaxSamplesPerFrame whole samples.
bool payloadLengthFits(FrameType type, size_t length);

// The types that carry a payload and a tag made by AES-GCM.
bool isEncryptedType(FrameType type);

// The direction in which a type travels, which is also the direction byte of its nonce.
Direction directionOf(FrameType type);

}  // namespace wattlink
