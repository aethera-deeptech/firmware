#include "setup-session.h"

namespace wattlink {

bool SetupSession::start(uint32_t nowMs) {
  if (state_ == SessionState::Running) {
    return false;
  }
  state_ = SessionState::Running;
  startedAtMs_ = nowMs;
  return true;
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

bool SetupSession::isOpen(uint32_t nowMs) const {
  return state_ == SessionState::Running && nowMs - startedAtMs_ < kSetupSessionMs;
}

uint32_t SetupSession::secondsLeft(uint32_t nowMs) const {
  if (!isOpen(nowMs)) {
    return 0;
  }
  return (kSetupSessionMs - (nowMs - startedAtMs_) + 999) / 1000;
}

}  // namespace wattlink
