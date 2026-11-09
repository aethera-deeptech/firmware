#pragma once

#include <stddef.h>
#include <stdint.h>

#include "frame-codec.h"
#include "hub-link.h"
#include "wifi-client.h"

namespace wattlink {

// The ESP-NOW link of the hub on the chip. It is the RadioPort that HubLink sends through, and it is the queue that the receive callback fills.
// ESP-NOW only works while the radio is in station mode, so this class follows the Wi-Fi client. It starts when the client switches the radio on
// and it stops before the client switches the radio off, because the portal and a missing setup both need the radio for something else.
// Only one instance may exist, because the callbacks of the stack have no argument that could name an instance, so the queue is one for the whole firmware.
// One task must call receive() and send(). The receive callback runs in the Wi-Fi task and only copies the frame into the queue, because the
// work of HubLink writes flash and needs a big stack.
class EspNowRadio : public RadioPort, public RadioListener {
 public:
  // One frame as the callback saw it. The data is the frame as it came off the air, with the header and the tag still in place.
  struct Received {
    uint8_t mac[kMacSize];
    uint8_t length;
    uint8_t data[kMaxFrameSize];
  };

  // Starts ESP-NOW. Returns true when it runs, also when it ran already. It returns false when the stack refused, and then nothing is left running.
  bool begin();
  // Stops ESP-NOW and drops the frames that wait in the queue. Safe to call when it is not running.
  void end();
  bool active() const { return active_; }

  // The Wi-Fi client calls these when it switches the radio on and off.
  void onRadioOn() override;
  void onRadioOff() override;

  // Takes the oldest waiting frame. Returns false at once when the queue is empty.
  bool receive(Received *out);

  // Adds the node as a temporary peer, sends, waits for the link layer to report on the send and removes the peer. The wait is 50 ms at most.
  // Returns true when the stack took the frame. A frame that the node did not acknowledge still returns true and is counted in failed(). A peer that exists already is used and left in place.
  bool send(const uint8_t mac[kMacSize], const uint8_t *frame, size_t length) override;

  // Counters since the start of the chip, for the log. dropped() counts frames that the callback threw away because the queue was full.
  // failed() counts sends that the link layer reported as not acknowledged, and timeouts() counts sends with no report within the wait.
  // A send that times out and is reported late counts in both.
  uint32_t received() const;
  uint32_t dropped() const;
  uint32_t sent() const { return sent_; }
  uint32_t failed() const;
  uint32_t timeouts() const;

 private:
  bool active_ = false;
  uint32_t sent_ = 0;
  bool addErrorLogged_ = false;  // one log line for each start when a peer cannot be added, so a flood of sends cannot flood the log
  uint32_t timeouts_ = 0;  // written by the sending task only, while the callback counters are written by the Wi-Fi task only
};

}  // namespace wattlink
