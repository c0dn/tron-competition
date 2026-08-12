# TAVRN-BLE-V2.3 bounded GTT gossip

## Goal
Replace benchmark-introduced periodic JOIN floods with HELLO-count-triggered bounded active GTT synchronization, resolve terminal discovery pending-data stalls, update the V2.3 specification, and prove the result in six-board HIL before a matched 1200-second comparison.

## Scope
- Extend FULL ordinary HELLO with bounded known remote-node count
- Trigger targeted active GTT synchronization only when a direct neighbor advertises more known remote nodes
- Reuse bounded SYNC_OFFER/PULL/DATA transfer without gating ordinary application traffic
- Remove periodic self-JOIN reannouncement; preserve one-shot bootstrap and real JOIN/LEAVE TC behavior
- Release destination-scoped pending DATA when route discovery reaches terminal exhaustion
- Update normative V2.3 profile, wire, identity, deviations, and test-matrix documentation
- Run software/resource review, short six-board HIL gate, then matched AODV/FULL 1200-second captures

## Non-goals
- General equal-cardinality GTT reconciliation
- Digest/Merkle consensus
- Changing RF PHY, advertising channels, or frozen radio/scheduler modules
- One-hour benchmark run
- Unbounded queues, heap allocation, or background flooding

## Constraints
- No heap and fixed capacities
- 24-byte maximum control PDU
- Homogeneous V2.3 fleet for the HELLO wire update
- Count excludes self and departed entries and saturates at GTT capacity
- One active anti-entropy session at a time with cooldown and no catch-up
- Active repair must not set public REJOINING/SYNCING state or gate application submissions
- Initial one-shot JOIN and real membership-change TC remain unchanged
- Frozen files microbit/app/drivers/ble_radio.{c,h} and microbit/app/protocol/ble_mesh_scheduler.{c,h} remain untouched
- Flash/settle time remains outside measured capture duration

## Relevant files
- microbit/app/protocol/tavrn_router.{c,h}
- microbit/app/protocol/tavrn_maintenance.{c,h}
- microbit/app/protocol/tavrn_mentorship.{c,h}
- microbit/app/protocol/tavrn_gtt.{c,h}
- microbit/app/protocol/tavrn_wire_v2.{c,h}
- microbit/app/protocol/aodv_core.{c,h}
- microbit/docs/tavrn_ble_profile.md
- microbit/docs/tavrn_ble_wire_v2.md
- microbit/docs/tavrn_ble_identity.md
- microbit/docs/tavrn_ble_deviations.md
- microbit/docs/tavrn_ble_test_matrix.md
- microbit/app/tavrn_routed_node/src/main.c
- microbit/app/ble_mesh_node/cmake/TronBleProfiles.cmake
- microbit/scripts/run_tavrn_benchmark.py
- microbit/scripts/summarize_tavrn_benchmark.py
- microbit/tests/protocol/test_tavrn_phase5_maintenance.c
- microbit/tests/protocol/test_tavrn_phase5_tc_metadata.c
- microbit/tests/protocol/test_tavrn_router.c
- microbit/tests/protocol/test_tavrn_aodv.c

## Spec files
- microbit/docs/tavrn_ble_profile.md
- microbit/docs/tavrn_ble_wire_v2.md
- microbit/docs/tavrn_ble_identity.md
- microbit/docs/tavrn_ble_deviations.md
- microbit/docs/tavrn_ble_architecture.md
- microbit/docs/tavrn_ble_test_matrix.md

## Execution phases
### 1. Specify bounded gossip and discovery cleanup <!-- workplan-phase-id: phase-lively-anchor-133762 -->
- Status: completed
- Id: phase-lively-anchor-133762
#### 1.1 Define HELLO count semantics and wire layout <!-- workplan-step-id: step-cedar-orbit-254365 -->
- Status: completed
- Id: step-cedar-orbit-254365
- Target: tavrn_wire_v2/router/maintenance/GTT/specs
- Action: Define a breaking homogeneous-V2.3 20-byte ordinary SID8 HELLO: existing 17-byte base, occupied nonself nondeparted known count at byte 17 (0..15; hard-expired retained members count), bytes 18..19 reserved zero. Targeted HELLO forms remain unchanged; legacy 17-byte ordinary FULL HELLO is rejected.
- Validation: Golden vectors; 0/15 bounds; >15/nonzero-reserved rejection; legacy/new mixed-version rejection; BUSY retains exact bytes
#### 1.2 Define active GTT repair FSM <!-- workplan-step-id: step-spruce-stream-007650 -->
- Status: completed
- Id: step-spruce-stream-007650
- Target: mentorship private active-repair FSM plus main RX handoff
- Action: After maintenance accepts a direct HELLO as RX_UNIQUE and post-admission local count is lower, first eligible neighbor starts one private active RFI transaction. Use reserved SYNC_PULL RFI flag on page 0 to make neighbor freeze a snapshot and answer contiguous existing SYNC_DATA pages. Keep public SID8_ACTIVE, active width, incarnation, and application ungated; never activate/recover/originate JOIN. Existing sync retry/page bounds apply; mentor_sync_dedupe_ms is cooldown after success/abort; later triggers while active/cooling are counted and ignored; rejoin/conflict/bootstrap/real TC obligations preempt.
- Validation: Trigger-after-unique-only, public-state invariant, page/retry/BUSY/wrap/cooldown, preemption, collision and no-catch-up tests
#### 1.3 Define terminal discovery release <!-- workplan-step-id: step-swift-path-178796 -->
- Status: completed
- Id: step-swift-path-178796
- Target: aodv_core terminal discovery
- Action: At terminal discovery exhaustion require one action slot, enqueue exactly one PENDING_DATA_FAILED carrying destination, then clear all destination-matching pending DATA and discovery. If action queue is full retain all state and retry next tick. Preserve independent per-item deadline expiry elsewhere and originate no TC/LEAVE.
- Validation: Full-action retry; multiple same-destination release; ordinary/scoped discovery; wrap/equality; fresh rediscovery; no TC/LEAVE

