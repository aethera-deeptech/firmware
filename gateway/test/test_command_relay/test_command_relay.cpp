#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "../support/fake-crypto.h"
#include "command-relay.h"

using namespace wattlink;
using namespace wattlink::testsupport;

namespace {

constexpr uint32_t kBoot = 0x105;
constexpr uint32_t kServerTime = 1800000000;
constexpr uint32_t kExpires = kServerTime + 600;
constexpr uint32_t kNodeBoot = 3;

struct Call {
  uint8_t mac[kMacSize];
  uint32_t echoBootId;
  uint32_t echoCounter;
  uint16_t number;
  uint8_t action;
};

class FakeSender : public CommandSender {
 public:
  bool sendCommand(const uint8_t mac[kMacSize], uint32_t echoBootId, uint32_t echoCounter, uint16_t number, uint8_t action) override {
    if (count < 256) {
      memcpy(calls[count].mac, mac, kMacSize);
      calls[count].echoBootId = echoBootId;
      calls[count].echoCounter = echoCounter;
      calls[count].number = number;
      calls[count].action = action;
    }
    count++;
    return !fail;
  }

  Call calls[256];
  size_t count = 0;
  bool fail = false;
};

class FakeLog : public CommandLog {
 public:
  LogRead read(size_t slot, CommandRecord *out) override {
    if (unreadable[slot]) {
      return LogRead::Unreadable;
    }
    if (!present[slot]) {
      return LogRead::Free;
    }
    *out = slots[slot];
    return LogRead::Record;
  }

  bool write(size_t slot, const CommandRecord &record) override {
    writes++;
    if (failWrites || failWriteSlot == static_cast<int>(slot)) {
      return false;
    }
    slots[slot] = record;
    present[slot] = true;
    unreadable[slot] = false;
    return true;
  }

  CommandRecord slots[kCommandLogSlots] = {};
  bool present[kCommandLogSlots] = {};
  bool unreadable[kCommandLogSlots] = {};
  int failWriteSlot = -1;
  int writes = 0;
  bool failWrites = false;
};

struct Rig {
  explicit Rig(uint32_t boot = kBoot) : registry(store), relay(sender, registry, log, boot) { registry.load(); }

  FakeStore store;
  NodeRegistry registry;
  FakeSender sender;
  FakeLog log;
  CommandRelay relay;
};

void macOfNumber(uint8_t number, uint8_t mac[kMacSize]) {
  const uint8_t value[kMacSize] = {0x40, 0x00, 0x00, 0x00, 0x00, number};
  memcpy(mac, value, kMacSize);
}

// Pairs a plug with a fixed key, and writes its id for the backend.
void pairPlug(Rig &rig, uint8_t number, char id[kNodeIdSize]) {
  uint8_t mac[kMacSize];
  macOfNumber(number, mac);
  uint8_t key[kNodeKeySize] = {number};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::Ok), static_cast<int>(rig.registry.pair(kKindPlug, mac, key)));
  TEST_ASSERT_TRUE(rig.registry.nodeId(mac, id));
}

LinkEvent telemetry(uint8_t number, uint32_t counter, uint32_t bootId = kNodeBoot) {
  LinkEvent event;
  memset(&event, 0, sizeof(event));
  event.kind = LinkEventKind::Telemetry;
  macOfNumber(number, event.mac);
  event.frameBootId = bootId;
  event.frameCounter = counter;
  return event;
}

LinkEvent resultEvent(uint8_t number, uint16_t commandNumber, bool ok) {
  LinkEvent event;
  memset(&event, 0, sizeof(event));
  event.kind = LinkEventKind::ResultReceived;
  macOfNumber(number, event.mac);
  event.frameBootId = kNodeBoot;
  event.frameCounter = 100;
  event.commandNumber = commandNumber;
  event.commandOk = ok;
  return event;
}

int result(SubmitResult value) {
  return static_cast<int>(value);
}

SubmitResult submit(Rig &rig, uint32_t nowMs, const char *id, const char *nodeId, bool on = true, uint32_t expiresAt = kExpires) {
  return rig.relay.submit(nowMs, kServerTime, id, nodeId, on, expiresAt);
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_a_queued_command_waits_for_a_frame_of_its_plug_and_goes_out_with_the_echo_of_that_frame() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 1000, "cmd-1", plug)));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.pendingCount());
  rig.relay.update(1500);
  TEST_ASSERT_EQUAL_UINT(0, rig.sender.count);
  // A frame of another plug is not the moment of this command.
  char other[kNodeIdSize];
  pairPlug(rig, 2, other);
  rig.relay.onEvent(1600, telemetry(2, 7));
  TEST_ASSERT_EQUAL_UINT(0, rig.sender.count);
  rig.relay.onEvent(2000, telemetry(1, 10));
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
  uint8_t mac[kMacSize];
  macOfNumber(1, mac);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(mac, rig.sender.calls[0].mac, kMacSize);
  TEST_ASSERT_EQUAL_UINT32(kNodeBoot, rig.sender.calls[0].echoBootId);
  TEST_ASSERT_EQUAL_UINT32(10, rig.sender.calls[0].echoCounter);
  // The high byte of the number is the low byte of the boot number of the hub, and the low byte counts from 0.
  TEST_ASSERT_EQUAL_UINT16(0x0500, rig.sender.calls[0].number);
  TEST_ASSERT_EQUAL_UINT8(1, rig.sender.calls[0].action);
  // The record is in the log before the frame, in the state Sent.
  TEST_ASSERT_EQUAL_INT(1, rig.log.writes);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandState::Sent), static_cast<int>(rig.log.slots[0].state));
  TEST_ASSERT_EQUAL_STRING("cmd-1", rig.log.slots[0].id);
  TEST_ASSERT_EQUAL_UINT16(0x0500, rig.log.slots[0].number);
  TEST_ASSERT_EQUAL_UINT32(kExpires, rig.log.slots[0].expiresAt);
}

