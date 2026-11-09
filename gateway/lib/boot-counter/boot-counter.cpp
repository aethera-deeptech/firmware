#include "boot-counter.h"

namespace wattlink {

BootResult BootCounter::next(uint32_t *bootId) {
  *bootId = 0;
  uint32_t previous = 0;
  CounterRead found = store_.read(&previous);
  if (found == CounterRead::Failed) {
    // One more try, because a read can fail once.
    found = store_.read(&previous);
  }
  if (found == CounterRead::Failed) {
    return BootResult::Failed;
  }
  if (found == CounterRead::Missing) {
    previous = 0;
  }
  if (previous >= kMaxBootId) {
    return BootResult::Failed;
  }

  const uint32_t number = previous + 1;
  if (!store_.write(number)) {
    return BootResult::Failed;
  }
  // The number counts only when the flash gives it back.
  uint32_t stored = 0;
  if (store_.read(&stored) != CounterRead::Ok || stored != number) {
    return BootResult::Failed;
  }
  *bootId = number;
  return found == CounterRead::Missing ? BootResult::First : BootResult::Ok;
}

}  // namespace wattlink
