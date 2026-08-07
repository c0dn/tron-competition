# TRON BLE mesh execution-level research pass

Source: OpenCode daemon-backed BLE mesh continuation run (`TRON BLE mesh execution-level research pass`)
Status: persisted by Hermes from execution-level research output
Worktree: `/home/lucas/Workspace/tron-worktrees/ble-mesh`

## Codebase reality summary

- Worktree: `/home/lucas/Workspace/tron-worktrees/ble-mesh`.
- `microbit/` is a custom C/uT-Kernel firmware workspace, not CODAL/MakeCode/Zephyr/NCS.
- The current BLE work already bypasses stock S113:
  - `app/drivers/ble_radio.c` directly programs the nRF52833 RADIO.
  - `ble_beacon` transmits legacy BLE `ADV_NONCONN_IND` packets on channels 37/38/39.
  - `ble_observer` passively scans by camping/hopping primary advertising channels.
- There is no Bluetooth Mesh stack yet:
  - no provisioning,
  - no mesh security,
  - no relay cache,
  - no TTL/sequence/network PDU layer,
  - no SAR,
  - no standard mesh models.
- The repo currently has separate beacon and observer apps, but not a combined “listen most of the time, scheduled TX when needed” detector node.
- Local build is currently blocked until the submodule is initialized: `microbit/libs/mtkernel_3` is empty and `git submodule status` shows it uninitialized.
- `flash.sh` uses `pyocd erase --mass`; that conflicts with the flash-gating posture for unattended experiments unless Hermes explicitly approves the target/command.

## Feasible BLE-mesh architecture options for this repo

1. **Best near-term path: custom BLE-advertising mesh-like protocol on current raw RADIO driver**
   - Keep uT-Kernel and raw nRF52833 RADIO.
   - Encode TRON packets inside legacy advertising data.
   - Add:
     - source ID,
     - sequence number,
     - TTL,
     - message type,
     - compact payload,
     - duplicate suppression,
     - relay/backoff policy,
     - controlled TX slots.
   - Detector behavior: passive scan by default, interrupt scan for bounded TX windows, then return to scan. This matches Criterion 3.

2. **Spec-shaped advertising bearer subset**
   - Mirror Bluetooth Mesh advertising-bearer concepts where useful:
     - Mesh Message AD type,
     - Mesh Beacon AD type,
     - PB-ADV concepts,
     - managed flood relay behavior.
   - But avoid claiming full Bluetooth Mesh compliance unless provisioning, security, replay protection, and model behavior are implemented.

3. **Alternate-stack lane with Zephyr/NCS**
   - Most credible route for actual standards-compliant Bluetooth Mesh.
   - Zephyr latest docs provide:
     - Bluetooth Mesh sample with PB-ADV/PB-GATT,
     - scan+advertise sample that interleaves Broadcaster and Observer roles,
     - Mesh API implementing Bluetooth Mesh Protocol 1.1.1.
   - This is a larger repo/build-system lane, not a minimal extension of the current uT-Kernel tree.

4. **Stock S113/CODAL lane**
   - Not a valid default for this BLE mesh lane.
   - Stock micro:bit v2 BLE/S113 should continue to be treated as insufficient unless proven otherwise.

## Constraints that block naive implementation

- One 2.4 GHz radio means no true simultaneous TX/RX. Any “detector + beacon” behavior must be scheduled/interleaved.
- Stock micro:bit v2 BLE uses S113; the persisted research and current docs support treating it as peripheral/broadcaster-oriented, not a safe mesh observer+broadcaster base.
- Existing raw driver is legacy advertising only:
  - 31-byte payload limit,
  - no connections,
  - no GATT,
  - no scan responses,
  - no full BLE controller/link-layer state machine.
- Current observer RX path is polling/simple; before using it as a mesh relay, it likely needs safer packet buffering around continuous RX/restart.
- Current apps prove beaconing and observing separately, not combined scheduling.
- Full Bluetooth Mesh is much more than “advertise packets”:
  - provisioning,
  - NetKey/AppKey handling,
  - AES-CCM/CMAC,
  - sequence/replay protection,
  - IV index,
  - relay cache,
  - TTL,
  - segmentation/reassembly,
  - model/config behavior.
- Build baseline is not currently runnable because the uT-Kernel submodule is absent.
- Flashing is gated; no flash step should happen without code-checker + Hermes review.

## External specs/references to follow

- **Bluetooth SIG Mesh FAQ / Mesh Protocol**
  - Mesh needs GAP Broadcaster + Observer behavior.
  - Mesh is optimized for small messages and managed flooding, not streaming.
  - Advertising bearer is the preferred mesh bearer where available.
- **Bluetooth Mesh Protocol 1.1.1**
  - Use as the standard reference if implementing spec-shaped PDUs.
- **Zephyr**
  - `samples/bluetooth/scan_adv`: reference for interleaved advertising + scanning.
  - `samples/bluetooth/mesh`: reference for PB-ADV/PB-GATT, provisioning, Generic OnOff models.
  - Zephyr Bluetooth Mesh API docs: reference for stack architecture and feature boundaries.
- **nRF Connect SDK**
  - Mesh `light`, `light_switch`, `sensor_client`, `sensor_server` samples as Nordic-flavored references for nRF52-class mesh apps.
- **Nordic nRF52833 Product Specification**
  - RADIO peripheral, BLE 1M mode, whitening, CRC, shorts/events, timing.
- **micro:bit hardware/Bluetooth docs**
  - Board limits, S113 presence, GPIO/power limits, distinction between BLE and micro:bit proprietary radio.

## Proposed execution plan skeleton

1. **Restore build baseline**
   - Initialize `microbit/libs/mtkernel_3`.
   - Build `test_firmware`, `ble_beacon`, and `ble_observer`.
   - Do not flash.

2. **Define protocol target**
   - Decide: custom TRON advertising protocol vs spec-shaped mesh subset.
   - For near-term competition feasibility, choose custom protocol with explicit “not full Bluetooth Mesh” wording.

3. **Add mesh packet layer**
   - Encode/decode compact advertising payloads.
   - Add node ID, sequence, TTL, message type, payload, checksum/MIC if feasible.
   - Add unit-testable pure C helpers where possible.

4. **Refactor radio scheduling**
   - Convert raw radio usage into explicit RX/TX scheduler:
     - passive scan windows,
     - bounded TX events,
     - channel dwell/hop policy,
     - RX packet buffering,
     - counters for received/dropped/relayed packets.

5. **Create combined detector/mesh node app**
   - New target, e.g. `ble_mesh_node`.
   - Passive detector listens by default.
   - Controlled TX for own beacons and relays.
   - Duplicate suppression + randomized relay backoff.

6. **Verification before hardware**
   - Build-only validation first.
   - Code-checker review focused on radio correctness and hard-brick/flash risk.
   - Hermes review before any flash command.
   - Only then controlled multi-board test; no `MAINTENANCE` mode, no ambiguous target flashing.