void test_a_command_is_sent_again_every_five_seconds_with_the_echo_of_the_newest_frame() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  submit(rig, 0, "cmd-1", plug, false);
  rig.relay.onEvent(1000, telemetry(1, 10));
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT8(0, rig.sender.calls[0].action);
  // A later frame does not send again, it only becomes the echo of the next send.
  rig.relay.onEvent(3000, telemetry(1, 11));
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
  rig.relay.update(1000 + kCommandResendMs - 1);
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
  rig.relay.update(1000 + kCommandResendMs);
  TEST_ASSERT_EQUAL_UINT(2, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT32(11, rig.sender.calls[1].echoCounter);
  TEST_ASSERT_EQUAL_UINT16(rig.sender.calls[0].number, rig.sender.calls[1].number);
  rig.relay.update(1000 + 2 * kCommandResendMs - 1);
  TEST_ASSERT_EQUAL_UINT(2, rig.sender.count);
  rig.relay.update(1000 + 2 * kCommandResendMs);
  TEST_ASSERT_EQUAL_UINT(3, rig.sender.count);
}

void test_a_result_with_the_number_of_the_command_closes_it_and_is_kept_for_the_batch() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  submit(rig, 0, "cmd-1", plug);
  rig.relay.onEvent(1000, telemetry(1, 10));
  const uint16_t number = rig.sender.calls[0].number;
  // Another number, and the same number from another plug, are not this command.
  char other[kNodeIdSize];
  pairPlug(rig, 2, other);
  rig.relay.onEvent(1100, resultEvent(1, static_cast<uint16_t>(number + 1), true));
  rig.relay.onEvent(1200, resultEvent(2, number, true));
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.pendingCount());
  rig.relay.onEvent(1300, resultEvent(1, number, true));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_STRING("cmd-1", rig.relay.results()[0].id);
  TEST_ASSERT_TRUE(rig.relay.results()[0].ok);
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.pendingCount());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandState::Done), static_cast<int>(rig.log.slots[0].state));
  // The command is closed, so it is not sent again.
  rig.relay.update(1000 + 10 * kCommandResendMs);
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
}

void test_a_plug_that_could_not_switch_gives_a_result_that_is_not_ok_and_a_clamp_gets_the_command_too() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  submit(rig, 0, "cmd-1", plug);
  rig.relay.onEvent(1000, telemetry(1, 10));
  rig.relay.onEvent(1100, resultEvent(1, rig.sender.calls[0].number, false));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  TEST_ASSERT_FALSE(rig.relay.results()[0].ok);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandState::Failed), static_cast<int>(rig.log.slots[0].state));
  // A clamp answers every command with ok 0, so the hub sends it like any other and reports what the node says.
  uint8_t mac[kMacSize];
  macOfNumber(9, mac);
  uint8_t key[kNodeKeySize] = {9};
  TEST_ASSERT_EQUAL_INT(static_cast<int>(PairResult::Ok), static_cast<int>(rig.registry.pair(kKindClamp, mac, key)));
  char clamp[kNodeIdSize];
  TEST_ASSERT_TRUE(rig.registry.nodeId(mac, clamp));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 2000, "cmd-2", clamp)));
  rig.relay.onEvent(2100, telemetry(9, 4));
  TEST_ASSERT_EQUAL_UINT(2, rig.sender.count);
}

void test_the_same_id_is_never_queued_twice_and_an_id_that_ran_only_gets_its_result_again() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "cmd-1", plug)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Duplicate), result(submit(rig, 100, "cmd-1", plug)));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.pendingCount());
  rig.relay.onEvent(1000, telemetry(1, 10));
  rig.relay.onEvent(1100, resultEvent(1, rig.sender.calls[0].number, true));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  rig.relay.dropResults(1);
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.resultCount());
  // The backend sends the command again because its result was not reported yet. The plug is not asked again.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::AlreadyRan), result(submit(rig, 2000, "cmd-1", plug)));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  TEST_ASSERT_TRUE(rig.relay.results()[0].ok);
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::AlreadyRan), result(submit(rig, 2100, "cmd-1", plug)));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.pendingCount());
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
}

void test_a_newer_command_for_the_same_plug_takes_the_place_of_a_waiting_and_of_a_sent_one() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "cmd-a", plug, true)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 10, "cmd-b", plug, false)));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.pendingCount());
  rig.relay.onEvent(1000, telemetry(1, 10));
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT8(0, rig.sender.calls[0].action);
  const uint16_t second = rig.sender.calls[0].number;
  // A command that is in flight is replaced too, and its number is never sent again.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 2000, "cmd-c", plug, true)));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.pendingCount());
  rig.relay.update(1000 + kCommandResendMs);
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
  rig.relay.onEvent(7000, telemetry(1, 12));
  TEST_ASSERT_EQUAL_UINT(2, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT8(1, rig.sender.calls[1].action);
  TEST_ASSERT_TRUE(rig.sender.calls[1].number != second);
  // The result of the replaced command is not taken, and the replaced commands made no result.
  rig.relay.onEvent(7100, resultEvent(1, second, true));
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.pendingCount());
}

void test_a_command_without_life_is_refused_and_a_long_life_is_cut_to_ten_minutes_and_ends_without_a_result() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Expired), result(submit(rig, 0, "cmd-1", plug, true, kServerTime)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Expired), result(submit(rig, 0, "cmd-1", plug, true, kServerTime - 5)));
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.pendingCount());
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 1000, "cmd-2", plug, true, kServerTime + 3600)));
  rig.relay.update(1000 + kMaxCommandLifeSeconds * 1000 - 1);
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.pendingCount());
  rig.relay.update(1000 + kMaxCommandLifeSeconds * 1000);
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.pendingCount());
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.resultCount());
  // A frame after the end finds nothing to send.
  rig.relay.onEvent(1000 + kMaxCommandLifeSeconds * 1000 + 10, telemetry(1, 50));
  TEST_ASSERT_EQUAL_UINT(0, rig.sender.count);
}

