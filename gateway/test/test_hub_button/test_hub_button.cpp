#include <unity.h>

#include "hub-button.h"

using wattlink::ButtonClassifier;
using wattlink::ButtonEvent;
using wattlink::kBounceMs;
using wattlink::kNewCodeHoldMs;

namespace {

struct Counts {
  int pairing = 0;
  int portal = 0;
  int portalDuringHold = 0;
  int newCode = 0;
  int newCodeDuringHold = 0;
};

void tally(ButtonEvent event, Counts &counts) {
  if (event == ButtonEvent::Pairing) {
    counts.pairing++;
  } else if (event == ButtonEvent::Portal) {
    counts.portal++;
  } else if (event == ButtonEvent::NewCode) {
    counts.newCode++;
  }
}

// Reads the button every 10 ms while it is held for heldMs, then reads it released once.
Counts press(ButtonClassifier &button, uint32_t startMs, uint32_t heldMs) {
  Counts counts;
  for (uint32_t elapsed = 0; elapsed < heldMs; elapsed += 10) {
    const ButtonEvent event = button.update(true, startMs + elapsed);
    tally(event, counts);
    if (event == ButtonEvent::Portal) {
      counts.portalDuringHold++;
    }
    if (event == ButtonEvent::NewCode) {
      counts.newCodeDuringHold++;
    }
  }
  // The release counts when the readings stay released for kBounceMs, so a second reading follows.
  tally(button.update(false, startMs + heldMs), counts);
  tally(button.update(false, startMs + heldMs + kBounceMs), counts);
  return counts;
}

ButtonClassifier started() {
  ButtonClassifier button;
  button.update(false, 0);
  return button;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_short_press_opens_pairing_on_release() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 1000, 200);
  TEST_ASSERT_EQUAL_INT(1, counts.pairing);
  TEST_ASSERT_EQUAL_INT(0, counts.portal);
}

void test_press_under_50_ms_is_bounce() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 1000, 40);
  TEST_ASSERT_EQUAL_INT(0, counts.pairing);
  TEST_ASSERT_EQUAL_INT(0, counts.portal);
}

void test_press_of_exactly_50_ms_is_a_short_press() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 1000, 50);
  TEST_ASSERT_EQUAL_INT(1, counts.pairing);
}

void test_press_just_under_3_seconds_is_still_a_short_press() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 1000, 2990);
  TEST_ASSERT_EQUAL_INT(1, counts.pairing);
  TEST_ASSERT_EQUAL_INT(0, counts.portal);
}

void test_hold_of_3_seconds_or_more_starts_the_portal_on_release_and_not_while_held() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 1000, 3500);
  TEST_ASSERT_EQUAL_INT(1, counts.portal);
  TEST_ASSERT_EQUAL_INT(0, counts.portalDuringHold);
  TEST_ASSERT_EQUAL_INT(0, counts.newCode);
}

void test_long_hold_never_opens_pairing() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 1000, 8000);
  TEST_ASSERT_EQUAL_INT(0, counts.pairing);
  TEST_ASSERT_EQUAL_INT(1, counts.portal);
  TEST_ASSERT_EQUAL_INT(0, counts.newCode);
}

void test_hold_seen_only_on_release_counts_as_the_portal() {
  ButtonClassifier button = started();
  button.update(true, 1000);
  TEST_ASSERT_EQUAL(ButtonEvent::None, button.update(false, 5000));
  TEST_ASSERT_EQUAL(ButtonEvent::Portal, button.update(false, 5000 + kBounceMs));
}

void test_button_down_at_start_up_is_ignored_until_released() {
  ButtonClassifier button;
  Counts counts;
  for (uint32_t t = 0; t < 4000; t += 10) {
    tally(button.update(true, t), counts);
  }
  tally(button.update(false, 4000), counts);
  TEST_ASSERT_EQUAL_INT(0, counts.pairing);
  TEST_ASSERT_EQUAL_INT(0, counts.portal);

  const Counts next = press(button, 5000, 200);
  TEST_ASSERT_EQUAL_INT(1, next.pairing);
}

void test_two_presses_in_a_row_each_give_one_event() {
  ButtonClassifier button = started();
  const Counts first = press(button, 1000, 200);
  const Counts second = press(button, 3000, 200);
  TEST_ASSERT_EQUAL_INT(1, first.pairing);
  TEST_ASSERT_EQUAL_INT(1, second.pairing);
}

void test_press_across_the_wrap_of_the_millisecond_counter() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 0xFFFFFFFFu - 100, 200);
  TEST_ASSERT_EQUAL_INT(1, counts.pairing);
}

void test_press_of_49_ms_is_bounce() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 1000, 49);
  TEST_ASSERT_EQUAL_INT(0, counts.pairing);
  TEST_ASSERT_EQUAL_INT(0, counts.portal);
}

void test_press_of_2999_ms_is_a_short_press() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 1000, 2999);
  TEST_ASSERT_EQUAL_INT(1, counts.pairing);
  TEST_ASSERT_EQUAL_INT(0, counts.portal);
}

