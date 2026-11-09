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

  // How long the button has been held, for a hint on the display. It is 0 when the button is not held, while a release is being checked, and after an event
  // that was reported while the button was held. A value of nowMs that is older than the last reading gives 0.
  uint32_t heldMs(uint32_t nowMs) const {
    if (state_ != State::Pressed || releasing_) {
      return 0;
    }
    const uint32_t held = nowMs - pressedAtMs_;
    return static_cast<int32_t>(held) < 0 ? 0 : held;
  }

 private:
  enum class State : uint8_t { Startup, Idle, Pressed, Used };

  // A release counts only when the readings stay released for kBounceMs, so a glitch of the contact in the middle of a hold does not end it.
  bool releaseConfirmed(bool pressed, uint32_t nowMs);

  State state_ = State::Startup;
  uint32_t pressedAtMs_ = 0;
  bool releasing_ = false;
  uint32_t releaseSeenAtMs_ = 0;
};

}  // namespace wattlink
