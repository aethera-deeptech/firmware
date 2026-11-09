#include <string.h>
#include <unity.h>

#include "sample-store.h"

using namespace wattlink;

namespace {

const uint8_t kMacA[kMacSize] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x01};
const uint8_t kMacB[kMacSize] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x02};
constexpr uint32_t kBoot = 3;

// A good sample. The tests change what they need.
Sample sampleWith(uint32_t seq) {
  Sample sample;
  memset(&sample, 0, sizeof(sample));
  sample.sampleSeq = seq;
  sample.ageMs = 500;
  sample.voltageDeciV = 2305;
  sample.currentCentiA = 123;
  sample.powerDeciW = 2840;
  sample.energyWh = 15200;
  sample.frequencyDeciHz = 500;
  sample.powerFactorCenti = 87;
  sample.condition = 0;
  sample.relay = 1;
  return sample;
}

uint32_t send(SampleStore &store, uint32_t nowMs, const uint8_t mac[kMacSize], uint32_t bootId, const Sample *samples, size_t count) {
  return store.onSamples(nowMs, mac, bootId, samples, count);
}

uint32_t sendOne(SampleStore &store, uint32_t nowMs, const uint8_t mac[kMacSize], uint32_t bootId, const Sample &sample) {
  return send(store, nowMs, mac, bootId, &sample, 1);
}

void makeMac(uint8_t mac[kMacSize], uint32_t number) {
  mac[0] = 0x40;
  mac[1] = 0x00;
  mac[2] = 0x00;
  mac[3] = static_cast<uint8_t>(number >> 16);
  mac[4] = static_cast<uint8_t>(number >> 8);
  mac[5] = static_cast<uint8_t>(number);
}

// Sample number index of a ring of 16 nodes that take turns, as many nodes do, so that no node reaches its share while the pool is filled and pushed round.
// The nodes are the numbers 1 to 16, so the first is kMacA and the second is kMacB.
constexpr uint32_t kRingNodes = 16;

uint32_t sendInRing(SampleStore &store, uint32_t nowMs, uint32_t index, const Sample *override = nullptr) {
  uint8_t mac[kMacSize];
  makeMac(mac, 1 + index % kRingNodes);
  const Sample sample = override != nullptr ? *override : sampleWith(index / kRingNodes);
  return sendOne(store, nowMs, mac, kBoot, sample);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_the_limits_are_the_ones_of_the_contract() {
  TEST_ASSERT_EQUAL_UINT32(600000, kMaxPowerDeciW);  // 60000 W
  TEST_ASSERT_EQUAL_UINT(5000, kMaxVoltageDeciV);    // 500 V
  TEST_ASSERT_EQUAL_UINT(20000, kMaxCurrentCentiA);  // 200 A
  TEST_ASSERT_EQUAL_UINT(1000, kMaxFrequencyDeciHz);  // 100 Hz
  TEST_ASSERT_EQUAL_UINT(100, kMaxPowerFactorCenti);
  TEST_ASSERT_EQUAL_UINT(3, kMaxCondition);
  TEST_ASSERT_EQUAL_UINT32(86400000u, kMaxSampleAgeMs);
  TEST_ASSERT_EQUAL_UINT32(86400000u - 600000u, kKeepSampleMs);
}

void test_a_good_sample_is_stored_in_the_units_of_the_frame() {
  SampleStore store;
  const Sample sample = sampleWith(7);
  TEST_ASSERT_EQUAL_UINT32(7, sendOne(store, 10000, kMacA, kBoot, sample));
  TEST_ASSERT_EQUAL_UINT(1, store.size());
  const StoredSample &stored = store.at(0);
  TEST_ASSERT_EQUAL_UINT32(10000 - 500, stored.capturedMs);
  TEST_ASSERT_EQUAL_UINT32(2840, stored.powerDeciW);
  TEST_ASSERT_EQUAL_UINT32(15200, stored.energyWh);
  TEST_ASSERT_EQUAL_UINT(2305, stored.voltageDeciV);
  TEST_ASSERT_EQUAL_UINT(123, stored.currentCentiA);
  TEST_ASSERT_EQUAL_UINT(500, stored.frequencyDeciHz);
  TEST_ASSERT_EQUAL_UINT8(87, stored.powerFactorCenti);
  TEST_ASSERT_EQUAL_UINT8(0, stored.condition);
  TEST_ASSERT_EQUAL_UINT8(1, stored.relay);
  TEST_ASSERT_EQUAL_UINT8(0x7F, stored.fields);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kMacA, store.macOf(stored), kMacSize);
  TEST_ASSERT_EQUAL_UINT32(1, store.counts().stored);
}

