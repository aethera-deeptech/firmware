#pragma once

#include "flash-command-log.h"

namespace wattlink {

// The flash storage (NVS) of the chip for the log of the relay commands. One blob for each slot, in a namespace of its own, so the settings and the
// nodes never share a key. Each call opens the namespace, does one thing, and closes it. Nothing here prints a value.
// NVS writes a new blob before it drops the old one, and the commit makes the change visible in one step, so a loss of power leaves one of the two.
class NvsCommandBlobStore : public CommandBlobStore {
 public:
  ReadResult read(size_t slot, uint8_t out[kCommandBlobSize]) override;
  bool write(size_t slot, const uint8_t in[kCommandBlobSize]) override;
};

}  // namespace wattlink
