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
  uint32_t onSamples(const uint8_t mac[kMacSize], uint32_t nodeBootId, const Sample *samples, size_t count) override {
    calls++;
    memcpy(lastMac, mac, kMacSize);
    lastBootId = nodeBootId;
    lastCount = count;
    firstSeq = samples[0].sampleSeq;
    return useAnswer ? answer : samples[count - 1].sampleSeq;
  }

  int calls = 0;
  uint8_t lastMac[kMacSize] = {0};
  uint32_t lastBootId = 0;
  size_t lastCount = 0;
  uint32_t firstSeq = 0;
  bool useAnswer = false;
  uint32_t answer = 0;
};

struct Rig {
  Rig()
      : registry(store),
        crypto{&knobs, fakeHmac, fakeOpen, fakeSeal, fakeGenerate, fakeAgree},
        pairing(window, registry, crypto, kHubMac, kHubBootId),
        link(pairing, registry, radio, sink, crypto, kHubBootId) {
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
  const NodeSim node = makeNode(1);
  for (int i = 0; i < 5; i++) {
    TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, node, 100 + i)));
  }
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, node, 500)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, node, 100 + kHelloWindowMs - 1)));
  TEST_ASSERT_EQUAL_UINT(5, rig.radio.count);
  // A new window starts one second after the first HELLO of the old one.
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, node, 100 + kHelloWindowMs)));
  TEST_ASSERT_EQUAL_UINT(6, rig.radio.count);
}

void test_the_hello_limit_works_across_the_wrap_of_the_clock() {
  Rig rig;
  const NodeSim node = makeNode(1);
  for (int i = 0; i < 5; i++) {
    TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, node, 0xFFFFFF00u)));
  }
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloLimited), kind(hello(rig, node, 0xFFFFFF10u)));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::HelloAnswered), kind(hello(rig, node, 0xFFFFFF00u + kHelloWindowMs)));
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

void test_frames_of_the_direction_hub_to_node_and_result_frames_are_ignored() {
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
  // an ACK and a RESULT sealed with the real key of the node
  uint8_t payload[kMaxPayloadSize];
  const Ack ack = {kNodeBootId, 0};
  size_t length = encodeAck(ack, payload, sizeof(payload));
  Header header = {kProtocolVersion, FrameType::Ack, kNodeBootId, 5};
  size = sealFrame(fakeSeal, nullptr, node.key, header, payload, length, frame, sizeof(frame));
  TEST_ASSERT_EQUAL_INT(kind(LinkEventKind::Ignored), kind(rig.link.onFrame(0, node.mac, frame, size)));
  const Result result = {kHubBootId, 1, 1};
  length = encodeResult(result, payload, sizeof(payload));
  header.type = FrameType::Result;
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

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_hello_is_answered_with_a_hello_ack_to_the_sender);
  RUN_TEST(test_the_hub_answers_five_hello_frames_in_a_second_and_then_stops);
  RUN_TEST(test_the_hello_limit_works_across_the_wrap_of_the_clock);
  RUN_TEST(test_a_radio_that_refuses_the_hello_ack_is_reported);
  RUN_TEST(test_frames_that_are_not_valid_or_not_for_the_hub_are_ignored);
  RUN_TEST(test_frames_of_the_direction_hub_to_node_and_result_frames_are_ignored);
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
  return UNITY_END();
}
