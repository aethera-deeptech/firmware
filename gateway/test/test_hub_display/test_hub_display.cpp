#include <string.h>
#include <unity.h>

#include "screen-text.h"
#include "setup-code.h"

using namespace wattlink;

namespace {

// Gives the numbers of a script one after the other and counts the calls.
struct Script {
  const uint32_t *values;
  size_t length;
  size_t next;
};

uint32_t scripted(void *context) {
  Script *script = static_cast<Script *>(context);
  const uint32_t value = script->values[script->next % script->length];
  script->next++;
  return value;
}

void assertLinesFit(const Screen &screen) {
  for (uint8_t i = 0; i < screen.count; i++) {
    TEST_ASSERT_LESS_OR_EQUAL_UINT(kLineWidth, strlen(screen.line[i]));
  }
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_the_setup_code_has_twelve_symbols_of_the_alphabet() {
  const uint32_t values[] = {5, 17, 400, 99, 7, 1000, 31, 62, 12345, 54321, 3, 4294967291u};
  Script script = {values, 12, 0};
  char code[kSetupCodeSize];
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, code));
  TEST_ASSERT_EQUAL_UINT(kSetupCodeLength, strlen(code));
  for (size_t i = 0; i < kSetupCodeLength; i++) {
    TEST_ASSERT_NOT_NULL(strchr(kSetupCodeAlphabet, code[i]));
  }
}

void test_the_alphabet_has_31_different_symbols_without_the_look_alikes() {
  TEST_ASSERT_EQUAL_UINT(kSetupCodeAlphabetSize, strlen(kSetupCodeAlphabet));
  for (size_t i = 0; i < kSetupCodeAlphabetSize; i++) {
    TEST_ASSERT_NULL(strchr(kSetupCodeAlphabet + i + 1, kSetupCodeAlphabet[i]));
  }
  TEST_ASSERT_NULL(strpbrk(kSetupCodeAlphabet, "0O1IL"));
  TEST_ASSERT_NULL(strpbrk(kSetupCodeAlphabet, "abcdefghijklmnopqrstuvwxyz-"));
}

void test_each_symbol_is_picked_by_the_remainder_of_the_number() {
  const uint32_t values[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  Script script = {values, 12, 0};
  char code[kSetupCodeSize];
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, code));
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHJKMN", code);
}

void test_the_remainder_wraps_at_the_alphabet_size() {
  const uint32_t values[] = {31, 61, 62, 93, 30, 0, 1, 2, 3, 4, 5, 6};
  Script script = {values, 12, 0};
  char code[kSetupCodeSize];
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, code));
  TEST_ASSERT_EQUAL_STRING("A9AA9ABCDEFG", code);
}

void test_a_number_at_the_limit_is_drawn_again() {
  // 4294967292 is the largest multiple of 31 that fits in 32 bits, so it and everything above it must be skipped.
  const uint32_t values[] = {4294967292u, 4294967295u, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  Script script = {values, 14, 0};
  char code[kSetupCodeSize];
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, code));
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHJKMN", code);
  TEST_ASSERT_EQUAL_UINT(14, script.next);
}

void test_the_largest_accepted_number_gives_the_last_symbol() {
  const uint32_t values[] = {4294967291u};
  Script script = {values, 1, 0};
  char code[kSetupCodeSize];
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, code));
  TEST_ASSERT_EQUAL_STRING("999999999999", code);
  TEST_ASSERT_EQUAL_UINT(kSetupCodeLength, script.next);
}

void test_two_calls_with_one_source_give_two_codes() {
  uint32_t values[24];
  for (uint32_t i = 0; i < 24; i++) {
    values[i] = i * 7;
  }
  Script script = {values, 24, 0};
  char first[kSetupCodeSize];
  char second[kSetupCodeSize];
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, first));
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, second));
  TEST_ASSERT_NOT_EQUAL(0, strcmp(first, second));
}

void test_a_stuck_source_fails_and_leaves_an_empty_code() {
  const uint32_t values[] = {4294967295u};
  Script script = {values, 1, 0};
  char code[kSetupCodeSize];
  memset(code, 'x', sizeof(code));
  TEST_ASSERT_FALSE(makeSetupCode(scripted, &script, code));
  TEST_ASSERT_EQUAL_UINT(0, strlen(code));
  TEST_ASSERT_EQUAL_UINT(kMaxRejectedDraws, script.next);
}

void test_the_code_does_not_write_past_its_buffer() {
  const uint32_t values[] = {1, 2, 3, 4, 5, 6, 7, 8};
  Script script = {values, 8, 0};
  char buffer[kSetupCodeSize + 1];
  buffer[kSetupCodeSize] = 0x5A;
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, buffer));
  TEST_ASSERT_EQUAL_UINT8(0x5A, static_cast<uint8_t>(buffer[kSetupCodeSize]));
}

void test_the_code_is_shown_in_groups_of_four() {
  char shown[kSetupCodeShownSize];
  formatSetupCode("ABCDEFGHJKMN", shown);
  TEST_ASSERT_EQUAL_STRING("ABCD-EFGH-JKMN", shown);
  TEST_ASSERT_EQUAL_UINT(14, strlen(shown));
}

