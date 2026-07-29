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
   - message type: `uint8_t` (`0x01` dummy/status for the first slice);
   - network/group ID: `uint8_t`;
   - TTL: `uint8_t`, max `3`;
   - source ID: `uint16_t` little-endian;
   - sequence: 24-bit little-endian, RAM-only per boot;
   - payload length: `uint8_t`, max `12`;
   - dummy payload bytes.

Total AdvData length is `19 + payload_len`, exactly 31 bytes at maximum payload.
Encoders must fail before raw-radio advertising would truncate.

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
```

If the scheduler slice adds a host/simulation harness:

```bash
./tests/protocol/run_ble_mesh_scheduler_tests.sh
```