void test_the_life_of_a_command_is_counted_across_the_wrap_of_the_clock() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  const uint32_t start = 0xFFFFFF00u;
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, start, "cmd-1", plug)));
  rig.relay.update(start + 100);
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.pendingCount());
  rig.relay.update(start + 600 * 1000u - 1);
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.pendingCount());
  rig.relay.update(start + 600 * 1000u);
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.pendingCount());
}

void test_a_command_for_an_unknown_node_or_with_a_bad_id_is_refused_without_a_change() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::UnknownNode), result(submit(rig, 0, "cmd-1", "PLUG-FFFFFF")));
  // The command can never run here, so the backend gets a result that is not ok and stops listing it.
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_STRING("cmd-1", rig.relay.results()[0].id);
  TEST_ASSERT_FALSE(rig.relay.results()[0].ok);
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::BadId), result(submit(rig, 0, "", plug)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::BadId), result(submit(rig, 0, "a b", plug)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::BadId), result(submit(rig, 0, "abcdefghijabcdefghijabcdefghijabcdefg", plug)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::BadId), result(submit(rig, 0, nullptr, plug)));
  // The same command again, as the backend sends it until the result is reported, only finds its result that waits, and makes no second one.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::AlreadyRan), result(submit(rig, 0, "cmd-1", nullptr)));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  // A bad id gets no result, because it is not safe to write into the JSON.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "abcdefghijabcdefghijabcdefghijabcdef", plug)));
  // A node that was removed keeps its id, but it takes no command.
  uint8_t mac[kMacSize];
  macOfNumber(1, mac);
  TEST_ASSERT_TRUE(rig.registry.unpair(mac));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::UnknownNode), result(submit(rig, 0, "cmd-2", plug)));
  TEST_ASSERT_EQUAL_UINT(2, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_STRING("cmd-2", rig.relay.results()[1].id);
  TEST_ASSERT_EQUAL_UINT(0, rig.sender.count);
  TEST_ASSERT_EQUAL_INT(0, rig.log.writes);
}

void test_a_command_that_was_sent_before_a_restart_is_sent_again_with_the_same_number() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  submit(rig, 0, "cmd-1", plug);
  rig.relay.onEvent(1000, telemetry(1, 10));
  const uint16_t number = rig.sender.calls[0].number;
  TEST_ASSERT_EQUAL_UINT16(0x0500, number);
  // The hub starts again with the next boot number, and the log is what it was.
  CommandRelay restarted(rig.sender, rig.registry, rig.log, 0x206);
  TEST_ASSERT_EQUAL_UINT(1, restarted.load());
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Resumed), result(restarted.submit(50, kServerTime + 30, "cmd-1", plug, true, kExpires)));
  restarted.onEvent(100, telemetry(1, 3, 4));
  TEST_ASSERT_EQUAL_UINT(2, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT16(number, rig.sender.calls[1].number);
  TEST_ASSERT_EQUAL_UINT32(4, rig.sender.calls[1].echoBootId);
  // The record exists already, so nothing is written for the resent frame.
  TEST_ASSERT_EQUAL_INT(1, rig.log.writes);
  restarted.onEvent(200, resultEvent(1, number, true));
  TEST_ASSERT_EQUAL_UINT(1, restarted.resultCount());
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandState::Done), static_cast<int>(rig.log.slots[0].state));
  // A new command of the new start has a number from the new start.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(restarted.submit(300, kServerTime + 40, "cmd-2", plug, true, kExpires)));
  restarted.onEvent(400, telemetry(1, 5, 4));
  TEST_ASSERT_EQUAL_UINT16(0x0600, rig.sender.calls[2].number);
}

void test_a_command_that_ran_before_a_restart_only_gets_its_result_again() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  submit(rig, 0, "cmd-1", plug);
  rig.relay.onEvent(1000, telemetry(1, 10));
  rig.relay.onEvent(1100, resultEvent(1, rig.sender.calls[0].number, false));
  CommandRelay restarted(rig.sender, rig.registry, rig.log, 0x206);
  TEST_ASSERT_EQUAL_UINT(1, restarted.load());
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::AlreadyRan), result(restarted.submit(0, kServerTime, "cmd-1", plug, true, kExpires)));
  TEST_ASSERT_EQUAL_UINT(1, restarted.resultCount());
  TEST_ASSERT_FALSE(restarted.results()[0].ok);
  TEST_ASSERT_EQUAL_UINT(0, restarted.pendingCount());
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
}

void test_load_holds_slots_that_cannot_be_read_or_are_damaged_and_a_record_that_has_ended_is_forgotten() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  CommandRecord good = {};
  strcpy(good.id, "old-1");
  good.number = 0x0A01;
  good.state = CommandState::Done;
  good.expiresAt = kServerTime - kNumberReserveSeconds - 1;
  rig.log.slots[0] = good;
  rig.log.present[0] = true;
  CommandRecord kept = good;
  strcpy(kept.id, "live-1");
  kept.number = 0x0A02;
  kept.expiresAt = kServerTime + 100;
  rig.log.slots[1] = kept;
  rig.log.present[1] = true;
  CommandRecord damaged = good;
  damaged.state = static_cast<CommandState>(9);
  strcpy(damaged.id, "bad-1");
  rig.log.slots[2] = damaged;
  rig.log.present[2] = true;
  CommandRecord badId = good;
  strcpy(badId.id, "a b");
  rig.log.slots[3] = badId;
  rig.log.present[3] = true;
  rig.log.unreadable[4] = true;
  TEST_ASSERT_EQUAL_UINT(2, rig.relay.load());
  // The state that is not known, the id that is not safe and the slot that the flash cannot read are all kept as used.
  TEST_ASSERT_EQUAL_UINT(3, rig.relay.unreadableSlots());
  // The record of "live-1" is still alive for the service, so it only gives its result again.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::AlreadyRan), result(submit(rig, 0, "live-1", plug)));
  // The record of "old-1" ended longer ago than a plug remembers a number, so the id is forgotten. A switch on with it waits, because a slot that is
  // lost may be that command. A switch off is queued.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::LogUnreadable), result(submit(rig, 0, "old-1", plug)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "old-1", plug, false)));
}

