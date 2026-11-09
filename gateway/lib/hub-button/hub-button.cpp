#include "hub-button.h"

namespace wattlink {

// The first reading of a release starts the check, and the release is confirmed when the readings stay released for kBounceMs. A reading of the button as
// pressed ends the check. The time of the release for the length of the hold is the time of the first reading of it.
bool ButtonClassifier::releaseConfirmed(bool pressed, uint32_t nowMs) {
  if (pressed) {
    releasing_ = false;
    return false;
  }
  if (!releasing_) {
    releasing_ = true;
    releaseSeenAtMs_ = nowMs;
    return false;
  }
  // The subtraction stays right when the millisecond counter wraps.
  return nowMs - releaseSeenAtMs_ >= kBounceMs;
}

ButtonEvent ButtonClassifier::update(bool pressed, uint32_t nowMs) {
  switch (state_) {
    case State::Startup:
      // A button that is already down when the firmware starts is ignored until the hub has seen it released.
      if (!pressed) {
        state_ = State::Idle;
      }
      return ButtonEvent::None;

    case State::Idle:
      if (pressed) {
        state_ = State::Pressed;
        pressedAtMs_ = nowMs;
        releasing_ = false;
      }
      return ButtonEvent::None;

    case State::Pressed: {
      if (pressed) {
        releasing_ = false;
        // The portal waits for the release, because a hold of 10 seconds must not start it on the way.
        if (nowMs - pressedAtMs_ >= kNewCodeHoldMs) {
          state_ = State::Used;
          return ButtonEvent::NewCode;
        }
        return ButtonEvent::None;
      }
      if (!releaseConfirmed(pressed, nowMs)) {
        return ButtonEvent::None;
      }
      state_ = State::Idle;
      releasing_ = false;
      const uint32_t heldMs = releaseSeenAtMs_ - pressedAtMs_;
      // A release after 10 seconds that no reading saw while the button was held still counts as that hold, and one after 3 seconds as the portal. Neither is
      // ever a short press.
      if (heldMs >= kNewCodeHoldMs) {
        return ButtonEvent::NewCode;
      }
      if (heldMs >= kPortalHoldMs) {
        return ButtonEvent::Portal;
      }
      return heldMs >= kBounceMs ? ButtonEvent::Pairing : ButtonEvent::None;
    }

    case State::Used:
      if (releaseConfirmed(pressed, nowMs)) {
        state_ = State::Idle;
        releasing_ = false;
      }
      return ButtonEvent::None;
  }
  return ButtonEvent::None;
}

}  // namespace wattlink
