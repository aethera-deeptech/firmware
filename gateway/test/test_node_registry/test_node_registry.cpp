#include <string.h>
#include <unity.h>

#include <initializer_list>

#include "node-registry.h"

using namespace wattlink;

namespace {

// A store in memory. It can be told to fail a write or a read, to test what the registry does with a flash that misbehaves.
class FakeStore : public RecordStore {
 public:
  ReadResult read(size_t slot, uint8_t out[kRecordSize]) override {
    if (failReadSlot == static_cast<int>(slot)) {
      return ReadResult::Failed;
    }
    if (failReadOnceSlot == static_cast<int>(slot)) {
      failReadOnceSlot = -1;
      return ReadResult::Failed;
    }
    if (!present[slot]) {
      return ReadResult::Missing;
    }
    memcpy(out, data[slot], kRecordSize);
    return ReadResult::Ok;
  }
  bool write(size_t slot, const uint8_t in[kRecordSize]) override {
    writes++;
    if (failWrites) {
      return false;
    }
    memcpy(data[slot], in, kRecordSize);
    present[slot] = true;
    return true;
  }

  uint8_t data[kMaxNodes][kRecordSize] = {};
  bool present[kMaxNodes] = {};
  bool failWrites = false;
  int failReadSlot = -1;
  int failReadOnceSlot = -1;
  int writes = 0;
};

const uint8_t kMacA[kMacSize] = {0x40, 0x4c, 0xca, 0x3c, 0x5a, 0xa4};
const uint8_t kMacB[kMacSize] = {0x48, 0xe7, 0x29, 0xb6, 0xe0, 0x84};
const uint8_t kKeyA[kNodeKeySize] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
const uint8_t kKeyB[kNodeKeySize] = {0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF};

int result(PairResult value) {
  return static_cast<int>(value);
}

int check(FrameCheck value) {
  return static_cast<int>(value);
}

void makeMac(uint8_t mac[kMacSize], uint32_t number) {
  mac[0] = 0x40;
  mac[1] = 0x00;
  mac[2] = 0x00;
  mac[3] = static_cast<uint8_t>(number >> 16);
  mac[4] = static_cast<uint8_t>(number >> 8);
  mac[5] = static_cast<uint8_t>(number);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_the_limits_are_the_documented_ones() {
  TEST_ASSERT_EQUAL_UINT(64, kMaxNodes);
  TEST_ASSERT_EQUAL_UINT(16, kNodeKeySize);
  TEST_ASSERT_EQUAL_UINT(29, kRecordSize);
  TEST_ASSERT_EQUAL_UINT8(1, kKindPlug);
  TEST_ASSERT_EQUAL_UINT8(2, kKindClamp);
}

void test_a_new_registry_is_empty() {
  FakeStore store;
  NodeRegistry registry(store);
  TEST_ASSERT_EQUAL_UINT(0, registry.load());
  TEST_ASSERT_EQUAL_UINT(0, registry.pairedCount());
  TEST_ASSERT_EQUAL_UINT(0, registry.idCount());
  TEST_ASSERT_EQUAL_UINT(0, registry.damagedCount());
  TEST_ASSERT_FALSE(registry.isPaired(kMacA));
  TEST_ASSERT_NULL(registry.keyOf(kMacA));
  TEST_ASSERT_EQUAL_UINT8(0, registry.kindOf(kMacA));
}

void test_a_pairing_is_kept_and_found() {
  FakeStore store;
  NodeRegistry registry(store);
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.pair(kKindPlug, kMacA, kKeyA)));
  TEST_ASSERT_TRUE(registry.isPaired(kMacA));
  TEST_ASSERT_EQUAL_UINT8(kKindPlug, registry.kindOf(kMacA));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kKeyA, registry.keyOf(kMacA), kNodeKeySize);
  TEST_ASSERT_EQUAL_UINT(1, registry.pairedCount());
  TEST_ASSERT_EQUAL_UINT(1, registry.idCount());
  TEST_ASSERT_FALSE(registry.isPaired(kMacB));
}

