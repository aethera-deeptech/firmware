#include <string.h>
#include <unity.h>

#include <string>

#include "../support/fake-crypto.h"
#include "batch-builder.h"

using namespace wattlink;
using namespace wattlink::testsupport;

namespace {

constexpr uint32_t kEpoch = 1793618400;
constexpr uint32_t kNow = 10000;
constexpr uint32_t kBoot = 3;

const uint8_t kKey[kNodeKeySize] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

void makeMac(uint8_t mac[kMacSize], uint32_t number) {
  mac[0] = 0x40;
  mac[1] = 0x00;
  mac[2] = 0x00;
  mac[3] = static_cast<uint8_t>(number >> 16);
  mac[4] = static_cast<uint8_t>(number >> 8);
  mac[5] = static_cast<uint8_t>(number);
}

class BufferOutput : public BatchOutput {
 public:
  bool write(const char *text, size_t length) override {
    body.append(text, length);
    return true;
  }
  std::string body;
};

// Refuses every write after a number of bytes.
class FailingOutput : public BatchOutput {
 public:
  explicit FailingOutput(size_t limit) : limit_(limit) {}
  bool write(const char *, size_t length) override {
    if (written_ + length > limit_) {
      return false;
    }
    written_ += length;
    return true;
  }

 private:
  size_t limit_;
  size_t written_ = 0;
};

struct Rig {
  Rig() : registry(flash), builder(store, registry) { registry.load(); }

  void pairPlug(uint32_t number) {
    uint8_t mac[kMacSize];
    makeMac(mac, number);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::Ok), static_cast<int>(registry.pair(kKindPlug, mac, kKey)));
  }
  void pairClamp(uint32_t number) {
    uint8_t mac[kMacSize];
    makeMac(mac, number);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::Ok), static_cast<int>(registry.pair(kKindClamp, mac, kKey)));
  }

  FakeStore flash;
  NodeRegistry registry;
  SampleStore store;
  BatchBuilder builder;
};

Sample fullSample(uint32_t seq) {
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

// A sample with only a power, so the JSON is short.
Sample powerOnly(uint32_t seq, uint32_t powerDeciW, uint32_t ageMs = 0) {
  Sample sample = fullSample(seq);
  sample.ageMs = ageMs;
  sample.powerDeciW = powerDeciW;
  sample.voltageDeciV = kNoU16;
  sample.currentCentiA = kNoU16;
  sample.energyWh = kNoU32;
  sample.frequencyDeciHz = kNoU16;
  sample.powerFactorCenti = kNoU8;
  sample.condition = kNoU8;
  sample.relay = kNoU8;
  return sample;
}

void put(Rig &rig, uint32_t nowMs, uint32_t node, const Sample &sample, uint32_t bootId = kBoot) {
  uint8_t mac[kMacSize];
  makeMac(mac, node);
  rig.store.onSamples(nowMs, mac, bootId, &sample, 1);
}

BatchPrepare prepare(Rig &rig, uint32_t nowMs = kNow, uint32_t epoch = kEpoch, const RelayResult *results = nullptr, size_t count = 0) {
  return rig.builder.prepare(nowMs, epoch, results, count);
}

int result(BatchPrepare value) {
  return static_cast<int>(value);
}

std::string body(const Rig &rig) {
  BufferOutput out;
  TEST_ASSERT_TRUE(rig.builder.render(out));
  return out.body;
}

size_t countOf(const std::string &text, const char *piece) {
  size_t count = 0;
  const size_t length = strlen(piece);
  for (size_t at = text.find(piece); at != std::string::npos; at = text.find(piece, at + length)) {
    count++;
  }
  return count;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_the_limits_are_the_ones_of_the_contract() {
  TEST_ASSERT_EQUAL_UINT(16, kMaxBatchNodes);
  TEST_ASSERT_EQUAL_UINT(1000, kMaxBatchSamplesPerNode);
  TEST_ASSERT_EQUAL_UINT(5000, kMaxBatchSamples);
  TEST_ASSERT_EQUAL_UINT(64, kMaxBatchResults);
  TEST_ASSERT_EQUAL_UINT(262144, kMaxBodyBytes);
  TEST_ASSERT_EQUAL_UINT32(6000, kMinBatchGapMs);
  TEST_ASSERT_EQUAL_UINT32(1700000000u, kMinUnixSeconds);
  TEST_ASSERT_EQUAL_UINT(37, kResultIdSize);
}

void test_with_no_samples_there_is_no_batch() {
  Rig rig;
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::NoSamples), result(prepare(rig)));
}

