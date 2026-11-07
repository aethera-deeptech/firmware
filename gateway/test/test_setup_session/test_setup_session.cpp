#include <unity.h>

#include "setup-session.h"

using namespace wattlink;

void setUp() {}
void tearDown() {}

void test_a_new_session_is_idle_with_no_time_left() {
  SetupSession session;
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Idle), static_cast<uint8_t>(session.state()));
  TEST_ASSERT_EQUAL_UINT32(0, session.secondsLeft(0));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Idle), static_cast<uint8_t>(session.update(999999)));
}

void test_a_session_runs_for_five_minutes() {
  SetupSession session;
  session.start(1000);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Running), static_cast<uint8_t>(session.state()));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Running), static_cast<uint8_t>(session.update(1000 + kSetupSessionMs - 1)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Expired), static_cast<uint8_t>(session.update(1000 + kSetupSessionMs)));
}

void test_the_limit_is_exactly_five_minutes() {
  TEST_ASSERT_EQUAL_UINT32(300000, kSetupSessionMs);
}

void test_an_expired_session_stays_expired_until_it_starts_again() {
  SetupSession session;
  session.start(0);
  session.update(kSetupSessionMs);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Expired), static_cast<uint8_t>(session.update(kSetupSessionMs + 5)));
  session.finish();
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Expired), static_cast<uint8_t>(session.state()));
  session.start(kSetupSessionMs + 10);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Running), static_cast<uint8_t>(session.state()));
}

void test_saving_the_settings_finishes_a_running_session() {
  SetupSession session;
  session.start(0);
  session.finish();
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Done), static_cast<uint8_t>(session.state()));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Done), static_cast<uint8_t>(session.update(kSetupSessionMs * 2)));
  TEST_ASSERT_EQUAL_UINT32(0, session.secondsLeft(10));
}

void test_finish_does_nothing_when_no_session_runs() {
  SetupSession session;
  session.finish();
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Idle), static_cast<uint8_t>(session.state()));
}

void test_cancel_returns_to_idle() {
  SetupSession session;
  session.start(0);
  session.cancel();
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Idle), static_cast<uint8_t>(session.state()));
  TEST_ASSERT_EQUAL_UINT32(0, session.secondsLeft(0));
}

void test_starting_again_gives_the_full_time() {
  SetupSession session;
  session.start(0);
  TEST_ASSERT_EQUAL_UINT32(100, session.secondsLeft(200000));
  session.start(200000);
  TEST_ASSERT_EQUAL_UINT32(300, session.secondsLeft(200000));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Running), static_cast<uint8_t>(session.update(200000 + kSetupSessionMs - 1)));
}

void test_the_seconds_left_round_up() {
  SetupSession session;
  session.start(0);
  TEST_ASSERT_EQUAL_UINT32(300, session.secondsLeft(0));
  TEST_ASSERT_EQUAL_UINT32(300, session.secondsLeft(1));
  TEST_ASSERT_EQUAL_UINT32(299, session.secondsLeft(1000));
  TEST_ASSERT_EQUAL_UINT32(1, session.secondsLeft(kSetupSessionMs - 1));
  TEST_ASSERT_EQUAL_UINT32(0, session.secondsLeft(kSetupSessionMs));
  TEST_ASSERT_EQUAL_UINT32(0, session.secondsLeft(kSetupSessionMs + 5000));
}

void test_the_timer_survives_the_wrap_of_the_millis_counter() {
  SetupSession session;
  const uint32_t start = 0xFFFFFFFFu - 100000u;
  session.start(start);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Running), static_cast<uint8_t>(session.update(start + 100000u)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Running), static_cast<uint8_t>(session.update(start + (kSetupSessionMs - 1))));
  TEST_ASSERT_EQUAL_UINT32(1, session.secondsLeft(start + (kSetupSessionMs - 1)));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SessionState::Expired), static_cast<uint8_t>(session.update(start + kSetupSessionMs)));
}

void test_at_most_two_clients_may_join() {
  TEST_ASSERT_EQUAL_UINT8(2, kMaxSetupClients);
  TEST_ASSERT_TRUE(SetupSession::canAcceptClient(0));
  TEST_ASSERT_TRUE(SetupSession::canAcceptClient(1));
  TEST_ASSERT_FALSE(SetupSession::canAcceptClient(2));
  TEST_ASSERT_FALSE(SetupSession::canAcceptClient(255));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_a_new_session_is_idle_with_no_time_left);
  RUN_TEST(test_a_session_runs_for_five_minutes);
  RUN_TEST(test_the_limit_is_exactly_five_minutes);
  RUN_TEST(test_an_expired_session_stays_expired_until_it_starts_again);
  RUN_TEST(test_saving_the_settings_finishes_a_running_session);
  RUN_TEST(test_finish_does_nothing_when_no_session_runs);
  RUN_TEST(test_cancel_returns_to_idle);
  RUN_TEST(test_starting_again_gives_the_full_time);
  RUN_TEST(test_the_seconds_left_round_up);
  RUN_TEST(test_the_timer_survives_the_wrap_of_the_millis_counter);
  RUN_TEST(test_at_most_two_clients_may_join);
  return UNITY_END();
}