void test_the_id_is_the_kind_and_the_last_three_bytes_of_the_mac() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  registry.pair(kKindClamp, kMacB, kKeyB);
  char id[kNodeIdSize];
  TEST_ASSERT_TRUE(registry.nodeId(kMacA, id));
  TEST_ASSERT_EQUAL_STRING("PLUG-3C5AA4", id);
  TEST_ASSERT_TRUE(registry.nodeId(kMacB, id));
  TEST_ASSERT_EQUAL_STRING("CLAMP-B6E084", id);
  const uint8_t other[kMacSize] = {0x40, 0, 0, 1, 2, 3};
  TEST_ASSERT_FALSE(registry.nodeId(other, id));
  TEST_ASSERT_EQUAL_STRING("", id);
}

void test_the_longest_id_fits_the_buffer() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindClamp, kMacB, kKeyB);
  char id[kNodeIdSize];
  registry.nodeId(kMacB, id);
  TEST_ASSERT_EQUAL_UINT(kNodeIdSize - 1, strlen(id));
}

void test_a_bad_kind_or_mac_is_refused_and_nothing_is_written() {
  FakeStore store;
  NodeRegistry registry(store);
  const uint8_t zero[kMacSize] = {0};
  const uint8_t group[kMacSize] = {0x01, 0x00, 0x5e, 1, 2, 3};
  TEST_ASSERT_EQUAL_INT(result(PairResult::InvalidKind), result(registry.pair(0, kMacA, kKeyA)));
  TEST_ASSERT_EQUAL_INT(result(PairResult::InvalidKind), result(registry.pair(3, kMacA, kKeyA)));
  TEST_ASSERT_EQUAL_INT(result(PairResult::InvalidMac), result(registry.pair(kKindPlug, zero, kKeyA)));
  TEST_ASSERT_EQUAL_INT(result(PairResult::InvalidMac), result(registry.pair(kKindPlug, group, kKeyA)));
  TEST_ASSERT_EQUAL_INT(0, store.writes);
  TEST_ASSERT_EQUAL_UINT(0, registry.idCount());
}

void test_a_node_that_is_paired_cannot_pair_again() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  TEST_ASSERT_EQUAL_INT(result(PairResult::AlreadyPaired), result(registry.pair(kKindPlug, kMacA, kKeyB)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kKeyA, registry.keyOf(kMacA), kNodeKeySize);
  TEST_ASSERT_EQUAL_INT(result(PairResult::AlreadyPaired), result(registry.pair(kKindClamp, kMacA, kKeyB)));
}

void test_the_same_id_for_another_mac_is_refused() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  const uint8_t sameEnd[kMacSize] = {0x40, 0x11, 0x22, 0x3c, 0x5a, 0xa4};
  TEST_ASSERT_EQUAL_INT(result(PairResult::IdTaken), result(registry.pair(kKindPlug, sameEnd, kKeyB)));
  // Another kind has another id, so it is allowed.
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.pair(kKindClamp, sameEnd, kKeyB)));
  TEST_ASSERT_EQUAL_UINT(2, registry.idCount());
}

void test_the_id_stays_taken_after_the_node_is_removed() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  TEST_ASSERT_TRUE(registry.unpair(kMacA));
  const uint8_t sameEnd[kMacSize] = {0x40, 0x11, 0x22, 0x3c, 0x5a, 0xa4};
  TEST_ASSERT_EQUAL_INT(result(PairResult::IdTaken), result(registry.pair(kKindPlug, sameEnd, kKeyB)));
  TEST_ASSERT_EQUAL_UINT(1, registry.idCount());
  TEST_ASSERT_EQUAL_UINT(0, registry.pairedCount());
}

