#pragma once

#include <stdint.h>

#include "link-supervisor.h"
#include "settings-store.h"

namespace wattlink {

// Something that needs the radio to be in station mode, such as ESP-NOW. The client tells it when the radio goes on and before it goes off.
class RadioListener {
 public:
  virtual ~RadioListener() = default;
  // Called after the radio is switched to station mode. It may be called again while the radio is on, so it must be safe to repeat.
  virtual void onRadioOn() = 0;
  // Called before the radio is switched off. It may be called when the radio is already off.
  virtual void onRadioOff() = 0;
};

// Joins the Wi-Fi of the shop with the saved settings, watches the link, and joins again with a growing wait when it drops. The settings
// are read from the store at the start of every try and are not kept, so the password lives in RAM only for a moment.
// Nothing here prints a name or a password. Everything runs in the loop task.
class WifiClient {
 public:
  explicit WifiClient(SettingsStore &settings, RadioListener *listener = nullptr) : settings_(settings), listener_(listener) {}

  // Switches the radio to station mode and starts to join. Returns false, with the radio off, when no Wi-Fi is saved.
  bool begin(uint32_t nowMs);

  // Call on every pass of loop().
  void update(uint32_t nowMs);

  // Drops the link and switches the radio off. The portal needs the radio for its own network.
  void stop();

  bool connected() const { return connected_; }
  LinkState state() const { return link_.state(); }

 private:
  SettingsStore &settings_;
  RadioListener *listener_;
  LinkSupervisor link_;
  bool connected_ = false;
};

}  // namespace wattlink
