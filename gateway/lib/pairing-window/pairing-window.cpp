#include "pairing-window.h"

namespace wattlink {

bool PairingWindow::expired(uint32_t nowMs) const {
  return static_cast<int32_t>(nowMs - endMs_) >= 0;
}

PressResult PairingWindow::press(uint32_t nowMs) {
  if (isPairing(nowMs)) {
    return PressResult::Ignored;
  }
  const bool wasOpen = isOpen(nowMs);
  state_ = WindowState::Open;
  endMs_ = nowMs + kPairingWindowMs;
  return wasOpen ? PressResult::Restarted : PressResult::Opened;
}

bool PairingWindow::beginPairing(uint32_t nowMs) {
  if (!isOpen(nowMs)) {
    return false;
  }
  state_ = WindowState::Pairing;
  // The pending time never reaches past the end of the window, so no pairing outlasts the 60 seconds after the last short press.
  const uint32_t pendingEndMs = nowMs + kPairingPendingMs;
  if (static_cast<int32_t>(pendingEndMs - endMs_) < 0) {
    endMs_ = pendingEndMs;
  }
  return true;
}

bool PairingWindow::commit(uint32_t nowMs) {
  if (!isPairing(nowMs)) {
    return false;
  }
  state_ = WindowState::Closed;
  return true;
}

void PairingWindow::close() {
  state_ = WindowState::Closed;
}

WindowState PairingWindow::update(uint32_t nowMs) {
  if (state_ != WindowState::Closed && expired(nowMs)) {
    state_ = WindowState::Closed;
  }
  return state_;
}

bool PairingWindow::isOpen(uint32_t nowMs) const {
  return state_ == WindowState::Open && !expired(nowMs);
}

bool PairingWindow::isPairing(uint32_t nowMs) const {
  return state_ == WindowState::Pairing && !expired(nowMs);
}

uint32_t PairingWindow::secondsLeft(uint32_t nowMs) const {
  if (!isOpen(nowMs)) {
    return 0;
  }
  const uint32_t left = endMs_ - nowMs;
  return (left + 999) / 1000;
}

}  // namespace wattlink