void test_removing_forgets_the_pairing_and_wipes_the_key() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  TEST_ASSERT_TRUE(registry.unpair(kMacA));
  TEST_ASSERT_FALSE(registry.isPaired(kMacA));
  TEST_ASSERT_NULL(registry.keyOf(kMacA));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Unknown), check(registry.check(kMacA, 1, 1)));
  // The stored record has no key left.
  const uint8_t zeroKey[kNodeKeySize] = {0};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(zeroKey, store.data[0] + 9, kNodeKeySize);
  TEST_ASSERT_EQUAL_UINT8(0, store.data[0][2]);
  TEST_ASSERT_FALSE(registry.unpair(kMacA));
}

void test_a_removed_node_pairs_again_in_its_old_slot_with_a_clean_boot_value() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  registry.accept(kMacA, 7, 1);
  registry.unpair(kMacA);
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.pair(kKindPlug, kMacA, kKeyB)));
  TEST_ASSERT_EQUAL_UINT(1, registry.idCount());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kKeyB, registry.keyOf(kMacA), kNodeKeySize);
  // The boot value 7 of the old pairing is forgotten, so a node that starts again at 1 is accepted.
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Accepted), check(registry.accept(kMacA, 1, 1)));
}

void test_a_removed_node_that_comes_back_as_the_other_kind_takes_a_new_slot() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  registry.unpair(kMacA);
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.pair(kKindClamp, kMacA, kKeyB)));
  TEST_ASSERT_EQUAL_UINT(2, registry.idCount());
  TEST_ASSERT_EQUAL_UINT(1, registry.pairedCount());
  TEST_ASSERT_EQUAL_UINT8(kKindClamp, registry.kindOf(kMacA));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kKeyB, registry.keyOf(kMacA), kNodeKeySize);
  char id[kNodeIdSize];
  TEST_ASSERT_TRUE(registry.nodeId(kMacA, id));
  TEST_ASSERT_EQUAL_STRING("CLAMP-3C5AA4", id);
  // The other kind is then a node that is paired, so it cannot pair as the first kind.
  TEST_ASSERT_EQUAL_INT(result(PairResult::AlreadyPaired), result(registry.pair(kKindPlug, kMacA, kKeyA)));
  // Remove it, and it can go back to the first kind in the first slot.
  TEST_ASSERT_TRUE(registry.unpair(kMacA));
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.pair(kKindPlug, kMacA, kKeyA)));
  TEST_ASSERT_EQUAL_UINT(2, registry.idCount());
}

void test_the_registry_is_full_after_64_ids_for_the_life_of_the_gateway() {
  FakeStore store;
  NodeRegistry registry(store);
  uint8_t mac[kMacSize];
  for (uint32_t i = 1; i <= kMaxNodes; i++) {
    makeMac(mac, i);
    TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.pair(kKindPlug, mac, kKeyA)));
  }
  TEST_ASSERT_EQUAL_UINT(64, registry.idCount());
  makeMac(mac, 1000);
  TEST_ASSERT_EQUAL_INT(result(PairResult::RegistryFull), result(registry.pair(kKindPlug, mac, kKeyA)));
  // Removing a node does not free its id.
  makeMac(mac, 1);
  TEST_ASSERT_TRUE(registry.unpair(mac));
  makeMac(mac, 1000);
  TEST_ASSERT_EQUAL_INT(result(PairResult::RegistryFull), result(registry.pair(kKindPlug, mac, kKeyA)));
  // The removed node itself can still pair again.
  makeMac(mac, 1);
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.pair(kKindPlug, mac, kKeyA)));
}

void test_a_failed_write_leaves_no_pairing() {
  FakeStore store;
  NodeRegistry registry(store);
  store.failWrites = true;
  TEST_ASSERT_EQUAL_INT(result(PairResult::StoreFailed), result(registry.pair(kKindPlug, kMacA, kKeyA)));
  TEST_ASSERT_FALSE(registry.isPaired(kMacA));
  TEST_ASSERT_EQUAL_UINT(0, registry.idCount());
  store.failWrites = false;
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.pair(kKindPlug, kMacA, kKeyA)));
}

