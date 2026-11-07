#pragma once

#include "screen-text.h"

namespace wattlink {

// The 0.96 inch SSD1306 display of the hub. It shows the lines of a Screen with a 6 pixel wide font, which gives the 21 characters of a
// line (kLineWidth) on the 128 pixels, and 6 lines of 10 pixels on the 64 pixels (kMaxScreenLines).
// Nothing here writes to a log, because the setup screen shows the setup code and it must appear only on the display.
class Oled {
 public:
  // Starts the display on the pins and the address of config.h. Returns false when nothing answers at the address of the display, or
  // when the library fails to start. U8g2 itself cannot tell whether a display is connected, so begin() sends an empty I2C message to the
  // address and checks that it is acknowledged. A caller that has a secret to show must stop when this returns false.
  bool begin();

  // True after begin() found the display. A caller that has a secret to show checks this first.
  bool ready() const { return ready_; }

  // Draws the screen and clears everything that was there. Does nothing when the display is not ready.
  void show(const Screen &screen);

  // Blanks the display. Does nothing when the display is not ready.
  void clear();

 private:
  bool ready_ = false;
};

}  // namespace wattlink
