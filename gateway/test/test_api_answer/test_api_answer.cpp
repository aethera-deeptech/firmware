#include <string.h>
#include <unity.h>

#include <string>

#include "api-answer.h"

using namespace wattlink;

namespace {

const char kPoll[] = "0123456789abcdef0123456789abcdef";

bool readings(const std::string &body, ReadingsAnswer *out) { return parseReadingsAnswer(body.data(), body.size(), out); }
bool enroll(const std::string &body, EnrollAnswer *out) { return parseEnrollAnswer(body.data(), body.size(), out); }
bool failure(const std::string &body, ErrorAnswer *out) { return parseErrorAnswer(body.data(), body.size(), out); }

std::string command(const char *id, const char *node, const char *action, const char *expires) {
  return std::string("{\"id\":\"") + id + "\",\"nodeId\":\"" + node + "\",\"action\":\"" + action + "\",\"expiresAt\":\"" + expires + "\"}";
}

std::string answer(const std::string &commands) {
  return "{\"ackSeq\":41,\"serverTime\":\"2026-11-03T08:01:02Z\",\"accepted\":1,\"rejected\":0,\"commands\":[" + commands + "]}";
}

uint32_t timeOf(const char *text) {
  uint32_t value = 0;
  TEST_ASSERT_TRUE(parseIsoTime(text, &value));
  return value;
}

bool badTime(const char *text) {
  uint32_t value = 77;
  return !parseIsoTime(text, &value);
}

void test_the_time_of_the_service_is_read_in_unix_seconds() {
  TEST_ASSERT_EQUAL_UINT32(1793692862u, timeOf("2026-11-03T08:01:02Z"));
  TEST_ASSERT_EQUAL_UINT32(1700000000u, timeOf("2023-11-14T22:13:20Z"));
  TEST_ASSERT_EQUAL_UINT32(0u, timeOf("1970-01-01T00:00:00Z"));
  TEST_ASSERT_EQUAL_UINT32(1709164800u, timeOf("2024-02-29T00:00:00Z"));
}

void test_an_offset_and_a_fraction_of_a_second_are_read() {
  // The same moment in the time zone of the business, seven hours ahead of UTC.
  TEST_ASSERT_EQUAL_UINT32(1793692862u, timeOf("2026-11-03T15:01:02+07:00"));
  TEST_ASSERT_EQUAL_UINT32(1793692862u, timeOf("2026-11-03T01:01:02-07:00"));
  TEST_ASSERT_EQUAL_UINT32(1793692862u, timeOf("2026-11-03T08:01:02.123456Z"));
  TEST_ASSERT_EQUAL_UINT32(1793692862u, timeOf("2026-11-03T13:31:02+05:30"));
  // An offset that moves the time over the end of the day.
  TEST_ASSERT_EQUAL_UINT32(1793692862u, timeOf("2026-11-04T03:01:02+19:00"));
}

void test_a_time_that_does_not_exist_or_is_not_the_form_is_refused() {
  TEST_ASSERT_TRUE(badTime(""));
  TEST_ASSERT_TRUE(badTime("2026-11-03"));
  TEST_ASSERT_TRUE(badTime("2026-11-03 08:01:02Z"));
  TEST_ASSERT_TRUE(badTime("2026-11-03T08:01:02"));
  TEST_ASSERT_TRUE(badTime("2026-11-03T08:01:02z"));
  TEST_ASSERT_TRUE(badTime("2026-11-03T08:01:02Z "));
  TEST_ASSERT_TRUE(badTime("2026-11-03T08:01:02.Z"));
  TEST_ASSERT_TRUE(badTime("2026-11-03T08:01:02+0700"));
  TEST_ASSERT_TRUE(badTime("2026-11-03T08:01:02+24:00"));
  TEST_ASSERT_TRUE(badTime("2026-11-03T08:01:02+07:60"));
  TEST_ASSERT_TRUE(badTime("2026-13-03T08:01:02Z"));
  TEST_ASSERT_TRUE(badTime("2026-00-03T08:01:02Z"));
  TEST_ASSERT_TRUE(badTime("2026-11-31T08:01:02Z"));
  TEST_ASSERT_TRUE(badTime("2026-02-29T08:01:02Z"));
  TEST_ASSERT_TRUE(badTime("2100-02-29T08:01:02Z"));
  TEST_ASSERT_TRUE(badTime("2026-11-03T24:01:02Z"));
  TEST_ASSERT_TRUE(badTime("2026-11-03T08:60:02Z"));
  TEST_ASSERT_TRUE(badTime("2026-11-03T08:01:60Z"));
  TEST_ASSERT_TRUE(badTime("2026-1a-03T08:01:02Z"));
  TEST_ASSERT_TRUE(badTime("+2026-11-03T08:01:02Z"));
  uint32_t value = 0;
  TEST_ASSERT_FALSE(parseIsoTime(nullptr, &value));
}

void test_a_time_outside_the_range_of_32_bits_is_refused_and_the_edges_are_read() {
  TEST_ASSERT_EQUAL_UINT32(4294967295u, timeOf("2106-02-07T06:28:15Z"));
  TEST_ASSERT_TRUE(badTime("2106-02-07T06:28:16Z"));
  TEST_ASSERT_TRUE(badTime("1969-12-31T23:59:59Z"));
  TEST_ASSERT_TRUE(badTime("0000-01-01T00:00:00Z"));
  TEST_ASSERT_TRUE(badTime("9999-12-31T23:59:59Z"));
  // The offset can move a time before 1970 over the edge too.
  TEST_ASSERT_TRUE(badTime("1970-01-01T00:00:00+01:00"));
  TEST_ASSERT_EQUAL_UINT32(0u, timeOf("1970-01-01T01:00:00+01:00"));
  TEST_ASSERT_TRUE(badTime("2106-02-07T06:28:15-00:01"));
}

void test_a_good_answer_of_the_readings_route_is_read() {
  ReadingsAnswer a;
  TEST_ASSERT_TRUE(readings(answer(""), &a));
  TEST_ASSERT_EQUAL_UINT32(41, a.ackSeq);
  TEST_ASSERT_EQUAL_UINT32(1793692862u, a.serverTime);
  TEST_ASSERT_EQUAL_UINT(0, a.commandCount);
  TEST_ASSERT_EQUAL_UINT(0, a.badCommands);
}

void test_the_commands_of_an_answer_are_read_in_order() {
  ReadingsAnswer a;
  const std::string body =
      answer(command("6f1c7e0a-0000-4000-8000-000000000001", "PLUG-0001", "off", "2026-11-03T08:11:02Z") + "," +
             command("6f1c7e0a-0000-4000-8000-000000000002", "PLUG-0002", "on", "2026-11-03T15:11:02+07:00"));
  TEST_ASSERT_TRUE(readings(body, &a));
  TEST_ASSERT_EQUAL_UINT(2, a.commandCount);
  TEST_ASSERT_EQUAL_STRING("6f1c7e0a-0000-4000-8000-000000000001", a.commands[0].id);
  TEST_ASSERT_EQUAL_STRING("PLUG-0001", a.commands[0].nodeId);
  TEST_ASSERT_FALSE(a.commands[0].on);
  TEST_ASSERT_EQUAL_UINT32(1793693462u, a.commands[0].expiresAt);
  TEST_ASSERT_EQUAL_STRING("6f1c7e0a-0000-4000-8000-000000000002", a.commands[1].id);
  TEST_ASSERT_TRUE(a.commands[1].on);
  TEST_ASSERT_EQUAL_UINT32(1793693462u, a.commands[1].expiresAt);
}

void test_the_order_of_the_keys_white_space_and_unknown_keys_do_not_matter() {
  ReadingsAnswer a;
  const std::string body =
      " \n{ \"extra\" : {\"deep\":[1,2.5e3,-4,true,false,null,\"x\",{\"a\":[]}]},\n \"commands\" : [ { \"expiresAt\":\"2026-11-03T08:11:02Z\", \"x\":1,"
      "\"action\":\"on\",\"nodeId\":\"PLUG-0001\",\"id\":\"abc-1\" } ],\"serverTime\":\"2026-11-03T08:01:02Z\",\"ackSeq\":7,\"accepted\":0,\"rejected\":0 } \r\n";
  TEST_ASSERT_TRUE(readings(body, &a));
  TEST_ASSERT_EQUAL_UINT32(7, a.ackSeq);
  TEST_ASSERT_EQUAL_UINT(1, a.commandCount);
  TEST_ASSERT_EQUAL_STRING("abc-1", a.commands[0].id);
}

void test_an_answer_without_a_list_of_commands_has_none() {
  ReadingsAnswer a;
  TEST_ASSERT_TRUE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\"}", &a));
  TEST_ASSERT_EQUAL_UINT(0, a.commandCount);
  TEST_ASSERT_TRUE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"commands\":null}", &a));
  TEST_ASSERT_EQUAL_UINT(0, a.commandCount);
  TEST_ASSERT_TRUE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"commands\":{\"a\":1}}", &a));
  TEST_ASSERT_EQUAL_UINT(0, a.commandCount);
}

