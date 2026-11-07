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
// The class cannot close the access point. The caller closes it as soon as isOpen() is false, and does not rely on a state that it read
// earlier. The class is not thread safe, so one task must call all of it. Read millis() once after any start(), because a time that is
// older than the start time looks like a very long session and ends it at once, which is safe but surprising.
class SetupSession {
 public:
  // Starts a new session and returns true. A session that is already running is left alone and the call returns false, so nothing
  // that reaches this call can keep the setup network open beyond the limit. The caller makes a new setup code for a new session.
  bool start(uint32_t nowMs);

  // Moves a running session to Expired when its time is over. Returns the state.
  SessionState update(uint32_t nowMs);

  // The owner saved the settings. Only a running session can finish. Done and Expired stay as they are until start() is called.
  void finish();

  // Ends the session without a result, for example when the portal is left by a restart.
  void cancel();

  SessionState state() const { return state_; }

  // True while the session runs and its time is not over. This does not depend on update() being called, so the access point code uses
  // it for every decision to keep the network up.
  bool isOpen(uint32_t nowMs) const;

  // Whole seconds left, rounded up, so the display shows 1 for the last second. Zero when the session is not open.
  uint32_t secondsLeft(uint32_t nowMs) const;

  // Only a predicate. The real limit is the fifth argument of WiFi.softAP, which must be kMaxSetupClients, because the stack default is 4.
  // The stack may still count a phone that left until it times out, so the owner can be refused for a short while. That is acceptable.
  static bool canAcceptClient(uint8_t connected) { return connected < kMaxSetupClients; }

 private:
  SessionState state_ = SessionState::Idle;
  uint32_t startedAtMs_ = 0;
};

}  // namespace wattlink