void test_a_failed_write_when_pairing_again_leaves_the_node_removed() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  registry.unpair(kMacA);
  store.failWrites = true;
  TEST_ASSERT_EQUAL_INT(result(PairResult::StoreFailed), result(registry.pair(kKindPlug, kMacA, kKeyB)));
  TEST_ASSERT_FALSE(registry.isPaired(kMacA));
  TEST_ASSERT_NULL(registry.keyOf(kMacA));
}

void test_a_failed_write_when_removing_keeps_the_pairing_in_ram_too() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  registry.accept(kMacA, 3, 5);
  store.failWrites = true;
  TEST_ASSERT_FALSE(registry.unpair(kMacA));
  TEST_ASSERT_TRUE(registry.isPaired(kMacA));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kKeyA, registry.keyOf(kMacA), kNodeKeySize);
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Stale), check(registry.check(kMacA, 3, 5)));
}

void test_a_frame_of_a_node_that_is_not_paired_is_unknown() {
  FakeStore store;
  NodeRegistry registry(store);
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Unknown), check(registry.check(kMacA, 1, 1)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Unknown), check(registry.accept(kMacA, 1, 1)));
}

void test_the_first_frame_of_a_new_pairing_is_accepted_and_a_repeat_is_not() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Accepted), check(registry.accept(kMacA, 1, 0)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Stale), check(registry.accept(kMacA, 1, 0)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Accepted), check(registry.accept(kMacA, 1, 1)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Stale), check(registry.accept(kMacA, 1, 1)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Stale), check(registry.accept(kMacA, 1, 0)));
}

void test_a_counter_may_jump_up_but_never_down() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  registry.accept(kMacA, 2, 10);
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Accepted), check(registry.accept(kMacA, 2, 500)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Stale), check(registry.accept(kMacA, 2, 499)));
}

void test_a_frame_of_an_older_boot_is_stale() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  registry.accept(kMacA, 5, 3);
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Stale), check(registry.accept(kMacA, 4, 1000)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Accepted), check(registry.accept(kMacA, 6, 0)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Stale), check(registry.accept(kMacA, 5, 1000)));
}

void test_check_changes_nothing() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  const int writes = store.writes;
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Accepted), check(registry.check(kMacA, 9, 9)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Accepted), check(registry.check(kMacA, 9, 9)));
  TEST_ASSERT_EQUAL_INT(writes, store.writes);
}

void test_the_boot_value_is_written_only_when_it_grows() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  const int afterPair = store.writes;
  registry.accept(kMacA, 5, 1);
  TEST_ASSERT_EQUAL_INT(afterPair + 1, store.writes);
  registry.accept(kMacA, 5, 2);
  registry.accept(kMacA, 5, 3);
  registry.accept(kMacA, 4, 9);
  TEST_ASSERT_EQUAL_INT(afterPair + 1, store.writes);
  registry.accept(kMacA, 6, 1);
  TEST_ASSERT_EQUAL_INT(afterPair + 2, store.writes);
}

void test_a_boot_value_that_cannot_be_written_drops_the_frame() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  registry.accept(kMacA, 5, 1);
  store.failWrites = true;
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::StoreFailed), check(registry.accept(kMacA, 6, 1)));
  // Nothing changed, so the old boot and counter still decide.
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Accepted), check(registry.check(kMacA, 6, 1)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Stale), check(registry.check(kMacA, 5, 1)));
  store.failWrites = false;
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Accepted), check(registry.accept(kMacA, 6, 1)));
}

