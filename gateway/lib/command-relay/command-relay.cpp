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
    : sender_(sender), registry_(registry), log_(log), hubBootId_(hubBootId), nextSeq_(0), logFailures_(0), resultCount_(0) {
  memset(pending_, 0, sizeof(pending_));
  memset(records_, 0, sizeof(records_));
  memset(results_, 0, sizeof(results_));
}

size_t CommandRelay::load() {
  size_t held = 0;
  for (size_t i = 0; i < kCommandLogSlots; i++) {
    CommandRecord record;
    memset(&record, 0, sizeof(record));
    // A slot that cannot be read, or that holds a state that this code does not know, counts as free.
    if (log_.read(i, &record) && record.state >= CommandState::Sent && record.state <= CommandState::Failed &&
        isSafeResultId(record.id)) {
      records_[i] = record;
      held++;
    } else {
      memset(&records_[i], 0, sizeof(records_[i]));
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

int CommandRelay::pickLogSlot() const {
  int oldest = 0;
  for (size_t i = 0; i < kCommandLogSlots; i++) {
    if (records_[i].state == CommandState::Free) {
      return static_cast<int>(i);
    }
    if (records_[i].expiresAt < records_[oldest].expiresAt) {
      oldest = static_cast<int>(i);
    }
  }
  return oldest;
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
    if (!numberInUse(candidate)) {
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
  if (nodeId == nullptr) {
    return SubmitResult::UnknownNode;
  }
  // A record whose command has ended by the time of the service is forgotten, which frees its slot and its number. Nothing is written for that.
  for (size_t i = 0; i < kCommandLogSlots; i++) {
    if (records_[i].state != CommandState::Free && records_[i].expiresAt < serverTime) {
      memset(&records_[i], 0, sizeof(records_[i]));
    }
  }
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

  const int logged = findLog(id);
  if (logged >= 0 && records_[logged].state != CommandState::Sent) {
    pushResult(id, records_[logged].state == CommandState::Done);
    return SubmitResult::AlreadyRan;
  }

  uint8_t mac[kMacSize];
  if (!registry_.macOf(nodeId, mac)) {
    return SubmitResult::UnknownNode;
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
  } else if (!allocateNumber(&number)) {
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
  pushResult(pending->id, ok);
  memset(pending, 0, sizeof(*pending));
}

void CommandRelay::pushResult(const char *id, bool ok) {
  for (size_t i = 0; i < resultCount_; i++) {
    if (strncmp(results_[i].id, id, kResultIdSize) == 0) {
      results_[i].ok = ok;
      return;
    }
  }
  if (resultCount_ == kMaxBatchResults) {
    memmove(&results_[0], &results_[1], (kMaxBatchResults - 1) * sizeof(results_[0]));
    resultCount_--;
  }
  RelayResult &entry = results_[resultCount_++];
  memset(&entry, 0, sizeof(entry));
  memcpy(entry.id, id, strnlen(id, kResultIdSize - 1));
  entry.ok = ok;
}

void CommandRelay::dropResults(size_t count) {
  if (count >= resultCount_) {
    resultCount_ = 0;
    return;
  }
  memmove(&results_[0], &results_[count], (resultCount_ - count) * sizeof(results_[0]));
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
    if (p.sent && p.haveEcho && reached(nowMs, p.lastSendMs + kCommandResendMs)) {
      p.lastSendMs = nowMs;
      sender_.sendCommand(p.mac, p.echoBootId, p.echoCounter, p.number, p.action);
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
