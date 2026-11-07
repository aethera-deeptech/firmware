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

void test_the_setup_code_has_eight_digits_and_a_terminating_zero() {
  const uint32_t values[] = {1234567, 7654321, 42, 99, 5, 0, 10, 19};
  Script script = {values, 8, 0};
  char code[kSetupCodeDigits + 1];
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, code));
  TEST_ASSERT_EQUAL_UINT(kSetupCodeDigits, strlen(code));
  for (size_t i = 0; i < kSetupCodeDigits; i++) {
    TEST_ASSERT_TRUE(code[i] >= '0' && code[i] <= '9');
  }
}

void test_each_digit_is_the_last_digit_of_one_number() {
  const uint32_t values[] = {1234567, 7654321, 42, 99, 5, 0, 10, 19};
  Script script = {values, 8, 0};
  char code[kSetupCodeDigits + 1];
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, code));
  TEST_ASSERT_EQUAL_STRING("71295009", code);
  TEST_ASSERT_EQUAL_UINT(8, script.next);
}

void test_a_number_at_the_limit_is_drawn_again() {
  // 4294967290 is the largest multiple of 10 that fits in 32 bits, so it and everything above it must be skipped.
  const uint32_t values[] = {4294967290u, 4294967295u, 4294967291u, 1, 2, 3, 4, 5, 6, 7, 8};
  Script script = {values, 11, 0};
  char code[kSetupCodeDigits + 1];
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, code));
  TEST_ASSERT_EQUAL_STRING("12345678", code);
  TEST_ASSERT_EQUAL_UINT(11, script.next);
}

void test_the_largest_accepted_number_gives_the_digit_9() {
  const uint32_t values[] = {4294967289u};
  Script script = {values, 1, 0};
  char code[kSetupCodeDigits + 1];
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, code));
  TEST_ASSERT_EQUAL_STRING("99999999", code);
  TEST_ASSERT_EQUAL_UINT(8, script.next);
}

void test_different_random_numbers_give_a_different_code() {
  const uint32_t first[] = {1, 2, 3, 4, 5, 6, 7, 8};
  const uint32_t second[] = {8, 7, 6, 5, 4, 3, 2, 1};
  Script scriptA = {first, 8, 0};
  Script scriptB = {second, 8, 0};
  char a[kSetupCodeDigits + 1];
  char b[kSetupCodeDigits + 1];
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &scriptA, a));
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &scriptB, b));
  TEST_ASSERT_NOT_EQUAL(0, strcmp(a, b));
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
  const Screen screen = portalScreen("WATTLINK-E084", "12345678");
  TEST_ASSERT_EQUAL_UINT8(4, screen.count);
  TEST_ASSERT_EQUAL_STRING("MODE SETUP", screen.line[0]);
  TEST_ASSERT_EQUAL_STRING("Wi-Fi: WATTLINK-E084", screen.line[1]);
  TEST_ASSERT_EQUAL_STRING("Kode: 12345678", screen.line[2]);
  TEST_ASSERT_EQUAL_STRING("Buka 192.168.4.1", screen.line[3]);
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
  TEST_ASSERT_TRUE(statusScreen(true, true, 1, 1).count <= kMaxScreenLines);
}

void test_two_calls_with_one_source_give_two_codes() {
  const uint32_t values[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 9, 8, 7, 6, 5, 4};
  Script script = {values, 16, 0};
  char first[kSetupCodeDigits + 1];
  char second[kSetupCodeDigits + 1];
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, first));
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, second));
  TEST_ASSERT_EQUAL_STRING("12345678", first);
  TEST_ASSERT_EQUAL_STRING("90987654", second);
}

void test_a_stuck_source_fails_and_leaves_an_empty_code() {
  const uint32_t values[] = {4294967295u};
  Script script = {values, 1, 0};
  char code[kSetupCodeDigits + 1];
  memset(code, 'x', sizeof(code));
  TEST_ASSERT_FALSE(makeSetupCode(scripted, &script, code));
  TEST_ASSERT_EQUAL_UINT(0, strlen(code));
  TEST_ASSERT_EQUAL_UINT(kMaxRejectedDraws, script.next);
}

void test_the_code_does_not_write_past_its_buffer() {
  const uint32_t values[] = {1, 2, 3, 4, 5, 6, 7, 8};
  Script script = {values, 8, 0};
  char buffer[kSetupCodeDigits + 2];
  buffer[kSetupCodeDigits + 1] = 0x5A;
  TEST_ASSERT_TRUE(makeSetupCode(scripted, &script, buffer));
  TEST_ASSERT_EQUAL_UINT8(0x5A, static_cast<uint8_t>(buffer[kSetupCodeDigits + 1]));
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
  RUN_TEST(test_the_setup_code_has_eight_digits_and_a_terminating_zero);
  RUN_TEST(test_each_digit_is_the_last_digit_of_one_number);
  RUN_TEST(test_a_number_at_the_limit_is_drawn_again);
  RUN_TEST(test_the_largest_accepted_number_gives_the_digit_9);
  RUN_TEST(test_different_random_numbers_give_a_different_code);
  RUN_TEST(test_two_calls_with_one_source_give_two_codes);
  RUN_TEST(test_a_stuck_source_fails_and_leaves_an_empty_code);
  RUN_TEST(test_the_code_does_not_write_past_its_buffer);
  RUN_TEST(test_the_first_four_bytes_of_the_mac_do_not_change_the_network_name);
  RUN_TEST(test_the_pairing_countdown_is_capped_to_keep_the_line_width);
  RUN_TEST(test_a_missing_text_shows_as_empty_and_a_long_version_is_cut);
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