void test_release_at_exactly_3000_ms_counts_as_the_portal() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 1000, 3000);
  TEST_ASSERT_EQUAL_INT(0, counts.pairing);
  TEST_ASSERT_EQUAL_INT(1, counts.portal);
}

void test_hold_across_the_wrap_starts_the_portal_on_release() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 0xFFFFFFFFu - 1500, 3500);
  TEST_ASSERT_EQUAL_INT(1, counts.portal);
  TEST_ASSERT_EQUAL_INT(0, counts.portalDuringHold);
  TEST_ASSERT_EQUAL_INT(0, counts.pairing);
}

void test_hold_of_10_seconds_asks_for_a_new_code_while_held_and_never_starts_the_portal() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 1000, 12000);
  TEST_ASSERT_EQUAL_INT(1, counts.newCode);
  TEST_ASSERT_EQUAL_INT(1, counts.newCodeDuringHold);
  TEST_ASSERT_EQUAL_INT(0, counts.portal);
  TEST_ASSERT_EQUAL_INT(0, counts.pairing);
}

void test_hold_of_9990_ms_is_still_the_portal_and_exactly_10000_ms_is_the_new_code() {
  ButtonClassifier button = started();
  const Counts below = press(button, 1000, 9990);
  TEST_ASSERT_EQUAL_INT(1, below.portal);
  TEST_ASSERT_EQUAL_INT(0, below.newCode);
  const Counts exact = press(button, 20000, 10000);
  TEST_ASSERT_EQUAL_INT(0, exact.portal);
  TEST_ASSERT_EQUAL_INT(1, exact.newCode);
}

void test_hold_of_10_seconds_seen_only_on_release_counts_as_the_new_code() {
  ButtonClassifier button = started();
  button.update(true, 1000);
  TEST_ASSERT_EQUAL(ButtonEvent::None, button.update(false, 15000));
  TEST_ASSERT_EQUAL(ButtonEvent::NewCode, button.update(false, 15000 + kBounceMs));
}

void test_the_new_code_comes_once_and_the_next_press_is_normal() {
  ButtonClassifier button = started();
  const Counts hold = press(button, 1000, 15000);
  const Counts next = press(button, 20000, 200);
  TEST_ASSERT_EQUAL_INT(1, hold.newCode);
  TEST_ASSERT_EQUAL_INT(1, next.pairing);
  TEST_ASSERT_EQUAL_INT(0, next.newCode);
}

void test_hold_of_10_seconds_across_the_wrap_asks_for_a_new_code() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 0xFFFFFFFFu - 4000, 11000);
  TEST_ASSERT_EQUAL_INT(1, counts.newCodeDuringHold);
  TEST_ASSERT_EQUAL_INT(0, counts.portal);
}

// The contact of the button may open for a moment while it is held. A reading released that is followed by a reading pressed within kBounceMs is a glitch.
void test_a_glitch_in_the_middle_of_a_hold_does_not_end_it_and_the_new_code_still_comes_at_10_seconds() {
  const uint32_t glitchTimes[] = {5000, 9000, 9990};
  for (uint32_t glitchAt : glitchTimes) {
    ButtonClassifier button = started();
    Counts counts;
    for (uint32_t elapsed = 0; elapsed < 12000; elapsed += 10) {
      const uint32_t t = 1000 + elapsed;
      const bool glitch = elapsed == glitchAt;
      const ButtonEvent event = button.update(!glitch, t);
      tally(event, counts);
      if (event == ButtonEvent::NewCode) {
        counts.newCodeDuringHold++;
      }
    }
    tally(button.update(false, 13000), counts);
    tally(button.update(false, 13000 + kBounceMs), counts);
    TEST_ASSERT_EQUAL_INT(1, counts.newCode);
    TEST_ASSERT_EQUAL_INT(1, counts.newCodeDuringHold);
    TEST_ASSERT_EQUAL_INT(0, counts.portal);
    TEST_ASSERT_EQUAL_INT(0, counts.pairing);
  }
}

void test_a_glitch_after_the_event_does_not_start_a_new_press_that_ends_as_pairing() {
  ButtonClassifier button = started();
  Counts counts;
  for (uint32_t elapsed = 0; elapsed < 10100; elapsed += 10) {
    tally(button.update(true, 1000 + elapsed), counts);
  }
  TEST_ASSERT_EQUAL_INT(1, counts.newCode);
  // One reading released, and the button is pressed again, still the same hold.
  tally(button.update(false, 11200), counts);
  for (uint32_t elapsed = 0; elapsed < 800; elapsed += 10) {
    tally(button.update(true, 11210 + elapsed), counts);
  }
  tally(button.update(false, 12100), counts);
  tally(button.update(false, 12100 + kBounceMs), counts);
  TEST_ASSERT_EQUAL_INT(1, counts.newCode);
  TEST_ASSERT_EQUAL_INT(0, counts.pairing);
  TEST_ASSERT_EQUAL_INT(0, counts.portal);
  // The next press is a normal press.
  const Counts next = press(button, 20000, 200);
  TEST_ASSERT_EQUAL_INT(1, next.pairing);
}