void test_the_time_of_a_sample_is_the_arrival_minus_the_age_and_survives_the_wrap_of_the_clock() {
  SampleStore store;
  Sample sample = sampleWith(1);
  sample.ageMs = 1000;
  sendOne(store, 400, kMacA, kBoot, sample);
  TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFu - 599, store.at(0).capturedMs);
}

void test_a_frame_with_new_and_old_samples_stores_only_the_new_ones() {
  SampleStore store;
  Sample first[3] = {sampleWith(0), sampleWith(1), sampleWith(2)};
  TEST_ASSERT_EQUAL_UINT32(2, send(store, 1000, kMacA, kBoot, first, 3));
  Sample second[3] = {sampleWith(1), sampleWith(2), sampleWith(3)};
  TEST_ASSERT_EQUAL_UINT32(3, send(store, 11000, kMacA, kBoot, second, 3));
  TEST_ASSERT_EQUAL_UINT(4, store.size());
  TEST_ASSERT_EQUAL_UINT32(2, store.counts().repeats);
}

void test_a_frame_of_repeats_is_acknowledged_with_the_highest_number_that_is_held() {
  SampleStore store;
  Sample first[3] = {sampleWith(5), sampleWith(6), sampleWith(7)};
  send(store, 1000, kMacA, kBoot, first, 3);
  const Sample old = sampleWith(5);
  TEST_ASSERT_EQUAL_UINT32(7, sendOne(store, 11000, kMacA, kBoot, old));
  TEST_ASSERT_EQUAL_UINT(3, store.size());
  TEST_ASSERT_EQUAL_UINT32(1, store.counts().repeats);
}

void test_the_acknowledged_number_is_never_below_the_highest_of_the_frame() {
  SampleStore store;
  // The first frame after a start of the hub is new even when its numbers are high, and a gap before it is lost for good.
  Sample first[2] = {sampleWith(40), sampleWith(41)};
  TEST_ASSERT_EQUAL_UINT32(41, send(store, 1000, kMacA, kBoot, first, 2));
  // A frame of repeats and a new sample after a gap gives the number of the new sample, and a frame of repeats only gives the held number.
  Sample mixed[3] = {sampleWith(40), sampleWith(41), sampleWith(45)};
  TEST_ASSERT_EQUAL_UINT32(45, send(store, 2000, kMacA, kBoot, mixed, 3));
  Sample old[2] = {sampleWith(40), sampleWith(41)};
  TEST_ASSERT_EQUAL_UINT32(45, send(store, 3000, kMacA, kBoot, old, 2));
  TEST_ASSERT_EQUAL_UINT(3, store.size());
}

void test_samples_out_of_order_in_one_frame_are_handled_by_the_running_highest_number() {
  SampleStore store;
  Sample frame[3] = {sampleWith(5), sampleWith(3), sampleWith(6)};
  TEST_ASSERT_EQUAL_UINT32(6, send(store, 1000, kMacA, kBoot, frame, 3));
  TEST_ASSERT_EQUAL_UINT(2, store.size());
  TEST_ASSERT_EQUAL_UINT32(1, store.counts().repeats);
}

void test_a_new_boot_of_the_node_counts_from_zero_again() {
  SampleStore store;
  sendOne(store, 1000, kMacA, kBoot, sampleWith(90));
  TEST_ASSERT_EQUAL_UINT32(0, sendOne(store, 2000, kMacA, kBoot + 1, sampleWith(0)));
  TEST_ASSERT_EQUAL_UINT(2, store.size());
  // The same number again in the new boot is a repeat.
  TEST_ASSERT_EQUAL_UINT32(0, sendOne(store, 3000, kMacA, kBoot + 1, sampleWith(0)));
  TEST_ASSERT_EQUAL_UINT(2, store.size());
  // The store follows the boot that it sees last, so a lower boot number starts the count again too.
  TEST_ASSERT_EQUAL_UINT32(0, sendOne(store, 4000, kMacA, kBoot - 1, sampleWith(0)));
  TEST_ASSERT_EQUAL_UINT(3, store.size());
}

void test_after_a_start_of_the_hub_the_first_sample_is_new_whatever_its_number() {
  SampleStore store;
  TEST_ASSERT_EQUAL_UINT32(0, sendOne(store, 1000, kMacA, kBoot, sampleWith(0)));
  // The store of a restarted hub knows nothing of the node, so a high number is new and is acknowledged.
  SampleStore restarted;
  TEST_ASSERT_EQUAL_UINT32(500, sendOne(restarted, 1000, kMacA, kBoot, sampleWith(500)));
  TEST_ASSERT_EQUAL_UINT(1, restarted.size());
}