void test_an_unreadable_slot_is_kept_as_used_and_a_switch_on_for_an_unknown_id_waits_until_the_hold_ends() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  rig.log.unreadable[7] = true;
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.load());
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.unreadableSlots());
  // The first answer of the service starts the count.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::LogUnreadable), result(rig.relay.submit(0, kServerTime, "cmd-1", plug, true, kExpires)));
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.pendingCount());
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_INT(0, rig.log.writes);
  // A switch off for the same id goes on.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(rig.relay.submit(0, kServerTime, "cmd-1", plug, false, kExpires)));
  // One second before the end of the hold the switch on is still refused, and at the end the slot is free again.
  const uint32_t before = kServerTime + kUnreadableHoldSeconds - 1;
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::LogUnreadable), result(rig.relay.submit(0, before, "cmd-2", plug, true, before + 100)));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.unreadableSlots());
  const uint32_t after = kServerTime + kUnreadableHoldSeconds;
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(rig.relay.submit(0, after, "cmd-2", plug, true, after + 100)));
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.unreadableSlots());
}

void test_a_switch_on_for_an_id_that_the_log_knows_is_not_refused_when_another_slot_is_unreadable() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  CommandRecord live = {};
  strcpy(live.id, "live-1");
  live.number = 0x0A02;
  live.state = CommandState::Sent;
  live.expiresAt = kServerTime + 100;
  rig.log.slots[1] = live;
  rig.log.present[1] = true;
  rig.log.unreadable[5] = true;
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.load());
  // The record is readable, so the relay knows that the command was sent and sends it again with the same number.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Resumed), result(submit(rig, 0, "live-1", plug)));
}

void test_an_unreadable_slot_is_never_written_and_does_not_count_as_room() {
  Rig rig;
  char plug1[kNodeIdSize];
  char plug2[kNodeIdSize];
  pairPlug(rig, 1, plug1);
  pairPlug(rig, 2, plug2);
  for (size_t i = 0; i < kCommandLogSlots - 1; i++) {
    rig.log.unreadable[i] = true;
  }
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.load());
  TEST_ASSERT_EQUAL_UINT(kCommandLogSlots - 1, rig.relay.unreadableSlots());
  // One slot is left. The first command gets it. The second switch off finds no slot and is taken anyway, because the fault took the room.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "off-1", plug1, false)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "off-2", plug2, false)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::LogUnreadable), result(submit(rig, 0, "on-3", plug1, true)));
  rig.relay.onEvent(1000, telemetry(1, 10));
  TEST_ASSERT_EQUAL_INT(1, rig.log.writes);
  TEST_ASSERT_TRUE(rig.log.present[kCommandLogSlots - 1]);
  for (size_t i = 0; i < kCommandLogSlots - 1; i++) {
    TEST_ASSERT_FALSE(rig.log.present[i]);
  }
  // The second command has no record, and its frame goes out all the same. The failure is counted.
  rig.relay.onEvent(1100, telemetry(2, 10));
  TEST_ASSERT_EQUAL_UINT(2, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT32(1, rig.relay.logFailures());
}

void test_a_switch_off_is_taken_when_every_slot_is_unreadable_and_a_switch_on_is_not() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  for (size_t i = 0; i < kCommandLogSlots; i++) {
    rig.log.unreadable[i] = true;
  }
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.load());
  TEST_ASSERT_EQUAL_UINT(kCommandLogSlots, rig.relay.unreadableSlots());
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::LogUnreadable), result(submit(rig, 0, "on-1", plug, true)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "off-1", plug, false)));
  rig.relay.onEvent(1000, telemetry(1, 10));
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT8(0, rig.sender.calls[0].action);
}

void test_the_end_of_the_hold_writes_a_free_record_so_the_next_start_does_not_hold_again() {
  Rig rig;
  rig.log.unreadable[2] = true;
  rig.log.unreadable[9] = true;
  rig.relay.load();
  rig.relay.forgetEnded(kServerTime);
  TEST_ASSERT_EQUAL_INT(0, rig.log.writes);
  rig.relay.forgetEnded(kServerTime + kUnreadableHoldSeconds);
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.unreadableSlots());
  TEST_ASSERT_EQUAL_INT(2, rig.log.writes);
  // The same flash, read by a relay of the next start.
  CommandRelay restarted(rig.sender, rig.registry, rig.log, 0x206);
  TEST_ASSERT_EQUAL_UINT(0, restarted.load());
  TEST_ASSERT_EQUAL_UINT(0, restarted.unreadableSlots());
}

void test_a_slot_that_cannot_be_written_does_not_hold_back_a_switch_on_when_another_slot_is_free() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  rig.log.failWriteSlot = 0;
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "on-1", plug, true)));
  rig.relay.onEvent(1000, telemetry(1, 10));
  // The first slot refused the record, the second took it, and the switch on went out.
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT32(1, rig.relay.logFailures());
  TEST_ASSERT_FALSE(rig.log.present[0]);
  TEST_ASSERT_TRUE(rig.log.present[1]);
  TEST_ASSERT_EQUAL_STRING("on-1", rig.log.slots[1].id);
}

void test_the_hold_of_an_unreadable_slot_is_counted_from_the_first_time_of_the_service_also_without_a_command() {
  Rig rig;
  rig.log.unreadable[0] = true;
  rig.relay.load();
  // An answer with no command still starts the count, and a time before it does not end it.
  rig.relay.forgetEnded(kServerTime);
  rig.relay.forgetEnded(kServerTime - 5);
  rig.relay.forgetEnded(kServerTime + kUnreadableHoldSeconds - 1);
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.unreadableSlots());
  rig.relay.forgetEnded(kServerTime + kUnreadableHoldSeconds);
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.unreadableSlots());
}

