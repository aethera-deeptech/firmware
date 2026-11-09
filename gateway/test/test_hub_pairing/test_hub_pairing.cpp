#include <string.h>
#include <unity.h>

#include "../support/fake-crypto.h"
#include "hub-pairing.h"
#include "node-registry.h"

using namespace wattlink;
using namespace wattlink::testsupport;

namespace {

const uint8_t kHubMac[kMacSize] = {0x48, 0xe7, 0x29, 0xb6, 0xe0, 0x84};
constexpr uint32_t kHubBootId = 7;

// Everything that the hub side needs, wired as in the firmware.
struct Rig {
  explicit Rig(uint32_t hubBootId = kHubBootId)
      : registry(store), crypto{&knobs, fakeHmac, fakeOpen, fakeSeal, fakeGenerate, fakeAgree}, pairing(window, registry, crypto, kHubMac, hubBootId) {
    registry.load();
  }

  FakeStore store;
  NodeRegistry registry;
  PairingWindow window;
  Knobs knobs;
  HubCrypto crypto;
  HubPairing pairing;
};

// The node side of an exchange, as far as the tests need it.
struct NodeSim {
  uint8_t mac[kMacSize];
  uint8_t privateKey[kPublicKeySize];
  uint8_t publicKey[kPublicKeySize];
};

NodeSim makeNode(uint8_t number) {
  NodeSim node;
  const uint8_t mac[kMacSize] = {0x40, 0x00, 0x00, 0x00, 0x00, number};
  memcpy(node.mac, mac, kMacSize);
  for (size_t i = 0; i < kPublicKeySize; i++) {
    node.privateKey[i] = static_cast<uint8_t>(0x80 + number + i);
    node.publicKey[i] = node.privateKey[i] ^ kMask;
  }
  return node;
}

int result(RequestResult value) {
  return static_cast<int>(value);
}

int result(FirstFrameResult value) {
  return static_cast<int>(value);
}

RequestResult ask(Rig &rig, const NodeSim &node, uint32_t nowMs, RequestOutcome *outcome, NodeKind kind = NodeKind::Plug) {
  PairRequest request;
  request.kind = kind;
  memcpy(request.nodePublicKey, node.publicKey, kPublicKeySize);
  uint8_t payload[kPairRequestSize];
  TEST_ASSERT_EQUAL_UINT(kPairRequestSize, encodePairRequest(request, payload, sizeof(payload)));
  return rig.pairing.onPairRequest(nowMs, node.mac, payload, sizeof(payload), outcome);
}

// The key that the node derives from the answer of the hub, as the protocol says.
void nodeKey(Rig &rig, const NodeSim &node, const RequestOutcome &outcome, uint8_t key[kPairingKeySize]) {
  PairAccept accept;
  TEST_ASSERT_TRUE(decodePairAccept(outcome.acceptPayload, kPairAcceptSize, &accept));
  uint8_t shared[kSharedSecretSize];
  TEST_ASSERT_TRUE(fakeAgree(&rig.knobs, node.privateKey, accept.hubPublicKey, shared));
  TEST_ASSERT_TRUE(derivePairingKey(fakeHmac, &rig.knobs, shared, node.publicKey, accept.hubPublicKey, node.mac, kHubMac, key));
}

size_t sealed(const uint8_t key[kAesKeySize], FrameType type, uint32_t bootId, uint32_t counter, uint8_t *out, size_t capacity,
              uint32_t hubBootId = kHubBootId) {
  uint8_t payload[kMaxPayloadSize];
  size_t length = 0;
  if (type == FrameType::Telemetry) {
    Sample sample = {};
    length = encodeTelemetryPayload(hubBootId, &sample, 1, payload, sizeof(payload));
  } else if (type == FrameType::Result) {
    Result value = {0, 1, 1};
    length = encodeResult(value, payload, sizeof(payload));
  } else {
    Ack value = {bootId, 0};
    length = encodeAck(value, payload, sizeof(payload));
  }
  Header header = {kProtocolVersion, type, bootId, counter};
  const size_t size = sealFrame(fakeSeal, nullptr, key, header, payload, length, out, capacity);
  TEST_ASSERT_TRUE(size > 0);
  return size;
}

// Opens the window, and makes the node ask at the given time. The node key is returned for the frames of the tests.
void startPairing(Rig &rig, const NodeSim &node, uint32_t nowMs, uint8_t key[kPairingKeySize]) {
  rig.window.press(nowMs);
  RequestOutcome outcome;
  TEST_ASSERT_EQUAL_INT(result(RequestResult::Accepted), result(ask(rig, node, nowMs, &outcome)));
  nodeKey(rig, node, outcome, key);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_a_request_in_an_open_window_is_accepted_and_names_the_node() {
  Rig rig;
  const NodeSim node = makeNode(1);
  rig.window.press(1000);
  RequestOutcome outcome;
  TEST_ASSERT_EQUAL_INT(result(RequestResult::Accepted), result(ask(rig, node, 2000, &outcome)));
  TEST_ASSERT_EQUAL_STRING("PLUG-000001", outcome.nodeId);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::Ok), static_cast<int>(outcome.refusal));
  PairAccept accept;
  TEST_ASSERT_TRUE(decodePairAccept(outcome.acceptPayload, kPairAcceptSize, &accept));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(node.publicKey, accept.nodePublicKey, kPublicKeySize);
  TEST_ASSERT_TRUE(rig.window.isPairing(2000));
  TEST_ASSERT_TRUE(rig.pairing.pending());
  // Nothing is stored before the first frame proves the key.
  TEST_ASSERT_FALSE(rig.registry.isPaired(node.mac));
  TEST_ASSERT_EQUAL_INT(0, rig.store.writes);
}

