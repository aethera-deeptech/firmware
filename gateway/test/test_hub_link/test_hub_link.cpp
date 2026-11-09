#include <string.h>
#include <unity.h>

#include "../support/fake-crypto.h"
#include "hub-link.h"

using namespace wattlink;
using namespace wattlink::testsupport;

namespace {

const uint8_t kHubMac[kMacSize] = {0x48, 0xe7, 0x29, 0xb6, 0xe0, 0x84};
constexpr uint32_t kHubBootId = 7;
constexpr uint32_t kNodeBootId = 3;

struct SentFrame {
  uint8_t mac[kMacSize];
  uint8_t data[kMaxFrameSize];
  size_t length;
};

// Keeps every frame that the hub hands to the radio.
class FakeRadio : public RadioPort {
 public:
  bool send(const uint8_t mac[kMacSize], const uint8_t *frame, size_t length) override {
    attempts++;
    if (fail || count >= 64) {
      return false;
    }
    memcpy(sent[count].mac, mac, kMacSize);
    memcpy(sent[count].data, frame, length);
    sent[count].length = length;
    count++;
    return true;
  }

  SentFrame sent[64];
  size_t count = 0;
  int attempts = 0;
  bool fail = false;
};

// Remembers what it was given. It answers with the sampleSeq of the last sample of the frame, or with a fixed value when one is set.
class FakeSink : public SampleSink {
 public:
  uint32_t onSamples(uint32_t nowMs, const uint8_t mac[kMacSize], uint32_t nodeBootId, const Sample *samples, size_t count) override {
    calls++;
    lastNowMs = nowMs;
    memcpy(lastMac, mac, kMacSize);
    lastBootId = nodeBootId;
    lastCount = count;
    firstSeq = samples[0].sampleSeq;
    return useAnswer ? answer : samples[count - 1].sampleSeq;
  }

  int calls = 0;
  uint32_t lastNowMs = 0;
  uint8_t lastMac[kMacSize] = {0};
  uint32_t lastBootId = 0;
  size_t lastCount = 0;
  uint32_t firstSeq = 0;
  bool useAnswer = false;
  uint32_t answer = 0;
};

struct Rig {
  explicit Rig(uint32_t hubBootId = kHubBootId)
      : registry(store),
        crypto{&knobs, fakeHmac, fakeOpen, fakeSeal, fakeGenerate, fakeAgree},
        pairing(window, registry, crypto, kHubMac, hubBootId),
        link(pairing, registry, radio, sink, crypto) {
    registry.load();
  }

  FakeStore store;
  NodeRegistry registry;
  PairingWindow window;
  Knobs knobs;
  HubCrypto crypto;
  HubPairing pairing;
  FakeRadio radio;
  FakeSink sink;
  HubLink link;
};

struct NodeSim {
  uint8_t mac[kMacSize];
  uint8_t privateKey[kPublicKeySize];
  uint8_t publicKey[kPublicKeySize];
  uint8_t key[kPairingKeySize];
};

NodeSim makeNode(uint8_t number) {
  NodeSim node;
  const uint8_t mac[kMacSize] = {0x40, 0x00, 0x00, 0x00, 0x00, number};
  memcpy(node.mac, mac, kMacSize);
  for (size_t i = 0; i < kPublicKeySize; i++) {
    node.privateKey[i] = static_cast<uint8_t>(0x80 + number + i);
    node.publicKey[i] = node.privateKey[i] ^ kMask;
  }
  memset(node.key, 0, sizeof(node.key));
  return node;
}

int kind(const LinkEvent &event) {
  return static_cast<int>(event.kind);
}

int kind(LinkEventKind value) {
  return static_cast<int>(value);
}

size_t plainFrame(FrameType type, uint32_t bootId, uint32_t counter, const uint8_t *payload, size_t length, uint8_t *out) {
  const Header header = {kProtocolVersion, type, bootId, counter};
  const size_t size = buildPlainFrame(header, payload, length, out, kMaxFrameSize);
  TEST_ASSERT_TRUE(size > 0);
  return size;
}

size_t helloFrame(uint8_t *out) {
  const Hello hello = {NodeKind::Plug, 1, 0, 0};
  uint8_t payload[kHelloSize];
  TEST_ASSERT_EQUAL_UINT(kHelloSize, encodeHello(hello, payload, sizeof(payload)));
  return plainFrame(FrameType::Hello, kNodeBootId, 0, payload, sizeof(payload), out);
}

size_t requestFrame(const NodeSim &node, NodeKind nodeKind, uint8_t *out) {
  PairRequest request;
  request.kind = nodeKind;
  memcpy(request.nodePublicKey, node.publicKey, kPublicKeySize);
  uint8_t payload[kPairRequestSize];
  TEST_ASSERT_EQUAL_UINT(kPairRequestSize, encodePairRequest(request, payload, sizeof(payload)));
  return plainFrame(FrameType::PairRequest, kNodeBootId, 0, payload, sizeof(payload), out);
}

size_t telemetryFrame(const uint8_t key[kAesKeySize], uint32_t bootId, uint32_t counter, uint32_t sampleSeq, uint8_t *out,
                      uint32_t hubBootId = kHubBootId) {
  Sample sample = {};
  sample.sampleSeq = sampleSeq;
  uint8_t payload[kMaxPayloadSize];
  const size_t length = encodeTelemetryPayload(hubBootId, &sample, 1, payload, sizeof(payload));
  const Header header = {kProtocolVersion, FrameType::Telemetry, bootId, counter};
  const size_t size = sealFrame(fakeSeal, nullptr, key, header, payload, length, out, kMaxFrameSize);
  TEST_ASSERT_TRUE(size > 0);
  return size;
}

// A frame with a TELEMETRY header whose payload says 2 samples but holds 1. The length fits the type, so it can be sealed, and the payload
// does not decode.
size_t malformedTelemetryFrame(const uint8_t key[kAesKeySize], uint32_t bootId, uint32_t counter, uint8_t *out) {
  Sample sample = {};
  uint8_t payload[kMaxPayloadSize];
  const size_t length = encodeTelemetryPayload(kHubBootId, &sample, 1, payload, sizeof(payload));
  payload[4] = 2;
  const Header header = {kProtocolVersion, FrameType::Telemetry, bootId, counter};
  const size_t size = sealFrame(fakeSeal, nullptr, key, header, payload, length, out, kMaxFrameSize);
  TEST_ASSERT_TRUE(size > 0);
  return size;
}

LinkEvent hello(Rig &rig, const NodeSim &node, uint32_t nowMs) {
  uint8_t frame[kMaxFrameSize];
  const size_t size = helloFrame(frame);
  return rig.link.onFrame(nowMs, node.mac, frame, size);
}

LinkEvent request(Rig &rig, const NodeSim &node, uint32_t nowMs, NodeKind nodeKind = NodeKind::Plug) {
  uint8_t frame[kMaxFrameSize];
  const size_t size = requestFrame(node, nodeKind, frame);
  return rig.link.onFrame(nowMs, node.mac, frame, size);
}

LinkEvent telemetry(Rig &rig, const NodeSim &node, uint32_t nowMs, uint32_t bootId, uint32_t counter, uint32_t sampleSeq,
                    uint32_t hubBootId = kHubBootId) {
  uint8_t frame[kMaxFrameSize];
  const size_t size = telemetryFrame(node.key, bootId, counter, sampleSeq, frame, hubBootId);
  return rig.link.onFrame(nowMs, node.mac, frame, size);
}

size_t resultFrame(const uint8_t key[kAesKeySize], uint32_t bootId, uint32_t counter, uint16_t number, uint8_t ok, uint8_t *out,
                   uint32_t hubBootId = kHubBootId) {
  Result value = {hubBootId, number, ok};
  uint8_t payload[kResultSize];
  TEST_ASSERT_EQUAL_UINT(kResultSize, encodeResult(value, payload, sizeof(payload)));
  const Header header = {kProtocolVersion, FrameType::Result, bootId, counter};
  const size_t size = sealFrame(fakeSeal, nullptr, key, header, payload, sizeof(payload), out, kMaxFrameSize);
  TEST_ASSERT_TRUE(size > 0);
  return size;
}

LinkEvent result(Rig &rig, const NodeSim &node, uint32_t nowMs, uint32_t bootId, uint32_t counter, uint16_t number, uint8_t ok,
                 uint32_t hubBootId = kHubBootId) {
  uint8_t frame[kMaxFrameSize];
  const size_t size = resultFrame(node.key, bootId, counter, number, ok, frame, hubBootId);
  return rig.link.onFrame(nowMs, node.mac, frame, size);
}

// The key that the node derives from the PAIR_ACCEPT frame that the hub sent last.
void takeKey(Rig &rig, NodeSim *node) {
  TEST_ASSERT_TRUE(rig.radio.count > 0);
  const SentFrame &last = rig.radio.sent[rig.radio.count - 1];
  Header header;
  const uint8_t *payload = nullptr;
  size_t payloadLength = 0;
  TEST_ASSERT_TRUE(parsePlainFrame(last.data, last.length, &header, &payload, &payloadLength));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FrameType::PairAccept), static_cast<int>(header.type));
  PairAccept accept;
  TEST_ASSERT_TRUE(decodePairAccept(payload, payloadLength, &accept));
  uint8_t shared[kSharedSecretSize];
  TEST_ASSERT_TRUE(fakeAgree(&rig.knobs, node->privateKey, accept.hubPublicKey, shared));
  TEST_ASSERT_TRUE(derivePairingKey(fakeHmac, &rig.knobs, shared, node->publicKey, accept.hubPublicKey, node->mac, kHubMac, node->key));
}

