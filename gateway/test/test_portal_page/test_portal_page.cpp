#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "portal-page.h"
#include "portal-strings.h"

using namespace wattlink;

namespace {

// The buffer of the chip is a static array of this size in the portal module.
constexpr size_t kPageBuffer = 3072;

void assertContains(const char *text, const char *part) {
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(text, part), part);
}

void assertMissing(const char *text, const char *part) {
  TEST_ASSERT_NULL_MESSAGE(strstr(text, part), part);
}

size_t render(const PageModel &model, char *out) {
  return renderSetupPage(model, out, kPageBuffer);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_a_valid_form_is_saved() {
  const SaveCheck check = decideSave("Warung", "password123", nullptr);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SaveDecision::Save), static_cast<int>(check.decision));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FormResult::Ok), static_cast<int>(check.form));
}

void test_an_invalid_form_is_refused_with_the_reason() {
  SaveCheck check = decideSave("", "password123", nullptr);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SaveDecision::Invalid), static_cast<int>(check.decision));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FormResult::SsidEmpty), static_cast<int>(check.form));
  check = decideSave("Warung", "short", nullptr);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(FormResult::PasswordTooShort), static_cast<int>(check.form));
  check = decideSave(nullptr, nullptr, "x");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SaveDecision::Invalid), static_cast<int>(check.decision));
}

void test_an_open_network_asks_once_and_saves_after_the_confirmation() {
  SaveCheck check = decideSave("WifiTamu", "", nullptr);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SaveDecision::AskOpenConfirm), static_cast<int>(check.decision));
  check = decideSave("WifiTamu", nullptr, "");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SaveDecision::AskOpenConfirm), static_cast<int>(check.decision));
  check = decideSave("WifiTamu", "", "WifiTamu");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SaveDecision::Save), static_cast<int>(check.decision));
}

void test_a_confirmation_for_one_name_does_not_carry_over_to_another() {
  SaveCheck check = decideSave("OtherNet", "", "WifiTamu");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SaveDecision::AskOpenConfirm), static_cast<int>(check.decision));
  check = decideSave("wifitamu", "", "WifiTamu");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SaveDecision::AskOpenConfirm), static_cast<int>(check.decision));
  check = decideSave("WifiTamu", "password123", "OtherNet");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SaveDecision::Save), static_cast<int>(check.decision));
}

void test_a_password_of_eight_spaces_is_a_password_and_not_an_open_network() {
  const SaveCheck check = decideSave("Warung", "        ", nullptr);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SaveDecision::Save), static_cast<int>(check.decision));
}

void test_the_confirmation_does_not_excuse_an_invalid_form() {
  const SaveCheck check = decideSave("", "", "");
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SaveDecision::Invalid), static_cast<int>(check.decision));
}

void test_every_form_result_has_a_notice() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Notice::None), static_cast<int>(noticeForForm(FormResult::Ok)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Notice::SsidEmpty), static_cast<int>(noticeForForm(FormResult::SsidEmpty)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Notice::SsidTooLong), static_cast<int>(noticeForForm(FormResult::SsidTooLong)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Notice::PasswordTooShort), static_cast<int>(noticeForForm(FormResult::PasswordTooShort)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Notice::PasswordTooLong), static_cast<int>(noticeForForm(FormResult::PasswordTooLong)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(Notice::BadCharacters), static_cast<int>(noticeForForm(FormResult::BadCharacters)));
}