void test_the_name_of_a_clamp_starts_with_clamp() {
  Rig rig;
  rig.window.press(0);
  RequestOutcome outcome;
  TEST_ASSERT_EQUAL_INT(result(RequestResult::Accepted), result(ask(rig, makeNode(2), 10, &outcome, NodeKind::Clamp)));
  TEST_ASSERT_EQUAL_STRING("CLAMP-000002", outcome.nodeId);
}

void test_the_hub_and_the_node_derive_the_same_key_and_the_first_frame_pairs_the_node() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 1000, key);

  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 3, 0, frame, sizeof(frame));
  FirstFrame first;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::Committed), result(rig.pairing.onFrame(5000, node.mac, frame, size, &first)));
  TEST_ASSERT_TRUE(rig.registry.isPaired(node.mac));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(key, rig.registry.keyOf(node.mac), kPairingKeySize);
  TEST_ASSERT_EQUAL_UINT8(kKindPlug, rig.registry.kindOf(node.mac));
  TEST_ASSERT_EQUAL_UINT32(3, first.header.bootId);
  TEST_ASSERT_EQUAL_UINT32(0, first.header.counter);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FrameType::Telemetry), static_cast<int>(first.header.type));
  TEST_ASSERT_EQUAL_UINT(kTelemetryPrefixSize + kSampleSize, first.payloadLength);
  // One pairing is all that the window allows, and the key is not kept outside the registry.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Closed), static_cast<int>(rig.window.state()));
  TEST_ASSERT_FALSE(rig.pairing.pending());
  // The frame counts as received, so the same frame again is a replay.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FrameCheck::Stale), static_cast<int>(rig.registry.check(node.mac, 3, 0)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FrameCheck::Accepted), static_cast<int>(rig.registry.check(node.mac, 3, 1)));
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::NotPending), result(rig.pairing.onFrame(5001, node.mac, frame, size, &first)));
}

void test_a_closed_window_ignores_a_request() {
  Rig rig;
  RequestOutcome outcome;
  TEST_ASSERT_EQUAL_INT(result(RequestResult::NotOpen), result(ask(rig, makeNode(1), 0, &outcome)));
  TEST_ASSERT_EQUAL_STRING("", outcome.nodeId);
  TEST_ASSERT_FALSE(rig.pairing.pending());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Closed), static_cast<int>(rig.window.state()));
}