void test_numbers_count_from_the_boot_number_and_skip_the_ones_that_are_still_held() {
  Rig rig(0x1FF);
  CommandRecord held = {};
  strcpy(held.id, "held-1");
  held.number = 0xFF01;
  held.state = CommandState::Sent;
  held.expiresAt = kServerTime + 300;
  rig.log.slots[5] = held;
  rig.log.present[5] = true;
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.load());
  uint16_t numbers[3];
  for (uint8_t i = 0; i < 3; i++) {
    char plug[kNodeIdSize];
    pairPlug(rig, static_cast<uint8_t>(i + 1), plug);
    char id[16];
    snprintf(id, sizeof(id), "cmd-%u", static_cast<unsigned>(i));
    TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, id, plug)));
    rig.relay.onEvent(10, telemetry(static_cast<uint8_t>(i + 1), 1));
    numbers[i] = rig.sender.calls[i].number;
  }
  TEST_ASSERT_EQUAL_UINT16(0xFF00, numbers[0]);
  TEST_ASSERT_EQUAL_UINT16(0xFF02, numbers[1]);
  TEST_ASSERT_EQUAL_UINT16(0xFF03, numbers[2]);
}

void test_a_flash_that_refuses_the_record_holds_back_a_switch_on_but_not_a_switch_off() {
  Rig rig;
  char plug[kNodeIdSize];
  char other[kNodeIdSize];
  pairPlug(rig, 1, plug);
  pairPlug(rig, 2, other);
  rig.log.failWrites = true;
  submit(rig, 0, "cmd-on", plug, true);
  submit(rig, 0, "cmd-off", other, false);
  rig.relay.onEvent(1000, telemetry(1, 10));
  TEST_ASSERT_EQUAL_UINT(0, rig.sender.count);
  // The record is tried in two slots, and both writes are counted.
  TEST_ASSERT_EQUAL_UINT32(2, rig.relay.logFailures());
  // Nothing is resent for a command that never went out, and the next frame tries again.
  rig.relay.update(1000 + 2 * kCommandResendMs);
  TEST_ASSERT_EQUAL_UINT(0, rig.sender.count);
  rig.log.failWrites = false;
  rig.relay.onEvent(20000, telemetry(1, 11));
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT8(1, rig.sender.calls[0].action);
  // A switch off goes out although the record cannot be written, because running it twice does no harm.
  rig.log.failWrites = true;
  rig.relay.onEvent(21000, telemetry(2, 3));
  TEST_ASSERT_EQUAL_UINT(2, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT8(0, rig.sender.calls[1].action);
  TEST_ASSERT_EQUAL_UINT32(4, rig.relay.logFailures());
  // The result is still reported when its record cannot be written.
  rig.relay.onEvent(22000, resultEvent(2, rig.sender.calls[1].number, true));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_UINT32(6, rig.relay.logFailures());
}

void test_a_radio_that_refuses_the_frame_does_not_stop_the_resends() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  submit(rig, 0, "cmd-1", plug);
  rig.sender.fail = true;
  rig.relay.onEvent(1000, telemetry(1, 10));
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
  rig.relay.update(1000 + kCommandResendMs);
  TEST_ASSERT_EQUAL_UINT(2, rig.sender.count);
  rig.sender.fail = false;
  rig.relay.update(1000 + 2 * kCommandResendMs);
  TEST_ASSERT_EQUAL_UINT(3, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT16(rig.sender.calls[0].number, rig.sender.calls[2].number);
}

void test_the_queue_holds_sixteen_commands_and_a_command_for_a_queued_plug_still_fits() {
  Rig rig;
  char ids[kMaxQueuedCommands + 1][kNodeIdSize];
  for (size_t i = 0; i <= kMaxQueuedCommands; i++) {
    pairPlug(rig, static_cast<uint8_t>(i + 1), ids[i]);
  }
  for (size_t i = 0; i < kMaxQueuedCommands; i++) {
    char id[16];
    snprintf(id, sizeof(id), "cmd-%u", static_cast<unsigned>(i));
    TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, id, ids[i])));
  }
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Full), result(submit(rig, 0, "cmd-x", ids[kMaxQueuedCommands])));
  TEST_ASSERT_EQUAL_UINT(kMaxQueuedCommands, rig.relay.pendingCount());
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "cmd-y", ids[3])));
  TEST_ASSERT_EQUAL_UINT(kMaxQueuedCommands, rig.relay.pendingCount());
}

// Runs one command of a plug through to its result, so the result waits for a batch.
void finishCommand(Rig &rig, uint8_t number, const char *id, uint32_t expiresAt, uint32_t counter) {
  char plug[kNodeIdSize];
  uint8_t mac[kMacSize];
  macOfNumber(number, mac);
  TEST_ASSERT_TRUE(rig.registry.nodeId(mac, plug));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(rig.relay.submit(0, kServerTime, id, plug, true, expiresAt)));
  rig.relay.onEvent(10, telemetry(number, counter));
  rig.relay.onEvent(20, resultEvent(number, rig.sender.calls[rig.sender.count - 1].number, true));
}