void test_an_answer_needs_the_number_and_the_time_of_the_service() {
  ReadingsAnswer a;
  TEST_ASSERT_FALSE(readings("{\"serverTime\":\"2026-11-03T08:01:02Z\",\"commands\":[]}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"commands\":[]}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"yesterday\"}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":1793692862}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":\"1\",\"serverTime\":\"2026-11-03T08:01:02Z\"}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":-1,\"serverTime\":\"2026-11-03T08:01:02Z\"}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1.5,\"serverTime\":\"2026-11-03T08:01:02Z\"}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1e2,\"serverTime\":\"2026-11-03T08:01:02Z\"}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":4294967296,\"serverTime\":\"2026-11-03T08:01:02Z\"}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":99999999999999999999999,\"serverTime\":\"2026-11-03T08:01:02Z\"}", &a));
  TEST_ASSERT_TRUE(readings("{\"ackSeq\":4294967295,\"serverTime\":\"2026-11-03T08:01:02Z\"}", &a));
  TEST_ASSERT_EQUAL_UINT32(4294967295u, a.ackSeq);
  // A failed read leaves nothing to use.
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"commands\":[]}", &a));
  TEST_ASSERT_EQUAL_UINT32(0, a.ackSeq);
  TEST_ASSERT_EQUAL_UINT(0, a.commandCount);
}

void test_a_key_that_comes_twice_is_taken_from_its_last_value() {
  ReadingsAnswer a;
  TEST_ASSERT_TRUE(readings("{\"ackSeq\":1,\"ackSeq\":2,\"serverTime\":\"2026-11-03T08:01:02Z\"}", &a));
  TEST_ASSERT_EQUAL_UINT32(2, a.ackSeq);
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"ackSeq\":\"x\",\"serverTime\":\"2026-11-03T08:01:02Z\"}", &a));
}

