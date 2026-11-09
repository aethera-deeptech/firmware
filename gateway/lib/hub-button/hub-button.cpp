#include "hub-button.h"

namespace wattlink {

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
      }
      return ButtonEvent::None;

    case State::Pressed: {
      // The subtraction stays right when the millisecond counter wraps.
      const uint32_t heldMs = nowMs - pressedAtMs_;
      if (pressed) {
        // The portal waits for the release, because a hold of 10 seconds must not start it on the way.
        if (heldMs >= kNewCodeHoldMs) {
          state_ = State::Used;
          return ButtonEvent::NewCode;
        }
        return ButtonEvent::None;
      }
      state_ = State::Idle;
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
      if (!pressed) {
        state_ = State::Idle;
      }
      return ButtonEvent::None;
  }
  return ButtonEvent::None;
}

}  // namespace wattlink