void test_a_batch_takes_sixty_four_results_and_the_rest_waits_and_drop_keeps_the_newer() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  for (uint32_t i = 0; i < 70; i++) {
    char id[16];
    snprintf(id, sizeof(id), "c%u", static_cast<unsigned>(i));
    finishCommand(rig, 1, id, kExpires + i, 10 + i);
  }
  TEST_ASSERT_EQUAL_UINT(kMaxBatchResults, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_STRING("c0", rig.relay.results()[0].id);
  TEST_ASSERT_EQUAL_STRING("c63", rig.relay.results()[kMaxBatchResults - 1].id);
  // The batch carries the first 64, and a result that comes in meanwhile does not move them.
  finishCommand(rig, 1, "c70", kExpires + 70, 90);
  TEST_ASSERT_EQUAL_STRING("c0", rig.relay.results()[0].id);
  TEST_ASSERT_EQUAL_STRING("c63", rig.relay.results()[kMaxBatchResults - 1].id);
  rig.relay.dropResults(kMaxBatchResults);
  TEST_ASSERT_EQUAL_UINT(7, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_STRING("c64", rig.relay.results()[0].id);
  TEST_ASSERT_EQUAL_STRING("c70", rig.relay.results()[6].id);
  rig.relay.dropResults(1000);
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.resultCount());
}

void test_a_full_log_refuses_a_new_command_until_the_records_are_forgotten_whatever_happened_to_their_results() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  for (uint32_t i = 0; i < kCommandLogSlots; i++) {
    char id[16];
    snprintf(id, sizeof(id), "c%u", static_cast<unsigned>(i));
    finishCommand(rig, 1, id, kExpires + i, 10 + i);
  }
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Full), result(submit(rig, 0, "next-1", plug)));
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.pendingCount());
  // Passing the results on does not free a slot, because a plug may still hold the numbers.
  rig.relay.dropResults(kMaxBatchResults);
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Full), result(submit(rig, 0, "next-1", plug)));
  // When the time of the service is past the end of the first commands and the reserve of their numbers, those records are forgotten, and only those
  // whose results were passed on. The records whose result still waits stay.
  const uint32_t later = kExpires + 10 + kNumberReserveSeconds + 1;
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(rig.relay.submit(0, later, "next-1", plug, true, later + 100)));
  rig.relay.onEvent(30, telemetry(1, 500));
  TEST_ASSERT_EQUAL_STRING("next-1", rig.log.slots[0].id);
  TEST_ASSERT_EQUAL_STRING("c1", rig.log.slots[1].id);
  TEST_ASSERT_EQUAL_INT(static_cast<int>(CommandState::Sent), static_cast<int>(rig.log.slots[0].state));
  TEST_ASSERT_EQUAL_STRING("c70", rig.log.slots[70].id);
}

void test_a_command_whose_unknown_node_result_waits_is_not_run_when_the_node_pairs_meanwhile() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  uint8_t mac[kMacSize];
  macOfNumber(1, mac);
  TEST_ASSERT_TRUE(rig.registry.unpair(mac));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::UnknownNode), result(submit(rig, 0, "cmd-1", plug)));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  // The node pairs again while the failed result is on its way, and the backend lists the command again.
  char again[kNodeIdSize];
  pairPlug(rig, 1, again);
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::AlreadyRan), result(submit(rig, 100, "cmd-1", plug)));
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.pendingCount());
  rig.relay.onEvent(200, telemetry(1, 5));
  TEST_ASSERT_EQUAL_UINT(0, rig.sender.count);
  TEST_ASSERT_FALSE(rig.relay.results()[0].ok);
  // After the result was passed on, the backend lists nothing more, and a new id for the plug is a new command.
  rig.relay.dropResults(1);
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 300, "cmd-2", plug)));
}

void test_records_of_commands_that_were_sent_before_a_restart_are_never_reused_for_a_new_command() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  for (size_t i = 0; i < kCommandLogSlots; i++) {
    CommandRecord record = {};
    snprintf(record.id, sizeof(record.id), "sent-%u", static_cast<unsigned>(i));
    record.number = static_cast<uint16_t>(0x0A00 + i + 1);
    record.state = CommandState::Sent;
    record.expiresAt = kServerTime + 100;
    rig.log.slots[i] = record;
    rig.log.present[i] = true;
  }
  TEST_ASSERT_EQUAL_UINT(kCommandLogSlots, rig.relay.load());
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Full), result(submit(rig, 0, "new-1", plug)));
  // A command that the backend lists again is still found.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Resumed), result(submit(rig, 0, "sent-3", plug)));
}

void test_a_number_stays_reserved_after_its_command_ended_and_the_number_zero_is_never_used() {
  Rig rig(0x1FF);
  CommandRecord ended = {};
  strcpy(ended.id, "ended-1");
  ended.number = 0xFF01;
  ended.state = CommandState::Done;
  ended.expiresAt = kServerTime - 100;
  rig.log.slots[0] = ended;
  rig.log.present[0] = true;
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.load());
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  // The command ended 100 seconds ago, a plug remembers the number for 600 seconds more, so 0xFF01 is skipped.
  submit(rig, 0, "cmd-1", plug);
  rig.relay.onEvent(10, telemetry(1, 1));
  TEST_ASSERT_EQUAL_UINT16(0xFF00, rig.sender.calls[0].number);
  rig.relay.onEvent(20, resultEvent(1, 0xFF00, true));
  submit(rig, 30, "cmd-2", plug);
  rig.relay.onEvent(40, telemetry(1, 2));
  TEST_ASSERT_EQUAL_UINT16(0xFF02, rig.sender.calls[1].number);
  // A boot number with the low byte 0 would give the number 0 first, and the relay goes on to 1.
  Rig zero(0x100);
  char other[kNodeIdSize];
  pairPlug(zero, 1, other);
  submit(zero, 0, "cmd-1", other);
  zero.relay.onEvent(10, telemetry(1, 1));
  TEST_ASSERT_EQUAL_UINT16(0x0001, zero.sender.calls[0].number);
}

