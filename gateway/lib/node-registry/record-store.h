#pragma once

#include <stddef.h>
#include <stdint.h>

namespace wattlink {

constexpr size_t kRecordSize = 29;

enum class ReadResult : uint8_t {
  Ok,
  Missing,  // nothing was ever written to this slot
  Failed,   // the flash could not be read, so the slot may hold a record
};

// The storage of the registry, one fixed size record for each slot. On the chip it is the flash (NVS), and in the tests it is a fake in memory.
class RecordStore {
 public:
  virtual ~RecordStore() = default;
  virtual ReadResult read(size_t slot, uint8_t out[kRecordSize]) = 0;
  virtual bool write(size_t slot, const uint8_t in[kRecordSize]) = 0;
};

}  // namespace wattlink