void test_a_clock_that_is_not_set_holds_the_samples() {
  Rig rig;
  rig.pairPlug(1);
  put(rig, kNow, 1, fullSample(0));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::ClockNotSet), result(prepare(rig, kNow, kMinUnixSeconds - 1)));
  TEST_ASSERT_EQUAL_UINT(1, rig.store.size());
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow, kMinUnixSeconds + 600)));
}

void test_a_batch_waits_six_seconds_after_the_last_send() {
  Rig rig;
  rig.pairPlug(1);
  put(rig, kNow, 1, fullSample(0));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  rig.builder.markSent(kNow);
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::TooSoon), result(prepare(rig, kNow + kMinBatchGapMs - 1)));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow + kMinBatchGapMs)));
}

void test_a_sample_is_written_as_the_contract_says() {
  Rig rig;
  rig.pairPlug(1);
  put(rig, kNow, 1, fullSample(7));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  TEST_ASSERT_EQUAL_UINT32(1, rig.builder.seq());
  TEST_ASSERT_EQUAL_UINT(1, rig.builder.sampleCount());
  TEST_ASSERT_EQUAL_UINT(1, rig.builder.nodeCount());
  // The age of 500 ms rounds to 1 second, the energy goes from Wh to kWh, and the units of the frame become the units of the contract.
  TEST_ASSERT_EQUAL_STRING(
      "{\"seq\":1,\"sentAt\":1793618400,\"nodes\":[{\"nodeId\":\"PLUG-000001\",\"kind\":\"plug\",\"samples\":[{\"t\":1793618399,\"v\":230.5,\"i\":1.23,"
      "\"p\":284.0,\"e\":15.200,\"f\":50.0,\"pf\":0.87,\"a\":0,\"r\":1}]}]}",
      body(rig).c_str());
}

void test_the_size_of_the_body_is_the_size_that_is_written_and_the_body_is_the_same_every_time() {
  Rig rig;
  rig.pairPlug(1);
  put(rig, kNow, 1, fullSample(0));
  put(rig, kNow, 1, fullSample(1));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  const std::string first = body(rig);
  TEST_ASSERT_EQUAL_UINT(first.size(), rig.builder.bodySize());
  TEST_ASSERT_EQUAL_STRING(first.c_str(), body(rig).c_str());
}

void test_a_value_that_is_left_out_is_not_written_and_a_clamp_has_no_relay() {
  Rig rig;
  rig.pairClamp(2);
  put(rig, kNow, 2, powerOnly(0, 1000));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  TEST_ASSERT_EQUAL_STRING(
      "{\"seq\":1,\"sentAt\":1793618400,\"nodes\":[{\"nodeId\":\"CLAMP-000002\",\"kind\":\"clamp\",\"samples\":[{\"t\":1793618400,\"p\":100.0}]}]}",
      body(rig).c_str());
}

void test_the_limits_of_each_value_are_written_in_full() {
  Rig rig;
  rig.pairPlug(1);
  Sample sample = fullSample(0);
  sample.ageMs = 0;
  sample.voltageDeciV = kMaxVoltageDeciV;
  sample.currentCentiA = kMaxCurrentCentiA;
  sample.powerDeciW = kMaxPowerDeciW;
  sample.energyWh = 4294967294u;
  sample.frequencyDeciHz = kMaxFrequencyDeciHz;
  sample.powerFactorCenti = kMaxPowerFactorCenti;
  sample.condition = 3;
  sample.relay = 0;
  put(rig, kNow, 1, sample);
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  const std::string text = body(rig);
  TEST_ASSERT_TRUE(text.find("{\"t\":1793618400,\"v\":500.0,\"i\":200.00,\"p\":60000.0,\"e\":4294967.294,\"f\":100.0,\"pf\":1.00,\"a\":3,\"r\":0}") !=
                   std::string::npos);
  // A small value keeps its zeros, so 5 deciwatts is 0.5 and 3 Wh is 0.003 kWh.
  Rig other;
  other.pairPlug(1);
  Sample small = fullSample(0);
  small.ageMs = 0;
  small.powerDeciW = 5;
  small.energyWh = 3;
  small.currentCentiA = 7;
  put(other, kNow, 1, small);
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(other)));
  const std::string smallText = body(other);
  TEST_ASSERT_TRUE(smallText.find("\"i\":0.07,\"p\":0.5,\"e\":0.003,") != std::string::npos);
}