void test_the_last_free_log_slot_is_promised_to_one_queued_command_only() {
  Rig rig;
  char first[kNodeIdSize];
  char second[kNodeIdSize];
  pairPlug(rig, 1, first);
  pairPlug(rig, 2, second);
  // All slots but one hold the record of a command that was sent before a restart, and those are never reused.
  for (size_t i = 0; i + 1 < kCommandLogSlots; i++) {
    CommandRecord record = {};
    snprintf(record.id, sizeof(record.id), "sent-%u", static_cast<unsigned>(i));
    record.number = static_cast<uint16_t>(0x0A00 + i + 1);
    record.state = CommandState::Sent;
    record.expiresAt = kServerTime + 100;
    rig.log.slots[i] = record;
    rig.log.present[i] = true;
  }
  TEST_ASSERT_EQUAL_UINT(kCommandLogSlots - 1, rig.relay.load());
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "new-1", first)));
  // The one free slot is taken by the first command in advance, so a second command for another plug is refused now and not silently held back later.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Full), result(submit(rig, 0, "new-2", second)));
  // A newer command for the same plug takes the place of the first one, so it needs no second slot.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "new-3", first)));
  rig.relay.onEvent(10, telemetry(1, 1));
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT32(0, rig.relay.logFailures());
}

void test_an_unknown_node_with_no_id_does_not_turn_a_result_of_a_command_that_ran_into_a_failure() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  finishCommand(rig, 1, "ran-1", kExpires, 10);
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  TEST_ASSERT_TRUE(rig.relay.results()[0].ok);
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::AlreadyRan), result(submit(rig, 0, "ran-1", nullptr)));
  TEST_ASSERT_TRUE(rig.relay.results()[0].ok);
  // A command that is queued stays queued, and no failure is reported for it.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "wait-1", plug)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Duplicate), result(submit(rig, 0, "wait-1", nullptr)));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.pendingCount());
}

void test_answers_about_unknown_nodes_are_capped_and_never_push_out_a_result_that_a_batch_took() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  for (size_t i = 0; i < kMaxBatchResults; i++) {
    char id[16];
    snprintf(id, sizeof(id), "u%u", static_cast<unsigned>(i));
    TEST_ASSERT_EQUAL_INT(result(SubmitResult::UnknownNode), result(submit(rig, 0, id, "PLUG-FFFFFF")));
  }
  TEST_ASSERT_EQUAL_UINT(kMaxBatchResults, rig.relay.resultCount());
  // The share is used, so a new one is refused and the batch that took the first 64 still sees the same entries.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Full), result(submit(rig, 0, "u-more", "PLUG-FFFFFF")));
  TEST_ASSERT_EQUAL_STRING("u0", rig.relay.results()[0].id);
  TEST_ASSERT_EQUAL_STRING("u63", rig.relay.results()[kMaxBatchResults - 1].id);
  // The results of commands that ran still find room on top of that share, as many as the log has records.
  for (size_t i = 0; i < kCommandLogSlots; i++) {
    char id[16];
    snprintf(id, sizeof(id), "r%u", static_cast<unsigned>(i));
    finishCommand(rig, 1, id, kExpires + i, 10 + i);
  }
  TEST_ASSERT_EQUAL_STRING("u0", rig.relay.results()[0].id);
  rig.relay.dropResults(kMaxBatchResults);
  // The share of unknown nodes is free again, and the results of the commands that ran are the first ones now.
  TEST_ASSERT_EQUAL_STRING("r0", rig.relay.results()[0].id);
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::UnknownNode), result(submit(rig, 0, "u-more", "PLUG-FFFFFF")));
  rig.relay.dropResults(1000);
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.resultCount());
}

void test_a_record_whose_result_still_waits_is_kept_after_its_command_and_its_number_reserve_ended() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  finishCommand(rig, 1, "ran-1", kExpires, 10);
  // A long outage. The time of the service is far past the end of the command and of the reserve of its number, and the result was never passed on.
  const uint32_t later = kExpires + kNumberReserveSeconds + 3600;
  rig.relay.forgetEnded(later);
  TEST_ASSERT_EQUAL_STRING("ran-1", rig.log.slots[0].id);
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  // The relay still knows the command, so the backend that lists it again only gets its result and the plug is not asked.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::AlreadyRan), result(rig.relay.submit(0, later, "ran-1", plug, true, later + 100)));
  // After the batch is acknowledged the record is not needed any more, and the next call forgets it.
  rig.relay.dropResults(1);
  rig.relay.forgetEnded(later);
  CommandRelay restarted(rig.sender, rig.registry, rig.log, 0x206);
  TEST_ASSERT_EQUAL_UINT(1, restarted.load());
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(restarted.submit(0, later, "fresh-1", plug, true, later + 100)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(rig.relay.submit(0, later, "fresh-2", plug, true, later + 100)));
}

void test_a_command_whose_life_is_over_is_not_sent_when_a_frame_comes_before_update() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  submit(rig, 1000, "cmd-1", plug);
  rig.relay.onEvent(1000 + kMaxCommandLifeSeconds * 1000 + 1, telemetry(1, 50));
  TEST_ASSERT_EQUAL_UINT(0, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.pendingCount());
}

void test_a_command_is_not_sent_to_a_node_that_was_removed_and_forget_ends_it_for_a_pairing_that_comes_back() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  uint8_t mac[kMacSize];
  macOfNumber(1, mac);
  submit(rig, 0, "cmd-1", plug);
  TEST_ASSERT_TRUE(rig.registry.unpair(mac));
  rig.relay.onEvent(100, telemetry(1, 5));
  TEST_ASSERT_EQUAL_UINT(0, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.pendingCount());
  // A command for the old pairing must not reach the node when it is paired again, so the unpair path calls forget().
  char again[kNodeIdSize];
  pairPlug(rig, 1, again);
  submit(rig, 200, "cmd-2", again);
  rig.relay.forget(mac);
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.pendingCount());
  rig.relay.onEvent(300, telemetry(1, 6));
  TEST_ASSERT_EQUAL_UINT(0, rig.sender.count);
}

