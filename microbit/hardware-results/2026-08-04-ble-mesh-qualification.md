# BLE mesh hardware qualification — 2026-08-04

## Source and tools

- Branch: `origin/exp/ble-mesh`
- Commit: `a09804824edfa4eb087c4123c26d593aab77391e`
- Target: `ble_mesh_node`
- Kernel submodule: `5606cfba1625350901ad2eb521572b0a3f7735cb`
- pyOCD: `0.45.1`
- grabserial: `2.0.4`
- ARM GCC: `16.1.0`

Host verification before flashing:

- 16 TRON packet tests passed.
- 11 duplicate-suppression tests passed.
- BLE radio-driver host tests passed.
- BLE scheduler host tests passed.
- All three labelled ARM EABI5 ELFs built successfully.

## Board mapping

| Role | Node ID | Probe UID | Stable serial link | Artifact SHA-256 |
| --- | --- | --- | --- | --- |
| NODE-A | `0x0001` | `9906360200052820cf57b9f988a30e16000000006e052820` | `/dev/serial/by-id/usb-Arm_BBC_micro:bit_CMSIS-DAP_9906360200052820cf57b9f988a30e16000000006e052820-if01` | `f8ae6e1cd7511f6fd161ddec8d4d6c35affd8d850acef56f80f8078af51a9b60` |
| RELAY | `0x0002` | `99063602000528205539bee7957c8dea000000006e052820` | `/dev/serial/by-id/usb-Arm_BBC_micro:bit_CMSIS-DAP_99063602000528205539bee7957c8dea000000006e052820-if01` | `85a9d19e28d42df0ce667dab061ac7560ce9adf66a3b0c65a3a5970ed6a1aab7` |

pyOCD identified NODE-A as a BBC micro:bit V2 with an nRF52833 target. Its
board-ID prefix `9906` identifies the v2.21 board revision.

## NODE-A standalone smoke test

- Image: `artifacts/ble_mesh_node-0x0001.elf`
- Test topology setting: direct full-TTL packets from `0x0003` are ignored.
- Procedure: explicit-UID mass erase, load, reset, then 60-second timestamped
  serial capture at 115200 baud.
- Log: `2026-08-04-node-a-0x0001-smoke.log`
- Result: PASS for standalone boot, TX progress, serial stability, and no reset.
- Final observed counters near 59 seconds:
  - `rx_ok=0` (no peer connected)
  - `tx_ok=59`
  - `queue_drop=0`
  - `relay_scheduled=0`
  - `relay_rate_limited=0`
  - `blocked_direct=0`

`decode_error` reached 2957 while NODE-A was the only TRON node. These are
ambient advertisements rejected by the TRON decoder; the counter name does not
distinguish unrelated BLE traffic from malformed TRON traffic.

## RELAY standalone smoke test

- Image: `artifacts/ble_mesh_node-0x0002.elf`
- Test topology setting: direct-peer blocking disabled.
- Procedure: explicit-UID mass erase, load, reset, then 60-second timestamped
  serial capture at 115200 baud.
- Log: `2026-08-04-relay-0x0002-smoke.log`
- Result: PASS for standalone boot, TX progress, serial stability, and no reset.
- Final observed counters near 59 seconds:
  - `rx_ok=0` (no peer connected)
  - `tx_ok=59`
  - `queue_drop=0`
  - `relay_scheduled=0`
  - `relay_rate_limited=0`
  - `blocked_direct=0`

`decode_error` reached 3519 while RELAY was the only TRON node, consistent with
ambient non-TRON advertisements reaching the generic decode-failure counter.

## Two-node exchange test

Both labelled boards were connected simultaneously using their stable serial
links. No erase, reset, or flash occurred during these captures.

### 60-second smoke exchange

- Logs:
  - `2026-08-04-two-node-60s-node-a.log`
  - `2026-08-04-two-node-60s-relay.log`
- NODE-A originated 60 packets and first-saw/relayed 51 RELAY packets.
- RELAY originated 60 packets and first-saw/relayed 49 NODE-A packets.
- Approximate unique peer-packet observation was 85.0% at NODE-A and 81.7% at
  RELAY.
- Duplicate counters progressed on both nodes.
- Neither node reported queue drops, TTL drops, relay-rate limits, or a reset.

### Five-minute stability exchange

- Logs:
  - `2026-08-04-two-node-5m-node-a.log`
  - `2026-08-04-two-node-5m-relay.log`
- NODE-A originated 299 packets and first-saw/relayed 241 RELAY packets.
- RELAY originated 300 packets and first-saw/relayed 242 NODE-A packets.
- Approximate unique peer-packet observation remained about 80–81%.
- NODE-A cumulative counters progressed from `rx_ok=234, tx_ok=274` near the
  beginning to `rx_ok=671, tx_ok=806` near the end.
- RELAY cumulative counters progressed from `rx_ok=230, tx_ok=713` near the
  beginning to `rx_ok=663, tx_ok=1246` near the end.
- Duplicate suppression progressed from 104 to 304 at NODE-A and from 110 to
  306 at RELAY.
- Both nodes retained `queue_drop=0`, `ttl_drop=0`, and
  `relay_rate_limited=0` throughout the capture.
- No reboot marker occurred, and pyOCD still detected both nRF52833 targets
  after the run.

Result: PASS for two-node bidirectional exchange and five-minute runtime
stability. The transport remains best-effort: the measured observation rate is
not sufficient evidence of reliable event delivery without event repetition,
ACK/retry, or higher-layer redundancy.

## Tests not run

- Isolated three-node relay proof: only two boards were available.
- Relay-loss and path-recovery test: requires a third node for a meaningful
  source → relay → destination topology.
- Patient-beacon observation during mesh operation: `ble_mesh_node` still
  decodes only the TRON dummy mesh packet format.
