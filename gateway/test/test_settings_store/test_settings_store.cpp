#include <map>
#include <string>
#include <string.h>
#include <unity.h>

#include "settings-store.h"

using namespace wattlink;

namespace {

// A store in memory. It can be told to fail on one call, and it counts the writes.
class FakeStore : public KeyValueStore {
 public:
  std::map<std::string, std::string> data;
  std::string failPutOn;
  std::string failRemoveOn;
  int puts = 0;

  bool get(const char *key, char *out, size_t size) override {
    if (size > 0) {
      out[0] = '\0';
    }
    const auto found = data.find(key);
    if (found == data.end() || found->second.size() + 1 > size) {
      return false;
    }
    memcpy(out, found->second.c_str(), found->second.size() + 1);
    return true;
  }

  bool put(const char *key, const char *value) override {
    if (value == nullptr || value[0] == '\0' || failPutOn == key) {
      return false;
    }
    puts++;
    data[key] = value;
    return true;
  }

  bool remove(const char *key) override {
    if (failRemoveOn == key) {
      return false;
    }
    data.erase(key);
    return true;
  }
};

// Gives the numbers of a script one after the other.
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

const uint32_t kBytesZeroToThirtyOne[8] = {0x03020100, 0x07060504, 0x0b0a0908, 0x0f0e0d0c, 0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c};
const char kKeyForZeroToThirtyOne[] = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";

}  // namespace

void setUp() {}
void tearDown() {}