void test_the_time_of_a_sample_rounds_to_the_nearest_second_and_survives_the_wrap_of_the_clock() {
  Rig rig;
  rig.pairPlug(1);
  put(rig, kNow, 1, powerOnly(0, 10, 499));
  put(rig, kNow, 1, powerOnly(1, 10, 500));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  const std::string text = body(rig);
  TEST_ASSERT_TRUE(text.find("{\"t\":1793618400,\"p\":1.0},{\"t\":1793618399,\"p\":1.0}") != std::string::npos);

  Rig wrapped;
  wrapped.pairPlug(1);
  put(wrapped, 0xFFFFFF00u, 1, powerOnly(0, 10, 500));
  // The batch is made 1012 ms after the sample was taken, across the wrap of the clock.
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(wrapped, 0x00000100u)));
  TEST_ASSERT_TRUE(body(wrapped).find("{\"t\":1793618399,\"p\":1.0}") != std::string::npos);
}

void test_nodes_are_in_the_order_of_their_first_sample_and_samples_are_in_the_order_of_arrival() {
  Rig rig;
  rig.pairPlug(1);
  rig.pairPlug(2);
  put(rig, kNow, 1, powerOnly(0, 100));
  put(rig, kNow, 2, powerOnly(0, 200));
  put(rig, kNow, 1, powerOnly(1, 300));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  TEST_ASSERT_EQUAL_UINT(2, rig.builder.nodeCount());
  TEST_ASSERT_EQUAL_UINT(3, rig.builder.sampleCount());
  TEST_ASSERT_EQUAL_STRING(
      "{\"seq\":1,\"sentAt\":1793618400,\"nodes\":[{\"nodeId\":\"PLUG-000001\",\"kind\":\"plug\",\"samples\":[{\"t\":1793618400,\"p\":10.0},{\"t\":"
      "1793618400,\"p\":30.0}]},{\"nodeId\":\"PLUG-000002\",\"kind\":\"plug\",\"samples\":[{\"t\":1793618400,\"p\":20.0}]}]}",
      body(rig).c_str());
}

void test_a_batch_has_at_most_16_nodes_and_the_rest_goes_in_the_next_batch() {
  Rig rig;
  for (uint32_t n = 1; n <= 20; n++) {
    rig.pairPlug(n);
    put(rig, kNow, n, powerOnly(0, 10 * n));
  }
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  TEST_ASSERT_EQUAL_UINT(16, rig.builder.nodeCount());
  TEST_ASSERT_EQUAL_UINT(16, rig.builder.sampleCount());
  TEST_ASSERT_EQUAL_UINT32(1, rig.builder.seq());
  rig.builder.markSent(kNow);
  rig.builder.onAck(1);
  TEST_ASSERT_EQUAL_UINT(4, rig.store.size());
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow + 6000, kEpoch + 6)));
  TEST_ASSERT_EQUAL_UINT(4, rig.builder.nodeCount());
  TEST_ASSERT_EQUAL_UINT32(2, rig.builder.seq());
  // The ids are in hex, so node 17 is PLUG-000011 and node 20 is PLUG-000014.
  TEST_ASSERT_TRUE(body(rig).find("PLUG-000011") != std::string::npos);
  TEST_ASSERT_TRUE(body(rig).find("PLUG-000014") != std::string::npos);
  TEST_ASSERT_TRUE(body(rig).find("PLUG-000010") == std::string::npos);
}

