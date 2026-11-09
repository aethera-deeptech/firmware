#pragma once

#include <stddef.h>
#include <stdint.h>

#include "frame-codec.h"
#include "hub-pairing.h"
#include "node-registry.h"
#include "payloads.h"
#include "sample-sink.h"
#include "secure-frame.h"

namespace wattlink {

// What the hub does with the frames that nodes send, as docs/espnow-protocol.md describes it in the sections "Finding the hub", "Pairing and keys",
// "Order and repeats" and "Sample". The class does the logic only. The radio, the flash and the clock are behind interfaces, so the same code runs
// on the host in the tests and on the chip. One task must call all of it. Never call it from the ESP-NOW receive callback, because it writes flash
// and the crypto needs a big stack. The callback passes the frame to this task through a queue.

// The hub answers at most this many HELLO frames in each window, as the section "Finding the hub" says.
constexpr uint32_t kHelloWindowMs = 1000;
constexpr uint8_t kHelloPerWindow = 5;
// One radio address gets at most this many answers in each window, so a node that repeats its HELLO does not use up the answers of the others. A node
// scans the channels one after the other and is heard on one of them, so it needs one answer. The hub remembers the last kHelloTrackedMacs addresses.
constexpr uint8_t kHelloPerMacPerWindow = 2;
constexpr size_t kHelloTrackedMacs = 8;

// The way out to the radio. The implementation adds a temporary peer for the send and removes it when the send is done, because the link layer holds few peers.
// A send may block the calling task for a short time. On the chip it waits 50 ms at most for the report of the link layer.
class RadioPort {
 public:
  virtual ~RadioPort() = default;
  // Sends one frame to one node. Returns false when the radio refused it.
  virtual bool send(const uint8_t mac[kMacSize], const uint8_t *frame, size_t length) = 0;
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
  ResultReceived,   // a fresh RESULT frame of a paired node, with the command number in commandNumber and the answer in commandOk
};

struct LinkEvent {
  LinkEventKind kind;
  bool replied;              // a frame was handed to the radio
  uint8_t mac[kMacSize];     // the radio address that the frame came from, set for every event
  uint32_t frameBootId;      // the bootId and the counter of the frame, set for Telemetry, PairedNode and ResultReceived
  uint32_t frameCounter;     // A COMMAND may echo only the values of a Telemetry or PairedNode event. For ResultReceived they are for the log only.
  uint16_t commandNumber;    // set for ResultReceived
  bool commandOk;            // set for ResultReceived, true when the node says that the relay switched
  RequestResult request;     // set when the kind is PairRequest
  PairResult refusal;        // the reason when the request was refused, or when the pairing failed
  char nodeId[kNodeIdSize];  // set for an accepted PairRequest and for PairedNode
  size_t samples;            // set for Telemetry and PairedNode
};

// The way out for a COMMAND frame. HubLink is the real one, and the tests of the command module give a fake one.
class CommandSender {
 public:
  virtual ~CommandSender() = default;
  // Sends a COMMAND to a paired node. The echo is the bootId and the counter of a TELEMETRY frame of that node. The action is 1 for on and 0 for off.
  // Returns false when the command was not handed to the radio.
  virtual bool sendCommand(const uint8_t mac[kMacSize], uint32_t echoBootId, uint32_t echoCounter, uint16_t number, uint8_t action) = 0;
};

class HubLink : public CommandSender {
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

  // The echo is the bootId and the counter of a TELEMETRY frame of that node, as the event of that frame names them. Returns false when the node is not
  // paired, the action is not 0 or 1, the hub has no boot number, or the radio refused the frame.
  // The command goes out with the counter of this class, so a pair of boot number and counter is never used twice with a key.
  bool sendCommand(const uint8_t mac[kMacSize], uint32_t echoBootId, uint32_t echoCounter, uint16_t number, uint8_t action) override;

 private:
  void onHello(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *frame, size_t length, LinkEvent *event);
  void onPairRequest(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *frame, size_t length, LinkEvent *event);
  void onTelemetry(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *frame, size_t length, LinkEvent *event);
  void onResult(uint32_t nowMs, const uint8_t mac[kMacSize], const uint8_t *frame, size_t length, LinkEvent *event);
  // Checks the key, the freshness and the tag of a frame of a paired node and decodes nothing else. The registry is not changed.
  bool openPaired(const uint8_t mac[kMacSize], const uint8_t *frame, size_t length, FrameType expected, Header *opened, uint8_t *payload,
                  size_t *payloadLength) const;
  // The part that both paths share, once the payload of a verified frame is decoded and the frame is accepted by the registry.
  void deliver(uint32_t nowMs, const uint8_t mac[kMacSize], const Header &header, uint32_t hubBootId, const Sample *samples, size_t count,
               bool justPaired, LinkEvent *event);
  void fillNodeId(const uint8_t mac[kMacSize], LinkEvent *event) const;
  bool helloAllowed(uint32_t nowMs, const uint8_t mac[kMacSize]);
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

  // The HELLO answers of the last addresses, each with the start of its own window.
  struct HelloSender {
    bool used;
    uint8_t mac[kMacSize];
    uint32_t windowStart;
    uint8_t count;
  };
  HelloSender helloSenders_[kHelloTrackedMacs];
};

}  // namespace wattlink