void test_html_escaping_covers_the_five_characters() {
  char out[64];
  TEST_ASSERT_TRUE(escapeHtml("<a href=\"x\">'&'</a>", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("&lt;a href=&quot;x&quot;&gt;&#39;&amp;&#39;&lt;/a&gt;", out);
}

void test_html_escaping_keeps_other_text_and_handles_empty_and_null() {
  char out[32];
  TEST_ASSERT_TRUE(escapeHtml("Kedai Kopi \xC3\xA9 123", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("Kedai Kopi \xC3\xA9 123", out);
  TEST_ASSERT_TRUE(escapeHtml("", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_TRUE(escapeHtml(nullptr, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);
}

void test_html_escaping_fits_exactly_and_refuses_one_byte_less() {
  char out[8];
  TEST_ASSERT_TRUE(escapeHtml("a&b", out, 8));
  TEST_ASSERT_EQUAL_STRING("a&amp;b", out);
  memset(out, 'x', sizeof(out));
  TEST_ASSERT_FALSE(escapeHtml("a&b", out, 7));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_FALSE(escapeHtml("a", out, 0));
}

void test_the_setup_page_is_a_complete_form_in_indonesian() {
  char html[kPageBuffer];
  const PageModel model = {Notice::None, nullptr, false};
  const size_t length = render(model, html);
  TEST_ASSERT_TRUE(length > 0);
  TEST_ASSERT_EQUAL_UINT(strlen(html), length);
  assertContains(html, "<!doctype html><html lang=\"id\">");
  assertContains(html, "<meta charset=\"utf-8\">");
  assertContains(html, "name=\"viewport\"");
  assertContains(html, "<title>Pengaturan WATTLINK Hub</title>");
  assertContains(html, "<form method=\"post\" action=\"/save\"");
  assertContains(html, "name=\"ssid\"");
  assertContains(html, "name=\"password\" type=\"password\" maxlength=\"63\"");
  assertContains(html, "id=\"ssid\" name=\"ssid\" maxlength=\"32\"");
  assertContains(html, "spellcheck=\"false\"");
  assertContains(html, "aria-describedby=\"pwhint\"><small id=\"pwhint\">");
  assertContains(html, "<meta name=\"color-scheme\" content=\"light\">");
  assertContains(html, "<button type=\"submit\">Simpan</button>");
  assertContains(html, "</html>");
  assertMissing(html, "name=\"open_ok\"");
  assertMissing(html, "class=\"n\"");
}

void test_the_page_has_no_script_and_no_outside_file() {
  char html[kPageBuffer];
  const PageModel model = {Notice::ConfirmOpen, "Warung", true};
  render(model, html);
  assertMissing(html, "<script");
  assertMissing(html, "http");
  assertMissing(html, "src=");
  assertMissing(html, "<link");
}

void test_the_field_limits_of_the_page_come_from_the_checks() {
  char html[kPageBuffer];
  const PageModel model = {Notice::None, nullptr, false};
  render(model, html);
  char expected[64];
  snprintf(expected, sizeof(expected), "id=\"ssid\" name=\"ssid\" maxlength=\"%u\"", static_cast<unsigned>(kMaxSsidLength));
  assertContains(html, expected);
  snprintf(expected, sizeof(expected), "type=\"password\" maxlength=\"%u\"", static_cast<unsigned>(kMaxPasswordLength));
  assertContains(html, expected);
  TEST_ASSERT_EQUAL_STRING("ssid", kFieldSsid);
  TEST_ASSERT_EQUAL_STRING("password", kFieldPassword);
  TEST_ASSERT_EQUAL_STRING("open_ok", kFieldOpenConfirmed);
  TEST_ASSERT_EQUAL_STRING("/save", kSavePath);
}

void test_each_notice_is_shown_as_an_alert() {
  const struct {
    Notice notice;
    const char *text;
  } cases[] = {
      {Notice::SsidEmpty, page::kSsidEmpty},
      {Notice::SsidTooLong, page::kSsidTooLong},
      {Notice::PasswordTooShort, page::kPasswordTooShort},
      {Notice::PasswordTooLong, page::kPasswordTooLong},
      {Notice::BadCharacters, page::kBadCharacters},
      {Notice::ConfirmOpen, page::kConfirmOpen},
      {Notice::SaveFailed, page::kSaveFailed},
      {Notice::Expired, page::kExpired},
  };
  for (const auto &c : cases) {
    char html[kPageBuffer];
    const PageModel model = {c.notice, nullptr, false};
    TEST_ASSERT_TRUE(render(model, html) > 0);
    assertContains(html, "<p class=\"n\" role=\"alert\">");
    assertContains(html, c.text);
  }
}

void test_the_open_network_confirmation_holds_the_confirmed_name() {
  char html[kPageBuffer];
  const PageModel model = {Notice::ConfirmOpen, "Wifi\"Tamu", true};
  render(model, html);
  assertContains(html, "<input type=\"hidden\" name=\"open_ok\" value=\"Wifi&quot;Tamu\">");
}

void test_the_name_is_shown_again_and_escaped() {
  char html[kPageBuffer];
  const PageModel model = {Notice::PasswordTooShort, "<script>alert(1)</script>", false};
  render(model, html);
  assertContains(html, "value=\"&lt;script&gt;alert(1)&lt;/script&gt;\"");
  assertMissing(html, "<script>");
}

void test_a_name_cannot_break_out_of_the_attribute() {
  char html[kPageBuffer];
  const PageModel model = {Notice::None, "x\" onfocus=\"alert(1)", false};
  render(model, html);
  assertContains(html, "value=\"x&quot; onfocus=&quot;alert(1)\"");
  assertMissing(html, "value=\"x\"");
}

void test_a_name_of_exactly_32_bytes_and_a_utf8_name_are_shown() {
  char html[kPageBuffer];
  PageModel model = {Notice::None, "12345678901234567890123456789012", false};
  render(model, html);
  assertContains(html, "value=\"12345678901234567890123456789012\"");
  model.ssid = "Kedai Kopi \xC3\xA9";
  render(model, html);
  assertContains(html, "value=\"Kedai Kopi \xC3\xA9\"");
}

void test_the_password_field_never_has_a_value() {
  char html[kPageBuffer];
  const PageModel model = {Notice::PasswordTooShort, "password123", true};
  render(model, html);
  const char *field = strstr(html, "name=\"password\"");
  TEST_ASSERT_NOT_NULL(field);
  const char *end = strchr(field, '>');
  TEST_ASSERT_NOT_NULL(end);
  const size_t fieldLength = static_cast<size_t>(end - field);
  char tag[256];
  TEST_ASSERT_TRUE(fieldLength < sizeof(tag));
  memcpy(tag, field, fieldLength);
  tag[fieldLength] = '\0';
  assertMissing(tag, "value=");
}

void test_a_name_that_is_too_long_is_not_shown_again() {
  char html[kPageBuffer];
  const PageModel model = {Notice::SsidTooLong, "123456789012345678901234567890123", false};
  TEST_ASSERT_TRUE(render(model, html) > 0);
  assertMissing(html, "123456789012345678901234567890123");
  assertContains(html, "required value=\"\">");
}

void test_the_page_says_that_only_2_4_ghz_wifi_works() {
  char html[kPageBuffer];
  const PageModel model = {Notice::None, nullptr, false};
  TEST_ASSERT_TRUE(render(model, html) > 0);
  assertContains(html, "<p>Hub hanya bisa memakai Wi-Fi 2,4 GHz. Wi-Fi 5 GHz tidak didukung.</p>");
  // The hint comes right after the intro and before the form.
  TEST_ASSERT_TRUE(strstr(html, "Wi-Fi 5 GHz tidak didukung") < strstr(html, "<form"));
}

void test_the_longest_page_fits_the_buffer_of_the_chip() {
  char html[kPageBuffer];
  const PageModel model = {Notice::ConfirmOpen, "\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"", true};
  const size_t length = render(model, html);
  TEST_ASSERT_TRUE(length > 0);
  TEST_ASSERT_TRUE(length < 2560);
}

void test_a_small_buffer_gives_an_empty_page_and_never_overflows() {
  char big[kPageBuffer];
  const PageModel model = {Notice::ConfirmOpen, "Warung", true};
  const size_t length = render(model, big);
  TEST_ASSERT_TRUE(length > 0);

  char exact[kPageBuffer];
  memset(exact, 'x', sizeof(exact));
  TEST_ASSERT_EQUAL_UINT(length, renderSetupPage(model, exact, length + 1));
  TEST_ASSERT_EQUAL_STRING(big, exact);

  memset(exact, 'x', sizeof(exact));
  exact[length] = 0x5A;
  TEST_ASSERT_EQUAL_UINT(0, renderSetupPage(model, exact, length));
  TEST_ASSERT_EQUAL_STRING("", exact);
  TEST_ASSERT_EQUAL_UINT8(0x5A, static_cast<uint8_t>(exact[length]));

  TEST_ASSERT_EQUAL_UINT(0, renderSetupPage(model, exact, 0));
  TEST_ASSERT_EQUAL_UINT(0, renderSetupPage(model, exact, 1));
  TEST_ASSERT_EQUAL_STRING("", exact);
}

void test_the_saved_page_has_no_form() {
  char html[kPageBuffer];
  const size_t length = renderSavedPage(html, sizeof(html));
  TEST_ASSERT_TRUE(length > 0);
  assertContains(html, "<html lang=\"id\">");
  assertContains(html, "<h1>Tersimpan</h1>");
  assertContains(html, page::kSavedBody);
  assertMissing(html, "<form");
  assertMissing(html, "<input");
  char small[16];
  TEST_ASSERT_EQUAL_UINT(0, renderSavedPage(small, sizeof(small)));
  TEST_ASSERT_EQUAL_STRING("", small);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_a_valid_form_is_saved);
  RUN_TEST(test_an_invalid_form_is_refused_with_the_reason);
  RUN_TEST(test_an_open_network_asks_once_and_saves_after_the_confirmation);
  RUN_TEST(test_a_confirmation_for_one_name_does_not_carry_over_to_another);
  RUN_TEST(test_a_password_of_eight_spaces_is_a_password_and_not_an_open_network);
  RUN_TEST(test_the_confirmation_does_not_excuse_an_invalid_form);
  RUN_TEST(test_every_form_result_has_a_notice);
  RUN_TEST(test_html_escaping_covers_the_five_characters);
  RUN_TEST(test_html_escaping_keeps_other_text_and_handles_empty_and_null);
  RUN_TEST(test_html_escaping_fits_exactly_and_refuses_one_byte_less);
  RUN_TEST(test_the_setup_page_is_a_complete_form_in_indonesian);
  RUN_TEST(test_the_page_has_no_script_and_no_outside_file);
  RUN_TEST(test_the_field_limits_of_the_page_come_from_the_checks);
  RUN_TEST(test_each_notice_is_shown_as_an_alert);
  RUN_TEST(test_the_open_network_confirmation_holds_the_confirmed_name);
  RUN_TEST(test_the_name_is_shown_again_and_escaped);
  RUN_TEST(test_a_name_cannot_break_out_of_the_attribute);
  RUN_TEST(test_a_name_of_exactly_32_bytes_and_a_utf8_name_are_shown);
  RUN_TEST(test_the_password_field_never_has_a_value);
  RUN_TEST(test_a_name_that_is_too_long_is_not_shown_again);
  RUN_TEST(test_the_page_says_that_only_2_4_ghz_wifi_works);
  RUN_TEST(test_the_longest_page_fits_the_buffer_of_the_chip);
  RUN_TEST(test_a_small_buffer_gives_an_empty_page_and_never_overflows);
  RUN_TEST(test_the_saved_page_has_no_form);
  return UNITY_END();
}
