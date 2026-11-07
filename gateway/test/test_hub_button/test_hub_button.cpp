#include <unity.h>

#include "hub-button.h"

using wattlink::ButtonClassifier;
using wattlink::ButtonEvent;

namespace {

struct Counts {
  int pairing = 0;
  int portal = 0;
  int portalDuringHold = 0;
};

void tally(ButtonEvent event, Counts &counts) {
  if (event == ButtonEvent::Pairing) {
    counts.pairing++;
  } else if (event == ButtonEvent::Portal) {
    counts.portal++;
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
  }
  tally(button.update(false, startMs + heldMs), counts);
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

void test_hold_starts_the_portal_while_held() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 1000, 3500);
  TEST_ASSERT_EQUAL_INT(1, counts.portal);
  TEST_ASSERT_EQUAL_INT(1, counts.portalDuringHold);
}

void test_long_hold_never_opens_pairing() {
  ButtonClassifier button = started();
  const Counts counts = press(button, 1000, 8000);
  TEST_ASSERT_EQUAL_INT(0, counts.pairing);
  TEST_ASSERT_EQUAL_INT(1, counts.portal);
}

void test_hold_seen_only_on_release_counts_as_the_portal() {
  ButtonClassifier button = started();
  button.update(true, 1000);
  TEST_ASSERT_EQUAL(ButtonEvent::Portal, button.update(false, 5000));
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

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_short_press_opens_pairing_on_release);
  RUN_TEST(test_press_under_50_ms_is_bounce);
  RUN_TEST(test_press_of_exactly_50_ms_is_a_short_press);
  RUN_TEST(test_press_just_under_3_seconds_is_still_a_short_press);
  RUN_TEST(test_hold_starts_the_portal_while_held);
  RUN_TEST(test_long_hold_never_opens_pairing);
  RUN_TEST(test_hold_seen_only_on_release_counts_as_the_portal);
  RUN_TEST(test_button_down_at_start_up_is_ignored_until_released);
  RUN_TEST(test_two_presses_in_a_row_each_give_one_event);
  RUN_TEST(test_press_across_the_wrap_of_the_millisecond_counter);
  return UNITY_END();
}
