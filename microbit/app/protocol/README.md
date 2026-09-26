# TRON BLE advertising mesh-like protocol

This directory contains shared protocol code for the experimental BLE lane.

This transport is **not Bluetooth Mesh compliant**. It intentionally uses a
lab-only Manufacturer Specific Data AD structure (`type=0xFF`, company ID
`0xFFFF`) instead of the official Bluetooth Mesh Mesh Message AD type (`0x2A`),
because this firmware does not implement provisioning, mesh security, replay
protection, IV index, SAR, GATT/PB-GATT, or mesh models.

## Packet contract

Legacy AdvData is capped at 31 bytes:

1. Flags AD structure: `[0x02, 0x01, 0x06]`.
2. Manufacturer Specific Data AD structure:
   - length byte: `0x0F + payload_len`, max `0x1B`;
   - AD type: `0xFF`;
   - company ID LE: `0xFFFF`;
   - magic: ASCII `T`, `M`;
   - version: `0x01`;
   - message type: `uint8_t` (`0x01` dummy/status, `0x02` PING, `0x03` PONG);
   - network/group ID: `uint8_t`;
   - TTL: `uint8_t`, max `3`;
   - source ID: `uint16_t` little-endian;
   - sequence: 24-bit little-endian, RAM-only per boot;
   - payload length: `uint8_t`, max `12`;
   - payload bytes.

Total AdvData length is `19 + payload_len`, exactly 31 bytes at maximum payload.
Encoders must fail before raw-radio advertising would truncate.

## Addressed two-board PING/PONG

`tron_mesh_pingpong` adds a pure, host-tested best-effort application exchange
without changing the wire header or flood scheduler:

- root/initiator ID: `0x0001`;
- leaf/responder ID: `0x0002`;
- network ID: `0x01`;
- PING type: `0x02`, exact payload `[0x02, 0x00]` (leaf destination LE);
- PONG type: `0x03`, exact payload `[0x01, 0x00]` (root destination LE);
- both encode to exactly 21 AdvData bytes;
- PONG uses leaf source and echoes the PING's 24-bit sequence. Message type and
  source therefore keep the PING and PONG dedupe keys distinct;
- root attempts one PING every 2000 ms and allows one pending transaction;
- pending starts only after scheduler enqueue success and expires after 1500 ms;
- compile-time guards require timeout to be less than interval and keep both
  values below the wrap-safe 32-bit half-range;
- every due attempt advances the periodic deadline, including queue failures;
- leaf answers only an exact root-to-leaf PING; root correlates only an exact
  leaf-to-root PONG with the pending sequence;
- TTL zero is still valid for addressed local handling, but is not relayed;
- no ACK, retry, or overlapping pending transaction is added.

Logs call enqueue-to-matched-PONG application latency **RTT**. It includes
scheduler queueing and advertisement flooding delay; it is not a link-layer or
Bluetooth controller round-trip measurement. The designated root and leaf do
not originate legacy DUMMY/status packets.

Firmware receive processing is ordered as decode/network admission, optional
test-only direct-peer drop, supported PING/PONG semantic validation, dedupe,
addressed local response/correlation, then normal TTL-bounded relay. Invalid
PING/PONG frames are dropped before they can poison dedupe state.
The transport `rx_ok` counter still increments at admission before semantic and
dedupe drops. Periodic logs keep transport counters on `mesh counters` and put
bidirectional semantic, queue success/failure, timeout, and RTT fields on a
separate concise `bidi counters` line.

## Ownership boundaries

- `tron_mesh_packet` is pure encode/decode logic over AdvData bytes. It must not
  read FICR, own sequence counters, or access RADIO state.
- Node/platform code owns FICR-derived default node IDs, optional `TRON_NODE_ID`,
  and RAM-only sequence counters.
- Scheduler code owns raw-radio calls during `ble_mesh_node` operation and must
  restore passive RX after bounded TX windows.
- Firmware targets include shared sources directly in their target CMake unless
  a small shared object/static library is introduced deliberately.

## Planned no-hardware tests

From `microbit/`:

```bash
./tests/protocol/run_tron_mesh_packet_tests.sh
./tests/protocol/run_tron_mesh_pingpong_tests.sh
./tests/protocol/run_tron_mesh_dedupe_tests.sh
```

If the scheduler slice adds a host/simulation harness:

```bash
./tests/protocol/run_ble_mesh_scheduler_tests.sh
```
