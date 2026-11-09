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

void test_a_full_pool_drops_the_oldest_sample() {
  SampleStore store;
  for (uint32_t i = 0; i < kSamplePoolSize + 3; i++) {
    sendOne(store, 1000 + i, kMacA, kBoot, sampleWith(i));
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
    sendOne(store, 1000 + i, (i % 2 == 0) ? kMacA : kMacB, kBoot, sampleWith(i));
  }
  const size_t before = store.size();
  TEST_ASSERT_EQUAL_UINT(kSamplePoolSize, before);
  store.forget(kMacA);
  TEST_ASSERT_EQUAL_UINT(before / 2, store.size());
  // Only the samples of B are left, and they are still in the order of their arrival.
  uint32_t last = 0;
  for (size_t i = 0; i < store.size(); i++) {
    TEST_ASSERT_EQUAL_UINT8_ARRAY(kMacB, store.macOf(store.at(i)), kMacSize);
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
    Sample sample = sampleWith(i);
    sample.ageMs = (i % 2 == 0) ? 0 : 1000;
    sendOne(store, 5000, kMacA, kBoot, sample);
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
    sendOne(store, 1000 + i, (i % 2 == 0) ? kMacA : kMacB, kBoot, sampleWith(i));
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
  RUN_TEST(test_a_full_pool_drops_the_oldest_sample);
  RUN_TEST(test_discard_oldest_takes_samples_from_the_front);
  RUN_TEST(test_forget_drops_the_samples_and_the_numbers_of_a_node_and_keeps_the_others_in_order);
  RUN_TEST(test_forget_works_when_the_pool_has_wrapped_around);
  RUN_TEST(test_the_table_holds_64_nodes_and_refuses_a_65th_until_one_is_forgotten);
  RUN_TEST(test_a_frame_with_a_bad_power_and_a_good_sample_acknowledges_both);
  RUN_TEST(test_expire_drops_samples_that_were_held_too_long_whatever_their_place_in_the_pool);
  RUN_TEST(test_expire_works_across_the_wrap_of_the_clock);
  RUN_TEST(test_expire_works_when_the_pool_has_wrapped_around);
  RUN_TEST(test_a_full_pool_works_with_forget_and_a_push_after_a_discard_at_the_wrap);
  return UNITY_END();
}
