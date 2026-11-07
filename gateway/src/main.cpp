#include <Arduino.h>

#include "config.h"
#include "hub-button.h"
#include "screen-text.h"
// The portal code that uses these comes in the next changes. They are included now so that CI builds them for the 32 bit chip.
#include "setup-session.h"
#include "wifi-form.h"

static wattlink::ButtonClassifier button;

void setup() {
  Serial.begin(115200);
  pinMode(config::kHubButtonPin, INPUT_PULLUP);
  Serial.printf("WATTLINK hub %s\n", config::kFirmwareVersion);
  // The OLED driver comes later. Until then the boot screen goes to the serial port, which also makes CI build the display code for the chip.
  const wattlink::Screen boot = wattlink::bootScreen(config::kFirmwareVersion);
  for (uint8_t i = 0; i < boot.count; i++) {
    Serial.println(boot.line[i]);
  }
}

void loop() {
  const bool pressed = digitalRead(config::kHubButtonPin) == LOW;

  switch (button.update(pressed, millis())) {
    case wattlink::ButtonEvent::Pairing:
      Serial.println("button: open the pairing window");
      break;
    case wattlink::ButtonEvent::Portal:
      Serial.println("button: start the portal");
      break;
    case wattlink::ButtonEvent::None:
      break;
  }

  delay(10);
}