void test_a_node_with_many_samples_is_split_by_the_size_of_the_batch() {
  Rig rig;
  rig.pairPlug(1);
  for (uint32_t i = 0; i < 1100; i++) {
    put(rig, kNow, 1, fullSample(i));
  }
  size_t total = 0;
  int batches = 0;
  uint32_t now = kNow;
  while (prepare(rig, now, kEpoch + (now - kNow) / 1000) == BatchPrepare::Ready) {
    TEST_ASSERT_TRUE(rig.builder.bodySize() < kBatchBudgetBytes);
    TEST_ASSERT_TRUE(rig.builder.bodySize() < kMaxBodyBytes);
    TEST_ASSERT_EQUAL_UINT(rig.builder.bodySize(), body(rig).size());
    total += rig.builder.sampleCount();
    batches++;
    rig.builder.markSent(now);
    rig.builder.onAck(rig.builder.seq());
    now += kMinBatchGapMs;
    TEST_ASSERT_TRUE(batches < 10);
  }
  TEST_ASSERT_EQUAL_UINT(1100, total);
  TEST_ASSERT_EQUAL_INT(2, batches);
  TEST_ASSERT_EQUAL_UINT(0, rig.store.size());
}

void test_a_full_pool_of_the_largest_samples_stays_below_the_budget_in_every_batch() {
  Rig rig;
  rig.pairPlug(1);
  rig.pairPlug(2);
  for (uint32_t i = 0; i < kSamplePoolSize; i++) {
    Sample sample = fullSample(i);
    sample.ageMs = 0;
    sample.voltageDeciV = kMaxVoltageDeciV;
    sample.currentCentiA = kMaxCurrentCentiA;
    sample.powerDeciW = kMaxPowerDeciW;
    sample.energyWh = 4294967294u;
    sample.frequencyDeciHz = kMaxFrequencyDeciHz;
    sample.powerFactorCenti = kMaxPowerFactorCenti;
    sample.condition = 3;
    put(rig, kNow, 1 + i % 2, sample);
  }
  size_t total = 0;
  uint32_t now = kNow;
  for (int batch = 0; batch < 6 && prepare(rig, now, kEpoch + (now - kNow) / 1000) == BatchPrepare::Ready; batch++) {
    TEST_ASSERT_TRUE(rig.builder.bodySize() <= kBatchBudgetBytes);
    total += rig.builder.sampleCount();
    rig.builder.markSent(now);
    rig.builder.onAck(rig.builder.seq());
    now += kMinBatchGapMs;
  }
  TEST_ASSERT_EQUAL_UINT(kSamplePoolSize, total);
}

void test_the_acknowledgement_drops_the_samples_of_the_batch_and_leaves_the_later_ones() {
  Rig rig;
  rig.pairPlug(1);
  put(rig, kNow, 1, powerOnly(0, 10));
  put(rig, kNow, 1, powerOnly(1, 20));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  put(rig, kNow + 100, 1, powerOnly(2, 30));
  rig.builder.markSent(kNow);
  rig.builder.onAck(1);
  TEST_ASSERT_EQUAL_UINT(1, rig.store.size());
  TEST_ASSERT_EQUAL_UINT32(30, rig.store.at(0).powerDeciW);
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow + 6000, kEpoch + 6)));
  TEST_ASSERT_EQUAL_UINT32(2, rig.builder.seq());
  TEST_ASSERT_EQUAL_UINT(1, rig.builder.sampleCount());
}

void test_a_batch_that_was_sent_and_not_acknowledged_keeps_its_samples_and_its_number() {
  Rig rig;
  rig.pairPlug(1);
  for (uint32_t i = 0; i < 3; i++) {
    put(rig, kNow, 1, powerOnly(i, 10 + i));
  }
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  rig.builder.markSent(kNow);
  // A new sample comes in, and the answer never arrives.
  put(rig, kNow + 2000, 1, powerOnly(3, 99));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow + 6000, kEpoch + 6)));
  TEST_ASSERT_EQUAL_UINT32(1, rig.builder.seq());
  TEST_ASSERT_EQUAL_UINT(3, rig.builder.sampleCount());
  const std::string retry = body(rig);
  TEST_ASSERT_EQUAL_UINT(3, countOf(retry, "{\"t\":"));
  // The retry carries the clock of the retry, and the same samples a few seconds later.
  TEST_ASSERT_TRUE(retry.find("\"sentAt\":1793618406") != std::string::npos);
  TEST_ASSERT_TRUE(retry.find("\"p\":9.9") == std::string::npos);
  rig.builder.markSent(kNow + 6000);
  rig.builder.onAck(1);
  TEST_ASSERT_EQUAL_UINT(1, rig.store.size());
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow + 12000, kEpoch + 12)));
  TEST_ASSERT_EQUAL_UINT32(2, rig.builder.seq());
  TEST_ASSERT_TRUE(body(rig).find("\"p\":9.9") != std::string::npos);
}

