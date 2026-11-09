#pragma once

#include "flash-command-log.h"

namespace wattlink {

// The flash storage (NVS) of the chip for the log of the relay commands. One blob for each slot, in a namespace of its own, so the settings and the
// nodes never share a key. Each call opens the namespace, does one thing, and closes it. Nothing here prints a value.
// NVS writes the new data of a blob before it drops the old data, so a loss of power in the middle of a write is expected to leave the old blob or the new
// blob. This follows the design of NVS and was not checked in the source of the ESP-IDF version that the hub uses.
class NvsCommandBlobStore : public CommandBlobStore {
 public:
  ReadResult read(size_t slot, uint8_t out[kCommandBlobSize]) override;
  bool write(size_t slot, const uint8_t in[kCommandBlobSize]) override;
};

}  // namespace wattlink