void test_a_window_whose_time_is_over_ignores_a_request() {
  Rig rig;
  rig.window.press(0);
  RequestOutcome outcome;
  TEST_ASSERT_EQUAL_INT(result(RequestResult::NotOpen), result(ask(rig, makeNode(1), kPairingWindowMs, &outcome)));
  TEST_ASSERT_FALSE(rig.pairing.pending());
}

void test_a_second_request_during_a_pairing_is_busy_and_the_first_pairing_goes_on() {
  Rig rig;
  const NodeSim first = makeNode(1);
  const NodeSim second = makeNode(2);
  uint8_t key[kPairingKeySize];
  startPairing(rig, first, 0, key);
  RequestOutcome outcome;
  TEST_ASSERT_EQUAL_INT(result(RequestResult::Busy), result(ask(rig, second, 100, &outcome)));
  TEST_ASSERT_EQUAL_STRING("", outcome.nodeId);
  TEST_ASSERT_TRUE(rig.window.isPairing(100));

  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::Committed), result(rig.pairing.onFrame(200, first.mac, frame, size, &out)));
  TEST_ASSERT_TRUE(rig.registry.isPaired(first.mac));
  TEST_ASSERT_FALSE(rig.registry.isPaired(second.mac));
}

void test_a_payload_that_is_not_a_pair_request_is_ignored_and_the_window_stays_open() {
  Rig rig;
  rig.window.press(0);
  RequestOutcome outcome;
  const uint8_t shortPayload[10] = {1};
  TEST_ASSERT_EQUAL_INT(result(RequestResult::Malformed), result(rig.pairing.onPairRequest(10, makeNode(1).mac, shortPayload, sizeof(shortPayload), &outcome)));
  uint8_t badKind[kPairRequestSize] = {3};
  TEST_ASSERT_EQUAL_INT(result(RequestResult::Malformed), result(rig.pairing.onPairRequest(10, makeNode(1).mac, badKind, sizeof(badKind), &outcome)));
  TEST_ASSERT_TRUE(rig.window.isOpen(10));
  TEST_ASSERT_FALSE(rig.pairing.pending());
}

void test_a_node_that_is_paired_is_refused_and_the_window_stays_open() {
  Rig rig;
  const NodeSim node = makeNode(1);
  const uint8_t key[kNodeKeySize] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::Ok), static_cast<int>(rig.registry.pair(kKindPlug, node.mac, key)));
  rig.window.press(0);
  RequestOutcome outcome;
  TEST_ASSERT_EQUAL_INT(result(RequestResult::Refused), result(ask(rig, node, 10, &outcome)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::AlreadyPaired), static_cast<int>(outcome.refusal));
  TEST_ASSERT_TRUE(rig.window.isOpen(10));
  TEST_ASSERT_FALSE(rig.pairing.pending());
  // No key was made for a node that is refused.
  TEST_ASSERT_EQUAL_UINT8(1, rig.knobs.next);
}

void test_a_full_registry_refuses_a_new_node_and_a_bad_address_is_refused() {
  Rig rig;
  const uint8_t key[kNodeKeySize] = {9};
  for (uint8_t i = 1; i <= kMaxNodes; i++) {
    const NodeSim other = makeNode(i);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::Ok), static_cast<int>(rig.registry.pair(kKindPlug, other.mac, key)));
  }
  rig.window.press(0);
  RequestOutcome outcome;
  TEST_ASSERT_EQUAL_INT(result(RequestResult::Refused), result(ask(rig, makeNode(100), 10, &outcome)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::RegistryFull), static_cast<int>(outcome.refusal));

  NodeSim zero = makeNode(1);
  memset(zero.mac, 0, kMacSize);
  TEST_ASSERT_EQUAL_INT(result(RequestResult::Refused), result(ask(rig, zero, 10, &outcome)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::InvalidMac), static_cast<int>(outcome.refusal));
}