void test_a_command_that_cannot_be_run_is_counted_and_left_out_and_the_rest_stay() {
  ReadingsAnswer a;
  const std::string good = command("good-1", "PLUG-0001", "on", "2026-11-03T08:11:02Z");
  const std::string body = answer(
      command("bad id", "PLUG-0001", "on", "2026-11-03T08:11:02Z") + "," +                       // a space in the id
      command("", "PLUG-0001", "on", "2026-11-03T08:11:02Z") + "," +                             // no id
      command("0123456789012345678901234567890123456", "PLUG-0001", "on", "2026-11-03T08:11:02Z") + "," +  // 37 characters
      command("id-4", "", "on", "2026-11-03T08:11:02Z") + "," +                                  // no node
      command("id-5", "ABCDEFGHIJKLM", "on", "2026-11-03T08:11:02Z") + "," +                     // a node id of 13 characters
      command("id-6", "PLUG-0001", "toggle", "2026-11-03T08:11:02Z") + "," +                     // an action that is not on or off
      command("id-7", "PLUG-0001", "ON", "2026-11-03T08:11:02Z") + "," +
      command("id-8", "PLUG-0001", "on", "soon") + "," +                                         // a time that is not a time
      "{\"id\":\"id-9\",\"nodeId\":\"PLUG-0001\",\"action\":\"on\"}," +                           // no expiry
      "{\"id\":5,\"nodeId\":\"PLUG-0001\",\"action\":\"on\",\"expiresAt\":\"2026-11-03T08:11:02Z\"}," +
      "\"text\",7,null,[]," + good);
  TEST_ASSERT_TRUE(readings(body, &a));
  TEST_ASSERT_EQUAL_UINT(1, a.commandCount);
  TEST_ASSERT_EQUAL_STRING("good-1", a.commands[0].id);
  TEST_ASSERT_EQUAL_UINT(14, a.badCommands);
}

void test_a_node_id_of_12_characters_fits_and_a_text_with_an_escape_is_read_as_its_characters() {
  ReadingsAnswer a;
  TEST_ASSERT_TRUE(readings(answer("{\"id\":\"id\\u002d1\",\"nodeId\":\"ABCDEFGHIJKL\",\"action\":\"o\\u006e\",\"expiresAt\":\"2026-11-03T08:11:02Z\"}"), &a));
  TEST_ASSERT_EQUAL_UINT(1, a.commandCount);
  TEST_ASSERT_EQUAL_STRING("id-1", a.commands[0].id);
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJKL", a.commands[0].nodeId);
  TEST_ASSERT_TRUE(a.commands[0].on);
}

void test_a_text_with_a_character_that_is_not_printable_ascii_is_not_used_in_any_field() {
  ReadingsAnswer a;
  // The node id is only checked for its characters, so these cases prove the rule of the scanner and not the check of an id. Each one is left out.
  const char *nodes[] = {"PLUG-\\n1",         "PLUG-\\u0001x", "PLUG-\\u007f", "PLUG-\\u00e9", "PLUG-\\u0000x", "PLUG-\\u20ac",
                         "PLUG-\xC3\xA9", "PLUG-\x7F",     "PLUG/0001",   "PLUG 0001",   "PLUG-\\\"1"};
  for (const char *node : nodes) {
    const std::string body = answer(std::string("{\"id\":\"id-1\",\"nodeId\":\"") + node + "\",\"action\":\"on\",\"expiresAt\":\"2026-11-03T08:11:02Z\"}");
    TEST_ASSERT_TRUE(readings(body, &a));
    TEST_ASSERT_EQUAL_UINT(0, a.commandCount);
    TEST_ASSERT_EQUAL_UINT(1, a.badCommands);
  }
  // The characters of a node id of the contract are kept.
  TEST_ASSERT_TRUE(readings(answer("{\"id\":\"id-1\",\"nodeId\":\"a:B_c-9\",\"action\":\"on\",\"expiresAt\":\"2026-11-03T08:11:02Z\"}"), &a));
  TEST_ASSERT_EQUAL_UINT(1, a.commandCount);
  // A code of an error with such a character is empty, and a code with only printable characters is kept.
  ErrorAnswer e;
  TEST_ASSERT_TRUE(failure("{\"code\":\"a\\nb\"}", &e));
  TEST_ASSERT_EQUAL_STRING("", e.code);
  TEST_ASSERT_TRUE(failure("{\"code\":\"a\\u001bb\"}", &e));
  TEST_ASSERT_EQUAL_STRING("", e.code);
  TEST_ASSERT_TRUE(failure("{\"code\":\"a\xC3\xA9\"}", &e));
  TEST_ASSERT_EQUAL_STRING("", e.code);
  TEST_ASSERT_TRUE(failure("{\"code\":\"a b~\"}", &e));
  TEST_ASSERT_EQUAL_STRING("a b~", e.code);
  // A raw line feed inside a string is not valid JSON at all.
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"x\":\"a\nb\"}", &a));
}

