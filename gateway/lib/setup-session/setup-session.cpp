#include "setup-session.h"

namespace wattlink {

void SetupSession::start(uint32_t nowMs) {
  state_ = SessionState::Running;
  startedAtMs_ = nowMs;
}

SessionState SetupSession::update(uint32_t nowMs) {
  if (state_ == SessionState::Running && nowMs - startedAtMs_ >= kSetupSessionMs) {
    state_ = SessionState::Expired;
  }
  return state_;
}

void SetupSession::finish() {
  if (state_ == SessionState::Running) {
    state_ = SessionState::Done;
  }
}

void SetupSession::cancel() {
  state_ = SessionState::Idle;
}

uint32_t SetupSession::secondsLeft(uint32_t nowMs) const {
  if (state_ != SessionState::Running) {
    return 0;
  }
  const uint32_t elapsed = nowMs - startedAtMs_;
  if (elapsed >= kSetupSessionMs) {
    return 0;
  }
  return (kSetupSessionMs - elapsed + 999) / 1000;
}

}  // namespace wattlink