void test_base64url_matches_the_rfc_4648_examples() {
  char out[8];
  TEST_ASSERT_EQUAL_UINT(0, base64UrlEncode(reinterpret_cast<const uint8_t *>(""), 0, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_EQUAL_UINT(2, base64UrlEncode(reinterpret_cast<const uint8_t *>("f"), 1, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("Zg", out);
  TEST_ASSERT_EQUAL_UINT(3, base64UrlEncode(reinterpret_cast<const uint8_t *>("fo"), 2, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("Zm8", out);
  TEST_ASSERT_EQUAL_UINT(4, base64UrlEncode(reinterpret_cast<const uint8_t *>("foo"), 3, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("Zm9v", out);
}

void test_base64url_uses_the_url_alphabet_and_no_padding() {
  const uint8_t data[3] = {0xfb, 0xff, 0xbf};
  char out[8];
  TEST_ASSERT_EQUAL_UINT(4, base64UrlEncode(data, 3, out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("-_-_", out);
  uint8_t ones[32];
  memset(ones, 0xFF, sizeof(ones));
  char longOut[kApiKeySize];
  TEST_ASSERT_EQUAL_UINT(43, base64UrlEncode(ones, sizeof(ones), longOut, sizeof(longOut)));
  TEST_ASSERT_EQUAL_STRING("__________________________________________8", longOut);
}

void test_base64url_refuses_a_small_buffer() {
  const uint8_t data[3] = {1, 2, 3};
  char out[4] = {'x', 'x', 'x', 'x'};
  TEST_ASSERT_EQUAL_UINT(0, base64UrlEncode(data, 3, out, 4));
  TEST_ASSERT_EQUAL_STRING("", out);
  TEST_ASSERT_EQUAL_UINT(4, base64UrlEncode(data, 3, out, 5));
}

void test_wifi_settings_are_saved_and_loaded() {
  FakeStore store;
  SettingsStore settings(store);
  TEST_ASSERT_TRUE(settings.saveWifi("Warung Bu Sri", "kopi-susu-2026"));
  WifiSettings loaded;
  TEST_ASSERT_TRUE(settings.loadWifi(&loaded));
  TEST_ASSERT_EQUAL_STRING("Warung Bu Sri", loaded.ssid);
  TEST_ASSERT_EQUAL_STRING("kopi-susu-2026", loaded.password);
}

void test_an_open_network_stores_no_password_and_removes_an_old_one() {
  FakeStore store;
  SettingsStore settings(store);
  TEST_ASSERT_TRUE(settings.saveWifi("WifiTamu", "oldpassword1"));
  TEST_ASSERT_TRUE(settings.saveWifi("WifiTamu", ""));
  TEST_ASSERT_EQUAL_UINT(0, store.data.count(key::kWifiPassword));
  WifiSettings loaded;
  TEST_ASSERT_TRUE(settings.loadWifi(&loaded));
  TEST_ASSERT_EQUAL_STRING("WifiTamu", loaded.ssid);
  TEST_ASSERT_EQUAL_STRING("", loaded.password);
  TEST_ASSERT_TRUE(settings.saveWifi("WifiTamu", nullptr));
}

void test_a_bad_form_is_refused_and_writes_nothing() {
  FakeStore store;
  SettingsStore settings(store);
  TEST_ASSERT_FALSE(settings.saveWifi("", "password123"));
  TEST_ASSERT_FALSE(settings.saveWifi("Warung", "short"));
  TEST_ASSERT_FALSE(settings.saveWifi("War\nung", "password123"));
  TEST_ASSERT_EQUAL_INT(0, store.puts);
  TEST_ASSERT_TRUE(store.data.empty());
}

void test_a_failed_password_write_removes_the_name_again() {
  FakeStore store;
  SettingsStore settings(store);
  store.failPutOn = key::kWifiPassword;
  TEST_ASSERT_FALSE(settings.saveWifi("Warung", "password123"));
  TEST_ASSERT_EQUAL_UINT(0, store.data.count(key::kWifiSsid));
  WifiSettings loaded;
  TEST_ASSERT_FALSE(settings.loadWifi(&loaded));
}

void test_a_failed_name_write_stores_nothing() {
  FakeStore store;
  SettingsStore settings(store);
  store.failPutOn = key::kWifiSsid;
  TEST_ASSERT_FALSE(settings.saveWifi("Warung", "password123"));
  TEST_ASSERT_TRUE(store.data.empty());
}

void test_nothing_is_loaded_when_no_name_is_stored() {
  FakeStore store;
  SettingsStore settings(store);
  WifiSettings loaded;
  memset(&loaded, 0x7A, sizeof(loaded));
  TEST_ASSERT_FALSE(settings.loadWifi(&loaded));
  TEST_ASSERT_EQUAL_UINT8(0x7A, static_cast<uint8_t>(loaded.ssid[0]));
}

void test_a_damaged_flash_is_refused() {
  FakeStore store;
  SettingsStore settings(store);
  WifiSettings loaded;
  store.data[key::kWifiSsid] = std::string(40, 'a');
  TEST_ASSERT_FALSE(settings.loadWifi(&loaded));
  store.data[key::kWifiSsid] = "Warung";
  store.data[key::kWifiPassword] = "short";
  TEST_ASSERT_FALSE(settings.loadWifi(&loaded));
  store.data[key::kWifiPassword] = "pass\nword12";
  TEST_ASSERT_FALSE(settings.loadWifi(&loaded));
}

void test_clearing_removes_both_and_reports_a_failure() {
  FakeStore store;
  SettingsStore settings(store);
  settings.saveWifi("Warung", "password123");
  TEST_ASSERT_TRUE(settings.clearWifi());
  TEST_ASSERT_TRUE(store.data.empty());
  settings.saveWifi("Warung", "password123");
  store.failRemoveOn = key::kWifiPassword;
  TEST_ASSERT_FALSE(settings.clearWifi());
  TEST_ASSERT_EQUAL_UINT(0, store.data.count(key::kWifiSsid));
}

void test_the_key_is_made_from_32_random_bytes_and_stored() {
  FakeStore store;
  SettingsStore settings(store);
  Script script = {kBytesZeroToThirtyOne, 8, 0};
  char apiKey[kApiKeySize];
  bool created = false;
  TEST_ASSERT_TRUE(settings.ensureApiKey(scripted, &script, apiKey, &created));
  TEST_ASSERT_TRUE(created);
  TEST_ASSERT_EQUAL_STRING(kKeyForZeroToThirtyOne, apiKey);
  TEST_ASSERT_EQUAL_UINT(kGeneratedApiKeyLength, strlen(apiKey));
  TEST_ASSERT_EQUAL_STRING(kKeyForZeroToThirtyOne, store.data[key::kApiKey].c_str());
  TEST_ASSERT_EQUAL_UINT(8, script.next);
}

void test_the_key_is_made_only_once() {
  FakeStore store;
  SettingsStore settings(store);
  Script script = {kBytesZeroToThirtyOne, 8, 0};
  char first[kApiKeySize];
  char second[kApiKeySize];
  bool created = false;
  TEST_ASSERT_TRUE(settings.ensureApiKey(scripted, &script, first, &created));
  TEST_ASSERT_TRUE(created);
  TEST_ASSERT_TRUE(settings.ensureApiKey(scripted, &script, second, &created));
  TEST_ASSERT_FALSE(created);
  TEST_ASSERT_EQUAL_STRING(first, second);
  TEST_ASSERT_EQUAL_UINT(8, script.next);
  TEST_ASSERT_EQUAL_INT(1, store.puts);
}

void test_a_stuck_random_source_makes_no_key() {
  FakeStore store;
  SettingsStore settings(store);
  const uint32_t stuck[1] = {0x12345678};
  Script script = {stuck, 1, 0};
  char apiKey[kApiKeySize];
  memset(apiKey, 'x', sizeof(apiKey));
  bool created = true;
  TEST_ASSERT_FALSE(settings.ensureApiKey(scripted, &script, apiKey, &created));
  TEST_ASSERT_FALSE(created);
  TEST_ASSERT_EQUAL_STRING("", apiKey);
  TEST_ASSERT_TRUE(store.data.empty());
}

void test_a_failed_key_write_reports_a_failure() {
  FakeStore store;
  SettingsStore settings(store);
  store.failPutOn = key::kApiKey;
  Script script = {kBytesZeroToThirtyOne, 8, 0};
  char apiKey[kApiKeySize];
  bool created = true;
  TEST_ASSERT_FALSE(settings.ensureApiKey(scripted, &script, apiKey, &created));
  TEST_ASSERT_FALSE(created);
  TEST_ASSERT_EQUAL_STRING("", apiKey);
}

void test_a_stored_key_of_the_wrong_shape_is_not_used() {
  FakeStore store;
  SettingsStore settings(store);
  char apiKey[kApiKeySize];
  store.data[key::kApiKey] = "too-short";
  TEST_ASSERT_FALSE(settings.loadApiKey(apiKey));
  store.data[key::kApiKey] = std::string(31, 'a');
  TEST_ASSERT_FALSE(settings.loadApiKey(apiKey));
  store.data[key::kApiKey] = std::string(32, 'a');
  TEST_ASSERT_TRUE(settings.loadApiKey(apiKey));
  store.data[key::kApiKey] = std::string(128, 'a');
  TEST_ASSERT_TRUE(settings.loadApiKey(apiKey));
  store.data[key::kApiKey] = std::string(129, 'a');
  TEST_ASSERT_FALSE(settings.loadApiKey(apiKey));
  store.data[key::kApiKey] = std::string(31, 'a') + "+";
  TEST_ASSERT_FALSE(settings.loadApiKey(apiKey));
  TEST_ASSERT_EQUAL_STRING("", apiKey);
}

void test_a_damaged_key_is_replaced_by_a_new_one() {
  FakeStore store;
  SettingsStore settings(store);
  store.data[key::kApiKey] = "damaged";
  Script script = {kBytesZeroToThirtyOne, 8, 0};
  char apiKey[kApiKeySize];
  bool created = false;
  TEST_ASSERT_TRUE(settings.ensureApiKey(scripted, &script, apiKey, &created));
  TEST_ASSERT_TRUE(created);
  TEST_ASSERT_EQUAL_STRING(kKeyForZeroToThirtyOne, apiKey);
}

void test_the_key_can_be_cleared() {
  FakeStore store;
  SettingsStore settings(store);
  Script script = {kBytesZeroToThirtyOne, 8, 0};
  char apiKey[kApiKeySize];
  bool created = false;
  settings.ensureApiKey(scripted, &script, apiKey, &created);
  TEST_ASSERT_TRUE(settings.clearApiKey());
  TEST_ASSERT_FALSE(settings.loadApiKey(apiKey));
  store.failRemoveOn = key::kApiKey;
  TEST_ASSERT_FALSE(settings.clearApiKey());
}

void test_the_log_line_says_what_is_stored_and_shows_no_secret() {
  FakeStore store;
  SettingsStore settings(store);
  char line[128];
  settings.describe(line, sizeof(line));
  TEST_ASSERT_EQUAL_STRING("wifi=none api_key=none", line);

  settings.saveWifi("Warung Bu Sri", "kopi-susu-2026");
  Script script = {kBytesZeroToThirtyOne, 8, 0};
  char apiKey[kApiKeySize];
  bool created = false;
  settings.ensureApiKey(scripted, &script, apiKey, &created);
  const size_t length = settings.describe(line, sizeof(line));
  TEST_ASSERT_EQUAL_STRING("wifi=Warung Bu Sri password=set api_key=set", line);
  TEST_ASSERT_EQUAL_UINT(strlen(line), length);
  TEST_ASSERT_NULL(strstr(line, "kopi-susu-2026"));
  TEST_ASSERT_NULL(strstr(line, kKeyForZeroToThirtyOne));

  settings.saveWifi("WifiTamu", "");
  settings.describe(line, sizeof(line));
  TEST_ASSERT_EQUAL_STRING("wifi=WifiTamu password=none api_key=set", line);
}

void test_the_log_line_is_cut_to_a_small_buffer() {
  FakeStore store;
  SettingsStore settings(store);
  settings.saveWifi("Warung Bu Sri", "kopi-susu-2026");
  char small[12];
  const size_t length = settings.describe(small, sizeof(small));
  TEST_ASSERT_EQUAL_UINT(11, length);
  TEST_ASSERT_EQUAL_UINT(11, strlen(small));
  TEST_ASSERT_EQUAL_UINT(0, settings.describe(small, 0));
}

void test_the_keys_fit_the_limit_of_the_flash() {
  TEST_ASSERT_TRUE(strlen(key::kWifiSsid) <= 15);
  TEST_ASSERT_TRUE(strlen(key::kWifiPassword) <= 15);
  TEST_ASSERT_TRUE(strlen(key::kApiKey) <= 15);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_base64url_matches_the_rfc_4648_examples);
  RUN_TEST(test_base64url_uses_the_url_alphabet_and_no_padding);
  RUN_TEST(test_base64url_refuses_a_small_buffer);
  RUN_TEST(test_wifi_settings_are_saved_and_loaded);
  RUN_TEST(test_an_open_network_stores_no_password_and_removes_an_old_one);
  RUN_TEST(test_a_bad_form_is_refused_and_writes_nothing);
  RUN_TEST(test_a_failed_password_write_removes_the_name_again);
  RUN_TEST(test_a_failed_name_write_stores_nothing);
  RUN_TEST(test_nothing_is_loaded_when_no_name_is_stored);
  RUN_TEST(test_a_damaged_flash_is_refused);
  RUN_TEST(test_clearing_removes_both_and_reports_a_failure);
  RUN_TEST(test_the_key_is_made_from_32_random_bytes_and_stored);
  RUN_TEST(test_the_key_is_made_only_once);
  RUN_TEST(test_a_stuck_random_source_makes_no_key);
  RUN_TEST(test_a_failed_key_write_reports_a_failure);
  RUN_TEST(test_a_stored_key_of_the_wrong_shape_is_not_used);
  RUN_TEST(test_a_damaged_key_is_replaced_by_a_new_one);
  RUN_TEST(test_the_key_can_be_cleared);
  RUN_TEST(test_the_log_line_says_what_is_stored_and_shows_no_secret);
  RUN_TEST(test_the_log_line_is_cut_to_a_small_buffer);
  RUN_TEST(test_the_keys_fit_the_limit_of_the_flash);
  return UNITY_END();
}
