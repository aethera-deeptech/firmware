#pragma once

#include <stddef.h>
#include <stdint.h>

namespace wattlink {

constexpr size_t kSetupCodeDigits = 8;
constexpr size_t kApNameSize = 14;
constexpr unsigned kMaxRejectedDraws = 64;

// WPA2 takes a passphrase of 8 to 63 characters.
static_assert(kSetupCodeDigits >= 8 && kSetupCodeDigits <= 63, "the setup code is the WPA2 passphrase");

// A source of random 32 bit numbers. On the chip it is the hardware generator, and in the tests it is a script.
using RandomU32 = uint32_t (*)(void *context);

// Writes kSetupCodeDigits digits and a terminating zero. A number at or above the largest multiple of 10 that fits in 32 bits is
// drawn again, so every digit is equally likely. The code is new at every call and is never derived from the MAC address.
// Returns false, with an empty string, when the source gives kMaxRejectedDraws numbers in a row that must be drawn again, so a stuck
// source cannot hang the hub. The caller must then not start the setup network.
// On the chip the source is esp_random(), which is only unpredictable while the radio is on. Call this after the Wi-Fi mode is set.
bool makeSetupCode(RandomU32 random, void *context, char out[kSetupCodeDigits + 1]);

// The name of the setup network is WATTLINK- and the last two bytes of the MAC address in upper case hex, such as WATTLINK-A1B2.
void makeApName(const uint8_t mac[6], char out[kApNameSize]);

}  // namespace wattlink
