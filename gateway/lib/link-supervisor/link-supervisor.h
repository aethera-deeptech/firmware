#pragma once

#include <stdint.h>

namespace wattlink {

// The wait between two tries doubles from 2 seconds up to 5 minutes, which also matches what the firmware guide of the backend asks for a
// service that does not answer (a pause that grows from 5 seconds to 5 minutes).
constexpr uint32_t kBackoffBaseMs = 2000;
constexpr uint32_t kBackoffCapMs = 300000;

// A try that has not joined after this time is given up.
constexpr uint32_t kConnectTimeoutMs = 15000;

// The wait before the next try. next() gives the current wait and then doubles it up to the cap, and reset() starts again at the base.
class Backoff {
 public:
  uint32_t next();
  void reset() { nextMs_ = kBackoffBaseMs; }

 private:
  uint32_t nextMs_ = kBackoffBaseMs;
};

enum class LinkState : uint8_t {
  Idle,        // not asked to join
  Connecting,  // a try is running
  Connected,
  Waiting,  // the last try failed or the link was lost, and the next try is at retryAt
};

// What the caller has to do after update().
enum class LinkAction : uint8_t {
  None,
  StartConnect,  // start a try now
  Abort,         // stop the try that ran too long
};

// The state of one link. It does only the arithmetic and tells the caller what to do, so the same code runs on the host in the tests and on
// the chip with Wi-Fi. The time is the millis() counter, which wraps after about 49 days, and the subtractions are safe across the wrap.
// One task must call all of it.
class LinkSupervisor {
 public:
  // Starts to join at the next update(). Asking again while the link is on does nothing.
  void enable(uint32_t nowMs);

  // Stops. The caller drops the link itself.
  void disable();

  // connected is what the network stack says now. Call it often, for example on every pass of loop().
  LinkAction update(uint32_t nowMs, bool connected);

  LinkState state() const { return state_; }

  // Whole seconds until the next try, rounded up. Zero when no try is waiting.
  uint32_t secondsToRetry(uint32_t nowMs) const;

 private:
  void wait(uint32_t nowMs);

  LinkState state_ = LinkState::Idle;
  uint32_t attemptStartedAtMs_ = 0;
  uint32_t retryAtMs_ = 0;
  Backoff backoff_;
};

}  // namespace wattlink