void test_two_nodes_do_not_share_their_numbers() {
  SampleStore store;
  sendOne(store, 1000, kMacA, kBoot, sampleWith(10));
  TEST_ASSERT_EQUAL_UINT32(2, sendOne(store, 1000, kMacB, kBoot, sampleWith(2)));
  TEST_ASSERT_EQUAL_UINT(2, store.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kMacA, store.macOf(store.at(0)), kMacSize);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kMacB, store.macOf(store.at(1)), kMacSize);
}

void test_a_sample_without_a_power_or_with_a_power_out_of_range_is_dropped_but_counts_as_received() {
  SampleStore store;
  Sample missing = sampleWith(1);
  missing.powerDeciW = kNoU32;
  TEST_ASSERT_EQUAL_UINT32(1, sendOne(store, 1000, kMacA, kBoot, missing));
  Sample high = sampleWith(2);
  high.powerDeciW = kMaxPowerDeciW + 1;
  TEST_ASSERT_EQUAL_UINT32(2, sendOne(store, 1000, kMacA, kBoot, high));
  TEST_ASSERT_EQUAL_UINT(0, store.size());
  TEST_ASSERT_EQUAL_UINT32(2, store.counts().rejected);
  // The power at the limit is good, and the dropped samples are not new any more.
  Sample edge = sampleWith(3);
  edge.powerDeciW = kMaxPowerDeciW;
  sendOne(store, 1000, kMacA, kBoot, edge);
  TEST_ASSERT_EQUAL_UINT(1, store.size());
  sendOne(store, 1000, kMacA, kBoot, missing);
  TEST_ASSERT_EQUAL_UINT(1, store.size());
}

void test_a_value_that_is_missing_or_out_of_range_is_left_out_and_the_sample_stays() {
  SampleStore store;
  Sample sample = sampleWith(1);
  sample.voltageDeciV = kNoU16;
  sample.currentCentiA = kMaxCurrentCentiA + 1;
  sample.energyWh = kNoU32;
  sample.frequencyDeciHz = kMaxFrequencyDeciHz + 1;
  sample.powerFactorCenti = kNoU8;
  sample.condition = kMaxCondition + 1;
  sample.relay = kNoU8;
  sendOne(store, 1000, kMacA, kBoot, sample);
  TEST_ASSERT_EQUAL_UINT(1, store.size());
  TEST_ASSERT_EQUAL_UINT8(0, store.at(0).fields);
  TEST_ASSERT_EQUAL_UINT32(2840, store.at(0).powerDeciW);
}

void test_each_value_at_its_limit_is_kept() {
  SampleStore store;
  Sample sample = sampleWith(1);
  sample.voltageDeciV = kMaxVoltageDeciV;
  sample.currentCentiA = kMaxCurrentCentiA;
  sample.frequencyDeciHz = kMaxFrequencyDeciHz;
  sample.powerFactorCenti = kMaxPowerFactorCenti;
  sample.condition = kMaxCondition;
  sample.relay = 0;
  sample.energyWh = 0;
  sendOne(store, 1000, kMacA, kBoot, sample);
  TEST_ASSERT_EQUAL_UINT8(0x7F, store.at(0).fields);
  TEST_ASSERT_EQUAL_UINT8(0, store.at(0).relay);
}

void test_a_relay_value_of_2_is_left_out() {
  SampleStore store;
  Sample sample = sampleWith(1);
  sample.relay = 2;
  sendOne(store, 1000, kMacA, kBoot, sample);
  TEST_ASSERT_EQUAL_UINT8(0x7F & ~kHasRelay, store.at(0).fields);
}

void test_a_sample_older_than_a_day_is_dropped_and_one_of_exactly_a_day_is_kept() {
  SampleStore store;
  Sample old = sampleWith(1);
  old.ageMs = kMaxSampleAgeMs + 1;
  TEST_ASSERT_EQUAL_UINT32(1, sendOne(store, 100000, kMacA, kBoot, old));
  TEST_ASSERT_EQUAL_UINT(0, store.size());
  Sample edge = sampleWith(2);
  edge.ageMs = kMaxSampleAgeMs;
  sendOne(store, 100000, kMacA, kBoot, edge);
  TEST_ASSERT_EQUAL_UINT(1, store.size());
}

void test_a_sample_number_of_all_bits_is_never_taken_or_acknowledged() {
  SampleStore store;
  TEST_ASSERT_EQUAL_UINT32(kNoSampleSeq, sendOne(store, 1000, kMacA, kBoot, sampleWith(kNoU32)));
  TEST_ASSERT_EQUAL_UINT(0, store.size());
  TEST_ASSERT_EQUAL_UINT32(1, store.counts().rejected);
  // With other samples in the frame, the answer is the number of the last good one.
  Sample frame[2] = {sampleWith(4), sampleWith(kNoU32)};
  TEST_ASSERT_EQUAL_UINT32(4, send(store, 1000, kMacA, kBoot, frame, 2));
  TEST_ASSERT_EQUAL_UINT(1, store.size());
}