void test_a_batch_that_lost_samples_while_it_waited_shrinks_and_one_that_lost_all_is_gone() {
  Rig rig;
  rig.pairPlug(1);
  for (uint32_t i = 0; i < 3; i++) {
    put(rig, kNow, 1, powerOnly(i, 10 + i));
  }
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  rig.builder.markSent(kNow);
  rig.store.discardOldest(2);
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow + 6000, kEpoch + 6)));
  TEST_ASSERT_EQUAL_UINT(1, rig.builder.sampleCount());
  TEST_ASSERT_EQUAL_UINT32(1, rig.builder.seq());
  rig.store.discardOldest(1);
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::NoSamples), result(prepare(rig, kNow + 12000, kEpoch + 12)));
  // The number was not used, so the next batch is number 1 again.
  put(rig, kNow + 13000, 1, powerOnly(5, 50));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow + 18000, kEpoch + 18)));
  TEST_ASSERT_EQUAL_UINT32(1, rig.builder.seq());
}

void test_the_answer_409_sets_the_number_of_the_batch_that_waits_and_of_the_next_one() {
  Rig rig;
  rig.pairPlug(1);
  put(rig, kNow, 1, powerOnly(0, 10));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  rig.builder.markSent(kNow);
  rig.builder.onExpectedSeq(41);
  TEST_ASSERT_EQUAL_UINT32(41, rig.builder.seq());
  // The caller makes the batch again, so the size of the number counts. The samples are the same.
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow + 6000, kEpoch + 6)));
  TEST_ASSERT_TRUE(body(rig).find("{\"seq\":41,") == 0);
  TEST_ASSERT_EQUAL_UINT(1, rig.builder.sampleCount());
  rig.builder.markSent(kNow + 6000);
  rig.builder.onAck(41);
  put(rig, kNow + 7000, 1, powerOnly(1, 20));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow + 12000, kEpoch + 12)));
  TEST_ASSERT_EQUAL_UINT32(42, rig.builder.seq());

  // A hub that has just started has no batch, and takes the number for the first one.
  Rig fresh;
  fresh.pairPlug(1);
  fresh.builder.onExpectedSeq(0);
  fresh.builder.onExpectedSeq(7);
  put(fresh, kNow, 1, powerOnly(0, 10));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(fresh)));
  TEST_ASSERT_EQUAL_UINT32(7, fresh.builder.seq());
}

void test_an_acknowledgement_below_the_number_of_the_batch_is_not_ours() {
  Rig rig;
  rig.pairPlug(1);
  put(rig, kNow, 1, powerOnly(0, 10));
  prepare(rig);
  rig.builder.onExpectedSeq(41);
  prepare(rig, kNow + 6000, kEpoch + 6);
  rig.builder.onAck(40);
  TEST_ASSERT_EQUAL_UINT(1, rig.store.size());
  // An acknowledgement with no batch in flight changes nothing either.
  Rig idle;
  idle.pairPlug(1);
  put(idle, kNow, 1, powerOnly(0, 10));
  idle.builder.onAck(5);
  TEST_ASSERT_EQUAL_UINT(1, idle.store.size());
  // One above the number is taken as stored.
  rig.builder.onAck(50);
  TEST_ASSERT_EQUAL_UINT(0, rig.store.size());
}

