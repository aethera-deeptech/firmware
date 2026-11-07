#pragma once

#include <stddef.h>
#include <stdint.h>

namespace wattlink {

constexpr size_t kSetupCodeDigits = 8;
constexpr size_t kApNameSize = 14;

// A source of random 32 bit numbers. On the chip it is the hardware generator, and in the tests it is a script.
using RandomU32 = uint32_t (*)(void *context);

// Writes kSetupCodeDigits digits and a terminating zero. A number at or above the largest multiple of 10 that fits in 32 bits is
// drawn again, so every digit is equally likely. The code is new at every call and is never derived from the MAC address.
void makeSetupCode(RandomU32 random, void *context, char out[kSetupCodeDigits + 1]);

// The name of the setup network is WATTLINK- and the last two bytes of the MAC address in upper case hex, such as WATTLINK-A1B2.
void makeApName(const uint8_t mac[6], char out[kApNameSize]);

}  // namespace wattlink
