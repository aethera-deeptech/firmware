#pragma once

#include "screen-text.h"

namespace wattlink {

// The 0.96 inch SSD1306 display of the hub. It shows the lines of a Screen with a 6 pixel wide font, which gives the 21 characters of a
// line (kLineWidth) on the 128 pixels, and 6 lines of 10 pixels on the 64 pixels (kMaxScreenLines).
// Nothing here writes to a log, because the setup screen shows the setup code and it must appear only on the display.
class Oled {
 public:
  // Starts the display on the pins and the address of config.h. Returns false when the library fails to start. The library cannot tell
  // whether a display is connected, so a true result does not prove that a screen is there.
  bool begin();

  // Draws the screen and clears everything that was there.
  void show(const Screen &screen);

  // Blanks the display.
  void clear();
};

}  // namespace wattlink
