#include <unity.h>

#include "pairing-window.h"

using namespace wattlink;

namespace {

int state(const PairingWindow &window) {
  return static_cast<int>(window.state());
}

int press(PairingWindow &window, uint32_t nowMs) {
  return static_cast<int>(window.press(nowMs));
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_the_limits_are_the_documented_ones() {
  TEST_ASSERT_EQUAL_UINT32(60000, kPairingWindowMs);
  TEST_ASSERT_EQUAL_UINT32(30000, kPairingPendingMs);
}

void test_a_new_window_is_closed() {
  PairingWindow window;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Closed), state(window));
  TEST_ASSERT_FALSE(window.isOpen(0));
  TEST_ASSERT_FALSE(window.isPairing(0));
  TEST_ASSERT_EQUAL_UINT32(0, window.secondsLeft(0));
  TEST_ASSERT_FALSE(window.beginPairing(0));
  TEST_ASSERT_FALSE(window.commit(0));
}

void test_a_press_opens_the_window_for_sixty_seconds() {
  PairingWindow window;
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PressResult::Opened), press(window, 1000));
  TEST_ASSERT_TRUE(window.isOpen(1000));
  TEST_ASSERT_EQUAL_UINT32(60, window.secondsLeft(1000));
  TEST_ASSERT_TRUE(window.isOpen(1000 + kPairingWindowMs - 1));
  TEST_ASSERT_FALSE(window.isOpen(1000 + kPairingWindowMs));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Closed), static_cast<int>(window.update(1000 + kPairingWindowMs)));
}

void test_the_seconds_left_round_up() {
  PairingWindow window;
  window.press(0);
  TEST_ASSERT_EQUAL_UINT32(60, window.secondsLeft(0));
  TEST_ASSERT_EQUAL_UINT32(60, window.secondsLeft(1));
  TEST_ASSERT_EQUAL_UINT32(59, window.secondsLeft(1000));
  TEST_ASSERT_EQUAL_UINT32(1, window.secondsLeft(kPairingWindowMs - 1));
  TEST_ASSERT_EQUAL_UINT32(0, window.secondsLeft(kPairingWindowMs));
}

void test_a_short_press_while_open_starts_the_sixty_seconds_again() {
  PairingWindow window;
  window.press(0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PressResult::Restarted), press(window, 40000));
  TEST_ASSERT_TRUE(window.isOpen(40000 + kPairingWindowMs - 1));
  TEST_ASSERT_FALSE(window.isOpen(40000 + kPairingWindowMs));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Open), state(window));
}

void test_a_press_after_the_window_ended_opens_a_new_one() {
  PairingWindow window;
  window.press(0);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PressResult::Opened), press(window, kPairingWindowMs));
  TEST_ASSERT_TRUE(window.isOpen(kPairingWindowMs));
}

void test_a_pairing_starts_only_in_an_open_window() {
  PairingWindow window;
  window.press(0);
  TEST_ASSERT_FALSE(window.beginPairing(kPairingWindowMs));
  window.press(kPairingWindowMs);
  TEST_ASSERT_TRUE(window.beginPairing(kPairingWindowMs + 5000));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Pairing), state(window));
  TEST_ASSERT_FALSE(window.isOpen(kPairingWindowMs + 5000));
  TEST_ASSERT_TRUE(window.isPairing(kPairingWindowMs + 5000));
}

void test_only_one_node_pairs_at_a_time() {
  PairingWindow window;
  window.press(0);
  TEST_ASSERT_TRUE(window.beginPairing(1000));
  TEST_ASSERT_FALSE(window.beginPairing(1500));
  TEST_ASSERT_TRUE(window.isPairing(1500));
}

void test_a_press_does_not_disturb_a_pairing_in_progress() {
  PairingWindow window;
  window.press(0);
  window.beginPairing(1000);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PressResult::Ignored), press(window, 2000));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Pairing), state(window));
  // The end time is not moved either.
  TEST_ASSERT_TRUE(window.isPairing(1000 + kPairingPendingMs - 1));
  TEST_ASSERT_FALSE(window.isPairing(1000 + kPairingPendingMs));
}

void test_the_first_telemetry_commits_and_closes_the_window() {
  PairingWindow window;
  window.press(0);
  window.beginPairing(1000);
  TEST_ASSERT_TRUE(window.commit(3000));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Closed), state(window));
  TEST_ASSERT_FALSE(window.isOpen(3000));
  TEST_ASSERT_FALSE(window.beginPairing(3000));
  // A repeat of the first frame does not commit a second time.
  TEST_ASSERT_FALSE(window.commit(4000));
}

void test_a_pairing_that_is_not_proven_in_time_commits_nothing() {
  PairingWindow window;
  window.press(0);
  window.beginPairing(1000);
  TEST_ASSERT_FALSE(window.commit(1000 + kPairingPendingMs));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Closed), static_cast<int>(window.update(1000 + kPairingPendingMs)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PressResult::Opened), press(window, 1000 + kPairingPendingMs));
}

void test_a_commit_without_a_pairing_in_progress_is_refused() {
  PairingWindow window;
  window.press(0);
  TEST_ASSERT_FALSE(window.commit(100));
  TEST_ASSERT_TRUE(window.isOpen(100));
}

