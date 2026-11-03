# ESP-NOW protocol between nodes and the hub

This page describes version 1 of the frames that a node (Plug or Clamp) and the hub exchange over ESP-NOW. It is the design that the first firmware follows. The contract between the hub and the backend is in the [firmware guide](https://github.com/aethera-deeptech/backend/blob/main/docs/firmware-guide.md) and is not repeated here.

## Topology

- One hub and many nodes form a star. Every node talks to the hub only. A node never relays a frame, so the `hops` value that the hub sends to the backend is always 0.
- The hub joins the Wi-Fi network of the building to reach the backend, so ESP-NOW runs on the channel of that network. A node does not know the channel when it starts, so it finds the hub first.
- Every frame is at most 250 bytes, which is the limit of ESP-NOW.

## Identity

- A node is identified by the MAC address of its radio. The hub builds the `nodeId` for the backend from the kind and the last 3 bytes of the MAC, for example `PLUG-A1B2C3` or `CLAMP-A1B2C3`. The id stays the same after a restart, because the backend counts every different id of a gateway and keeps at most 64 for ever.
- The node sends its kind in the frames that need it. The kind is `plug` or `clamp`, which are the only kinds that the backend accepts. A Clamp meter is a Clamp for the protocol.

## Frame layout

Every frame starts with a header of 6 bytes. Numbers are little endian.

| Offset | Size | Field | Meaning |
| :--- | :--- | :--- | :--- |
| 0 | 1 | `version` | Protocol version, 1 for this page |
| 1 | 1 | `type` | Frame type from the table below |
| 2 | 4 | `counter` | Number of the frame of the sender, counted from 0 at every start |

A receiver drops a frame with an unknown version, an unknown type or a length that does not fit the type. The `counter` is for ordering and for finding a repeated frame. It does not protect the link, because the encryption of ESP-NOW does.

| Type | Name | Direction | Encrypted | Payload |
| :--- | :--- | :--- | :--- | :--- |
| 0x01 | HELLO | node to broadcast | no | kind (1 byte), firmware version (3 bytes) |
| 0x02 | HELLO_ACK | hub to node | no | none, the channel is the one that carried the frame |
| 0x10 | PAIR_REQUEST | node to hub | no | kind (1 byte), X25519 public key (32 bytes) |
| 0x11 | PAIR_ACCEPT | hub to node | no | X25519 public key (32 bytes) |
| 0x20 | TELEMETRY | node to hub | yes | sample count (1 byte), then the samples |
| 0x30 | COMMAND | hub to node | yes | command number (2 bytes), action (1 byte), life in seconds (2 bytes) |
| 0x31 | RESULT | node to hub | yes | command number (2 bytes), ok (1 byte) |

### Sample

A TELEMETRY frame holds 1 to 10 samples of 21 bytes each.

| Size | Field | Unit | Maps to |
| :--- | :--- | :--- | :--- |
| 4 | `ageMs` | milliseconds since the node took the sample | `t` |
| 2 | voltage | 0.1 V | `v` |
| 2 | current | 0.01 A | `i` |
| 4 | power | 0.1 W | `p` |
| 4 | energy | Wh | `e` |
| 2 | frequency | 0.1 Hz | `f` |
| 1 | power factor | 0.01 | `pf` |
| 1 | condition | 0 normal, 1 standby, 2 motor degradation, 3 overload | `a` |
| 1 | relay | 1 on, 0 off, 255 none | `r` |

- A node has no clock that matters, so it sends the age of the sample. The hub subtracts the age from its own time when the frame arrives and so makes `t`. This keeps one clock in the system, the one of the hub.
- A value that the node cannot measure is sent as the maximum of its field (all bits set), and the hub leaves it out of the batch. A Clamp sends 255 for the relay, because the backend takes relay commands and the relay state from a plug only.
- The hub sends `e` in kWh, made from the Wh value. An energy counter that falls after a restart of the node is harmless, because the backend then takes the energy of that gap from the power.

## Finding the hub

1. A node that has no stored channel sends HELLO on channel 1 and waits 200 ms for HELLO_ACK. It tries the next channel up to channel 13.
2. The hub answers every HELLO from an unknown or a paired node with HELLO_ACK on the channel of its own radio, so the node learns the channel from the answer.
3. The node stores the channel. After 10 sends in a row that fail, it forgets the channel and starts again at step 1.

## Pairing and encryption

- Every node and the hub use the encryption of ESP-NOW (CCMP) with a key of 16 bytes for each node. A frame of the encrypted types is dropped when it does not arrive with the key of that node.
- The key is made when the node is paired. The hub has a pairing window that opens when the owner presses a button on the hub and closes after 60 seconds. The node asks for pairing when the owner holds the button of the node.
- The node sends PAIR_REQUEST with a new X25519 public key. The hub, inside its window, answers PAIR_ACCEPT with its own key. Each side computes the shared secret and takes the first 16 bytes of HKDF-SHA256 over it as the key, with the two MAC addresses and the text `wattlink-espnow-v1` as the context. The hub adds the node as an encrypted peer and the node adds the hub.
- The key lives in flash on both sides and never leaves the device. The hub forgets a node when the owner removes it, and a node forgets the hub when the owner holds its button for 10 seconds.
- Limit of the design. Someone who is close to the devices during the pairing window and who can transmit can pair as a node, or answer as the hub. The window is short and the owner opens it on purpose, but the exchange has no check that the owner saw. After pairing, the traffic is encrypted and the hub only accepts the paired nodes.

## Commands

- The hub gets a command from the backend with a string id and a life of up to 10 minutes. It gives the command a number of 2 bytes that is only used on the link, and keeps the table between the number and the id.
- The node switches the relay for the action (1 on, 0 off) and answers RESULT with the same number. The action is never a toggle, so a command that arrives twice does no harm.
- The hub sends the command again every 5 seconds until it gets RESULT or the life ends, and it does not send a command that the life of the backend has ended.
- A Clamp does not switch a machine. It answers RESULT with ok 0 to any command.

## Overload

- A Plug switches its relay off by itself when the power is above 2100 W, sends a sample with condition 3 and relay 0, and keeps sending condition 3 on every sample while the overload lasts. A Clamp only reports condition 3.
- The node does not switch the relay on again by itself. Only a command from the hub switches it on.

## Timing

- A node sends a TELEMETRY frame every 10 seconds. It sends 1 sample per frame in normal operation, and it keeps up to 10 samples in a frame when the hub did not answer.
- The hub sends no acknowledgement of TELEMETRY. ESP-NOW tells the sender whether the frame reached the radio of the hub, and the node keeps the samples that failed for the next frame.
