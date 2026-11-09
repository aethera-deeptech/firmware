#pragma once

#include <stddef.h>
#include <stdint.h>

#include "frame-codec.h"
#include "node-registry.h"

namespace wattlink {

// The highest sampleSeq in an ACK that tells the node that the hub holds no sample of it. It is above any sample, so a node takes it as no progress.
constexpr uint32_t kNoSampleSeq = kNoU32;

// The place that takes the samples. The batches for the backend are built from what it keeps.
class SampleSink {
 public:
  virtual ~SampleSink() = default;
  // Called for a TELEMETRY frame that is fresh and that verified. nowMs is the clock of the hub when the frame arrived. The samples are valid for the
  // call only. Returns the highest sampleSeq that the hub now holds for this node and this boot of the node, and the hub puts it in the ACK. It must be
  // at least the highest sampleSeq of the frame, also when the frame holds only repeats, because a node commits a new key only on an ACK that it can
  // use. It returns kNoSampleSeq only when it can take nothing from this node.
  virtual uint32_t onSamples(uint32_t nowMs, const uint8_t mac[kMacSize], uint32_t nodeBootId, const Sample *samples, size_t count) = 0;
};

}  // namespace wattlink
