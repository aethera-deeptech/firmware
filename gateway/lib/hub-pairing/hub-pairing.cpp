#include "hub-pairing.h"

#include <string.h>

#include "byte-order.h"
#include "secure-wipe.h"

namespace wattlink {

HubPairing::HubPairing(PairingWindow &window, NodeRegistry &registry, const HubCrypto &crypto, const uint8_t hubMac[kMacSize],
                       uint32_t hubBootId)
    : window_(window), registry_(registry), crypto_(crypto), hubBootId_(hubBootId), pending_(false), pendingKind_(0) {
  memcpy(hubMac_, hubMac, kMacSize);
  memset(pendingMac_, 0, sizeof(pendingMac_));
  memset(pendingKey_, 0, sizeof(pendingKey_));
}

HubPairing::~HubPairing() {
  wipePending();
}

void HubPairing::wipePending() {
  pending_ = false;
  pendingKind_ = 0;
  wipe(pendingMac_, sizeof(pendingMac_));
  wipe(pendingKey_, sizeof(pendingKey_));
}

void HubPairing::update(uint32_t nowMs) {
  window_.update(nowMs);
  if (pending_ && !window_.isPairing(nowMs)) {
    wipePending();
  }
}

void HubPairing::close() {
  window_.close();
  wipePending();
}

RequestResult HubPairing::onPairRequest(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *payload, size_t length,
                                        RequestOutcome *outcome) {
  outcome->refusal = PairResult::Ok;
  outcome->nodeId[0] = '\0';
  memset(outcome->acceptPayload, 0, sizeof(outcome->acceptPayload));

  update(nowMs);
  if (window_.isPairing(nowMs)) {
    return RequestResult::Busy;
  }
  if (!window_.isOpen(nowMs)) {
    return RequestResult::NotOpen;
  }

  PairRequest request;
  if (!decodePairRequest(payload, length, &request)) {
    return RequestResult::Malformed;
  }

  // The registry is asked first, so a node that is known is refused before any key is made.
  const uint8_t kind = static_cast<uint8_t>(request.kind);
  const PairResult can = registry_.canPair(kind, mac);
  if (can != PairResult::Ok) {
    outcome->refusal = can;
    return RequestResult::Refused;
  }

  uint8_t privateKey[kPublicKeySize] = {0};
  uint8_t hubPublic[kPublicKeySize] = {0};
  uint8_t shared[kSharedSecretSize] = {0};
  uint8_t key[kPairingKeySize] = {0};
  // derivePairingKey refuses an all zero shared secret, as the protocol says.
  const bool made = crypto_.generate(crypto_.context, privateKey, hubPublic) &&
                    crypto_.agree(crypto_.context, privateKey, request.nodePublicKey, shared) &&
                    derivePairingKey(crypto_.hmac, crypto_.context, shared, request.nodePublicKey, hubPublic, mac, hubMac_, key);
  wipe(privateKey, sizeof(privateKey));
  wipe(shared, sizeof(shared));

  PairAccept accept;
  memcpy(accept.hubPublicKey, hubPublic, kPublicKeySize);
  memcpy(accept.nodePublicKey, request.nodePublicKey, kPublicKeySize);
  const bool encoded = made && encodePairAccept(accept, outcome->acceptPayload, sizeof(outcome->acceptPayload)) == kPairAcceptSize;
  // The window is changed last, so a failure above leaves it open for the owner to try again.
  if (!encoded || !window_.beginPairing(nowMs)) {
    wipe(key, sizeof(key));
    memset(outcome->acceptPayload, 0, sizeof(outcome->acceptPayload));
    return RequestResult::CryptoFailed;
  }

  pending_ = true;
  pendingKind_ = kind;
  memcpy(pendingMac_, mac, kMacSize);
  memcpy(pendingKey_, key, kPairingKeySize);
  wipe(key, sizeof(key));
  makeNodeId(kind, mac, outcome->nodeId);
  return RequestResult::Accepted;
}

FirstFrameResult HubPairing::onFrame(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *frame, size_t frameLength,
                                     FirstFrame *out) {
  out->pairResult = PairResult::Ok;
  memset(&out->header, 0, sizeof(out->header));
  out->payloadLength = 0;
  memset(out->payload, 0, sizeof(out->payload));

  update(nowMs);
  if (!pending_ || !window_.isPairing(nowMs) || memcmp(mac, pendingMac_, kMacSize) != 0) {
    return FirstFrameResult::NotPending;
  }

  // The tag proves that the sender holds the new key. A frame that fails here changes nothing, so a stranger cannot cancel the pairing.
  Header header;
  size_t payloadLength = 0;
  if (!openFrame(crypto_.open, crypto_.context, pendingKey_, Direction::NodeToHub, frame, frameLength, &header, out->payload,
                 sizeof(out->payload), &payloadLength) ||
      header.type != FrameType::Telemetry || header.bootId == 0 || payloadLength < sizeof(uint32_t) || getU32(out->payload) != hubBootId_) {
    memset(out->payload, 0, sizeof(out->payload));
    return FirstFrameResult::BadFrame;
  }

  // One pairing is all that the window allows, so it closes now. The node is stored as paired BEFORE the caller sends the ACK, because
  // a node that commits a key that the hub never stored would be cut off for good.
  window_.commit(nowMs);
  const PairResult paired = registry_.pair(pendingKind_, mac, pendingKey_);
  wipePending();
  if (paired != PairResult::Ok) {
    out->pairResult = paired;
    memset(out->payload, 0, sizeof(out->payload));
    return FirstFrameResult::PairFailed;
  }
  if (registry_.accept(mac, header.bootId, header.counter) != FrameCheck::Accepted) {
    memset(out->payload, 0, sizeof(out->payload));
    return FirstFrameResult::PairedNoFrame;
  }

  out->header = header;
  out->payloadLength = payloadLength;
  return FirstFrameResult::Committed;
}

}  // namespace wattlink