void test_the_commands_beyond_the_limit_are_counted_and_the_first_ones_are_kept() {
  ReadingsAnswer a;
  std::string commands;
  for (size_t i = 0; i < kMaxAnswerCommands + 5; i++) {
    if (i > 0) {
      commands += ",";
    }
    commands += command(("id-" + std::to_string(i)).c_str(), "PLUG-0001", "on", "2026-11-03T08:11:02Z");
  }
  TEST_ASSERT_TRUE(readings(answer(commands), &a));
  TEST_ASSERT_EQUAL_UINT(kMaxAnswerCommands, a.commandCount);
  TEST_ASSERT_EQUAL_UINT(5, a.extraCommands);
  TEST_ASSERT_EQUAL_STRING("id-0", a.commands[0].id);
  TEST_ASSERT_EQUAL_STRING("id-23", a.commands[kMaxAnswerCommands - 1].id);
}

void test_a_body_that_is_broken_is_refused() {
  ReadingsAnswer a;
  const std::string good = answer(command("id-1", "PLUG-0001", "on", "2026-11-03T08:11:02Z"));
  TEST_ASSERT_TRUE(readings(good, &a));
  // Every shorter body is cut in the middle of the JSON, so none of them is read.
  for (size_t length = 0; length < good.size(); length++) {
    TEST_ASSERT_FALSE(parseReadingsAnswer(good.data(), length, &a));
  }
  TEST_ASSERT_FALSE(readings(good + "x", &a));
  TEST_ASSERT_FALSE(readings(good + "{}", &a));
  TEST_ASSERT_TRUE(readings(good + "  \n", &a));
  TEST_ASSERT_FALSE(readings("[]", &a));
  TEST_ASSERT_FALSE(readings("\"text\"", &a));
  TEST_ASSERT_FALSE(readings("<html>502</html>", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":01,\"serverTime\":\"2026-11-03T08:01:02Z\"}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"x\":tru}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"x\":\"\\q\"}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"x\":\"\\u12\"}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"x\":-}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"x\":1.}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"x\":1e}", &a));
  TEST_ASSERT_FALSE(readings("", &a));
  TEST_ASSERT_FALSE(parseReadingsAnswer(nullptr, 5, &a));
  std::string withZero = "{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\"}";
  withZero.push_back('\0');
  withZero += "x";
  TEST_ASSERT_FALSE(readings(withZero, &a));
}

void test_a_body_of_deep_nesting_or_of_a_huge_size_is_refused_without_a_crash() {
  ReadingsAnswer a;
  std::string deep = "{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"x\":";
  for (int i = 0; i < 20000; i++) {
    deep += "[";
  }
  TEST_ASSERT_FALSE(readings(deep, &a));
  std::string nested = "{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"x\":";
  for (int i = 0; i < 6; i++) {
    nested += "[";
  }
  for (int i = 0; i < 6; i++) {
    nested += "]";
  }
  TEST_ASSERT_TRUE(readings(nested + "}", &a));
  // A body above the limit is refused, even when it is valid JSON.
  const std::string padding(kMaxAnswerBytes, ' ');
  TEST_ASSERT_FALSE(readings(answer("") + padding, &a));
  TEST_ASSERT_TRUE(readings(answer("") + std::string(kMaxAnswerBytes - answer("").size(), ' '), &a));
}

void test_a_long_text_is_cut_by_size_and_never_written_past_its_buffer() {
  ReadingsAnswer a;
  const std::string longText(5000, 'a');
  TEST_ASSERT_TRUE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"message\":\"" + longText + "\"}", &a));
  // The key that is too long for the buffer is not the key that we know.
  TEST_ASSERT_TRUE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"ackSeq" + std::string(100, 'x') + "\":5}", &a));
  TEST_ASSERT_EQUAL_UINT32(1, a.ackSeq);
  // A server time that is too long for its buffer is not a time.
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z" + std::string(60, ' ') + "\"}", &a));
}

void test_an_error_body_gives_the_expected_number_and_the_wait() {
  ErrorAnswer e;
  TEST_ASSERT_TRUE(failure("{\"code\":\"sequence_gap\",\"message\":\"The sequence number is not the next one.\",\"expectedSeq\":57}", &e));
  TEST_ASSERT_EQUAL_STRING("sequence_gap", e.code);
  TEST_ASSERT_TRUE(e.hasExpectedSeq);
  TEST_ASSERT_EQUAL_UINT32(57, e.expectedSeq);
  TEST_ASSERT_EQUAL_UINT32(0, e.retryAfterSeconds);
  TEST_ASSERT_TRUE(failure("{\"code\":\"rate_limited\",\"message\":\"Too many\",\"retryAfterSeconds\":42}", &e));
  TEST_ASSERT_EQUAL_STRING("rate_limited", e.code);
  TEST_ASSERT_FALSE(e.hasExpectedSeq);
  TEST_ASSERT_EQUAL_UINT32(42, e.retryAfterSeconds);
  TEST_ASSERT_TRUE(failure("{\"code\":\"invalid_gateway_key\",\"message\":\"No\"}", &e));
  TEST_ASSERT_EQUAL_STRING("invalid_gateway_key", e.code);
  // The number 0 is a real expected number.
  TEST_ASSERT_TRUE(failure("{\"expectedSeq\":0}", &e));
  TEST_ASSERT_TRUE(e.hasExpectedSeq);
  TEST_ASSERT_EQUAL_UINT32(0, e.expectedSeq);
}