void test_the_state_comes_back_after_a_restart_but_the_counter_does_not() {
  FakeStore store;
  {
    NodeRegistry registry(store);
    registry.pair(kKindPlug, kMacA, kKeyA);
    registry.pair(kKindClamp, kMacB, kKeyB);
    registry.accept(kMacA, 9, 40);
    registry.unpair(kMacB);
  }
  NodeRegistry registry(store);
  TEST_ASSERT_EQUAL_UINT(2, registry.load());
  TEST_ASSERT_EQUAL_UINT(1, registry.pairedCount());
  TEST_ASSERT_EQUAL_UINT(2, registry.idCount());
  TEST_ASSERT_TRUE(registry.isPaired(kMacA));
  TEST_ASSERT_FALSE(registry.isPaired(kMacB));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kKeyA, registry.keyOf(kMacA), kNodeKeySize);
  char id[kNodeIdSize];
  TEST_ASSERT_TRUE(registry.nodeId(kMacA, id));
  TEST_ASSERT_EQUAL_STRING("PLUG-3C5AA4", id);
  // The boot value 9 is kept, so an older boot is stale. The counter lives in RAM only, so the next frame of boot 9 is accepted once.
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Stale), check(registry.accept(kMacA, 8, 99)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Accepted), check(registry.accept(kMacA, 9, 40)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Stale), check(registry.accept(kMacA, 9, 40)));
  // The id of the removed node is still taken, and the node can pair again.
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.pair(kKindClamp, kMacB, kKeyB)));
  TEST_ASSERT_EQUAL_UINT(2, registry.idCount());
}

void test_a_damaged_record_is_skipped_and_its_slot_is_never_reused() {
  FakeStore store;
  {
    NodeRegistry registry(store);
    registry.pair(kKindPlug, kMacA, kKeyA);
  }
  store.data[0][0] = 99;
  NodeRegistry registry(store);
  TEST_ASSERT_EQUAL_UINT(0, registry.load());
  TEST_ASSERT_EQUAL_UINT(1, registry.damagedCount());
  TEST_ASSERT_FALSE(registry.isPaired(kMacA));
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.pair(kKindPlug, kMacB, kKeyB)));
  // The new pairing went to slot 1, and the damaged record of slot 0 was not overwritten.
  TEST_ASSERT_EQUAL_UINT8(99, store.data[0][0]);
  TEST_ASSERT_TRUE(store.present[1]);
}

void test_every_kind_of_damage_is_found() {
  const uint8_t good[kRecordSize] = {1, 1, 1, 0x40, 0, 0, 1, 2, 3, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0, 0, 0, 0};
  for (size_t byte : {static_cast<size_t>(0), static_cast<size_t>(1), static_cast<size_t>(2)}) {
    for (uint8_t bad : {static_cast<uint8_t>(0), static_cast<uint8_t>(3), static_cast<uint8_t>(0xFF)}) {
      FakeStore store;
      memcpy(store.data[0], good, kRecordSize);
      store.data[0][byte] = bad;
      store.present[0] = true;
      NodeRegistry registry(store);
      const size_t valid = registry.load();
      // Flags other than 0 and 1, a bad version and a bad kind are damaged, and a flag of 0 is a valid removed node.
      const bool shouldBeValid = byte == 2 && bad == 0;
      TEST_ASSERT_EQUAL_UINT(shouldBeValid ? 1 : 0, valid);
      TEST_ASSERT_EQUAL_UINT(shouldBeValid ? 0 : 1, registry.damagedCount());
    }
  }
  FakeStore store;
  memcpy(store.data[0], good, kRecordSize);
  store.data[0][3] = 0x41;
  store.present[0] = true;
  NodeRegistry registry(store);
  TEST_ASSERT_EQUAL_UINT(0, registry.load());
  TEST_ASSERT_EQUAL_UINT(1, registry.damagedCount());
}

void test_a_slot_that_cannot_be_read_counts_as_damaged_and_is_not_reused() {
  FakeStore store;
  {
    NodeRegistry registry(store);
    registry.pair(kKindPlug, kMacA, kKeyA);
  }
  store.failReadSlot = 0;
  NodeRegistry registry(store);
  TEST_ASSERT_EQUAL_UINT(0, registry.load());
  TEST_ASSERT_EQUAL_UINT(1, registry.damagedCount());
  registry.pair(kKindPlug, kMacB, kKeyB);
  TEST_ASSERT_TRUE(store.present[1]);
}

