#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "../support/fake-crypto.h"
#include "flash-command-log.h"

using namespace wattlink;
using namespace wattlink::testsupport;

namespace {

constexpr uint32_t kServerTime = 1800000000;
constexpr uint32_t kExpires = kServerTime + 600;
constexpr uint32_t kNodeBoot = 3;

// A blob store in memory. It can fail reads and writes, and a test can change the bytes of a slot.
class FakeBlobStore : public CommandBlobStore {
 public:
  ReadResult read(size_t slot, uint8_t out[kCommandBlobSize]) override {
    reads++;
    if (failReadSlot == static_cast<int>(slot) || (failReadOnceSlot == static_cast<int>(slot) && reads == 1)) {
      return ReadResult::Failed;
    }
    if (!present[slot]) {
      return ReadResult::Missing;
    }
    memcpy(out, data[slot], kCommandBlobSize);
    return ReadResult::Ok;
  }

  bool write(size_t slot, const uint8_t in[kCommandBlobSize]) override {
    writes++;
    if (failWrites) {
      return false;
    }
    memcpy(data[slot], in, kCommandBlobSize);
    present[slot] = true;
    return true;
  }

  uint8_t data[kCommandLogSlots][kCommandBlobSize] = {};
  bool present[kCommandLogSlots] = {};
  bool failWrites = false;
  int failReadSlot = -1;
  int failReadOnceSlot = -1;
  int reads = 0;
  int writes = 0;
};

class FakeSender : public CommandSender {
 public:
  bool sendCommand(const uint8_t[kMacSize], uint32_t, uint32_t, uint16_t number, uint8_t) override {
    lastNumber = number;
    count++;
    return true;
  }