void test_an_error_body_with_wrong_types_or_odd_content_gives_empty_fields() {
  ErrorAnswer e;
  TEST_ASSERT_TRUE(failure("{\"code\":5,\"expectedSeq\":\"7\",\"retryAfterSeconds\":-3,\"fields\":{\"password\":\"too_short\"}}", &e));
  TEST_ASSERT_EQUAL_STRING("", e.code);
  TEST_ASSERT_FALSE(e.hasExpectedSeq);
  TEST_ASSERT_EQUAL_UINT32(0, e.retryAfterSeconds);
  TEST_ASSERT_TRUE(failure("{\"code\":\"" + std::string(60, 'c') + "\"}", &e));
  TEST_ASSERT_EQUAL_STRING("", e.code);
  TEST_ASSERT_TRUE(failure("{\"expectedSeq\":4294967296,\"retryAfterSeconds\":1.5}", &e));
  TEST_ASSERT_FALSE(e.hasExpectedSeq);
  TEST_ASSERT_EQUAL_UINT32(0, e.retryAfterSeconds);
  TEST_ASSERT_TRUE(failure("{}", &e));
  TEST_ASSERT_EQUAL_STRING("", e.code);
}

void test_an_error_body_that_is_not_json_is_reported_and_leaves_empty_fields() {
  ErrorAnswer e;
  TEST_ASSERT_FALSE(failure("<html>Bad gateway</html>", &e));
  TEST_ASSERT_EQUAL_STRING("", e.code);
  TEST_ASSERT_FALSE(e.hasExpectedSeq);
  TEST_ASSERT_FALSE(failure("", &e));
  TEST_ASSERT_FALSE(failure("{\"code\":\"x\"", &e));
  TEST_ASSERT_EQUAL_STRING("", e.code);
  TEST_ASSERT_FALSE(failure("[1,2]", &e));
}

void test_the_answer_of_the_enroll_route_is_read() {
  EnrollAnswer e;
  TEST_ASSERT_TRUE(enroll(std::string("{\"claimCode\":\"ABCD2345\",\"pollId\":\"") + kPoll + "\",\"expiresInSeconds\":600}", &e));
  TEST_ASSERT_EQUAL_STRING("ABCD2345", e.claimCode);
  TEST_ASSERT_EQUAL_STRING(kPoll, e.pollId);
  TEST_ASSERT_EQUAL_UINT32(600, e.expiresInSeconds);
  // A poll id of 64 characters is the longest that fits.
  TEST_ASSERT_TRUE(enroll("{\"claimCode\":\"ABCD2345\",\"pollId\":\"" + std::string(64, 'p') + "\",\"expiresInSeconds\":600,\"extra\":true}", &e));
  TEST_ASSERT_EQUAL_UINT(64, strlen(e.pollId));
}

void test_an_enroll_answer_with_a_code_or_a_poll_id_that_the_hub_cannot_use_is_refused() {
  EnrollAnswer e;
  const std::string poll = std::string("\"pollId\":\"") + kPoll + "\"";
  TEST_ASSERT_FALSE(enroll("{\"claimCode\":\"ABCD234\"," + poll + ",\"expiresInSeconds\":600}", &e));    // 7 characters
  TEST_ASSERT_FALSE(enroll("{\"claimCode\":\"ABCD23456\"," + poll + ",\"expiresInSeconds\":600}", &e));  // 9 characters
  TEST_ASSERT_FALSE(enroll("{\"claimCode\":\"abcd2345\"," + poll + ",\"expiresInSeconds\":600}", &e));   // lower case
  TEST_ASSERT_FALSE(enroll("{\"claimCode\":\"ABCD2340\"," + poll + ",\"expiresInSeconds\":600}", &e));   // 0 is not in the alphabet
  TEST_ASSERT_FALSE(enroll("{\"claimCode\":\"ABCD 345\"," + poll + ",\"expiresInSeconds\":600}", &e));
  TEST_ASSERT_FALSE(enroll("{\"claimCode\":\"ABCDI345\"," + poll + ",\"expiresInSeconds\":600}", &e));   // no I in the alphabet
  TEST_ASSERT_FALSE(enroll("{\"claimCode\":\"ABCDO345\"," + poll + ",\"expiresInSeconds\":600}", &e));   // no O in the alphabet
  TEST_ASSERT_FALSE(enroll("{\"claimCode\":12345678," + poll + ",\"expiresInSeconds\":600}", &e));
  TEST_ASSERT_FALSE(enroll("{\"pollId\":\"" + std::string(31, 'p') + "\",\"claimCode\":\"ABCD2345\",\"expiresInSeconds\":600}", &e));  // too short
  TEST_ASSERT_FALSE(enroll("{\"pollId\":\"" + std::string(65, 'p') + "\",\"claimCode\":\"ABCD2345\",\"expiresInSeconds\":600}", &e));  // too long
  TEST_ASSERT_FALSE(enroll("{\"pollId\":\"" + std::string(31, 'p') + "/\",\"claimCode\":\"ABCD2345\",\"expiresInSeconds\":600}", &e));  // a slash
  TEST_ASSERT_FALSE(enroll("{\"pollId\":\"" + std::string(31, 'p') + "?\",\"claimCode\":\"ABCD2345\",\"expiresInSeconds\":600}", &e));
  TEST_ASSERT_FALSE(enroll("{\"claimCode\":\"ABCD2345\",\"expiresInSeconds\":600}", &e));
  TEST_ASSERT_FALSE(enroll("{\"claimCode\":\"ABCD2345\"," + poll + "}", &e));
  TEST_ASSERT_FALSE(enroll("{\"claimCode\":\"ABCD2345\"," + poll + ",\"expiresInSeconds\":\"600\"}", &e));
  TEST_ASSERT_FALSE(enroll("{}", &e));
  TEST_ASSERT_EQUAL_STRING("", e.claimCode);
  TEST_ASSERT_EQUAL_STRING("", e.pollId);
}

