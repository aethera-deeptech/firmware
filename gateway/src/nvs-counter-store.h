#pragma once

#include "boot-counter.h"

namespace wattlink {

// The flash storage (NVS) of the chip for the boot number of the hub. One 32 bit value in a namespace of its own, so the settings, the nodes and the
// counter never share a key. Each call opens the namespace, does one thing, and closes it.
class NvsCounterStore : public CounterStore {
 public:
  CounterRead read(uint32_t *value) override;
  bool write(uint32_t value) override;
};

}  // namespace wattlink
