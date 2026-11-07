#include <unity.h>

#include "link-supervisor.h"

using namespace wattlink;

namespace {

int state(const LinkSupervisor &link) {
  return static_cast<int>(link.state());
}

int action(LinkAction value) {
  return static_cast<int>(value);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_the_wait_doubles_from_two_seconds_up_to_five_minutes() {
  Backoff backoff;
  const uint32_t expected[] = {2000, 4000, 8000, 16000, 32000, 64000, 128000, 256000, 300000, 300000, 300000};
  for (uint32_t value : expected) {
    TEST_ASSERT_EQUAL_UINT32(value, backoff.next());
  }
}

void test_the_wait_starts_again_after_a_reset() {
  Backoff backoff;
  backoff.next();
  backoff.next();
  backoff.reset();
  TEST_ASSERT_EQUAL_UINT32(kBackoffBaseMs, backoff.next());
}

void test_the_limits_are_the_documented_ones() {
  TEST_ASSERT_EQUAL_UINT32(2000, kBackoffBaseMs);
  TEST_ASSERT_EQUAL_UINT32(300000, kBackoffCapMs);
  TEST_ASSERT_EQUAL_UINT32(15000, kConnectTimeoutMs);
}

void test_a_link_that_is_not_enabled_does_nothing() {
  LinkSupervisor link;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Idle), state(link));
  TEST_ASSERT_EQUAL_INT(action(LinkAction::None), action(link.update(1000, false)));
  TEST_ASSERT_EQUAL_INT(action(LinkAction::None), action(link.update(999999, true)));
  TEST_ASSERT_EQUAL_UINT32(0, link.secondsToRetry(0));
}

void test_enabling_starts_a_try_at_the_next_update() {
  LinkSupervisor link;
  link.enable(5000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Waiting), state(link));
  TEST_ASSERT_EQUAL_INT(action(LinkAction::StartConnect), action(link.update(5000, false)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Connecting), state(link));
  TEST_ASSERT_EQUAL_INT(action(LinkAction::None), action(link.update(5001, false)));
}

void test_a_try_that_joins_makes_the_link_connected() {
  LinkSupervisor link;
  link.enable(0);
  link.update(0, false);
  TEST_ASSERT_EQUAL_INT(action(LinkAction::None), action(link.update(3000, true)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Connected), state(link));
  TEST_ASSERT_EQUAL_INT(action(LinkAction::None), action(link.update(4000, true)));
}

void test_a_try_that_runs_too_long_is_aborted_and_waits_two_seconds() {
  LinkSupervisor link;
  link.enable(0);
  link.update(0, false);
  TEST_ASSERT_EQUAL_INT(action(LinkAction::None), action(link.update(kConnectTimeoutMs - 1, false)));
  TEST_ASSERT_EQUAL_INT(action(LinkAction::Abort), action(link.update(kConnectTimeoutMs, false)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Waiting), state(link));
  TEST_ASSERT_EQUAL_UINT32(2, link.secondsToRetry(kConnectTimeoutMs));
  TEST_ASSERT_EQUAL_INT(action(LinkAction::None), action(link.update(kConnectTimeoutMs + 1999, false)));
  TEST_ASSERT_EQUAL_INT(action(LinkAction::StartConnect), action(link.update(kConnectTimeoutMs + 2000, false)));
}

void test_each_failed_try_waits_longer() {
  LinkSupervisor link;
  link.enable(0);
  uint32_t now = 0;
  const uint32_t waits[] = {2000, 4000, 8000, 16000};
  for (uint32_t wait : waits) {
    TEST_ASSERT_EQUAL_INT(action(LinkAction::StartConnect), action(link.update(now, false)));
    now += kConnectTimeoutMs;
    TEST_ASSERT_EQUAL_INT(action(LinkAction::Abort), action(link.update(now, false)));
    TEST_ASSERT_EQUAL_UINT32(wait / 1000, link.secondsToRetry(now));
    now += wait;
  }
  TEST_ASSERT_EQUAL_INT(action(LinkAction::StartConnect), action(link.update(now, false)));
}

