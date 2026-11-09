#include "command-relay.h"

#include <string.h>

namespace wattlink {

namespace {

bool sameMac(const uint8_t a[kMacSize], const uint8_t b[kMacSize]) {
  return memcmp(a, b, kMacSize) == 0;
}

// A time that is reached when the signed difference is not negative, so it works across the wrap of the clock.
bool reached(uint32_t nowMs, uint32_t targetMs) {
  return static_cast<int32_t>(nowMs - targetMs) >= 0;
}

}  // namespace

CommandRelay::CommandRelay(CommandSender &sender, const NodeRegistry &registry, CommandLog &log, uint32_t hubBootId)
    : sender_(sender), registry_(registry), log_(log), hubBootId_(hubBootId), nextSeq_(0), logFailures_(0), unreadableTimed_(false), unreadableFrom_(0), resultCount_(0), unknownCount_(0) {
  memset(pending_, 0, sizeof(pending_));
  memset(records_, 0, sizeof(records_));
  memset(unreadable_, 0, sizeof(unreadable_));
  memset(results_, 0, sizeof(results_));
  memset(unknown_, 0, sizeof(unknown_));
}

size_t CommandRelay::load() {
  size_t held = 0;
  for (size_t i = 0; i < kCommandLogSlots; i++) {
    CommandRecord record;
    memset(&record, 0, sizeof(record));
    const LogRead found = log_.read(i, &record);
    memset(&records_[i], 0, sizeof(records_[i]));
    unreadable_[i] = false;
    if (found == LogRead::Record && record.state >= CommandState::Sent && record.state <= CommandState::Failed && isSafeResultId(record.id)) {
      records_[i] = record;
      held++;
    } else if (found == LogRead::Unreadable || found == LogRead::Record) {
      // A slot that cannot be read, or that holds a state or an id that this code does not accept, may hold a command that ran. It is kept as used and
      // not as free, and a switch on is refused while it lasts. A record in the state Free is a slot that was never used.
      unreadable_[i] = found == LogRead::Unreadable || record.state != CommandState::Free;
    }
  }
  return held;
}

int CommandRelay::findPending(const char *id) const {
  for (size_t i = 0; i < kMaxQueuedCommands; i++) {
    if (pending_[i].used && strncmp(pending_[i].id, id, kResultIdSize) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int CommandRelay::findLog(const char *id) const {
  for (size_t i = 0; i < kCommandLogSlots; i++) {
    if (records_[i].state != CommandState::Free && strncmp(records_[i].id, id, kResultIdSize) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool CommandRelay::resultWaiting(const char *id) const {
  for (size_t i = 0; i < resultCount_; i++) {
    if (strncmp(results_[i].id, id, kResultIdSize) == 0) {
      return true;
    }
  }
  return false;
}

size_t CommandRelay::usableLogSlots() const {
  size_t usable = 0;
  for (size_t i = 0; i < kCommandLogSlots; i++) {
    if (records_[i].state == CommandState::Free && !unreadable_[i]) {
      usable++;
    }
  }
  return usable;
}

// The queued commands that still need a slot for their record, not counting the one that a new command for the same plug would replace.
size_t CommandRelay::unpersistedExcept(const uint8_t mac[kMacSize]) const {
  size_t count = 0;
  for (size_t i = 0; i < kMaxQueuedCommands; i++) {
    if (pending_[i].used && !pending_[i].persisted && !sameMac(pending_[i].mac, mac)) {
      count++;
    }
  }
  return count;
}

int CommandRelay::pickLogSlot() const {
  // Only a free slot. A record is never taken before it is forgotten, which is when the life of its command and the time that a plug remembers its number
  // are over, because the number of a record that is gone could be given to a new command while the plug still holds it, and then the plug would not run
  // the new command. The cost is that a burst of commands can fill the log, and then a new command is refused until the oldest records are forgotten.
  for (size_t i = 0; i < kCommandLogSlots; i++) {
    if (records_[i].state == CommandState::Free && !unreadable_[i]) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool CommandRelay::numberInUse(uint16_t number) const {
  for (size_t i = 0; i < kMaxQueuedCommands; i++) {
    if (pending_[i].used && pending_[i].number == number) {
      return true;
    }
  }
  for (size_t i = 0; i < kCommandLogSlots; i++) {
    if (records_[i].state != CommandState::Free && records_[i].number == number) {
      return true;
    }
  }
  return false;
}

bool CommandRelay::allocateNumber(uint16_t *number) {
  // The high byte is the low byte of the boot number, so a start gets numbers that the plug did not see in the last start, and the low byte counts.
  // A number that a queued command or a record in the log still holds is skipped.
  for (unsigned tries = 0; tries < 256; tries++) {
    const uint16_t candidate = static_cast<uint16_t>(((hubBootId_ & 0xFFu) << 8) | nextSeq_);
    nextSeq_ = static_cast<uint8_t>(nextSeq_ + 1);
    // The number 0 is never used, because a plug may treat it as an empty place in its list of numbers.
    if (candidate != 0 && !numberInUse(candidate)) {
      *number = candidate;
      return true;
    }
  }
  return false;
}

bool CommandRelay::persist(const Pending &pending, CommandState state) {
  int slot = findLog(pending.id);
  if (slot < 0) {
    slot = pickLogSlot();
  }
  if (slot < 0) {
    // submit() counts the slots, so this is not expected. It is counted so that it shows in the log if it ever happens.
    logFailures_++;
    return false;
  }
  CommandRecord record;
  memset(&record, 0, sizeof(record));
  memcpy(record.id, pending.id, sizeof(record.id) - 1);
  record.number = pending.number;
  record.state = state;
  record.expiresAt = pending.expiresAt;
  if (!log_.write(static_cast<size_t>(slot), record)) {
    logFailures_++;
    return false;
  }
  records_[slot] = record;
  return true;
}

SubmitResult CommandRelay::submit(uint32_t nowMs, uint32_t serverTime, const char *id, const char *nodeId, bool on, uint32_t expiresAt) {
  if (id == nullptr || !isSafeResultId(id)) {
    return SubmitResult::BadId;
  }
  forgetEnded(serverTime);
  if (expiresAt <= serverTime) {
    return SubmitResult::Expired;
  }
  uint32_t lifeSeconds = expiresAt - serverTime;
  if (lifeSeconds > kMaxCommandLifeSeconds) {
    lifeSeconds = kMaxCommandLifeSeconds;
  }
  if (findPending(id) >= 0) {
    return SubmitResult::Duplicate;
  }

  // A result for this id waits for a batch already, an answer about an unknown node or the result of a run. The backend lists the command until it gets
  // that result, so the command must not run now, for example because its node paired meanwhile, and change the answer that is on its way.
  if (resultWaiting(id) && findLog(id) < 0) {
    return SubmitResult::AlreadyRan;
  }
  const int logged = findLog(id);
  if (logged >= 0 && records_[logged].state != CommandState::Sent) {
    pushResult(id, records_[logged].state == CommandState::Done, false);
    return SubmitResult::AlreadyRan;
  }

  uint8_t mac[kMacSize];
  if (nodeId == nullptr || !registry_.macOf(nodeId, mac)) {
    // The plug is not known here, so the command cannot run. The backend lists it until it gets a result or the life ends, and a failed result closes it.
    // This comes after the checks of the queue and of the log, so the result of a command that ran is never turned into a failure. When the list of
    // such results is full the command is refused, and the backend sends it again.
    if (!pushResult(id, false, true)) {
      return SubmitResult::Full;
    }
    return SubmitResult::UnknownNode;
  }

  // A slot of the log could not be read, so this id may be a command that ran before the restart. Running a switch on again could undo an overload cut
  // of the plug. A switch off does no harm when it runs twice, so it goes on. The backend lists the command again, and it is taken when the hold ends.
  if (on && logged < 0 && unreadableSlots() > 0) {
    return SubmitResult::LogUnreadable;
  }

  // A newer command for the same plug takes the place of the older one, whether it waits or is in flight, so the commands never run in the wrong
  // order. Nothing is changed before the new command is sure to fit, so a refusal leaves the older command as it was.
  int place = -1;
  for (size_t i = 0; i < kMaxQueuedCommands; i++) {
    if (pending_[i].used && sameMac(pending_[i].mac, mac)) {
      place = static_cast<int>(i);
      break;
    }
  }
  for (size_t i = 0; place < 0 && i < kMaxQueuedCommands; i++) {
    if (!pending_[i].used) {
      place = static_cast<int>(i);
    }
  }
  if (place < 0) {
    return SubmitResult::Full;
  }
  uint16_t number = 0;
  if (logged >= 0) {
    number = records_[logged].number;
  } else if (usableLogSlots() <= unpersistedExcept(mac) || !allocateNumber(&number)) {
    // No record could be kept for the command, because the slots that can be used are promised to the commands that wait already. The backend sends it
    // again when room is free.
    return SubmitResult::Full;
  }

  Pending &slot = pending_[place];
  memset(&slot, 0, sizeof(slot));
  slot.persisted = logged >= 0;
  slot.used = true;
  memcpy(slot.mac, mac, kMacSize);
  slot.action = on ? 1 : 0;
  slot.number = number;
  memcpy(slot.id, id, strnlen(id, kResultIdSize - 1));
  slot.deadlineMs = nowMs + lifeSeconds * 1000u;
  slot.expiresAt = expiresAt;
  return logged >= 0 ? SubmitResult::Resumed : SubmitResult::Queued;
}

void CommandRelay::trySend(uint32_t nowMs, Pending *pending) {
  if (!pending->haveEcho) {
    return;
  }
  // The life is checked here too, because a frame can come in before update() has ended the command, and a plug has no clock to refuse it.
  if (reached(nowMs, pending->deadlineMs) || !registry_.isPaired(pending->mac)) {
    memset(pending, 0, sizeof(*pending));
    return;
  }
  if (!pending->persisted) {
    // The record is written before the first frame, so a restart finds the command and does not run it again. A switch on after a restart could
    // undo an overload cut, so it is not sent when the record cannot be written. A switch off is sent in any case, because running it twice does no harm.
    if (persist(*pending, CommandState::Sent)) {
      pending->persisted = true;
    } else if (pending->action == 1) {
      return;
    }
  }
  pending->sent = true;
  pending->lastSendMs = nowMs;
  sender_.sendCommand(pending->mac, pending->echoBootId, pending->echoCounter, pending->number, pending->action);
}

void CommandRelay::finish(Pending *pending, bool ok) {
  persist(*pending, ok ? CommandState::Done : CommandState::Failed);
  pushResult(pending->id, ok, false);
  memset(pending, 0, sizeof(*pending));
}

bool CommandRelay::pushResult(const char *id, bool ok, bool unknownNode) {
  for (size_t i = 0; i < resultCount_; i++) {
    if (strncmp(results_[i].id, id, kResultIdSize) == 0) {
      results_[i].ok = ok;
      return true;
    }
  }
  // The list has room for a result of every record and kMaxBatchResults answers about unknown nodes, and the slot of a record whose result waits is not
  // reused. So a result of a command that ran always finds room, and an answer about an unknown node is refused when its share is used. Nothing is
  // moved or pushed out, because a batch may have taken the first entries and waits for dropResults.
  if (resultCount_ == kMaxHeldResults || (unknownNode && unknownCount_ >= kMaxBatchResults)) {
    return false;
  }
  RelayResult &entry = results_[resultCount_];
  memset(&entry, 0, sizeof(entry));
  memcpy(entry.id, id, strnlen(id, kResultIdSize - 1));
  entry.ok = ok;
  unknown_[resultCount_] = unknownNode;
  resultCount_++;
  if (unknownNode) {
    unknownCount_++;
  }
  return true;
}

void CommandRelay::dropResults(size_t count) {
  if (count > resultCount_) {
    count = resultCount_;
  }
  for (size_t i = 0; i < count; i++) {
    if (unknown_[i] && unknownCount_ > 0) {
      unknownCount_--;
    }
  }
  memmove(&results_[0], &results_[count], (resultCount_ - count) * sizeof(results_[0]));
  memmove(&unknown_[0], &unknown_[count], (resultCount_ - count) * sizeof(unknown_[0]));
  resultCount_ -= count;
}

void CommandRelay::onEvent(uint32_t nowMs, const LinkEvent &event) {
  if (event.kind == LinkEventKind::Telemetry || event.kind == LinkEventKind::PairedNode) {
    for (size_t i = 0; i < kMaxQueuedCommands; i++) {
      Pending &p = pending_[i];
      if (!p.used || !sameMac(p.mac, event.mac)) {
        continue;
      }
      // The newest frame is the echo for every send from now on, and a command that has not gone out yet goes out at once.
      p.echoBootId = event.frameBootId;
      p.echoCounter = event.frameCounter;
      p.echoMs = nowMs;
      p.haveEcho = true;
      if (!p.sent) {
        trySend(nowMs, &p);
      }
    }
    return;
  }
  if (event.kind == LinkEventKind::ResultReceived) {
    for (size_t i = 0; i < kMaxQueuedCommands; i++) {
      Pending &p = pending_[i];
      if (p.used && p.sent && p.number == event.commandNumber && sameMac(p.mac, event.mac)) {
        finish(&p, event.commandOk);
        return;
      }
    }
  }
}

void CommandRelay::update(uint32_t nowMs) {
  for (size_t i = 0; i < kMaxQueuedCommands; i++) {
    Pending &p = pending_[i];
    if (!p.used) {
      continue;
    }
    if (reached(nowMs, p.deadlineMs)) {
      memset(&p, 0, sizeof(p));
      continue;
    }
    if (p.sent && p.haveEcho && nowMs - p.echoMs < kEchoUsableMs && reached(nowMs, p.lastSendMs + kCommandResendMs)) {
      p.lastSendMs = nowMs;
      sender_.sendCommand(p.mac, p.echoBootId, p.echoCounter, p.number, p.action);
    }
  }
}

void CommandRelay::forget(const uint8_t mac[kMacSize]) {
  for (size_t i = 0; i < kMaxQueuedCommands; i++) {
    if (pending_[i].used && sameMac(pending_[i].mac, mac)) {
      memset(&pending_[i], 0, sizeof(pending_[i]));
    }
  }
}

size_t CommandRelay::unreadableSlots() const {
  size_t count = 0;
  for (size_t i = 0; i < kCommandLogSlots; i++) {
    if (unreadable_[i]) {
      count++;
    }
  }
  return count;
}

void CommandRelay::forgetEnded(uint32_t serverTime) {
  // A slot that could not be read is held from the first time of the service that the relay sees, and it is free to use again when the longest life of
  // a command and the reserve of its number are over. The first call after load() starts the count.
  if (unreadableSlots() > 0) {
    if (!unreadableTimed_) {
      unreadableTimed_ = true;
      unreadableFrom_ = serverTime;
    } else if (serverTime >= unreadableFrom_ + kUnreadableHoldSeconds) {
      memset(unreadable_, 0, sizeof(unreadable_));
    }
  }
  // The number of a command stays reserved for the time that a plug remembers it, so the record goes later than the command does. A record whose result
  // still waits for a batch stays, for example after a long outage, so the list of results never holds more than the log can account for.
  for (size_t i = 0; i < kCommandLogSlots; i++) {
    if (records_[i].state != CommandState::Free && records_[i].expiresAt + kNumberReserveSeconds < serverTime &&
        !resultWaiting(records_[i].id)) {
      memset(&records_[i], 0, sizeof(records_[i]));
    }
  }
}

size_t CommandRelay::pendingCount() const {
  size_t count = 0;
  for (size_t i = 0; i < kMaxQueuedCommands; i++) {
    if (pending_[i].used) {
      count++;
    }
  }
  return count;
}

}  // namespace wattlink
