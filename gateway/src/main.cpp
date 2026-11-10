#include <Arduino.h>
#include <esp_system.h>
#include <nvs.h>
#include <new>

#include "batch-builder.h"
#include "boot-counter.h"
#include "command-relay.h"
#include "config.h"
#include "crypto-selftest.h"
#include "espnow-radio.h"
#include "flash-command-log.h"
#include "hub-button.h"
#include "hub-link.h"
#include "hub-pairing.h"
#include "mbedtls-crypto.h"
#include "mbedtls-x25519.h"
#include "node-registry.h"
#include "nvs-command-blob-store.h"
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
// The pool of samples is about 58 KB, so it is a static object and never on the stack. The link layer and the pairing need the boot number of the
// hub, which exists only after the counter has run in setup(), so they are built there, in this memory. They stay null when the hub must not pair,
// which means that the crypto self test failed or that there is no boot number. Frames are then read and dropped, and nothing is sent.
static wattlink::SampleStore sampleStore;
alignas(wattlink::HubPairing) static uint8_t hubPairingMemory[sizeof(wattlink::HubPairing)];
alignas(wattlink::HubLink) static uint8_t hubLinkMemory[sizeof(wattlink::HubLink)];
static wattlink::HubPairing *hubPairing = nullptr;
static wattlink::HubLink *hubLink = nullptr;
// The relay of the commands of the backend. It sends a command to its plug with the next frame of that plug, so it needs the link layer and the boot
// number, and it is built next to them. The backend client does not hand it commands yet, so for now it only reads its log and follows the frames.
static wattlink::NvsCommandBlobStore commandFlash;
static wattlink::FlashCommandLog commandLog(commandFlash);
alignas(wattlink::CommandRelay) static uint8_t commandRelayMemory[sizeof(wattlink::CommandRelay)];
static wattlink::CommandRelay *commandRelay = nullptr;
// The frames that one pass of loop() takes from the queue. HubLink writes flash for a frame, so the button and the Wi-Fi watch must still get their turn.
constexpr int kFramesPerPass = 4;
// The time of the last expiry of old samples. The pool drops a sample after about 24 hours, and this keeps the clock reads far from the wrap.
static uint32_t lastExpireMs = 0;
constexpr uint32_t kExpireEveryMs = 60000;
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
  const unsigned paired = static_cast<unsigned>(nodes.pairedCount());
  const int key = static_cast<int>(paired) * 3 + (needSetup ? 2 : (wifi.connected() ? 1 : 0));
  if (!force && key == lastHome) {
    return;
  }
  lastHome = key;
  if (needSetup) {
    oled.show(wattlink::needSetupScreen());
  } else {
    oled.show(wattlink::statusScreen(wifi.connected(), false, paired, 0));
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

// One line for each event that matters. The frames of a normal run (a HELLO, a TELEMETRY frame) are not logged one by one, because a node sends
// one every few seconds. A line never holds a key, and it names a node by its id and not by its MAC.
static void logLinkEvent(const wattlink::LinkEvent &event) {
  switch (event.kind) {
    case wattlink::LinkEventKind::PairRequest:
      // A request with a closed window, a busy window or a bad payload comes from anyone in range and needs no key, so it is not logged.
      if (event.request == wattlink::RequestResult::Accepted || event.request == wattlink::RequestResult::Refused ||
          event.request == wattlink::RequestResult::CryptoFailed) {
        Serial.printf("Link: pair request, result %d, reason %d\n", static_cast<int>(event.request), static_cast<int>(event.refusal));
      }
      break;
    case wattlink::LinkEventKind::PairedNode:
      Serial.printf("Link: paired %s, %u samples in its first frame\n", event.nodeId, static_cast<unsigned>(event.samples));
      break;
    case wattlink::LinkEventKind::PairedNoAck:
      Serial.printf("Link: paired %s, but its first frame was not answered\n", event.nodeId);
      break;
    case wattlink::LinkEventKind::PairingFailed:
      Serial.printf("Link: pairing failed, reason %d\n", static_cast<int>(event.refusal));
      break;
    case wattlink::LinkEventKind::ResultReceived: {
      // The event holds the MAC and not the id, and a line names a node by its id.
      char id[wattlink::kNodeIdSize];
      if (!nodes.nodeId(event.mac, id)) {
        strcpy(id, "?");
      }
      Serial.printf("Link: %s answered command number %u, %s\n", id, static_cast<unsigned>(event.commandNumber),
                    event.commandOk ? "switched" : "did not switch");
      break;
    }
    case wattlink::LinkEventKind::StoreFailed:
      Serial.println("Link: a frame was dropped because the flash refused the boot value");
      break;
    case wattlink::LinkEventKind::HubBootMismatch:
      Serial.printf("Link: %s names another boot of the hub\n", event.nodeId);
      break;
    default:
      break;
  }
}

// The frames of the nodes wait in the queue of the radio, and this is the one place that takes them out. When the link layer does not exist the
// frames are read and dropped, so the queue never stays full. One line shows the counters at most every 5 seconds and only when they moved.
static void serviceRadio(uint32_t now) {
  wattlink::EspNowRadio::Received frame;
  for (int i = 0; i < kFramesPerPass && radio.receive(&frame); i++) {
    if (hubLink != nullptr) {
      // The clock is read for each frame, because a frame before this one may have waited for a send, and the time of a frame is the time of its
      // arrival for the window, the limit of HELLO answers and the capture time of its samples.
      const uint32_t arrivedMs = millis();
      const wattlink::LinkEvent event = hubLink->onFrame(arrivedMs, frame.mac, frame.data, frame.length);
      logLinkEvent(event);
      // A frame of a plug is the moment to send its command, and a RESULT closes the command.
      if (commandRelay != nullptr) {
        commandRelay->onEvent(arrivedMs, event);
      }
    }
  }
  if (hubPairing != nullptr) {
    // The radio can go off with the window open, when the saved Wi-Fi is gone. No node can answer then, so the window is closed.
    if (!radio.active()) {
      hubPairing->close();
    }
    hubPairing->update(millis());
  }
  if (commandRelay != nullptr) {
    commandRelay->update(now);
  }
  if (now - lastExpireMs >= kExpireEveryMs) {
    lastExpireMs = now;
    sampleStore.expire(now);
  }
  if (now - lastRadioLogMs < 5000) {
    return;
  }
  if (radio.received() != lastReceivedLogged || radio.dropped() != lastDroppedLogged) {
    lastReceivedLogged = radio.received();
    lastDroppedLogged = radio.dropped();
    lastRadioLogMs = now;
    Serial.printf("ESP-NOW: %lu frames received, %lu dropped by a full queue, %u samples held, %lu bytes of heap free\n",
                  static_cast<unsigned long>(lastReceivedLogged), static_cast<unsigned long>(lastDroppedLogged),
                  static_cast<unsigned>(sampleStore.size()), static_cast<unsigned long>(ESP.getFreeHeap()));
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
  // The link layer and the pairing are built only when the hub may pair, which is when the chip passed its self test and the boot number exists.
  if (cryptoResult == wattlink::SelfTestResult::Ok && hubBootId != 0) {
    uint8_t hubMac[wattlink::kMacSize];
    esp_read_mac(hubMac, ESP_MAC_WIFI_STA);
    wattlink::HubCrypto crypto;
    crypto.context = nullptr;
    crypto.hmac = &wattlink::mbedtlsHmacSha256;
    crypto.open = &wattlink::mbedtlsAesGcmOpen;
    crypto.seal = &wattlink::mbedtlsAesGcmSeal;
    crypto.generate = &wattlink::mbedtlsX25519Generate;
    crypto.agree = &wattlink::mbedtlsX25519Agree;
    hubPairing = new (hubPairingMemory) wattlink::HubPairing(pairing, nodes, crypto, hubMac, hubBootId);
    hubLink = new (hubLinkMemory) wattlink::HubLink(*hubPairing, nodes, radio, sampleStore, crypto);
    Serial.println("Link: ready");
    // The relay reads its log once. A slot that the flash cannot read is held, and a switch on waits while one exists. The line says how many.
    commandRelay = new (commandRelayMemory) wattlink::CommandRelay(*hubLink, nodes, commandLog, hubBootId);
    const size_t held = commandRelay->load();
    Serial.printf("Commands: %u records held, %u slots unreadable (%lu damaged, %lu not readable by the flash)\n", static_cast<unsigned>(held),
                  static_cast<unsigned>(commandRelay->unreadableSlots()), static_cast<unsigned long>(commandLog.damaged()),
                  static_cast<unsigned long>(commandLog.failedReads()));
  } else {
    Serial.println("Link: off, the hub does not take frames");
  }
  // The flash of the settings, the nodes and the command log shares one NVS partition of 20 KB. This line shows how full it is, so a log that grows
  // can be seen before a write fails.
  nvs_stats_t nvsStats;
  if (nvs_get_stats(nullptr, &nvsStats) == ESP_OK) {
    Serial.printf("NVS: %u of %u entries used, %u free, %u namespaces\n", static_cast<unsigned>(nvsStats.used_entries),
                  static_cast<unsigned>(nvsStats.total_entries), static_cast<unsigned>(nvsStats.free_entries),
                  static_cast<unsigned>(nvsStats.namespace_count));
  } else {
    Serial.println("NVS: the statistics could not be read");
  }
  // The heap is read before the radio starts and after it, to see what is left for the backend client. A hub that holds its pool as a static
  // object has the 58 KB in the figure of the static memory and not in the heap.
  Serial.printf("Heap before Wi-Fi: %lu free, largest block %lu\n", static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMaxAllocHeap()));
  wifi.begin(millis());
  Serial.printf("Heap after Wi-Fi start: %lu free, largest block %lu\n", static_cast<unsigned long>(ESP.getFreeHeap()),
                static_cast<unsigned long>(ESP.getMaxAllocHeap()));
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
  serviceRadio(now);
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
      // Without the radio no node can answer, and without the link layer a frame of a node would not be read.
      if (hubLink == nullptr || !radio.active()) {
        Serial.println("Pairing: refused, the ESP-NOW link is not running");
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
      if (hubPairing != nullptr) {
        hubPairing->close();
      }
      lastPairingSeconds = -1;
      wifi.stop();
      if (!portal.start(now)) {
        Serial.println("Portal: not started");
        wifi.begin(now);
        drawHome(true);
      }
      break;
    case wattlink::ButtonEvent::NewCode:
      // The hold of 10 seconds asks the backend client for a new claim code. The client is not started by this program yet.
      Serial.println("Button: new claim code asked, the backend client is not started yet");
      break;
    case wattlink::ButtonEvent::None:
      break;
  }

  delay(10);
}