void test_every_sample_gets_the_next_serial_and_a_drop_or_a_forget_leaves_gaps_but_no_repeat() {
  SampleStore store;
  sendOne(store, 1000, kMacA, kBoot, sampleWith(0));
  sendOne(store, 1001, kMacB, kBoot, sampleWith(0));
  sendOne(store, 1002, kMacA, kBoot, sampleWith(1));
  TEST_ASSERT_EQUAL_UINT32(1, store.at(0).serial);
  TEST_ASSERT_EQUAL_UINT32(2, store.at(1).serial);
  TEST_ASSERT_EQUAL_UINT32(3, store.at(2).serial);
  store.forget(kMacB);
  TEST_ASSERT_EQUAL_UINT(2, store.size());
  TEST_ASSERT_EQUAL_UINT32(1, store.at(0).serial);
  TEST_ASSERT_EQUAL_UINT32(3, store.at(1).serial);
  sendOne(store, 1003, kMacA, kBoot, sampleWith(2));
  TEST_ASSERT_EQUAL_UINT32(4, store.at(2).serial);
  // A sample that is dropped as a repeat or as bad does not use a serial.
  sendOne(store, 1004, kMacA, kBoot, sampleWith(2));
  sendOne(store, 1005, kMacA, kBoot, sampleWith(3));
  TEST_ASSERT_EQUAL_UINT32(5, store.at(3).serial);
}

void test_a_full_pool_drops_the_oldest_sample() {
  SampleStore store;
  for (uint32_t i = 0; i < kSamplePoolSize + 3; i++) {
    sendInRing(store, 1000 + i, i);
  }
  TEST_ASSERT_EQUAL_UINT(kSamplePoolSize, store.size());
  TEST_ASSERT_EQUAL_UINT32(3, store.counts().overflowed);
  // The first three are gone, and the order is kept across the wrap of the pool.
  TEST_ASSERT_EQUAL_UINT32(1000 + 3 - 500, store.at(0).capturedMs);
  TEST_ASSERT_EQUAL_UINT32(1000 + kSamplePoolSize + 2 - 500, store.at(kSamplePoolSize - 1).capturedMs);
}

void test_discard_oldest_takes_samples_from_the_front() {
  SampleStore store;
  for (uint32_t i = 0; i < 5; i++) {
    sendOne(store, 1000 + i, kMacA, kBoot, sampleWith(i));
  }
  store.discardOldest(2);
  TEST_ASSERT_EQUAL_UINT(3, store.size());
  TEST_ASSERT_EQUAL_UINT32(1002 - 500, store.at(0).capturedMs);
  store.discardOldest(99);
  TEST_ASSERT_EQUAL_UINT(0, store.size());
}

void test_forget_drops_the_samples_and_the_numbers_of_a_node_and_keeps_the_others_in_order() {
  SampleStore store;
  sendOne(store, 1000, kMacA, kBoot, sampleWith(0));
  sendOne(store, 1001, kMacB, kBoot, sampleWith(0));
  sendOne(store, 1002, kMacA, kBoot, sampleWith(1));
  sendOne(store, 1003, kMacB, kBoot, sampleWith(1));
  store.forget(kMacA);
  TEST_ASSERT_EQUAL_UINT(2, store.size());
  TEST_ASSERT_EQUAL_UINT32(1001 - 500, store.at(0).capturedMs);
  TEST_ASSERT_EQUAL_UINT32(1003 - 500, store.at(1).capturedMs);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kMacB, store.macOf(store.at(0)), kMacSize);
  // A node that comes back starts again, so its sample number 0 is new.
  TEST_ASSERT_EQUAL_UINT32(0, sendOne(store, 2000, kMacA, kBoot, sampleWith(0)));
  TEST_ASSERT_EQUAL_UINT(3, store.size());
  // An unknown node is no error.
  const uint8_t unknown[kMacSize] = {0x40, 1, 2, 3, 4, 5};
  store.forget(unknown);
  TEST_ASSERT_EQUAL_UINT(3, store.size());
}

