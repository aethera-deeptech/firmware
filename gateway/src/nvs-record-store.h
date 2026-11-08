#pragma once

#include "record-store.h"

namespace wattlink {

// The flash storage (NVS) of the chip for the records of the node registry. One blob for each slot, in a namespace of its own, so the
// settings and the nodes never share a key. Each call opens the namespace, does one thing, and closes it. Nothing here prints a value.
class NvsRecordStore : public RecordStore {
 public:
  ReadResult read(size_t slot, uint8_t out[kRecordSize]) override;
  bool write(size_t slot, const uint8_t in[kRecordSize]) override;
};

}  // namespace wattlink