void test_a_failure_of_the_key_exchange_changes_nothing_and_leaves_the_window_open() {
  const NodeSim node = makeNode(1);
  for (int knob = 0; knob < 4; knob++) {
    Rig rig;
    rig.window.press(0);
    rig.knobs.failGenerate = knob == 0;
    rig.knobs.failAgree = knob == 1;
    rig.knobs.zeroShared = knob == 2;
    rig.knobs.failHmac = knob == 3;
    RequestOutcome outcome;
    TEST_ASSERT_EQUAL_INT(result(RequestResult::CryptoFailed), result(ask(rig, node, 10, &outcome)));
    TEST_ASSERT_EQUAL_STRING("", outcome.nodeId);
    const uint8_t zeros[kPairAcceptSize] = {0};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(zeros, outcome.acceptPayload, kPairAcceptSize);
    TEST_ASSERT_TRUE(rig.window.isOpen(10));
    TEST_ASSERT_FALSE(rig.pairing.pending());
  }
}

void test_a_frame_that_does_not_verify_changes_nothing_and_a_good_frame_still_pairs() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 0, key);

  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  uint8_t bad[kMaxFrameSize];
  memcpy(bad, frame, size);
  bad[kHeaderSize + 2] ^= 0x01;
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::BadFrame), result(rig.pairing.onFrame(100, node.mac, bad, size, &out)));
  const uint8_t zeros[kMaxPayloadSize] = {0};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(zeros, out.payload, kMaxPayloadSize);
  TEST_ASSERT_TRUE(rig.pairing.pending());
  TEST_ASSERT_TRUE(rig.window.isPairing(100));
  TEST_ASSERT_FALSE(rig.registry.isPaired(node.mac));

  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::Committed), result(rig.pairing.onFrame(200, node.mac, frame, size, &out)));
}

void test_a_frame_with_a_key_that_is_not_the_new_key_does_not_pair() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 0, key);
  uint8_t wrong[kPairingKeySize];
  memcpy(wrong, key, kPairingKeySize);
  wrong[0] ^= 0xFF;
  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(wrong, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::BadFrame), result(rig.pairing.onFrame(100, node.mac, frame, size, &out)));
  TEST_ASSERT_FALSE(rig.registry.isPaired(node.mac));
}

void test_a_frame_from_another_address_is_not_pending_and_the_pairing_goes_on() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 0, key);
  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::NotPending), result(rig.pairing.onFrame(100, makeNode(2).mac, frame, size, &out)));
  TEST_ASSERT_TRUE(rig.pairing.pending());
  TEST_ASSERT_FALSE(rig.registry.isPaired(node.mac));
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::Committed), result(rig.pairing.onFrame(200, node.mac, frame, size, &out)));
}

void test_a_frame_before_a_request_is_not_pending() {
  Rig rig;
  uint8_t frame[kMaxFrameSize];
  const uint8_t key[kPairingKeySize] = {1};
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::NotPending), result(rig.pairing.onFrame(0, makeNode(1).mac, frame, size, &out)));
}

void test_the_first_frame_must_come_within_thirty_seconds() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 1000, key);
  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::NotPending), result(rig.pairing.onFrame(1000 + kPairingPendingMs, node.mac, frame, size, &out)));
  TEST_ASSERT_FALSE(rig.pairing.pending());
  TEST_ASSERT_FALSE(rig.registry.isPaired(node.mac));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Closed), static_cast<int>(rig.window.state()));
}

void test_a_frame_in_the_last_millisecond_still_pairs() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 1000, key);
  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::Committed), result(rig.pairing.onFrame(1000 + kPairingPendingMs - 1, node.mac, frame, size, &out)));
}

void test_the_window_end_caps_the_wait_for_the_first_frame() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  rig.window.press(0);
  RequestOutcome outcome;
  TEST_ASSERT_EQUAL_INT(result(RequestResult::Accepted), result(ask(rig, node, kPairingWindowMs - 5000, &outcome)));
  nodeKey(rig, node, outcome, key);
  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::NotPending), result(rig.pairing.onFrame(kPairingWindowMs, node.mac, frame, size, &out)));
}

