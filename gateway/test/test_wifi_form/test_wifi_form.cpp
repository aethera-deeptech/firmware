#include <string.h>
#include <unity.h>

#include "wifi-form.h"

using namespace wattlink;

namespace {

void assertResult(FormResult expected, const char *ssid, const char *password) {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(checkWifiForm(ssid, password)));
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_a_normal_name_and_password_pass() {
  assertResult(FormResult::Ok, "Warung Bu Sri", "kopi-susu-2026");
}

void test_an_open_network_has_an_empty_or_missing_password() {
  assertResult(FormResult::Ok, "WifiTamu", "");
  assertResult(FormResult::Ok, "WifiTamu", nullptr);
}

void test_the_name_must_not_be_empty() {
  assertResult(FormResult::SsidEmpty, "", "password123");
  assertResult(FormResult::SsidEmpty, nullptr, "password123");
}

void test_the_name_may_have_32_bytes_and_not_33() {
  assertResult(FormResult::Ok, "12345678901234567890123456789012", "password123");
  assertResult(FormResult::SsidTooLong, "123456789012345678901234567890123", "password123");
}

void test_a_name_with_no_end_is_refused_without_reading_past_the_buffer() {
  // No terminating zero. A strlen would read past this buffer, and strnlen with a bound of 33 stays inside it.
  char noEnd[kMaxSsidLength + 1];
  memset(noEnd, 'a', sizeof(noEnd));
  assertResult(FormResult::SsidTooLong, noEnd, "password123");
}

void test_the_name_is_counted_in_bytes_and_not_in_letters() {
  // 17 letters of two bytes each are 34 bytes.
  assertResult(FormResult::SsidTooLong, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9", "password123");
}

void test_a_control_character_at_the_last_position_of_the_name_is_found() {
  char name[kMaxSsidLength + 1];
  memset(name, 'a', kMaxSsidLength);
  name[kMaxSsidLength - 1] = '\n';
  name[kMaxSsidLength] = '\0';
  assertResult(FormResult::BadCharacters, name, "password123");
}

void test_the_name_refuses_control_characters() {
  assertResult(FormResult::BadCharacters, "Warung\nBu", "password123");
  assertResult(FormResult::BadCharacters, "Warung\tBu", "password123");
  assertResult(FormResult::BadCharacters, "Warung\x7F", "password123");
  assertResult(FormResult::BadCharacters, "\x01", "password123");
}

void test_the_name_may_hold_letters_outside_ascii() {
  assertResult(FormResult::Ok, "Kedai Kopi \xC3\xA9", "password123");
}

void test_the_password_needs_8_to_63_characters() {
  assertResult(FormResult::PasswordTooShort, "Warung", "1234567");
  assertResult(FormResult::Ok, "Warung", "12345678");
  char sixtyThree[64];
  memset(sixtyThree, 'p', 63);
  sixtyThree[63] = '\0';
  assertResult(FormResult::Ok, "Warung", sixtyThree);
  char sixtyFour[65];
  memset(sixtyFour, 'p', 64);
  sixtyFour[64] = '\0';
  assertResult(FormResult::PasswordTooLong, "Warung", sixtyFour);
}

void test_a_password_with_no_end_is_refused_without_reading_past_the_buffer() {
  char noEnd[kMaxPasswordLength + 1];
  memset(noEnd, 'p', sizeof(noEnd));
  assertResult(FormResult::PasswordTooLong, "Warung", noEnd);
}

void test_the_length_of_the_password_is_checked_before_its_characters() {
  char sixtyFour[kMaxPasswordLength + 2];
  memset(sixtyFour, 'p', kMaxPasswordLength + 1);
  sixtyFour[10] = '\n';
  sixtyFour[kMaxPasswordLength + 1] = '\0';
  assertResult(FormResult::PasswordTooLong, "Warung", sixtyFour);
}

void test_a_key_of_64_hex_digits_is_refused_because_the_form_takes_a_passphrase() {
  assertResult(FormResult::PasswordTooLong, "Warung", "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
}

void test_the_password_allows_spaces_and_symbols_and_refuses_the_rest() {
  assertResult(FormResult::Ok, "Warung", "pass word! #1");
  assertResult(FormResult::Ok, "Warung", "~~~~~~~~");
  assertResult(FormResult::BadCharacters, "Warung", "pass\nword12");
  assertResult(FormResult::BadCharacters, "Warung", "kata\x7Fsandi1");
  assertResult(FormResult::BadCharacters, "Warung", "sandi-\xC3\xA9-panjang");
}

void test_the_name_is_checked_before_the_password() {
  assertResult(FormResult::SsidEmpty, "", "short");
  assertResult(FormResult::SsidTooLong, "123456789012345678901234567890123", "short");
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_a_normal_name_and_password_pass);
  RUN_TEST(test_an_open_network_has_an_empty_or_missing_password);
  RUN_TEST(test_the_name_must_not_be_empty);
  RUN_TEST(test_the_name_may_have_32_bytes_and_not_33);
  RUN_TEST(test_a_name_with_no_end_is_refused_without_reading_past_the_buffer);
  RUN_TEST(test_the_name_is_counted_in_bytes_and_not_in_letters);
  RUN_TEST(test_a_control_character_at_the_last_position_of_the_name_is_found);
  RUN_TEST(test_the_name_refuses_control_characters);
  RUN_TEST(test_the_name_may_hold_letters_outside_ascii);
  RUN_TEST(test_the_password_needs_8_to_63_characters);
  RUN_TEST(test_a_password_with_no_end_is_refused_without_reading_past_the_buffer);
  RUN_TEST(test_the_length_of_the_password_is_checked_before_its_characters);
  RUN_TEST(test_a_key_of_64_hex_digits_is_refused_because_the_form_takes_a_passphrase);
  RUN_TEST(test_the_password_allows_spaces_and_symbols_and_refuses_the_rest);
  RUN_TEST(test_the_name_is_checked_before_the_password);
  return UNITY_END();
}