### 2. Implement protocol and documentation <!-- workplan-phase-id: phase-maple-bridge-583899 -->
- Status: completed
- Id: phase-maple-bridge-583899
#### 2.1 Implement HELLO count encode/ingest <!-- workplan-step-id: step-wild-reef-302969 -->
- Status: completed
- Id: step-wild-reef-302969
- Target: wire/router/maintenance/GTT
- Action: Build, validate, and consume count-bearing 20-byte ordinary FULL HELLO.
- Validation: Wire, router, maintenance, and profile suites
#### 2.2 Implement active bounded GTT sync <!-- workplan-step-id: step-cobalt-orbit-992324 -->
- Status: completed
- Id: step-cobalt-orbit-992324
- Target: mentorship
- Action: Remove periodic JOIN deadline/state and add one active repair transaction/cooldown using bounded sync transfer.
- Validation: Mentorship bootstrap plus active-repair regressions
#### 2.3 Implement discovery pending release <!-- workplan-step-id: step-nimble-pine-004904 -->
- Status: completed
- Id: step-nimble-pine-004904
- Target: aodv_core/router
- Action: Resolve buffered destination packets on terminal discovery failure.
- Validation: AODV/router regressions
#### 2.4 Update V2.3 specs <!-- workplan-step-id: step-amber-signal-350013 -->
- Status: completed
- Id: step-amber-signal-350013
- Target: microbit/docs
- Action: Normatively document count gossip, RFI-style active sync, one-shot JOIN semantics, limits, and discovery cleanup.
- Validation: Requirement/test traceability review

### 3. Verify and hardware-prove <!-- workplan-phase-id: phase-gentle-lantern-945520 -->
- Status: in_progress
- Id: phase-gentle-lantern-945520
#### 3.1 Run software/resource matrix <!-- workplan-step-id: step-young-comet-084158 -->
- Status: completed
- Id: step-young-comet-084158
- Target: microbit tests/build
- Action: Run focused suites, profiles, resource gates, and code-checker review.
- Validation: No blocker/major findings; stack/RAM gates pass
#### 3.2 Run short HIL gate <!-- workplan-step-id: step-steady-pine-124221 -->
- Status: completed
- Id: step-steady-pine-124221
- Target: six boards
- Action: From a clean reviewed checkpoint publish six UID-bound FULL candidates. Deterministic gate: hold F, allow A-E bootstrap; hold A, release/reset F so B learns F; resume A and prove a larger-count direct HELLO triggers exactly one bounded active repair, A remains SID8_ACTIVE/ungated, GTT converges, and stable windows spanning at least two former 30-second refresh intervals add no periodic TC JOIN traffic.
- Validation: No queue/fault/drop health counters; copied active-repair counters prove one trigger/success; GTT six-role recall/precision/agreement 1.0; TC stable-window delta excludes periodic refresh
#### 3.3 Run matched 1200-second comparison <!-- workplan-step-id: step-lively-field-230378 -->
- Status: in_progress
- Id: step-lively-field-230378
- Target: six boards
- Action: Run the new FULL V2.3 build for 1200 seconds immediately and compare it with the prior valid AODV 1200-second baseline using an automated fault-tolerant metrics report.
- Validation: FULL completed_by_duration with final charts; script reports application pipeline, workload/bursts, GTT convergence, control/byte proxies, retries, backpressure, health/validity, and ratios versus prior AODV

## Adversarial review findings
_None_

## Notes
- Count-only gossip intentionally detects missing members (remote_count > local_count); it does not claim equal-count table consensus.
- Existing SYNC wire formats already transfer one full-identity record per page and remain the bounded repair data path.
- Periodic JOIN was a benchmark-derived workaround and will be removed rather than normalized into V2.3.
- Plan review clarified that HELLO bytes 17..19 are a new V2.3 trailer, not pre-existing padding.
- Active repair uses an RFI-marked SYNC_PULL/private FSM rather than bootstrap SYNC_OFFER/public SYNCING.
- Homogeneous V2.3 deployment is required because ordinary FULL HELLO changes from exact 17 to exact 20 bytes.
- Final code-checker result: READY with no blocker/critical/major findings after RFI ownership, cancellation, collision, TC precedence, optional callback composition, and AODV ordering corrections.
- BALANCED repair-on benchmark fixed state 70212 bytes; mesh stack 2968/4096 and logger stack 816/1840; no heap.
- Short HIL mechanism proof passed: A was halted while F joined; after A resumed, A's GTT acquired F, observed SYNC_DATA increased from 5 to 11, and TC_UPDATE remained flat at 11 for more than two former 30-second refresh intervals.

## Status
- Overall status: in_progress
- Metadata file: .opencode/workplan/tavrn-v23-bounded-gossip.json
- Detailed plan file: .opencode/workplan/tavrn-v23-bounded-gossip.md
