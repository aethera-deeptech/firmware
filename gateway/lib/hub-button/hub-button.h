#pragma once

#include <stdint.h>

namespace wattlink {

// The rules are in the section "The button of the hub" of docs/espnow-protocol.md.
constexpr uint32_t kBounceMs = 50;
constexpr uint32_t kPortalHoldMs = 3000;
// A hold of this length asks for a new claim code, and it is reported while the button is still held.
constexpr uint32_t kNewCodeHoldMs = 10000;

enum class ButtonEvent : uint8_t {
  None,
  Pairing,
  Portal,
  NewCode,
};

// Feed it the level of the button and the time of every reading. It returns an event once for each press, and it has no
// clock or pin of its own, so the logic runs on the host in the tests.
class ButtonClassifier {
 public:
  ButtonEvent update(bool pressed, uint32_t nowMs);

  // How long the button has been held, for a hint on the display, or 0 when it is not held (also after an event that was reported while it was held).
  uint32_t heldMs(uint32_t nowMs) const { return state_ == State::Pressed ? nowMs - pressedAtMs_ : 0; }

 private:
  enum class State : uint8_t { Startup, Idle, Pressed, Used };

  State state_ = State::Startup;
  uint32_t pressedAtMs_ = 0;
};

}  // namespace wattlink