void test_a_lost_link_is_joined_again_after_a_short_wait_and_the_wait_starts_over() {
  LinkSupervisor link;
  link.enable(0);
  link.update(0, false);
  // Two failed tries first, so the wait has grown to 8 seconds.
  link.update(kConnectTimeoutMs, false);
  link.update(kConnectTimeoutMs + 2000, false);
  link.update(2 * kConnectTimeoutMs + 2000, false);
  const uint32_t startThird = 2 * kConnectTimeoutMs + 2000 + 4000;
  TEST_ASSERT_EQUAL_INT(action(LinkAction::StartConnect), action(link.update(startThird, false)));
  link.update(startThird + 1000, true);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Connected), state(link));

  // The link drops. The success reset the wait, so the first wait is 2 seconds again.
  const uint32_t lostAt = startThird + 60000;
  TEST_ASSERT_EQUAL_INT(action(LinkAction::None), action(link.update(lostAt, false)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Waiting), state(link));
  TEST_ASSERT_EQUAL_UINT32(2, link.secondsToRetry(lostAt));
  TEST_ASSERT_EQUAL_INT(action(LinkAction::StartConnect), action(link.update(lostAt + 2000, false)));
}

void test_the_seconds_to_retry_round_up_and_reach_zero() {
  LinkSupervisor link;
  link.enable(0);
  link.update(0, false);
  link.update(kConnectTimeoutMs, false);
  TEST_ASSERT_EQUAL_UINT32(2, link.secondsToRetry(kConnectTimeoutMs));
  TEST_ASSERT_EQUAL_UINT32(2, link.secondsToRetry(kConnectTimeoutMs + 1));
  TEST_ASSERT_EQUAL_UINT32(1, link.secondsToRetry(kConnectTimeoutMs + 1001));
  TEST_ASSERT_EQUAL_UINT32(1, link.secondsToRetry(kConnectTimeoutMs + 1999));
  TEST_ASSERT_EQUAL_UINT32(0, link.secondsToRetry(kConnectTimeoutMs + 2000));
  TEST_ASSERT_EQUAL_UINT32(0, link.secondsToRetry(kConnectTimeoutMs + 50000));
}

void test_disabling_stops_everything_and_enabling_again_starts_over() {
  LinkSupervisor link;
  link.enable(0);
  link.update(0, false);
  link.update(kConnectTimeoutMs, false);
  link.disable();
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Idle), state(link));
  TEST_ASSERT_EQUAL_INT(action(LinkAction::None), action(link.update(kConnectTimeoutMs + 100000, false)));
  link.enable(200000);
  TEST_ASSERT_EQUAL_INT(action(LinkAction::StartConnect), action(link.update(200000, false)));
  link.update(200000 + kConnectTimeoutMs, false);
  TEST_ASSERT_EQUAL_UINT32(2, link.secondsToRetry(200000 + kConnectTimeoutMs));
}

void test_enabling_a_running_link_does_not_restart_it() {
  LinkSupervisor link;
  link.enable(0);
  link.update(0, false);
  link.update(1000, true);
  link.enable(2000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(LinkState::Connected), state(link));
}

void test_the_timing_runs_across_the_wrap_of_the_millis_counter() {
  LinkSupervisor link;
  const uint32_t start = 0xFFFFFFFFu - 5000u;
  link.enable(start);
  TEST_ASSERT_EQUAL_INT(action(LinkAction::StartConnect), action(link.update(start, false)));
  TEST_ASSERT_EQUAL_INT(action(LinkAction::None), action(link.update(start + (kConnectTimeoutMs - 1), false)));
  TEST_ASSERT_EQUAL_INT(action(LinkAction::Abort), action(link.update(start + kConnectTimeoutMs, false)));
  const uint32_t afterAbort = start + kConnectTimeoutMs;
  TEST_ASSERT_EQUAL_UINT32(2, link.secondsToRetry(afterAbort));
  TEST_ASSERT_EQUAL_INT(action(LinkAction::StartConnect), action(link.update(afterAbort + 2000, false)));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_the_wait_doubles_from_two_seconds_up_to_five_minutes);
  RUN_TEST(test_the_wait_starts_again_after_a_reset);
  RUN_TEST(test_the_limits_are_the_documented_ones);
  RUN_TEST(test_a_link_that_is_not_enabled_does_nothing);
  RUN_TEST(test_enabling_starts_a_try_at_the_next_update);
  RUN_TEST(test_a_try_that_joins_makes_the_link_connected);
  RUN_TEST(test_a_try_that_runs_too_long_is_aborted_and_waits_two_seconds);
  RUN_TEST(test_each_failed_try_waits_longer);
  RUN_TEST(test_a_lost_link_is_joined_again_after_a_short_wait_and_the_wait_starts_over);
  RUN_TEST(test_the_seconds_to_retry_round_up_and_reach_zero);
  RUN_TEST(test_disabling_stops_everything_and_enabling_again_starts_over);
  RUN_TEST(test_enabling_a_running_link_does_not_restart_it);
  RUN_TEST(test_the_timing_runs_across_the_wrap_of_the_millis_counter);
  return UNITY_END();
}
