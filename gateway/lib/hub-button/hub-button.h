#pragma once

#include <stdint.h>

namespace wattlink {

// The rules are in the section "The button of the hub" of docs/espnow-protocol.md.
constexpr uint32_t kBounceMs = 50;
constexpr uint32_t kPortalHoldMs = 3000;

enum class ButtonEvent : uint8_t {
  None,
  Pairing,
  Portal,
};

// Feed it the level of the button and the time of every reading. It returns an event once for each press, and it has no
// clock or pin of its own, so the logic runs on the host in the tests.
class ButtonClassifier {
 public:
  ButtonEvent update(bool pressed, uint32_t nowMs);

 private:
  enum class State : uint8_t { Startup, Idle, Pressed, Used };

  State state_ = State::Startup;
  uint32_t pressedAtMs_ = 0;
};

}  // namespace wattlink
