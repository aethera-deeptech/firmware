#include "oled.h"

#include <U8g2lib.h>

#include "config.h"

namespace wattlink {

namespace {

// A full buffer in RAM (1 KB). The clock and data pins are given, because the pins of the hub are not the defaults of the board.
U8G2_SSD1306_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE, config::kOledSclPin, config::kOledSdaPin);

constexpr uint8_t kLineHeight = 10;

static_assert(kMaxScreenLines * kLineHeight <= 64, "the lines must fit the height of the display");

}  // namespace

bool Oled::begin() {
  // U8g2 takes the I2C address multiplied by 2, and the call must come before begin().
  display.setI2CAddress(config::kOledAddress << 1);
  if (!display.begin()) {
    return false;
  }
  display.setFont(u8g2_font_6x10_tf);
  return true;
}

void Oled::show(const Screen &screen) {
  display.clearBuffer();
  for (uint8_t i = 0; i < screen.count && i < kMaxScreenLines; i++) {
    // drawStr puts the baseline at y, so the first line is at y = 10 and its text stays inside the first 10 rows.
    display.drawStr(0, (i + 1) * kLineHeight, screen.line[i]);
  }
  display.sendBuffer();
}

void Oled::clear() {
  display.clearDisplay();
}

}  // namespace wattlink
