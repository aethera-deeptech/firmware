#include <Arduino.h>

#include "config.h"
#include "hub-button.h"

static wattlink::ButtonClassifier button;

void setup() {
  Serial.begin(115200);
  pinMode(config::kHubButtonPin, INPUT_PULLUP);
  Serial.printf("WATTLINK hub %s\n", config::kFirmwareVersion);
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
