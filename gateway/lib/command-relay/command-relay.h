#pragma once

#include <stddef.h>
#include <stdint.h>

#include "batch-builder.h"
#include "hub-link.h"
#include "node-registry.h"

namespace wattlink {

// The relay commands of the backend, as the section "Commands" of docs/espnow-protocol.md and the section "Relay commands" of the firmware guide of
// the backend describe them. The class does the logic only. The radio is behind CommandSender and the flash is behind CommandLog, so the same code runs
// on the host in the tests and on the chip. One task must call all of it, the task that calls HubLink.

// How many commands wait or are in flight at the same time. A command lives 10 minutes at most.
constexpr size_t kMaxQueuedCommands = 16;
// How many records the log keeps, enough for a command of every node and every queue place. A record is kept for the life of its command and then for
// kNumberReserveSeconds more, because a plug remembers the number of a command for 10 minutes after it ran it. A slot is reused only when its
// record is free, or when the command is done and its result is no longer waiting for a batch. A new command is refused when no slot can be used.
constexpr size_t kCommandLogSlots = kMaxNodes + kMaxQueuedCommands;
constexpr uint32_t kNumberReserveSeconds = 600;
// The hub sends a command again after this time, with the echo of the newest frame of the plug, until the plug answers or the command ends.
constexpr uint32_t kCommandResendMs = 5000;
// The hub sends a command again only while the newest frame of the plug is not older than this, because a plug that is not heard cannot be reached
// with an echo that it may have passed already. The next frame of the plug starts the resends again. A plug keeps the last 6 frames at 10 seconds.
constexpr uint32_t kEchoUsableMs = 50000;
// The longest life of a command, in seconds. A command that the backend lets live longer is cut to this.
constexpr uint32_t kMaxCommandLifeSeconds = 600;

// The state of a command in the log. Free is 0, so a slot that was never written reads as free.
enum class CommandState : uint8_t {
  Free = 0,
  Sent = 1,    // the first COMMAND frame was handed to the radio, and the answer of the plug is not known
  Done = 2,    // the plug answered that the relay switched
  Failed = 3,  // the plug answered that the relay did not switch
};

// What is kept in flash for a command, so that a hub that restarts does not run the same id twice. A command that comes again is found by its id.
struct CommandRecord {
  char id[kResultIdSize];
  uint16_t number;      // the number of the command on the link, so a resend after a restart is the same command for the plug
  CommandState state;
  uint32_t expiresAt;   // seconds in the time of the service, so the record is dropped by the time that the backend sends, and not by a clock of the hub
};

// The storage of the records. On the chip it is the flash, and in the tests it is a fake in memory.
class CommandLog {
 public:
  virtual ~CommandLog() = default;
  // Reads one slot. Returns false when the slot cannot be read or holds a damaged record, and the slot then counts as free.
  virtual bool read(size_t slot, CommandRecord *out) = 0;
  // Writes one slot. A write that fails or is cut by a loss of power must leave the old record or the new record readable, never a damaged one,
  // because a record that cannot be read counts as free, and the hub could then run an id again. The flash implementation has to keep this.
  virtual bool write(size_t slot, const CommandRecord &record) = 0;
};

enum class SubmitResult : uint8_t {
  Queued,       // a new command waits for the next frame of its plug
  Resumed,      // the command was sent before a restart and its answer is not known, so it is sent again with the same number
  Duplicate,    // the command is queued already, and nothing changed
  AlreadyRan,   // the plug answered it before, and the result is queued again for the next batch
  UnknownNode,  // no paired node has this id
  BadId,        // the id of the command is empty, longer than 36 characters, or holds a character that is not a letter, a digit or a hyphen
  Expired,      // the command has no life left, so it is not run
  Full,         // the queue, the numbers or the log are used up, so the command is not taken and the backend sends it again
};

class CommandRelay {
 public:
  // The boot number of the hub makes the high byte of every command number, so numbers of two starts never meet while a plug still remembers them.
  CommandRelay(CommandSender &sender, const NodeRegistry &registry, CommandLog &log, uint32_t hubBootId);

