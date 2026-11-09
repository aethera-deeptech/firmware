#include <Arduino.h>

#include "batch-builder.h"
#include "boot-counter.h"
#include "config.h"
#include "crypto-selftest.h"
#include "espnow-radio.h"
#include "hub-button.h"
#include "hub-link.h"
#include "hub-pairing.h"
#include "node-registry.h"
#include "nvs-counter-store.h"
#include "nvs-record-store.h"
#include "nvs-store.h"
#include "oled.h"
#include "payloads.h"
#include "pairing-window.h"
#include "portal.h"
#include "sample-store.h"
#include "screen-text.h"
#include "settings-store.h"
#include "wifi-client.h"

static wattlink::ButtonClassifier button;
static wattlink::Oled oled;
static wattlink::NvsKeyValueStore flash;
static wattlink::SettingsStore settings(flash);
static wattlink::NvsRecordStore nodeFlash;
static wattlink::NodeRegistry nodes(nodeFlash);
static wattlink::NvsCounterStore counterFlash;
static wattlink::BootCounter bootCounter(counterFlash);
// The boot number of this start. It is 0 until it is made, and the hub must not send a frame or pair with a number of 0.
static uint32_t hubBootId = 0;

// The registry keeps the kind as a plain byte, and this ties it to the kinds of the frames.
static_assert(static_cast<uint8_t>(wattlink::NodeKind::Plug) == wattlink::kKindPlug, "the plug kind of the registry matches the frames");
static_assert(static_cast<uint8_t>(wattlink::NodeKind::Clamp) == wattlink::kKindClamp, "the clamp kind of the registry matches the frames");
static wattlink::Portal portal(settings, oled);
// ESP-NOW follows the Wi-Fi client, which starts it when the radio goes on and stops it before the radio goes off.
static wattlink::EspNowRadio radio;
static wattlink::WifiClient wifi(settings, &radio);
// The counters that the last ESP-NOW log line showed, and the time of that line. The line is printed only when a counter has moved.
static uint32_t lastReceivedLogged = 0;
static uint32_t lastDroppedLogged = 0;
static uint32_t lastRadioLogMs = 0;
static wattlink::PairingWindow pairing;
// The seconds that the pairing screen showed last, so it is drawn again only when the number changes. -1 means that it is not on screen.
static int lastPairingSeconds = -1;
// What the home screen showed last, so that it is drawn again only when it changes. -1 means that it has to be drawn.
static int lastHome = -1;
// The result of the crypto self test stays here, so the pairing code can refuse to start when the chip disagrees with the host tests.
// Until the test has run, the result counts as failed.
static wattlink::SelfTestResult cryptoResult = wattlink::SelfTestResult::HkdfFailed;

// The home screen. It says that the Wi-Fi is not set up yet, or whether the hub is joined. The server line stays "putus" until the client of
// the backend exists, and the node count and the power stay 0 until the nodes are connected.
static void drawHome(bool force) {
  const bool needSetup = wifi.state() == wattlink::LinkState::Idle;
  const int key = needSetup ? 2 : (wifi.connected() ? 1 : 0);
  if (!force && key == lastHome) {
    return;
  }
  lastHome = key;
  if (needSetup) {
    oled.show(wattlink::needSetupScreen());
  } else {
    oled.show(wattlink::statusScreen(wifi.connected(), false, 0, 0));
  }
}

// The screen owner. While the pairing window is open its countdown is on the display, and otherwise the home screen is.
static void drawScreen(uint32_t now) {
  const wattlink::WindowState window = pairing.update(now);
  if (window == wattlink::WindowState::Open) {
    const int seconds = static_cast<int>(pairing.secondsLeft(now));
    if (seconds != lastPairingSeconds) {
      lastPairingSeconds = seconds;
      oled.show(wattlink::pairingScreen(static_cast<unsigned>(seconds)));
    }
    return;
  }
  if (window == wattlink::WindowState::Pairing) {
    return;
  }
  if (lastPairingSeconds != -1) {
    // The window just ended, so the home screen comes back.
    lastPairingSeconds = -1;
    drawHome(true);
    return;
  }
  drawHome(false);
}

