#pragma once

#include <stdint.h>

namespace wattlink {

// The rules are in the sections "Pairing and keys" and "The button of the hub" of docs/espnow-protocol.md.
constexpr uint32_t kPairingWindowMs = 60000;

// After a PAIR_REQUEST, the hub waits this long for the first TELEMETRY frame that proves the new key, or until the window ends if that
// comes first. When the frame does not arrive, nothing is committed and the window is closed, so the owner pairs again.
constexpr uint32_t kPairingPendingMs = 15000;

enum class WindowState : uint8_t {
  Closed,
  Open,     // waits for a PAIR_REQUEST
  Pairing,  // one node asked, and the hub waits for its first TELEMETRY frame
};

enum class PressResult : uint8_t {
  Opened,     // a new window was opened
  Restarted,  // the window was open and has 60 seconds again
  Ignored,    // a pairing is in progress and is not disturbed
};

// The state of the pairing window. It only does the arithmetic, so the same code runs on the host in the tests and on the chip.
// The time is the millis() counter, which wraps after about 49 days, and the subtractions are safe across the wrap.
// The class cannot stop the radio. The caller asks isOpen() or isPairing() for every frame, and does not rely on a state that it read
// earlier. One task must call all of it.
class PairingWindow {
 public:
  // A short press of the button of the hub. Opens the window, or starts the 60 seconds again when it is open. It does not close the
  // window, it does not disturb a pairing in progress, and it never opens a second window.
  PressResult press(uint32_t nowMs);

  // A PAIR_REQUEST from a node arrived. Only an open window takes it, and only one node at a time. Returns true when the pairing starts.
  bool beginPairing(uint32_t nowMs);

  // The first TELEMETRY frame verified with the new key. The window closes because one pairing is all it allows.
  // Returns true only for a pairing that was in progress and not out of time.
  bool commit(uint32_t nowMs);

  // Closes at once, for example when the portal starts.
  void close();

  // Moves a window whose time is over to Closed. Returns the state.
  WindowState update(uint32_t nowMs);

  WindowState state() const { return state_; }

  // True while the window waits for a PAIR_REQUEST and its time is not over. Does not depend on update() being called.
  bool isOpen(uint32_t nowMs) const;

  // True while a pairing is in progress and its time is not over.
  bool isPairing(uint32_t nowMs) const;

  // Whole seconds left, rounded up, so the display shows 1 for the last second. Zero when the window is not open.
  uint32_t secondsLeft(uint32_t nowMs) const;

 private:
  bool expired(uint32_t nowMs) const;

  WindowState state_ = WindowState::Closed;
  uint32_t endMs_ = 0;
};

}  // namespace wattlink
