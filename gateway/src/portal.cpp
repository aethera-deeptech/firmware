#include "portal.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <string.h>

#include "portal-page.h"
#include "screen-text.h"
#include "secure-wipe.h"
#include "setup-code.h"
#include "wifi-form.h"

namespace wattlink {

namespace {

// Channel 1 is a plain choice. The setup network runs alone, and the link to the nodes is not up while it runs.
constexpr int kChannel = 1;

// The page that confirms the save has to reach the phone before the network closes.
constexpr uint32_t kCloseDelayMs = 2000;

// The page is built in this buffer, which sits in static memory because the loop task has only 8192 bytes of stack.
constexpr size_t kPageBufferSize = 3072;
char pageBuffer[kPageBufferSize];

uint32_t hardwareRandom(void *) {
  return esp_random();
}

// Phones and laptops ask for these addresses to find out whether the network has internet. Answering with a redirect to the setup page
// makes the system open the page by itself.
const char *const kProbePaths[] = {
    "/generate_204",                // Android
    "/gen_204",                     // Android
    "/hotspot-detect.html",         // Apple
    "/library/test/success.html",   // Apple
    "/connecttest.txt",             // Windows
    "/ncsi.txt",                    // Windows
    "/canonical.html",              // Firefox
    "/success.txt",                 // Firefox
    "/redirect",                    // Windows
};

}  // namespace

Portal::Portal(SettingsStore &settings, Oled &oled) : settings_(settings), oled_(oled), server_(80) {
  server_.on("/", HTTP_GET, [this]() { handleRoot(); });
  server_.on("/", HTTP_HEAD, [this]() { handleRoot(); });
  server_.on(kSavePath, HTTP_POST, [this]() { handleSave(); });
  for (const char *path : kProbePaths) {
    server_.on(path, HTTP_ANY, [this]() { sendRedirect(); });
  }
  server_.onNotFound([this]() { sendRedirect(); });
}

bool Portal::start(uint32_t nowMs) {
  // The code can only be shown on the display, so no display means no setup network.
  if (running_ || !oled_.ready()) {
    return false;
  }
  if (!session_.start(nowMs)) {
    return false;
  }

  // The radio is switched on first, because the hardware random generator is only unpredictable while it runs.
  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_AP)) {
    stop();
    return false;
  }

  char code[kSetupCodeSize];
  // WiFi.softAP makes an OPEN network when the password is empty, so a code of the wrong length must never reach it.
  if (!makeSetupCode(hardwareRandom, nullptr, code) || strlen(code) != kSetupCodeLength) {
    wipe(code, sizeof(code));
    stop();
    return false;
  }
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char apName[kApNameSize];
  makeApName(mac, apName);

  const bool apUp = WiFi.softAP(apName, code, kChannel, 0, kMaxSetupClients);
  Screen screen = portalScreen(apName, code);
  wipe(code, sizeof(code));
  if (apUp) {
    oled_.show(screen);
  }
  wipe(&screen, sizeof(screen));
  if (!apUp) {
    stop();
    return false;
  }

  // A DNS server that answers 0.0.0.0 for every name would leave the portal silently dead.
  const IPAddress apAddress = WiFi.softAPIP();
  dns_.setErrorReplyCode(DNSReplyCode::NoError);
  if (apAddress == IPAddress(0, 0, 0, 0) || !dns_.start(53, "*", apAddress)) {
    stop();
    return false;
  }
  server_.begin();
  running_ = true;
  closing_ = false;
  Serial.println("Portal: open");
  return true;
}

SessionState Portal::update(uint32_t nowMs) {
  if (!running_) {
    return session_.state();
  }
  if (closing_) {
    server_.handleClient();
    if (static_cast<int32_t>(nowMs - closeAtMs_) >= 0) {
      stop();
      Serial.println("Portal: closed");
    }
    return session_.state();
  }
  if (session_.update(nowMs) == SessionState::Expired) {
    stop();
    Serial.println("Portal: expired");
    return SessionState::Expired;
  }
  dns_.processNextRequest();
  server_.handleClient();
  return session_.state();
}

void Portal::stop() {
  server_.stop();
  dns_.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  oled_.clear();
  running_ = false;
  closing_ = false;
  // A session that stops before it ended on its own (a start that failed halfway) goes back to idle.
  if (session_.state() == SessionState::Running) {
    session_.cancel();
  }
}

void Portal::sendPage(int status, const PageModel &model) {
  const size_t length = renderSetupPage(model, pageBuffer, sizeof(pageBuffer));
  server_.sendHeader("Cache-Control", "no-store");
  if (length == 0) {
    server_.send(500, "text/plain; charset=utf-8", "Terjadi kesalahan.");
    return;
  }
  server_.send(status, "text/html; charset=utf-8", pageBuffer);
}

void Portal::sendExpired() {
  const PageModel model = {Notice::Expired, nullptr, false};
  sendPage(403, model);
}

void Portal::sendRedirect() {
  const String url = String("http://") + WiFi.softAPIP().toString() + "/";
  server_.sendHeader("Location", url, true);
  server_.sendHeader("Cache-Control", "no-store");
  server_.send(302, "text/plain", "");
}

void Portal::sendSaved() {
  const size_t length = renderSavedPage(pageBuffer, sizeof(pageBuffer));
  server_.sendHeader("Cache-Control", "no-store");
  if (length == 0) {
    server_.send(200, "text/plain; charset=utf-8", "Tersimpan.");
  } else {
    server_.send(200, "text/html; charset=utf-8", pageBuffer);
  }
}

void Portal::handleRoot() {
  // After the save the network stays up for a moment. A second tap on Save or a reload shows the same confirmation.
  if (closing_) {
    sendSaved();
    return;
  }
  if (!session_.isOpen(millis())) {
    sendExpired();
    return;
  }
  const PageModel model = {Notice::None, nullptr, false};
  sendPage(200, model);
}

void Portal::handleSave() {
  if (closing_) {
    sendSaved();
    return;
  }
  const uint32_t now = millis();
  if (!session_.isOpen(now)) {
    sendExpired();
    return;
  }

  // The values pass through Arduino Strings on the heap, which cannot be wiped. Only a phone that joined the setup network can post.
  const String ssid = server_.arg(kFieldSsid);
  const String password = server_.arg(kFieldPassword);
  const String confirmed = server_.arg(kFieldOpenConfirmed);

  const SaveCheck check = decideSave(ssid.c_str(), password.c_str(), confirmed.c_str());
  PageModel model = {Notice::None, ssid.c_str(), false};
  if (check.decision == SaveDecision::Invalid) {
    model.notice = noticeForForm(check.form);
    sendPage(400, model);
    return;
  }
  if (check.decision == SaveDecision::AskOpenConfirm) {
    model.notice = Notice::ConfirmOpen;
    model.openConfirmed = true;
    sendPage(200, model);
    return;
  }

  if (!settings_.saveWifi(ssid.c_str(), password.c_str())) {
    model.notice = Notice::SaveFailed;
    sendPage(500, model);
    return;
  }
  if (!session_.finish(now)) {
    sendExpired();
    return;
  }

  sendSaved();
  closing_ = true;
  closeAtMs_ = now + kCloseDelayMs;
  Serial.println("Portal: saved");
}

}  // namespace wattlink