void test_forget_works_when_the_pool_has_wrapped_around() {
  SampleStore store;
  // Fill the pool and push it round, so the oldest sample sits near the end of the array.
  for (uint32_t i = 0; i < kSamplePoolSize + 10; i++) {
    sendInRing(store, 1000 + i, i);
  }
  const size_t before = store.size();
  TEST_ASSERT_EQUAL_UINT(kSamplePoolSize, before);
  size_t ofA = 0;
  for (size_t i = 0; i < before; i++) {
    if (memcmp(store.macOf(store.at(i)), kMacA, kMacSize) == 0) {
      ofA++;
    }
  }
  TEST_ASSERT_TRUE(ofA > 0);
  store.forget(kMacA);
  TEST_ASSERT_EQUAL_UINT(before - ofA, store.size());
  // None of the samples of A are left, and the others are still in the order of their arrival.
  uint32_t last = 0;
  for (size_t i = 0; i < store.size(); i++) {
    TEST_ASSERT_TRUE(memcmp(store.macOf(store.at(i)), kMacA, kMacSize) != 0);
    TEST_ASSERT_TRUE(i == 0 || store.at(i).capturedMs > last);
    last = store.at(i).capturedMs;
  }
}

void test_the_table_holds_64_nodes_and_refuses_a_65th_until_one_is_forgotten() {
  SampleStore store;
  uint8_t mac[kMacSize];
  for (uint32_t i = 1; i <= kMaxNodes; i++) {
    makeMac(mac, i);
    TEST_ASSERT_EQUAL_UINT32(0, sendOne(store, 1000, mac, kBoot, sampleWith(0)));
  }
  makeMac(mac, 100);
  TEST_ASSERT_EQUAL_UINT32(kNoSampleSeq, sendOne(store, 1000, mac, kBoot, sampleWith(0)));
  TEST_ASSERT_EQUAL_UINT32(1, store.counts().refused);
  TEST_ASSERT_EQUAL_UINT(kMaxNodes, store.size());
  uint8_t gone[kMacSize];
  makeMac(gone, 5);
  store.forget(gone);
  TEST_ASSERT_EQUAL_UINT32(0, sendOne(store, 1000, mac, kBoot, sampleWith(0)));
}

void test_a_frame_with_a_bad_power_and_a_good_sample_acknowledges_both() {
  SampleStore store;
  Sample frame[2] = {sampleWith(1), sampleWith(2)};
  frame[0].powerDeciW = kNoU32;
  TEST_ASSERT_EQUAL_UINT32(2, send(store, 1000, kMacA, kBoot, frame, 2));
  TEST_ASSERT_EQUAL_UINT(1, store.size());
  TEST_ASSERT_EQUAL_UINT32(1, store.counts().rejected);
}

void test_expire_drops_samples_that_were_held_too_long_whatever_their_place_in_the_pool() {
  SampleStore store;
  // A node can set any age, so the pool is not in the order of the age. The first sample is the newest.
  Sample recent = sampleWith(1);
  recent.ageMs = 0;
  sendOne(store, 100000, kMacA, kBoot, recent);
  Sample old = sampleWith(2);
  old.ageMs = kMaxSampleAgeMs;
  sendOne(store, 100000, kMacA, kBoot, old);
  TEST_ASSERT_EQUAL_UINT(2, store.size());
  // The second sample was 24 hours old at its arrival, which is past the keep window, so it goes at once and the first stays.
  TEST_ASSERT_EQUAL_UINT(1, store.expire(100000));
  TEST_ASSERT_EQUAL_UINT(1, store.size());
  TEST_ASSERT_EQUAL_UINT32(100000, store.at(0).capturedMs);
  TEST_ASSERT_EQUAL_UINT(0, store.expire(100000 + kKeepSampleMs));
  TEST_ASSERT_EQUAL_UINT(1, store.expire(100000 + kKeepSampleMs + 1));
  TEST_ASSERT_EQUAL_UINT(0, store.size());
  TEST_ASSERT_EQUAL_UINT32(2, store.counts().expired);
}

void test_expire_with_a_clock_value_from_before_the_arrival_drops_nothing() {
  SampleStore store;
  Sample fresh = sampleWith(1);
  fresh.ageMs = 0;
  sendOne(store, 100050, kMacA, kBoot, fresh);
  // nowMs was read 50 ms before the sample came in, so the sample is not 49 days old.
  TEST_ASSERT_EQUAL_UINT(0, store.expire(100000));
  TEST_ASSERT_EQUAL_UINT(1, store.size());
}

// Sends count samples of one node with the numbers first, first + 1 and so on, eight in a frame as the radio carries them. The power names the number.
void sendRun(SampleStore &store, uint32_t nowMs, const uint8_t mac[kMacSize], uint32_t first, uint32_t count) {
  uint32_t sent = 0;
  while (sent < count) {
    Sample frame[8];
    size_t n = 0;
    while (n < 8 && sent < count) {
      frame[n] = sampleWith(first + sent);
      frame[n].powerDeciW = first + sent;
      n++;
      sent++;
    }
    send(store, nowMs, mac, kBoot, frame, n);
  }
}

