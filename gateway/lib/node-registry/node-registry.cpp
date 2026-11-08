#include "node-registry.h"

#include <string.h>

#include "secure-wipe.h"

namespace wattlink {

namespace {

constexpr uint8_t kRecordVersion = 1;
constexpr uint8_t kEmpty = 0;
constexpr uint8_t kInUse = 1;
constexpr uint8_t kDamaged = 2;

// Layout of a record of kRecordSize bytes. Version, kind, flags (bit 0 is paired), MAC, key, and the highest boot value in little endian.
constexpr size_t kOffsetKind = 1;
constexpr size_t kOffsetFlags = 2;
constexpr size_t kOffsetMac = 3;
constexpr size_t kOffsetKey = kOffsetMac + kMacSize;
constexpr size_t kOffsetBoot = kOffsetKey + kNodeKeySize;
static_assert(kOffsetBoot + 4 == kRecordSize, "the record layout fills kRecordSize");

bool validKind(uint8_t kind) {
  return kind == kKindPlug || kind == kKindClamp;
}

bool validMac(const uint8_t mac[kMacSize]) {
  bool allZero = true;
  for (size_t i = 0; i < kMacSize; i++) {
    if (mac[i] != 0) {
      allZero = false;
    }
  }
  // Bit 0 of the first byte marks a group address, which no radio of a node has.
  return !allZero && (mac[0] & 0x01) == 0;
}

bool sameMac(const uint8_t *a, const uint8_t *b) {
  return memcmp(a, b, kMacSize) == 0;
}

}  // namespace

NodeRegistry::~NodeRegistry() {
  wipe(slots_, sizeof(slots_));
}

int NodeRegistry::findSlot(const uint8_t mac[kMacSize], uint8_t kind) const {
  for (size_t i = 0; i < kMaxNodes; i++) {
    if (slots_[i].state == kInUse && slots_[i].kind == kind && sameMac(slots_[i].mac, mac)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// At most one slot of a MAC is paired, because pair() refuses a MAC that is paired as any kind.
int NodeRegistry::findPaired(const uint8_t mac[kMacSize]) const {
  for (size_t i = 0; i < kMaxNodes; i++) {
    if (slots_[i].state == kInUse && slots_[i].paired && sameMac(slots_[i].mac, mac)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool NodeRegistry::persist(size_t index) {
  const Slot &slot = slots_[index];
  uint8_t record[kRecordSize] = {};
  record[0] = kRecordVersion;
  record[kOffsetKind] = slot.kind;
  record[kOffsetFlags] = slot.paired ? 1 : 0;
  memcpy(record + kOffsetMac, slot.mac, kMacSize);
  memcpy(record + kOffsetKey, slot.key, kNodeKeySize);
  for (size_t i = 0; i < 4; i++) {
    record[kOffsetBoot + i] = static_cast<uint8_t>(slot.bootId >> (8 * i));
  }
  const bool ok = store_.write(index, record);
  wipe(record, sizeof(record));
  return ok;
}

size_t NodeRegistry::load() {
  wipe(slots_, sizeof(slots_));
  size_t valid = 0;
  for (size_t i = 0; i < kMaxNodes; i++) {
    uint8_t record[kRecordSize] = {};
    ReadResult read = store_.read(i, record);
    if (read == ReadResult::Failed) {
      // A read can fail for a moment. One more try keeps a good record from being treated as damaged for this start.
      read = store_.read(i, record);
    }
    if (read == ReadResult::Missing) {
      continue;
    }
    Slot &slot = slots_[i];
    const bool understood = read == ReadResult::Ok && record[0] == kRecordVersion && validKind(record[kOffsetKind]) &&
                            record[kOffsetFlags] <= 1 && validMac(record + kOffsetMac);
    // Two records with the same id, or two pairings of one MAC, cannot come from this class. The later one is left out as damaged.
    bool duplicate = false;
    if (understood) {
      for (size_t j = 0; j < i && !duplicate; j++) {
        if (slots_[j].state != kInUse) {
          continue;
        }
        const bool sameId = slots_[j].kind == record[kOffsetKind] && memcmp(slots_[j].mac + 3, record + kOffsetMac + 3, 3) == 0;
        const bool bothPaired = record[kOffsetFlags] == 1 && slots_[j].paired && sameMac(slots_[j].mac, record + kOffsetMac);
        duplicate = sameId || bothPaired;
      }
    }
    if (!understood || duplicate) {
      slot.state = kDamaged;
      wipe(record, sizeof(record));
      continue;
    }
    slot.state = kInUse;
    slot.kind = record[kOffsetKind];
    slot.paired = record[kOffsetFlags] == 1;
    memcpy(slot.mac, record + kOffsetMac, kMacSize);
    if (slot.paired) {
      memcpy(slot.key, record + kOffsetKey, kNodeKeySize);
      slot.bootId = static_cast<uint32_t>(record[kOffsetBoot]) | static_cast<uint32_t>(record[kOffsetBoot + 1]) << 8 |
                    static_cast<uint32_t>(record[kOffsetBoot + 2]) << 16 | static_cast<uint32_t>(record[kOffsetBoot + 3]) << 24;
    }
    wipe(record, sizeof(record));
    valid++;
  }
  return valid;
}

PairResult NodeRegistry::plan(uint8_t kind, const uint8_t mac[kMacSize], int *reuse, int *fresh) const {
  *reuse = -1;
  *fresh = -1;
  if (!validKind(kind)) {
    return PairResult::InvalidKind;
  }
  if (!validMac(mac)) {
    return PairResult::InvalidMac;
  }
  if (findPaired(mac) >= 0) {
    return PairResult::AlreadyPaired;
  }

  const int known = findSlot(mac, kind);
  if (known >= 0) {
    *reuse = known;
    return PairResult::Ok;
  }

  // The id is the kind and the last 3 bytes of the MAC, so a second MAC with the same ending and kind would be the same id for the backend.
  for (size_t i = 0; i < kMaxNodes; i++) {
    if (slots_[i].state == kInUse && slots_[i].kind == kind && memcmp(slots_[i].mac + 3, mac + 3, 3) == 0) {
      return PairResult::IdTaken;
    }
  }

  for (size_t i = 0; i < kMaxNodes; i++) {
    if (slots_[i].state == kEmpty) {
      *fresh = static_cast<int>(i);
      return PairResult::Ok;
    }
  }
  return PairResult::RegistryFull;
}

PairResult NodeRegistry::canPair(uint8_t kind, const uint8_t mac[kMacSize]) const {
  int reuse;
  int fresh;
  return plan(kind, mac, &reuse, &fresh);
}

PairResult NodeRegistry::pair(uint8_t kind, const uint8_t mac[kMacSize], const uint8_t key[kNodeKeySize]) {
  int reuse;
  int fresh;
  const PairResult planned = plan(kind, mac, &reuse, &fresh);
  if (planned != PairResult::Ok) {
    return planned;
  }

  if (reuse >= 0) {
    Slot &slot = slots_[reuse];
    memcpy(slot.key, key, kNodeKeySize);
    slot.paired = true;
    slot.bootId = 0;
    slot.haveCounter = false;
    slot.counter = 0;
    if (!persist(reuse)) {
      wipe(slot.key, sizeof(slot.key));
      slot.paired = false;
      return PairResult::StoreFailed;
    }
    return PairResult::Ok;
  }

  Slot &slot = slots_[fresh];
  slot.state = kInUse;
  slot.kind = kind;
  slot.paired = true;
  memcpy(slot.mac, mac, kMacSize);
  memcpy(slot.key, key, kNodeKeySize);
  slot.bootId = 0;
  slot.haveCounter = false;
  slot.counter = 0;
  if (!persist(fresh)) {
    wipe(&slot, sizeof(slot));
    return PairResult::StoreFailed;
  }
  return PairResult::Ok;
}

bool NodeRegistry::unpair(const uint8_t mac[kMacSize]) {
  const int index = findPaired(mac);
  if (index < 0) {
    return false;
  }
  Slot &slot = slots_[index];
  Slot before = slot;
  wipe(slot.key, sizeof(slot.key));
  slot.paired = false;
  slot.bootId = 0;
  slot.haveCounter = false;
  slot.counter = 0;
  if (!persist(index)) {
    // The flash still holds the pairing, so the RAM keeps it too and the two never disagree.
    slot = before;
    wipe(&before, sizeof(before));
    return false;
  }
  wipe(&before, sizeof(before));
  return true;
}

bool NodeRegistry::isPaired(const uint8_t mac[kMacSize]) const {
  return findPaired(mac) >= 0;
}

uint8_t NodeRegistry::kindOf(const uint8_t mac[kMacSize]) const {
  const int index = findPaired(mac);
  return index >= 0 ? slots_[index].kind : 0;
}

const uint8_t *NodeRegistry::keyOf(const uint8_t mac[kMacSize]) const {
  const int index = findPaired(mac);
  return index >= 0 ? slots_[index].key : nullptr;
}

bool makeNodeId(uint8_t kind, const uint8_t mac[kMacSize], char out[kNodeIdSize]) {
  if (!validKind(kind)) {
    out[0] = '\0';
    return false;
  }
  static const char kHex[] = "0123456789ABCDEF";
  const char *prefix = kind == kKindPlug ? "PLUG-" : "CLAMP-";
  size_t length = 0;
  while (*prefix != '\0') {
    out[length++] = *prefix++;
  }
  for (size_t i = 3; i < kMacSize; i++) {
    out[length++] = kHex[mac[i] >> 4];
    out[length++] = kHex[mac[i] & 0x0F];
  }
  out[length] = '\0';
  return true;
}

bool NodeRegistry::nodeId(const uint8_t mac[kMacSize], char out[kNodeIdSize]) const {
  const int index = findPaired(mac);
  if (index < 0) {
    out[0] = '\0';
    return false;
  }
  return makeNodeId(slots_[index].kind, mac, out);
}

FrameCheck NodeRegistry::check(const uint8_t mac[kMacSize], uint32_t bootId, uint32_t counter) const {
  const int index = findPaired(mac);
  if (index < 0) {
    return FrameCheck::Unknown;
  }
  const Slot &slot = slots_[index];
  // A node starts at 1 and grows by 1 at every start, so 0 is not a real start.
  if (bootId == 0 || bootId < slot.bootId) {
    return FrameCheck::Stale;
  }
  if (bootId == slot.bootId && slot.haveCounter && counter <= slot.counter) {
    return FrameCheck::Stale;
  }
  return FrameCheck::Accepted;
}

FrameCheck NodeRegistry::accept(const uint8_t mac[kMacSize], uint32_t bootId, uint32_t counter) {
  const FrameCheck fresh = check(mac, bootId, counter);
  if (fresh != FrameCheck::Accepted) {
    return fresh;
  }
  const int index = findPaired(mac);
  Slot &slot = slots_[index];
  if (bootId > slot.bootId) {
    const uint32_t oldBoot = slot.bootId;
    slot.bootId = bootId;
    if (!persist(index)) {
      slot.bootId = oldBoot;
      return FrameCheck::StoreFailed;
    }
  }
  slot.haveCounter = true;
  slot.counter = counter;
  return FrameCheck::Accepted;
}

size_t NodeRegistry::pairedCount() const {
  size_t count = 0;
  for (size_t i = 0; i < kMaxNodes; i++) {
    if (slots_[i].state == kInUse && slots_[i].paired) {
      count++;
    }
  }
  return count;
}

size_t NodeRegistry::idCount() const {
  size_t count = 0;
  for (size_t i = 0; i < kMaxNodes; i++) {
    if (slots_[i].state == kInUse) {
      count++;
    }
  }
  return count;
}

size_t NodeRegistry::damagedCount() const {
  size_t count = 0;
  for (size_t i = 0; i < kMaxNodes; i++) {
    if (slots_[i].state == kDamaged) {
      count++;
    }
  }
  return count;
}

}  // namespace wattlink