void test_a_boot_value_of_zero_and_other_types_do_not_pair() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 0, key);
  uint8_t frame[kMaxFrameSize];
  FirstFrame out;
  size_t size = sealed(key, FrameType::Telemetry, 0, 0, frame, sizeof(frame));
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::BadFrame), result(rig.pairing.onFrame(100, node.mac, frame, size, &out)));
  // A RESULT frame is made with the new key too, but only TELEMETRY proves a pairing.
  size = sealed(key, FrameType::Result, 1, 0, frame, sizeof(frame));
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::BadFrame), result(rig.pairing.onFrame(100, node.mac, frame, size, &out)));
  // A frame of the direction hub to node, made with the same key, is refused as well.
  size = sealed(key, FrameType::Ack, 1, 0, frame, sizeof(frame));
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::BadFrame), result(rig.pairing.onFrame(100, node.mac, frame, size, &out)));
  TEST_ASSERT_FALSE(rig.registry.isPaired(node.mac));
  TEST_ASSERT_TRUE(rig.pairing.pending());
}

void test_a_flash_that_refuses_the_pairing_pairs_nothing_and_closes_the_window() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 0, key);
  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  rig.store.failWrites = true;
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::PairFailed), result(rig.pairing.onFrame(100, node.mac, frame, size, &out)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::StoreFailed), static_cast<int>(out.pairResult));
  TEST_ASSERT_EQUAL_UINT(0, out.payloadLength);
  TEST_ASSERT_FALSE(rig.registry.isPaired(node.mac));
  TEST_ASSERT_FALSE(rig.pairing.pending());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Closed), static_cast<int>(rig.window.state()));
}

void test_a_boot_value_that_cannot_be_stored_leaves_the_node_paired_without_an_ack() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 0, key);
  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  rig.store.failWriteNumber = 2;  // the pairing is the first write, and the boot value is the second
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::PairedNoFrame), result(rig.pairing.onFrame(100, node.mac, frame, size, &out)));
  TEST_ASSERT_EQUAL_UINT(0, out.payloadLength);
  TEST_ASSERT_TRUE(rig.registry.isPaired(node.mac));
  TEST_ASSERT_FALSE(rig.pairing.pending());
  // The node sends the frame again, and the normal path accepts it once the flash works.
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FrameCheck::Accepted), static_cast<int>(rig.registry.accept(node.mac, 1, 1)));
}

void test_close_wipes_the_pairing_and_closes_the_window() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 0, key);
  rig.pairing.close();
  TEST_ASSERT_FALSE(rig.pairing.pending());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Closed), static_cast<int>(rig.window.state()));
  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::NotPending), result(rig.pairing.onFrame(10, node.mac, frame, size, &out)));
}

void test_update_wipes_the_pairing_when_the_time_is_over_and_the_owner_can_start_again() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 0, key);
  rig.pairing.update(kPairingPendingMs - 1);
  TEST_ASSERT_TRUE(rig.pairing.pending());
  rig.pairing.update(kPairingPendingMs);
  TEST_ASSERT_FALSE(rig.pairing.pending());

  // A new press opens a new window, and the new request makes a new key pair.
  rig.window.press(40000);
  RequestOutcome outcome;
  TEST_ASSERT_EQUAL_INT(result(RequestResult::Accepted), result(ask(rig, node, 40100, &outcome)));
  uint8_t second[kPairingKeySize];
  nodeKey(rig, node, outcome, second);
  TEST_ASSERT_TRUE(memcmp(key, second, kPairingKeySize) != 0);
}

void test_a_repeated_request_from_the_pending_node_is_busy_and_keeps_the_first_key() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 0, key);
  RequestOutcome outcome;
  TEST_ASSERT_EQUAL_INT(result(RequestResult::Busy), result(ask(rig, node, 100, &outcome)));
  TEST_ASSERT_EQUAL_UINT8(2, rig.knobs.next);  // no second key pair was made
  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::Committed), result(rig.pairing.onFrame(200, node.mac, frame, size, &out)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(key, rig.registry.keyOf(node.mac), kPairingKeySize);
}