void test_the_release_is_confirmed_after_exactly_50_ms_of_released_readings() {
  ButtonClassifier button = started();
  button.update(true, 1000);
  TEST_ASSERT_EQUAL(ButtonEvent::None, button.update(true, 1200));
  TEST_ASSERT_EQUAL(ButtonEvent::None, button.update(false, 1200));
  TEST_ASSERT_EQUAL(ButtonEvent::None, button.update(false, 1200 + kBounceMs - 1));
  TEST_ASSERT_EQUAL(ButtonEvent::Pairing, button.update(false, 1200 + kBounceMs));
  // The length of the hold is counted to the first released reading, so a press of 200 ms is a short press.
}

void test_a_button_held_down_from_the_start_never_gives_the_new_code_without_a_release() {
  ButtonClassifier button;
  Counts counts;
  for (uint32_t t = 0; t < 12000; t += 10) {
    tally(button.update(true, t), counts);
  }
  TEST_ASSERT_EQUAL_INT(0, counts.newCode);
  TEST_ASSERT_EQUAL_INT(0, counts.portal);
  TEST_ASSERT_EQUAL_INT(0, counts.pairing);
}

void test_the_time_held_is_given_for_the_display_and_is_zero_when_the_button_is_not_held() {
  ButtonClassifier button = started();
  TEST_ASSERT_EQUAL_UINT32(0, button.heldMs(500));
  button.update(true, 1000);
  TEST_ASSERT_EQUAL_UINT32(0, button.heldMs(1000));
  TEST_ASSERT_EQUAL_UINT32(2500, button.heldMs(3500));
  button.update(false, 3500);
  TEST_ASSERT_EQUAL_UINT32(0, button.heldMs(3600));
  // Across the wrap, and zero after an event that came while the button was held.
  ButtonClassifier other = started();
  other.update(true, 0xFFFFFFFFu - 100);
  TEST_ASSERT_EQUAL_UINT32(400, other.heldMs(299));
  // A time that is older than the last reading gives 0 and not a huge number.
  TEST_ASSERT_EQUAL_UINT32(0, other.heldMs(0xFFFFFFFFu - 200));
  other.update(true, 0xFFFFFFFFu - 100 + kNewCodeHoldMs);
  TEST_ASSERT_EQUAL_UINT32(0, other.heldMs(0xFFFFFFFFu - 100 + kNewCodeHoldMs + 5));
}

void test_short_press_after_a_portal_hold_gives_pairing() {
  ButtonClassifier button = started();
  const Counts hold = press(button, 1000, 4000);
  const Counts next = press(button, 6000, 200);
  TEST_ASSERT_EQUAL_INT(1, hold.portal);
  TEST_ASSERT_EQUAL_INT(1, next.pairing);
  TEST_ASSERT_EQUAL_INT(0, next.portal);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_short_press_opens_pairing_on_release);
  RUN_TEST(test_press_under_50_ms_is_bounce);
  RUN_TEST(test_press_of_exactly_50_ms_is_a_short_press);
  RUN_TEST(test_press_just_under_3_seconds_is_still_a_short_press);
  RUN_TEST(test_hold_of_3_seconds_or_more_starts_the_portal_on_release_and_not_while_held);
  RUN_TEST(test_long_hold_never_opens_pairing);
  RUN_TEST(test_hold_seen_only_on_release_counts_as_the_portal);
  RUN_TEST(test_button_down_at_start_up_is_ignored_until_released);
  RUN_TEST(test_two_presses_in_a_row_each_give_one_event);
  RUN_TEST(test_press_across_the_wrap_of_the_millisecond_counter);
  RUN_TEST(test_press_of_49_ms_is_bounce);
  RUN_TEST(test_press_of_2999_ms_is_a_short_press);
  RUN_TEST(test_release_at_exactly_3000_ms_counts_as_the_portal);
  RUN_TEST(test_hold_across_the_wrap_starts_the_portal_on_release);
  RUN_TEST(test_hold_of_10_seconds_asks_for_a_new_code_while_held_and_never_starts_the_portal);
  RUN_TEST(test_hold_of_9990_ms_is_still_the_portal_and_exactly_10000_ms_is_the_new_code);
  RUN_TEST(test_hold_of_10_seconds_seen_only_on_release_counts_as_the_new_code);
  RUN_TEST(test_the_new_code_comes_once_and_the_next_press_is_normal);
  RUN_TEST(test_hold_of_10_seconds_across_the_wrap_asks_for_a_new_code);
  RUN_TEST(test_a_glitch_in_the_middle_of_a_hold_does_not_end_it_and_the_new_code_still_comes_at_10_seconds);
  RUN_TEST(test_a_glitch_after_the_event_does_not_start_a_new_press_that_ends_as_pairing);
  RUN_TEST(test_the_release_is_confirmed_after_exactly_50_ms_of_released_readings);
  RUN_TEST(test_a_button_held_down_from_the_start_never_gives_the_new_code_without_a_release);
  RUN_TEST(test_the_time_held_is_given_for_the_display_and_is_zero_when_the_button_is_not_held);
  RUN_TEST(test_short_press_after_a_portal_hold_gives_pairing);
  return UNITY_END();
}