void test_a_stored_removed_node_has_no_key_or_boot_after_load() {
  FakeStore store;
  {
    NodeRegistry registry(store);
    registry.pair(kKindPlug, kMacA, kKeyA);
    registry.accept(kMacA, 4, 1);
    registry.unpair(kMacA);
  }
  NodeRegistry registry(store);
  registry.load();
  TEST_ASSERT_FALSE(registry.isPaired(kMacA));
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.pair(kKindPlug, kMacA, kKeyB)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Accepted), check(registry.accept(kMacA, 1, 0)));
}

void test_the_stored_record_has_the_layout_of_the_store() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindClamp, kMacB, kKeyB);
  registry.accept(kMacB, 0x04030201u, 1);
  const uint8_t expected[kRecordSize] = {1, 2, 1, 0x48, 0xe7, 0x29, 0xb6, 0xe0, 0x84, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6,
                                         0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0x01, 0x02, 0x03, 0x04};
  TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, store.data[0], kRecordSize);
}

void test_a_boot_value_of_zero_is_never_accepted() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Stale), check(registry.check(kMacA, 0, 0)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Stale), check(registry.accept(kMacA, 0, 5)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Accepted), check(registry.accept(kMacA, 1, 0)));
}

void test_the_highest_counter_and_boot_value_work_and_a_new_boot_starts_again() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Accepted), check(registry.accept(kMacA, 0xFFFFFFFFu, 0xFFFFFFFFu)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Stale), check(registry.accept(kMacA, 0xFFFFFFFFu, 0xFFFFFFFFu)));
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Stale), check(registry.accept(kMacA, 0xFFFFFFFEu, 1)));
}

void test_a_frame_of_a_node_that_was_removed_is_unknown() {
  FakeStore store;
  NodeRegistry registry(store);
  registry.pair(kKindPlug, kMacA, kKeyA);
  registry.unpair(kMacA);
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Unknown), check(registry.accept(kMacA, 1, 1)));
}

void test_a_removed_record_with_a_key_left_in_it_gives_no_key_after_load() {
  FakeStore store;
  const uint8_t record[kRecordSize] = {1, 1, 0, 0x40, 0, 0, 1, 2, 3, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 7, 0, 0, 0};
  memcpy(store.data[0], record, kRecordSize);
  store.present[0] = true;
  NodeRegistry registry(store);
  TEST_ASSERT_EQUAL_UINT(1, registry.load());
  const uint8_t mac[kMacSize] = {0x40, 0, 0, 1, 2, 3};
  TEST_ASSERT_FALSE(registry.isPaired(mac));
  TEST_ASSERT_NULL(registry.keyOf(mac));
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.pair(kKindPlug, mac, kKeyA)));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kKeyA, registry.keyOf(mac), kNodeKeySize);
  // The boot value 7 that was left in the record is not used.
  TEST_ASSERT_EQUAL_INT(check(FrameCheck::Accepted), check(registry.accept(mac, 1, 0)));
}

void test_a_second_record_with_the_same_mac_or_the_same_id_is_left_out() {
  FakeStore store;
  const uint8_t first[kRecordSize] = {1, 1, 1, 0x40, 0, 0, 1, 2, 3, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 0, 0, 0, 0};
  memcpy(store.data[0], first, kRecordSize);
  memcpy(store.data[1], first, kRecordSize);
  store.data[1][9] = 0xEE;
  memcpy(store.data[2], first, kRecordSize);
  store.data[2][3] = 0x42;
  store.present[0] = store.present[1] = store.present[2] = true;
  NodeRegistry registry(store);
  TEST_ASSERT_EQUAL_UINT(1, registry.load());
  TEST_ASSERT_EQUAL_UINT(2, registry.damagedCount());
  const uint8_t mac[kMacSize] = {0x40, 0, 0, 1, 2, 3};
  TEST_ASSERT_EQUAL_UINT8(1, registry.keyOf(mac)[0]);
}

