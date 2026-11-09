#pragma once

#include <stddef.h>
#include <stdint.h>

#include "node-registry.h"
#include "sample-sink.h"

namespace wattlink {

// The samples that nodes sent, in RAM until a batch for the backend takes them. The rules are in the section "Sample" of docs/espnow-protocol.md, and the
// ranges are the ones of the schema Sample in the contract of the backend (openapi.yaml). The class does the logic only, so the same code runs on the host in the
// tests and on the chip. One task must call all of it. It builds no batch and has no clock of the world. The time of a sample is kept as the millis counter of the hub
// (capturedMs), and the batch code turns it into Unix seconds when the clock of the hub is set.

// The pool holds this many samples for all nodes, oldest first. When it is full, the oldest sample is dropped. It is 28 bytes for each sample, so about 56 KB of RAM,
// and the whole class is about 58 KB. It must be a static object, because it does not fit on the stack of a task, and a block of that size may not be free later.
// Three nodes that send a sample every 10 seconds fill it in about 2 hours, and 64 nodes fill it in about 5 minutes.
constexpr size_t kSamplePoolSize = 2048;

// One node may hold this many samples at the same time, which is an eighth of the pool. A node that sends faster than the hub takes the samples away, for
// example a faulty node that floods the radio, then pushes out its own oldest samples and not those of the other nodes. The most that it can push out of
// the others is this many samples, and only while the pool is full. The cost is that one node in a long outage of the backend keeps the last 256 samples,
// which is about 43 minutes at one sample every 10 seconds, although the pool could hold more of a single node.
constexpr size_t kMaxSamplesPerNode = kSamplePoolSize / 8;

// A sampleSeq counts from 0 at every boot of a node and grows by 1 for each sample. A node that samples every 10 seconds needs about 680 years to pass the
// first limit, so a larger value is a fault and is not stored. The second limit is the largest step that the hub accepts within one boot of a node, which is
// about 5.3 years of samples every 10 seconds, so a node that was out of reach for months still counts as new. A larger step is a fault too, and it is not
// stored and not taken as the new count, so one wrong number cannot make every later sample of the node look like a repeat.
constexpr uint32_t kMaxSampleSeq = 0x7FFFFFFFu;
constexpr uint32_t kMaxSampleSeqStep = 1UL << 24;

// The backend drops a sample that is older than a day. The hub refuses a sample that arrives with an age above a day, and expire() drops a held sample before it
// is a day old, with a margin for the time that a batch takes.
constexpr uint32_t kMaxSampleAgeMs = 24UL * 60UL * 60UL * 1000UL;
constexpr uint32_t kExpiryMarginMs = 10UL * 60UL * 1000UL;
constexpr uint32_t kKeepSampleMs = kMaxSampleAgeMs - kExpiryMarginMs;

// The limits of the schema Sample, in the units of the frame. Power is required, and a sample without a power in range is dropped. The other values are left out
// when they are out of range, so the backend does not refuse the sample.
constexpr uint32_t kMaxPowerDeciW = 600000;     // 60000 W
constexpr uint16_t kMaxVoltageDeciV = 5000;     // 500 V
constexpr uint16_t kMaxCurrentCentiA = 20000;   // 200 A
constexpr uint16_t kMaxFrequencyDeciHz = 1000;  // 100 Hz
constexpr uint8_t kMaxPowerFactorCenti = 100;   // 1
constexpr uint8_t kMaxCondition = 3;

// Which values of a stored sample are present.
constexpr uint8_t kHasVoltage = 1 << 0;
constexpr uint8_t kHasCurrent = 1 << 1;
constexpr uint8_t kHasEnergy = 1 << 2;
constexpr uint8_t kHasFrequency = 1 << 3;
constexpr uint8_t kHasPowerFactor = 1 << 4;
constexpr uint8_t kHasRelay = 1 << 5;
constexpr uint8_t kHasCondition = 1 << 6;

// A sample in the units of the frame. The batch code converts them (energy goes from Wh to kWh) when it writes the JSON.
struct StoredSample {
  uint32_t serial;      // grows by 1 for each sample that the pool takes, so a batch can name the samples that it holds, and it only wraps after 4 billion samples
  uint32_t capturedMs;  // the millis counter of the hub when the node took the sample, which is the arrival time minus the age
  uint32_t powerDeciW;
  uint32_t energyWh;
  uint16_t voltageDeciV;
  uint16_t currentCentiA;
  uint16_t frequencyDeciHz;
  uint8_t powerFactorCenti;
  uint8_t condition;
  uint8_t relay;   // 1 on, 0 off
  uint8_t fields;  // the kHas bits
  uint8_t node;    // the slot of the node in the table of this store
};

// What the store counted. They only grow, and they help to see a node or a flash that misbehaves.
struct SampleCounts {
  uint32_t stored;
  uint32_t repeats;     // samples whose sampleSeq was not new
  uint32_t rejected;    // samples that were dropped for no power, a power out of range, an age above a day, or a sampleSeq of all bits, above kMaxSampleSeq or a step above kMaxSampleSeqStep
  uint32_t overflowed;  // samples that were dropped because the pool was full
  uint32_t expired;     // samples that were held too long, see expire()
  uint32_t refused;     // frames from a node that the table had no room for
  uint32_t capped;      // samples that were pushed out because their node held kMaxSamplesPerNode already
};

class SampleStore : public SampleSink {
 public:
  SampleStore();

