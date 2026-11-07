#include "wifi-client.h"

#include <Arduino.h>
#include <WiFi.h>

namespace wattlink {

namespace {

// A plain memset on a buffer that is not read again can be removed by the optimizer, a write through a volatile pointer cannot.
void wipe(void *data, size_t length) {
  volatile uint8_t *bytes = static_cast<volatile uint8_t *>(data);
  while (length-- > 0) {
    *bytes++ = 0;
  }
}

}  // namespace

bool WifiClient::begin(uint32_t nowMs) {
  WifiSettings saved;
  const bool hasSettings = settings_.loadWifi(&saved);
  wipe(&saved, sizeof(saved));
  if (!hasSettings) {
    stop();
    return false;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // The link is watched here, so the stack must not reconnect on its own. The modem sleep is off because the hub is powered from USB and
  // the link to the nodes (ESP-NOW) needs the radio awake to receive.
  WiFi.setAutoReconnect(false);
  WiFi.setSleep(false);
  link_.enable(nowMs);
  Serial.println("WiFi: joining");
  return true;
}

void WifiClient::update(uint32_t nowMs) {
  const bool up = WiFi.status() == WL_CONNECTED;
  const LinkAction action = link_.update(nowMs, up);

  if (up != connected_) {
    connected_ = up;
    Serial.println(up ? "WiFi: connected" : "WiFi: lost");
  }

  if (action == LinkAction::StartConnect) {
    WifiSettings saved;
    if (settings_.loadWifi(&saved)) {
      WiFi.disconnect();
      WiFi.begin(saved.ssid, saved.password[0] != '\0' ? saved.password : nullptr);
    }
    wipe(&saved, sizeof(saved));
  } else if (action == LinkAction::Abort) {
    WiFi.disconnect();
    Serial.println("WiFi: try failed");
  }
}

void WifiClient::stop() {
  link_.disable();
  connected_ = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

}  // namespace wattlink
