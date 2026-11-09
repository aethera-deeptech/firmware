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
  bool read(size_t slot, CommandRecord *out) override {
    if (!present[slot]) {
      return false;
    }
    *out = slots[slot];
    return true;
  }

  bool write(size_t slot, const CommandRecord &record) override {
    writes++;
    if (failWrites) {
      return false;
    }
    slots[slot] = record;
    present[slot] = true;
    return true;
  }

  CommandRecord slots[kCommandLogSlots] = {};
  bool present[kCommandLogSlots] = {};
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
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::BadId), result(submit(rig, 0, "", plug)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::BadId), result(submit(rig, 0, "a b", plug)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::BadId), result(submit(rig, 0, "abcdefghijabcdefghijabcdefghijabcdefg", plug)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::BadId), result(submit(rig, 0, nullptr, plug)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::UnknownNode), result(submit(rig, 0, "cmd-1", nullptr)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "abcdefghijabcdefghijabcdefghijabcdef", plug)));
  // A node that was removed keeps its id, but it takes no command.
  uint8_t mac[kMacSize];
  macOfNumber(1, mac);
  TEST_ASSERT_TRUE(rig.registry.unpair(mac));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::UnknownNode), result(submit(rig, 0, "cmd-2", plug)));
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

void test_load_skips_slots_that_cannot_be_read_or_are_damaged_and_a_record_that_has_ended_is_forgotten() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  CommandRecord good = {};
  strcpy(good.id, "old-1");
  good.number = 0x0A01;
  good.state = CommandState::Done;
  good.expiresAt = kServerTime - 1;
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
  TEST_ASSERT_EQUAL_UINT(2, rig.relay.load());
  // The record of "live-1" is still alive for the service, so it only gives its result again.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::AlreadyRan), result(submit(rig, 0, "live-1", plug)));
  // The record of "old-1" had ended before the time of the service, so the id is forgotten and a command with it is new.
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "old-1", plug)));
  TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(submit(rig, 0, "bad-1", plug)));
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
  TEST_ASSERT_EQUAL_UINT32(1, rig.relay.logFailures());
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
  TEST_ASSERT_EQUAL_UINT32(2, rig.relay.logFailures());
  // The result is still reported when its record cannot be written.
  rig.relay.onEvent(22000, resultEvent(2, rig.sender.calls[1].number, true));
  TEST_ASSERT_EQUAL_UINT(1, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_UINT32(3, rig.relay.logFailures());
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

void test_the_results_are_capped_at_sixty_four_with_the_oldest_dropped_first_and_drop_keeps_the_newer() {
  Rig rig;
  char plug[kNodeIdSize];
  pairPlug(rig, 1, plug);
  for (uint32_t i = 0; i < 70; i++) {
    char id[16];
    snprintf(id, sizeof(id), "c%u", static_cast<unsigned>(i));
    TEST_ASSERT_EQUAL_INT(result(SubmitResult::Queued), result(rig.relay.submit(0, kServerTime, id, plug, true, kExpires + i)));
    rig.relay.onEvent(10, telemetry(1, 10 + i));
    rig.relay.onEvent(20, resultEvent(1, rig.sender.calls[i].number, true));
  }
  TEST_ASSERT_EQUAL_UINT(kMaxBatchResults, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_STRING("c6", rig.relay.results()[0].id);
  TEST_ASSERT_EQUAL_STRING("c69", rig.relay.results()[kMaxBatchResults - 1].id);
  rig.relay.dropResults(10);
  TEST_ASSERT_EQUAL_UINT(kMaxBatchResults - 10, rig.relay.resultCount());
  TEST_ASSERT_EQUAL_STRING("c16", rig.relay.results()[0].id);
  rig.relay.dropResults(1000);
  TEST_ASSERT_EQUAL_UINT(0, rig.relay.resultCount());
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
  RUN_TEST(test_load_skips_slots_that_cannot_be_read_or_are_damaged_and_a_record_that_has_ended_is_forgotten);
  RUN_TEST(test_numbers_count_from_the_boot_number_and_skip_the_ones_that_are_still_held);
  RUN_TEST(test_a_flash_that_refuses_the_record_holds_back_a_switch_on_but_not_a_switch_off);
  RUN_TEST(test_a_radio_that_refuses_the_frame_does_not_stop_the_resends);
  RUN_TEST(test_the_queue_holds_sixteen_commands_and_a_command_for_a_queued_plug_still_fits);
  RUN_TEST(test_the_results_are_capped_at_sixty_four_with_the_oldest_dropped_first_and_drop_keeps_the_newer);
  return UNITY_END();
}