// Opens the window and pairs the node through the frames, so the node has its key and the registry holds it. The radio log keeps the frames.
void pairNode(Rig &rig, NodeSim *node, uint32_t nowMs) {
  rig.window.press(nowMs);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::PairRequest), kind(request(rig, *node, nowMs)));
  takeKey(rig, node);
  const LinkEvent first = telemetry(rig, *node, nowMs + 100, kNodeBootId, 0, 0);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::PairedNode), kind(first));
  TEST_ASSERT_TRUE(rig.registry.isPaired(node->mac));
}

// Opens a frame that the hub sent, with the key of the node, and gives its header and its ACK payload.
void openAck(const SentFrame &frame, const uint8_t key[kAesKeySize], Header *header, Ack *ack) {
  uint8_t payload[kMaxPayloadSize];
  size_t length = 0;
  TEST_ASSERT_TRUE(openFrame(fakeOpen, nullptr, key, Direction::HubToNode, frame.data, frame.length, header, payload, sizeof(payload), &length));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FrameType::Ack), static_cast<int>(header->type));
  TEST_ASSERT_TRUE(decodeAck(payload, length, ack));
}

// Opens a frame that the hub sent, with the key of the node, and gives its header and its COMMAND payload.
void openCommand(const SentFrame &frame, const uint8_t key[kAesKeySize], Header *header, Command *command) {
  uint8_t payload[kMaxPayloadSize];
  size_t length = 0;
  TEST_ASSERT_TRUE(openFrame(fakeOpen, nullptr, key, Direction::HubToNode, frame.data, frame.length, header, payload, sizeof(payload), &length));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FrameType::Command), static_cast<int>(header->type));
  TEST_ASSERT_TRUE(decodeCommand(payload, length, command));
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_a_hello_is_answered_with_a_hello_ack_to_the_sender() {
  Rig rig;
  const NodeSim node = makeNode(1);
  const LinkEvent event = hello(rig, node, 0);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(event));
  TEST_ASSERT_TRUE(event.replied);
  TEST_ASSERT_EQUAL_UINT(1, rig.radio.count);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(node.mac, rig.radio.sent[0].mac, kMacSize);
  Header header;
  const uint8_t *payload = nullptr;
  size_t payloadLength = 99;
  TEST_ASSERT_TRUE(parsePlainFrame(rig.radio.sent[0].data, rig.radio.sent[0].length, &header, &payload, &payloadLength));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FrameType::HelloAck), static_cast<int>(header.type));
  TEST_ASSERT_EQUAL_UINT32(kHubBootId, header.bootId);
  TEST_ASSERT_EQUAL_UINT(0, payloadLength);
}

void test_the_hub_answers_five_hello_frames_in_a_second_and_then_stops() {
  Rig rig;
  for (uint8_t i = 0; i < 5; i++) {
    TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, makeNode(static_cast<uint8_t>(i + 1)), 100 + i)));
  }
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, makeNode(6), 500)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, makeNode(7), 100 + kHelloWindowMs - 1)));
  TEST_ASSERT_EQUAL_UINT(5, rig.radio.count);
  // A new window starts one second after the first HELLO of the old one.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, makeNode(8), 100 + kHelloWindowMs)));
  TEST_ASSERT_EQUAL_UINT(6, rig.radio.count);
}

void test_one_address_gets_two_answers_in_a_second_and_its_refused_frames_do_not_use_the_global_limit() {
  Rig rig;
  const NodeSim noisy = makeNode(1);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, noisy, 0)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, noisy, 10)));
  for (uint32_t t = 20; t < 400; t += 20) {
    TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, noisy, t)));
  }
  TEST_ASSERT_EQUAL_UINT(2, rig.radio.count);
  // The global limit is 5, and the noisy address used 2, so three other addresses are answered and the fourth is not.
  for (uint8_t i = 2; i <= 4; i++) {
    TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, makeNode(i), 500)));
  }
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, makeNode(5), 501)));
  TEST_ASSERT_EQUAL_UINT(5, rig.radio.count);
  // The window of the address is its own and starts with its first HELLO, so it is answered again one second later.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, noisy, kHelloWindowMs - 1)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, noisy, kHelloWindowMs)));
}

