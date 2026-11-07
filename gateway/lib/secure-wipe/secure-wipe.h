#pragma once

#include <stddef.h>
#include <stdint.h>

namespace wattlink {

// A plain memset on a buffer that is not read again can be removed by the optimizer, a write through a volatile pointer cannot.
// The shared code in common/ has its own copy, because common/ does not depend on the folders of the hub.
inline void wipe(void *data, size_t length) {
  volatile uint8_t *bytes = static_cast<volatile uint8_t *>(data);
  while (length-- > 0) {
    *bytes++ = 0;
  }
}

}  // namespace wattlink