void test_a_read_that_fails_once_is_tried_again() {
  FakeStore store;
  {
    NodeRegistry registry(store);
    registry.pair(kKindPlug, kMacA, kKeyA);
  }
  store.failReadOnceSlot = 0;
  NodeRegistry registry(store);
  TEST_ASSERT_EQUAL_UINT(1, registry.load());
  TEST_ASSERT_EQUAL_UINT(0, registry.damagedCount());
  TEST_ASSERT_TRUE(registry.isPaired(kMacA));
}

void test_the_full_state_and_a_kind_change_survive_a_restart() {
  FakeStore store;
  uint8_t mac[kMacSize];
  {
    NodeRegistry registry(store);
    for (uint32_t i = 1; i <= kMaxNodes; i++) {
      makeMac(mac, i);
      registry.pair(kKindPlug, mac, kKeyA);
    }
  }
  NodeRegistry registry(store);
  TEST_ASSERT_EQUAL_UINT(64, registry.load());
  makeMac(mac, 1000);
  TEST_ASSERT_EQUAL_INT(result(PairResult::RegistryFull), result(registry.pair(kKindPlug, mac, kKeyA)));
  makeMac(mac, 5);
  TEST_ASSERT_TRUE(registry.unpair(mac));
  // The other kind of a known MAC is a new id, and the registry is full, so it is refused.
  TEST_ASSERT_EQUAL_INT(result(PairResult::RegistryFull), result(registry.pair(kKindClamp, mac, kKeyA)));
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.pair(kKindPlug, mac, kKeyA)));
}

void test_can_pair_says_what_pair_would_answer_and_changes_nothing() {
  FakeStore store;
  NodeRegistry registry(store);
  const uint8_t zero[kMacSize] = {0};
  TEST_ASSERT_EQUAL_INT(result(PairResult::InvalidKind), result(registry.canPair(0, kMacA)));
  TEST_ASSERT_EQUAL_INT(result(PairResult::InvalidMac), result(registry.canPair(kKindPlug, zero)));
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.canPair(kKindPlug, kMacA)));
  TEST_ASSERT_EQUAL_INT(0, store.writes);
  TEST_ASSERT_EQUAL_UINT(0, registry.idCount());
  registry.pair(kKindPlug, kMacA, kKeyA);
  const int writes = store.writes;
  TEST_ASSERT_EQUAL_INT(result(PairResult::AlreadyPaired), result(registry.canPair(kKindPlug, kMacA)));
  TEST_ASSERT_EQUAL_INT(result(PairResult::AlreadyPaired), result(registry.canPair(kKindClamp, kMacA)));
  const uint8_t sameEnd[kMacSize] = {0x40, 0x11, 0x22, 0x3c, 0x5a, 0xa4};
  TEST_ASSERT_EQUAL_INT(result(PairResult::IdTaken), result(registry.canPair(kKindPlug, sameEnd)));
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.canPair(kKindClamp, sameEnd)));
  TEST_ASSERT_EQUAL_INT(writes, store.writes);
}

void test_can_pair_says_ok_for_a_removed_node_and_full_for_a_full_registry() {
  FakeStore store;
  NodeRegistry registry(store);
  uint8_t mac[kMacSize];
  for (uint32_t i = 1; i <= kMaxNodes; i++) {
    makeMac(mac, i);
    registry.pair(kKindPlug, mac, kKeyA);
  }
  makeMac(mac, 1000);
  TEST_ASSERT_EQUAL_INT(result(PairResult::RegistryFull), result(registry.canPair(kKindPlug, mac)));
  makeMac(mac, 7);
  registry.unpair(mac);
  TEST_ASSERT_EQUAL_INT(result(PairResult::Ok), result(registry.canPair(kKindPlug, mac)));
  TEST_ASSERT_EQUAL_INT(result(PairResult::RegistryFull), result(registry.canPair(kKindClamp, mac)));
}