void test_a_node_that_was_paired_during_the_wait_makes_the_first_frame_fail() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 0, key);
  const uint8_t other[kNodeKeySize] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::Ok), static_cast<int>(rig.registry.pair(kKindPlug, node.mac, other)));
  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::PairFailed), result(rig.pairing.onFrame(100, node.mac, frame, size, &out)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::AlreadyPaired), static_cast<int>(out.pairResult));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(other, rig.registry.keyOf(node.mac), kNodeKeySize);
  TEST_ASSERT_FALSE(rig.pairing.pending());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Closed), static_cast<int>(rig.window.state()));
}

void test_bad_frames_do_not_extend_the_wait() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 0, key);
  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  uint8_t bad[kMaxFrameSize];
  memcpy(bad, frame, size);
  bad[kHeaderSize] ^= 0x01;
  FirstFrame out;
  const uint32_t times[3] = {10000, 20000, kPairingPendingMs - 1};
  for (size_t i = 0; i < 3; i++) {
    TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::BadFrame), result(rig.pairing.onFrame(times[i], node.mac, bad, size, &out)));
  }
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::NotPending), result(rig.pairing.onFrame(kPairingPendingMs, node.mac, frame, size, &out)));
  TEST_ASSERT_FALSE(rig.registry.isPaired(node.mac));
}

void test_a_request_after_the_wait_ended_finds_the_window_closed_and_not_busy() {
  Rig rig;
  uint8_t key[kPairingKeySize];
  startPairing(rig, makeNode(1), 0, key);
  RequestOutcome outcome;
  TEST_ASSERT_EQUAL_INT(result(RequestResult::NotOpen), result(ask(rig, makeNode(2), kPairingPendingMs, &outcome)));
  TEST_ASSERT_FALSE(rig.pairing.pending());
}

void test_the_times_work_across_the_wrap_of_the_clock() {
  Rig rig;
  const NodeSim node = makeNode(1);
  rig.window.press(0xFFFFFF00u);
  RequestOutcome outcome;
  TEST_ASSERT_EQUAL_INT(result(RequestResult::Accepted), result(ask(rig, node, 0xFFFFFF10u, &outcome)));
  uint8_t key[kPairingKeySize];
  nodeKey(rig, node, outcome, key);
  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame));
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::Committed), result(rig.pairing.onFrame(0x00000100u, node.mac, frame, size, &out)));
}

void test_a_result_that_is_not_committed_clears_the_old_header_and_payload() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 0, key);
  uint8_t frame[kMaxFrameSize];
  const size_t size = sealed(key, FrameType::Telemetry, 5, 9, frame, sizeof(frame));
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::Committed), result(rig.pairing.onFrame(10, node.mac, frame, size, &out)));
  TEST_ASSERT_EQUAL_UINT32(5, out.header.bootId);
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::NotPending), result(rig.pairing.onFrame(20, node.mac, frame, size, &out)));
  TEST_ASSERT_EQUAL_UINT32(0, out.header.bootId);
  TEST_ASSERT_EQUAL_UINT32(0, out.header.counter);
  TEST_ASSERT_EQUAL_UINT(0, out.payloadLength);
}

void test_a_first_frame_with_another_hub_boot_number_does_not_pair() {
  Rig rig;
  const NodeSim node = makeNode(1);
  uint8_t key[kPairingKeySize];
  startPairing(rig, node, 0, key);
  uint8_t frame[kMaxFrameSize];
  FirstFrame out;
  size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame), kHubBootId + 1);
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::BadFrame), result(rig.pairing.onFrame(100, node.mac, frame, size, &out)));
  size = sealed(key, FrameType::Telemetry, 1, 1, frame, sizeof(frame), 0);
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::BadFrame), result(rig.pairing.onFrame(100, node.mac, frame, size, &out)));
  TEST_ASSERT_FALSE(rig.registry.isPaired(node.mac));
  TEST_ASSERT_TRUE(rig.pairing.pending());
  // The right number still pairs, so a frame with a wrong number does not cancel the pairing.
  size = sealed(key, FrameType::Telemetry, 1, 2, frame, sizeof(frame));
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::Committed), result(rig.pairing.onFrame(200, node.mac, frame, size, &out)));
}

