#pragma once

#include <stddef.h>
#include <stdint.h>

#include "command-relay.h"
#include "record-store.h"

namespace wattlink {

// The record of a command as it is kept in flash. Version 1 has 48 bytes, all numbers little endian.
//   byte 0       the version of the layout
//   byte 1       the state of the command
//   bytes 2-3    the number of the command on the link
//   bytes 4-7    the end of the life of the command, in the time of the service
//   bytes 8-43   the id of the command, 36 bytes, padded with zero
//   bytes 44-47  CRC-32 of bytes 0 to 43
constexpr size_t kCommandBlobSize = 48;
constexpr uint8_t kCommandBlobVersion = 1;

// CRC-32 as zlib and Ethernet use it, so the known answer for the text "123456789" is 0xCBF43926.
uint32_t crc32Ieee(const uint8_t *data, size_t length);

// Writes the record into the 48 bytes. Returns false when the id is longer than 36 characters or the state is not one of the four, and writes nothing then.
bool encodeCommandRecord(const CommandRecord &record, uint8_t out[kCommandBlobSize]);

// Reads a record. Returns false for a layout of another version, for a state that this code does not know, or for bytes whose CRC does not match, and
// *out is not changed then.
bool decodeCommandRecord(const uint8_t in[kCommandBlobSize], CommandRecord *out);

// The flash behind FlashCommandLog. One blob of kCommandBlobSize bytes for each slot. On the chip it is NVS, and in the tests it is a fake in memory.
// ReadResult::Failed means that the flash could not be read, and not that the slot is empty.
class CommandBlobStore {
 public:
  virtual ~CommandBlobStore() = default;
  virtual ReadResult read(size_t slot, uint8_t out[kCommandBlobSize]) = 0;
  // Must replace the old blob in one step. A write that fails or is cut by a loss of power leaves the old blob or the new blob, never a mix of both.
  virtual bool write(size_t slot, const uint8_t in[kCommandBlobSize]) = 0;
};

// The log of the relay on top of a blob store. A slot that was never written is Free. A slot that the flash cannot read, also after one more try, or
// whose bytes do not pass the check is Unreadable, as CommandLog says. The two counters tell the caller how many slots were lost, so the hub can print it.
class FlashCommandLog : public CommandLog {
 public:
  explicit FlashCommandLog(CommandBlobStore &store) : store_(store), damaged_(0), failedReads_(0) {}

  LogRead read(size_t slot, CommandRecord *out) override;
  bool write(size_t slot, const CommandRecord &record) override;

  // Slots that held bytes which did not pass the check.
  uint32_t damaged() const { return damaged_; }
  // Slots that the flash could not read, also after one more try.
  uint32_t failedReads() const { return failedReads_; }

 private:
  CommandBlobStore &store_;
  uint32_t damaged_;
  uint32_t failedReads_;
};

}  // namespace wattlink
