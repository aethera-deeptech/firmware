#pragma once

#include <stdint.h>

#include "display-strings.h"

namespace wattlink {

constexpr size_t kMaxScreenLines = 6;

// The countdown of the pairing window is 60 seconds, and a larger number is shown as this one so that the line keeps its width.
constexpr unsigned kMaxPairingSeconds = 999;

// The lines of one screen. Each line holds at most kLineWidth characters and a terminating zero.
struct Screen {
  char line[kMaxScreenLines][kLineWidth + 1];
  uint8_t count;
};

Screen bootScreen(const char *version);
// The setup code is the plain code, and the screen shows it in groups of 4.
Screen portalScreen(const char *apName, const char *setupCode);
Screen pairingScreen(unsigned secondsLeft);
Screen claimScreen(const char *claimCode);
Screen statusScreen(bool wifiUp, bool serverUp, unsigned nodes, uint32_t watts);

}  // namespace wattlink
