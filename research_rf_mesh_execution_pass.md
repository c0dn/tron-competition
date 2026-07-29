# TRON RF mesh execution-level research pass

Source: OpenCode daemon-backed RF mesh continuation run (`TRON RF mesh execution-level research pass`)
Status: persisted by Hermes from execution-level research output
Worktree: `/home/lucas/Workspace/tron-worktrees/rf-mesh`

## Codebase reality summary

- Active lane worktree is `/home/lucas/Workspace/tron-worktrees/rf-mesh`.
- `microbit/` is a C/CMake firmware workspace targeting micro:bit v2 / nRF52833 on μT-Kernel 3.0.
- Current firmware targets:
  - `test_firmware`: serial hello / kernel version smoke target.
  - `ble_beacon`: raw nRF RADIO BLE advertising beacon, no SoftDevice.
  - `ble_observer`: raw nRF RADIO passive BLE advertising scanner.
- There is **no RF mesh implementation yet** and no proprietary micro:bit-radio driver yet.
- Current radio driver is `app/drivers/ble_radio.c/.h`. It directly programs the nRF52833 RADIO for BLE advertising channels 37/38/39. This is useful proof that direct radio register work is already accepted in this repo, but it is **not** micro:bit proprietary radio and not BLE mesh.
- `microbit/libs/mtkernel_3` is present but empty/uninitialized. `git submodule status` shows:
  - `-5606cf... microbit/libs/mtkernel_3`
  So the worktree is not currently build-verifiable until the submodule is initialized.
- `flash.sh` currently does `pyocd erase --mass`, `pyocd load`, `pyocd reset`. This conflicts with the research takeaway to avoid mass erase unless explicitly intended and must be handled under `FLASH_GATING_POLICY.md`.
- I did **not** flash hardware or run any hardware step.

## Feasible RF-mesh architecture options for this repo

1. **Recommended default: repository-native proprietary RF flood/relay**
   - Add a new direct-register `rf_radio.c/.h` driver for Nordic proprietary 2.4 GHz mode.
   - Add a new app target, e.g. `app/rf_mesh_node/`.
   - Implement app-layer flooding: node ID, sequence number, TTL, duplicate suppression, randomized relay delay, serial/debug counters.
   - This best matches the lane constraint: start from micro:bit proprietary radio semantics, not BLE mesh assumptions.

2. **MicroPython/MakeCode-compatible micro:bit radio protocol**
   - Try to match the existing micro:bit radio wire semantics: channel/group/address filtering, 1M/2Mbit, payload limits, no encryption, no built-in mesh.
   - Higher design risk because this repo does not use DAL/CODAL/MicroPython; exact packet layout and compatibility expectations must be pinned before implementation.
   - Feasible only after a deeper protocol-design pass.

3. **Raw BLE-advertising flood using existing `ble_radio`**
   - Lowest code effort because current TX/RX BLE advertising primitives exist.
   - But it is not the requested likely starting point and is not standard BLE mesh.
   - Useful only if the user explicitly wants a temporary diagnostic/prototype path.

4. **ESP32-C3 sidecar offload**
   - `esp-sidecar/` currently only has hello-world FreeRTOS bring-up.
   - Could later use ESP-NOW/Wi-Fi/BLE as a sidecar mesh, but that is not micro:bit proprietary radio and would need a microbit↔ESP serial/wiring protocol.
   - Treat as separate scope unless user says sidecar RF is desired.

## Constraints, missing protocol pieces, and likely design tradeoffs

- One radio means no simultaneous TX/RX. Mesh must time-slice: mostly listen, briefly transmit/relay, then return to RX.
- Current `ble_radio` style is blocking/polling. A mesh needs careful scheduling so relay bursts do not starve receiving or display/serial tasks.
- Missing protocol pieces:
  - packet header format
  - network/group ID
  - node identity
  - sequence number size and wrap rules
  - TTL/hop count
  - duplicate cache size/expiry
  - relay probability or deterministic relay rules
  - backoff/jitter timing
  - queue overflow behavior
  - payload type registry
  - counters/telemetry format
  - optional authentication/encryption decision
- Persisted and verified micro:bit radio docs confirm:
  - micro:bit radio is distinct from BLE.
  - no built-in mesh.
  - no built-in encryption.
  - group/channel/address-style filtering exists in stock runtimes.
  - MicroPython v2 exposes configurable length up to 251 bytes, queue size, channel `0..83`, power `0..7`, and 1M/2Mbit rates.
- Interop vs simplicity is the main tradeoff:
  - Exact micro:bit-radio compatibility increases research burden.
  - Project-owned proprietary packet format is faster and safer to implement in this C/uT-Kernel repo.
- Current build reality blocks verification until `microbit/libs/mtkernel_3` is initialized.
- Flashing must remain gated by Hermes. No later flash should happen until code-checker review + Hermes approval + target UID clarity.

## What must be decided by the user before implementation is safe to proceed

1. Should RF mesh be **compatible with MakeCode/MicroPython micro:bit radio**, or can it be a **project-owned proprietary packet format** over the nRF RADIO?
2. What is the first mesh payload?
   - heartbeat only
   - sensor telemetry
   - button/event messages
   - command/control
3. Expected test topology:
   - number of micro:bits
   - sender/relay/receiver roles
   - range/latency/loss success criteria
4. Radio parameters:
   - channel/frequency
   - group/network ID
   - 1M vs 2Mbit
   - TX power
   - fixed vs configurable payload size
5. Flood policy:
   - TTL value
   - relay once vs probabilistic relay
   - retry count
   - random backoff window
6. Security expectation:
   - accept stock-style no encryption
   - add lightweight message authentication
   - defer security entirely for competition prototype
7. Whether to modify `flash.sh` later to avoid default `erase --mass` before any hardware run.
8. Whether ESP sidecar is out of scope for this RF mesh lane.

## Proposed execution plan skeleton

1. **Deeper design pass with user comments**
   - Produce a short RF mesh protocol spec.
   - Resolve the decisions above before code.

2. **Repo readiness**
   - Initialize `microbit/libs/mtkernel_3`.
   - Build existing targets only; no flash.
   - Confirm baseline compile and toolchain.

3. **Implement non-hardware protocol core**
   - Packet structs/constants.
   - Duplicate suppression ring.
   - TTL/relay/backoff logic.
   - Counters and serial log format.

4. **Implement radio transport**
   - New `app/drivers/rf_radio.c/.h`.
   - Keep separate from `ble_radio` unless a small shared register helper is clearly worthwhile.

5. **Add first firmware target**
   - `app/rf_mesh_node/`.
   - Config cloned from nearest suitable existing app.
   - LED/serial observability.

6. **Verification before hardware**
   - Build target.
   - Code-checker review focused on radio correctness and hardware/flash safety.
   - Review `flash.sh` mass-erase behavior.

7. **Hardware-gated experiment only after approval**
   - Hermes confirms target/probe UID and no concurrent flashing.
   - Run narrow test plan.
   - Collect serial logs and tune timing/relay parameters.
