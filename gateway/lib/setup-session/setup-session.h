#pragma once

#include <stdint.h>

namespace wattlink {

// The setup network closes after 5 minutes, or earlier when the settings were saved. Only two phones may join, so a third device cannot
// sit on the network next to the owner.
constexpr uint32_t kSetupSessionMs = 5UL * 60UL * 1000UL;
constexpr uint8_t kMaxSetupClients = 2;

enum class SessionState : uint8_t {
  Idle,     // no setup network
  Running,  // the setup network is open
  Done,     // the owner saved the settings
  Expired,  // the time ran out
};

// The state of one setup session. It only does the arithmetic, so the same code runs on the host in the tests and on the chip.
// The time is the millis() counter, which wraps after about 49 days, and the subtractions below are safe across the wrap.
class SetupSession {
 public:
  // Starts a new session. A session that already runs starts again with the full time. The caller makes a new setup code for it.
  void start(uint32_t nowMs);

  // Moves a running session to Expired when its time is over. Returns the state.
  SessionState update(uint32_t nowMs);

  // The owner saved the settings. Only a running session can finish. Done and Expired stay as they are until start() is called.
  void finish();

  // Ends the session without a result, for example when the portal is left by a restart.
  void cancel();

  SessionState state() const { return state_; }

  // Whole seconds left, rounded up, so the display never shows 0 while the network is still open. Zero when the session is not running.
  uint32_t secondsLeft(uint32_t nowMs) const;

  // The access point refuses a client above this limit. The number is what the Wi-Fi stack reports as connected.
  static bool canAcceptClient(uint8_t connected) { return connected < kMaxSetupClients; }

 private:
  SessionState state_ = SessionState::Idle;
  uint32_t startedAtMs_ = 0;
};

}  // namespace wattlink
