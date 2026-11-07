#include "secure-frame.h"

#include <string.h>

namespace wattlink {

namespace {

bool headerIsValid(const Header &header) {
  return header.version == kProtocolVersion && isKnownFrameType(static_cast<uint8_t>(header.type));
}

}  // namespace

size_t buildPlainFrame(const Header &header, const uint8_t *payload, size_t length, uint8_t *out, size_t capacity) {
  if (!headerIsValid(header) || isEncryptedType(header.type) || !payloadLengthFits(header.type, length) ||
      capacity < kHeaderSize + length) {
    return 0;
  }
  if (!encodeHeader(header, out, capacity)) {
    return 0;
  }
  if (length > 0) {
    memcpy(out + kHeaderSize, payload, length);
  }
  return kHeaderSize + length;
}

bool parsePlainFrame(const uint8_t *frame, size_t length, Header *header, const uint8_t **payload, size_t *payloadLength) {
  Header parsed;
  if (!decodeHeader(frame, length, &parsed) || isEncryptedType(parsed.type)) {
    return false;
  }
  const size_t size = length - kHeaderSize;
  if (!payloadLengthFits(parsed.type, size)) {
    return false;
  }
  *header = parsed;
  *payload = frame + kHeaderSize;
  *payloadLength = size;
  return true;
}

size_t sealFrame(AesGcmSeal seal, void *context, const uint8_t key[kAesKeySize], const Header &header, const uint8_t *payload,
                 size_t length, uint8_t *out, size_t capacity) {
  if (!headerIsValid(header) || !isEncryptedType(header.type) || !payloadLengthFits(header.type, length)) {
    return 0;
  }
  const size_t total = kHeaderSize + length + kTagSize;
  if (total > kMaxFrameSize || capacity < total) {
    return 0;
  }
  if (!encodeHeader(header, out, capacity)) {
    return 0;
  }
  uint8_t nonce[kNonceSize];
  buildNonce(header.bootId, header.counter, directionOf(header.type), nonce);
  if (!seal(context, key, nonce, out, kHeaderSize, payload, length, out + kHeaderSize, out + kHeaderSize + length)) {
    memset(out, 0, total);
    return 0;
  }
  return total;
}

bool openFrame(AesGcmOpen open, void *context, const uint8_t key[kAesKeySize], const uint8_t *frame, size_t frameLength, Header *header,
               uint8_t *payload, size_t capacity, size_t *payloadLength) {
  Header parsed;
  if (frameLength > kMaxFrameSize || !decodeHeader(frame, frameLength, &parsed) || !isEncryptedType(parsed.type) ||
      frameLength < kHeaderSize + kTagSize) {
    return false;
  }
  const size_t size = frameLength - kHeaderSize - kTagSize;
  if (!payloadLengthFits(parsed.type, size) || capacity < size) {
    return false;
  }
  uint8_t nonce[kNonceSize];
  buildNonce(parsed.bootId, parsed.counter, directionOf(parsed.type), nonce);
  if (!open(context, key, nonce, frame, kHeaderSize, frame + kHeaderSize, size, frame + kHeaderSize + size, payload)) {
    if (size > 0) {
      memset(payload, 0, size);
    }
    return false;
  }
  *header = parsed;
  *payloadLength = size;
  return true;
}

}  // namespace wattlink
