#include "sample-store.h"

#include <string.h>

namespace wattlink {

SampleStore::SampleStore() : head_(0), size_(0), nextSerial_(1) {
  memset(nodes_, 0, sizeof(nodes_));
  memset(pool_, 0, sizeof(pool_));
  memset(&counts_, 0, sizeof(counts_));
}

int SampleStore::findNode(const uint8_t mac[kMacSize]) const {
  for (size_t i = 0; i < kMaxNodes; i++) {
    if (nodes_[i].inUse && memcmp(nodes_[i].mac, mac, kMacSize) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void SampleStore::dropOldestOf(uint8_t node) {
  bool done = false;
  removeWhere([&done, node](const StoredSample &sample) -> bool {
    if (!done && sample.node == node) {
      done = true;
      return true;
    }
    return false;
  });
}

void SampleStore::push(const StoredSample &sample) {
  // A node that holds its share already loses its own oldest sample first, so it cannot push out the samples of the other nodes.
  if (nodes_[sample.node].held >= kMaxSamplesPerNode) {
    dropOldestOf(sample.node);
    counts_.capped++;
  }
  if (size_ == kSamplePoolSize) {
    // The oldest sample goes, because the newest are the ones that the owner looks at.
    if (nodes_[pool_[head_].node].held > 0) {
      nodes_[pool_[head_].node].held--;
    }
    head_ = (head_ + 1) % kSamplePoolSize;
    size_--;
    counts_.overflowed++;
  }
  StoredSample taken = sample;
  taken.serial = nextSerial_++;
  pool_[(head_ + size_) % kSamplePoolSize] = taken;
  size_++;
  nodes_[sample.node].held++;
}

// Returns false when the sample is dropped. A value other than the power is left out when it is out of range.
bool SampleStore::store(uint32_t nowMs, uint8_t node, const Sample &sample) {
  if (sample.powerDeciW > kMaxPowerDeciW || sample.ageMs > kMaxSampleAgeMs) {
    // A power of all bits is above the limit too, so a missing power is dropped here.
    return false;
  }
  StoredSample stored;
  memset(&stored, 0, sizeof(stored));
  stored.capturedMs = nowMs - sample.ageMs;
  stored.powerDeciW = sample.powerDeciW;
  stored.node = node;
  if (sample.voltageDeciV <= kMaxVoltageDeciV) {
    stored.voltageDeciV = sample.voltageDeciV;
    stored.fields |= kHasVoltage;
  }
  if (sample.currentCentiA <= kMaxCurrentCentiA) {
    stored.currentCentiA = sample.currentCentiA;
    stored.fields |= kHasCurrent;
  }
  if (sample.energyWh != kNoU32) {
    stored.energyWh = sample.energyWh;
    stored.fields |= kHasEnergy;
  }
  if (sample.frequencyDeciHz <= kMaxFrequencyDeciHz) {
    stored.frequencyDeciHz = sample.frequencyDeciHz;
    stored.fields |= kHasFrequency;
  }
  if (sample.powerFactorCenti <= kMaxPowerFactorCenti) {
    stored.powerFactorCenti = sample.powerFactorCenti;
    stored.fields |= kHasPowerFactor;
  }
  if (sample.condition <= kMaxCondition) {
    stored.condition = sample.condition;
    stored.fields |= kHasCondition;
  }
  if (sample.relay <= 1) {
    stored.relay = sample.relay;
    stored.fields |= kHasRelay;
  }
  push(stored);
  counts_.stored++;
  return true;
}

uint32_t SampleStore::onSamples(uint32_t nowMs, const uint8_t mac[kMacSize], uint32_t nodeBootId, const Sample *samples, size_t count) {
  int index = findNode(mac);
  if (index < 0) {
    for (size_t i = 0; i < kMaxNodes; i++) {
      if (!nodes_[i].inUse) {
        index = static_cast<int>(i);
        memset(&nodes_[i], 0, sizeof(nodes_[i]));
        nodes_[i].inUse = true;
        memcpy(nodes_[i].mac, mac, kMacSize);
        break;
      }
    }
  }
  if (index < 0) {
    counts_.refused++;
    return kNoSampleSeq;
  }

  NodeState &node = nodes_[index];
  if (node.hasSeq && node.bootId != nodeBootId) {
    // A new boot of the node counts its samples from 0 again.
    node.hasSeq = false;
  }
  node.bootId = nodeBootId;

  for (size_t i = 0; i < count; i++) {
    const Sample &sample = samples[i];
    if (sample.sampleSeq == kNoSampleSeq) {
      // The value is the "none" of an ACK, so a node could never be told that this sample arrived.
      counts_.rejected++;
      continue;
    }
    if (sample.sampleSeq > kMaxSampleSeq || (node.hasSeq && sample.sampleSeq > node.highestSeq && sample.sampleSeq - node.highestSeq > kMaxSampleSeqStep)) {
      // A number that no node reaches, or a step that no node makes. The sample is dropped and the count of the node stays, so the next samples of
      // the node still count as new.
      counts_.rejected++;
      continue;
    }
    if (node.hasSeq && sample.sampleSeq <= node.highestSeq) {
      counts_.repeats++;
      continue;
    }
    // The sample counts as received, also when it is dropped below, so a bad sample never stops the ACK.
    node.highestSeq = sample.sampleSeq;
    node.hasSeq = true;
    if (!store(nowMs, static_cast<uint8_t>(index), sample)) {
      counts_.rejected++;
    }
  }
  return node.hasSeq ? node.highestSeq : kNoSampleSeq;
}

void SampleStore::discardOldest(size_t count) {
  if (count >= size_) {
    count = size_;
  }
  for (size_t i = 0; i < count; i++) {
    if (nodes_[at(i).node].held > 0) {
      nodes_[at(i).node].held--;
    }
  }
  head_ = (head_ + count) % kSamplePoolSize;
  size_ -= count;
}

void SampleStore::forget(const uint8_t mac[kMacSize]) {
  const int index = findNode(mac);
  if (index < 0) {
    return;
  }
  removeWhere([index](const StoredSample &sample) { return sample.node == index; });
  memset(&nodes_[index], 0, sizeof(nodes_[index]));
}

size_t SampleStore::expire(uint32_t nowMs) {
  // A sample cannot be older than its arrival, so a difference with the top bit set means that nowMs was read before the sample came in, and it is no age.
  const size_t removed = removeWhere([nowMs](const StoredSample &sample) {
    const uint32_t held = nowMs - sample.capturedMs;
    return static_cast<int32_t>(held) > 0 && held > kKeepSampleMs;
  });
  counts_.expired += static_cast<uint32_t>(removed);
  return removed;
}

}  // namespace wattlink
