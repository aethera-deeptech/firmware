#include <Arduino.h>

#include "config.h"
#include "crypto-selftest.h"
#include "hub-button.h"
#include "nvs-store.h"
#include "oled.h"
#include "portal.h"
#include "screen-text.h"
#include "settings-store.h"
#include "wifi-client.h"

static wattlink::ButtonClassifier button;
static wattlink::Oled oled;
static wattlink::NvsKeyValueStore flash;
static wattlink::SettingsStore settings(flash);
static wattlink::Portal portal(settings, oled);
static wattlink::WifiClient wifi(settings);
static uint32_t lastScreenMs = 0;
// The result of the crypto self test stays here, so the pairing code can refuse to start when the chip disagrees with the host tests.
// Until the test has run, the result counts as failed.
static wattlink::SelfTestResult cryptoResult = wattlink::SelfTestResult::HkdfFailed;

// The home screen. It says that the Wi-Fi is not set up yet, or whether the hub is joined. The server line stays "putus" until the client of
// the backend exists, and the node count and the power stay 0 until the nodes are connected.
static void drawHome() {
  if (wifi.state() == wattlink::LinkState::Idle) {
    oled.show(wattlink::needSetupScreen());
  } else {
    oled.show(wattlink::statusScreen(wifi.connected(), false, 0, 0));
  }
}

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
  // The known answers of the host tests, run through the mbedTLS of the chip.
  cryptoResult = wattlink::runCryptoSelfTest();
  Serial.printf("Crypto self test: %s\n", wattlink::selfTestName(cryptoResult));
  wifi.begin(millis());
  drawHome();
}

void loop() {
  const uint32_t now = millis();

  if (portal.isRunning()) {
    // The button is ignored while the portal runs. The portal ends by itself when the settings are saved or the time is over.
    portal.update(now);
    if (!portal.isRunning()) {
      // The portal is over. The Wi-Fi is joined with what is saved now, which is the new setting after a save.
      wifi.begin(now);
      drawHome();
    }
    delay(2);
    return;
  }

  wifi.update(now);
  if (now - lastScreenMs >= 1000) {
    lastScreenMs = now;
    drawHome();
  }

  const bool pressed = digitalRead(config::kHubButtonPin) == LOW;

  switch (button.update(pressed, now)) {
    case wattlink::ButtonEvent::Pairing:
      Serial.println("button: open the pairing window");
      break;
    case wattlink::ButtonEvent::Portal:
      // The setup network needs the radio, so the link to the shop Wi-Fi is dropped first.
      wifi.stop();
      // The portal refuses to start without a display, because the setup code can only be shown there.
      if (!portal.start(now)) {
        Serial.println("Portal: not started");
        wifi.begin(now);
        drawHome();
      }
      break;
    case wattlink::ButtonEvent::None:
      break;
  }

  delay(10);
}
