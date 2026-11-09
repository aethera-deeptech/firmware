#pragma once

#include <stddef.h>
#include <stdint.h>

#include "frame-codec.h"
#include "hub-pairing.h"
#include "node-registry.h"
#include "payloads.h"
#include "secure-frame.h"

namespace wattlink {

// What the hub does with the frames that nodes send, as docs/espnow-protocol.md describes it in the sections "Finding the hub", "Pairing and keys",
// "Order and repeats" and "Sample". The class does the logic only. The radio, the flash and the clock are behind interfaces, so the same code runs
// on the host in the tests and on the chip. One task must call all of it. Never call it from the ESP-NOW receive callback, because it writes flash
// and the crypto needs a big stack. The callback passes the frame to this task through a queue.

// The hub answers at most this many HELLO frames in each window, as the section "Finding the hub" says.
constexpr uint32_t kHelloWindowMs = 1000;
constexpr uint8_t kHelloPerWindow = 5;

// The highest sampleSeq in an ACK that tells the node that the hub holds no sample of it. It is above any sample, so a node ignores it as progress.
constexpr uint32_t kNoSampleSeq = kNoU32;

// The way out to the radio. The implementation adds a temporary peer for the send and removes it at once, because the link layer holds few peers.
class RadioPort {
 public:
  virtual ~RadioPort() = default;
  // Sends one frame to one node. Returns false when the radio refused it.
  virtual bool send(const uint8_t mac[kMacSize], const uint8_t *frame, size_t length) = 0;
};

// The place that takes the samples. The batches for the backend are built from what it keeps.
class SampleSink {
 public:
  virtual ~SampleSink() = default;
  // Called for a TELEMETRY frame that is fresh and that verified. The samples are valid for the call only. Returns the highest sampleSeq that the
  // hub now holds for this node and this boot of the node, and the hub puts it in the ACK. A repeat that the sink drops still counts as held.
  virtual uint32_t onSamples(const uint8_t mac[kMacSize], uint32_t nodeBootId, const Sample *samples, size_t count) = 0;
};

enum class LinkEventKind : uint8_t {
  Ignored,          // not for the hub, or not valid, or stale, and nothing changed
  HelloAnswered,    // HELLO_ACK was handed to the radio, and replied tells whether the radio took it
  HelloLimited,     // the limit of HELLO frames in a window was reached, so no answer
  PairRequest,      // a PAIR_REQUEST, with the result in request and the reason in refusal
  PairedNode,       // the first TELEMETRY frame proved the key and the node is stored as paired, with its id in nodeId
  PairedNoAck,      // the node is stored as paired, but its first frame could not be answered, so the node sends again
  PairingFailed,    // the registry refused or could not store the pairing, and nothing is paired
  Telemetry,        // a fresh TELEMETRY frame of a paired node, with the number of samples
  StoreFailed,      // a good frame of a paired node was dropped because the flash refused its new boot value
  HubBootMismatch,  // a frame of a paired node that names another boot of the hub, answered with an ACK that carries the right boot number
};

struct LinkEvent {
  LinkEventKind kind;
  bool replied;              // a frame was handed to the radio
  RequestResult request;     // set when the kind is PairRequest
  PairResult refusal;        // the reason when the request was refused, or when the pairing failed
  char nodeId[kNodeIdSize];  // set for an accepted PairRequest and for PairedNode
  size_t samples;            // set for Telemetry and PairedNode
};

class HubLink {
 public:
  // The boot number of the hub comes from the pairing, so both use one value. It must already be in flash, because the hub sends no frame before
  // that, and a value of 0 means that it was not read, so the hub then ignores every frame and sends nothing.
  // The crypto must have open and seal, and it must be the same crypto that pairing has.
  // The counter of the frames that the hub sends belongs to this class, so the sender of the COMMAND frames must go through it too, or a pair of
  // boot number and counter could be used twice with one key.
  HubLink(HubPairing &pairing, NodeRegistry &registry, RadioPort &radio, SampleSink &sink, const HubCrypto &crypto);

  HubLink(const HubLink &) = delete;
  HubLink &operator=(const HubLink &) = delete;

  // A frame arrived from the radio address mac. Returns what happened. It never keeps the frame.
  LinkEvent onFrame(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *frame, size_t length);

 private:
  void onHello(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *frame, size_t length, LinkEvent *event);
  void onPairRequest(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *frame, size_t length, LinkEvent *event);
  void onTelemetry(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *frame, size_t length, LinkEvent *event);
  // The part that both paths share, once the payload of a verified frame is decoded and the frame is accepted by the registry.
  void deliver(const uint8_t mac[kMacSize], const Header &header, uint32_t hubBootId, const Sample *samples, size_t count, bool justPaired,
               LinkEvent *event);
  void fillNodeId(const uint8_t mac[kMacSize], LinkEvent *event) const;
  bool helloAllowed(uint32_t nowMs);
  bool sendPlain(const uint8_t mac[kMacSize], FrameType type, const uint8_t *payload, size_t length);
  bool sendAck(const uint8_t mac[kMacSize], uint32_t nodeBootId, uint32_t highestSampleSeq);

  HubPairing &pairing_;
  NodeRegistry &registry_;
  RadioPort &radio_;
  SampleSink &sink_;
  HubCrypto crypto_;
  uint32_t hubBootId_;
  uint32_t counter_;  // one counter for every frame that the hub sends, so no pair of boot number and counter is used twice with a key
  uint32_t helloWindowStart_;
  uint8_t helloCount_;
};

}  // namespace wattlink