void test_a_node_holds_at_most_its_share_and_keeps_its_newest_samples() {
  SampleStore store;
  sendRun(store, 10000, kMacA, 0, 1000);
  TEST_ASSERT_EQUAL_UINT(kMaxSamplesPerNode, store.size());
  TEST_ASSERT_EQUAL_UINT32(1000 - kMaxSamplesPerNode, store.counts().capped);
  TEST_ASSERT_EQUAL_UINT32(0, store.counts().overflowed);
  TEST_ASSERT_EQUAL_UINT32(1000 - kMaxSamplesPerNode, store.at(0).powerDeciW);
  TEST_ASSERT_EQUAL_UINT32(999, store.at(store.size() - 1).powerDeciW);
}

void test_a_node_that_floods_cannot_push_out_more_than_its_share_of_the_samples_of_the_others() {
  SampleStore store;
  // Eight nodes fill the pool, each with its share.
  const size_t others = kSamplePoolSize / kMaxSamplesPerNode;
  for (size_t i = 0; i < others; i++) {
    uint8_t mac[kMacSize];
    makeMac(mac, 100 + static_cast<uint32_t>(i));
    sendRun(store, 10000, mac, 0, kMaxSamplesPerNode);
  }
  TEST_ASSERT_EQUAL_UINT(kSamplePoolSize, store.size());
  // A ninth node sends many more samples than the pool holds.
  uint8_t flooder[kMacSize];
  makeMac(flooder, 999);
  sendRun(store, 20000, flooder, 0, 5000);
  TEST_ASSERT_EQUAL_UINT(kSamplePoolSize, store.size());
  size_t held[2] = {0, 0};
  for (size_t i = 0; i < store.size(); i++) {
    held[memcmp(store.macOf(store.at(i)), flooder, kMacSize) == 0 ? 1 : 0]++;
  }
  // The flooder holds its share and no more, and the others lost that much at most, which is what it took while the pool was full.
  TEST_ASSERT_EQUAL_UINT(kMaxSamplesPerNode, held[1]);
  TEST_ASSERT_EQUAL_UINT(kSamplePoolSize - kMaxSamplesPerNode, held[0]);
  TEST_ASSERT_EQUAL_UINT32(kMaxSamplesPerNode, store.counts().overflowed);
}

// Fills the share of a node, and then sends one sample more. A count that is too high shows as a push-out that should not be, and a count that is too low
// shows as no push-out that should be.
void sendUntilCappedOnce(SampleStore &store, uint32_t nowMs, const uint8_t mac[kMacSize], uint32_t *next) {
  const uint32_t before = store.counts().capped;
  size_t guard = 0;
  while (store.counts().capped == before && guard++ < 2 * kMaxSamplesPerNode) {
    sendRun(store, nowMs, mac, *next, 1);
    (*next)++;
  }
  TEST_ASSERT_EQUAL_UINT32(before + 1, store.counts().capped);
}

void test_the_count_of_a_node_follows_every_way_that_samples_leave_the_pool() {
  SampleStore store;
  uint32_t next = 0;
  sendRun(store, 10000, kMacA, next, kMaxSamplesPerNode);
  next += kMaxSamplesPerNode;
  TEST_ASSERT_EQUAL_UINT32(0, store.counts().capped);
  // The next sample is pushed out as soon as the share is full.
  sendUntilCappedOnce(store, 10000, kMacA, &next);
  TEST_ASSERT_EQUAL_UINT(kMaxSamplesPerNode, store.size());
  // Discarding makes room for exactly that many samples, and then the share is full again.
  store.discardOldest(10);
  sendRun(store, 10000, kMacA, next, 10);
  next += 10;
  TEST_ASSERT_EQUAL_UINT32(1, store.counts().capped);
  sendUntilCappedOnce(store, 10000, kMacA, &next);
  // A removal by a rule, as the batch code does after an acknowledgement.
  const size_t removed = store.removeWhere([](const StoredSample &sample) { return (sample.powerDeciW % 4) == 0; });
  TEST_ASSERT_TRUE(removed > 0);
  sendRun(store, 10000, kMacA, next, removed);
  next += static_cast<uint32_t>(removed);
  TEST_ASSERT_EQUAL_UINT32(2, store.counts().capped);
  sendUntilCappedOnce(store, 10000, kMacA, &next);
  // Expiry. All samples are old now, so the share is empty and it takes the full share again before the next push-out.
  store.expire(10000 + kKeepSampleMs + 600);
  TEST_ASSERT_EQUAL_UINT(0, store.size());
  sendRun(store, 10000 + kKeepSampleMs + 700, kMacA, next, kMaxSamplesPerNode);
  next += kMaxSamplesPerNode;
  TEST_ASSERT_EQUAL_UINT32(3, store.counts().capped);
  sendUntilCappedOnce(store, 10000 + kKeepSampleMs + 700, kMacA, &next);
  // A node that is forgotten starts with an empty share, also when the same address comes back.
  store.forget(kMacA);
  uint32_t again = 0;
  sendRun(store, 20000, kMacA, again, kMaxSamplesPerNode);
  again += kMaxSamplesPerNode;
  TEST_ASSERT_EQUAL_UINT32(4, store.counts().capped);
  sendUntilCappedOnce(store, 20000, kMacA, &again);
}

