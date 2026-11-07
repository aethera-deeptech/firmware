#include <Arduino.h>

#include "config.h"
#include "hub-button.h"
#include "nvs-store.h"
#include "oled.h"
#include "portal.h"
#include "screen-text.h"
#include "settings-store.h"

static wattlink::ButtonClassifier button;
static wattlink::Oled oled;
static wattlink::NvsKeyValueStore flash;
static wattlink::SettingsStore settings(flash);
static wattlink::Portal portal(settings, oled);

void setup() {
  Serial.begin(115200);
  pinMode(config::kHubButtonPin, INPUT_PULLUP);
  Serial.printf("WATTLINK hub %s\n", config::kFirmwareVersion);
  const wattlink::Screen boot = wattlink::bootScreen(config::kFirmwareVersion);
  if (oled.begin()) {
    oled.show(boot);
  } else {
    // No display answered. The boot screen has nothing secret, so it goes to the serial port instead.
    Serial.println("OLED: no answer at the address");
    for (uint8_t i = 0; i < boot.count; i++) {
      Serial.println(boot.line[i]);
    }
  }
  // The line says what is stored and never shows the password or the key.
  char stored[96];
  settings.describe(stored, sizeof(stored));
  Serial.printf("Settings: %s\n", stored);
}

void loop() {
  const uint32_t now = millis();

  if (portal.isRunning()) {
    // The button is ignored while the portal runs. The portal ends by itself when the settings are saved or the time is over.
    portal.update(now);
    if (!portal.isRunning()) {
      oled.show(wattlink::bootScreen(config::kFirmwareVersion));
    }
    delay(2);
    return;
  }

  const bool pressed = digitalRead(config::kHubButtonPin) == LOW;

  switch (button.update(pressed, now)) {
    case wattlink::ButtonEvent::Pairing:
      Serial.println("button: open the pairing window");
      break;
    case wattlink::ButtonEvent::Portal:
      // The portal refuses to start without a display, because the setup code can only be shown there.
      if (!portal.start(now)) {
        Serial.println("Portal: not started");
      }
      break;
    case wattlink::ButtonEvent::None:
      break;
  }

  delay(10);
}