// The frames of the nodes wait in the queue of the radio. HubLink is not wired in yet, so they are counted and dropped here. One line shows the
// counters at most every 5 seconds and only when they moved.
static void drainRadio(uint32_t now) {
  wattlink::EspNowRadio::Received frame;
  while (radio.receive(&frame)) {
    // Counted by the callback already, and dropped here until HubLink reads the queue.
  }
  if (now - lastRadioLogMs < 5000) {
    return;
  }
  if (radio.received() != lastReceivedLogged || radio.dropped() != lastDroppedLogged) {
    lastReceivedLogged = radio.received();
    lastDroppedLogged = radio.dropped();
    lastRadioLogMs = now;
    Serial.printf("ESP-NOW: %lu frames received, %lu dropped by a full queue\n", static_cast<unsigned long>(lastReceivedLogged),
                  static_cast<unsigned long>(lastDroppedLogged));
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
  // The registry says how many nodes are paired and how many ids the backend has counted. A damaged slot is named, and it is never reused.
  nodes.load();
  Serial.printf("Nodes: %u paired, %u of %u ids used, %u damaged\n", static_cast<unsigned>(nodes.pairedCount()),
                static_cast<unsigned>(nodes.idCount()), static_cast<unsigned>(wattlink::kMaxNodes), static_cast<unsigned>(nodes.damagedCount()));
  // The boot number is written to flash and read back before any frame is sent. When the counter is lost and keys are kept (paired or damaged
  // slots), numbers could repeat with the old keys, so the counter gives none, at every start, until the keys are gone.
  const bool keysExist = nodes.pairedCount() > 0 || nodes.damagedCount() > 0;
  const wattlink::BootResult bootStatus = bootCounter.next(keysExist, &hubBootId);
  if (hubBootId != 0) {
    Serial.printf("Hub boot number: %lu\n", static_cast<unsigned long>(hubBootId));
  } else if (bootStatus == wattlink::BootResult::Lost) {
    Serial.println("Hub boot number: the counter was lost while nodes are paired, so the hub does not pair, erase the flash to start again");
  } else {
    Serial.println("Hub boot number: FAILED, the hub does not pair");
  }
  // The known answers of the host tests, run through the mbedTLS of the chip.
  const uint32_t selfTestStart = millis();
  cryptoResult = wattlink::runCryptoSelfTest();
  Serial.printf("Crypto self test: %s (%lu ms)\n", wattlink::selfTestName(cryptoResult), static_cast<unsigned long>(millis() - selfTestStart));
  wifi.begin(millis());
  drawHome(true);
}

void loop() {
  const uint32_t now = millis();

  if (portal.isRunning()) {
    // The button is ignored while the portal runs. The portal ends by itself when the settings are saved or the time is over.
    portal.update(now);
    if (!portal.isRunning()) {
      // The portal is over. The Wi-Fi is joined with what is saved now, which is the new setting after a save.
      wifi.begin(now);
      drawHome(true);
    }
    delay(2);
    return;
  }

  // The button is read before any redraw, because a redraw takes tens of milliseconds and would make the press look longer.
  const bool pressed = digitalRead(config::kHubButtonPin) == LOW;

  wifi.update(now);
  drainRadio(now);
  drawScreen(now);

  switch (button.update(pressed, now)) {
    case wattlink::ButtonEvent::Pairing:
      // A chip whose crypto disagrees with the host tests must not pair. A result that has not been set counts as failed.
      if (cryptoResult != wattlink::SelfTestResult::Ok) {
        Serial.println("Pairing: refused, the crypto self test did not pass");
        break;
      }
      if (hubBootId == 0) {
        Serial.println("Pairing: refused, the hub has no boot number");
        break;
      }
      switch (pairing.press(now)) {
        case wattlink::PressResult::Opened:
          Serial.println("Pairing: window opened");
          break;
        case wattlink::PressResult::Restarted:
          Serial.println("Pairing: window restarted");
          break;
        case wattlink::PressResult::Ignored:
          break;
      }
      break;
    case wattlink::ButtonEvent::Portal:
      // The portal refuses to start without a display, because the setup code can only be shown there. That is checked before the link to
      // the shop Wi-Fi is dropped, so a refusal costs nothing.
      if (!portal.canStart()) {
        Serial.println("Portal: not started");
        break;
      }
      // The setup network needs the radio, so the link is dropped first. The portal also closes the pairing window.
      pairing.close();
      lastPairingSeconds = -1;
      wifi.stop();
      if (!portal.start(now)) {
        Serial.println("Portal: not started");
        wifi.begin(now);
        drawHome(true);
      }
      break;
    case wattlink::ButtonEvent::None:
      break;
  }

  delay(10);
}