void test_the_table_of_hello_senders_replaces_the_address_with_the_oldest_window_first() {
  Rig rig;
  // Twenty addresses, one second apart, so the global limit never matters. Every one is answered.
  for (uint8_t i = 1; i <= 20; i++) {
    TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, makeNode(i), static_cast<uint32_t>(i) * 1000u)));
  }
  const NodeSim last = makeNode(20);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, last, 20000 + 10)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, last, 20000 + 20)));
  TEST_ASSERT_EQUAL_UINT(21, rig.radio.count);
}

void test_a_full_table_of_hello_senders_forgets_the_oldest_window_and_the_newest_keep_their_count() {
  Rig rig;
  const NodeSim first = makeNode(1);
  // Address 1 uses its two answers, and the table fills with other addresses in the same second. An address that is refused by the global limit still
  // takes a place in the table.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, makeNode(100), 0)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, first, 900)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, first, 901)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, first, 902)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, makeNode(2), 910)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, makeNode(3), 911)));
  for (uint8_t i = 4; i <= 7; i++) {
    TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, makeNode(i), 916u + i)));
  }
  // The table has 8 places and is full now. Two more addresses take the place of the oldest windows, which are address 100 and then address 1.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, makeNode(8), 930)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, makeNode(9), 931)));
  // The window of address 1 is still open at 1010, but the hub forgot it, so it starts with a new count, which is the weakness that the documentation names.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, first, 1010)));
  // The newest address keeps its count.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, makeNode(9), 1011)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, makeNode(9), 1012)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, makeNode(9), 1013)));
}

void test_the_hello_limit_works_across_the_wrap_of_the_clock() {
  Rig rig;
  for (uint8_t i = 1; i <= 5; i++) {
    TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, makeNode(i), 0xFFFFFF00u)));
  }
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, makeNode(6), 0xFFFFFF10u)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, makeNode(7), 0xFFFFFF00u + kHelloWindowMs)));
  // The limit of one address works across the wrap too.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, makeNode(1), 0xFFFFFF00u + kHelloWindowMs)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, makeNode(1), 0xFFFFFF00u + kHelloWindowMs + 1)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, makeNode(1), 0xFFFFFF00u + kHelloWindowMs + 2)));
}

void test_a_radio_that_refuses_the_hello_ack_is_reported() {
  Rig rig;
  rig.radio.fail = true;
  const LinkEvent event = hello(rig, makeNode(1), 0);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(event));
  TEST_ASSERT_FALSE(event.replied);
  TEST_ASSERT_EQUAL_UINT(0, rig.radio.count);
}

void test_frames_that_are_not_valid_or_not_for_the_hub_are_ignored() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t frame[kMaxFrameSize];
  // too short, wrong version, unknown type
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(0, node.mac, frame, 3)));
  size_t size = helloFrame(frame);
  frame[0] = 9;
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(0, node.mac, frame, size)));
  size = helloFrame(frame);
  frame[1] = 0x77;
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(0, node.mac, frame, size)));
  // a HELLO with a payload of the wrong length, and a HELLO with a kind that is not known
  size = helloFrame(frame);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(0, node.mac, frame, size - 1)));
  size = helloFrame(frame);
  frame[kHeaderSize] = 9;
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(0, node.mac, frame, size)));
  // a PAIR_REQUEST of the wrong length
  size = requestFrame(node, NodeKind::Plug, frame);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(0, node.mac, frame, size - 1)));
  TEST_ASSERT_EQUAL_UINT(0, rig.radio.count);
}

void test_frames_of_a_hub_to_node_type_are_ignored_by_type() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 0);
  const size_t before = rig.radio.count;
  uint8_t frame[kMaxFrameSize];
  // PAIR_ACCEPT and HELLO_ACK from a node
  const uint8_t none = 0;
  size_t size = plainFrame(FrameType::HelloAck, 1, 0, &none, 0, frame);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(0, node.mac, frame, size)));
  uint8_t accept[kPairAcceptSize] = {0};
  size = plainFrame(FrameType::PairAccept, 1, 0, accept, sizeof(accept), frame);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(0, node.mac, frame, size)));
  // an ACK sealed with the real key of the node, because the ACK goes from the hub to a node
  uint8_t payload[kMaxPayloadSize];
  const Ack ack = {kNodeBootId, 0};
  const size_t length = encodeAck(ack, payload, sizeof(payload));
  const Header header = {kProtocolVersion, FrameType::Ack, kNodeBootId, 5};
  size = sealFrame(fakeSeal, nullptr, node.key, header, payload, length, frame, sizeof(frame));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(0, node.mac, frame, size)));
  TEST_ASSERT_EQUAL_UINT(before, rig.radio.count);
}

void test_a_pair_request_in_an_open_window_gets_a_pair_accept() {
  Rig rig;
  const NodeSim node = makeNode(1);
  rig.window.press(0);
  const LinkEvent event = request(rig, node, 10);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::PairRequest), kind(event));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(RequestResult::Accepted), static_cast<int>(event.request));
  TEST_ASSERT_EQUAL_STRING("PLUG-000001", event.nodeId);
  TEST_ASSERT_TRUE(event.replied);
  TEST_ASSERT_EQUAL_UINT(1, rig.radio.count);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(node.mac, rig.radio.sent[0].mac, kMacSize);
  Header header;
  const uint8_t *payload = nullptr;
  size_t payloadLength = 0;
  TEST_ASSERT_TRUE(parsePlainFrame(rig.radio.sent[0].data, rig.radio.sent[0].length, &header, &payload, &payloadLength));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FrameType::PairAccept), static_cast<int>(header.type));
  TEST_ASSERT_EQUAL_UINT32(kHubBootId, header.bootId);
  PairAccept accept;
  TEST_ASSERT_TRUE(decodePairAccept(payload, payloadLength, &accept));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(node.publicKey, accept.nodePublicKey, kPublicKeySize);
}

void test_a_pair_request_that_is_refused_gets_no_answer() {
  Rig rig;
  const NodeSim node = makeNode(1);
  // closed window
  LinkEvent event = request(rig, node, 0);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::PairRequest), kind(event));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(RequestResult::NotOpen), static_cast<int>(event.request));
  TEST_ASSERT_FALSE(event.replied);
  TEST_ASSERT_EQUAL_STRING("", event.nodeId);
  // a node that is known
  const uint8_t key[kNodeKeySize] = {1};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::Ok), static_cast<int>(rig.registry.pair(kKindPlug, node.mac, key)));
  rig.window.press(10);
  event = request(rig, node, 20);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(RequestResult::Refused), static_cast<int>(event.request));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::AlreadyPaired), static_cast<int>(event.refusal));
  TEST_ASSERT_EQUAL_UINT(0, rig.radio.count);
}

