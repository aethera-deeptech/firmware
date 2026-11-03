# ESP-NOW protocol between nodes and the hub

This page describes version 1 of the frames that a node (Plug or Clamp) and the hub exchange over ESP-NOW. It is the design that the first firmware follows. The contract between the hub and the backend is in the [firmware guide](https://github.com/aethera-deeptech/backend/blob/main/docs/firmware-guide.md) and is not repeated here.

## Topology

- One hub and many nodes form a star. Every node talks to the hub only. A node never relays a frame, so the `hops` value that the hub sends to the backend is always 0.
- The hub joins the Wi-Fi network of the building to reach the backend, so ESP-NOW runs on the channel of that network. A node does not know the channel when it starts, so it finds the hub first.
- A frame is at most 250 bytes, which is the limit of ESP-NOW version 1.0. The frames on this page stay far below it.
- The encryption is done by the firmware in each frame and not by the link layer of ESP-NOW. The link layer allows 17 encrypted peers at most, and the backend allows 64 nodes for a gateway. The hub adds a node as a plain peer only while it sends to it, so the table of 20 peers never limits the number of nodes.

## Identity

- A node is identified by the MAC address of its radio. The hub builds the `nodeId` for the backend from the kind and the last 3 bytes of the MAC, for example `PLUG-A1B2C3` or `CLAMP-A1B2C3`. The id stays the same after a restart, because the backend counts every different id of a gateway and keeps at most 64 for ever.
- The hub refuses to pair a node whose id is already in use, and it refuses every pairing when it knows 64 nodes.
- The kind is `plug` or `clamp`, which are the only kinds that the backend accepts. A Clamp meter is a Clamp for the protocol.

## Frame layout

Every frame starts with a header of 10 bytes. Numbers are little endian.

| Offset | Size | Field | Meaning |
| :--- | :--- | :--- | :--- |
| 0 | 1 | `version` | Protocol version, 1 for this page |
| 1 | 1 | `type` | Frame type from the table below |
| 2 | 4 | `bootId` | Random number that the sender picks at every start |
| 6 | 4 | `counter` | Number of the frame of the sender, counted from 0 at every start |

The frames of the encrypted types have the payload followed by a tag of 16 bytes. The payload and the tag are made with AES-128-GCM. The header is the associated data, and the nonce is the `bootId`, the `counter`, a byte for the direction (0 node to hub, 1 hub to node) and 3 bytes of zero. A receiver drops a frame with an unknown version, an unknown type, a length that does not fit the type or a tag that does not verify.

| Type | Name | Direction | Encrypted | Payload |
| :--- | :--- | :--- | :--- | :--- |
| 0x01 | HELLO | node to broadcast | no | kind (1 byte), firmware version (3 bytes) |
| 0x02 | HELLO_ACK | hub to node | no | none, the channel is the one that carried the frame |
| 0x10 | PAIR_REQUEST | node to hub | no | kind (1 byte), X25519 public key (32 bytes) |
| 0x11 | PAIR_ACCEPT | hub to node | no | X25519 public key (32 bytes) |
| 0x20 | TELEMETRY | node to hub | yes | sample count (1 byte), then the samples |
| 0x21 | ACK | hub to node | yes | `counter` of the last TELEMETRY frame that the hub accepted (4 bytes) |
| 0x30 | COMMAND | hub to node | yes | echo (4 bytes), command number (2 bytes), action (1 byte), life in seconds (2 bytes) |
| 0x31 | RESULT | node to hub | yes | command number (2 bytes), ok (1 byte) |

### Sample

A TELEMETRY frame holds 1 to 8 samples of 23 bytes each, which makes a frame of at most 211 bytes.

| Size | Field | Unit | Maps to |
| :--- | :--- | :--- | :--- |
| 2 | `sampleSeq` | number of the sample of the node, counted from 0 | none, the hub uses it to find a repeat |
| 4 | `ageMs` | milliseconds since the node took the sample, set again at every send | `t` |
| 2 | voltage | 0.1 V | `v` |
| 2 | current | 0.01 A | `i` |
| 4 | power | 0.1 W | `p` |
| 4 | energy | Wh | `e` |
| 2 | frequency | 0.1 Hz | `f` |
| 1 | power factor | 0.01 | `pf` |
| 1 | condition | 0 normal, 1 standby, 2 motor degradation, 3 overload | `a` |
| 1 | relay | 1 on, 0 off, 255 none | `r` |

- A node has no clock that matters, so it sends the age of the sample. The hub subtracts the age from its own time when the frame arrives and so makes `t`. This keeps one clock in the system, the one of the hub. The hub holds samples until its clock is set, because the backend refuses a time before 1700000000.
- A value that the node cannot measure is sent as the maximum of its field (all bits set), and the hub leaves it out of the batch. A Clamp sends 255 for the relay, because the backend takes relay commands and the relay state from a plug only.
- The hub drops a sample whose `sampleSeq` it already has, so a frame that is sent again after a lost acknowledgement stores nothing twice. It also drops a value that is out of the range of the contract, so the backend does not refuse the sample.
- The energy is the counter of the PZEM-004T, which the PZEM keeps in its own memory. The hub sends it in kWh. A counter that falls after a replaced meter is harmless, because the backend then takes the energy of that gap from the power.
- A Plug sends the power factor only when it is set to motor mode, and a Clamp always sends it. The backend judges every node that sends `pf`, and a charger or a lamp has a low power factor that would raise a false motor alert.

## Finding the hub

1. A node that has no stored channel sends HELLO on channel 1 and waits 200 ms for HELLO_ACK. It tries the next channel up to channel 13.
2. The hub answers every HELLO with HELLO_ACK on the channel of its own radio through a temporary peer, which it removes at once. It answers at most 5 HELLO frames in a second.
3. The node stores the channel. After 10 sends in a row that fail, or after 2 minutes without a valid ACK, it forgets the channel and starts again at step 1. It waits a random time of up to 10 seconds before it starts, so several nodes do not call at the same moment after a channel change.
4. A node that has had no valid ACK for 10 minutes shows it with its LED, because the hub may have forgotten its key.

## Pairing and keys

- The hub has a pairing window that opens when the owner presses a button on the hub and closes after one pairing or after 60 seconds. The hub shows the id of the node on its display. A node asks for pairing when the owner holds its button, and stops after 60 seconds.
- The node sends PAIR_REQUEST with a new X25519 public key. The hub, inside its window, answers PAIR_ACCEPT with its own new key. Both sides refuse a shared secret that is all zero.
- Each side takes 16 bytes from HKDF-SHA256 over the shared secret. The salt is the SHA-256 of the text `wattlink-espnow-v1`. The context is the public key of the node, the public key of the hub, the MAC of the node and the MAC of the hub, in this order. The result is the key of AES-128-GCM for this node.
- The key lives in the flash of both sides. The hub forgets a node when the owner removes it, and a node forgets the hub when the owner holds its button for 10 seconds. A device keeps the key in plain flash, so someone with physical access can read it unless flash encryption is turned on.
- Limit of the design. Someone who is close to the devices during the pairing window and who can transmit can pair as a node, or answer as the hub. The window is short and the owner opens it on purpose and sees the node id, but the exchange has no check that the owner could make of the other device. After pairing, the traffic is encrypted and authenticated and the hub only accepts the paired nodes.

## Order and repeats

- A receiver keeps the `bootId` and the highest `counter` of each peer and drops a frame with the same `bootId` and a counter that is not higher. A new `bootId` starts a new session, and a node asks the hub to confirm it with the next ACK before it trusts the hub again.
- A COMMAND is accepted only when its echo is the `counter` of one of the last 6 TELEMETRY frames that the node sent. A captured command is therefore useless after about a minute, and a command that arrives twice does no harm because the action is never a toggle.

## Commands

- The hub gets a command from the backend with a string id and a life of up to 10 minutes. It gives the command a number of 2 bytes that is only used on the link, and keeps the table between the number and the id.
- The hub waits for the next TELEMETRY frame of the node and then sends the command with the echo of that frame. It sends the command again after 5 seconds with the echo of the newest frame, until it gets RESULT or the life ends. The life in the frame is what remains of the life of the backend at that moment, and the hub sends no command whose life has ended.
- The node switches the relay for the action (1 on, 0 off) and answers RESULT with the same number. A Clamp does not switch a machine, so it answers RESULT with ok 0 to every command.
- A command that finds the node with no channel waits in the hub until the node is back, and the backend life decides whether it is still sent.

## Overload and conditions

- The node decides the condition of its samples (0 normal, 1 standby, 2 motor degradation, 3 overload) and the hub passes it on as `a`.
- A Plug switches its relay off by itself when the power is above 2100 W and sends a TELEMETRY frame at once with condition 3 and relay 0. It keeps sending condition 3 on every sample until a command switches the relay on again, because the load is about 0 W after the cut and the overload has nothing else to clear it. The node never switches the relay on by itself.
- A Clamp only reports condition 3. The limit of a Clamp is set for the machine it measures, and a Clamp has no default limit, so it reports no overload until the limit is set.

## What the hub does with the frames

- The hub puts the newest samples of every node in one batch for the backend every 10 to 60 seconds. A batch holds at most 16 nodes and 1000 samples for each node, and the hub sends more than one batch when it has more nodes than that. The backend allows 600 batches in an hour, so the hub never sends a batch for each frame.
- Every batch holds at least one node, because the backend sends the commands in the answer to a batch.
- The buffer of the hub for an outage of the Wi-Fi network and the Wi-Fi power saving mode of the hub belong to the design of the hub firmware and are not part of this protocol.
