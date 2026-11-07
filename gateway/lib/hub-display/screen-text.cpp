#include "screen-text.h"

#include <stdio.h>
#include <string.h>

namespace wattlink {

namespace {

Screen emptyScreen() {
  Screen screen;
  memset(&screen, 0, sizeof(screen));
  return screen;
}

// snprintf cuts a line that is too long and always ends it with a zero, so a long value never writes past the line.
void setLine(Screen *screen, const char *text) {
  if (screen->count < kMaxScreenLines) {
    snprintf(screen->line[screen->count], kLineWidth + 1, "%s", text);
    screen->count++;
  }
}

void setLine(Screen *screen, const char *format, const char *value) {
  if (screen->count < kMaxScreenLines) {
    snprintf(screen->line[screen->count], kLineWidth + 1, format, value);
    screen->count++;
  }
}

void setLine(Screen *screen, const char *format, unsigned value) {
  if (screen->count < kMaxScreenLines) {
    snprintf(screen->line[screen->count], kLineWidth + 1, format, value);
    screen->count++;
  }
}

void setLine(Screen *screen, const char *format, unsigned long value) {
  if (screen->count < kMaxScreenLines) {
    snprintf(screen->line[screen->count], kLineWidth + 1, format, value);
    screen->count++;
  }
}

}  // namespace

Screen bootScreen(const char *version) {
  Screen screen = emptyScreen();
  setLine(&screen, text::kBootTitle);
  setLine(&screen, "v%s", version);
  setLine(&screen, text::kBootStarting);
  return screen;
}

Screen portalScreen(const char *apName, const char *setupCode) {
  Screen screen = emptyScreen();
  setLine(&screen, text::kPortalTitle);
  setLine(&screen, text::kPortalWifiFormat, apName);
  setLine(&screen, text::kPortalCodeFormat, setupCode);
  setLine(&screen, text::kPortalOpen);
  return screen;
}

Screen pairingScreen(unsigned secondsLeft) {
  Screen screen = emptyScreen();
  setLine(&screen, text::kPairingTitleFormat, secondsLeft);
  setLine(&screen, text::kPairingHintFirst);
  setLine(&screen, text::kPairingHintSecond);
  return screen;
}

Screen claimScreen(const char *claimCode) {
  Screen screen = emptyScreen();
  setLine(&screen, text::kClaimTitle);
  setLine(&screen, "%s", claimCode);
  setLine(&screen, text::kClaimHint);
  return screen;
}

Screen statusScreen(bool wifiUp, bool serverUp, unsigned nodes, uint32_t watts) {
  Screen screen = emptyScreen();
  setLine(&screen, wifiUp ? text::kWifiConnected : text::kWifiDown);
  setLine(&screen, serverUp ? text::kServerConnected : text::kServerDown);
  setLine(&screen, text::kNodesFormat, nodes);
  setLine(&screen, text::kPowerFormat, static_cast<unsigned long>(watts));
  return screen;
}

}  // namespace wattlink
