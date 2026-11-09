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
      nextSeq_(1),
      seq_(1),
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
// left out here.
bool BatchBuilder::sampleTime(const StoredSample &sample, uint32_t *seconds) const {
  const uint32_t ageMs = nowMs_ - sample.capturedMs;
  if (ageMs > kMaxSampleAgeMs) {
    return false;
  }
  const uint32_t ageSeconds = (ageMs + 500) / 1000;
  if (ageSeconds > epoch_ || epoch_ - ageSeconds < kMinUnixSeconds) {
    return false;
  }
  *seconds = epoch_ - ageSeconds;
  return true;
}

// Looks at the samples from the oldest. A frozen plan takes every sample up to the last serial of the batch that was sent. A new plan takes samples until a limit
// of the batch is reached. A sample whose node is not paired, or whose time the service would refuse, is passed over and goes with the batch that it sits in.
// Returns false when the batch has no sample.
bool BatchBuilder::plan(bool frozen) {
  nodeCount_ = 0;
  sampleCount_ = 0;
  size_t perNode[kMaxBatchNodes];
  memset(perNode, 0, sizeof(perNode));
  size_t estimate = kFixedJsonBytes;
  uint32_t last = 0;
  size_t looked = 0;
  for (; looked < store_.size(); looked++) {
    const StoredSample &sample = store_.at(looked);
    if (frozen && static_cast<int32_t>(sample.serial - lastSerial_) > 0) {
      break;
    }
    uint32_t seconds = 0;
    if (registry_.kindOf(store_.macOf(sample)) == 0 || !sampleTime(sample, &seconds)) {
      if (!frozen) {
        skipped_++;
      }
      last = sample.serial;
      continue;
    }
    int slot = -1;
    for (size_t n = 0; n < nodeCount_; n++) {
      if (nodeSlots_[n] == sample.node) {
        slot = static_cast<int>(n);
      }
    }
    const bool newNode = slot < 0;
    if (!frozen) {
      if ((newNode && nodeCount_ >= kMaxBatchNodes) || (!newNode && perNode[slot] >= kMaxBatchSamplesPerNode) || sampleCount_ >= kMaxBatchSamples ||
          estimate + kMaxSampleJsonBytes + (newNode ? kNodeJsonBytes : 0) > kBatchBudgetBytes) {
        break;
      }
    }
    if (newNode) {
      if (nodeCount_ >= kMaxBatchNodes) {
        break;
      }
      slot = static_cast<int>(nodeCount_);
      nodeSlots_[nodeCount_++] = sample.node;
      estimate += kNodeJsonBytes;
    }
    perNode[slot]++;
    sampleCount_++;
    estimate += kMaxSampleJsonBytes;
    last = sample.serial;
  }
  if (sampleCount_ == 0) {
    // Everything that was looked at is passed over for good, and a later clock would not change that, so it goes.
    store_.discardOldest(looked);
    return false;
  }
  if (!frozen) {
    lastSerial_ = last;
  }
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
    inFlight_ = false;
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
  // The samples of the batch are at the front of the pool, because the batch took them from the oldest on.
  size_t count = 0;
  while (count < store_.size() && static_cast<int32_t>(store_.at(count).serial - lastSerial_) <= 0) {
    count++;
  }
  store_.discardOldest(count);
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