  SampleStore(const SampleStore &) = delete;
  SampleStore &operator=(const SampleStore &) = delete;

  // As SampleSink says. A sample is new when its sampleSeq is higher than the last one that the store took for the same boot of the node, and the first sample
  // after a start of the hub, or of a new boot of the node, is new. A sample that is a repeat, or that is dropped, still counts as received, so a gap or a bad
  // sample never stops the ACK. The exception is a sampleSeq above kMaxSampleSeq or above the step kMaxSampleSeqStep, which is dropped and not counted as
  // received, so the count of the node stays as it was. Returns the highest sampleSeq that the store holds for the node, or kNoSampleSeq when the table has no room for the node, or
  // when every sample of the frame has the sampleSeq of all bits.
  uint32_t onSamples(uint32_t nowMs, const uint8_t mac[kMacSize], uint32_t nodeBootId, const Sample *samples, size_t count) override;

  // Forgets a node that the owner removed. Its samples are dropped, the others keep their order, and the node starts again if it is paired again.
  void forget(const uint8_t mac[kMacSize]);

  // The samples in the pool, the oldest first.
  size_t size() const { return size_; }
  const StoredSample &at(size_t index) const { return pool_[(head_ + index) % kSamplePoolSize]; }

  // The radio address of the node of a stored sample.
  const uint8_t *macOf(const StoredSample &sample) const { return nodes_[sample.node].mac; }
  const uint8_t *macOfNode(uint8_t node) const { return nodes_[node].mac; }

  // Drops the oldest samples, for example after the backend took them.
  void discardOldest(size_t count);

  // Drops every sample that has been held for more than kKeepSampleMs, so the batch code never sends one that the backend would refuse as older than a day.
  // The pool is in the order of arrival and the ages are not, because a node sets its own age, so this looks at every sample. Call it before each batch and so
  // often that no sample reaches an age of 2^31 ms (about 24.8 days), because a larger difference reads as a negative age and counts as no age.
  // A clock value from before the arrival of a sample counts as no age, so a value that was read a moment too early drops nothing.
  // Returns the number of samples that it dropped.
  size_t expire(uint32_t nowMs);

  // Drops every sample for which drop() is true. The kept samples move towards the head in the same order. A write goes to the place of the kept count and a
  // read comes from the place of the index, and the kept count is never above the index, so a sample is read before its place is written, also when the pool wraps.
  // Returns the number of samples that it dropped.
  template <typename Drop>
  size_t removeWhere(Drop drop) {
    size_t kept = 0;
    for (size_t i = 0; i < size_; i++) {
      const StoredSample sample = at(i);
      if (!drop(sample)) {
        pool_[(head_ + kept) % kSamplePoolSize] = sample;
        kept++;
      } else if (nodes_[sample.node].held > 0) {
        nodes_[sample.node].held--;
      }
    }
    const size_t removed = size_ - kept;
    size_ = kept;
    return removed;
  }

  const SampleCounts &counts() const { return counts_; }

 private:
  struct NodeState {
    bool inUse;
    uint8_t mac[kMacSize];
    bool hasSeq;
    uint32_t bootId;
    uint32_t highestSeq;
    uint16_t held;  // how many samples of the node are in the pool, which is never more than kMaxSamplesPerNode
  };

  int findNode(const uint8_t mac[kMacSize]) const;
  bool store(uint32_t nowMs, uint8_t node, const Sample &sample);
  void push(const StoredSample &sample);
  void dropOldestOf(uint8_t node);

  NodeState nodes_[kMaxNodes];
  StoredSample pool_[kSamplePoolSize];
  size_t head_;
  size_t size_;
  uint32_t nextSerial_;
  SampleCounts counts_;
};

}  // namespace wattlink