void test_the_first_frame_pairs_the_node_stores_its_samples_and_gets_an_ack() {
  Rig rig;
  NodeSim node = makeNode(1);
  rig.window.press(0);
  request(rig, node, 10);
  takeKey(rig, &node);
  rig.sink.useAnswer = true;
  rig.sink.answer = 41;
  const LinkEvent event = telemetry(rig, node, 20, kNodeBootId, 0, 41);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::PairedNode), kind(event));
  TEST_ASSERT_EQUAL_STRING("PLUG-000001", event.nodeId);
  TEST_ASSERT_EQUAL_UINT(1, event.samples);
  TEST_ASSERT_TRUE(event.replied);
  TEST_ASSERT_TRUE(rig.registry.isPaired(node.mac));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(node.key, rig.registry.keyOf(node.mac), kPairingKeySize);

  TEST_ASSERT_EQUAL_INT(1, rig.sink.calls);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(node.mac, rig.sink.lastMac, kMacSize);
  TEST_ASSERT_EQUAL_UINT32(kNodeBootId, rig.sink.lastBootId);
  TEST_ASSERT_EQUAL_UINT32(41, rig.sink.firstSeq);

  // The ACK is the second frame that the hub sent, and the node can open it with its key.
  TEST_ASSERT_EQUAL_UINT(2, rig.radio.count);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(node.mac, rig.radio.sent[1].mac, kMacSize);
  Header header;
  Ack ack;
  openAck(rig.radio.sent[1], node.key, &header, &ack);
  TEST_ASSERT_EQUAL_UINT32(kHubBootId, header.bootId);
  TEST_ASSERT_EQUAL_UINT32(kNodeBootId, ack.nodeBootId);
  TEST_ASSERT_EQUAL_UINT32(41, ack.highestSampleSeq);
}

void test_a_first_frame_with_another_hub_boot_number_pairs_nothing_and_gets_no_ack() {
  Rig rig;
  NodeSim node = makeNode(1);
  rig.window.press(0);
  request(rig, node, 10);
  takeKey(rig, &node);
  const LinkEvent event = telemetry(rig, node, 20, kNodeBootId, 0, 0, kHubBootId + 1);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(event));
  TEST_ASSERT_FALSE(rig.registry.isPaired(node.mac));
  TEST_ASSERT_EQUAL_UINT(1, rig.radio.count);
  TEST_ASSERT_EQUAL_INT(0, rig.sink.calls);
  // The pairing waits on, and a good frame still pairs.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::PairedNode), kind(telemetry(rig, node, 30, kNodeBootId, 1, 0)));
}

void test_a_radio_that_refuses_the_first_ack_leaves_the_node_paired() {
  Rig rig;
  NodeSim node = makeNode(1);
  rig.window.press(0);
  request(rig, node, 10);
  takeKey(rig, &node);
  rig.radio.fail = true;
  const LinkEvent event = telemetry(rig, node, 20, kNodeBootId, 0, 0);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::PairedNode), kind(event));
  TEST_ASSERT_FALSE(event.replied);
  TEST_ASSERT_TRUE(rig.registry.isPaired(node.mac));
  // The node sends again with the next counter, and the normal path answers it.
  rig.radio.fail = false;
  const LinkEvent again = telemetry(rig, node, 30, kNodeBootId, 1, 1);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Telemetry), kind(again));
  TEST_ASSERT_TRUE(again.replied);
}

void test_a_flash_that_refuses_the_pairing_pairs_nothing_and_sends_nothing() {
  Rig rig;
  NodeSim node = makeNode(1);
  rig.window.press(0);
  request(rig, node, 10);
  takeKey(rig, &node);
  rig.store.failWrites = true;
  const LinkEvent event = telemetry(rig, node, 20, kNodeBootId, 0, 0);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::PairingFailed), kind(event));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::StoreFailed), static_cast<int>(event.refusal));
  TEST_ASSERT_FALSE(rig.registry.isPaired(node.mac));
  TEST_ASSERT_EQUAL_UINT(1, rig.radio.count);
  TEST_ASSERT_EQUAL_INT(0, rig.sink.calls);
}

void test_a_boot_value_that_cannot_be_stored_leaves_the_node_paired_without_an_ack() {
  Rig rig;
  NodeSim node = makeNode(1);
  rig.window.press(0);
  request(rig, node, 10);
  takeKey(rig, &node);
  rig.store.failWriteNumber = 2;  // the pairing is the first write and the boot value is the second
  const LinkEvent event = telemetry(rig, node, 20, kNodeBootId, 0, 0);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::PairedNoAck), kind(event));
  TEST_ASSERT_FALSE(event.replied);
  TEST_ASSERT_EQUAL_STRING("PLUG-000001", event.nodeId);
  TEST_ASSERT_TRUE(rig.registry.isPaired(node.mac));
  TEST_ASSERT_EQUAL_UINT(1, rig.radio.count);
  TEST_ASSERT_EQUAL_INT(0, rig.sink.calls);
  const LinkEvent again = telemetry(rig, node, 30, kNodeBootId, 1, 0);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Telemetry), kind(again));
}

void test_a_frame_that_does_not_verify_does_not_end_a_pairing() {
  Rig rig;
  NodeSim node = makeNode(1);
  rig.window.press(0);
  request(rig, node, 10);
  takeKey(rig, &node);
  uint8_t frame[kMaxFrameSize];
  const size_t size = telemetryFrame(node.key, kNodeBootId, 0, 0, frame);
  frame[kHeaderSize + 1] ^= 0x01;
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(20, node.mac, frame, size)));
  TEST_ASSERT_EQUAL_UINT(1, rig.radio.count);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::PairedNode), kind(telemetry(rig, node, 30, kNodeBootId, 0, 0)));
}

void test_a_fresh_frame_of_a_paired_node_is_stored_and_acknowledged() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 0);
  const size_t before = rig.radio.count;
  const LinkEvent event = telemetry(rig, node, 10000, kNodeBootId, 1, 9);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Telemetry), kind(event));
  TEST_ASSERT_EQUAL_UINT(1, event.samples);
  TEST_ASSERT_TRUE(event.replied);
  TEST_ASSERT_EQUAL_INT(2, rig.sink.calls);
  TEST_ASSERT_EQUAL_UINT32(10000, rig.sink.lastNowMs);
  TEST_ASSERT_EQUAL_UINT32(9, rig.sink.firstSeq);
  TEST_ASSERT_EQUAL_UINT(before + 1, rig.radio.count);
  Header header;
  Ack ack;
  openAck(rig.radio.sent[before], node.key, &header, &ack);
  TEST_ASSERT_EQUAL_UINT32(kNodeBootId, ack.nodeBootId);
  TEST_ASSERT_EQUAL_UINT32(9, ack.highestSampleSeq);
}