void test_the_shown_code_handles_short_long_and_missing_text() {
  char shown[kSetupCodeShownSize];
  formatSetupCode("ABCDEF", shown);
  TEST_ASSERT_EQUAL_STRING("ABCD-EF", shown);
  formatSetupCode("ABCDEFGH", shown);
  TEST_ASSERT_EQUAL_STRING("ABCD-EFGH", shown);
  formatSetupCode("ABCD", shown);
  TEST_ASSERT_EQUAL_STRING("ABCD", shown);
  formatSetupCode("", shown);
  TEST_ASSERT_EQUAL_STRING("", shown);
  formatSetupCode(nullptr, shown);
  TEST_ASSERT_EQUAL_STRING("", shown);
  formatSetupCode("ABCDEFGHJKMNPQRSTUV", shown);
  TEST_ASSERT_EQUAL_STRING("ABCD-EFGH-JKMN", shown);
}

void test_the_network_name_uses_the_last_two_bytes_in_upper_case() {
  const uint8_t mac[6] = {0x48, 0xe7, 0x29, 0xb6, 0xe0, 0x84};
  char name[kApNameSize];
  makeApName(mac, name);
  TEST_ASSERT_EQUAL_STRING("WATTLINK-E084", name);
  TEST_ASSERT_EQUAL_UINT(kApNameSize - 1, strlen(name));
}

void test_the_network_name_keeps_leading_zeros() {
  const uint8_t mac[6] = {0, 0, 0, 0, 0x00, 0x0A};
  char name[kApNameSize];
  makeApName(mac, name);
  TEST_ASSERT_EQUAL_STRING("WATTLINK-000A", name);
}

void test_the_boot_screen() {
  const Screen screen = bootScreen("0.1.0");
  TEST_ASSERT_EQUAL_UINT8(3, screen.count);
  TEST_ASSERT_EQUAL_STRING("WATTLINK Hub", screen.line[0]);
  TEST_ASSERT_EQUAL_STRING("v0.1.0", screen.line[1]);
  TEST_ASSERT_EQUAL_STRING("Memulai...", screen.line[2]);
  assertLinesFit(screen);
}

void test_the_portal_screen_shows_the_network_and_the_code() {
  const Screen screen = portalScreen("WATTLINK-E084", "ABCDEFGHJKMN");
  TEST_ASSERT_EQUAL_UINT8(5, screen.count);
  TEST_ASSERT_EQUAL_STRING("MODE SETUP", screen.line[0]);
  TEST_ASSERT_EQUAL_STRING("Wi-Fi: WATTLINK-E084", screen.line[1]);
  TEST_ASSERT_EQUAL_STRING("Kode: ABCD-EFGH-JKMN", screen.line[2]);
  TEST_ASSERT_EQUAL_STRING("Buka 192.168.4.1", screen.line[3]);
  TEST_ASSERT_EQUAL_STRING("Ketik tanpa tanda -", screen.line[4]);
  assertLinesFit(screen);
}

void test_a_long_value_is_cut_to_the_width_of_the_line() {
  const Screen screen = portalScreen("A-NETWORK-NAME-THAT-IS-FAR-TOO-LONG", "123456789012345");
  assertLinesFit(screen);
  TEST_ASSERT_EQUAL_UINT(kLineWidth, strlen(screen.line[1]));
}

void test_the_pairing_screen_counts_down() {
  const Screen screen = pairingScreen(60);
  TEST_ASSERT_EQUAL_UINT8(3, screen.count);
  TEST_ASSERT_EQUAL_STRING("PAIRING 60 dtk", screen.line[0]);
  TEST_ASSERT_EQUAL_STRING("Tahan tombol node", screen.line[1]);
  TEST_ASSERT_EQUAL_STRING("selama 3 detik", screen.line[2]);
  assertLinesFit(screen);
  TEST_ASSERT_EQUAL_STRING("PAIRING 5 dtk", pairingScreen(5).line[0]);
}

void test_the_claim_screen_shows_the_code() {
  const Screen screen = claimScreen("ABCD2345");
  TEST_ASSERT_EQUAL_UINT8(3, screen.count);
  TEST_ASSERT_EQUAL_STRING("Kode klaim:", screen.line[0]);
  TEST_ASSERT_EQUAL_STRING("ABCD2345", screen.line[1]);
  TEST_ASSERT_EQUAL_STRING("Masukkan di aplikasi", screen.line[2]);
  assertLinesFit(screen);
}

