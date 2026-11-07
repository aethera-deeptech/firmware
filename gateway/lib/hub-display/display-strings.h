#pragma once

#include <stddef.h>

namespace wattlink {

// The display fits 21 characters in each line, and Indonesian runs longer than English, so every line is short.
// All the texts of the display are in this file, so the language can be switched here and nowhere else.
constexpr size_t kLineWidth = 21;

constexpr size_t constLength(const char *text) {
  return *text == '\0' ? 0 : 1 + constLength(text + 1);
}

namespace text {

constexpr const char *kBootTitle = "WATTLINK Hub";
constexpr const char *kBootStarting = "Memulai...";

constexpr const char *kPortalTitle = "MODE SETUP";
constexpr const char *kPortalWifiFormat = "Wi-Fi: %s";
constexpr const char *kPortalCodeFormat = "Kode: %s";
constexpr const char *kPortalOpen = "Buka 192.168.4.1";
constexpr const char *kPortalTypeHint = "Ketik tanpa tanda -";

constexpr const char *kPairingTitleFormat = "PAIRING %u dtk";
constexpr const char *kPairingHintFirst = "Tahan tombol node";
constexpr const char *kPairingHintSecond = "selama 3 detik";

constexpr const char *kClaimTitle = "Kode klaim:";
constexpr const char *kClaimHint = "Masukkan di aplikasi";

constexpr const char *kWifiConnected = "Wi-Fi: terhubung";
constexpr const char *kWifiDown = "Wi-Fi: putus";
constexpr const char *kServerConnected = "Server: terhubung";
constexpr const char *kServerDown = "Server: putus";
constexpr const char *kNodesFormat = "Node: %u";
constexpr const char *kPowerFormat = "Daya: %lu W";

}  // namespace text

// A text without a format code must fit a line as it is. The texts with a format code are checked with their longest values in the tests.
static_assert(constLength(text::kBootTitle) <= kLineWidth, "line too long");
static_assert(constLength(text::kBootStarting) <= kLineWidth, "line too long");
static_assert(constLength(text::kPortalTitle) <= kLineWidth, "line too long");
static_assert(constLength(text::kPortalOpen) <= kLineWidth, "line too long");
static_assert(constLength(text::kPortalTypeHint) <= kLineWidth, "line too long");
static_assert(constLength(text::kPairingHintFirst) <= kLineWidth, "line too long");
static_assert(constLength(text::kPairingHintSecond) <= kLineWidth, "line too long");
static_assert(constLength(text::kClaimTitle) <= kLineWidth, "line too long");
static_assert(constLength(text::kClaimHint) <= kLineWidth, "line too long");
static_assert(constLength(text::kWifiConnected) <= kLineWidth, "line too long");
static_assert(constLength(text::kWifiDown) <= kLineWidth, "line too long");
static_assert(constLength(text::kServerConnected) <= kLineWidth, "line too long");
static_assert(constLength(text::kServerDown) <= kLineWidth, "line too long");

}  // namespace wattlink
