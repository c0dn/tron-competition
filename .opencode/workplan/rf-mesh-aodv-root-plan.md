# RF mesh AODV/root-aware design and implementation plan

## Goal

Design and later implement a pragmatic project-owned RF mesh path for micro:bit v2 / nRF52833 in the TRON RF mesh lane. The implementation should use proprietary 2.4 GHz radio semantics, not BLE mesh, and should support a sane provisional root-aware/AODV-style approach with application-layer discovery/flooding, bounded relays, duplicate suppression, TTL, backoff, and dummy payloads.

This is a workflow-planning/design artifact. It does not authorize hardware interaction.

## Ground truth

- Worktree: `/home/lucas/Workspace/tron-worktrees/rf-mesh`.
- Platform: micro:bit v2 / nRF52833.
- Current `microbit/` codebase: C/CMake firmware on μT-Kernel 3.0.
- Existing app targets: `test_firmware`, `ble_beacon`, `ble_observer`.
- Existing radio code: `microbit/app/drivers/ble_radio.c/.h`, direct-register BLE advertising/scanning primitives. This is only a register-programming reference, not the RF mesh implementation.
- No RF mesh implementation or proprietary RF radio driver exists yet.
- `microbit/libs/mtkernel_3` is currently an uninitialized/empty submodule in this checkout; build verification requires checking out the repo-pinned submodule commit.
- `microbit/flash.sh` currently uses `pyocd erase --mass`; this is a future safety issue and not part of this workflow.
- No concrete schema/spec/protocol file was visible locally after a narrow search. The user-approved conceptual schema and prior research are authoritative for now. Do not continue schema hunting unless the user reopens that question.

## Safety boundary

No hardware actions are in scope. Do not run `flash.sh`, pyOCD probe/list/reset/erase/load commands, serial monitor against hardware, or any command that interacts with a board. Do not claim RF runtime behavior. Any future hardware phase requires a separate approved plan, code-checker hardware-safety review, Hermes approval, probe UID targeting, no concurrent flashing, and explicit replacement/avoidance of the current mass-erase behavior unless the user and Hermes approve otherwise.

## Execution phases

### Phase 0 — Readiness and guardrails

1. Record the prior schema finding and use the approved conceptual schema; do not hunt for more schema files.
2. Initialize `microbit/libs/mtkernel_3` only to the repo-pinned commit. Do not advance the submodule pointer or vendor dependency contents.
3. Build existing targets only after submodule readiness: `test_firmware`, `ble_beacon`, `ble_observer`. No flash or hardware commands.
4. Preserve strict no-hardware policy and flag `flash.sh` mass erase as a future blocker to resolve before any hardware run.

### Phase 1 — Implementation-facing protocol and radio ABI design

Produce a concise design note, e.g. `microbit/docs/rf_mesh_protocol.md`, before implementation. It must freeze enough detail for independent code-writer slices:

- exact packet byte layout, field widths, endianness, max packet size, version/type/flags, network/root/source/destination ids, sequence/request ids, TTL/hop count, metrics, payload length, dummy payload bytes, and reserved checksum/auth behavior;
- nRF RADIO proprietary-mode mapping: channel/frequency, 1M/2M, TX power, address/network/group filtering, S0/LENGTH/S1 usage, CRC, whitening, max on-air payload, RSSI behavior;
- message types: `HELLO`/root beacon, `RREQ`, `RREP`, `DATA` with dummy payload, and optional/deferred `ACK`;
- root-aware/AODV-style rules: root role/configuration, root beacons, route freshness, route expiry, reverse routes, tie-breaking, discovery flood behavior, broadcast DATA behavior, duplicate/drop rules;
- single-radio scheduler/MAC rules: RX-first state machine, max blocking TX/RX operations, relay jitter/backoff, deterministic fallback if randomness is unavailable, bounded TX queue and drop policy, ACK/retry v1 decision, and guarantee that BLE and RF drivers are not active in the same firmware target;
- public interfaces: `rf_mesh_packet.h`, `rf_mesh_core.h` or equivalent, `rf_radio.h`, and configuration constants/CMake defines.

Protocol remains provisional and may evolve after later user comments. Payloads remain dummy/test-only.

### Phase 2 — Hardware-independent protocol core

Implement and test the core without radio hardware:

- packet constants and pack/unpack helpers with bounds checks;
- duplicate suppression ring keyed by source id, sequence/request id, and type;
- TTL decrement/drop behavior;
- fixed-size route table for root/destination, next hop, metric, sequence freshness, last seen, expiry, and tie-breaking;
- bounded TX/relay queue, jitter/backoff scheduling, deterministic fallback, priority/drop policy, and counters.

Validation should include host/non-hardware tests where feasible: valid and malformed packets, max/truncated lengths, duplicate detection, TTL drop, route update/expiry, queue overflow, jitter bounds, and dummy payload handling.

### Phase 3 — Proprietary RF radio driver

Add `microbit/app/drivers/rf_radio.c/.h`, separate from `ble_radio.c/.h` initially. Provide minimal API:

- `rf_radio_init`
- `rf_radio_listen`
- `rf_radio_poll`
- `rf_radio_send`
- optional RSSI accessor/output

Configure the nRF52833 RADIO in proprietary 2.4 GHz mode per the Phase 1 ABI. Keep driver primitives simple and bounded; no full-duplex assumption; return to RX promptly after TX. Do not alter BLE behavior unless a later reviewed shared-helper refactor is clearly justified.

### Phase 4 — RF mesh app target

Add `microbit/app/rf_mesh_node/` using existing app target patterns.

V1 app behavior:

- compile-time or CMake/config-based node/root/network/channel configuration, with optional FICR-derived node id fallback;
- root/non-root role selection;
- RX-first event loop: poll, decode/validate, update dedup/routes, enqueue relays/local dummy delivery, process bounded TX queue during jitter windows, return to listen;
- dummy heartbeat/DATA payload only;
- serial counters: tx, rx, drops, duplicates, relays, route updates, current root metric;
- LED diagnostics only.

### Phase 5 — Safe parallel slices after dependencies are met

Parallelization is allowed only after Phase 1 interfaces are frozen and Phase 0 baseline build succeeds.

- Slice A — protocol core: owns `microbit/app/rf_mesh_core/` and non-hardware tests. No radio registers, no app CMake integration.
- Slice B — RF radio driver: owns `microbit/app/drivers/rf_radio.c/.h`. No routing core or app logic.
- Slice C — app skeleton: owns `microbit/app/rf_mesh_node/` using frozen interfaces or stubs. No core or driver internals.
- Serial integration: one integrator owns CMake/source inclusion, stub removal, API mismatch fixes, and build-only verification.

Run code-checker after meaningful slices and final integration. Reviews must cover spec alignment, packet bounds, route/dedup correctness, timing/scheduler assumptions, radio register safety, hardware safety, hard-brick risk, wrong-target/wrong-probe risk, and flash-script assumptions.

### Phase 6 — Future hardware gate

This workflow stops before hardware. A future hardware plan may be proposed only after software validation and code-checker gates pass. That future plan must be separately approved by the user and Hermes, specify exact command and target/probe UID, avoid concurrent flashing, avoid `MAINTENANCE`, and address the current mass-erase behavior before any run.

## Open questions intentionally deferred

- Final application payload schema and semantics.
- Whether to add authentication/encryption after the dummy-payload prototype.
- Exact field sizes if user later provides a concrete schema/spec file.
- Hardware test topology and success thresholds.

## Handoff

Proceed to execution only after this plan is approved. First execution step is Phase 0 readiness: pinned submodule checkout and build-only baseline, with no hardware commands.
