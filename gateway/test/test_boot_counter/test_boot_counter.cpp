#include <unity.h>

#include "boot-counter.h"

using namespace wattlink;

namespace {

// A store in memory. Each fault can be set on its own.
class FakeStore : public CounterStore {
 public:
  CounterRead read(uint32_t *out) override {
    reads++;
    if (failReads > 0) {
      failReads--;
      return CounterRead::Failed;
    }
    if (!present) {
      return CounterRead::Missing;
    }
    *out = lieAfterWrite && writes > 0 ? stale : value;
    return CounterRead::Ok;
  }
  bool write(uint32_t in) override {
    writes++;
    if (failWrite) {
      return false;
    }
    stale = value;
    value = in;
    present = true;
    return true;
  }

  uint32_t value = 0;
  uint32_t stale = 0;
  bool present = false;
  int failReads = 0;  // the next reads fail
  bool failWrite = false;
  bool lieAfterWrite = false;  // a read after a write gives the old value, as a flash that lost the write would
  int reads = 0;
  int writes = 0;
};

int result(BootResult value) {
  return static_cast<int>(value);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_the_first_start_gives_1_and_is_marked_as_first() {
  FakeStore store;
  BootCounter counter(store);
  uint32_t id = 99;
  TEST_ASSERT_EQUAL_INT(result(BootResult::First), result(counter.next(&id)));
  TEST_ASSERT_EQUAL_UINT32(1, id);
  TEST_ASSERT_TRUE(store.present);
  TEST_ASSERT_EQUAL_UINT32(1, store.value);
  TEST_ASSERT_EQUAL_INT(1, store.writes);
}

void test_every_start_gives_one_more_and_stores_it() {
  FakeStore store;
  uint32_t id = 0;
  for (uint32_t expected = 1; expected <= 5; expected++) {
    BootCounter counter(store);  // a new object for each start, as after a reset
    const BootResult got = counter.next(&id);
    TEST_ASSERT_EQUAL_INT(result(expected == 1 ? BootResult::First : BootResult::Ok), result(got));
    TEST_ASSERT_EQUAL_UINT32(expected, id);
    TEST_ASSERT_EQUAL_UINT32(expected, store.value);
  }
}

void test_the_number_is_continued_from_what_the_flash_holds() {
  FakeStore store;
  store.present = true;
  store.value = 41;
  BootCounter counter(store);
  uint32_t id = 0;
  TEST_ASSERT_EQUAL_INT(result(BootResult::Ok), result(counter.next(&id)));
  TEST_ASSERT_EQUAL_UINT32(42, id);
}

void test_a_read_that_fails_once_is_tried_again() {
  FakeStore store;
  store.present = true;
  store.value = 7;
  store.failReads = 1;
  BootCounter counter(store);
  uint32_t id = 0;
  TEST_ASSERT_EQUAL_INT(result(BootResult::Ok), result(counter.next(&id)));
  TEST_ASSERT_EQUAL_UINT32(8, id);
}

void test_a_read_that_fails_twice_gives_no_number_and_writes_nothing() {
  FakeStore store;
  store.present = true;
  store.value = 7;
  store.failReads = 2;
  BootCounter counter(store);
  uint32_t id = 99;
  TEST_ASSERT_EQUAL_INT(result(BootResult::Failed), result(counter.next(&id)));
  TEST_ASSERT_EQUAL_UINT32(0, id);
  // A flash that cannot be read must not look like a first start, so nothing is written and no number is repeated.
  TEST_ASSERT_EQUAL_INT(0, store.writes);
  TEST_ASSERT_EQUAL_UINT32(7, store.value);
}

void test_a_write_that_fails_gives_no_number() {
  FakeStore store;
  store.failWrite = true;
  BootCounter counter(store);
  uint32_t id = 99;
  TEST_ASSERT_EQUAL_INT(result(BootResult::Failed), result(counter.next(&id)));
  TEST_ASSERT_EQUAL_UINT32(0, id);
}

void test_a_number_that_the_flash_does_not_give_back_is_not_used() {
  FakeStore store;
  store.present = true;
  store.value = 5;
  store.lieAfterWrite = true;
  BootCounter counter(store);
  uint32_t id = 99;
  TEST_ASSERT_EQUAL_INT(result(BootResult::Failed), result(counter.next(&id)));
  TEST_ASSERT_EQUAL_UINT32(0, id);
}

void test_a_read_back_that_fails_gives_no_number() {
  FakeStore store;
  store.present = true;
  store.value = 5;
  uint32_t id = 99;
  // The first read works, the write works, and the read back fails.
  struct ReadBackFails : CounterStore {
    explicit ReadBackFails(FakeStore &inner) : inner_(inner) {}
    CounterRead read(uint32_t *out) override {
      return inner_.writes > 0 ? CounterRead::Failed : inner_.read(out);
    }
    bool write(uint32_t in) override { return inner_.write(in); }
    FakeStore &inner_;
  } wrapper(store);
  BootCounter wrapped(wrapper);
  TEST_ASSERT_EQUAL_INT(result(BootResult::Failed), result(wrapped.next(&id)));
  TEST_ASSERT_EQUAL_UINT32(0, id);
}

void test_the_highest_number_is_given_once_and_the_next_start_fails_without_a_write() {
  FakeStore store;
  store.present = true;
  store.value = kMaxBootId - 1;
  BootCounter counter(store);
  uint32_t id = 0;
  TEST_ASSERT_EQUAL_INT(result(BootResult::Ok), result(counter.next(&id)));
  TEST_ASSERT_EQUAL_UINT32(kMaxBootId, id);
  const int writes = store.writes;
  TEST_ASSERT_EQUAL_INT(result(BootResult::Failed), result(counter.next(&id)));
  TEST_ASSERT_EQUAL_UINT32(0, id);
  TEST_ASSERT_EQUAL_INT(writes, store.writes);
}

void test_a_stored_number_above_the_limit_fails_and_is_not_wrapped() {
  FakeStore store;
  store.present = true;
  store.value = 0xFFFFFFFFu;
  BootCounter counter(store);
  uint32_t id = 99;
  TEST_ASSERT_EQUAL_INT(result(BootResult::Failed), result(counter.next(&id)));
  TEST_ASSERT_EQUAL_UINT32(0, id);
  TEST_ASSERT_EQUAL_INT(0, store.writes);
}

void test_the_number_is_never_0() {
  FakeStore store;
  BootCounter counter(store);
  uint32_t id = 0;
  counter.next(&id);
  TEST_ASSERT_TRUE(id != 0);
  // A stored 0 is the same as nothing, and the next number is 1.
  FakeStore zero;
  zero.present = true;
  zero.value = 0;
  BootCounter again(zero);
  again.next(&id);
  TEST_ASSERT_EQUAL_UINT32(1, id);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_the_first_start_gives_1_and_is_marked_as_first);
  RUN_TEST(test_every_start_gives_one_more_and_stores_it);
  RUN_TEST(test_the_number_is_continued_from_what_the_flash_holds);
  RUN_TEST(test_a_read_that_fails_once_is_tried_again);
  RUN_TEST(test_a_read_that_fails_twice_gives_no_number_and_writes_nothing);
  RUN_TEST(test_a_write_that_fails_gives_no_number);
  RUN_TEST(test_a_number_that_the_flash_does_not_give_back_is_not_used);
  RUN_TEST(test_a_read_back_that_fails_gives_no_number);
  RUN_TEST(test_the_highest_number_is_given_once_and_the_next_start_fails_without_a_write);
  RUN_TEST(test_a_stored_number_above_the_limit_fails_and_is_not_wrapped);
  RUN_TEST(test_the_number_is_never_0);
  return UNITY_END();
}
