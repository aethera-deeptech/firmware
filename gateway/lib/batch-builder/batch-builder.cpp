#include "batch-builder.h"

#include <string.h>

namespace wattlink {

namespace {

// Writes the pieces of the JSON and remembers whether a write failed. The numbers are written from integers, so no float code is needed.
class Emitter {
 public:
  explicit Emitter(BatchOutput &out) : out_(out), ok_(true) {}

  void raw(const char *text, size_t length) {
    if (ok_ && length > 0 && !out_.write(text, length)) {
      ok_ = false;
    }
  }
  void text(const char *text) { raw(text, strlen(text)); }

  void number(uint32_t value) {
    char digits[10];
    size_t count = 0;
    do {
      digits[count++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value > 0);
    char out[10];
    for (size_t i = 0; i < count; i++) {
      out[i] = digits[count - 1 - i];
    }
    raw(out, count);
  }

  // The value divided by 10 to the power of decimals, with all the decimals, for example 2305 with 1 decimal is 230.5 and 15200 with 3 decimals is 15.200.
  void fixed(uint32_t value, unsigned decimals) {
    uint32_t divisor = 1;
    for (unsigned i = 0; i < decimals; i++) {
      divisor *= 10;
    }
    number(value / divisor);
    char fraction[4];
    uint32_t rest = value % divisor;
    for (unsigned i = decimals; i > 0; i--) {
      fraction[i - 1] = static_cast<char>('0' + rest % 10);
      rest /= 10;
    }
    raw(".", 1);
    raw(fraction, decimals);
  }

  bool ok() const { return ok_; }

 private:
  BatchOutput &out_;
  bool ok_;
};

class CountingOutput : public BatchOutput {
 public:
  bool write(const char *, size_t length) override {
    bytes += length;
    return true;
  }
  size_t bytes = 0;
};

bool idIsSafe(const char *id) {
  const size_t length = strnlen(id, kResultIdSize);
  if (length == 0 || length >= kResultIdSize) {
    return false;
  }
  for (size_t i = 0; i < length; i++) {
    const char c = id[i];
    const bool good = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-';
    if (!good) {
      return false;
    }
  }
  return true;
}

}  // namespace

BatchBuilder::BatchBuilder(SampleStore &store, const NodeRegistry &registry)
    : store_(store),
      registry_(registry),
      nextSeq_(kProbeSeq),
      seq_(kProbeSeq),
      inFlight_(false),
      lastSerial_(0),
      sentOnce_(false),
      lastSentMs_(0),
      skipped_(0),
      nowMs_(0),
      epoch_(0),
      results_(nullptr),
      resultCount_(0),
      nodeCount_(0),
      sampleCount_(0),
      bodyBytes_(0) {
  memset(nodeSlots_, 0, sizeof(nodeSlots_));
}

// The Unix time of a sample is the Unix time of the batch minus the age of the sample, rounded to a second. A sample that the service would refuse by its time is
// left out here. The service counts a day from its own clock, which is a little after sentAt, so the limit is the one of expire() and not a full day.
bool BatchBuilder::sampleTime(const StoredSample &sample, uint32_t *seconds) const {
  uint32_t ageMs = nowMs_ - sample.capturedMs;
  if (static_cast<int32_t>(ageMs) < 0) {
    // The clock value was read a moment before the sample came in.
    ageMs = 0;
  }
  if (ageMs > kKeepSampleMs) {
    return false;
  }
  const uint32_t ageSeconds = (ageMs + 500) / 1000;
  if (ageSeconds > epoch_ || epoch_ - ageSeconds < kMinUnixSeconds) {
    return false;
  }
  *seconds = epoch_ - ageSeconds;
  return true;
}

// A sample that cannot go in any batch, for good: its node is not paired, or the service would refuse its time. A later clock does not change that.
bool BatchBuilder::sendable(const StoredSample &sample) const {
  uint32_t seconds = 0;
  return registry_.kindOf(store_.macOf(sample)) != 0 && sampleTime(sample, &seconds);
}

int BatchBuilder::slotOf(uint8_t node) const {
  for (size_t n = 0; n < nodeCount_; n++) {
    if (nodeSlots_[n] == node) {
      return static_cast<int>(n);
    }
  }
  return -1;
}

// A sample is in the batch when its node is one of the nodes of the batch and it is not newer than the last sample that the batch took.
bool BatchBuilder::inBatch(const StoredSample &sample) const {
  return static_cast<int32_t>(sample.serial - lastSerial_) <= 0 && slotOf(sample.node) >= 0;
}

// A new plan drops the samples that cannot go in any batch, chooses up to 16 nodes from the oldest samples, and takes the samples of those nodes in the order of their
// arrival until a limit is reached. A frozen plan keeps the nodes and the last serial of the batch that was sent, and counts again what is still there, so a batch only
// shrinks. Returns false when the batch has no sample.
bool BatchBuilder::plan(bool frozen) {
  sampleCount_ = 0;
  size_t perNode[kMaxBatchNodes];
  memset(perNode, 0, sizeof(perNode));

  if (!frozen) {
    skipped_ += static_cast<uint32_t>(store_.removeWhere([this](const StoredSample &sample) { return !sendable(sample); }));
    nodeCount_ = 0;
    for (size_t i = 0; i < store_.size() && nodeCount_ < kMaxBatchNodes; i++) {
      const uint8_t node = store_.at(i).node;
      if (slotOf(node) < 0) {
        nodeSlots_[nodeCount_++] = node;
      }
    }
    size_t estimate = kFixedJsonBytes + nodeCount_ * kNodeJsonBytes;
    for (size_t i = 0; i < store_.size(); i++) {
      const StoredSample &sample = store_.at(i);
      const int slot = slotOf(sample.node);
      if (slot < 0) {
        continue;
      }
      if (perNode[slot] >= kMaxBatchSamplesPerNode || sampleCount_ >= kMaxBatchSamples || estimate + kMaxSampleJsonBytes > kBatchBudgetBytes) {
        break;
      }
      perNode[slot]++;
      sampleCount_++;
      estimate += kMaxSampleJsonBytes;
      lastSerial_ = sample.serial;
    }
    return sampleCount_ > 0;
  }

  for (size_t i = 0; i < store_.size(); i++) {
    const StoredSample &sample = store_.at(i);
    if (static_cast<int32_t>(sample.serial - lastSerial_) > 0) {
      break;
    }
    const int slot = slotOf(sample.node);
    if (slot >= 0 && sendable(sample)) {
      perNode[slot]++;
      sampleCount_++;
    }
  }
  if (sampleCount_ == 0) {
    // Nothing of the batch can be sent now, so what is left of it goes.
    store_.removeWhere([this](const StoredSample &sample) { return inBatch(sample); });
    return false;
  }
  // A node that has no sample left is not in the batch, and its place in the table of the store may belong to another node by now.
  size_t kept = 0;
  for (size_t n = 0; n < nodeCount_; n++) {
    if (perNode[n] > 0) {
      nodeSlots_[kept++] = nodeSlots_[n];
    }
  }
  nodeCount_ = kept;
  return true;
}

BatchPrepare BatchBuilder::prepare(uint32_t nowMs, uint32_t epochSeconds, const RelayResult *results, size_t resultCount) {
  if (epochSeconds < kMinUnixSeconds) {
    return BatchPrepare::ClockNotSet;
  }
  if (sentOnce_ && nowMs - lastSentMs_ < kMinBatchGapMs) {
    return BatchPrepare::TooSoon;
  }
  nowMs_ = nowMs;
  epoch_ = epochSeconds;
  results_ = results;
  resultCount_ = resultCount;

  if (!plan(inFlight_)) {
    if (inFlight_) {
      // The service may have stored the batch that is gone, and it answered too late for the hub. The next batch gets the next number, because the number of
      // the lost one would be taken as a repeat, and a real gap gives the answer 409 with the number to continue from.
      nextSeq_ = seq_ + 1;
      inFlight_ = false;
    }
    return BatchPrepare::NoSamples;
  }
  if (!inFlight_) {
    seq_ = nextSeq_;
    inFlight_ = true;
  }
  CountingOutput counter;
  render(counter);
  bodyBytes_ = counter.bytes;
  return BatchPrepare::Ready;
}

bool BatchBuilder::render(BatchOutput &out) const {
  Emitter emit(out);
  emit.text("{\"seq\":");
  emit.number(seq_);
  emit.text(",\"sentAt\":");
  emit.number(epoch_);
  emit.text(",\"nodes\":[");
  for (size_t n = 0; n < nodeCount_; n++) {
    const uint8_t slot = nodeSlots_[n];
    const uint8_t *mac = store_.macOfNode(slot);
    const uint8_t kind = registry_.kindOf(mac);
    char id[kNodeIdSize];
    makeNodeId(kind, mac, id);
    if (n > 0) {
      emit.text(",");
    }
    emit.text("{\"nodeId\":\"");
    emit.text(id);
    emit.text("\",\"kind\":\"");
    emit.text(kind == kKindPlug ? "plug" : "clamp");
    emit.text("\",\"samples\":[");
    bool first = true;
    for (size_t i = 0; i < store_.size(); i++) {
      const StoredSample &sample = store_.at(i);
      uint32_t seconds = 0;
      if (static_cast<int32_t>(sample.serial - lastSerial_) > 0) {
        break;
      }
      if (sample.node != slot || !sampleTime(sample, &seconds)) {
        continue;
      }
      if (!first) {
        emit.text(",");
      }
      first = false;
      emit.text("{\"t\":");
      emit.number(seconds);
      if ((sample.fields & kHasVoltage) != 0) {
        emit.text(",\"v\":");
        emit.fixed(sample.voltageDeciV, 1);
      }
      if ((sample.fields & kHasCurrent) != 0) {
        emit.text(",\"i\":");
        emit.fixed(sample.currentCentiA, 2);
      }
      emit.text(",\"p\":");
      emit.fixed(sample.powerDeciW, 1);
      if ((sample.fields & kHasEnergy) != 0) {
        // The frame has watt hours and the contract has kilowatt hours.
        emit.text(",\"e\":");
        emit.fixed(sample.energyWh, 3);
      }
      if ((sample.fields & kHasFrequency) != 0) {
        emit.text(",\"f\":");
        emit.fixed(sample.frequencyDeciHz, 1);
      }
      if ((sample.fields & kHasPowerFactor) != 0) {
        emit.text(",\"pf\":");
        emit.fixed(sample.powerFactorCenti, 2);
      }
      if ((sample.fields & kHasCondition) != 0) {
        emit.text(",\"a\":");
        emit.number(sample.condition);
      }
      if ((sample.fields & kHasRelay) != 0) {
        emit.text(",\"r\":");
        emit.number(sample.relay);
      }
      emit.text("}");
    }
    emit.text("]}");
  }
  emit.text("]");

  bool firstResult = true;
  size_t written = 0;
  for (size_t i = 0; i < resultCount_ && written < kMaxBatchResults; i++) {
    if (!idIsSafe(results_[i].id)) {
      continue;
    }
    emit.text(firstResult ? ",\"results\":[" : ",");
    firstResult = false;
    emit.text("{\"id\":\"");
    emit.text(results_[i].id);
    emit.text(results_[i].ok ? "\",\"ok\":true}" : "\",\"ok\":false}");
    written++;
  }
  if (!firstResult) {
    emit.text("]");
  }
  emit.text("}");
  return emit.ok();
}

void BatchBuilder::markSent(uint32_t nowMs) {
  sentOnce_ = true;
  lastSentMs_ = nowMs;
}

void BatchBuilder::onAck(uint32_t ackSeq) {
  if (!inFlight_ || static_cast<int32_t>(ackSeq - seq_) < 0) {
    return;
  }
  // The samples of the batch are the ones of its nodes up to its last serial. The samples of the other nodes stay for a later batch.
  store_.removeWhere([this](const StoredSample &sample) { return inBatch(sample); });
  inFlight_ = false;
  nextSeq_ = seq_ + 1;
}

void BatchBuilder::onExpectedSeq(uint32_t expectedSeq) {
  if (expectedSeq == 0) {
    return;
  }
  nextSeq_ = expectedSeq;
  if (inFlight_) {
    seq_ = expectedSeq;
  }
}

}  // namespace wattlink
