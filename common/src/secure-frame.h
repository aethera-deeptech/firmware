#pragma once

#include <stddef.h>
#include <stdint.h>

#include "frame-codec.h"
#include "payloads.h"

namespace wattlink {

constexpr size_t kAesKeySize = 16;
constexpr size_t kMaxPayloadSize = kMaxFrameSize - kHeaderSize - kTagSize;

// AES-128-GCM is supplied by the caller. On the chip it wraps mbedTLS, and in the tests it wraps OpenSSL, so this code needs no library.
// The buffers of the plain text and the cipher text must not overlap. Both functions return false on any failure.
using AesGcmSeal = bool (*)(void *context, const uint8_t key[kAesKeySize], const uint8_t nonce[kNonceSize], const uint8_t *aad,
                            size_t aadLength, const uint8_t *plain, size_t length, uint8_t *cipher, uint8_t tag[kTagSize]);

// Must return false when the tag does not verify, and must not be trusted for the plain text then.
using AesGcmOpen = bool (*)(void *context, const uint8_t key[kAesKeySize], const uint8_t nonce[kNonceSize], const uint8_t *aad,
                            size_t aadLength, const uint8_t *cipher, size_t length, const uint8_t tag[kTagSize], uint8_t *plain);

// A frame without encryption (HELLO, HELLO_ACK, PAIR_REQUEST, PAIR_ACCEPT) is the header followed by the payload.
// Returns the size, or 0 when the version is not kProtocolVersion, the type is an encrypted one, the length does not fit the type or the
// buffer is too small.
size_t buildPlainFrame(const Header &header, const uint8_t *payload, size_t length, uint8_t *out, size_t capacity);

// Returns false for a short frame, an unknown version or type, an encrypted type or a payload length that does not fit the type.
// On success payload points into the frame.
bool parsePlainFrame(const uint8_t *frame, size_t length, Header *header, const uint8_t **payload, size_t *payloadLength);

// A frame of an encrypted type (TELEMETRY, ACK, COMMAND, RESULT) is the header, the cipher text and the tag of 16 bytes. The header is the
// associated data, and the nonce is the bootId, the counter and the direction of the type, as docs/espnow-protocol.md says. Because the
// direction comes from the type, a frame of one direction cannot be accepted as a frame of the other.
// Returns the size, or 0 when the version is not kProtocolVersion, the type is not an encrypted one, the payload length does not fit the
// type, the frame would be longer than kMaxFrameSize, the buffer is too small or the cipher fails.
size_t sealFrame(AesGcmSeal seal, void *context, const uint8_t key[kAesKeySize], const Header &header, const uint8_t *payload,
                 size_t length, uint8_t *out, size_t capacity);

// Checks the header, the type, the length and the tag, and decrypts. On success it fills the header and the payload and returns true.
// On any failure it returns false, leaves the header as it was and writes zeros into the payload buffer (up to kMaxPayloadSize bytes of it).
//
// The key serves both directions, so the caller says which direction it expects. A hub passes NodeToHub and accepts TELEMETRY and RESULT.
// A node passes HubToNode and accepts ACK and COMMAND. A frame of the other direction is refused, so a frame that a device sent itself
// cannot be played back to it.
//
// A true result proves that the key holder made the frame. It does not prove that the frame is fresh. The caller still has to do this.
// 1. Pick the key by the MAC of the sender.
// 2. Compare the bootId and the counter with what it kept for the peer, as the section Order and repeats of the protocol document says.
// 3. Write a new bootId to flash only after this returned true.
// 4. Check the fields that bind a frame to the receiver: the hubBootId of TELEMETRY and RESULT, the nodeBootId of ACK and the echo of COMMAND.
bool openFrame(AesGcmOpen open, void *context, const uint8_t key[kAesKeySize], Direction expected, const uint8_t *frame, size_t frameLength,
               Header *header, uint8_t *payload, size_t capacity, size_t *payloadLength);

}  // namespace wattlink
