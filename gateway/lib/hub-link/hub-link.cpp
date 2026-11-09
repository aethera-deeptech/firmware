#include "hub-link.h"

#include <string.h>

namespace wattlink {

namespace {

void clearEvent(LinkEvent *event) {
  event->kind = LinkEventKind::Ignored;
  event->replied = false;
  event->request = RequestResult::NotOpen;
  event->refusal = PairResult::Ok;
  event->nodeId[0] = '\0';
  event->samples = 0;
}

}  // namespace

HubLink::HubLink(HubPairing &pairing, NodeRegistry &registry, RadioPort &radio, SampleSink &sink, const HubCrypto &crypto, uint32_t hubBootId)
    : pairing_(pairing),
      registry_(registry),
      radio_(radio),
      sink_(sink),
      crypto_(crypto),
      hubBootId_(hubBootId),
      counter_(0),
      helloWindowStart_(0),
      helloCount_(0) {}

LinkEvent HubLink::onFrame(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *frame, size_t length) {
  LinkEvent event;
  clearEvent(&event);
  Header header;
  if (!decodeHeader(frame, length, &header)) {
    return event;
  }
  switch (header.type) {
    case FrameType::Hello:
      onHello(nowMs, mac, frame, length, &event);
      break;
    case FrameType::PairRequest:
      onPairRequest(nowMs, mac, frame, length, &event);
      break;
    case FrameType::Telemetry:
      onTelemetry(nowMs, mac, frame, length, &event);
      break;
    default:
      // RESULT comes with the commands. The other types go from the hub to a node, so a node that sends one is ignored.
      break;
  }
  return event;
}

bool HubLink::helloAllowed(uint32_t nowMs) {
  if (helloCount_ == 0 || nowMs - helloWindowStart_ >= kHelloWindowMs) {
    helloWindowStart_ = nowMs;
    helloCount_ = 0;
  }
  if (helloCount_ >= kHelloPerWindow) {
    return false;
  }
  helloCount_++;
  return true;
}

void HubLink::onHello(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *frame, size_t length, LinkEvent *event) {
  Header header;
  const uint8_t *payload = nullptr;
  size_t payloadLength = 0;
  Hello hello;
  if (!parsePlainFrame(frame, length, &header, &payload, &payloadLength) || !decodeHello(payload, payloadLength, &hello)) {
    return;
  }
  if (!helloAllowed(nowMs)) {
    event->kind = LinkEventKind::HelloLimited;
    return;
  }
  // The answer has no payload, and the channel is the one that carried the HELLO frame.
  const uint8_t none = 0;
  event->kind = LinkEventKind::HelloAnswered;
  event->replied = sendPlain(mac, FrameType::HelloAck, &none, kHelloAckSize);
}

void HubLink::onPairRequest(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *frame, size_t length, LinkEvent *event) {
  Header header;
  const uint8_t *payload = nullptr;
  size_t payloadLength = 0;
  if (!parsePlainFrame(frame, length, &header, &payload, &payloadLength)) {
    return;
  }
  RequestOutcome outcome;
  event->kind = LinkEventKind::PairRequest;
  event->request = pairing_.onPairRequest(nowMs, mac, payload, payloadLength, &outcome);
  event->refusal = outcome.refusal;
  if (event->request == RequestResult::Accepted) {
    memcpy(event->nodeId, outcome.nodeId, sizeof(event->nodeId));
    event->replied = sendPlain(mac, FrameType::PairAccept, outcome.acceptPayload, kPairAcceptSize);
  }
}

void HubLink::onTelemetry(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *frame, size_t length, LinkEvent *event) {
  // A frame of the node that is being paired proves the new key. The pairing stores the node before the ACK below is sent.
  FirstFrame first;
  const FirstFrameResult pairing = pairing_.onFrame(nowMs, mac, frame, length, &first);
  switch (pairing) {
    case FirstFrameResult::Committed: {
      Sample samples[kMaxSamplesPerFrame];
      size_t count = 0;
      uint32_t boundBoot = 0;
      if (!decodeTelemetryPayload(first.payload, first.payloadLength, &boundBoot, samples, kMaxSamplesPerFrame, &count)) {
        // The frame is authentic and the node is stored, but the payload is not usable. The node sends again.
        event->kind = LinkEventKind::PairedNoAck;
        return;
      }
      deliver(mac, first.header, boundBoot, samples, count, true, event);
      return;
    }
    case FirstFrameResult::PairedNoFrame:
      event->kind = LinkEventKind::PairedNoAck;
      return;
    case FirstFrameResult::PairFailed:
      event->kind = LinkEventKind::PairingFailed;
      event->refusal = first.pairResult;
      return;
    case FirstFrameResult::BadFrame:
      // A frame of the pending node that does not prove the key. The pairing waits on, and nothing is answered.
      return;
    case FirstFrameResult::NotPending:
      break;
  }

  // A frame of a paired node. The freshness is checked before the tag, because it costs nothing, and the registry changes only after the tag
  // and the payload are good.
  const uint8_t *key = registry_.keyOf(mac);
  Header header;
  if (key == nullptr || !decodeHeader(frame, length, &header) ||
      registry_.check(mac, header.bootId, header.counter) != FrameCheck::Accepted) {
    return;
  }
  Header opened;
  uint8_t payload[kMaxPayloadSize];
  size_t payloadLength = 0;
  if (!openFrame(crypto_.open, crypto_.context, key, Direction::NodeToHub, frame, length, &opened, payload, sizeof(payload), &payloadLength) ||
      opened.type != FrameType::Telemetry) {
    return;
  }
  Sample samples[kMaxSamplesPerFrame];
  size_t count = 0;
  uint32_t boundBoot = 0;
  if (!decodeTelemetryPayload(payload, payloadLength, &boundBoot, samples, kMaxSamplesPerFrame, &count) ||
      registry_.accept(mac, opened.bootId, opened.counter) != FrameCheck::Accepted) {
    return;
  }
  deliver(mac, opened, boundBoot, samples, count, false, event);
}

void HubLink::deliver(const uint8_t mac[kMacSize], const Header &header, uint32_t hubBootId, const Sample *samples, size_t count,
                      bool justPaired, LinkEvent *event) {
  if (hubBootId != hubBootId_) {
    // The node names an old boot of the hub, or none. It gets an ACK that carries the boot number of the hub in its header, so it learns the
    // number. No sample is taken, so the ACK says that the hub holds none.
    event->kind = LinkEventKind::HubBootMismatch;
    event->replied = sendAck(mac, header.bootId, kNoSampleSeq);
    return;
  }
  const uint32_t highest = sink_.onSamples(mac, header.bootId, samples, count);
  event->kind = justPaired ? LinkEventKind::PairedNode : LinkEventKind::Telemetry;
  event->samples = count;
  if (justPaired) {
    makeNodeId(registry_.kindOf(mac), mac, event->nodeId);
  }
  event->replied = sendAck(mac, header.bootId, highest);
}

bool HubLink::sendPlain(const uint8_t mac[kMacSize], FrameType type, const uint8_t *payload, size_t length) {
  if (counter_ == kNoU32) {
    return false;
  }
  // The counter is used up even when the send fails, so it is never used again.
  const Header header = {kProtocolVersion, type, hubBootId_, counter_++};
  uint8_t out[kMaxFrameSize];
  const size_t size = buildPlainFrame(header, payload, length, out, sizeof(out));
  return size > 0 && radio_.send(mac, out, size);
}

bool HubLink::sendAck(const uint8_t mac[kMacSize], uint32_t nodeBootId, uint32_t highestSampleSeq) {
  const uint8_t *key = registry_.keyOf(mac);
  if (key == nullptr || counter_ == kNoU32) {
    return false;
  }
  Ack ack;
  ack.nodeBootId = nodeBootId;
  ack.highestSampleSeq = highestSampleSeq;
  uint8_t payload[kAckSize];
  if (encodeAck(ack, payload, sizeof(payload)) != kAckSize) {
    return false;
  }
  const Header header = {kProtocolVersion, FrameType::Ack, hubBootId_, counter_++};
  uint8_t out[kMaxFrameSize];
  const size_t size = sealFrame(crypto_.seal, crypto_.context, key, header, payload, sizeof(payload), out, sizeof(out));
  return size > 0 && radio_.send(mac, out, size);
}

}  // namespace wattlink