  CommandRelay(const CommandRelay &) = delete;
  CommandRelay &operator=(const CommandRelay &) = delete;

  // Reads the log. Call it once at the start, before the first submit. Returns the number of slots that hold a record.
  size_t load();

  // A command from the answer of the backend. serverTime is the time of the service in that answer, and expiresAt is the time of the command, both in
  // seconds, because the clock of the hub may be unset. nowMs is the time at which the answer arrived, and not the time at which the batch left, because
  // the life of the command is counted from it. A newer command for the same plug replaces the one that waits or is in flight, because the
  // backend lists only the newer one from then on. A command that has ended or was replaced gets no result, and the backend lets it expire.
  SubmitResult submit(uint32_t nowMs, uint32_t serverTime, const char *id, const char *nodeId, bool on, uint32_t expiresAt);

  // Feeds an event of HubLink. A TELEMETRY frame of a plug is the moment to send its command, with the echo of that frame, and a RESULT closes the command.
  void onEvent(uint32_t nowMs, const LinkEvent &event);

  // Call it in every turn of the loop. It ends the commands whose life is over and sends the ones that are due again.
  void update(uint32_t nowMs);

  // The results for the next batch. The caller passes them to the batch builder and keeps them until the backend acknowledges that batch, then calls
  // dropResults with the number that it passed, so a result that came in meanwhile stays for the next batch. A batch takes kMaxBatchResults at most,
  // so resultCount() is never more than that and the rest waits. The relay holds a result for every record of the log at most, and a record whose result
  // waits is never reused, so a new result always finds room and the entries that were passed never move.
  size_t resultCount() const { return resultCount_ < kMaxBatchResults ? resultCount_ : kMaxBatchResults; }
  const RelayResult *results() const { return results_; }
  void dropResults(size_t count);

  // Forgets the commands of a node that was removed, with no result, so a command for the old pairing never reaches a new one. Call it when a node is
  // unpaired.
  void forget(const uint8_t mac[kMacSize]);

  // Forgets the records whose command ended more than kNumberReserveSeconds before serverTime. submit() does it too, and the caller may call it for an
  // answer that holds no command, so the log does not fill up with old records. Nothing is written to flash.
  void forgetEnded(uint32_t serverTime);

  size_t pendingCount() const;
  // The number of writes that the flash refused, for the log of the hub.
  uint32_t logFailures() const { return logFailures_; }

 private:
  struct Pending {
    bool used;
    bool sent;         // a COMMAND frame was handed to the radio at least once
    bool persisted;    // the log holds the record of this command in the state Sent or later
    bool haveEcho;
    uint8_t mac[kMacSize];
    uint8_t action;
    uint16_t number;
    char id[kResultIdSize];
    uint32_t deadlineMs;
    uint32_t lastSendMs;
    uint32_t echoMs;     // the time of the frame that the echo comes from
    uint32_t echoBootId;
    uint32_t echoCounter;
    uint32_t expiresAt;
  };

  int findPending(const char *id) const;
  int findLog(const char *id) const;
  int pickLogSlot() const;  // -1 when no slot can be used
  bool resultWaiting(const char *id) const;
  bool numberInUse(uint16_t number) const;
  bool allocateNumber(uint16_t *number);
  bool persist(const Pending &pending, CommandState state);
  void trySend(uint32_t nowMs, Pending *pending);
  void finish(Pending *pending, bool ok);
  void pushResult(const char *id, bool ok);

  CommandSender &sender_;
  const NodeRegistry &registry_;
  CommandLog &log_;
  uint32_t hubBootId_;
  uint8_t nextSeq_;
  uint32_t logFailures_;
  Pending pending_[kMaxQueuedCommands];
  CommandRecord records_[kCommandLogSlots];
  RelayResult results_[kCommandLogSlots];
  size_t resultCount_;
};

}  // namespace wattlink