void test_the_edges_of_the_sizes_of_the_buffers() {
  ReadingsAnswer a;
  ErrorAnswer e;
  // A code of 40 characters fits and a code of 41 does not.
  TEST_ASSERT_TRUE(failure("{\"code\":\"" + std::string(40, 'c') + "\"}", &e));
  TEST_ASSERT_EQUAL_UINT(40, strlen(e.code));
  TEST_ASSERT_TRUE(failure("{\"code\":\"" + std::string(41, 'c') + "\"}", &e));
  TEST_ASSERT_EQUAL_STRING("", e.code);
  // A server time of 39 characters fits and one of 40 does not.
  const std::string tail = "\",\"ackSeq\":1}";
  TEST_ASSERT_TRUE(readings("{\"serverTime\":\"2026-11-03T08:01:02." + std::string(18, '1') + "Z" + tail, &a));
  TEST_ASSERT_FALSE(readings("{\"serverTime\":\"2026-11-03T08:01:02." + std::string(19, '1') + "Z" + tail, &a));
  // A key of 23 characters is read as a key and a key of 24 is not, and neither one is a key that the hub knows.
  TEST_ASSERT_TRUE(readings("{\"" + std::string(23, 'k') + "\":1,\"" + std::string(24, 'k') + "\":2,\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\"}", &a));
  // The time form of the Go service, a fraction of nine digits and an offset.
  TEST_ASSERT_EQUAL_UINT32(1793692862u, timeOf("2026-11-03T15:01:02.123456789+07:00"));
  TEST_ASSERT_EQUAL_UINT32(951782400u, timeOf("2000-02-29T00:00:00Z"));
}

void test_the_nesting_limit_has_an_exact_edge_for_arrays_and_for_objects() {
  ReadingsAnswer a;
  const std::string head = "{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"x\":";
  for (int depth = 7; depth <= 9; depth++) {
    std::string arrays = head;
    std::string objects = head;
    for (int i = 0; i < depth; i++) {
      arrays += "[";
      objects += "{\"k\":";
    }
    arrays += std::string(static_cast<size_t>(depth), ']');
    objects += "1" + std::string(static_cast<size_t>(depth), '}');
    const bool wanted = depth <= 8;
    TEST_ASSERT_EQUAL(wanted, readings(arrays + "}", &a));
    TEST_ASSERT_EQUAL(wanted, readings(objects + "}", &a));
  }
  // The same inside a command and inside the error body.
  std::string inCommand = "{\"id\":\"id-1\",\"nodeId\":\"PLUG-0001\",\"action\":\"on\",\"expiresAt\":\"2026-11-03T08:11:02Z\",\"x\":";
  inCommand += std::string(20000, '[');
  TEST_ASSERT_FALSE(readings(answer(inCommand), &a));
  ErrorAnswer e;
  TEST_ASSERT_FALSE(failure("{\"fields\":" + std::string(20000, '[') + "}", &e));
  TEST_ASSERT_EQUAL_STRING("", e.code);
}

void test_a_repeated_key_and_a_repeated_id_are_handled() {
  ReadingsAnswer a;
  const std::string one = command("id-1", "PLUG-0001", "on", "2026-11-03T08:11:02Z");
  // A second list of commands replaces the first, with its counts.
  TEST_ASSERT_TRUE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"commands\":[" + one + ",5],\"commands\":[" +
                                command("id-2", "PLUG-0002", "off", "2026-11-03T08:11:02Z") + "]}",
                            &a));
  TEST_ASSERT_EQUAL_UINT(1, a.commandCount);
  TEST_ASSERT_EQUAL_UINT(0, a.badCommands);
  TEST_ASSERT_EQUAL_STRING("id-2", a.commands[0].id);
  // The same id twice in one answer is given twice, because the command log of the relay decides what a repeat is.
  TEST_ASSERT_TRUE(readings(answer(one + "," + one), &a));
  TEST_ASSERT_EQUAL_UINT(2, a.commandCount);
  // The enroll answer takes the last value of a repeated key.
  EnrollAnswer e;
  TEST_ASSERT_TRUE(enroll(std::string("{\"claimCode\":\"AAAAAAAA\",\"claimCode\":\"BBBBBBBB\",\"pollId\":\"") + kPoll + "\",\"expiresInSeconds\":600}", &e));
  TEST_ASSERT_EQUAL_STRING("BBBBBBBB", e.claimCode);
  TEST_ASSERT_FALSE(enroll(std::string("{\"claimCode\":\"AAAAAAAA\",\"claimCode\":5,\"pollId\":\"") + kPoll + "\",\"expiresInSeconds\":600}", &e));
}

void test_an_error_body_with_a_code_and_a_list_of_fields_and_a_wait_of_zero() {
  ErrorAnswer e;
  TEST_ASSERT_TRUE(failure("{\"code\":\"validation\",\"message\":\"m\",\"fields\":{\"a\":\"b\",\"c\":[1,{\"d\":null}]},\"retryAfterSeconds\":0}", &e));
  TEST_ASSERT_EQUAL_STRING("validation", e.code);
  TEST_ASSERT_EQUAL_UINT32(0, e.retryAfterSeconds);
  TEST_ASSERT_FALSE(e.hasExpectedSeq);
}

