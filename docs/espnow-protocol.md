# ESP-NOW protocol between nodes and the hub

This page describes version 1 of the frames that a node (Plug or Clamp) and the hub exchange over ESP-NOW. It is the design that the first firmware follows. The contract between the hub and the backend is in the [firmware guide](https://github.com/aethera-deeptech/backend/blob/main/docs/firmware-guide.md) and is not repeated here.

## Topology

- One hub and many nodes form a star. Every node talks to the hub only. A node never relays a frame, so the `hops` value that the hub sends to the backend is always 0.
- The hub joins the Wi-Fi network of the building to reach the backend, so ESP-NOW runs on the channel of that network. A node does not know the channel when it starts, so it finds the hub first.
- A frame is at most 250 bytes, which is the limit of ESP-NOW version 1.0. The frames on this page stay below it.
- The encryption is done by the firmware in each frame and not by the link layer of ESP-NOW. The link layer allows 17 encrypted peers at most, and the backend allows 64 nodes for a gateway. The hub adds a node as a plain peer only while it sends to it, so the table of 20 peers never limits the number of nodes.

## Identity

- A node is identified by the MAC address of its radio. The hub builds the `nodeId` for the backend from the kind and the last 3 bytes of the MAC, for example `PLUG-A1B2C3` or `CLAMP-A1B2C3`. The id stays the same after a restart, because the backend counts every different id of a gateway and keeps at most 64 for ever.
- The hub keeps in flash every distinct id that it ever reported to the backend. It refuses to pair a node whose id it already knows, and it refuses a new id when it has reported 64. A node that is removed and paired again has the same MAC and so the same id, and it costs no new slot. A replaced node has a new MAC and takes a new slot for ever, so 64 ids is a hard limit for the life of a gateway.
- The kind is `plug` or `clamp`, which are the only kinds that the backend accepts. A Clamp meter is a Clamp for the protocol.

## Frame layout

Every frame starts with a header of 10 bytes. Numbers are little endian.

| Offset | Size | Field | Meaning |
| :--- | :--- | :--- | :--- |
| 0 | 1 | `version` | Protocol version, 1 for this page |
| 1 | 1 | `type` | Frame type from the table below |
| 2 | 4 | `bootId` | Number of the start of the sender, which grows by 1 at every start and is kept in flash |
| 6 | 4 | `counter` | Number of the frame of the sender, counted from 0 at every start |

The frames of the encrypted types have the payload followed by a tag of 16 bytes. The payload and the tag are made with AES-128-GCM. The header is the associated data, and the nonce is the `bootId`, the `counter`, a byte for the direction (0 node to hub, 1 hub to node) and 3 bytes of zero. A device has one `bootId` for all its peers. It writes the new value to flash and waits for the write to be confirmed before it sends the first frame, so a pair of `bootId` and `counter` is never used twice with the same key. A device whose flash was erased or restored from an old image has lost this guarantee, so the owner removes it and pairs it again, which makes a new key. One key serves both directions, and the direction byte keeps their nonces apart. A receiver drops a frame with an unknown version, an unknown type, a length that does not fit the type or a tag that does not verify.

| Type | Name | Direction | Encrypted | Payload |
| :--- | :--- | :--- | :--- | :--- |
| 0x01 | HELLO | node to broadcast | no | kind (1 byte), firmware version (3 bytes, major then minor then patch, one byte each) |
| 0x02 | HELLO_ACK | hub to node | no | none, the channel is the one that carried the frame |
| 0x10 | PAIR_REQUEST | node to hub | no | kind (1 byte, 1 for `plug` and 2 for `clamp`), X25519 public key (32 bytes) |
| 0x11 | PAIR_ACCEPT | hub to node | no | X25519 public key of the hub (32 bytes), then the X25519 public key of the node from the request that it answers (32 bytes) |
| 0x20 | TELEMETRY | node to hub | yes | `bootId` of the hub as the node last saw it (4 bytes), sample count (1 byte), then the samples |
| 0x21 | ACK | hub to node | yes | `bootId` of the node (4 bytes), highest `sampleSeq` that the hub received (2 bytes) |
| 0x30 | COMMAND | hub to node | yes | echo (8 bytes, the `bootId` and the `counter` of a TELEMETRY frame of the node), command number (2 bytes), action (1 byte) |
| 0x31 | RESULT | node to hub | yes | `bootId` of the hub as the node last saw it (4 bytes), command number (2 bytes), ok (1 byte) |

### Sample

A TELEMETRY frame holds 1 to 8 samples of 23 bytes each, which makes a frame of at most 215 bytes.

| Size | Field | Unit | Maps to |
| :--- | :--- | :--- | :--- |
| 2 | `sampleSeq` | number of the sample of the node, counted from 0 at every start and wrapping at 65536 | none, the hub uses it to find a repeat |
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
- A frame that is sent again has a new `counter` and a new `ageMs` and is encrypted again, and it holds the oldest samples that the hub has not acknowledged. The hub drops a sample whose `sampleSeq` it already has for the same `bootId`, and it sends an ACK also for a frame that holds only repeats. The hub treats every sample before the first sample of a frame as lost for good, and a sample that it dropped as received, so a gap never stops the ACK. A sample is new when its `sampleSeq` is up to 32768 ahead of the last one that the hub stored, counted in a circle. The hub keeps this only in RAM.
- A node keeps the samples that are not acknowledged, up to 64 samples, which is a little more than 10 minutes. When the buffer is full, it drops the oldest sample first. A node ignores an ACK whose `bootId` is not its own or whose `sampleSeq` is above its newest sample. When more than 8 samples wait, it sends the oldest 8 and then sends the next frame 1 second later, until fewer than 8 wait.
- The ACK means that the hub holds the sample in its memory and not that the backend has it. A hub that restarts can lose up to 30 seconds of samples, and a sample that arrives again after the restart may reach the backend twice with a shifted time.
- The hub drops the whole sample when its power is missing or out of the range of the contract, because the backend needs the power. It leaves out any other field that is out of range, so the backend does not refuse the sample.
- The energy is the counter of the PZEM-004T, which the PZEM keeps in its own memory. The hub sends it in kWh. A counter that falls after a replaced meter is not an error, because the backend then takes the energy of that gap from the power, for at most 5 minutes of it, so a little energy can be lost.
- A Plug sends the power factor only when it is set to motor mode, and a Clamp always sends it. The backend judges every node that sends `pf`, and a charger or a lamp has a low power factor that would raise a false motor alert.

## Finding the hub

1. A node that has no stored channel sends HELLO on channel 1 and waits 200 ms for HELLO_ACK. It tries the next channel up to channel 13.
2. The hub answers every HELLO with HELLO_ACK on the channel of its own radio through a temporary peer, which it removes at once. It answers at most 5 HELLO frames in a second.
3. The node stores the channel. After 10 sends in a row that fail, or after 2 minutes without a valid ACK, it forgets the channel and starts again at step 1. It waits a random time of up to 10 seconds before it starts, so several nodes do not call at the same moment after a channel change.
4. A node that has had no valid ACK for 10 minutes shows it with its LED, because the hub may have forgotten its key.

## Pairing and keys

- The hub has a pairing window that opens when the owner presses a button on the hub and closes after one pairing or after 60 seconds. The hub shows the id of the node on its display. A node asks for pairing when the owner holds its button for 3 seconds, and stops after 60 seconds. A short press on a paired node does nothing.
- The node sends PAIR_REQUEST with a new X25519 public key. The hub, inside its window, answers PAIR_ACCEPT with its own new key and the node key from that request. A node ignores a PAIR_ACCEPT whose echoed node key is not the key of its own pending request, and both sides refuse a shared secret that is all zero.
- Each side keeps the new key as pending. The node sends its first TELEMETRY frame with the pending key and commits the key only when it receives the ACK for that frame. The hub accepts the first TELEMETRY frame and its repeats with the pending key, commits the key when it verifies a frame, and answers with the ACK, and then the window closes. When PAIR_ACCEPT is lost, or the window ends first, nothing is committed and the owner pairs again. The node also stores the MAC of the hub, and a paired node accepts HELLO_ACK and ACK only from that MAC.
- The hub refuses a node that it already knows, and it shows the message on its display. The owner removes the node on the hub first, and then pairs it again. A node whose button hold of 10 seconds forgot the hub is in this case.
- Each side takes 16 bytes from HKDF-SHA256. The input keying material is the 32 bytes of the shared secret. The salt is the 32 bytes of the SHA-256 of the text `wattlink-espnow-v1`. The info field is 76 bytes made by joining, with no separator, the public key of the node (32 bytes), the public key of the hub (32 bytes), the MAC of the node (6 bytes) and the MAC of the hub (6 bytes), in this order and with each MAC in the order of its bytes as the radio prints it. The output length is 16 bytes, and it is the key of AES-128-GCM for this node.
- The key lives in the flash of both sides. The hub forgets a node when the owner removes it, and a node forgets the hub when the owner holds its button for 10 seconds, and so the hold of 3 seconds pairs and the hold of 10 seconds forgets. A device keeps the key in plain flash, so someone with physical access can read it unless flash encryption is turned on.
- Limit of the design. Someone who is close to the devices during the pairing window and who can transmit can pair as a node, or answer as the hub. The window is short and the owner opens it on purpose and sees the node id, but the exchange has no check that the owner could make of the other device. After pairing, the traffic is encrypted and authenticated and the hub only accepts the paired nodes.

## Order and repeats

These rules apply to the encrypted frames. HELLO, HELLO_ACK, PAIR_REQUEST and PAIR_ACCEPT have the header but no freshness, and they change nothing until a pairing is committed.

- A receiver keeps the highest `bootId` of each peer in flash. It writes a new value only after the tag of the frame has verified, and only when the value grows. It drops a frame with a lower `bootId`, and with the same `bootId` it drops a frame whose `counter` is not higher than the last one. A frame of an old start of the sender can therefore not be played again. A new pairing makes a new key, and the receiver then forgets the `bootId` that it kept for that peer.
- The `counter` of a peer lives only in RAM, so after a restart of the receiver the newest frame of the current `bootId` could be played once. A node therefore puts the `bootId` of the hub in every TELEMETRY and RESULT frame, and the hub drops a frame whose value is not its current `bootId`. It answers such a frame with an ACK that carries its `bootId` in the header, so the node learns the new value. A node binds the other direction the same way, because an ACK holds the `bootId` of the node and a COMMAND echoes a frame of the node.
- A COMMAND is accepted only when its echo is the `bootId` and the `counter` of one of the last 6 TELEMETRY frames that the node sent. A node sends a frame every 10 seconds, so a captured command is useless after a short time, and a command that arrives twice does no harm because the action is never a toggle. A frame that a node sends in a burst, such as the one after an overload cut, also counts toward the 6.
- A node keeps the numbers of the commands that it ran for 10 minutes, and it answers a command that it already ran with RESULT only. This way a resend of an old "on" does not switch the relay on again after an overload cut.

## Commands

- The hub gets a command from the backend with a string id and a life of up to 10 minutes. It gives the command a number of 2 bytes that is only used on the link, and keeps the table between the number and the id in flash for 10 minutes, so a hub that restarts does not run the same backend id twice. It drops a waiting command for a plug when a newer one for the same plug arrives, so the commands never run in the wrong order.
- The hub waits for the next TELEMETRY frame of the node and then sends the command with the echo of that frame. It sends the command again after 5 seconds with the echo of the newest frame, until it gets RESULT or the life ends. The hub sends no command whose life of 10 minutes has ended, and the node has no clock for it.
- The node switches the relay for the action (1 on, 0 off) and answers RESULT with the same number. The hub reports the result to the backend in its next batch. A Clamp does not switch a machine, so it answers RESULT with ok 0 to every command.
- A command that finds the node with no channel waits in the hub until the node is back, and the backend life decides whether it is still sent.

## Overload and conditions

- The node decides the condition of its samples (0 normal, 1 standby, 2 motor degradation, 3 overload) and the hub passes it on as `a`.
- A Plug switches its relay off by itself when the power is above 2100 W and sends a TELEMETRY frame at once with condition 3 and relay 0. It keeps sending condition 3 on every sample until a command switches the relay on again, because the load is about 0 W after the cut and the overload has nothing else to clear it. The node never switches the relay on by itself.
- A Clamp switches nothing, and version 1 has no overload limit for it, so a Clamp reports condition 3 never.

## What the hub does with the frames

- The hub sends a batch every 30 seconds, so a node that sends a sample every 10 seconds has 3 samples in each batch, which the backend needs to judge a motor. The hub sends the samples of one node together.
- A batch holds at most 16 nodes, 1000 samples for each node and 5000 samples in all, and the body is at most 256 KiB. The hub splits the nodes into more batches when a batch would pass one of these limits, and it keeps the time between two batches of the gateway at 6 seconds or more, because the backend allows 600 batches in an hour. With 64 nodes that is 4 batches in each 30 seconds, which is 480 in an hour.
- Every batch holds at least one node, because the backend sends the commands in the answer to a batch.
- The buffer of the hub for an outage of the Wi-Fi network and the Wi-Fi power saving mode of the hub belong to the design of the hub firmware and are not part of this protocol.