void test_the_hub_counter_grows_with_every_frame_that_it_sends() {
  Rig rig;
  NodeSim node = makeNode(1);
  hello(rig, node, 0);
  pairNode(rig, &node, 10);
  telemetry(rig, node, 20000, kNodeBootId, 1, 1);
  telemetry(rig, node, 30000, kNodeBootId, 2, 2);
  // HELLO_ACK, PAIR_ACCEPT, the ACK of the first frame and two more ACK frames
  TEST_ASSERT_EQUAL_UINT(5, rig.radio.count);
  for (size_t i = 0; i < rig.radio.count; i++) {
    Header header;
    TEST_ASSERT_TRUE(decodeHeader(rig.radio.sent[i].data, rig.radio.sent[i].length, &header));
    TEST_ASSERT_EQUAL_UINT32(kHubBootId, header.bootId);
    TEST_ASSERT_EQUAL_UINT32(i, header.counter);
  }
}

void test_a_replayed_or_older_frame_is_ignored() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 0);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Telemetry), kind(telemetry(rig, node, 10, kNodeBootId, 5, 1)));
  const size_t sent = rig.radio.count;
  const int calls = rig.sink.calls;
  // the same counter, a lower counter, and an older boot of the node
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(telemetry(rig, node, 20, kNodeBootId, 5, 1)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(telemetry(rig, node, 20, kNodeBootId, 4, 1)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(telemetry(rig, node, 20, kNodeBootId - 1, 99, 1)));
  TEST_ASSERT_EQUAL_UINT(sent, rig.radio.count);
  TEST_ASSERT_EQUAL_INT(calls, rig.sink.calls);
  // a new boot of the node, with a counter that starts again, is accepted
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Telemetry), kind(telemetry(rig, node, 30, kNodeBootId + 1, 0, 0)));
}

void test_a_frame_of_a_node_that_is_not_paired_is_ignored() {
  Rig rig;
  NodeSim stranger = makeNode(9);
  const uint8_t key[kPairingKeySize] = {5, 5, 5};
  memcpy(stranger.key, key, sizeof(key));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(telemetry(rig, stranger, 0, 1, 0, 0)));
  TEST_ASSERT_EQUAL_UINT(0, rig.radio.count);
  TEST_ASSERT_EQUAL_INT(0, rig.sink.calls);
}

void test_a_frame_that_does_not_verify_changes_nothing_and_does_not_use_up_the_counter() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 0);
  const size_t sent = rig.radio.count;
  uint8_t frame[kMaxFrameSize];
  const size_t size = telemetryFrame(node.key, kNodeBootId, 7, 1, frame);
  frame[kHeaderSize + 2] ^= 0x01;
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(10, node.mac, frame, size)));
  TEST_ASSERT_EQUAL_UINT(sent, rig.radio.count);
  // A frame made with another key is refused the same way.
  NodeSim other = node;
  other.key[0] ^= 0xFF;
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(telemetry(rig, other, 20, kNodeBootId, 7, 1)));
  // The real frame with the same counter is still accepted.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Telemetry), kind(telemetry(rig, node, 30, kNodeBootId, 7, 1)));
}

void test_a_frame_with_another_hub_boot_number_gets_an_ack_with_the_right_one_and_no_sample_is_taken() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 0);
  const size_t before = rig.radio.count;
  const int calls = rig.sink.calls;
  const LinkEvent event = telemetry(rig, node, 10, kNodeBootId, 1, 5, kHubBootId - 1);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HubBootMismatch), kind(event));
  TEST_ASSERT_TRUE(event.replied);
  TEST_ASSERT_EQUAL_INT(calls, rig.sink.calls);
  TEST_ASSERT_EQUAL_UINT(before + 1, rig.radio.count);
  Header header;
  Ack ack;
  openAck(rig.radio.sent[before], node.key, &header, &ack);
  TEST_ASSERT_EQUAL_UINT32(kHubBootId, header.bootId);
  TEST_ASSERT_EQUAL_UINT32(kNodeBootId, ack.nodeBootId);
  TEST_ASSERT_EQUAL_UINT32(kNoSampleSeq, ack.highestSampleSeq);
  // The frame was fresh and its tag was good, so its counter is used up.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(telemetry(rig, node, 20, kNodeBootId, 1, 5, kHubBootId - 1)));
}

void test_a_radio_that_refuses_an_ack_is_reported_and_the_samples_are_kept() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 0);
  rig.radio.fail = true;
  const LinkEvent event = telemetry(rig, node, 10, kNodeBootId, 1, 3);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Telemetry), kind(event));
  TEST_ASSERT_FALSE(event.replied);
  TEST_ASSERT_EQUAL_INT(2, rig.sink.calls);
}

void test_a_node_that_is_removed_is_no_longer_answered() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 0);
  TEST_ASSERT_TRUE(rig.registry.unpair(node.mac));
  const size_t sent = rig.radio.count;
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(telemetry(rig, node, 10, kNodeBootId, 1, 1)));
  TEST_ASSERT_EQUAL_UINT(sent, rig.radio.count);
}

void test_a_frame_from_an_address_that_no_node_has_is_ignored_and_costs_no_limit() {
  Rig rig;
  NodeSim zero = makeNode(1);
  memset(zero.mac, 0, kMacSize);
  NodeSim group = makeNode(1);
  const uint8_t groupMac[kMacSize] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  memcpy(group.mac, groupMac, kMacSize);
  for (int i = 0; i < 10; i++) {
    TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(hello(rig, zero, 0)));
    TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(hello(rig, group, 0)));
    TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(request(rig, group, 0)));
  }
  TEST_ASSERT_EQUAL_UINT(0, rig.radio.count);
  // The five answers of the window are still there for a real node.
  for (uint8_t i = 1; i <= 5; i++) {
    TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, makeNode(i), 0)));
  }
}

void test_a_hub_with_no_boot_number_is_mute() {
  Rig rig(0);
  NodeSim node = makeNode(1);
  rig.window.press(0);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(hello(rig, node, 0)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(request(rig, node, 0)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(telemetry(rig, node, 0, 1, 0, 0, 0)));
  TEST_ASSERT_EQUAL_INT(0, rig.radio.attempts);
  TEST_ASSERT_FALSE(rig.pairing.pending());
}

void test_a_hub_with_no_boot_number_sends_no_command_and_no_result_is_taken() {
  Rig rig(0);
  NodeSim node = makeNode(1);
  // The node is stored directly, because a hub without a boot number cannot pair one through the frames.
  uint8_t key[kPairingKeySize] = {1, 2, 3};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::Ok), static_cast<int>(rig.registry.pair(kKindPlug, node.mac, key)));
  memcpy(node.key, key, sizeof(node.key));
  TEST_ASSERT_FALSE(rig.link.sendCommand(node.mac, kNodeBootId, 1, 1, 1));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(result(rig, node, 0, kNodeBootId, 1, 1, 1, 0)));
  TEST_ASSERT_EQUAL_INT(0, rig.radio.attempts);
}

