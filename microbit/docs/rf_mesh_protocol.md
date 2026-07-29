# TRON RF mesh protocol v2 draft

Status: implementation-facing draft for the RF mesh lane. This is a sane
project-owned RF design for micro:bit v2 / nRF52833. It is **not** BLE mesh and
does not target MakeCode or MicroPython wire compatibility. Payloads are dummy
for now and protocol details may evolve after later user comments.

## Goals and non-goals

Goals:

- Use nRF52833 proprietary 2.4 GHz RADIO mode from μT-Kernel firmware.
- Support root-aware/AODV-style discovery and route maintenance.
- Use bounded flooding for discovery, root beacons, relays, and broadcast data.
- Keep all queues, caches, payloads, and timing rules fixed-size and testable.
- Prefer RX-first scheduling because the board has one radio and cannot TX/RX
  simultaneously.

Non-goals for v2:

- No BLE mesh, BLE connections, or SoftDevice dependency.
- No MakeCode/MicroPython micro:bit-radio packet compatibility.
- No encryption or authentication. V2 carries both previous-hop identity and a
  software immediate-receiver ID so next-hop routing is enforced despite the
  network-wide physical RADIO address.
- No final application payload semantics. DATA carries dummy/test bytes only.
- No hardware flashing or RF-runtime claims from this software workflow.

## Constants

| Name | Value | Notes |
| --- | ---: | --- |
| Protocol version | `2` | Reject other versions. |
| Max on-air payload | 32 bytes | The RADIO length field covers these bytes. |
| Header length | 18 bytes | Fixed header before dummy payload. |
| Max dummy payload | 14 bytes | `32 - 18`. |
| Broadcast id | `0xffff` | Destination/root marker for broadcast. |
| Default network id | `0x5452` | ASCII-ish `TR`, compile-time configurable. |
| Default root id | `0x0001` | Compile-time configurable. |
| Default TTL | `5` | Decrement before forwarding; drop at zero. |
| Max routes | `8` | Fixed route table. |
| Dedup entries | `16` | Fixed duplicate cache. |
| TX queue entries | `8` | Fixed outgoing queue. |
| Route expiry | `10000 ms` | Stale routes ignored. |
| Dedup expiry | `8000 ms` | Allows sequence reuse over time. |
| Root beacon interval | `1000 ms` | Root sends HELLO. |
| Dummy data interval | `3000 ms` | Non-root sends dummy DATA toward root if routed. |
| Relay jitter | `20..120 ms` | Deterministic fallback allowed. |
| Discovery jitter | `40..180 ms` | Used for RREQ floods. |

## Packet ABI

All multi-byte integers are little-endian. The radio packet payload is exactly
this protocol packet. The nRF RADIO address, CRC, and whitening are not part of
the protocol byte layout.

```text
offset  size  field
0       1     version_type: high nibble version, low nibble message type
1       1     flags
2       2     network_id
4       2     source_id
6       2     destination_id
8       2     sequence_id
10      1     ttl
11      1     hop_count
12      1     metric
13      1     payload_len
14      2     previous_hop_id
16      2     immediate_receiver_id
18      N     dummy payload bytes, where 0 <= N <= 14
```

`payload_len` must be `<= 14`, and the received packet length must equal
`18 + payload_len`. Pack/unpack code must reject truncated packets, overlong
payloads, unsupported versions, unknown message types, network-id mismatches,
and invalid packet lengths. `previous_hop_id` is the immediate transmitter for
the current hop. Locally originated packets set it to the local node id; each
relay rewrites it to the relaying node id while keeping `source_id` as the
originator for deduplication. `immediate_receiver_id` is set from the queued
next hop before transmission. Receivers process a frame only when this field is
their local node ID or `0xffff` broadcast; other overhearing nodes drop it
before deduplication, route learning, delivery, or relay.

### Message types