  uint16_t lastNumber = 0;
  size_t count = 0;
};

CommandRecord makeRecord(const char *id, uint16_t number, CommandState state, uint32_t expiresAt) {
  CommandRecord record;
  memset(&record, 0, sizeof(record));
  strncpy(record.id, id, kResultIdSize - 1);
  record.number = number;
  record.state = state;
  record.expiresAt = expiresAt;
  return record;
}

bool sameRecord(const CommandRecord &a, const CommandRecord &b) {
  return strncmp(a.id, b.id, kResultIdSize) == 0 && a.number == b.number && a.state == b.state && a.expiresAt == b.expiresAt;
}

// Puts the right CRC on a blob, so a test can change a field and still pass the check.
void seal(uint8_t blob[kCommandBlobSize]) {
  const uint32_t crc = crc32Ieee(blob, kCommandBlobSize - 4);
  for (int i = 0; i < 4; i++) {
    blob[kCommandBlobSize - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
  }
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_crc32_gives_the_known_answer() {
  const uint8_t text[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926u, crc32Ieee(text, sizeof(text)));
  TEST_ASSERT_EQUAL_HEX32(0u, crc32Ieee(text, 0));
}

void test_a_record_survives_encode_and_decode_in_every_state() {
  const CommandState states[] = {CommandState::Free, CommandState::Sent, CommandState::Done, CommandState::Failed};
  for (CommandState state : states) {
    const CommandRecord record = makeRecord("cmd-1", 0xA5F1, state, 0xFFFFFFF0u);
    uint8_t blob[kCommandBlobSize];
    TEST_ASSERT_TRUE(encodeCommandRecord(record, blob));
    CommandRecord back;
    memset(&back, 0x55, sizeof(back));
    TEST_ASSERT_TRUE(decodeCommandRecord(blob, &back));
    TEST_ASSERT_TRUE(sameRecord(record, back));
  }
}

void test_an_id_of_36_characters_fits_and_a_longer_one_is_refused() {
  char id36[kResultIdSize];
  memset(id36, 'k', sizeof(id36));
  id36[36] = '\0';
  CommandRecord record = makeRecord("x", 7, CommandState::Sent, 99);
  memcpy(record.id, id36, kResultIdSize);
  uint8_t blob[kCommandBlobSize];
  TEST_ASSERT_TRUE(encodeCommandRecord(record, blob));
  CommandRecord back;
  TEST_ASSERT_TRUE(decodeCommandRecord(blob, &back));
  TEST_ASSERT_TRUE(sameRecord(record, back));
  // A state that this code does not know is refused too, and nothing is written.
  CommandRecord oddState = makeRecord("cmd-1", 7, CommandState::Sent, 99);
  oddState.state = static_cast<CommandState>(9);
  memset(blob, 0xEE, sizeof(blob));
  TEST_ASSERT_FALSE(encodeCommandRecord(oddState, blob));
  TEST_ASSERT_EQUAL_HEX8(0xEE, blob[0]);
  // No end mark inside the buffer, so the id would be cut. Nothing is written.
  memset(record.id, 'k', kResultIdSize);
  memset(blob, 0xEE, sizeof(blob));
  TEST_ASSERT_FALSE(encodeCommandRecord(record, blob));
  TEST_ASSERT_EQUAL_HEX8(0xEE, blob[0]);
}

void test_bytes_after_the_end_of_the_id_do_not_change_the_blob() {
  CommandRecord a = makeRecord("cmd-1", 5, CommandState::Sent, 10);
  CommandRecord b = a;
  memset(b.id + 6, 'z', kResultIdSize - 6);
  uint8_t blobA[kCommandBlobSize];
  uint8_t blobB[kCommandBlobSize];
  TEST_ASSERT_TRUE(encodeCommandRecord(a, blobA));
  TEST_ASSERT_TRUE(encodeCommandRecord(b, blobB));
  TEST_ASSERT_EQUAL_MEMORY(blobA, blobB, kCommandBlobSize);
}

void test_a_single_changed_bit_anywhere_in_the_blob_is_found() {
  const CommandRecord record = makeRecord("cmd-1", 0x0500, CommandState::Sent, kExpires);
  uint8_t blob[kCommandBlobSize];
  TEST_ASSERT_TRUE(encodeCommandRecord(record, blob));
  for (size_t byte = 0; byte < kCommandBlobSize; byte++) {
    for (int bit = 0; bit < 8; bit++) {
      uint8_t copy[kCommandBlobSize];
      memcpy(copy, blob, sizeof(copy));
      copy[byte] ^= static_cast<uint8_t>(1 << bit);
      CommandRecord back = makeRecord("keep", 1, CommandState::Done, 1);
      TEST_ASSERT_FALSE(decodeCommandRecord(copy, &back));
      TEST_ASSERT_EQUAL_STRING("keep", back.id);
    }
  }
}

void test_a_blob_of_another_version_or_with_an_unknown_state_is_refused_even_when_the_crc_matches() {
  const CommandRecord record = makeRecord("cmd-1", 0x0500, CommandState::Sent, kExpires);
  uint8_t blob[kCommandBlobSize];
  TEST_ASSERT_TRUE(encodeCommandRecord(record, blob));
  CommandRecord back;
  uint8_t other[kCommandBlobSize];
  memcpy(other, blob, sizeof(other));
  other[0] = kCommandBlobVersion + 1;
  seal(other);
  TEST_ASSERT_FALSE(decodeCommandRecord(other, &back));
  memcpy(other, blob, sizeof(other));
  other[1] = 4;
  seal(other);
  TEST_ASSERT_FALSE(decodeCommandRecord(other, &back));
  memcpy(other, blob, sizeof(other));
  seal(other);
  TEST_ASSERT_TRUE(decodeCommandRecord(other, &back));
}

void test_the_layout_is_fixed_so_a_later_version_can_still_read_it() {
  const CommandRecord record = makeRecord("ab", 0x0102, CommandState::Done, 0x0A0B0C0Du);
  uint8_t blob[kCommandBlobSize];
  TEST_ASSERT_TRUE(encodeCommandRecord(record, blob));
  TEST_ASSERT_EQUAL_HEX8(1, blob[0]);
  TEST_ASSERT_EQUAL_HEX8(2, blob[1]);
  TEST_ASSERT_EQUAL_HEX8(0x02, blob[2]);
  TEST_ASSERT_EQUAL_HEX8(0x01, blob[3]);
  TEST_ASSERT_EQUAL_HEX8(0x0D, blob[4]);
  TEST_ASSERT_EQUAL_HEX8(0x0A, blob[7]);
  TEST_ASSERT_EQUAL_HEX8('a', blob[8]);
  TEST_ASSERT_EQUAL_HEX8('b', blob[9]);
  TEST_ASSERT_EQUAL_HEX8(0, blob[10]);
  TEST_ASSERT_EQUAL_HEX8(0, blob[43]);
}

void test_the_log_writes_and_reads_back_every_slot() {
  FakeBlobStore store;
  FlashCommandLog log(store);
  for (size_t slot = 0; slot < kCommandLogSlots; slot++) {
    char id[kResultIdSize];
    snprintf(id, sizeof(id), "cmd-%u", static_cast<unsigned>(slot));
    TEST_ASSERT_TRUE(log.write(slot, makeRecord(id, static_cast<uint16_t>(0x0500 + slot), CommandState::Sent, kExpires + slot)));
  }
  for (size_t slot = 0; slot < kCommandLogSlots; slot++) {
    char id[kResultIdSize];
    snprintf(id, sizeof(id), "cmd-%u", static_cast<unsigned>(slot));
    CommandRecord back;
    TEST_ASSERT_TRUE(log.read(slot, &back));
    TEST_ASSERT_TRUE(sameRecord(makeRecord(id, static_cast<uint16_t>(0x0500 + slot), CommandState::Sent, kExpires + slot), back));
  }
  TEST_ASSERT_EQUAL_UINT32(0, log.damaged());
  TEST_ASSERT_EQUAL_UINT32(0, log.failedReads());
}

void test_a_slot_outside_the_log_is_refused_and_touches_nothing() {
  FakeBlobStore store;
  FlashCommandLog log(store);
  CommandRecord back;
  TEST_ASSERT_FALSE(log.read(kCommandLogSlots, &back));
  TEST_ASSERT_FALSE(log.write(kCommandLogSlots, makeRecord("a", 1, CommandState::Sent, 1)));
  TEST_ASSERT_EQUAL_INT(0, store.reads);
  TEST_ASSERT_EQUAL_INT(0, store.writes);
}

void test_a_slot_that_was_never_written_reads_as_free_and_is_not_counted_as_lost() {
  FakeBlobStore store;
  FlashCommandLog log(store);
  CommandRecord back;
  TEST_ASSERT_FALSE(log.read(3, &back));
  TEST_ASSERT_EQUAL_UINT32(0, log.damaged());
  TEST_ASSERT_EQUAL_UINT32(0, log.failedReads());
}

void test_a_damaged_slot_reads_as_free_and_is_counted() {
  FakeBlobStore store;
  FlashCommandLog log(store);
  TEST_ASSERT_TRUE(log.write(2, makeRecord("cmd-1", 5, CommandState::Sent, 10)));
  store.data[2][20] ^= 0x40;
  CommandRecord back;
  TEST_ASSERT_FALSE(log.read(2, &back));
  TEST_ASSERT_EQUAL_UINT32(1, log.damaged());
  TEST_ASSERT_EQUAL_UINT32(0, log.failedReads());
}

void test_a_read_that_fails_once_is_tried_again_and_one_that_keeps_failing_is_counted() {
  FakeBlobStore store;
  FlashCommandLog log(store);
  TEST_ASSERT_TRUE(log.write(0, makeRecord("cmd-1", 5, CommandState::Sent, 10)));
  TEST_ASSERT_TRUE(log.write(1, makeRecord("cmd-2", 6, CommandState::Sent, 10)));
  store.failReadOnceSlot = 0;
  store.reads = 0;
  CommandRecord back;
  TEST_ASSERT_TRUE(log.read(0, &back));
  TEST_ASSERT_EQUAL_STRING("cmd-1", back.id);
  TEST_ASSERT_EQUAL_UINT32(0, log.failedReads());
  store.failReadSlot = 1;
  TEST_ASSERT_FALSE(log.read(1, &back));
  TEST_ASSERT_EQUAL_UINT32(1, log.failedReads());
  TEST_ASSERT_EQUAL_UINT32(0, log.damaged());
}

// The fake keeps the old bytes when a write fails, so this tests what the log does with a failed write and not what NVS does.
void test_a_failed_write_is_reported_and_the_log_does_not_touch_the_old_slot_and_a_long_id_is_not_written() {
  FakeBlobStore store;
  FlashCommandLog log(store);
  TEST_ASSERT_TRUE(log.write(4, makeRecord("cmd-1", 5, CommandState::Sent, 10)));
  store.failWrites = true;
  TEST_ASSERT_FALSE(log.write(4, makeRecord("cmd-1", 5, CommandState::Done, 10)));
  CommandRecord back;
  TEST_ASSERT_TRUE(log.read(4, &back));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandState::Sent), static_cast<int>(back.state));
  store.failWrites = false;
  CommandRecord bad = makeRecord("x", 1, CommandState::Sent, 1);
  memset(bad.id, 'k', kResultIdSize);
  const int before = store.writes;
  TEST_ASSERT_FALSE(log.write(4, bad));
  TEST_ASSERT_EQUAL_INT(before, store.writes);
}

// The same flow as the restart test of the relay, over the flash log, so the layout and the relay agree.
void test_a_relay_that_restarts_over_the_flash_log_sends_the_same_number_again() {
  FakeStore nodes;
  NodeRegistry registry(nodes);
  registry.load();
  uint8_t mac[kMacSize] = {0x40, 0x00, 0x00, 0x00, 0x00, 1};
  uint8_t key[kNodeKeySize] = {1};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::Ok), static_cast<int>(registry.pair(kKindPlug, mac, key)));
  char plug[kNodeIdSize];
  TEST_ASSERT_TRUE(registry.nodeId(mac, plug));

  FakeBlobStore store;
  FlashCommandLog log(store);
  FakeSender sender;
  CommandRelay relay(sender, registry, log, 0x105);
  TEST_ASSERT_EQUAL_UINT(0, relay.load());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SubmitResult::Queued), static_cast<int>(relay.submit(0, kServerTime, "cmd-1", plug, true, kExpires)));
  LinkEvent event;
  memset(&event, 0, sizeof(event));
  event.kind = LinkEventKind::Telemetry;
  memcpy(event.mac, mac, kMacSize);
  event.frameBootId = kNodeBoot;
  event.frameCounter = 10;
  relay.onEvent(1000, event);
  const uint16_t number = sender.lastNumber;
  TEST_ASSERT_EQUAL_UINT16(0x0500, number);
  TEST_ASSERT_EQUAL_INT(1, store.writes);

  // The hub starts again. A new object reads the same bytes of the flash.
  FlashCommandLog reread(store);
  CommandRelay restarted(sender, registry, reread, 0x206);
  TEST_ASSERT_EQUAL_UINT(1, restarted.load());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(SubmitResult::Resumed), static_cast<int>(restarted.submit(50, kServerTime + 30, "cmd-1", plug, true, kExpires)));
  event.frameBootId = 4;
  event.frameCounter = 3;
  restarted.onEvent(100, event);
  TEST_ASSERT_EQUAL_UINT16(number, sender.lastNumber);

  // The plug answers, and the new state is written over the old one.
  LinkEvent answer;
  memset(&answer, 0, sizeof(answer));
  answer.kind = LinkEventKind::ResultReceived;
  memcpy(answer.mac, mac, kMacSize);
  answer.frameBootId = 4;
  answer.frameCounter = 5;
  answer.commandNumber = number;
  answer.commandOk = true;
  restarted.onEvent(200, answer);
  CommandRecord back;
  TEST_ASSERT_TRUE(reread.read(0, &back));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandState::Done), static_cast<int>(back.state));
  TEST_ASSERT_EQUAL_UINT16(number, back.number);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_crc32_gives_the_known_answer);
  RUN_TEST(test_a_record_survives_encode_and_decode_in_every_state);
  RUN_TEST(test_an_id_of_36_characters_fits_and_a_longer_one_is_refused);
  RUN_TEST(test_bytes_after_the_end_of_the_id_do_not_change_the_blob);
  RUN_TEST(test_a_single_changed_bit_anywhere_in_the_blob_is_found);
  RUN_TEST(test_a_blob_of_another_version_or_with_an_unknown_state_is_refused_even_when_the_crc_matches);
  RUN_TEST(test_the_layout_is_fixed_so_a_later_version_can_still_read_it);
  RUN_TEST(test_the_log_writes_and_reads_back_every_slot);
  RUN_TEST(test_a_slot_outside_the_log_is_refused_and_touches_nothing);
  RUN_TEST(test_a_slot_that_was_never_written_reads_as_free_and_is_not_counted_as_lost);
  RUN_TEST(test_a_damaged_slot_reads_as_free_and_is_counted);
  RUN_TEST(test_a_read_that_fails_once_is_tried_again_and_one_that_keeps_failing_is_counted);
  RUN_TEST(test_a_failed_write_is_reported_and_the_log_does_not_touch_the_old_slot_and_a_long_id_is_not_written);
  RUN_TEST(test_a_relay_that_restarts_over_the_flash_log_sends_the_same_number_again);
  return UNITY_END();
}
