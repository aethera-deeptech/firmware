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
        if (heldMs >= kPortalHoldMs) {
          state_ = State::Used;
          return ButtonEvent::Portal;
        }
        return ButtonEvent::None;
      }
      state_ = State::Idle;
      // A release after 3 seconds that no reading saw while the button was held still counts as the hold, never as a short press.
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
