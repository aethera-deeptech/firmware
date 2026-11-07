#pragma once

#include <DNSServer.h>
#include <WebServer.h>
#include <stdint.h>

#include "oled.h"
#include "portal-page.h"
#include "settings-store.h"
#include "setup-session.h"

namespace wattlink {

// The captive portal. The hub opens a WPA2 network named WATTLINK-XXXX with a new setup code as its password, shows the name and the code
// on the display, answers every DNS name with its own address, and serves the setup page. The owner types the Wi-Fi of the shop, and the
// settings are saved. The network closes when they are saved or after kSetupSessionMs.
//
// The code is never printed to a log and is never kept after the network is up. It exists on the display and in the Wi-Fi stack only.
// Everything runs in the loop task, so the class has no locks. Call update() on every pass of loop() while isRunning() is true.
class Portal {
 public:
  Portal(SettingsStore &settings, Oled &oled);

  // Opens the setup network. Returns false, with everything closed, when a session already runs, when the display is not ready (the
  // code could not be shown), when the random source is stuck or when the radio, the DNS server or the web server fails to start.
  bool start(uint32_t nowMs);

  // Serves the phones and closes the network when the session ends. Returns the state of the session.
  SessionState update(uint32_t nowMs);

  bool isRunning() const { return running_; }
  SessionState state() const { return session_.state(); }

 private:
  void stop();
  void sendPage(int status, const PageModel &model);
  void sendExpired();
  void sendSaved();
  void sendRedirect();
  void handleRoot();
  void handleSave();

  SettingsStore &settings_;
  Oled &oled_;
  WebServer server_;
  DNSServer dns_;
  SetupSession session_;
  bool running_ = false;
  bool closing_ = false;
  uint32_t closeAtMs_ = 0;
};

}  // namespace wattlink