void test_closing_ends_every_state() {
  PairingWindow window;
  window.press(0);
  window.close();
  TEST_ASSERT_FALSE(window.isOpen(1));
  window.press(2);
  window.beginPairing(3);
  window.close();
  TEST_ASSERT_FALSE(window.isPairing(4));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Closed), state(window));
}

void test_the_timing_runs_across_the_wrap_of_the_millis_counter() {
  PairingWindow window;
  const uint32_t start = 0xFFFFFFFFu - 10000u;
  window.press(start);
  TEST_ASSERT_TRUE(window.isOpen(start + (kPairingWindowMs - 1)));
  TEST_ASSERT_FALSE(window.isOpen(start + kPairingWindowMs));
  TEST_ASSERT_EQUAL_UINT32(60, window.secondsLeft(start));
  TEST_ASSERT_EQUAL_UINT32(1, window.secondsLeft(start + (kPairingWindowMs - 1)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PressResult::Restarted), press(window, start));
  TEST_ASSERT_TRUE(window.beginPairing(start + 20000u));
  TEST_ASSERT_TRUE(window.commit(start + 20000u + 1000u));
}

void test_a_pairing_never_outlasts_the_window() {
  PairingWindow window;
  window.press(0);
  TEST_ASSERT_TRUE(window.beginPairing(kPairingWindowMs - 1));
  TEST_ASSERT_TRUE(window.isPairing(kPairingWindowMs - 1));
  TEST_ASSERT_FALSE(window.isPairing(kPairingWindowMs));
  TEST_ASSERT_FALSE(window.commit(kPairingWindowMs));
}

void test_a_pairing_early_in_the_window_gets_the_full_pending_time() {
  PairingWindow window;
  window.press(0);
  TEST_ASSERT_TRUE(window.beginPairing(10000));
  TEST_ASSERT_TRUE(window.isPairing(10000 + kPairingPendingMs - 1));
  TEST_ASSERT_TRUE(window.commit(10000 + kPairingPendingMs - 1));
}

void test_the_pending_time_is_capped_at_the_window_end_across_the_wrap() {
  PairingWindow window;
  const uint32_t start = 0xFFFFFFFFu - 10000u;
  window.press(start);
  // The window ends 60000 ms after the start, which is after the wrap, and this request comes 5000 ms before that.
  const uint32_t request = start + (kPairingWindowMs - 5000u);
  TEST_ASSERT_TRUE(window.beginPairing(request));
  TEST_ASSERT_TRUE(window.isPairing(start + (kPairingWindowMs - 1u)));
  TEST_ASSERT_FALSE(window.isPairing(start + kPairingWindowMs));
}

void test_the_pending_time_crosses_the_wrap() {
  PairingWindow window;
  const uint32_t start = 0xFFFFFFFFu - 2000u;
  window.press(start);
  TEST_ASSERT_TRUE(window.beginPairing(start));
  TEST_ASSERT_TRUE(window.isPairing(start + (kPairingPendingMs - 1u)));
  TEST_ASSERT_TRUE(window.commit(start + (kPairingPendingMs - 1u)));
}

void test_no_seconds_are_left_while_a_pairing_is_in_progress() {
  PairingWindow window;
  window.press(0);
  window.beginPairing(1000);
  TEST_ASSERT_EQUAL_UINT32(0, window.secondsLeft(2000));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Pairing), static_cast<int>(window.update(1000 + kPairingPendingMs - 1)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(WindowState::Closed), static_cast<int>(window.update(1000 + kPairingPendingMs)));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_the_limits_are_the_documented_ones);
  RUN_TEST(test_a_new_window_is_closed);
  RUN_TEST(test_a_press_opens_the_window_for_sixty_seconds);
  RUN_TEST(test_the_seconds_left_round_up);
  RUN_TEST(test_a_short_press_while_open_starts_the_sixty_seconds_again);
  RUN_TEST(test_a_press_after_the_window_ended_opens_a_new_one);
  RUN_TEST(test_a_pairing_starts_only_in_an_open_window);
  RUN_TEST(test_only_one_node_pairs_at_a_time);
  RUN_TEST(test_a_press_does_not_disturb_a_pairing_in_progress);
  RUN_TEST(test_the_first_telemetry_commits_and_closes_the_window);
  RUN_TEST(test_a_pairing_that_is_not_proven_in_time_commits_nothing);
  RUN_TEST(test_a_commit_without_a_pairing_in_progress_is_refused);
  RUN_TEST(test_closing_ends_every_state);
  RUN_TEST(test_the_timing_runs_across_the_wrap_of_the_millis_counter);
  RUN_TEST(test_a_pairing_never_outlasts_the_window);
  RUN_TEST(test_a_pairing_early_in_the_window_gets_the_full_pending_time);
  RUN_TEST(test_the_pending_time_is_capped_at_the_window_end_across_the_wrap);
  RUN_TEST(test_the_pending_time_crosses_the_wrap);
  RUN_TEST(test_no_seconds_are_left_while_a_pairing_is_in_progress);
  return UNITY_END();
}
