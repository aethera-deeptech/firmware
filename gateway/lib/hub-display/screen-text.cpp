#include "screen-text.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "setup-code.h"

namespace wattlink {

namespace {

Screen emptyScreen() {
  Screen screen;
  memset(&screen, 0, sizeof(screen));
  return screen;
}

// vsnprintf cuts a line that is too long and always ends it with a zero, so a long value never writes past the line.
// The format attribute makes the compiler check the format against the arguments.
__attribute__((format(printf, 2, 3))) void appendLine(Screen *screen, const char *format, ...) {
  if (screen->count >= kMaxScreenLines) {
    return;
  }
  va_list args;
  va_start(args, format);
  vsnprintf(screen->line[screen->count], kLineWidth + 1, format, args);
  va_end(args);
  screen->count++;
}

// A null pointer given to %s is undefined, so a missing value shows as an empty text.
const char *orEmpty(const char *value) {
  return value != nullptr ? value : "";
}

}  // namespace

Screen bootScreen(const char *version) {
  Screen screen = emptyScreen();
  appendLine(&screen, "%s", text::kBootTitle);
  appendLine(&screen, "v%s", orEmpty(version));
  appendLine(&screen, "%s", text::kBootStarting);
  return screen;
}

Screen portalScreen(const char *apName, const char *setupCode) {
  Screen screen = emptyScreen();
  appendLine(&screen, "%s", text::kPortalTitle);
  appendLine(&screen, text::kPortalWifiFormat, orEmpty(apName));
  char shown[kSetupCodeShownSize];
  formatSetupCode(setupCode, shown);
  appendLine(&screen, text::kPortalCodeFormat, shown);
  appendLine(&screen, "%s", text::kPortalOpen);
  appendLine(&screen, "%s", text::kPortalTypeHint);
  return screen;
}

Screen pairingScreen(unsigned secondsLeft) {
  Screen screen = emptyScreen();
  appendLine(&screen, text::kPairingTitleFormat, secondsLeft > kMaxPairingSeconds ? kMaxPairingSeconds : secondsLeft);
  appendLine(&screen, "%s", text::kPairingHintFirst);
  appendLine(&screen, "%s", text::kPairingHintSecond);
  return screen;
}

Screen claimScreen(const char *claimCode) {
  Screen screen = emptyScreen();
  appendLine(&screen, "%s", text::kClaimTitle);
  appendLine(&screen, "%s", orEmpty(claimCode));
  appendLine(&screen, "%s", text::kClaimHint);
  return screen;
}

Screen statusScreen(bool wifiUp, bool serverUp, unsigned nodes, uint32_t watts) {
  Screen screen = emptyScreen();
  appendLine(&screen, "%s", wifiUp ? text::kWifiConnected : text::kWifiDown);
  appendLine(&screen, "%s", serverUp ? text::kServerConnected : text::kServerDown);
  appendLine(&screen, text::kNodesFormat, nodes);
  appendLine(&screen, text::kPowerFormat, static_cast<unsigned long>(watts));
  return screen;
}

}  // namespace wattlink
