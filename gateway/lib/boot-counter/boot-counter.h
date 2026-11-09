#pragma once

#include <stdint.h>

namespace wattlink {

// The boot number of the hub, as docs/espnow-protocol.md describes it in the section "Frame layout". It starts at 1, grows by 1 at every start
// and is kept in flash. The hub writes the new value and waits for the write to be confirmed before it sends the first frame, so a pair of boot
// number and counter is never used twice with the same key.
// The highest number that the counter gives. The next start fails, and the hub stays mute, because a wrap would repeat numbers.
constexpr uint32_t kMaxBootId = 0xFFFFFFFEu;

enum class CounterRead : uint8_t {
  Ok,
  Missing,  // nothing was ever written
  Failed,   // the flash could not be read, so a number may be stored
};

// The storage of the counter. On the chip it is the flash (NVS), and in the tests it is a fake in memory. A read that fails must not be
// reported as Missing, because the counter would then start again at 1 and repeat numbers.
class CounterStore {
 public:
  virtual ~CounterStore() = default;
  virtual CounterRead read(uint32_t *value) = 0;
  virtual bool write(uint32_t value) = 0;
};

enum class BootResult : uint8_t {
  Ok,      // the number is the stored one plus 1
  First,   // no number was stored, so this is the first start of the hub, or the flash was erased. The number is 1.
  Failed,  // the number could not be read, written or confirmed, and the number is 0
};

// The class does the logic only, so the same code runs on the host in the tests and on the chip. Call it once for each start.
class BootCounter {
 public:
  explicit BootCounter(CounterStore &store) : store_(store) {}

  // Reads the stored number (one more try after a failed read), writes the next one, and reads it back. Sets *bootId to the new number, or to 0 when
  // the result is Failed. A caller must not send a frame with a boot number of 0. A caller that gets First while nodes are paired must treat it
  // as Failed, because the keys of the nodes outlived the counter.
  BootResult next(uint32_t *bootId);

 private:
  CounterStore &store_;
};

}  // namespace wattlink