void test_a_frame_that_the_radio_refuses_still_uses_up_its_counter() {
  Rig rig;
  const NodeSim node = makeNode(1);
  rig.radio.fail = true;
  hello(rig, node, 0);
  rig.radio.fail = false;
  hello(rig, node, 10);
  TEST_ASSERT_EQUAL_UINT(1, rig.radio.count);
  Header header;
  TEST_ASSERT_TRUE(decodeHeader(rig.radio.sent[0].data, rig.radio.sent[0].length, &header));
  TEST_ASSERT_EQUAL_UINT32(1, header.counter);
}

void test_a_second_pair_request_is_busy_and_gets_no_answer() {
  Rig rig;
  const NodeSim first = makeNode(1);
  const NodeSim second = makeNode(2);
  rig.window.press(0);
  request(rig, first, 10);
  const LinkEvent event = request(rig, second, 20);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::PairRequest), kind(event));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(RequestResult::Busy), static_cast<int>(event.request));
  TEST_ASSERT_FALSE(event.replied);
  TEST_ASSERT_EQUAL_UINT(1, rig.radio.count);
}

void test_a_paired_node_is_served_while_another_node_is_pending() {
  Rig rig;
  NodeSim paired = makeNode(1);
  pairNode(rig, &paired, 0);
  NodeSim pending = makeNode(2);
  rig.window.press(1000);
  request(rig, pending, 1010);
  takeKey(rig, &pending);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Telemetry), kind(telemetry(rig, paired, 1020, kNodeBootId, 1, 4)));
  TEST_ASSERT_TRUE(rig.pairing.pending());
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::PairedNode), kind(telemetry(rig, pending, 1030, kNodeBootId, 0, 0)));
}

void test_a_first_frame_after_the_wait_is_over_is_ignored() {
  Rig rig;
  NodeSim node = makeNode(1);
  rig.window.press(0);
  request(rig, node, 10);
  takeKey(rig, &node);
  const size_t sent = rig.radio.count;
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(telemetry(rig, node, 10 + kPairingPendingMs, kNodeBootId, 0, 0)));
  TEST_ASSERT_FALSE(rig.registry.isPaired(node.mac));
  TEST_ASSERT_EQUAL_UINT(sent, rig.radio.count);
  TEST_ASSERT_EQUAL_INT(0, rig.sink.calls);
}

void test_an_authentic_frame_with_a_payload_that_does_not_decode_changes_nothing() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 0);
  const size_t sent = rig.radio.count;
  uint8_t frame[kMaxFrameSize];
  const size_t size = malformedTelemetryFrame(node.key, kNodeBootId, 6, frame);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(10, node.mac, frame, size)));
  TEST_ASSERT_EQUAL_UINT(sent, rig.radio.count);
  // The counter was not used up, so the same counter is still good with a payload that decodes.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Telemetry), kind(telemetry(rig, node, 20, kNodeBootId, 6, 1)));
}

void test_a_first_frame_with_a_payload_that_does_not_decode_pairs_the_node_without_an_ack() {
  Rig rig;
  NodeSim node = makeNode(1);
  rig.window.press(0);
  request(rig, node, 10);
  takeKey(rig, &node);
  uint8_t frame[kMaxFrameSize];
  const size_t size = malformedTelemetryFrame(node.key, kNodeBootId, 0, frame);
  const LinkEvent event = rig.link.onFrame(20, node.mac, frame, size);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::PairedNoAck), kind(event));
  TEST_ASSERT_EQUAL_STRING("PLUG-000001", event.nodeId);
  TEST_ASSERT_FALSE(event.replied);
  TEST_ASSERT_TRUE(rig.registry.isPaired(node.mac));
  TEST_ASSERT_EQUAL_UINT(1, rig.radio.count);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Telemetry), kind(telemetry(rig, node, 30, kNodeBootId, 1, 0)));
}

void test_a_flash_that_refuses_a_new_boot_value_is_reported_and_the_frame_is_dropped() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 0);
  const size_t sent = rig.radio.count;
  const int calls = rig.sink.calls;
  rig.store.failWrites = true;
  const LinkEvent event = telemetry(rig, node, 10, kNodeBootId + 1, 0, 1);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::StoreFailed), kind(event));
  TEST_ASSERT_FALSE(event.replied);
  TEST_ASSERT_EQUAL_UINT(sent, rig.radio.count);
  TEST_ASSERT_EQUAL_INT(calls, rig.sink.calls);
  // The node sends again, and the frame is taken once the flash works.
  rig.store.failWrites = false;
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Telemetry), kind(telemetry(rig, node, 20, kNodeBootId + 1, 0, 1)));
}

void test_a_fresh_result_of_a_paired_node_is_reported_with_its_number_and_answer_and_gets_no_reply() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 1000);
  const size_t sentBefore = rig.radio.count;
  const LinkEvent event = result(rig, node, 2000, kNodeBootId, 5, 321, 1);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::ResultReceived), kind(event));
  TEST_ASSERT_EQUAL_UINT16(321, event.commandNumber);
  TEST_ASSERT_TRUE(event.commandOk);
  TEST_ASSERT_EQUAL_STRING("PLUG-000001", event.nodeId);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(node.mac, event.mac, kMacSize);
  TEST_ASSERT_EQUAL_UINT32(kNodeBootId, event.frameBootId);
  TEST_ASSERT_EQUAL_UINT32(5, event.frameCounter);
  TEST_ASSERT_FALSE(event.replied);
  TEST_ASSERT_EQUAL_UINT(sentBefore, rig.radio.count);
  // The answer of a node that could not switch the relay is carried as it is.
  const LinkEvent failed = result(rig, node, 2100, kNodeBootId, 6, 322, 0);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::ResultReceived), kind(failed));
  TEST_ASSERT_FALSE(failed.commandOk);
  TEST_ASSERT_EQUAL_UINT16(322, failed.commandNumber);
}