void test_a_sample_number_that_no_node_reaches_or_a_step_that_no_node_makes_is_dropped_and_the_count_stays() {
  SampleStore store;
  const Sample first[3] = {sampleWith(0), sampleWith(1), sampleWith(2)};
  TEST_ASSERT_EQUAL_UINT32(2, send(store, 1000, kMacA, kBoot, first, 3));
  // A huge number is dropped and not acknowledged, and the next sample of the node is new as before.
  TEST_ASSERT_EQUAL_UINT32(2, sendOne(store, 1100, kMacA, kBoot, sampleWith(0xFFFFFFFEu)));
  TEST_ASSERT_EQUAL_UINT32(2, sendOne(store, 1100, kMacA, kBoot, sampleWith(kMaxSampleSeq + 1)));
  TEST_ASSERT_EQUAL_UINT32(3, sendOne(store, 1200, kMacA, kBoot, sampleWith(3)));
  TEST_ASSERT_EQUAL_UINT(4, store.size());
  TEST_ASSERT_EQUAL_UINT32(2, store.counts().rejected);
  // The largest step is taken and one more is not.
  TEST_ASSERT_EQUAL_UINT32(3 + kMaxSampleSeqStep, sendOne(store, 1300, kMacA, kBoot, sampleWith(3 + kMaxSampleSeqStep)));
  TEST_ASSERT_EQUAL_UINT32(3 + kMaxSampleSeqStep, sendOne(store, 1400, kMacA, kBoot, sampleWith(3 + 2 * kMaxSampleSeqStep + 1)));
  TEST_ASSERT_EQUAL_UINT(5, store.size());
  TEST_ASSERT_EQUAL_UINT32(3, store.counts().rejected);
  // A repeat is still a repeat and not a step.
  TEST_ASSERT_EQUAL_UINT32(3 + kMaxSampleSeqStep, sendOne(store, 1500, kMacA, kBoot, sampleWith(2)));
  TEST_ASSERT_EQUAL_UINT32(1, store.counts().repeats);
}

void test_the_first_sample_of_a_boot_may_be_the_largest_number_that_a_node_reaches_and_no_larger() {
  SampleStore store;
  TEST_ASSERT_EQUAL_UINT32(kNoSampleSeq, sendOne(store, 1000, kMacA, kBoot, sampleWith(kMaxSampleSeq + 1)));
  TEST_ASSERT_EQUAL_UINT(0, store.size());
  TEST_ASSERT_EQUAL_UINT32(kMaxSampleSeq, sendOne(store, 1100, kMacA, kBoot, sampleWith(kMaxSampleSeq)));
  // A new boot of the node starts from nothing, so the step limit does not hold it to the old count.
  TEST_ASSERT_EQUAL_UINT32(5, sendOne(store, 1200, kMacA, kBoot + 1, sampleWith(5)));
  TEST_ASSERT_EQUAL_UINT(2, store.size());
}

void test_expire_works_across_the_wrap_of_the_clock() {
  SampleStore store;
  Sample sample = sampleWith(1);
  sample.ageMs = 0;
  sendOne(store, 0xFFFFFF00u, kMacA, kBoot, sample);
  const uint32_t edge = 0xFFFFFF00u + kKeepSampleMs;
  TEST_ASSERT_EQUAL_UINT(0, store.expire(edge));
  TEST_ASSERT_EQUAL_UINT(1, store.expire(edge + 1));
}

void test_expire_works_when_the_pool_has_wrapped_around() {
  SampleStore store;
  for (uint32_t i = 0; i < kSamplePoolSize + 10; i++) {
    Sample sample = sampleWith(i / kRingNodes);
    sample.ageMs = (i % 2 == 0) ? 0 : 1000;
    sendInRing(store, 5000, i, &sample);
  }
  TEST_ASSERT_EQUAL_UINT(kSamplePoolSize, store.size());
  // Samples that arrived with an age of 1000 ms were taken at 4000 ms, and the others at 5000 ms. At this time only the first kind is past the window.
  const size_t removed = store.expire(5000 + kKeepSampleMs);
  TEST_ASSERT_EQUAL_UINT(kSamplePoolSize / 2, removed);
  for (size_t i = 0; i < store.size(); i++) {
    TEST_ASSERT_EQUAL_UINT32(5000, store.at(i).capturedMs);
  }
}