void test_the_id_can_be_made_before_the_node_is_paired() {
  char id[kNodeIdSize];
  TEST_ASSERT_TRUE(makeNodeId(kKindPlug, kMacA, id));
  TEST_ASSERT_EQUAL_STRING("PLUG-3C5AA4", id);
  TEST_ASSERT_TRUE(makeNodeId(kKindClamp, kMacB, id));
  TEST_ASSERT_EQUAL_STRING("CLAMP-B6E084", id);
  TEST_ASSERT_FALSE(makeNodeId(0, kMacA, id));
  TEST_ASSERT_EQUAL_STRING("", id);
  TEST_ASSERT_FALSE(makeNodeId(3, kMacA, id));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_the_limits_are_the_documented_ones);
  RUN_TEST(test_a_new_registry_is_empty);
  RUN_TEST(test_a_pairing_is_kept_and_found);
  RUN_TEST(test_the_id_is_the_kind_and_the_last_three_bytes_of_the_mac);
  RUN_TEST(test_the_longest_id_fits_the_buffer);
  RUN_TEST(test_a_bad_kind_or_mac_is_refused_and_nothing_is_written);
  RUN_TEST(test_a_node_that_is_paired_cannot_pair_again);
  RUN_TEST(test_the_same_id_for_another_mac_is_refused);
  RUN_TEST(test_the_id_stays_taken_after_the_node_is_removed);
  RUN_TEST(test_removing_forgets_the_pairing_and_wipes_the_key);
  RUN_TEST(test_a_removed_node_pairs_again_in_its_old_slot_with_a_clean_boot_value);
  RUN_TEST(test_a_removed_node_that_comes_back_as_the_other_kind_takes_a_new_slot);
  RUN_TEST(test_the_registry_is_full_after_64_ids_for_the_life_of_the_gateway);
  RUN_TEST(test_a_failed_write_leaves_no_pairing);
  RUN_TEST(test_a_failed_write_when_pairing_again_leaves_the_node_removed);
  RUN_TEST(test_a_failed_write_when_removing_keeps_the_pairing_in_ram_too);
  RUN_TEST(test_a_frame_of_a_node_that_is_not_paired_is_unknown);
  RUN_TEST(test_the_first_frame_of_a_new_pairing_is_accepted_and_a_repeat_is_not);
  RUN_TEST(test_a_counter_may_jump_up_but_never_down);
  RUN_TEST(test_a_frame_of_an_older_boot_is_stale);
  RUN_TEST(test_check_changes_nothing);
  RUN_TEST(test_the_boot_value_is_written_only_when_it_grows);
  RUN_TEST(test_a_boot_value_that_cannot_be_written_drops_the_frame);
  RUN_TEST(test_the_state_comes_back_after_a_restart_but_the_counter_does_not);
  RUN_TEST(test_a_damaged_record_is_skipped_and_its_slot_is_never_reused);
  RUN_TEST(test_every_kind_of_damage_is_found);
  RUN_TEST(test_a_slot_that_cannot_be_read_counts_as_damaged_and_is_not_reused);
  RUN_TEST(test_a_stored_removed_node_has_no_key_or_boot_after_load);
  RUN_TEST(test_the_stored_record_has_the_layout_of_the_store);
  RUN_TEST(test_can_pair_says_what_pair_would_answer_and_changes_nothing);
  RUN_TEST(test_can_pair_says_ok_for_a_removed_node_and_full_for_a_full_registry);
  RUN_TEST(test_the_id_can_be_made_before_the_node_is_paired);
  RUN_TEST(test_a_boot_value_of_zero_is_never_accepted);
  RUN_TEST(test_the_highest_counter_and_boot_value_work_and_a_new_boot_starts_again);
  RUN_TEST(test_a_frame_of_a_node_that_was_removed_is_unknown);
  RUN_TEST(test_a_removed_record_with_a_key_left_in_it_gives_no_key_after_load);
  RUN_TEST(test_a_second_record_with_the_same_mac_or_the_same_id_is_left_out);
  RUN_TEST(test_a_read_that_fails_once_is_tried_again);
  RUN_TEST(test_the_full_state_and_a_kind_change_survive_a_restart);
  return UNITY_END();
}