std::string manyCommands(size_t count) {
  std::string commands;
  for (size_t i = 0; i < count; i++) {
    if (i > 0) {
      commands += ",";
    }
    commands += command(("id-" + std::to_string(i)).c_str(), "PLUG-0001", "on", "2026-11-03T08:11:02.123456789+07:00");
  }
  return commands;
}

void test_a_long_answer_that_was_cut_is_read_as_far_as_it_goes() {
  ReadingsAnswer a;
  const std::string body = answer(manyCommands(120));
  TEST_ASSERT_TRUE(body.size() > kMaxAnswerBytes);
  // Without the flag the cut body is refused, and with it the answer and the commands before the cut are kept.
  TEST_ASSERT_FALSE(parseReadingsAnswer(body.data(), kMaxAnswerBytes, &a));
  TEST_ASSERT_TRUE(parseReadingsAnswer(body.data(), kMaxAnswerBytes, &a, true));
  TEST_ASSERT_EQUAL_UINT32(41, a.ackSeq);
  TEST_ASSERT_EQUAL_UINT32(1793692862u, a.serverTime);
  TEST_ASSERT_EQUAL_UINT(kMaxAnswerCommands, a.commandCount);
  TEST_ASSERT_EQUAL_STRING("id-0", a.commands[0].id);
  TEST_ASSERT_TRUE(a.extraCommands > 0);
  TEST_ASSERT_EQUAL_UINT(0, a.badCommands);
}

void test_a_cut_at_every_byte_keeps_only_whole_commands_and_needs_the_first_fields() {
  const std::string body = answer(command("id-1", "PLUG-0001", "on", "2026-11-03T08:11:02Z") + "," + command("id-2", "PLUG-0002", "off", "2026-11-03T08:11:02Z"));
  const size_t afterTime = body.find("\"accepted\"");
  for (size_t length = 1; length < body.size(); length++) {
    ReadingsAnswer a;
    const bool ok = parseReadingsAnswer(body.data(), length, &a, true);
    // The answer is good once the time of the service is whole, which is a few bytes before the key after it.
    TEST_ASSERT_EQUAL(length >= afterTime - 1, ok);
    if (ok) {
      TEST_ASSERT_TRUE(a.commandCount <= 2);
      for (size_t i = 0; i < a.commandCount; i++) {
        TEST_ASSERT_EQUAL_STRING(i == 0 ? "id-1" : "id-2", a.commands[i].id);
        TEST_ASSERT_EQUAL_STRING(i == 0 ? "PLUG-0001" : "PLUG-0002", a.commands[i].nodeId);
      }
    }
  }
  ReadingsAnswer a;
  TEST_ASSERT_TRUE(parseReadingsAnswer(body.data(), body.size(), &a, true));
  TEST_ASSERT_EQUAL_UINT(2, a.commandCount);
}

void test_a_cut_body_with_wrong_bytes_before_the_cut_is_still_refused_and_the_keys_must_come_first() {
  ReadingsAnswer a;
  // A broken byte in the middle is not the end of the body.
  TEST_ASSERT_FALSE(parseReadingsAnswer("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"x\":?,\"commands\":[", 62, &a, true));
  // The fields come after the commands here, so the cut loses them.
  const std::string late = "{\"commands\":[" + manyCommands(3) + "],\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\"}";
  TEST_ASSERT_FALSE(parseReadingsAnswer(late.data(), late.size() - 10, &a, true));
  TEST_ASSERT_TRUE(parseReadingsAnswer(late.data(), late.size(), &a, true));
  // Bytes after a whole object are not a cut.
  const std::string good = answer("");
  TEST_ASSERT_FALSE(parseReadingsAnswer((good + "x").data(), good.size() + 1, &a, true));
  // A cut inside a word, a number and a string of a skipped value.
  const std::string head = "{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"accepted\":";
  TEST_ASSERT_TRUE(parseReadingsAnswer((head + "tru").data(), head.size() + 3, &a, true));
  TEST_ASSERT_TRUE(parseReadingsAnswer((head + "12").data(), head.size() + 2, &a, true));
  TEST_ASSERT_TRUE(parseReadingsAnswer((head + "\"ab").data(), head.size() + 3, &a, true));
  // A cut inside an escape of a skipped text is the end of the body too, and a wrong digit in the escape is not.
  TEST_ASSERT_TRUE(parseReadingsAnswer((head + "\"\\u00").data(), head.size() + 5, &a, true));
  TEST_ASSERT_TRUE(parseReadingsAnswer((head + "\"\\u").data(), head.size() + 3, &a, true));
  TEST_ASSERT_TRUE(parseReadingsAnswer((head + "\"\\").data(), head.size() + 2, &a, true));
  TEST_ASSERT_FALSE(parseReadingsAnswer((head + "\"\\u0x").data(), head.size() + 5, &a, true));
  // The wrong byte is the last byte of the body. It is a wrong byte and not the end of the body.
  TEST_ASSERT_FALSE(parseReadingsAnswer((head + "\"\\u000x").data(), head.size() + 7, &a, true));
  TEST_ASSERT_FALSE(parseReadingsAnswer((head + "\"a\n").data(), head.size() + 3, &a, true));
  TEST_ASSERT_FALSE(parseReadingsAnswer((head + "\"\\q").data(), head.size() + 3, &a, true));
  TEST_ASSERT_FALSE(parseReadingsAnswer((head + "trx").data(), head.size() + 3, &a, true));
}