void test_a_result_that_is_old_forged_unknown_or_malformed_is_ignored() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 1000);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::ResultReceived), kind(result(rig, node, 2000, kNodeBootId, 5, 1, 1)));
  const size_t sentBefore = rig.radio.count;
  // The same counter again, and a lower one, are replays.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(result(rig, node, 2001, kNodeBootId, 5, 1, 1)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(result(rig, node, 2002, kNodeBootId, 4, 1, 1)));
  // A frame with a damaged tag, and a frame sealed with another key.
  uint8_t frame[kMaxFrameSize];
  size_t size = resultFrame(node.key, kNodeBootId, 6, 2, 1, frame);
  frame[kHeaderSize] ^= 0x01;
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(2003, node.mac, frame, size)));
  uint8_t other[kAesKeySize];
  memcpy(other, node.key, sizeof(other));
  other[0] ^= 0x01;
  size = resultFrame(other, kNodeBootId, 7, 2, 1, frame);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(2004, node.mac, frame, size)));
  // A sender that is not paired.
  const NodeSim stranger = makeNode(9);
  size = resultFrame(node.key, kNodeBootId, 8, 2, 1, frame);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(2005, stranger.mac, frame, size)));
  // An answer that is not 0 or 1 does not decode.
  uint8_t payload[kResultSize];
  const Result good = {kHubBootId, 3, 1};
  TEST_ASSERT_EQUAL_UINT(kResultSize, encodeResult(good, payload, sizeof(payload)));
  payload[6] = 2;
  const Header header = {kProtocolVersion, FrameType::Result, kNodeBootId, 9};
  size = sealFrame(fakeSeal, nullptr, node.key, header, payload, sizeof(payload), frame, sizeof(frame));
  TEST_ASSERT_TRUE(size > 0);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(2006, node.mac, frame, size)));
  TEST_ASSERT_EQUAL_UINT(sentBefore, rig.radio.count);
  // None of these used up a counter of the node, so the next good result still passes.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::ResultReceived), kind(result(rig, node, 2007, kNodeBootId, 6, 4, 1)));
}

void test_a_result_with_another_hub_boot_number_gets_an_ack_with_the_right_one_and_is_not_reported() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 1000);
  const size_t sentBefore = rig.radio.count;
  const LinkEvent event = result(rig, node, 2000, kNodeBootId, 5, 77, 1, kHubBootId + 1);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HubBootMismatch), kind(event));
  TEST_ASSERT_TRUE(event.replied);
  TEST_ASSERT_EQUAL_UINT(sentBefore + 1, rig.radio.count);
  Header header;
  Ack ack;
  openAck(rig.radio.sent[rig.radio.count - 1], node.key, &header, &ack);
  TEST_ASSERT_EQUAL_UINT32(kHubBootId, header.bootId);
  TEST_ASSERT_EQUAL_UINT32(kNodeBootId, ack.nodeBootId);
  TEST_ASSERT_EQUAL_UINT32(kNoSampleSeq, ack.highestSampleSeq);
  // The frame counted as received, so the same frame again is a replay.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(result(rig, node, 2001, kNodeBootId, 5, 77, 1, kHubBootId + 1)));
}

void test_a_telemetry_event_names_the_sender_and_the_frame_that_a_command_can_echo() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 1000);
  const LinkEvent event = telemetry(rig, node, 2000, kNodeBootId, 4, 1);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Telemetry), kind(event));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(node.mac, event.mac, kMacSize);
  TEST_ASSERT_EQUAL_UINT32(kNodeBootId, event.frameBootId);
  TEST_ASSERT_EQUAL_UINT32(4, event.frameCounter);
  // The first frame of a pairing carries the same fields.
  Rig other;
  NodeSim second = makeNode(2);
  other.window.press(0);
  request(other, second, 10);
  takeKey(other, &second);
  const LinkEvent first = telemetry(other, second, 100, kNodeBootId, 0, 0);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::PairedNode), kind(first));
  TEST_ASSERT_EQUAL_UINT32(kNodeBootId, first.frameBootId);
  TEST_ASSERT_EQUAL_UINT32(0, first.frameCounter);
}

void test_a_result_with_a_higher_node_boot_number_passes_and_a_flash_that_refuses_it_drops_the_frame() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 1000);
  // A new boot of the node is a higher bootId, and its counter starts again.
  const LinkEvent next = result(rig, node, 2000, kNodeBootId + 1, 0, 8, 1);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::ResultReceived), kind(next));
  TEST_ASSERT_EQUAL_UINT32(kNodeBootId + 1, next.frameBootId);
  // The write of the next boot value fails, so the frame is dropped and the same frame passes when the flash works again.
  rig.store.failWriteNumber = rig.store.writes + 1;
  const LinkEvent refused = result(rig, node, 2100, kNodeBootId + 2, 0, 9, 1);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::StoreFailed), kind(refused));
  TEST_ASSERT_EQUAL_UINT16(0, refused.commandNumber);
  const LinkEvent again = result(rig, node, 2200, kNodeBootId + 2, 0, 9, 1);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::ResultReceived), kind(again));
  TEST_ASSERT_EQUAL_UINT16(9, again.commandNumber);
}

void test_a_result_sealed_with_the_key_of_another_node_is_ignored_and_every_event_names_the_sender() {
  Rig rig;
  NodeSim first = makeNode(1);
  NodeSim second = makeNode(2);
  pairNode(rig, &first, 1000);
  rig.radio.count = 0;
  pairNode(rig, &second, 5000);
  TEST_ASSERT_TRUE(memcmp(first.key, second.key, sizeof(first.key)) != 0);
  uint8_t frame[kMaxFrameSize];
  const size_t size = resultFrame(first.key, kNodeBootId, 9, 1, 1, frame);
  const LinkEvent forged = rig.link.onFrame(6000, second.mac, frame, size);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(forged));
  // An ignored event still names the address that sent the frame.
  TEST_ASSERT_EQUAL_UINT8_ARRAY(second.mac, forged.mac, kMacSize);
  const LinkEvent mismatch = result(rig, second, 6100, kNodeBootId, 10, 55, 1, kHubBootId + 1);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HubBootMismatch), kind(mismatch));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(second.mac, mismatch.mac, kMacSize);
  TEST_ASSERT_EQUAL_UINT16(0, mismatch.commandNumber);
  TEST_ASSERT_EQUAL_UINT32(0, mismatch.frameBootId);
}

void test_a_command_is_sealed_for_the_node_with_the_echo_the_number_and_the_action() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 1000);
  const size_t before = rig.radio.count;
  TEST_ASSERT_TRUE(rig.link.sendCommand(node.mac, kNodeBootId, 9, 4242, 1));
  TEST_ASSERT_EQUAL_UINT(before + 1, rig.radio.count);
  const SentFrame &sent = rig.radio.sent[before];
  TEST_ASSERT_EQUAL_UINT8_ARRAY(node.mac, sent.mac, kMacSize);
  Header header;
  Command command;
  openCommand(sent, node.key, &header, &command);
  TEST_ASSERT_EQUAL_UINT32(kHubBootId, header.bootId);
  TEST_ASSERT_EQUAL_UINT32(kNodeBootId, command.echoBootId);
  TEST_ASSERT_EQUAL_UINT32(9, command.echoCounter);
  TEST_ASSERT_EQUAL_UINT16(4242, command.number);
  TEST_ASSERT_EQUAL_UINT8(1, command.action);
  // The counter is the one that the ACKs use too, so it rises with every frame that the hub sends.
  const uint32_t first = header.counter;
  TEST_ASSERT_TRUE(rig.link.sendCommand(node.mac, kNodeBootId, 10, 4243, 0));
  openCommand(rig.radio.sent[before + 1], node.key, &header, &command);
  TEST_ASSERT_EQUAL_UINT32(first + 1, header.counter);
  TEST_ASSERT_EQUAL_UINT8(0, command.action);
  const LinkEvent acked = telemetry(rig, node, 3000, kNodeBootId, 11, 1);
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Telemetry), kind(acked));
  Header ackHeader;
  Ack ack;
  openAck(rig.radio.sent[rig.radio.count - 1], node.key, &ackHeader, &ack);
  TEST_ASSERT_EQUAL_UINT32(first + 2, ackHeader.counter);
}

