#include "wifi-client.h"

#include <Arduino.h>
#include <WiFi.h>

#include "secure-wipe.h"

namespace wattlink {

namespace {

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
  if (listener_ != nullptr) {
    listener_->onRadioOn();
  }
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
    } else {
      // The settings are gone or damaged, so there is nothing to join. The home screen then asks for the setup.
      Serial.println("WiFi: no settings");
      stop();
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
  // ESP-NOW goes first, because the stack is gone once the radio is off.
  if (listener_ != nullptr) {
    listener_->onRadioOff();
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

}  // namespace wattlink