void test_a_known_field_with_broken_json_fails_the_whole_answer_in_every_parser() {
  ReadingsAnswer a;
  ErrorAnswer e;
  EnrollAnswer n;
  const std::string poll = std::string("\"pollId\":\"") + kPoll + "\"";
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":-,\"serverTime\":\"2026-11-03T08:01:02Z\"}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"commands\":[{\"id\":\"ab}]}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"commands\":[{\"nodeId\":\"ab}]}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"commands\":[{\"action\":\"ab}]}", &a));
  TEST_ASSERT_FALSE(readings("{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"commands\":[{\"expiresAt\":\"ab}]}", &a));
  TEST_ASSERT_FALSE(failure("{\"code\":\"abc", &e));
  TEST_ASSERT_FALSE(failure("{\"expectedSeq\":-}", &e));
  TEST_ASSERT_FALSE(failure("{\"retryAfterSeconds\":1.}", &e));
  TEST_ASSERT_FALSE(enroll("{\"claimCode\":\"AB", &n));
  TEST_ASSERT_FALSE(enroll("{" + poll.substr(0, 20), &n));
  TEST_ASSERT_FALSE(enroll("{\"expiresInSeconds\":-}", &n));
}

void test_every_escape_of_json_and_the_forms_of_a_number_are_read() {
  ReadingsAnswer a;
  const std::string head = "{\"ackSeq\":1,\"serverTime\":\"2026-11-03T08:01:02Z\",\"x\":";
  TEST_ASSERT_TRUE(readings(head + "\"\\b\\f\\n\\r\\t\\/\\\\\\\"\\u00Ab\"}", &a));
  TEST_ASSERT_TRUE(readings(head + "[1e+5,1E-2,0.5,-0,-0.5e2,12345678901234567890]}", &a));
  // An escape that decodes to a control character makes the field invalid, and the answer is still read.
  ErrorAnswer e;
  TEST_ASSERT_TRUE(failure("{\"code\":\"a\\tb\"}", &e));
  TEST_ASSERT_EQUAL_STRING("", e.code);
  TEST_ASSERT_TRUE(failure("{\"code\":\"a\\u00Fz\"}", &e) == false);
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_time_of_the_service_is_read_in_unix_seconds);
  RUN_TEST(test_an_offset_and_a_fraction_of_a_second_are_read);
  RUN_TEST(test_a_time_that_does_not_exist_or_is_not_the_form_is_refused);
  RUN_TEST(test_a_time_outside_the_range_of_32_bits_is_refused_and_the_edges_are_read);
  RUN_TEST(test_a_good_answer_of_the_readings_route_is_read);
  RUN_TEST(test_the_commands_of_an_answer_are_read_in_order);
  RUN_TEST(test_the_order_of_the_keys_white_space_and_unknown_keys_do_not_matter);
  RUN_TEST(test_an_answer_without_a_list_of_commands_has_none);
  RUN_TEST(test_an_answer_needs_the_number_and_the_time_of_the_service);
  RUN_TEST(test_a_key_that_comes_twice_is_taken_from_its_last_value);
  RUN_TEST(test_a_command_that_cannot_be_run_is_counted_and_left_out_and_the_rest_stay);
  RUN_TEST(test_a_node_id_of_12_characters_fits_and_a_text_with_an_escape_is_read_as_its_characters);
  RUN_TEST(test_a_text_with_a_character_that_is_not_printable_ascii_is_not_used_in_any_field);
  RUN_TEST(test_the_commands_beyond_the_limit_are_counted_and_the_first_ones_are_kept);
  RUN_TEST(test_a_body_that_is_broken_is_refused);
  RUN_TEST(test_a_body_of_deep_nesting_or_of_a_huge_size_is_refused_without_a_crash);
  RUN_TEST(test_a_long_text_is_cut_by_size_and_never_written_past_its_buffer);
  RUN_TEST(test_an_error_body_gives_the_expected_number_and_the_wait);
  RUN_TEST(test_an_error_body_with_wrong_types_or_odd_content_gives_empty_fields);
  RUN_TEST(test_an_error_body_that_is_not_json_is_reported_and_leaves_empty_fields);
  RUN_TEST(test_the_edges_of_the_sizes_of_the_buffers);
  RUN_TEST(test_the_nesting_limit_has_an_exact_edge_for_arrays_and_for_objects);
  RUN_TEST(test_a_repeated_key_and_a_repeated_id_are_handled);
  RUN_TEST(test_an_error_body_with_a_code_and_a_list_of_fields_and_a_wait_of_zero);
  RUN_TEST(test_a_long_answer_that_was_cut_is_read_as_far_as_it_goes);
  RUN_TEST(test_a_cut_at_every_byte_keeps_only_whole_commands_and_needs_the_first_fields);
  RUN_TEST(test_a_cut_body_with_wrong_bytes_before_the_cut_is_still_refused_and_the_keys_must_come_first);
  RUN_TEST(test_a_known_field_with_broken_json_fails_the_whole_answer_in_every_parser);
  RUN_TEST(test_every_escape_of_json_and_the_forms_of_a_number_are_read);
  RUN_TEST(test_the_answer_of_the_enroll_route_is_read);
  RUN_TEST(test_an_enroll_answer_with_a_code_or_a_poll_id_that_the_hub_cannot_use_is_refused);
  return UNITY_END();
}