void test_a_command_is_refused_for_an_unpaired_node_a_bad_action_and_a_radio_that_refuses() {
  Rig rig;
  NodeSim node = makeNode(1);
  pairNode(rig, &node, 1000);
  const size_t before = rig.radio.count;
  const NodeSim stranger = makeNode(9);
  TEST_ASSERT_FALSE(rig.link.sendCommand(stranger.mac, kNodeBootId, 1, 1, 1));
  TEST_ASSERT_FALSE(rig.link.sendCommand(node.mac, kNodeBootId, 1, 1, 2));
  TEST_ASSERT_EQUAL_UINT(before, rig.radio.count);
  // A refusing radio makes the call false, and the counter of that frame is used up, so the next frame has a higher one.
  rig.radio.fail = true;
  TEST_ASSERT_FALSE(rig.link.sendCommand(node.mac, kNodeBootId, 1, 1, 1));
  rig.radio.fail = false;
  TEST_ASSERT_TRUE(rig.link.sendCommand(node.mac, kNodeBootId, 1, 2, 1));
  Header header;
  Command command;
  openCommand(rig.radio.sent[before], node.key, &header, &command);
  TEST_ASSERT_EQUAL_UINT32(before + 1, header.counter);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_hello_is_answered_with_a_hello_ack_to_the_sender);
  RUN_TEST(test_the_hub_answers_five_hello_frames_in_a_second_and_then_stops);
  RUN_TEST(test_one_address_gets_two_answers_in_a_second_and_its_refused_frames_do_not_use_the_global_limit);
  RUN_TEST(test_the_table_of_hello_senders_replaces_the_address_with_the_oldest_window_first);
  RUN_TEST(test_a_full_table_of_hello_senders_forgets_the_oldest_window_and_the_newest_keep_their_count);
  RUN_TEST(test_the_hello_limit_works_across_the_wrap_of_the_clock);
  RUN_TEST(test_a_radio_that_refuses_the_hello_ack_is_reported);
  RUN_TEST(test_frames_that_are_not_valid_or_not_for_the_hub_are_ignored);
  RUN_TEST(test_frames_of_a_hub_to_node_type_are_ignored_by_type);
  RUN_TEST(test_a_pair_request_in_an_open_window_gets_a_pair_accept);
  RUN_TEST(test_a_pair_request_that_is_refused_gets_no_answer);
  RUN_TEST(test_the_first_frame_pairs_the_node_stores_its_samples_and_gets_an_ack);
  RUN_TEST(test_a_first_frame_with_another_hub_boot_number_pairs_nothing_and_gets_no_ack);
  RUN_TEST(test_a_radio_that_refuses_the_first_ack_leaves_the_node_paired);
  RUN_TEST(test_a_flash_that_refuses_the_pairing_pairs_nothing_and_sends_nothing);
  RUN_TEST(test_a_boot_value_that_cannot_be_stored_leaves_the_node_paired_without_an_ack);
  RUN_TEST(test_a_frame_that_does_not_verify_does_not_end_a_pairing);
  RUN_TEST(test_a_fresh_frame_of_a_paired_node_is_stored_and_acknowledged);
  RUN_TEST(test_the_hub_counter_grows_with_every_frame_that_it_sends);
  RUN_TEST(test_a_replayed_or_older_frame_is_ignored);
  RUN_TEST(test_a_frame_of_a_node_that_is_not_paired_is_ignored);
  RUN_TEST(test_a_frame_that_does_not_verify_changes_nothing_and_does_not_use_up_the_counter);
  RUN_TEST(test_a_frame_with_another_hub_boot_number_gets_an_ack_with_the_right_one_and_no_sample_is_taken);
  RUN_TEST(test_a_radio_that_refuses_an_ack_is_reported_and_the_samples_are_kept);
  RUN_TEST(test_a_node_that_is_removed_is_no_longer_answered);
  RUN_TEST(test_a_frame_from_an_address_that_no_node_has_is_ignored_and_costs_no_limit);
  RUN_TEST(test_a_hub_with_no_boot_number_is_mute);
  RUN_TEST(test_a_frame_that_the_radio_refuses_still_uses_up_its_counter);
  RUN_TEST(test_a_second_pair_request_is_busy_and_gets_no_answer);
  RUN_TEST(test_a_paired_node_is_served_while_another_node_is_pending);
  RUN_TEST(test_a_first_frame_after_the_wait_is_over_is_ignored);
  RUN_TEST(test_an_authentic_frame_with_a_payload_that_does_not_decode_changes_nothing);
  RUN_TEST(test_a_first_frame_with_a_payload_that_does_not_decode_pairs_the_node_without_an_ack);
  RUN_TEST(test_a_flash_that_refuses_a_new_boot_value_is_reported_and_the_frame_is_dropped);
  RUN_TEST(test_a_fresh_result_of_a_paired_node_is_reported_with_its_number_and_answer_and_gets_no_reply);
  RUN_TEST(test_a_result_that_is_old_forged_unknown_or_malformed_is_ignored);
  RUN_TEST(test_a_result_with_another_hub_boot_number_gets_an_ack_with_the_right_one_and_is_not_reported);
  RUN_TEST(test_a_telemetry_event_names_the_sender_and_the_frame_that_a_command_can_echo);
  RUN_TEST(test_a_hub_with_no_boot_number_sends_no_command_and_no_result_is_taken);
  RUN_TEST(test_a_result_with_a_higher_node_boot_number_passes_and_a_flash_that_refuses_it_drops_the_frame);
  RUN_TEST(test_a_result_sealed_with_the_key_of_another_node_is_ignored_and_every_event_names_the_sender);
  RUN_TEST(test_a_command_is_sealed_for_the_node_with_the_echo_the_number_and_the_action);
  RUN_TEST(test_a_command_is_refused_for_an_unpaired_node_a_bad_action_and_a_radio_that_refuses);
  return UNITY_END();
}
