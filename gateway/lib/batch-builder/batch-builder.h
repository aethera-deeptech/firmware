#pragma once

#include <stddef.h>
#include <stdint.h>

#include "node-registry.h"
#include "sample-store.h"

namespace wattlink {

// The batch of readings that the hub sends to the backend (POST /device/v1/readings). The rules are in the section "What the hub does with the frames" of
// docs/espnow-protocol.md and in the contract of the backend (openapi.yaml, schemas ReadingBatch and Sample, and docs/firmware-guide.md of the backend).
// The class chooses the samples, numbers the batch, keeps the batch the same until the backend acknowledges it, and writes it as JSON. It opens no connection,
// has no clock of its own and keeps nothing in flash. One task must call all of it.
//
// A batch takes up to 16 nodes, the ones with the oldest samples, and then all the samples of those nodes up to the limits. So the samples of a node go together,
// which the backend needs to judge a motor (3 samples of a node in one batch), and a hub with more than 16 nodes still sends every sample.

// The limits of the contract. A batch has 1 to 16 nodes, at most 1000 samples for a node, 5000 in all, 64 results and a body of 256 KiB at most.
constexpr size_t kMaxBatchNodes = 16;
constexpr size_t kMaxBatchSamplesPerNode = 1000;
constexpr size_t kMaxBatchSamples = 5000;
constexpr size_t kMaxBatchResults = 64;
constexpr size_t kMaxBodyBytes = 256 * 1024;

// The hub keeps a batch far below the limit of the contract, so that one send does not hold the connection for long. The choice of samples uses an upper
// bound for the size of a sample (the worst case is 98 characters with its comma), and the exact size is counted when the batch is written. The limit of
// 1000 samples for a node and of 5000 samples are in the contract, and the budget is reached first, at about 840 samples.
constexpr size_t kMaxSampleJsonBytes = 100;
constexpr size_t kNodeJsonBytes = 80;
constexpr size_t kFixedJsonBytes = 6000;  // the frame of the batch and 64 results
constexpr size_t kBatchBudgetBytes = 90000;
static_assert(kBatchBudgetBytes < kMaxBodyBytes, "a batch must stay below the limit of the contract");

// The service counts every batch, also a retry, and allows 600 in an hour, which is one every 6 seconds.
constexpr uint32_t kMinBatchGapMs = 6000;

// The first batch after a start has a number that the service cannot expect, so it answers 409 and says which number to continue from. The hub keeps no number in
// flash. A start with the number 1 could meet a service whose last number is 1, which it takes as a repeat of the last batch and stores nothing, and then the hub
// would drop the samples as stored.
constexpr uint32_t kProbeSeq = 0x7FFFFFFFu;

// The service refuses a Unix time before this, in a sample or in sentAt.
constexpr uint32_t kMinUnixSeconds = 1700000000u;

// The id of a relay command is a UUID of 36 characters, and a terminating zero.
constexpr size_t kResultIdSize = 37;

// The answer of the hub to a relay command. The commands are not part of this class, and the module that has them passes the results for the next batch.
struct RelayResult {
  char id[kResultIdSize];
  bool ok;
};

// True for an id that is safe to write into the JSON without escaping, which is 1 to 36 letters, digits and hyphens. Other ids are never written.
bool isSafeResultId(const char *id);

// Where the JSON goes. The send code gives one that writes to the connection, and the tests give one that writes to a buffer. A write that returns false stops the batch.
// The batch is written in many small pieces (about 15 for each sample), so an output that sends to a TLS connection must collect them in a buffer first.
class BatchOutput {
 public:
  virtual ~BatchOutput() = default;
  virtual bool write(const char *text, size_t length) = 0;
};

enum class BatchPrepare : uint8_t {
  Ready,        // a batch is ready, so ask bodySize() and call render()
  NoSamples,    // nothing to send
  ClockNotSet,  // the Unix time is before kMinUnixSeconds, so the hub holds the samples until the clock is set
  TooSoon,      // less than kMinBatchGapMs since the last send
};

class BatchBuilder {
 public:
  BatchBuilder(SampleStore &store, const NodeRegistry &registry);

  BatchBuilder(const BatchBuilder &) = delete;
  BatchBuilder &operator=(const BatchBuilder &) = delete;

  // Chooses the batch, or takes the batch that was sent and not acknowledged, and fixes its numbers. nowMs is the millis counter and epochSeconds is the Unix time
  // at the same moment, and both are used for every byte of this batch, so the body is the same each time that render() runs. A batch that was sent and not
  // acknowledged keeps its samples and its number, also when new samples came in. Its times are made again, so a retry carries the clock of the retry.
  // nowMs must be read after the last sample was given to the store. A value from before the arrival of a sample counts as no age for it.
  // The results are the answers to relay commands, at most kMaxBatchResults, and a result whose id has a character other than a letter, a digit or a hyphen is left out.
  // The results are not part of the batch that is kept, so the caller passes the same results for every retry until the acknowledgement. The array must stay valid
  // until the send ends.
  BatchPrepare prepare(uint32_t nowMs, uint32_t epochSeconds, const RelayResult *results, size_t resultCount);

  // The number of the batch, and the size and the number of samples of the batch that prepare() made.
  uint32_t seq() const { return seq_; }
  size_t bodySize() const { return bodyBytes_; }
  size_t sampleCount() const { return sampleCount_; }
  size_t nodeCount() const { return nodeCount_; }

  // Writes the body of the batch that prepare() made, and returns false when the output refused a write.
  bool render(BatchOutput &out) const;

  // Call it when a send starts, so the next batch waits for kMinBatchGapMs.
  void markSent(uint32_t nowMs);

  // The answer 200. A batch whose number is not above ackSeq is stored, so its samples are dropped from the store and the next batch gets the next number.
  void onAck(uint32_t ackSeq);

  // The answer 409. The service says which number it waits for, so the next batch, and the one that waits, get that number.
  void onExpectedSeq(uint32_t expectedSeq);

  // Samples that were left out and dropped because their node is not paired any more, or because their time is before kMinUnixSeconds or too old for the service.
  uint32_t skipped() const { return skipped_; }

 private:
  bool sampleTime(const StoredSample &sample, uint32_t *seconds) const;
  bool sendable(const StoredSample &sample) const;
  int slotOf(uint8_t node) const;
  bool inBatch(const StoredSample &sample) const;
  bool plan(bool frozen);

  SampleStore &store_;
  const NodeRegistry &registry_;
  uint32_t nextSeq_;
  uint32_t seq_;
  bool inFlight_;
  uint32_t lastSerial_;
  bool sentOnce_;
  uint32_t lastSentMs_;
  uint32_t skipped_;

  uint32_t nowMs_;
  uint32_t epoch_;
  const RelayResult *results_;
  size_t resultCount_;
  uint8_t nodeSlots_[kMaxBatchNodes];
  size_t nodeCount_;
  size_t sampleCount_;
  size_t bodyBytes_;
};

}  // namespace wattlink
