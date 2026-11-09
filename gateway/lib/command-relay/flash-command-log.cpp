#include "flash-command-log.h"

#include <string.h>

namespace wattlink {

namespace {

constexpr size_t kOffsetVersion = 0;
constexpr size_t kOffsetState = 1;
constexpr size_t kOffsetNumber = 2;
constexpr size_t kOffsetExpires = 4;
constexpr size_t kOffsetId = 8;
constexpr size_t kIdBytes = kResultIdSize - 1;
constexpr size_t kOffsetCrc = kOffsetId + kIdBytes;
static_assert(kOffsetCrc + 4 == kCommandBlobSize, "the layout fills kCommandBlobSize");

void putU32(uint8_t *out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t getU32(const uint8_t *in) {
  return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) | (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

}  // namespace

uint32_t crc32Ieee(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc & 1u) != 0 ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
  }
  return ~crc;
}

bool encodeCommandRecord(const CommandRecord &record, uint8_t out[kCommandBlobSize]) {
  // The id has room for 36 characters and the end mark. A longer text is refused and not cut, because a cut id would match another command.
  if (memchr(record.id, 0, kResultIdSize) == nullptr) {
    return false;
  }
  memset(out, 0, kCommandBlobSize);
  out[kOffsetVersion] = kCommandBlobVersion;
  out[kOffsetState] = static_cast<uint8_t>(record.state);
  out[kOffsetNumber] = static_cast<uint8_t>(record.number);
  out[kOffsetNumber + 1] = static_cast<uint8_t>(record.number >> 8);
  putU32(out + kOffsetExpires, record.expiresAt);
  const size_t length = strnlen(record.id, kResultIdSize);
  memcpy(out + kOffsetId, record.id, length);
  putU32(out + kOffsetCrc, crc32Ieee(out, kOffsetCrc));
  return true;
}

bool decodeCommandRecord(const uint8_t in[kCommandBlobSize], CommandRecord *out) {
  if (in[kOffsetVersion] != kCommandBlobVersion || getU32(in + kOffsetCrc) != crc32Ieee(in, kOffsetCrc)) {
    return false;
  }
  if (in[kOffsetState] > static_cast<uint8_t>(CommandState::Failed)) {
    return false;
  }
  CommandRecord record;
  memset(&record, 0, sizeof(record));
  record.state = static_cast<CommandState>(in[kOffsetState]);
  record.number = static_cast<uint16_t>(in[kOffsetNumber] | (in[kOffsetNumber + 1] << 8));
  record.expiresAt = getU32(in + kOffsetExpires);
  memcpy(record.id, in + kOffsetId, kIdBytes);
  record.id[kIdBytes] = '\0';
  *out = record;
  return true;
}

bool FlashCommandLog::read(size_t slot, CommandRecord *out) {
  if (slot >= kCommandLogSlots) {
    return false;
  }
  uint8_t blob[kCommandBlobSize];
  ReadResult found = store_.read(slot, blob);
  if (found == ReadResult::Failed) {
    // One more try, because a read can fail for a moment while the flash is busy with another task.
    found = store_.read(slot, blob);
  }
  if (found == ReadResult::Failed) {
    failedReads_++;
    return false;
  }
  if (found == ReadResult::Missing) {
    return false;
  }
  if (!decodeCommandRecord(blob, out)) {
    damaged_++;
    return false;
  }
  return true;
}

bool FlashCommandLog::write(size_t slot, const CommandRecord &record) {
  if (slot >= kCommandLogSlots) {
    return false;
  }
  uint8_t blob[kCommandBlobSize];
  return encodeCommandRecord(record, blob) && store_.write(slot, blob);
}

}  // namespace wattlink