void test_the_results_of_relay_commands_are_written_after_the_nodes() {
  Rig rig;
  rig.pairPlug(1);
  put(rig, kNow, 1, powerOnly(0, 10));
  RelayResult results[4];
  memset(results, 0, sizeof(results));
  strcpy(results[0].id, "6f1c-AA01");
  results[0].ok = true;
  strcpy(results[1].id, "bad id");  // a space is not allowed
  results[1].ok = true;
  results[2].id[0] = '\0';  // an empty id
  strcpy(results[3].id, "x");
  results[3].ok = false;
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow, kEpoch, results, 4)));
  const std::string text = body(rig);
  TEST_ASSERT_TRUE(text.find("}]}],\"results\":[{\"id\":\"6f1c-AA01\",\"ok\":true},{\"id\":\"x\",\"ok\":false}]}") != std::string::npos);
  TEST_ASSERT_EQUAL_UINT(rig.builder.bodySize(), text.size());
}

void test_a_batch_has_at_most_64_results_and_none_means_no_key() {
  Rig rig;
  rig.pairPlug(1);
  put(rig, kNow, 1, powerOnly(0, 10));
  RelayResult results[70];
  memset(results, 0, sizeof(results));
  for (size_t i = 0; i < 70; i++) {
    strcpy(results[i].id, "abc");
    results[i].ok = true;
  }
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow, kEpoch, results, 70)));
  TEST_ASSERT_EQUAL_UINT(64, countOf(body(rig), "\"ok\":"));
  TEST_ASSERT_TRUE(rig.builder.bodySize() < kFixedJsonBytes);
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow, kEpoch, nullptr, 0)));
  TEST_ASSERT_TRUE(body(rig).find("results") == std::string::npos);
}

void test_a_result_id_of_37_characters_without_an_end_is_left_out() {
  Rig rig;
  rig.pairPlug(1);
  put(rig, kNow, 1, powerOnly(0, 10));
  RelayResult results[1];
  memset(results, 'a', sizeof(results));
  results[0].ok = true;
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow, kEpoch, results, 1)));
  TEST_ASSERT_TRUE(body(rig).find("results") == std::string::npos);
}

void test_samples_of_a_node_that_is_not_paired_are_left_out_and_go_with_the_batch() {
  Rig rig;
  rig.pairPlug(1);
  put(rig, kNow, 1, powerOnly(0, 10));
  put(rig, kNow, 5, powerOnly(0, 50));  // a node that is not paired
  put(rig, kNow, 1, powerOnly(1, 20));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  TEST_ASSERT_EQUAL_UINT(1, rig.builder.nodeCount());
  TEST_ASSERT_EQUAL_UINT(2, rig.builder.sampleCount());
  TEST_ASSERT_EQUAL_UINT32(1, rig.builder.skipped());
  rig.builder.markSent(kNow);
  rig.builder.onAck(1);
  TEST_ASSERT_EQUAL_UINT(0, rig.store.size());
}

void test_a_pool_with_only_samples_that_are_left_out_is_emptied() {
  Rig rig;
  put(rig, kNow, 5, powerOnly(0, 50));
  put(rig, kNow, 6, powerOnly(0, 60));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::NoSamples), result(prepare(rig)));
  TEST_ASSERT_EQUAL_UINT(0, rig.store.size());
  TEST_ASSERT_EQUAL_UINT32(2, rig.builder.skipped());
}

void test_a_sample_whose_time_the_service_would_refuse_is_left_out() {
  Rig rig;
  rig.pairPlug(1);
  // The clock was set a moment ago, and the sample is older than the time that the clock counts.
  put(rig, kNow, 1, powerOnly(0, 10, 20000));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::NoSamples), result(prepare(rig, kNow, kMinUnixSeconds + 10)));
  TEST_ASSERT_EQUAL_UINT32(1, rig.builder.skipped());
  TEST_ASSERT_EQUAL_UINT(0, rig.store.size());
  // The edge: a sample at exactly the minimum time is good.
  Rig edge;
  edge.pairPlug(1);
  put(edge, kNow, 1, powerOnly(0, 10, 10000));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(edge, kNow, kMinUnixSeconds + 10)));
  TEST_ASSERT_TRUE(body(edge).find("{\"t\":1700000000,") != std::string::npos);
}

