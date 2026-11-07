#include "link-supervisor.h"

namespace wattlink {

uint32_t Backoff::next() {
  const uint32_t current = nextMs_;
  nextMs_ = nextMs_ >= capMs_ / 2 ? capMs_ : nextMs_ * 2;
  return current;
}

void LinkSupervisor::enable(uint32_t nowMs) {
  if (state_ != LinkState::Idle) {
    return;
  }
  backoff_.reset();
  state_ = LinkState::Waiting;
  retryAtMs_ = nowMs;
}

void LinkSupervisor::disable() {
  state_ = LinkState::Idle;
  backoff_.reset();
}

void LinkSupervisor::wait(uint32_t nowMs) {
  state_ = LinkState::Waiting;
  retryAtMs_ = nowMs + backoff_.next();
}

LinkAction LinkSupervisor::update(uint32_t nowMs, bool connected) {
  switch (state_) {
    case LinkState::Idle:
      return LinkAction::None;

    case LinkState::Waiting:
      // The core makes one try of its own after the first drop, so the link can come back while this class waits.
      if (connected) {
        state_ = LinkState::Connected;
        connectedAtMs_ = nowMs;
        return LinkAction::None;
      }
      if (static_cast<int32_t>(nowMs - retryAtMs_) >= 0) {
        state_ = LinkState::Connecting;
        attemptStartedAtMs_ = nowMs;
        return LinkAction::StartConnect;
      }
      return LinkAction::None;

    case LinkState::Connecting:
      if (connected) {
        state_ = LinkState::Connected;
        connectedAtMs_ = nowMs;
        return LinkAction::None;
      }
      if (nowMs - attemptStartedAtMs_ >= kConnectTimeoutMs) {
        wait(nowMs);
        return LinkAction::Abort;
      }
      return LinkAction::None;

    case LinkState::Connected:
      if (!connected) {
        // Only a link that stayed up for a while starts the waits again at the base.
        if (nowMs - connectedAtMs_ >= kStableLinkMs) {
          backoff_.reset();
        }
        wait(nowMs);
      }
      return LinkAction::None;
  }
  return LinkAction::None;
}

uint32_t LinkSupervisor::secondsToRetry(uint32_t nowMs) const {
  if (state_ != LinkState::Waiting) {
    return 0;
  }
  const int32_t left = static_cast<int32_t>(retryAtMs_ - nowMs);
  if (left <= 0) {
    return 0;
  }
  return (static_cast<uint32_t>(left) + 999) / 1000;
}

}  // namespace wattlink
