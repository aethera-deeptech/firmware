#pragma once

#include <stdint.h>

namespace config {

constexpr const char *kFirmwareVersion = "0.1.0";

// GPIO 0 is the BOOT button of the DevKit V1. The chip reads it at reset to choose its boot mode, so the firmware reads it
// only after setup() and treats a press at start up as no request. A later external button is one line to change here.
constexpr uint8_t kHubButtonPin = 0;

// The backend of WATTLINK. The hub speaks HTTPS to it on port 443, and checks its certificate against the roots in certs/.
constexpr const char *kBackendHost = "aethera-api.syafahadyan.com";
constexpr uint16_t kBackendPort = 443;
// The time servers of the hub. The chip needs the Unix time to check a certificate and to stamp a batch.
constexpr const char *kNtpServerA = "pool.ntp.org";
constexpr const char *kNtpServerB = "time.cloudflare.com";

constexpr uint8_t kOledSdaPin = 21;
constexpr uint8_t kOledSclPin = 22;
constexpr uint8_t kOledAddress = 0x3C;

}  // namespace config