void test_a_hub_with_no_boot_number_takes_no_request_and_no_first_frame() {
  Rig rig(0);
  const NodeSim node = makeNode(1);
  rig.window.press(0);
  RequestOutcome outcome;
  TEST_ASSERT_EQUAL_INT(result(RequestResult::NotOpen), result(ask(rig, node, 10, &outcome)));
  TEST_ASSERT_EQUAL_STRING("", outcome.nodeId);
  TEST_ASSERT_FALSE(rig.pairing.pending());
  TEST_ASSERT_TRUE(rig.window.isOpen(10));
  // No key was made for it.
  TEST_ASSERT_EQUAL_UINT8(1, rig.knobs.next);
  uint8_t frame[kMaxFrameSize];
  const uint8_t key[kPairingKeySize] = {1};
  const size_t size = sealed(key, FrameType::Telemetry, 1, 0, frame, sizeof(frame), 0);
  FirstFrame out;
  TEST_ASSERT_EQUAL_INT(result(FirstFrameResult::NotPending), result(rig.pairing.onFrame(20, node.mac, frame, size, &out)));
  TEST_ASSERT_EQUAL_UINT32(0, rig.pairing.hubBootId());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_request_in_an_open_window_is_accepted_and_names_the_node);
  RUN_TEST(test_the_name_of_a_clamp_starts_with_clamp);
  RUN_TEST(test_the_hub_and_the_node_derive_the_same_key_and_the_first_frame_pairs_the_node);
  RUN_TEST(test_a_closed_window_ignores_a_request);
  RUN_TEST(test_a_window_whose_time_is_over_ignores_a_request);
  RUN_TEST(test_a_second_request_during_a_pairing_is_busy_and_the_first_pairing_goes_on);
  RUN_TEST(test_a_payload_that_is_not_a_pair_request_is_ignored_and_the_window_stays_open);
  RUN_TEST(test_a_node_that_is_paired_is_refused_and_the_window_stays_open);
  RUN_TEST(test_a_full_registry_refuses_a_new_node_and_a_bad_address_is_refused);
  RUN_TEST(test_a_failure_of_the_key_exchange_changes_nothing_and_leaves_the_window_open);
  RUN_TEST(test_a_frame_that_does_not_verify_changes_nothing_and_a_good_frame_still_pairs);
  RUN_TEST(test_a_frame_with_a_key_that_is_not_the_new_key_does_not_pair);
  RUN_TEST(test_a_frame_from_another_address_is_not_pending_and_the_pairing_goes_on);
  RUN_TEST(test_a_frame_before_a_request_is_not_pending);
  RUN_TEST(test_the_first_frame_must_come_within_thirty_seconds);
  RUN_TEST(test_a_frame_in_the_last_millisecond_still_pairs);
  RUN_TEST(test_the_window_end_caps_the_wait_for_the_first_frame);
  RUN_TEST(test_a_boot_value_of_zero_and_other_types_do_not_pair);
  RUN_TEST(test_a_flash_that_refuses_the_pairing_pairs_nothing_and_closes_the_window);
  RUN_TEST(test_a_boot_value_that_cannot_be_stored_leaves_the_node_paired_without_an_ack);
  RUN_TEST(test_close_wipes_the_pairing_and_closes_the_window);
  RUN_TEST(test_update_wipes_the_pairing_when_the_time_is_over_and_the_owner_can_start_again);
  RUN_TEST(test_a_repeated_request_from_the_pending_node_is_busy_and_keeps_the_first_key);
  RUN_TEST(test_a_node_that_was_paired_during_the_wait_makes_the_first_frame_fail);
  RUN_TEST(test_bad_frames_do_not_extend_the_wait);
  RUN_TEST(test_a_request_after_the_wait_ended_finds_the_window_closed_and_not_busy);
  RUN_TEST(test_the_times_work_across_the_wrap_of_the_clock);
  RUN_TEST(test_a_result_that_is_not_committed_clears_the_old_header_and_payload);
  RUN_TEST(test_a_first_frame_with_another_hub_boot_number_does_not_pair);
  RUN_TEST(test_a_hub_with_no_boot_number_takes_no_request_and_no_first_frame);
  return UNITY_END();
}
