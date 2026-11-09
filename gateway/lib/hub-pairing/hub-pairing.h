#pragma once

#include <stddef.h>
#include <stdint.h>

#include "node-registry.h"
#include "pairing-key.h"
#include "pairing-window.h"
#include "payloads.h"
#include "secure-frame.h"

namespace wattlink {

// The key in a PAIR_REQUEST and the key of the exchange are the same 32 bytes, and the code passes one where the other is expected.
static_assert(kPublicKeyBytes == kPublicKeySize, "the public key of the frames is the public key of the exchange");

// The steps of the hub in a pairing, as the section "Pairing and keys" of docs/espnow-protocol.md describes them. The class does the logic only.
// The radio, the display and the flash are not here, so the same code runs on the host in the tests and on the chip.
// One task must call all of it. Never call it from the ESP-NOW receive callback, because the registry writes flash and the crypto needs a big stack.

// X25519 is supplied by the caller. On the chip it wraps mbedTLS, and in the tests it is a fake, so this code needs no library.
// generate makes a new private key and its public key. agree makes the shared secret. Both return false on any failure.
using X25519Generate = bool (*)(void *context, uint8_t privateKey[kPublicKeySize], uint8_t publicKey[kPublicKeySize]);
using X25519Agree = bool (*)(void *context, const uint8_t privateKey[kPublicKeySize], const uint8_t peerPublicKey[kPublicKeySize],
                             uint8_t shared[kSharedSecretSize]);

// Everything that the pairing needs from the crypto library. One context is passed to every function.
struct HubCrypto {
  void *context;
  HmacSha256 hmac;
  AesGcmOpen open;
  AesGcmSeal seal;  // HubPairing does not seal, and HubLink does
  X25519Generate generate;
  X25519Agree agree;
};

enum class RequestResult : uint8_t {
  Accepted,      // send PAIR_ACCEPT to the node with the payload in the outcome
  NotOpen,       // the window is closed, so the frame is ignored
  Busy,          // another node is being paired, so the frame is ignored and that pairing goes on
  Malformed,     // the payload is not a PAIR_REQUEST, so the frame is ignored and the window stays open
  Refused,       // the registry says no, and the outcome names the reason, so the hub can show it
  CryptoFailed,  // the key exchange failed, for example an all zero shared secret, and nothing was changed
};

// What the caller needs besides the result. It is always filled, with empty values when they do not apply.
struct RequestOutcome {
  PairResult refusal;                      // the reason when the result is Refused, and Ok otherwise
  char nodeId[kNodeIdSize];                // for the display, set when the result is Accepted
  uint8_t acceptPayload[kPairAcceptSize];  // the payload of PAIR_ACCEPT, set when the result is Accepted
};

enum class FirstFrameResult : uint8_t {
  Committed,      // the node is paired and the frame is fresh, so send the ACK and treat the frame as received TELEMETRY
  NotPending,     // no pairing waits for this node, or its time is over, so treat the frame as a frame of a paired node
  BadFrame,       // the frame does not prove the new key, so no ACK, and the pairing waits on for a good frame until its time is over
  PairFailed,     // the registry refused or could not store the pairing, so no ACK, the window is closed and nothing is paired
  PairedNoFrame,  // the node is paired, but the frame was not accepted (the boot value could not be stored), so no ACK, and the node sends again
};

// Filled by onFrame. The header and the payload hold something only when the result is Committed, and they are cleared for every other result.
struct FirstFrame {
  Header header;
  uint8_t payload[kMaxPayloadSize];
  size_t payloadLength;
  PairResult pairResult;  // set when the result is PairFailed
};

class HubPairing {
 public:
  // hubBootId is the boot number of the hub for this start. The first TELEMETRY frame must carry it, because a node that took it from the header of
  // PAIR_ACCEPT knows it, and a frame that does not is not committed.
  HubPairing(PairingWindow &window, NodeRegistry &registry, const HubCrypto &crypto, const uint8_t hubMac[kMacSize], uint32_t hubBootId);
  ~HubPairing();

  // A copy would keep a second copy of the pending key that close() and update() cannot wipe.
  HubPairing(const HubPairing &) = delete;
  HubPairing &operator=(const HubPairing &) = delete;

  // A PAIR_REQUEST arrived from the radio address mac. The payload is the payload of the frame. The caller sends PAIR_ACCEPT for Accepted.
  // Nothing is stored in the registry yet. The window moves to the state Pairing, and the hub waits for the first TELEMETRY frame.
  RequestResult onPairRequest(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *payload, size_t length, RequestOutcome *outcome);

  // A frame of an encrypted type arrived from the radio address mac. The caller may offer every such frame, because only a frame of the node
  // that is pending is looked at. The hub pairs the node before it answers, so a frame that came back as Committed is stored as paired
  // and the ACK is safe to send. The caller still decodes the payload of the frame and checks its fields, as openFrame says.
  FirstFrameResult onFrame(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *frame, size_t frameLength, FirstFrame *out);

  // Wipes the pending key when the window is not in the pairing state any more. Call it in every turn of the loop.
  void update(uint32_t nowMs);

  // Closes the window and wipes the pending key, for example when the portal starts.
  void close();

  bool pending() const { return pending_; }

 private:
  void wipePending();

  PairingWindow &window_;
  NodeRegistry &registry_;
  HubCrypto crypto_;
  uint8_t hubMac_[kMacSize];
  uint32_t hubBootId_;

  bool pending_;
  uint8_t pendingMac_[kMacSize];
  uint8_t pendingKind_;
  uint8_t pendingKey_[kPairingKeySize];
};

}  // namespace wattlink
