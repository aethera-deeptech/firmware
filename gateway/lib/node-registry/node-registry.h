#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pairing-key.h"
#include "record-store.h"

namespace wattlink {

// The rules are in the sections "Identity", "Pairing and keys" and "Order and repeats" of docs/espnow-protocol.md.
constexpr size_t kMaxNodes = 64;
constexpr size_t kNodeKeySize = 16;

// kMacSize and the size of a pairing key come from common/src/pairing-key.h, and the key that the registry keeps is the key that the pairing makes.
static_assert(kNodeKeySize == kPairingKeySize, "the registry keeps the whole pairing key");

// The kinds that the backend accepts. They are the values of NodeKind in common/src/payloads.h, and the firmware of the hub checks that at build time.
constexpr uint8_t kKindPlug = 1;
constexpr uint8_t kKindClamp = 2;

// The longest node id is CLAMP-A1B2C3, which is 12 characters, and a terminating zero.
constexpr size_t kNodeIdSize = 13;

enum class PairResult : uint8_t {
  Ok,
  InvalidKind,
  InvalidMac,     // all zero, or a group address
  AlreadyPaired,  // the owner removes the node first
  IdTaken,        // the id is kept for another MAC
  RegistryFull,   // 64 ids were reported, and that limit is for the life of the gateway
  StoreFailed,
};

enum class FrameCheck : uint8_t {
  Accepted,
  Unknown,      // no paired node has this MAC
  Stale,        // a boot value of 0 or older than the kept one, or a counter that is not higher
  StoreFailed,  // a new boot could not be written to flash, so the frame is dropped
};

// Writes the id for the backend, for example PLUG-A1B2C3, and a terminating zero. Returns false for a kind that is not a plug or a clamp, and
// then out holds an empty text. The id does not depend on the registry, so the hub can show it while a node asks to pair. It does not check the
// MAC, so the hub calls canPair() first.
bool makeNodeId(uint8_t kind, const uint8_t mac[kMacSize], char out[kNodeIdSize]);

// The ids that the hub reported to the backend, and the pairings. Removing a node forgets its pairing and keeps its id, because the backend counts
// every different id of a gateway for ever. The class does the logic only, so the same code runs on the host in the tests and on the chip.
// One task must call all of it. Keys are kept in RAM while the hub runs and are wiped when a pairing ends.
class NodeRegistry {
 public:
  explicit NodeRegistry(RecordStore &store) : store_(store) {}
  ~NodeRegistry();

  // Reads every slot from the store. A slot that cannot be read or that holds a damaged record is left out and never reused, because it may
  // hold an id that the backend counted. Returns the number of slots that hold a valid record.
  size_t load();

  // Pairs a node with a new key. A node that was removed before and pairs again as the same kind has the same id and uses its old slot, and its kept boot
  // value is forgotten. The same MAC as the other kind is a new id, so it takes a new slot.
  PairResult pair(uint8_t kind, const uint8_t mac[kMacSize], const uint8_t key[kNodeKeySize]);

  // Says what pair() would answer, and changes nothing, so the hub can refuse a node that it knows before it sends PAIR_ACCEPT. It never
  // answers StoreFailed, because only a real write can fail.
  PairResult canPair(uint8_t kind, const uint8_t mac[kMacSize]) const;

  // Forgets the pairing of a node and wipes its key. The id keeps its slot. Returns false when no such node is paired or the flash refused the change.
  bool unpair(const uint8_t mac[kMacSize]);

  bool isPaired(const uint8_t mac[kMacSize]) const;
  uint8_t kindOf(const uint8_t mac[kMacSize]) const;  // 0 when the MAC is not paired

  // The key of a paired node, or nullptr. The pointer is valid until the next call that changes the registry.
  const uint8_t *keyOf(const uint8_t mac[kMacSize]) const;

  // Writes the id for the backend, for example PLUG-A1B2C3, and a terminating zero. Returns false when the MAC is not paired.
  bool nodeId(const uint8_t mac[kMacSize], char out[kNodeIdSize]) const;

  // Tells whether a frame is fresh. It changes nothing, so the caller can run it before the tag is checked.
  FrameCheck check(const uint8_t mac[kMacSize], uint32_t bootId, uint32_t counter) const;

  // Call it only after the tag of the frame has verified. It checks again, keeps the counter in RAM, and writes a higher boot value to flash
  // before it accepts the frame. A write that fails drops the frame, so a node that boots again is never accepted without a stored value.
  // The flash write can take tens of milliseconds, so never call it from the ESP-NOW receive callback. Pass the frame to the task that owns
  // the registry through a queue.
  FrameCheck accept(const uint8_t mac[kMacSize], uint32_t bootId, uint32_t counter);

  size_t pairedCount() const;
  size_t idCount() const;       // the ids that were reported, which count against the limit of 64
  size_t damagedCount() const;  // slots that could not be read or understood

 private:
  struct Slot {
    uint8_t state;  // 0 empty, 1 in use, 2 damaged
    uint8_t kind;
    bool paired;
    uint8_t mac[kMacSize];
    uint8_t key[kNodeKeySize];
    uint32_t bootId;
    bool haveCounter;
    uint32_t counter;
  };

  int findSlot(const uint8_t mac[kMacSize], uint8_t kind) const;
  // The checks that pair() and canPair() share. On Ok it names the slot to reuse, or the empty slot to take.
  PairResult plan(uint8_t kind, const uint8_t mac[kMacSize], int *reuse, int *fresh) const;
  int findPaired(const uint8_t mac[kMacSize]) const;
  bool persist(size_t index);

  RecordStore &store_;
  Slot slots_[kMaxNodes] = {};
};

}  // namespace wattlink