void test_the_status_screen_for_each_state() {
  const Screen up = statusScreen(true, true, 3, 1250);
  TEST_ASSERT_EQUAL_UINT8(4, up.count);
  TEST_ASSERT_EQUAL_STRING("Wi-Fi: terhubung", up.line[0]);
  TEST_ASSERT_EQUAL_STRING("Server: terhubung", up.line[1]);
  TEST_ASSERT_EQUAL_STRING("Node: 3", up.line[2]);
  TEST_ASSERT_EQUAL_STRING("Daya: 1250 W", up.line[3]);

  const Screen down = statusScreen(false, false, 0, 0);
  TEST_ASSERT_EQUAL_STRING("Wi-Fi: putus", down.line[0]);
  TEST_ASSERT_EQUAL_STRING("Server: putus", down.line[1]);
  TEST_ASSERT_EQUAL_STRING("Node: 0", down.line[2]);
  TEST_ASSERT_EQUAL_STRING("Daya: 0 W", down.line[3]);
  assertLinesFit(up);
  assertLinesFit(down);
}

void test_the_status_screen_fits_with_the_largest_numbers() {
  const Screen screen = statusScreen(true, true, 4294967295u, 4294967295u);
  assertLinesFit(screen);
  TEST_ASSERT_EQUAL_STRING("Daya: 4294967295 W", screen.line[3]);
}

void test_no_screen_has_more_lines_than_the_display_can_show() {
  TEST_ASSERT_TRUE(kMaxScreenLines <= 8);
  TEST_ASSERT_TRUE(bootScreen("0.1.0").count <= kMaxScreenLines);
  TEST_ASSERT_TRUE(portalScreen("WATTLINK-E084", "ABCDEFGHJKMN").count <= kMaxScreenLines);
  TEST_ASSERT_TRUE(statusScreen(true, true, 1, 1).count <= kMaxScreenLines);
}

void test_the_first_four_bytes_of_the_mac_do_not_change_the_network_name() {
  const uint8_t a[6] = {1, 2, 3, 4, 0xAB, 0xCD};
  const uint8_t b[6] = {9, 8, 7, 6, 0xAB, 0xCD};
  char nameA[kApNameSize];
  char nameB[kApNameSize];
  makeApName(a, nameA);
  makeApName(b, nameB);
  TEST_ASSERT_EQUAL_STRING("WATTLINK-ABCD", nameA);
  TEST_ASSERT_EQUAL_STRING(nameA, nameB);
}

void test_the_pairing_countdown_is_capped_to_keep_the_line_width() {
  const Screen screen = pairingScreen(4294967295u);
  TEST_ASSERT_EQUAL_STRING("PAIRING 999 dtk", screen.line[0]);
  assertLinesFit(screen);
}

void test_a_missing_text_shows_as_empty_and_a_long_version_is_cut() {
  TEST_ASSERT_EQUAL_STRING("Kode: ", portalScreen(nullptr, nullptr).line[2]);
  TEST_ASSERT_EQUAL_STRING("Wi-Fi: ", portalScreen(nullptr, nullptr).line[1]);
  TEST_ASSERT_EQUAL_STRING("", claimScreen(nullptr).line[1]);
  TEST_ASSERT_EQUAL_STRING("v", bootScreen(nullptr).line[1]);
  const Screen longVersion = bootScreen("1.2.3-a-very-long-pre-release-name");
  assertLinesFit(longVersion);
  TEST_ASSERT_EQUAL_UINT(kLineWidth, strlen(longVersion.line[1]));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_the_first_four_bytes_of_the_mac_do_not_change_the_network_name);
  RUN_TEST(test_the_pairing_countdown_is_capped_to_keep_the_line_width);
  RUN_TEST(test_a_missing_text_shows_as_empty_and_a_long_version_is_cut);
  RUN_TEST(test_the_setup_code_has_twelve_symbols_of_the_alphabet);
  RUN_TEST(test_the_alphabet_has_31_different_symbols_without_the_look_alikes);
  RUN_TEST(test_each_symbol_is_picked_by_the_remainder_of_the_number);
  RUN_TEST(test_the_remainder_wraps_at_the_alphabet_size);
  RUN_TEST(test_a_number_at_the_limit_is_drawn_again);
  RUN_TEST(test_the_largest_accepted_number_gives_the_last_symbol);
  RUN_TEST(test_two_calls_with_one_source_give_two_codes);
  RUN_TEST(test_a_stuck_source_fails_and_leaves_an_empty_code);
  RUN_TEST(test_the_code_does_not_write_past_its_buffer);
  RUN_TEST(test_the_code_is_shown_in_groups_of_four);
  RUN_TEST(test_the_shown_code_handles_short_long_and_missing_text);
  RUN_TEST(test_the_network_name_uses_the_last_two_bytes_in_upper_case);
  RUN_TEST(test_the_network_name_keeps_leading_zeros);
  RUN_TEST(test_the_boot_screen);
  RUN_TEST(test_the_portal_screen_shows_the_network_and_the_code);
  RUN_TEST(test_a_long_value_is_cut_to_the_width_of_the_line);
  RUN_TEST(test_the_pairing_screen_counts_down);
  RUN_TEST(test_the_claim_screen_shows_the_code);
  RUN_TEST(test_the_status_screen_for_each_state);
  RUN_TEST(test_the_status_screen_fits_with_the_largest_numbers);
  RUN_TEST(test_no_screen_has_more_lines_than_the_display_can_show);
  return UNITY_END();
}