void test_resends_stop_when_the_newest_frame_is_too_old_and_start_again_with_the_next_frame() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  submit(rig, 0, "cmd-1", plug);
  rig.relay.onEvent(1000, telemetry(1, 10));
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
  for (uint32_t t = 1000 + kCommandResendMs; t < 1000 + kEchoUsableMs; t += kCommandResendMs) {
    rig.relay.update(t);
  }
  const size_t beforeLimit = rig.sender.count;
  TEST_ASSERT_EQUAL_UINT(10, beforeLimit);
  rig.relay.update(1000 + kEchoUsableMs);
  rig.relay.update(1000 + kEchoUsableMs + kCommandResendMs);
  TEST_ASSERT_EQUAL_UINT(beforeLimit, rig.sender.count);
  // The next frame of the plug is a new echo, and the resends go on.
  rig.relay.onEvent(60000, telemetry(1, 20));
  rig.relay.update(60000);
  TEST_ASSERT_EQUAL_UINT(beforeLimit + 1, rig.sender.count);
  TEST_ASSERT_EQUAL_UINT32(20, rig.sender.calls[beforeLimit].echoCounter);
}

void test_a_paired_node_event_sends_the_command_and_a_result_before_the_first_send_is_ignored() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  submit(rig, 0, "cmd-1", plug);
  rig.relay.onEvent(50, resultEvent(1, 0x0500, true));
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.pendingCount());
  LinkEvent paired = telemetry(1, 0);
  paired.kind = LinkEventKind::PairedNode;
  rig.relay.onEvent(100, paired);
  TEST_ASSERT_EQUAL_UINT(1, rig.sender.count);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_a_queued_command_waits_for_a_frame_of_its_plug_and_goes_out_with_the_echo_of_that_frame);
  RUN_TEST(test_a_command_is_sent_again_every_five_seconds_with_the_echo_of_the_newest_frame);
  RUN_TEST(test_a_result_with_the_number_of_the_command_closes_it_and_is_kept_for_the_batch);
  RUN_TEST(test_a_plug_that_could_not_switch_gives_a_result_that_is_not_ok_and_a_clamp_gets_the_command_too);
  RUN_TEST(test_the_same_id_is_never_queued_twice_and_an_id_that_ran_only_gets_its_result_again);
  RUN_TEST(test_a_newer_command_for_the_same_plug_takes_the_place_of_a_waiting_and_of_a_sent_one);
  RUN_TEST(test_a_command_without_life_is_refused_and_a_long_life_is_cut_to_ten_minutes_and_ends_without_a_result);
  RUN_TEST(test_the_life_of_a_command_is_counted_across_the_wrap_of_the_clock);
  RUN_TEST(test_a_command_for_an_unknown_node_or_with_a_bad_id_is_refused_without_a_change);
  RUN_TEST(test_a_command_that_was_sent_before_a_restart_is_sent_again_with_the_same_number);
  RUN_TEST(test_a_command_that_ran_before_a_restart_only_gets_its_result_again);
  RUN_TEST(test_load_holds_slots_that_cannot_be_read_or_are_damaged_and_a_record_that_has_ended_is_forgotten);
  RUN_TEST(test_an_unreadable_slot_is_kept_as_used_and_a_switch_on_for_an_unknown_id_waits_until_the_hold_ends);
  RUN_TEST(test_a_switch_on_for_an_id_that_the_log_knows_is_not_refused_when_another_slot_is_unreadable);
  RUN_TEST(test_an_unreadable_slot_is_never_written_and_does_not_count_as_room);
  RUN_TEST(test_the_hold_of_an_unreadable_slot_is_counted_from_the_first_time_of_the_service_also_without_a_command);
  RUN_TEST(test_a_switch_off_is_taken_when_every_slot_is_unreadable_and_a_switch_on_is_not);
  RUN_TEST(test_the_end_of_the_hold_writes_a_free_record_so_the_next_start_does_not_hold_again);
  RUN_TEST(test_a_slot_that_cannot_be_written_does_not_hold_back_a_switch_on_when_another_slot_is_free);
  RUN_TEST(test_numbers_count_from_the_boot_number_and_skip_the_ones_that_are_still_held);
  RUN_TEST(test_a_flash_that_refuses_the_record_holds_back_a_switch_on_but_not_a_switch_off);
  RUN_TEST(test_a_radio_that_refuses_the_frame_does_not_stop_the_resends);
  RUN_TEST(test_the_queue_holds_sixteen_commands_and_a_command_for_a_queued_plug_still_fits);
  RUN_TEST(test_a_batch_takes_sixty_four_results_and_the_rest_waits_and_drop_keeps_the_newer);
  RUN_TEST(test_a_full_log_refuses_a_new_command_until_the_records_are_forgotten_whatever_happened_to_their_results);
  RUN_TEST(test_a_command_whose_unknown_node_result_waits_is_not_run_when_the_node_pairs_meanwhile);
  RUN_TEST(test_records_of_commands_that_were_sent_before_a_restart_are_never_reused_for_a_new_command);
  RUN_TEST(test_a_number_stays_reserved_after_its_command_ended_and_the_number_zero_is_never_used);
  RUN_TEST(test_the_last_free_log_slot_is_promised_to_one_queued_command_only);
  RUN_TEST(test_an_unknown_node_with_no_id_does_not_turn_a_result_of_a_command_that_ran_into_a_failure);
  RUN_TEST(test_answers_about_unknown_nodes_are_capped_and_never_push_out_a_result_that_a_batch_took);
  RUN_TEST(test_a_record_whose_result_still_waits_is_kept_after_its_command_and_its_number_reserve_ended);
  RUN_TEST(test_a_command_whose_life_is_over_is_not_sent_when_a_frame_comes_before_update);
  RUN_TEST(test_a_command_is_not_sent_to_a_node_that_was_removed_and_forget_ends_it_for_a_pairing_that_comes_back);
  RUN_TEST(test_resends_stop_when_the_newest_frame_is_too_old_and_start_again_with_the_next_frame);
  RUN_TEST(test_a_paired_node_event_sends_the_command_and_a_result_before_the_first_send_is_ignored);
  return UNITY_END();
}