void test_a_full_pool_works_with_forget_and_a_push_after_a_discard_at_the_wrap() {
  SampleStore store;
  for (uint32_t i = 0; i < kSamplePoolSize + 5; i++) {
    sendInRing(store, 1000 + i, i);
  }
  store.forget(kMacA);
  const size_t half = store.size();
  // Room for a new sample, and the order is still the order of arrival.
  sendOne(store, 9000, kMacB, kBoot, sampleWith(100000));
  TEST_ASSERT_EQUAL_UINT(half + 1, store.size());
  TEST_ASSERT_EQUAL_UINT32(9000 - 500, store.at(store.size() - 1).capturedMs);
  store.discardOldest(half);
  TEST_ASSERT_EQUAL_UINT(1, store.size());
  sendOne(store, 9100, kMacB, kBoot, sampleWith(100001));
  TEST_ASSERT_EQUAL_UINT(2, store.size());
  TEST_ASSERT_EQUAL_UINT32(9100 - 500, store.at(1).capturedMs);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_limits_are_the_ones_of_the_contract);
  RUN_TEST(test_a_good_sample_is_stored_in_the_units_of_the_frame);
  RUN_TEST(test_the_time_of_a_sample_is_the_arrival_minus_the_age_and_survives_the_wrap_of_the_clock);
  RUN_TEST(test_a_frame_with_new_and_old_samples_stores_only_the_new_ones);
  RUN_TEST(test_a_frame_of_repeats_is_acknowledged_with_the_highest_number_that_is_held);
  RUN_TEST(test_the_acknowledged_number_is_never_below_the_highest_of_the_frame);
  RUN_TEST(test_samples_out_of_order_in_one_frame_are_handled_by_the_running_highest_number);
  RUN_TEST(test_a_new_boot_of_the_node_counts_from_zero_again);
  RUN_TEST(test_after_a_start_of_the_hub_the_first_sample_is_new_whatever_its_number);
  RUN_TEST(test_two_nodes_do_not_share_their_numbers);
  RUN_TEST(test_a_sample_without_a_power_or_with_a_power_out_of_range_is_dropped_but_counts_as_received);
  RUN_TEST(test_a_value_that_is_missing_or_out_of_range_is_left_out_and_the_sample_stays);
  RUN_TEST(test_each_value_at_its_limit_is_kept);
  RUN_TEST(test_a_relay_value_of_2_is_left_out);
  RUN_TEST(test_a_sample_older_than_a_day_is_dropped_and_one_of_exactly_a_day_is_kept);
  RUN_TEST(test_a_sample_number_of_all_bits_is_never_taken_or_acknowledged);
  RUN_TEST(test_every_sample_gets_the_next_serial_and_a_drop_or_a_forget_leaves_gaps_but_no_repeat);
  RUN_TEST(test_a_full_pool_drops_the_oldest_sample);
  RUN_TEST(test_discard_oldest_takes_samples_from_the_front);
  RUN_TEST(test_forget_drops_the_samples_and_the_numbers_of_a_node_and_keeps_the_others_in_order);
  RUN_TEST(test_forget_works_when_the_pool_has_wrapped_around);
  RUN_TEST(test_the_table_holds_64_nodes_and_refuses_a_65th_until_one_is_forgotten);
  RUN_TEST(test_a_frame_with_a_bad_power_and_a_good_sample_acknowledges_both);
  RUN_TEST(test_expire_drops_samples_that_were_held_too_long_whatever_their_place_in_the_pool);
  RUN_TEST(test_expire_with_a_clock_value_from_before_the_arrival_drops_nothing);
  RUN_TEST(test_a_node_holds_at_most_its_share_and_keeps_its_newest_samples);
  RUN_TEST(test_a_node_that_floods_cannot_push_out_more_than_its_share_of_the_samples_of_the_others);
  RUN_TEST(test_the_count_of_a_node_follows_every_way_that_samples_leave_the_pool);
  RUN_TEST(test_a_sample_number_that_no_node_reaches_or_a_step_that_no_node_makes_is_dropped_and_the_count_stays);
  RUN_TEST(test_the_first_sample_of_a_boot_may_be_the_largest_number_that_a_node_reaches_and_no_larger);
  RUN_TEST(test_expire_works_across_the_wrap_of_the_clock);
  RUN_TEST(test_expire_works_when_the_pool_has_wrapped_around);
  RUN_TEST(test_a_full_pool_works_with_forget_and_a_push_after_a_discard_at_the_wrap);
  return UNITY_END();
}