| Type | Value | Meaning |
| --- | ---: | --- |
| `HELLO` | `1` | Root beacon / route advertisement. |
| `RREQ` | `2` | Route request flood. |
| `RREP` | `3` | Route reply toward requester. |
| `DATA` | `4` | Dummy payload; unicast to root or broadcast. |
| `ACK` | `5` | Reserved/deferred; not emitted in v2. |

### Flags

| Flag | Value | Meaning |
| --- | ---: | --- |
| `ROOT` | `0x01` | Sender is configured root. |
| `BROADCAST` | `0x02` | Packet is intended for flood/broadcast behavior. |
| `RELAYED` | `0x04` | Packet was forwarded at least once. |
| `ROUTE_REPLY` | `0x08` | RREP carries usable route information. |

Other bits are reserved and must be zero in transmitted v2 packets. Receivers
ignore unknown reserved bits after masking for known behavior.

## Routing behavior

Each node has a 16-bit node id. V1 configuration is compile-time by default:

- `RF_MESH_NODE_ID`: explicit node id, or `0` to derive from FICR device address.
- `RF_MESH_IS_ROOT`: `1` for root, `0` for normal node.
- `RF_MESH_NETWORK_ID`, `RF_MESH_ROOT_ID`, RF channel, data rate, and TX power.

There is no root election in v2. A root is configured. Non-root nodes prefer the
configured root and maintain the best known route toward it.

### Route table

Each route entry stores:

- destination id;
- next-hop sender id;
- hop count;
- metric, lower is better;
- latest sequence id seen for that destination/root;
- last-seen timestamp in milliseconds;
- validity flag.

Route update tie-break:

1. Reject expired entries before comparison.
2. Prefer newer destination/root sequence id when sequence freshness is
   comparable by unsigned 16-bit wrap-aware difference.
3. Prefer lower metric.
4. Prefer lower hop count.
5. Prefer most recently seen route.

Routes expire after `RF_MESH_ROUTE_EXPIRY_MS` without refresh.

### HELLO / root beacon

The configured root periodically broadcasts HELLO with:

- `source_id = root_id`;
- `destination_id = broadcast`;
- `sequence_id = root beacon sequence`;
- `ttl = default TTL`;
- `hop_count = 0`;
- `metric = 0`;
- `ROOT | BROADCAST` flags.

Receivers learn/refresh a route to the root via the packet sender. If TTL
permits and the packet is not a duplicate, non-root nodes enqueue one relayed
HELLO with TTL decremented, hop count incremented, metric incremented, and
`RELAYED` set. The next hop is the packet's `previous_hop_id`.

### RREQ / route request

When a node has dummy DATA for the root and no valid route, it broadcasts RREQ:

- `source_id = requester`;
- `destination_id = root_id` or broadcast if root unknown;
- `sequence_id = local request sequence`;
- `ttl = default TTL`;
- `hop_count = 0`;
- `metric = 0`;
- `BROADCAST` flag.

Every receiver records a reverse route to the requester via `previous_hop_id`.
Duplicate RREQs are dropped. Non-root receivers relay non-duplicate RREQs when
TTL remains. The root replies with RREP toward the requester if a reverse route
is known; otherwise it may broadcast RREP with `BROADCAST` as a fallback.

### RREP / route reply

RREP establishes a route to the root/destination:

- `source_id = root_id` for root replies;
- `destination_id = requester`;
- `sequence_id = root route sequence`;
- `ttl = default TTL`;
- `hop_count` and `metric` describe distance from root after each relay;
- `ROUTE_REPLY` flag, plus `RELAYED` after forwarding.

Receivers learn a route to the RREP source via `previous_hop_id`. If the local
node is not the destination and has a valid next hop toward `destination_id`, it
forwards the RREP unicast-style by sending the same packet after TTL decrement,
hop/metric increment, and jitter. If no route exists, it may flood once with
`BROADCAST` set in v2.

### DATA

DATA carries dummy/test payload bytes only.

- Root-bound DATA uses `destination_id = root_id` when a valid root route exists.
- Broadcast DATA uses `destination_id = broadcast` and `BROADCAST` for stress
  tests or diagnostics.
