#pragma once

#include <stddef.h>
#include <stdint.h>

namespace wattlink {

// The setup code is the WPA2 passphrase of the setup network. It has 12 characters from an alphabet of 31 symbols, which is about
// 59 bits. The alphabet leaves out 0, O, 1, I and L so that the code is easy to read from the display.
constexpr size_t kSetupCodeLength = 12;
constexpr size_t kSetupCodeSize = kSetupCodeLength + 1;
constexpr size_t kSetupCodeAlphabetSize = 31;
extern const char kSetupCodeAlphabet[kSetupCodeAlphabetSize + 1];

// The display shows the code in groups of 4 with a minus sign between them, for example ABCD-EFGH-JKMN.
constexpr size_t kSetupCodeShownSize = kSetupCodeLength + 2 + 1;

constexpr size_t kApNameSize = 14;
constexpr unsigned kMaxRejectedDraws = 64;

// WPA2 takes a passphrase of 8 to 63 characters.
static_assert(kSetupCodeLength >= 8 && kSetupCodeLength <= 63, "the setup code is the WPA2 passphrase");

// A source of random 32 bit numbers. On the chip it is the hardware generator, and in the tests it is a script.
using RandomU32 = uint32_t (*)(void *context);

// Writes kSetupCodeLength characters and a terminating zero. A number at or above the largest multiple of the alphabet size that fits in
// 32 bits is drawn again, so every symbol is equally likely. The code is new at every call and is never derived from the MAC address.
// Returns false, with an empty string, when the source gives kMaxRejectedDraws numbers in a row that must be drawn again, so a stuck
// source cannot hang the hub. The caller must then not start the setup network.
// On the chip the source is esp_random(), which is only unpredictable while the radio is on. Call this after the Wi-Fi mode is set.
bool makeSetupCode(RandomU32 random, void *context, char out[kSetupCodeSize]);

// Writes the code for the display, with a minus sign after every 4 characters. The person types the code without the minus signs.
// A longer text is cut after kSetupCodeLength characters, and a missing text gives an empty string.
void formatSetupCode(const char *code, char out[kSetupCodeShownSize]);

// The name of the setup network is WATTLINK- and the last two bytes of the MAC address in upper case hex, such as WATTLINK-A1B2.
void makeApName(const uint8_t mac[6], char out[kApNameSize]);

}  // namespace wattlink