void test_a_node_that_is_removed_while_its_batch_waits_is_left_out_of_the_retry() {
  Rig rig;
  rig.pairPlug(1);
  rig.pairPlug(2);
  put(rig, kNow, 1, powerOnly(0, 10));
  put(rig, kNow, 2, powerOnly(0, 20));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  TEST_ASSERT_EQUAL_UINT(2, rig.builder.nodeCount());
  rig.builder.markSent(kNow);
  uint8_t mac[kMacSize];
  makeMac(mac, 1);
  TEST_ASSERT_TRUE(rig.registry.unpair(mac));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig, kNow + 6000, kEpoch + 6)));
  TEST_ASSERT_EQUAL_UINT(1, rig.builder.nodeCount());
  TEST_ASSERT_TRUE(body(rig).find("PLUG-000001") == std::string::npos);
  TEST_ASSERT_EQUAL_UINT32(1, rig.builder.seq());
}

void test_an_output_that_refuses_a_write_stops_the_batch() {
  Rig rig;
  rig.pairPlug(1);
  put(rig, kNow, 1, fullSample(0));
  TEST_ASSERT_EQUAL_INT(result(BatchPrepare::Ready), result(prepare(rig)));
  const size_t size = rig.builder.bodySize();
  FailingOutput fails(size / 2);
  TEST_ASSERT_FALSE(rig.builder.render(fails));
  FailingOutput fits(size);
  TEST_ASSERT_TRUE(rig.builder.render(fits));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_limits_are_the_ones_of_the_contract);
  RUN_TEST(test_with_no_samples_there_is_no_batch);
  RUN_TEST(test_a_clock_that_is_not_set_holds_the_samples);
  RUN_TEST(test_a_batch_waits_six_seconds_after_the_last_send);
  RUN_TEST(test_a_sample_is_written_as_the_contract_says);
  RUN_TEST(test_the_size_of_the_body_is_the_size_that_is_written_and_the_body_is_the_same_every_time);
  RUN_TEST(test_a_value_that_is_left_out_is_not_written_and_a_clamp_has_no_relay);
  RUN_TEST(test_the_limits_of_each_value_are_written_in_full);
  RUN_TEST(test_the_time_of_a_sample_rounds_to_the_nearest_second_and_survives_the_wrap_of_the_clock);
  RUN_TEST(test_nodes_are_in_the_order_of_their_first_sample_and_samples_are_in_the_order_of_arrival);
  RUN_TEST(test_a_batch_has_at_most_16_nodes_and_the_rest_goes_in_the_next_batch);
  RUN_TEST(test_a_node_with_many_samples_is_split_by_the_size_of_the_batch);
  RUN_TEST(test_a_full_pool_of_the_largest_samples_stays_below_the_budget_in_every_batch);
  RUN_TEST(test_the_acknowledgement_drops_the_samples_of_the_batch_and_leaves_the_later_ones);
  RUN_TEST(test_a_batch_that_was_sent_and_not_acknowledged_keeps_its_samples_and_its_number);
  RUN_TEST(test_a_batch_that_lost_samples_while_it_waited_shrinks_and_one_that_lost_all_is_gone);
  RUN_TEST(test_the_answer_409_sets_the_number_of_the_batch_that_waits_and_of_the_next_one);
  RUN_TEST(test_an_acknowledgement_below_the_number_of_the_batch_is_not_ours);
  RUN_TEST(test_the_results_of_relay_commands_are_written_after_the_nodes);
  RUN_TEST(test_a_batch_has_at_most_64_results_and_none_means_no_key);
  RUN_TEST(test_a_result_id_of_37_characters_without_an_end_is_left_out);
  RUN_TEST(test_samples_of_a_node_that_is_not_paired_are_left_out_and_go_with_the_batch);
  RUN_TEST(test_a_pool_with_only_samples_that_are_left_out_is_emptied);
  RUN_TEST(test_a_sample_whose_time_the_service_would_refuse_is_left_out);
  RUN_TEST(test_a_node_that_is_removed_while_its_batch_waits_is_left_out_of_the_retry);
  RUN_TEST(test_an_output_that_refuses_a_write_stops_the_batch);
  return UNITY_END();
}