- Receivers locally deliver DATA addressed to themselves or broadcast.
- Relays forward non-duplicate DATA when TTL remains and either the packet is
  broadcast or a valid next hop exists for the destination.

ACK and retransmission are deferred in v2. Loss is tolerated and counted.

## Duplicate suppression

Dedup key:

```text
(network_id, message_type, source_id, sequence_id)
```

The cache stores first-seen timestamp and source. A packet with an existing,
unexpired key is dropped and increments the duplicate counter. Expired entries
may be reused. Dedup applies to HELLO, RREQ, RREP, and broadcast DATA. It also
applies to unicast DATA relays to limit loops.

## Single-radio scheduler / MAC

V2 is RX-first and polled. The app keeps the RADIO in RX whenever it is not
performing a short TX. There is no simultaneous TX/RX.

State loop:

1. Poll RX once or a small bounded burst.
2. Decode and update dedup/routes/counters.
3. Enqueue relays or local dummy DATA/discovery work with due times.
4. If the earliest TX item is due, disable RX, transmit one packet, then return
   immediately to RX.
5. Sleep/yield briefly with `tk_dly_tsk(1)` when idle.

TX queue priority from highest to lowest:

1. RREP toward a requester;
2. local DATA/RREQ;
3. relayed HELLO/RREQ/DATA.

When the TX queue is full, drop the lowest-priority newest relay first. If no
relay can be dropped, reject the new lower-priority item and increment the drop
counter. Local DATA may be dropped in v2 rather than blocking.

Jitter/backoff uses a small deterministic PRNG seeded from FICR-derived node id
and monotonic time. If entropy is unavailable, the deterministic sequence is
accepted for v2.

## RF radio ABI

The proprietary RF driver is separate from `ble_radio` and must not be active in
the same firmware target as BLE radio code.

Default radio parameters:

- data rate: Nordic proprietary 1 Mbit mode;
- frequency: channel `7` => `2407 MHz`, compile-time configurable `0..83`;
- TX power: 0 dBm by default, compile-time configurable using nRF TXPOWER
  register values;
- logical address: one RADIO logical address (`TXADDRESS=0`, `RXADDRESSES=1`);
- base/prefix: fixed project-owned values derived from `RF_MESH_NETWORK_ID`;
- packet config: no S0/S1 in v2, 8-bit LENGTH field, max payload 32 bytes;
- hardware CRC: 16-bit CRC enabled over packet payload, skip address;
- whitening: enabled with `DATAWHITEIV = channel & 0x3f`;
- RSSI: returned as positive dBm magnitude where hardware exposes it, matching
  the existing BLE driver convention (`actual ~= -rssi_dbm`).

Public API:

```c
void rf_radio_init(const rf_radio_config_t *cfg);
void rf_radio_listen(void);
int rf_radio_poll(UB *buf, UINT *len, UINT *rssi_dbm);
int rf_radio_send(const UB *buf, UINT len);
```

`rf_radio_send` sends one packet synchronously and returns after RADIO disabled.
The app must call `rf_radio_listen` again immediately after any TX. `rf_radio_poll`
is non-blocking and only returns CRC-valid packets with lengths `<= 32`.

## Core interfaces

Planned hardware-independent headers:

- `rf_mesh_packet.h`: packet structs, constants, pack/unpack helpers.
- `rf_mesh_core.h`: mesh state, route/dedup/queue APIs, counters.
- `rf_radio.h`: hardware transport API above.
- `rf_mesh_config.h`: firmware target config constants for node/root/network/RF.

Implementation slices must treat these interfaces as frozen for v2 unless a
parent integration pass updates all affected slices and tests together.

## Validation expectations

Non-hardware validation only:

- host tests for packet bounds, malformed/truncated packets, dedup expiry,
  TTL/drop behavior, route updates/expiry, queue overflow, and jitter bounds;
- firmware compile for existing targets and `rf_mesh_node`;
- code-checker reviews after protocol core, RF driver, app/integration, and
  before any future hardware proposal.

Hardware validation is explicitly out of scope until Hermes approves a separate
flash/run plan.
