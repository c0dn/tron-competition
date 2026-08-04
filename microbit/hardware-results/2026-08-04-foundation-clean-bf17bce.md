# Clean BLE PING/PONG foundation re-proof

Date: 2026-08-04

This is proof-of-concept evidence for the pre-TAVRN BLE foundation. It does not
claim reliable delivery, Bluetooth Mesh compliance, or production readiness.

## Immutable source and build

- Source commit: `bf17bce301cae10df0819b3b6fe7a8c19256bdde`
- Source tree: `5f831a36029003576a86ee23f3933b01530a6db8`
- Kernel submodule: `5606cfba1625350901ad2eb521572b0a3f7735cb`
- Source state during build: clean detached worktree
- ARM GCC: `16.1.0`
- CMake: `4.4.2`
- Ninja: `1.13.2`
- pyOCD: `0.45.1`
- PING interval: 2000 ms
- PING timeout: 1500 ms
- Direct-peer blocking: disabled

Host validation before flashing:

- 17 packet tests passed.
- 12 duplicate-cache tests passed.
- 12 PING/PONG tests and compile-time timing guards passed.
- BLE radio-driver host tests passed.
- BLE scheduler host tests passed.
- Clean root and leaf ARM EABI5 builds passed.

## Board and artifact mapping

| Role | Node | Probe UID | AdvA | Stable serial link | ELF SHA-256 |
| --- | --- | --- | --- | --- | --- |
| Root | `0x0001` | `9906360200052820cf57b9f988a30e16000000006e052820` | `18:42:de:52:4a:dd` | `/dev/serial/by-id/usb-Arm_BBC_micro:bit_CMSIS-DAP_9906360200052820cf57b9f988a30e16000000006e052820-if01` | `e2598a7b7fb07d8128dc8d8afe945cfc0dd08e1dbfbb303cd0f64da68f5f34d0` |
| Leaf | `0x0002` | `99063602000528205539bee7957c8dea000000006e052820` | `dc:4b:0a:06:03:f8` | `/dev/serial/by-id/usb-Arm_BBC_micro:bit_CMSIS-DAP_99063602000528205539bee7957c8dea000000006e052820-if01` | `ed06d27157ab986a500f17a7aaad573ae9678331864274b242815986b90d94ec` |

Both probes were detected as BBC micro:bit V2 / nRF52833 targets. Each board
was mass-erased, loaded, and reset sequentially through its exact UID.

## Results

### 60-second smoke

- Root queued 30 PINGs.
- Leaf received and queued 27 PONGs.
- Root matched 25 PONGs.
- Root observed 5 timeouts and 0 unmatched responses.
- Logs:
  - `2026-08-04-foundation-clean-60s-root.log`
  - `2026-08-04-foundation-clean-60s-leaf.log`

### Five-minute stability

- Root queued 150 PINGs.
- Leaf received and queued 118 PONGs.
- Root matched 98 PONGs.
- Root observed 52 timeouts and 0 unmatched responses.
- No semantic, PING-queue, or PONG-queue failures were reported.
- RTT counters remained 24–54 ms for matched responses.
- No reboot marker occurred in either capture.
- Logs:
  - `2026-08-04-foundation-clean-5m-root.log`
  - `2026-08-04-foundation-clean-5m-leaf.log`

Result: PASS for a clean-source, direct two-board, best-effort bidirectional
foundation. The 98/150 matched result is the baseline that link-v2 custody ACK
and bounded retry must improve; it is not a reliability acceptance result.

## Historical logs

The earlier `2026-08-04-ble-bidi-*` logs were captured from a dirty worktree
before commit `bf17bce` existed. They remain useful exploratory evidence but
have unresolved source provenance and are intentionally excluded from this
clean-candidate result.
