# Layered TAVRN over BLE proving ground

## Goal
Build and hardware-qualify a near-full proof-of-concept TAVRN-BLE testbed on micro:bit v2 using one shared BLE foundation and one TAVRN router whose base feature level is naive AODV, while preserving the proven controlled-flood fallback and adding TAVRN capabilities incrementally with TDD, reversible build flags, explicit deviations, and reproducible hardware evidence.

## Architecture

The implementation has one shared radio foundation and only two behavioral
branches. Naive AODV is not a third plugin: it is the minimum build level of
the TAVRN router. Full TAVRN adds state and policy around the same AODV route
engine, route table, queues, and routed wire-v2.

```text
patient/application bridge
           |
           v
+---------------- TAVRN routed node ----------------+
| FULL_TAVRN additions (build-time)                 |
|   GTT, Smart TTL, fixed-k ESC, mentorship,        |
|   maintenance, TC-UPDATE, verification, repair    |
|                        |                          |
| AODV core (always present)                        |
|   RREQ/RREP/RERR, route table, pending DATA       |
|                        |                          |
| routed BLE link-v2                               |
|   next hop, custody HACK, retry, failure events   |
+------------------------+--------------------------+
                         |
                         v
shared BLE radio / RX snapshot / scheduler / TX queue
                         ^
                         |
+---------------- legacy controlled flood ----------+
| wire-v1, TTL, dedupe, jitter, bounded relaying    |
+---------------------------------------------------+
```

Build-time composition:

```text
TRON_NODE_MODE=LEGACY_FLOOD
  -> proven simple flooding fallback

TRON_NODE_MODE=TAVRN_ROUTED
  TAVRN_FEATURE_LEVEL=AODV_ONLY
  -> link-v2 + the TAVRN router's AODV base only

TRON_NODE_MODE=TAVRN_ROUTED
  TAVRN_FEATURE_LEVEL=FULL_TAVRN
  -> exactly the same link-v2 and AODV base plus TAVRN modules
```

Dependency rules:

1. Radio/scheduler code knows nothing about routing or TAVRN.
2. Routed link-v2 reports accepted frames, custody results, RSSI, and bounded
   failure events; it does not select routes.
3. The AODV core knows destinations, next hops, route freshness, and control
   messages, but has no dependency on GTT, ESC, or mentorship.
4. FULL_TAVRN observes and guides AODV through narrow hooks; it does not replace
   AODV forwarding or duplicate its route table.
5. The patient bridge is above routing and keeps patient identity distinct from
   bedside mesh-node identity.
6. Every later phase must continue building and testing the legacy fallback and
   AODV-only level. Rollback means selecting a lower build-time level, not
   reverting source code.

The only intentional wire split is legacy flood-v1 versus routed-v2. AODV_ONLY
and FULL_TAVRN share routed-v2. TAVRN-only control types and fields are ignored
or rejected predictably when the AODV-only level is built; they do not create a
second routed protocol implementation.

## Parallel execution topology

Layer acceptance remains serial: link-v2 must pass before AODV, mandatory
three-board AODV must pass before TAVRN augmentation, and each later TAVRN layer
must pass before the next is merged. Within a layer, use isolated fanout:

```text
immutable phase base
  +-> code-writer lane A -> lane checker --+
  +-> code-writer lane B -> lane checker --+-> integration writer
  +-> code-writer lane C -> lane checker --+        |
                                                   v
                                         whole-slice checker
                                                   |
                                                   v
                                      immutable candidate commit
```

Each writer receives a detached or dedicated `slice/tavrn-*` worktree at the
same immutable phase base. Writers never share a Git index or edit overlapping
files. The integration writer alone owns cross-module headers, top-level CMake,
firmware orchestration, and cherry-pick/conflict resolution. Lane commits are
not hardware candidates; only the accepted integration commit is flashed.

Safe fanout by phase:

| Phase | Parallel lanes | Serialized integration |
| --- | --- | --- |
| Foundation | normative spec/source audit; wire/byte-budget+ACK contract; identity/sequence contract; provenance/manifest contract | sole profile authority, architecture and test matrix |
| Link-v2 | frame codec+golden vectors; custody/retry virtual-time FSM; queue/flood policy; build-manifest tooling | scheduler/radio hooks and link test firmware |
| AODV base | control codec; route table+serial arithmetic; deterministic network simulator; role/build tooling | `tavrn_router` orchestration over link-v2 |
| GTT/Smart TTL | passive GTT; Smart-TTL policy; application query/metrics | AODV observation and discovery hooks |
| ESC/mentorship | fixed-k identity+codec; mentorship FSM; pagination/golden vectors | bootstrap orchestration around the same GTT/AODV core |
| Maintenance | HELLO/timer policy; metadata codec; TC-UPDATE/dedupe; bounded verification | feedback-loop/rate-limit orchestration |
| Local repair | repair red-test scenarios; bounded-buffer FSM; four-node simulator | failed-DATA ownership and existing AODV/GTT integration |
| Patient | AdvA-preserving RX event; schema/classifier; measurement/log parser | event bridge, priority and sink policy |
| Scale/handoff | profile builds; log analysis; conformance evidence indexing; runbook drafting | final claims, checker and history audit |

TDD fanout rules:

1. Test-author lanes may run in parallel when their requirement IDs and files do
   not overlap.
2. Each lane records assertion-level red evidence while all lower-level suites
   remain green.
3. Implementation starts only after the relevant red tests and interfaces are
   frozen.
4. Every lane gets a focused checker; integration gets a separate adversarial
   checker over the complete slice.
5. Later-phase research and test design may proceed read-only while hardware is
   running, but later-phase implementation may not merge before the mandatory
   current-layer hardware gate.

Hardware fanout rules:

- Compile independent role/profile images in parallel from the same clean
  candidate commit.
- Flash one exact pyOCD UID at a time; flashing is never parallel.
- Capture all node serial streams concurrently through stable `/dev/serial/by-id`
  links.
- Parse/count independent logs in parallel after capture, then perform one
  integrated topology/result assessment.
- Finite soak tests can overlap read-only documentation and next-phase test
  design, but not unproven implementation merges.

## Scope
- Freeze a versioned TAVRN-BLE subset profile from TAVRN_v2.md, actual ns-3 behavior, the paper, and BLE constraints
- Preserve the existing controlled-flood BLE node as the simple fallback while sharing safe radio/scheduler primitives
- Improve the routed BLE link with explicit hop identity, logical next-hop addressing, custody ACK, bounded retry, failure events, controlled discovery flooding, tunable timing profiles, and reproducible manifests
- Implement a single TAVRN router codebase that always contains the AODV route engine; first prove it with TAVRN_FEATURE_LEVEL=AODV_ONLY, then enable TAVRN extensions without replacing the route core
- Add passive GTT, Smart TTL, fixed-k=1 ESC, mentorship/bootstrap, adaptive maintenance, TC-UPDATE, bounded verification, conditional metadata where the 24-byte budget permits, and medium-correctness local repair
- Keep legacy flood and both TAVRN feature levels independently buildable/testable after every later slice
- Integrate patient-beacon observations without changing fall/shout algorithms and quantify routing impact on beacon RX
- Qualify direct, forced-relay, alternate transit-path recovery, multi-node, and finite overnight behavior with timestamped logs and exact source/artifact/board provenance

## Non-goals
- No production readiness, certification, clinical-readiness, or Bluetooth Mesh Protocol compliance claim
- No cryptography, provisioning security, replay protection, persistent sequence state, secure boot, or adversarial-route defense in this proof of concept
- No dynamic entropy k, mixed address widths, full ESC ambiguity recovery, IPv4/IPv6 port, or general MANET interoperability in this workplan
- No third independent routing plugin: AODV_ONLY and FULL_TAVRN are feature levels of the same TAVRN router
- No production power optimization, OTA update, NVM route persistence, polished CLI, dashboard rewrite, or deployment automation
- No changes to patient fall/shout detection algorithms
- No obligation to resolve checker nits, stylistic preferences, or production-only hardening findings that do not affect medium PoC correctness, hardware safety, layer boundaries, or acceptance

## Constraints
- Branch is exp/tavrn-ble, based on the hardware-proven but currently uncommitted test/ble-bidi-pingpong work
- Correctness target is medium: deterministic fixed capacity, bounds checking, serial-number/timer wrap policy, duplicate safety, loop prevention, route freshness, queue/failure handling, and reproducible tests; not production completeness
- Test-driven development is auditable: each slice has a test-author step that records an exact red command with at least one assertion-level failure for new requirement IDs while all lower-profile suites stay green, followed by a separate implementation step and independent checker/re-check
- Architecture has only two behavioral branches above shared BLE primitives: legacy controlled flooding and the TAVRN routed node. The TAVRN routed node always composes the same AODV engine; FULL_TAVRN adds modules and never replaces that engine
- Build-time TRON_NODE_MODE={LEGACY_FLOOD,TAVRN_ROUTED}; TAVRN_FEATURE_LEVEL={AODV_ONLY,FULL_TAVRN} applies only to TAVRN_ROUTED; TRON_TIMER_PROFILE={FAST_TEST,BALANCED,SOAK}; optional test hooks are manifest-recorded
- The legacy flood wire-v1 behavior and target remain buildable and golden-vector compatible. Shared radio/scheduler correctness fixes are allowed only with legacy regression proof; routed AODV/TAVRN use one shared wire-v2
- The Phase 0 in-workspace TAVRN-BLE profile becomes sole firmware authority. It must resolve spec/source/paper conflicts and register all deviations
- Fixed ESC k=1 is enabled only after AODV_ONLY hardware proof; canonical full identity is the six-byte AdvA/FICR BLE address, pre-ESC routed ID is a specified 16-bit suffix, fixed-k TAVRN ID is the specified low suffix byte, and collision handling fails closed; dynamic k is roadmap-only
- Legacy advertising remains ADV_NONCONN_IND with at most 24 custom manufacturer bytes. Every type has a byte-offset budget, compile-time size guard, golden vector, dedupe key, and malformed-frame policy before implementation
- Patient and TAVRN advertisements share RX windows on one nRF52833 radio; never claim simultaneous TX/RX
- Firmware uses fixed-size structures and no heap in radio/link/routing/GTT/mentorship paths
- Checker policy: resolve evidence-backed blocker/major findings affecting PoC correctness, hardware safety, acceptance, provenance, or architecture; explicitly defer/reject pedantic minor and production-polish findings
- Commit sequence: commit the validated workplan alone; during execution inventory dirty foundation files, commit foundation source/tests separately, then for every slice obtain green tests/builds and checker acceptance, create an immutable clean candidate commit, build/flash that commit, and commit evidence separately. Never amend an already hardware-tested candidate
- Hardware permission is blanket-approved by the user. Before every operation still verify normal micro:bit v2 mode, exact pyOCD UID, source commit/tree, submodule state, role/profile/timers/test hooks, ELF SHA-256, and serial link; flash one UID at a time and preserve logs
- Three-board forced-relay AODV hardware proof is a mandatory gate before any TAVRN feature implementation; four-board local-repair topology must place repair at a transit node

## Relevant files
- .opencode/workplan/ble-mesh-near-spec-transport.md
- microbit/app/protocol/tron_mesh_packet.h
- microbit/app/protocol/tron_mesh_packet.c
- microbit/app/protocol/tron_mesh_dedupe.h
- microbit/app/protocol/tron_mesh_dedupe.c
- microbit/app/protocol/ble_mesh_scheduler.h
- microbit/app/protocol/ble_mesh_scheduler.c
- microbit/app/protocol/tron_mesh_pingpong.h
- microbit/app/protocol/tron_mesh_pingpong.c
- microbit/app/ble_mesh_node/src/main.c
- microbit/app/ble_mesh_node/CMakeLists.txt
- microbit/app/drivers/ble_radio.h
- microbit/app/drivers/ble_radio.c
- microbit/build-ble-node.sh
- microbit/tests/protocol/
- microbit/hardware-results/
- /home/lucas/Projects/tron-competition/shared/schema.h
- /home/lucas/Projects/tron-competition/microbit/app/wearable_app/src/ble_emit.c
- /home/lucas/Projects/iot/TAVRN-WiFi/TAVRN_v2.md
- /home/lucas/Projects/iot/TAVRN-WiFi/IMPLEMENTATION.md
- /home/lucas/Projects/iot/TAVRN-WiFi/LOCAL-REPAIR-SPEC.md
- /home/lucas/Projects/iot/TAVRN-WiFi/REPORT-AUDIT.md
- /home/lucas/Projects/iot/TAVRN-WiFi/src/tavrn/model/tavrn-routing-protocol.cc
- /home/lucas/Projects/iot/TAVRN-WiFi/src/tavrn/model/tavrn-gtt.cc
- /home/lucas/Projects/iot/TAVRN-WiFi/src/tavrn/model/tavrn-rtable.cc
- /home/lucas/Projects/iot/TAVRN-WiFi/src/tavrn/model/tavrn-packet.cc
- microbit/hardware-results/2026-08-04-foundation-clean-bf17bce.md
- microbit/docs/tavrn_ble_profile.md
- microbit/docs/tavrn_ble_deviations.md
- microbit/docs/tavrn_ble_test_matrix.md
- microbit/docs/tavrn_ble_wire_v2.md
- microbit/docs/tavrn_ble_ack_contract.md
- microbit/docs/tavrn_ble_identity.md
- microbit/docs/tavrn_ble_architecture.md
- microbit/app/protocol/tavrn_wire_v2.c
- microbit/app/protocol/tavrn_link_v2.c
- microbit/app/protocol/ble_mesh_tx_queue.c
- microbit/tests/protocol/test_tavrn_link_v2.c
- microbit/app/ble_link_v2_testbed/
- microbit/app/ble_mesh_node/cmake/TronBleProfiles.cmake
- microbit/build-tavrn-ble.sh
- microbit/tests/protocol/run_tron_ble_build_profile_tests.sh
- microbit/hardware-results/2026-08-05-linkv2-da5c0c9-failed-hardware.md

## Spec files
- .opencode/workplan/ble-mesh-near-spec-transport.md
- microbit/app/protocol/README.md
- microbit/docs/tavrn_ble_profile.md
- microbit/docs/tavrn_ble_deviations.md
- microbit/docs/tavrn_ble_test_matrix.md
- microbit/docs/tavrn_ble_wire_v2.md
- microbit/docs/tavrn_ble_ack_contract.md
- microbit/docs/tavrn_ble_identity.md
- microbit/docs/tavrn_ble_architecture.md

## Execution phases
### 1. Freeze provenance, normative profile, wire contract, and layered architecture <!-- workplan-phase-id: phase-0-contract-provenance -->
- Status: completed
- Id: phase-0-contract-provenance
#### 1.1 Commit plan alone and inventory the dirty proven foundation <!-- workplan-step-id: step-0a-plan-and-foundation-inventory -->
- Status: completed
- Id: step-0a-plan-and-foundation-inventory
- Target: exp/tavrn-ble branch, workplan files, current BLE PING/PONG source/tests/logs
- Action: After planning approval, commit only the validated workplan. During execution record every dirty/untracked foundation file, mark prior hardware logs as dirty-source/provenance-limited, re-run existing host tests, then commit foundation source/tests separately without TAVRN changes.
- Validation: Plan commit contains only workplan artifacts; foundation commit is reviewable; historical claims are not overstated; lower baseline tests pass.
#### 1.2 Rebuild and re-prove the foundation from an immutable commit <!-- workplan-step-id: step-0b-clean-baseline-candidate -->
- Status: completed
- Id: step-0b-clean-baseline-candidate
- Target: clean temporary worktree at the foundation commit
- Action: Build labelled BLE flood/PING-PONG artifacts from a clean commit and record commit/tree/submodule/tool versions, effective constants, full AdvA, logical IDs, role/timer/test hooks, artifact SHA-256 and board UID. Re-run a two-board smoke and five-minute proof; commit evidence separately.
- Validation: Foundation is attributable to a clean commit and exact artifacts; branch returns clean after evidence commit.
#### 1.3 Freeze TAVRN-BLE profile and deviation authority <!-- workplan-step-id: step-0c-normative-profile -->
- Status: completed
- Id: step-0c-normative-profile
- Target: microbit/docs/tavrn_ble_profile.md, tavrn_ble_deviations.md, tavrn_ble_test_matrix.md
- Action: Code-writer audits TAVRN_v2.md, actual ns-3 source at fc5f256 plus observed dirty changes, IMPLEMENTATION.md, paper, REPORT-AUDIT.md and LOCAL-REPAIR-SPEC.md. Resolve full bootstrap identity versus one-byte implementation, verification demand/caps, E_RREP_ACK versus link custody ACK, sequence width/wrap/reboot, metadata capacity, and local repair as v2.3 extension. Assign stable requirement IDs.
- Validation: The in-workspace profile explicitly becomes sole firmware authority; every planned test maps to requirement IDs and every intentional deviation has rationale/status.
#### 1.4 Freeze wire-v2, custody ACK, and identity contracts <!-- workplan-step-id: step-0d-wire-ack-identity-contract -->
- Status: completed
- Id: step-0d-wire-ack-identity-contract
- Target: microbit/docs/tavrn_ble_wire_v2.md, tavrn_ble_ack_contract.md, tavrn_ble_identity.md
- Action: Freeze byte offsets/sizes for link DATA/HACK/flood, AODV RREQ/RREP/RERR/E_RREP_ACK, and TAVRN HELLO/SYNC/TC/metadata/patient DATA. Account for 31-byte AdvData and 24-byte custom PDU; define version/network/type isolation, endianness, dedupe, malformed policy, metadata/page capacities and golden vectors. Define HACK accepted/duplicate/busy/rejected, custody meaning, ACKable classes, retry/deadline/correlation, and RETRY_EXHAUSTED as the only link-break event. Define canonical AdvA identity, 16-bit pre-ESC ID, low-byte k=1 ID, reserved IDs, configured AdvA override, full bootstrap identity and fail-closed collision handling.
- Validation: Every frame fits with compile-time guards; contracts are sufficient to author assertion-level tests without implementation guesses.
#### 1.5 Freeze two-implementation architecture and build profiles <!-- workplan-step-id: step-0e-layer-build-contract -->
- Status: completed
- Id: step-0e-layer-build-contract
- Target: microbit/docs/tavrn_ble_architecture.md plus CMake/header contract
- Action: Define shared radio/scheduler/queue primitives; legacy flood branch; one TAVRN routed node containing AODV always and optional full modules. Define TRON_NODE_MODE, TAVRN_FEATURE_LEVEL, TRON_TIMER_PROFILE, test hooks, source ownership, callback/event boundaries, fixed capacities, effective manifest fields, and forbidden dependencies. No AODV/TAVRN stubs count as profile proof.
- Validation: Legacy flood builds independently; routed AODV_ONLY and FULL_TAVRN share route/link code; no full-TAVRN header enters AODV-only dependency; profile rollback is explicit.
#### 1.6 Checker gate for contracts and auditable TDD <!-- workplan-step-id: step-0f-contract-checker -->
- Status: completed
- Id: step-0f-contract-checker
- Target: all Phase 0 documents, test IDs, commit/provenance process
- Action: Code-checker reviews spec mapping, bytes, ACK/failure semantics, identity, sequence/reboot policy, layering, profiles, capacities, red/green evidence process, and hardware provenance. Code-writer fixes valid blocker/major findings; checker re-reviews fixes.
- Validation: No open blocker/major PoC issue; Phase 0 profile/architecture/wire/deviation/test docs are added to workplan specFiles before Phase 1.

### 2. Improve routed BLE link and prove custody/retry while preserving legacy flood <!-- workplan-phase-id: phase-1-link-v2 -->
- Status: completed
- Id: phase-1-link-v2
#### 2.1 Author and record failing link-v2 tests <!-- workplan-step-id: step-1a-link-red-tests -->
- Status: completed
- Id: step-1a-link-red-tests
- Target: host codec/link virtual-time tests and golden vectors
- Action: Test author writes assertion-level red tests for all link requirement IDs: exact frame sizes, transmitter/receiver/origin/destination, flood admission, HACK statuses, custody, duplicate re-HACK exactly once, busy/rejected behavior, retries/deadlines/wrap, failed-DATA ownership, controlled flooding, queue bounds and RX restoration. Run exact red command; lower legacy suites must stay green; save red evidence.
- Validation: Failure is due to missing behavior, not compile/link breakage; red output lists requirement IDs; legacy tests/build remain green.
#### 2.2 Implement shared routed link-v2 <!-- workplan-step-id: step-1b-link-implementation -->
- Status: completed
- Id: step-1b-link-implementation
- Target: pure link codec/state modules and minimal shared scheduler/radio extensions
- Action: Code-writer implements routed wire-v2, logical next-hop filtering, custody queue, HACK, bounded retry, RETRY_EXHAUSTED event carrying failed DATA context, controlled flood API, RSSI/ACK observations, timer profiles, counters and complete manifests. Legacy flood wire-v1 remains golden-compatible.
- Validation: All link tests pass with -Wall -Wextra -Werror; legacy flood host tests/build pass; a dedicated link-v2 test harness/firmware builds for hardware proof. It is not an AODV_ONLY profile or third routing implementation. AODV_ONLY becomes a valid continuously tested feature level only after Phase 2 implements the real AODV core.
#### 2.3 Check, commit, and build immutable link candidate <!-- workplan-step-id: step-1c-link-checker-candidate -->
- Status: completed
- Id: step-1c-link-checker-candidate
- Target: complete link slice
- Action: Code-checker reviews bounds, ACK/idempotency, queue/failure semantics, scheduler ownership, starvation, admission-before-dedupe and legacy compatibility. Code-writer fixes valid findings; checker re-reviews. Commit the green accepted slice, then build clean candidate artifacts.
- Validation: Candidate commit clean and immutable; manifest includes full provenance and artifact hashes; no blocker/major remains.
#### 2.4 Correct hardware-observed HACK turnaround and republish candidate <!-- workplan-step-id: step-1c2-link-turnaround-correction -->
- Status: completed
- Id: step-1c2-link-turnaround-correction
- Target: link-v2 HACK scheduling, testbed aggregate RF telemetry, contracts, and replacement immutable artifacts
- Action: Preserve failed da5c0c9 evidence, freeze the correction, author assertion-level RED tests, then source HACK turnaround directly from the existing radio TX-event bound, delay every centralized HACK producer, and add testbed-only aggregate valid-wire/channel plus scheduler counters. Keep the 71-key timer registry, 250/750/840 ms response bounds, legacy behavior, and >=95/100 gate unchanged. Run focused/full regressions and independent checker before committing and publishing a clean replacement candidate.
- Validation: RED evidence fails on immediate HACK eligibility; all HACK producers pass due-1/due/wrap and response-window tests after implementation; telemetry classification rejects invalid frames/channels; all legacy/link/build suites and both firmware builds pass; checker reports no blocker/major; replacement artifacts are clean, hashed, board-bound, and immutable.
#### 2.5 Correct hardware-observed testbed poll-gap yielding and republish candidate <!-- workplan-step-id: step-1c3-poll-gap-correction -->
- Status: completed
- Id: step-1c3-poll-gap-correction
- Target: microbit/app/ble_link_v2_testbed; microbit/tests/protocol; microbit/docs; clean candidate artifacts
- Action: Preserve the failed a8589fc smoke separately. Keep the immutable 2 ms operation-return-to-next-poll contract, but replace the harness's unconditional 1 ms healthy-cycle delay with a wrap-safe remaining-slack yield. Seed the same poll gate from the initial bounded RX-start return, preserve a bounded fault-cycle logger yield, clarify runtime evidence, and publish a new clean UID/AdvA-bound candidate without changing timers, HACK turnaround, retry/dwell/channel/power policy, or acceptance thresholds.
- Validation: A focused RED test must fail before the remaining-slack helper exists. Host tests cover elapsed 0/1/2+ ms, uint32 wrap, and first-poll startup overrun. Order-aware source checks prove startup gate seeding, post-work yield calculation, exact arguments, zero-delay skip, and fault-cycle fallback. Full protocol/legacy/profile/scheduler tests and both firmware builds pass; adversarial review reports no blocker/major. Commit immutably, publish/verify clean unhooked and hooked artifacts, then rerun the unhooked hardware gate.
#### 2.6 Two-board custody/retry hardware proof <!-- workplan-step-id: step-1d-link-hardware -->
- Status: completed
- Id: step-1d-link-hardware
- Target: two micro:bit v2 boards, routed link-v2 test mode
- Action: Flash the candidate by UID. Run at least 100 transactions, deterministic first-HACK suppression, duplicate acceptance, BUSY queue hook, 60-second smoke, and five-minute stability. Commit a concise result summary; keep raw experiment logs local.
- Validation: At least 95/100 complete; suppressed HACK causes retry and duplicate HACK with one custody/application acceptance; BUSY defers and later completes without BUSY expiry, rejection, or link-break/fault terminal; no reset/stall/corruption. Neighbor state is not implemented by this non-routing harness.

### 3. Implement the TAVRN router at naive-AODV feature level <!-- workplan-phase-id: phase-2-aodv-base -->
- Status: draft
- Id: phase-2-aodv-base
#### 3.1 Author and record failing naive-AODV tests <!-- workplan-step-id: step-2a-aodv-red-tests -->
- Status: draft
- Id: step-2a-aodv-red-tests
- Target: pure route table/control codec/deterministic multi-node simulation
- Action: Write assertion-level red tests for RREQ/RREP/RERR/E_RREP_ACK vectors, request dedupe, reverse/forward routes, destination/request sequence serial arithmetic and half-range policy, reboot/rejoin policy, freshness/ties, expanding ring, expiry, loop prevention, pending DATA, precursors, HACK-triggered failure, two-node direct and three-node chain. Keep link and legacy suites green; save red evidence.
- Validation: Tests fail on missing AODV behavior only and map to normative requirement IDs.
#### 3.2 Implement the common TAVRN router with AODV_ONLY enabled <!-- workplan-step-id: step-2b-aodv-implementation -->
- Status: draft
- Id: step-2b-aodv-implementation
- Target: tavrn_router orchestrator and aodv_core modules
- Action: Code-writer implements fixed-capacity AODV route/control state over link-v2 callbacks. TAVRN router always owns this engine; FULL_TAVRN modules are absent/disabled, not alternative routing plugins. DATA has no broadcast fallback when a valid route is required.
- Validation: AODV simulations pass; legacy and link suites remain green; AODV_ONLY candidate builds without GTT/ESC/mentorship dependencies.
#### 3.3 Check and commit immutable AODV candidate <!-- workplan-step-id: step-2c-aodv-checker-candidate -->
- Status: draft
- Id: step-2c-aodv-checker-candidate
- Target: AODV_ONLY feature level and all lower profiles
- Action: Code-checker reviews sequence/loop safety, lifetimes, next-hop/final destination, discovery bounds, RERR/RREP_ACK, callback ownership and feature-level layering. Fix/re-review valid findings, commit accepted green slice, build clean role artifacts.
- Validation: No blocker/major medium-correctness issue; provenance complete; no TAVRN extension leakage.
#### 3.4 Mandatory two- and three-board AODV proof <!-- workplan-step-id: step-2d-aodv-hardware-mandatory -->
- Status: draft
- Id: step-2d-aodv-hardware-mandatory
- Target: two direct boards then A-B-C forced chain
- Action: Run 20 cold discoveries and at least 100 transactions direct, expiry/rediscovery and ACK loss. With three boards, force A->B->C using test hooks and capture all serial streams. Do not start Phase 3 until this gate passes.
- Validation: Direct delivery >=95/100; chain delivery >=90/100; one RREQ processing per request ID/node; reciprocal routes and exactly-once final delivery; rejoin recovers within manifest discovery bound.

### 4. Add passive GTT, application visibility, and Smart TTL without changing AODV <!-- workplan-phase-id: phase-3-gtt-smartttl -->
- Status: draft
- Id: phase-3-gtt-smartttl
#### 4.1 Author and record failing passive-GTT tests <!-- workplan-step-id: step-3a-gtt-red-tests -->
- Status: draft
- Id: step-3a-gtt-red-tests
- Target: pure GTT and AODV integration tests
- Action: Write red tests for separate membership/route state, serial freshness/wrap/reboot, passive evidence from all valid frame roles, hop estimates, fixed capacity, departed state, application enumeration, Smart-TTL initial scope, and mandatory full-flood fallback. Exclude HELLO, metadata, expiry verification and mentorship from this phase.
- Validation: Tests fail for absent TAVRN augmentation while AODV_ONLY and lower suites stay green.
#### 4.2 Implement passive GTT and Smart TTL as AODV augmentation <!-- workplan-step-id: step-3b-gtt-implementation -->
- Status: draft
- Id: step-3b-gtt-implementation
- Target: tavrn_gtt and full-feature orchestration hooks
- Action: Code-writer adds passive fixed-size GTT and application query API around the existing AODV engine. FULL_TAVRN consults fresh hop estimates for discovery scope and falls back; AODV route table and forwarding logic are unchanged.
- Validation: GTT/Smart-TTL tests pass; AODV_ONLY outputs/tests remain unchanged; no active GTT transmissions exist yet.
#### 4.3 Check and commit GTT/Smart-TTL candidate <!-- workplan-step-id: step-3c-gtt-checker-candidate -->
- Status: draft
- Id: step-3c-gtt-checker-candidate
- Target: passive GTT slice and all profile regressions
- Action: Checker reviews freshness, capacity, passive ownership, fallback, route/GTT separation and feature-level isolation. Fix/re-review, commit accepted candidate and build clean artifacts.
- Validation: No blocker/major; provenance complete; lower modes green.
#### 4.4 Prove passive membership and scoped discovery <!-- workplan-step-id: step-3d-gtt-hardware -->
- Status: draft
- Id: step-3d-gtt-hardware
- Target: three/four boards
- Action: Run scripted active sets and traffic. Compare AODV_ONLY and FULL_TAVRN GTT/Smart-TTL artifacts on identical topology; query each node and compare RREQ transmissions for known versus unknown destinations.
- Validation: Observed active set equals scripted set at every node after convergence, precision=recall=1.0; Smart TTL reduces initial scope/transmissions and fallback completes within manifest formula.

### 5. Add fixed-k=1 ESC and basic mentorship around the same AODV/GTT core <!-- workplan-phase-id: phase-4-esc-mentorship -->
- Status: draft
- Id: phase-4-esc-mentorship
#### 5.1 Author and record failing ESC/mentorship tests <!-- workplan-step-id: step-4a-esc-mentor-red-tests -->
- Status: draft
- Id: step-4a-esc-mentor-red-tests
- Target: TAVRN codec/identity and bootstrap FSM tests
- Action: Write red tests for full AdvA identity, 16-bit pre-ESC ID, low-byte k=1 ID, reserved/collision fail-closed behavior, fixed AM vectors, full bootstrap identity, deterministic offer tie-break, RSSI+jitter dampening, frozen sorted GTT snapshot, largest-GTT mentor, bounded pagination, retry/death/self-bootstrap and merge freshness.
- Validation: Tests fail before implementation; AODV_ONLY retains pre-ESC wire-v2 and stays green.
#### 5.2 Implement fixed-k ESC and mentorship <!-- workplan-step-id: step-4b-esc-mentor-implementation -->
- Status: draft
- Id: step-4b-esc-mentor-implementation
- Target: FULL_TAVRN-only codec identity and mentorship modules
- Action: Code-writer adds fixed k=1 type-specific encoding, explicit collision diagnostics, HELLO isNew, offer/collection/selection, SYNC_PULL/SYNC_DATA snapshot pagination, retry and self-bootstrap. Dynamic k stays deferred. Page capacities follow Phase 0 wire contract.
- Validation: Golden vectors and FSM tests pass; no custom PDU exceeds 24 bytes; AODV_ONLY common route engine and lower modes remain unchanged.
#### 5.3 Check and commit ESC/mentorship candidate <!-- workplan-step-id: step-4c-esc-mentor-checker-candidate -->
- Status: draft
- Id: step-4c-esc-mentor-checker-candidate
- Target: FULL_TAVRN ESC/bootstrap slice
- Action: Checker reviews context assumptions, identity collision, bootstrap deadlock, tie-break, pagination mutation, bounds and feature isolation. Fix/re-review, commit green candidate, build clean artifacts.
- Validation: No blocker/major; dynamic-k deviations current; lower modes green.
#### 5.4 Prove bounded mentorship bootstrap <!-- workplan-step-id: step-4d-mentorship-hardware -->
- Status: draft
- Id: step-4d-mentorship-hardware
- Target: three to eight boards
- Action: Bootstrap empty-GTT node into established mesh, collect competing offers, select mentor, pull frozen pages, remove mentor mid-sync, test self-bootstrap and duplicate suffix hook. Compute numerical bound from offer window + pages*(timeout*attempts) and record it in manifest.
- Validation: Membership converges to expected active set within computed bound; no oversize/reset/duplicate/deadlock; collision fails closed.

### 6. Add TAVRN maintenance, metadata, HELLO, TC-UPDATE, and bounded verification <!-- workplan-phase-id: phase-5-maintenance -->
- Status: draft
- Id: phase-5-maintenance
#### 6.1 Author and record failing maintenance tests <!-- workplan-step-id: step-5a-maintenance-red-tests -->
- Status: draft
- Id: step-5a-maintenance-red-tests
- Target: FULL_TAVRN maintenance host scenarios
- Action: Write red tests for adaptive/low-cadence HELLO, suppression, soft/hard expiry, demand-gated verification and chosen cap, route-assisted freshness HELLO, bounded RREQ fallback, TC-UPDATE UUID/subject dedupe, conditional metadata capacities per frame, attribution to explicit transmitter and no control feedback loop.
- Validation: Tests fail before maintenance code; AODV_ONLY and passive GTT suites stay green.
#### 6.2 Implement near-full maintenance around existing AODV/GTT <!-- workplan-step-id: step-5b-maintenance-implementation -->
- Status: draft
- Id: step-5b-maintenance-implementation
- Target: FULL_TAVRN maintenance orchestration
- Action: Code-writer implements timers from build profiles, HELLO/metadata/expiry/verification/TC-UPDATE according to normative profile. Compact metadata is included only within declared per-type slack; unsupported DATA piggybacking is an explicit deviation, never truncation.
- Validation: Tests pass; stable topology control floor matches configured HELLO policy; frame guards pass; lower modes unaffected.
#### 6.3 Check and commit maintenance candidate <!-- workplan-step-id: step-5c-maintenance-checker-candidate -->
- Status: draft
- Id: step-5c-maintenance-checker-candidate
- Target: maintenance slice and regressions
- Action: Checker reviews timer feedback, storm caps, false departure, attribution, rate limits, sequence freshness and deviations. Fix/re-review, commit accepted candidate and build clean artifacts.
- Validation: No blocker/major medium-PoC issue; manifests show effective maintenance values.
#### 6.4 Prove maintenance and bounded verification <!-- workplan-step-id: step-5d-maintenance-hardware -->
- Status: draft
- Id: step-5d-maintenance-hardware
- Target: four to eight boards
- Action: Run stable/idle/change scripts, node join/leave, soft/hard expiry, route-assisted verification and fallback. Compare control TX with AODV_ONLY and validate expected membership sets over time.
- Validation: Expected active set equality after each bounded convergence period; no storm/reset; verification count respects cap; control traffic returns to configured floor.

### 7. Add medium-correctness TAVRN local repair as a separable extension <!-- workplan-phase-id: phase-6-local-repair -->
- Status: draft
- Id: phase-6-local-repair
#### 7.1 Author and record failing local-repair tests <!-- workplan-step-id: step-6a-repair-red-tests -->
- Status: draft
- Id: step-6a-repair-red-tests
- Target: repair FSM and deterministic four-node simulation
- Action: Write red tests for RETRY_EXHAUSTED only trigger, failed transit DATA custody transfer, B as repairer, fixed repair buffer/concurrency, Smart-TTL RREQ, alternate route success/flush exactly once, timeout/RERR fallback, destination versus failed-neighbor state, rate limit and repair-disabled behavior.
- Validation: Tests fail before repair; FULL_TAVRN without repair and all lower modes stay green.
#### 7.2 Implement local repair without replacing AODV <!-- workplan-step-id: step-6b-repair-implementation -->
- Status: draft
- Id: step-6b-repair-implementation
- Target: optional FULL_TAVRN repair module around same router
- Action: Code-writer implements LOCAL-REPAIR-SPEC-inspired FSM using link-v2 failed-DATA event, existing AODV RREQ/RERR/table and GTT hints. Build flag can disable repair while retaining full TAVRN.
- Validation: No steady-state repair traffic; tests pass; no duplicate delivery or unbounded buffer; lower feature levels unchanged.
#### 7.3 Check and commit local-repair candidate <!-- workplan-step-id: step-6c-repair-checker-candidate -->
- Status: draft
- Id: step-6c-repair-checker-candidate
- Target: repair slice and profile matrix
- Action: Checker reviews ownership, triggers, buffering, route invalidation, false departure, timeout/rate-limit and disabled path. Fix/re-review, commit candidate, build repair on/off artifacts.
- Validation: No blocker/major; both artifacts attributable and lower profiles green.
#### 7.4 Prove transit local repair with four boards <!-- workplan-step-id: step-6d-four-board-repair -->
- Status: draft
- Id: step-6d-four-board-repair
- Target: A(source)-B(repairer)-C(destination), B-D-C alternate
- Action: Install primary A-B-C and begin A->C traffic so DATA is transit at B. Suppress B->C HACKs; verify B sees RETRY_EXHAUSTED, buffers, repairs through D and flushes. Compare repair-disabled artifact, then restore B-C.
- Validation: Repair-on recovers through D within configured bound with one final delivery; repair-off sends RERR and A discovers; all four logs prove B executed transit repair; restoration reconverges.

### 8. Integrate patient observations and measure shared-radio coexistence <!-- workplan-phase-id: phase-7-patient -->
- Status: draft
- Id: phase-7-patient
#### 8.1 Vendor patient schema contract and author failing coexistence tests <!-- workplan-step-id: step-7a-patient-contract-red-tests -->
- Status: draft
- Id: step-7a-patient-contract-red-tests
- Target: in-worktree hash-identified schema-v1, scheduler AdvA event, classifier/bridge tests
- Action: Preserve AdvA[6] in RX event and test it. Vendor/copy authoritative seven-byte schema contract with source hash. Define exact classifier, patient device identity byte, sink, urgent event classes, heartbeat local/sample/route policy, 8-bit sequence wrap dedupe and unchanged payload. Write assertion-level red tests.
- Validation: Red failures are behavioral; all mesh suites green; MIND classification requires exact MSD length/schema and TAVRN exact magic/version.
#### 8.2 Implement observation-to-TAVRN bridge <!-- workplan-step-id: step-7b-patient-implementation -->
- Status: draft
- Id: step-7b-patient-implementation
- Target: application bridge above selected node mode
- Action: Code-writer classifies patient/TAVRN frames, keeps patient and mesh-node identities separate, dedupes {AdvA,seq,event}, preserves seven bytes, prioritizes incidents, applies heartbeat policy, routes to explicit sink and exposes beacon RX/mesh TX counters. Do not alter detection algorithms.
- Validation: Golden vectors pass; retries yield exactly-once application event; no routing/GTT pollution from patient frames.
#### 8.3 Check and commit patient coexistence candidate <!-- workplan-step-id: step-7c-patient-checker-candidate -->
- Status: draft
- Id: step-7c-patient-checker-candidate
- Target: AdvA/scheduler/classifier/bridge and all modes
- Action: Checker reviews identity, ambiguity, wrap dedupe, starvation, queue priority, single-radio claims and lower-mode regression. Fix/re-review, commit accepted candidate and build clean artifacts.
- Validation: No blocker/major PoC issue; provenance complete.
#### 8.4 Measure patient RX and event delivery by mode <!-- workplan-step-id: step-7d-patient-hardware -->
- Status: draft
- Id: step-7d-patient-hardware
- Target: wearable emitter plus bedside/root nodes
- Action: Run paired same-position baseline and LEGACY_FLOOD, AODV_ONLY, FULL_TAVRN captures with at least 200 emitted heartbeat/status advertisements and 20 each fall, shout/distress and combined events. Observation denominator is unique {AdvA,seq} received divided by emitter-recorded transmissions.
- Validation: No silent starvation; target <=5 percentage-point observation loss versus listener baseline; all urgent events arrive in controlled run or misses are explicitly reported with retries/latency.

### 9. Run scale/soak, publish conformance, then final-check and hand off <!-- workplan-phase-id: phase-8-scale-handoff -->
- Status: draft
- Id: phase-8-scale-handoff
#### 9.1 Run complete node-mode/feature/timer regression matrix <!-- workplan-step-id: step-8a-build-matrix -->
- Status: draft
- Id: step-8a-build-matrix
- Target: legacy flood and TAVRN routed AODV_ONLY/FULL_TAVRN x timer profiles
- Action: Build clean temporary candidates, run all host suites, verify manifests, wire golden vectors, linker maps/module inclusion and stripped-section hashes where useful. Do not require debug ELF hash equality across paths.
- Validation: All required matrix cells pass; legacy vectors stay compatible; AODV_ONLY excludes full modules; artifacts attributable.
#### 9.2 Run up-to-eight-node controlled test and finite overnight soak <!-- workplan-step-id: step-8b-eight-node-soak -->
- Status: draft
- Id: step-8b-eight-node-soak
- Target: available micro:bit fleet
- Action: Map UIDs/full AdvA/suffix IDs, validate uniqueness, flash sequentially, run mentorship/membership/routes/patient events and planned failure/rejoin. Run finite approximately eight-hour SOAK profile and preserve all logs/counters/topology/artifact hashes.
- Validation: Expected active set equality with precision=recall=1.0 after bounds; no reset/stall/unbounded growth/storm; routes/GTT recover within documented PoC limits; event and RX statistics reported.
#### 9.3 Publish conformance/deviation report <!-- workplan-step-id: step-8c-conformance-report -->
- Status: draft
- Id: step-8c-conformance-report
- Target: microbit/docs/tavrn_ble_conformance.md
- Action: Mark every requirement pass/partial/deviated/deferred/not-applicable with test/log/commit references; distinguish TAVRN-BLE subset from original TAVRN, local-repair extension, Bluetooth Mesh compliance and production readiness.
- Validation: No unsupported claim and no hidden deviation.
#### 9.4 Finalize architecture/build/flash/test runbook <!-- workplan-step-id: step-8d-final-runbook -->
- Status: draft
- Id: step-8d-final-runbook
- Target: architecture docs and hardware runbook
- Action: Document shared layering, two behavioral branches, AODV/full feature levels, flags, timers, packets, IDs, commands, rollback, expected logs, known issues and clean-checkout reproduction.
- Validation: New agent can build and repeat each gate without chat history.
#### 9.5 Final adversarial checker after claims and docs exist <!-- workplan-step-id: step-8e-final-checker -->
- Status: draft
- Id: step-8e-final-checker
- Target: entire branch, history, workplan, docs and evidence
- Action: Checker reviews architecture boundaries, implementation, tests, hardware claims, conformance/deviations, runbook and commit history. Fix/re-review valid blocker/major findings; reject/defer production/pedantic findings with rationale.
- Validation: No blocker/major within medium PoC scope; claims trace to immutable candidates/logs.
#### 9.6 Commit final evidence/docs and leave clean handoff <!-- workplan-step-id: step-8f-final-audit -->
- Status: draft
- Id: step-8f-final-audit
- Target: exp/tavrn-ble branch and workplan state
- Action: Commit evidence/docs separately, verify granular history, generated artifact policy, no unrelated files/secrets, complete matrix at HEAD, update/validate workplan completion and provide deferred dynamic-k/production roadmap.
- Validation: Branch clean, workplan valid/completed, all modes build, handoff exact.

## Adversarial review findings
- [note] Proof-of-concept scope governs checker triage(open)— Resolve checker blocker/major findings tied to medium correctness, hardware safety, architecture, provenance, or acceptance. Explicitly defer/reject production security, certification, style, and polish requests outside scope. [source: User instruction 2026-08-04]
- [note] Blanket hardware authorization recorded(open)— Repeated UID-targeted mass erase/load/reset and finite serial capture are approved. Exact target/artifact/UID/provenance checks remain mandatory. [source: User instruction 2026-08-04]
- [blocker] Profile model conflated legacy flood and link-v2(resolved)— Resolved in revision: retain legacy flood branch and one TAVRN routed implementation; AODV_ONLY/FULL_TAVRN are feature levels of the same router and share wire-v2. [source: plan-checker ses_0327ea5b1ffeaqt39pUUewn0sY B1]
- [blocker] Hardware candidate provenance and commit order were circular(resolved)— Resolved in revision: clean immutable candidate commit precedes hardware; evidence is committed afterward; historical dirty-source logs are marked provenance-limited and baseline is re-proven. [source: plan-checker ses_0327ea5b1ffeaqt39pUUewn0sY B2]
- [blocker] Four-board topology did not exercise transit local repair(resolved)— Resolved in revision: primary A-B-C with B as transit repairer and B-D-C alternate; suppress B-to-C custody ACKs. [source: plan-checker ses_0327ea5b1ffeaqt39pUUewn0sY B3]
- [major] Wire, ACK, and identity contracts must freeze before implementation(resolved)— Phase 0 now requires byte-level wire, custody ACK, full identity/suffix, frame capacity, dedupe, failure-event, and golden-vector contracts before link code. [source: plan-checker ses_0327ea5b1ffeaqt39pUUewn0sY M1-M2]
- [major] TAVRN mechanisms were ordered before their dependencies(resolved)— Revision limits early GTT phase to passive state/Smart TTL, then fixed ESC/mentorship, then HELLO/metadata/expiry/verification, and finally local repair. [source: plan-checker ses_0327ea5b1ffeaqt39pUUewn0sY M3]
- [major] Serial-number wrap/reboot policy required(resolved)— Added to normative contract and AODV/GTT test requirements. [source: plan-checker ses_0327ea5b1ffeaqt39pUUewn0sY M4]
- [major] Patient AdvA identity was discarded by scheduler event(resolved)— Patient phase now first preserves/tests AdvA, vendors a hash-identified schema contract, defines exact classification/dedupe/sink/heartbeat policy, and uses emitted denominators. [source: plan-checker ses_0327ea5b1ffeaqt39pUUewn0sY M5]
- [major] Final claims were written after final checker(resolved)— Final checker moved after conformance/runbook generation and before final audit. [source: plan-checker ses_0327ea5b1ffeaqt39pUUewn0sY M8]
- [major] Phase 1 link harness was mislabeled AODV_ONLY(resolved)— Resolved in JSON and Markdown: Phase 1 uses a dedicated non-routing link-v2 harness; AODV_ONLY is introduced only with the real AODV engine in Phase 2. [source: plan-checker final gate ses_0327ea5b1ffeaqt39pUUewn0sY]
- [blocker] TTL width and expanding-ring dedupe conflict(resolved)— Resolved: the PoC net diameter is 15, all traversal-derived timers were recomputed, and every expanding-ring transmission receives a fresh RREQ ID correlated only in local discovery state. [source: code-checker ses_03223af62ffexcEZVBNT8ZC0C6 findings 1-2]
- [blocker] Custody ACK occurs before router/application reservation(resolved)— Resolved: link-v2 uses synchronous two-phase candidate admission; ACCEPTED commits dedupe and HACK only after route/application storage reservation, with bounded BUSY cleanup and continued timer/control progress. [source: code-checker ses_03223af62ffexcEZVBNT8ZC0C6 finding 3]
- [major] Pre-ESC identity and reboot/dedupe semantics conflict(resolved)— Resolved: SID16 is the AdvA-derived standalone AODV_ONLY logical namespace, direct peers retain full AdvA, routed-common boot nonce handles incarnation, and equality dedupe is distinct from half-range freshness. [source: code-checker ses_03223af62ffexcEZVBNT8ZC0C6 findings 4-6]
- [major] Scheduler/link seams incomplete for AdvA and terminal outcomes(resolved)— Resolved: full AdvA/raw/RSSI seams, complete Phase-1 codec/link APIs, bounded radio and scheduler service, partial-channel accounting, and six typed ownership outcomes are frozen and checker-accepted. [source: code-checker ses_03223af62ffexcEZVBNT8ZC0C6 findings 7-9 and final rechecks]
- [major] Timer, repair, and bounded-state registry incomplete(resolved)— Resolved: profile and architecture share the exact 71-key registry; repair uses one Smart-TTL plus one full attempt through deferred RERR; capacities, segmentation, and finite scheduler/radio bounds are explicit. [source: code-checker ses_03223af62ffexcEZVBNT8ZC0C6 findings 10-11,13 and rechecks]
- [major] Golden-vector and E_RREP_ACK correlation coverage incomplete(resolved)— Resolved: maximum-capacity and rejection vectors pass byte arithmetic, and E_RREP_ACK exact correlation includes destination sequence. [source: code-checker ses_03223af62ffexcEZVBNT8ZC0C6 findings 12,14]
- [major] Phase 0 specFiles and Markdown execution state stale(resolved)— Resolved: all seven Phase 0 contracts are registered, JSON/Markdown execution state is synchronized, and workplan validation passes. [source: code-checker ses_03223af62ffexcEZVBNT8ZC0C6 finding 15]
- [major] Phase 1 generated HACK receiver is reversed(resolved)— Resolved: generated HACK immediate_receiver is the prior DATA transmitter, and two independent link instances verify ACCEPTED, DUPLICATE, BUSY, and REJECTED correlation end to end. [source: code-checker re-review ses_031304a81ffeDVVDIzYydLu1ap]
- [major] Legacy shared scheduler bypasses typed radio seams(resolved)— Resolved: legacy mesh scheduling now uses only bounded typed radio calls, stores/passes canonical AdvA, emits truthful typed TX outcomes, preserves causal masks across partial and restore faults, and fail-stops visibly. [source: code-checker final host re-review ses_031304a81ffeDVVDIzYydLu1ap]
- [major] Dedicated Phase 1 firmware and provenance integration absent(resolved)— Resolved: the non-routing production-radio ble_link_v2_testbed, target-scoped generated profiles, strict identity inventory, runtime evidence, and durable artifact publisher are implemented and final-checker accepted. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [minor] Full DATA and FLOOD caches lack deterministic replacement(resolved)— Resolved: DATA skips pinned entries and replaces the wrap-safe oldest legal victim with stable ties; FLOOD uses expired-first deterministic oldest replacement. Full, tie, wrap, and all-pinned tests pass. [source: code-checker re-review ses_031304a81ffeDVVDIzYydLu1ap]
- [minor] HELLO Q validation and wire guard coverage incomplete(open)— Partially resolved: illegal HELLO Q combinations are rejected and mandatory Section 11 capacity vectors/mutations pass. Nonblocking completeness remains for constant-based HACK16/HACK8/RREQ16/RREP16 guards and the HELLO16 bootstrap vector. [source: code-checker final host re-review ses_031304a81ffeDVVDIzYydLu1ap]
- [major] Link harness cross-task queues are data-racy(resolved)— Resolved with fixed-capacity state protected by short μT-Kernel dispatch-disabled critical sections and host-tested reserve/ring behavior. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [major] Final-delivery reservation publishes before resolve(resolved)— Resolved: delivery remains provisional and invisible until ACCEPTED resolution succeeds; failure cancels without false delivery or leaked capacity. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [major] Harness service loop violates manifested poll bound(resolved)— Resolved: healthy cycles gate before poll, service one transition/dispatch, use a preemptible bounded wait, and inject/fail-stop before radio work on POLL_OVERRUN. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [major] Harness HACK suppression is not exact-current correlation(resolved)— Resolved under the user-approved testbed adapter: pre-call ordinal/state capture plus full decoded tuple/status/full-AdvA matching removes only the exact newly generated HACK and covers duplicate handling. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [major] Harness final-application policy is too permissive(resolved)— Resolved: only the exact diagnostic application shape is accepted; permanent rejection precedes transient BUSY and unsupported input consumes no BUSY hook. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [major] Generated timer registry is not runtime authority(resolved)— Resolved: one immutable generated tron_timer_config_t governs implemented scheduler, queue, link-harness, and legacy timing consumers; host tests link the frozen profile object. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [major] Global CMake inputs contaminate legacy target(resolved)— Resolved: explicit Phase 1 target selection scopes validation/builds, cross-target inputs fail configure, and the effective legacy hook alias is compiled and tested. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [major] Candidate inventory omits selected-width fleet collision checks(resolved)— Resolved: every TSV AdvA is semantically parsed, SID16 uniqueness/nonreservation gates Phase 1, and SID8/full-identity status plus selected record/hash are manifested. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [major] Artifact manifest and candidate eligibility are incomplete(resolved)— Resolved: complete capacity/timer/source/tool/identity/hook/eligibility schema and durable exact build-command/config sidecars are published with sizes and SHA-256 values. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [major] Runtime evidence cannot prove Phase 1 hardware acceptance(resolved)— Resolved: parseable identity equality, effective timer/hook configuration, causal outcomes/faults, hook activation, delivery, and transaction summary records support the planned two-board proof. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [minor] SID16-only Phase 1 rejects reserved SID8 unnecessarily(resolved)— Resolved: Phase 1 eligibility gates SID16 only while SID8 reservation/uniqueness is derived and manifested for future fixed-k activation. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [minor] Build tests miss acceptance-critical harness behavior(resolved)— Resolved for Step 1b with queue/reservation/policy/HACK/poll-gate tests, target contamination and fleet fixtures, exact manifest/sidecar checks, and dirty-candidate rejection. Positive clean candidate publication belongs to Step 1c after the immutable commit. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [major] Ordinary logger output deterministically trips poll bound(resolved)— Resolved: logger is lower priority, mesh uses a bounded preemptible wait, and the wrap-safe pre-poll gate skips radio work and fails closed before an over-budget poll. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [major] Effective build command evidence is hashed then deleted(resolved)— Resolved: exact selected-target Ninja commands, compile_commands, build.ninja, CMake cache, and generated config sidecars survive publication and are named/hashed/sized in the final manifest. [source: code-checker final integration gate ses_02f9806afffeuZphzNJhjbJGOT]
- [major] Immediate HACK eligibility races the sender's three-channel TX burst(resolved)— Resolved by DEV-023: every centralized receiver-side HACK producer uses validated hack_turnaround_ms sourced from the existing 8 ms radio TX-event bound and post-poll observation time. Assertion-level RED/green, wrap, all-producer, build, and adversarial checks passed. Immutable candidate 4afe720 then completed 100/100 unhooked custody transfers with balanced channel evidence and passed controlled HACK-drop/BUSY benches. [source: microbit/hardware-results/2026-08-05-linkv2-da5c0c9-failed-hardware.md; DEV-023; code-checker ses_02ed9cdcaffeW5cq6BRZYu7YF2; microbit/hardware-results/2026-08-05-linkv2-4afe720-passed-hardware.md]

## Notes
- Existing completed workplan ble-mesh-near-spec-transport is the baseline transport history and remains authoritative for the proven BLE_CORE behavior.
- External TAVRN files cannot be declared as in-workspace specFiles by workplan validation, so their absolute paths are recorded as relevant files and Phase 0 creates a versioned in-workspace TAVRN-BLE profile.
- The current exp/tavrn-ble worktree contains uncommitted, hardware-proven PING/PONG changes and hardware logs. Execution must commit that foundation separately before TAVRN implementation.
- TAVRN repository main is itself dirty; execution treats source inspection as read-only and records exact baseline commit fc5f256 plus working-tree differences used during the profile audit.
- Current hardware evidence: direct BLE addressed PING/PONG matched 113 of 150 five-minute attempts with no retries, while RF matched 142 of 148. This workplan aims to improve BLE through custody ACK/retry before routing.
- At plan creation only two micro:bit v2 boards are connected/available; the user expects at least eight and requests planning through four-board alternate-path proof and eight-node soak.
- Fixed ESC k=1 is intentionally delayed until after naive AODV proves the routing core. Mesh IDs derive naturally from the BLE MAC/FICR suffix; collision detection is required and dynamic k is future work.
- Conditional topology metadata is included only where type-specific BLE frames have remaining capacity. No frame may exceed 24 custom bytes; deviations from TAVRN DATA piggybacking are documented rather than hidden.
- All firmware logs/docs must say proof of concept and must not claim Bluetooth Mesh compliance, clinical readiness, or production-grade routing/security.
- 2026-08-04 user correction: AODV is not a separate plugin. The TAVRN router's base is AODV; FULL_TAVRN adds modules around the same route engine. At most two behavioral branches exist: legacy BLE flood and TAVRN routed.
- The current TAVRN reference repository is GPL-2.0-only. Execution should conceptually reimplement from the normative profile unless licensing is deliberately aligned; do not copy large source bodies casually.
- Plan-checker re-review found one remaining major: Phase 1 incorrectly called a link-only harness AODV_ONLY. Corrected so Phase 1 uses a dedicated non-routing test harness; AODV_ONLY is introduced only with the real AODV engine in Phase 2.
- Final plan-checker gate had no other blocker/major findings. JSON/Markdown mismatch for the Phase 1 validation was patched directly and revalidated.
- Parallel execution policy: each layer remains a serial acceptance gate, but non-overlapping codecs, pure state machines, test harnesses, and tooling fan out into dedicated slice worktrees from one immutable base. Each lane receives a focused checker; one integration writer owns shared headers/CMake/orchestration; one whole-slice checker gates the candidate commit. Builds and serial capture may run in parallel, but UID flashing remains serial.
- Execution approved with `use workflows to execute tavrn-ble-testbed` on 2026-08-04. Starting Phase 0 with foundation inventory/provenance; no new protocol implementation before contract freeze.
- Step 0a completed: existing BLE foundation host tests passed (17 packet, 12 dedupe, 12 PING/PONG plus compile guards, radio and scheduler), labelled root/leaf firmware built, and source/tests committed separately as bf17bce. Historical raw logs remain untracked and provenance-limited pending clean-commit re-proof.
- Step 0b completed from clean detached commit bf17bce/tree 5f831a3: all host suites and root/leaf builds passed; exact UID/AdvA/artifact mappings recorded; 60-second result 25/30 matched and five-minute result 98/150 matched with no unmatched/queue/semantic failure or reboot. This is the immutable best-effort baseline for link-v2 improvement. Phase 0 contract lanes 0c/0d/0e now fan out in parallel.
- Phase 0 contract lanes completed in parallel. Parent reconciled full AdvA event/build/test-hook identity seams and froze exact FAST_TEST/BALANCED/SOAK timer registry. Parent validation passed: 69/69 requirement-to-matrix coverage, seven balanced Markdown documents, no stale 16-bit transmitter seam, and clean diff whitespace. Entering whole-contract checker gate.
- Whole-contract checker pass 1 found 3 blockers and 12 major issues. Findings were grouped into seven actionable workplan items. Production-security/polish concerns were not accepted; contract correctness issues are entering one focused parallel fix pass.
- Focused Phase 0 checker-fix pass completed across the profile, wire/ACK/identity, and architecture lanes. Requirement-to-matrix coverage is now 70/70; contracts reduce net diameter to 15, use a fresh RREQ ID per ring, implement two-phase inbound DATA admission, and freeze boot-nonce, identity, capacity, repair, and E_RREP_ACK semantics. Parent reconciliation and whole-contract re-review remain in progress.
- Final bounded Phase 0 checker pass `ses_03223af62ffexcEZVBNT8ZC0C6` passed with no blocker/major findings. Accepted contracts have 70 requirements mapped 1:1 to 70 matrix rows, an exact 71-key profile/architecture timer registry, 23 validated complete wire vectors, coherent six-outcome custody semantics, and board-bound routed hardware provenance. Phase 0 is closed; Phase 1 begins with assertion-level red tests.
- Phase 1 started after contract commit 760c65d. Step 1a is authoring assertion-level red tests against the frozen link-v2 contracts while preserving all legacy baseline suites.
- Step 1a RED gate completed. Exact command `cd microbit && ./tests/protocol/run_tavrn_link_v2_tests.sh --red` compiled three binaries and exited 1 with assertion-level failures for BEARER-01..04 and LINK-01..06. Adversarial checker `ses_03192ee32ffe7TDrrn4TYEGny3` accepted the RED handoff. Legacy packet 17/17, dedupe 12/12, PING/PONG 12/12 plus guards, radio/scheduler host suites, and `./build.sh ble_mesh_node` remained green. Step 1b now implements production codec/link/queue/typed scheduler-radio behavior against these tests.
- Phase 1 production integration resumed under workflow execution on 2026-08-05. Corrected LINK-04 BUSY/REJECTED HACK fixtures so their sequence bytes correlate with custody sequences 0x6002/0x6003. `cd microbit && ./tests/protocol/run_tavrn_link_v2_tests.sh` now passes wire-v2, link-v2, and radio/scheduler-v2 suites under C99 `-Wall -Wextra -Werror`. Legacy regressions, whole-slice adversarial review, dedicated firmware harness, and build/provenance integration remain before step 1b completion.
- Whole-slice adversarial host review `ses_031474e2affe53mO8YnO2sCAPC` confirmed three acceptance-blocking High findings and two Medium contract gaps. Existing focused and legacy suites/build passed but do not exercise these defects. Entering one narrow parallel fix pass with disjoint link/wire and scheduler lanes; firmware/CMake/provenance integration remains a separate config-gated step.
- Phase 1 host fix cycle 2 completed and final adversarial re-review `ses_031304a81ffeDVVDIzYydLu1ap` accepted the pure host gate: no blocker/critical/high host issue remains. Parent reran production wire/link/radio-scheduler suites, intentional RED suite, packet 17/17, dedupe 12/12, PING/PONG 12/12 plus guards, BLE driver/scheduler tests, legacy firmware build, static typed-radio call check, and diff whitespace check successfully. Step 1b remains in progress solely because the dedicated non-routing link-v2 firmware/build/provenance integration has not been implemented; that next step requires explicit approval to modify configuration files.
- 2026-08-05 configuration gate approved: user authorized CMake/build changes for centralized Phase 1 profiles/source selection, the dedicated non-routing ble_link_v2_testbed target, and artifact/provenance publication. Preserve legacy wire-v1 behavior/builds, do not flash hardware during integration, and require full host/build/manifest regression plus final adversarial review.
- 2026-08-05 harness control decision: use deterministic build-time testbed inputs rather than a runtime serial command parser. Add generated peer AdvA, initiator flag, TX interval, and transaction target; use strict one-record-per-line TSV inventory `<probe_uid>\t<canonical_adva>`. Keep production protocol headers frozen. Implement HACK-drop/BUSY/RX-block behavior in the dedicated testbed adapter at scheduler/link boundaries and manifest every input.
- Phase 1 firmware/build integration lane `ses_02fb040a7ffeHsHYtKjwfpeims` added the dedicated production-radio link-v2 harness, centralized 71-key profile/config generation, strict TSV candidate identity validation, clean-temp artifact publisher, and focused positive/negative profile tests. Parent validation passed the production and RED link suites, all legacy host suites, the new build/profile/publisher suite, and both `./build.sh ble_mesh_node` and `./build.sh ble_link_v2_testbed`. Whole-integration adversarial review remains required before step 1b completion.
- Whole integration checker `ses_02f9806afffeuZphzNJhjbJGOT` verified the 71 timer registry, source isolation, fake-route rejection, SID16 runtime, basic publisher hashes/naming, and dirty-candidate rejection, but rejected step 1b on ten High acceptance defects. Entering a narrow fix pass; no commit or hardware action until all major findings are re-reviewed.
- Integration re-review resolved queue safety, provisional delivery, exact HACK suppression, app policy, timer authority, target scoping, fleet validation, runtime evidence, and most manifest requirements. One final disjoint fix pass remains for preemptible logger scheduling/pre-poll overrun checking and durable exact build-command sidecars.
- Final integration checker `ses_02f9806afffeuZphzNJhjbJGOT` passed Step 1b with no blocker/critical/high/medium findings. Parent and checker verified host/RED suites, all legacy regressions, immutable timer authority, harness state/poll-gate tests, target/profile/inventory/publisher tests, both firmware builds, durable exact command sidecars, script modes, diff hygiene, and valid workplan structure. Step 1b is complete; Step 1c begins immutable commit and clean candidate publication.
- Step 1c completed from immutable commit da5c0c9 in clean detached worktree `/tmp/opencode/tavrn-linkv2-da5c0c9` with initialized clean submodule 5606cfb. Four BALANCED board-bound publications passed independent manifest/artifact/sidecar verification in `/tmp/opencode/tavrn-linkv2-da5c0c9-artifacts`: root unhooked ELF fea3b76b50cc677b6eb6a62dd5170ca2bdb6a48e065750d23a55c32a5175285b; leaf unhooked 8a621542f008c62f85e2dec455b7c54b745d778825014bbf50009c54f0e9bf8b; leaf HACK-drop bench 616fa9e8827af80ca18259e7d50f7756393472a542415e785a4d7509a0f44f27; leaf BUSY bench 883e20d7ad28279f736226d8f8d3213c9d954901489459400bc26789f4249590. Unhooked artifacts are candidate.eligible=yes; hooked artifacts are candidate.hook_bench_eligible=yes and unhooked acceptance=no. Entering Step 1d UID-targeted serial hardware qualification.
- Step 1d hardware qualification blocked on 2026-08-05 before any device operation: repository-resolved pyOCD 0.45.1 reported `No available debug probes are connected`, and no `/dev/serial/by-id` links existed. No erase/load/reset/flash occurred. Clean commit da5c0c9 and all four verified candidate/bench artifacts remain preserved under `/tmp/opencode/tavrn-linkv2-da5c0c9-artifacts` for UID-targeted continuation when the known root and leaf boards are reconnected.
- Step 1d resumed after both known micro:bit v2 probes reconnected. Exact UID/AdvA, clean source commit/tree/submodule, unhooked manifests, artifact hashes, serial links, and pyOCD targets were verified before sequential flash/reset/capture.
- Immutable candidate da5c0c9 failed unhooked hardware acceptance on 2026-08-05. Post-reset 60-second result was root 25/60 custody transfer and leaf 45 unique deliveries. Five-minute result was root 43/100 transfer with 57 retry exhaustion and leaf 82 unique/28 duplicate, with all fault/reset/queue-remnant indicators zero. Boot-phase diagnostic runs produced 44/100 vs 81 unique and 42/100 vs 85 unique, so simple dwell phase lock was not dominant. Evidence is summarized in microbit/hardware-results/2026-08-05-linkv2-da5c0c9-failed-hardware.md. Hooked runs were deferred.
- User reported a more hostile RF environment and approved the corrective slice. Preserve the >=95/100 gate. First fix the deterministic HACK half-duplex turnaround race with the existing 8 ms radio TX-event bound and add aggregate RF telemetry; do not tune retry, dwell, channel order, timeout, or power policy without replacement-candidate evidence.
- Corrective Step 1c2 RED gate is reproducible with `cd microbit && ./tests/protocol/run_tavrn_link_v2_tests.sh --turnaround-red`: production wire and scheduler tests pass, exactly eight LINK-02 HACK due-time assertions fail from the host-only immediate-HACK mutation, and the runner exits 1; compile/link setup failures exit 2. The historical Phase 1 `--red` mode remains unchanged.
- Corrective Step 1c2 implementation now sources validated `hack_turnaround_ms=8` from the existing radio TX-event bound, delays the centralized HACK path, timestamps link events at post-poll observation, and emits guarded testbed-only valid-wire/channel plus scheduler aggregates. Parent full host/legacy/build validation passed. Final adversarial re-review `ses_02ed9cdcaffeW5cq6BRZYu7YF2` reports no blocker/major and accepts the slice for commit and clean candidate publication.
- Step 1c2 completed at immutable replacement commit `a8589fc586aa2889af241c84653cb64d38d0c8f1` (tree `6b72b6a477760b846aaac94a79d0838a913068bd`) from clean detached worktree `/tmp/opencode/tavrn-linkv2-a8589fc` with clean submodule `5606cfba1625350901ad2eb521572b0a3f7735cb`. Four BALANCED UID/AdvA-bound publications under `/tmp/opencode/tavrn-linkv2-a8589fc-artifacts` passed sorted/unique manifest, source, submodule, configuration, role/hook eligibility, and artifact/evidence hash verification. ELF SHA-256: root unhooked `426ea4627ba8d64c0c8518cf7b686512680666d05a5589d348916df0faea93bd`; leaf unhooked `ca2b03694b8ed14825c2c3b47784d5ce837d829924b759a2a72694a28c5517dc`; leaf HACK-drop bench `b378aef11b0eab3438a63d598b57f6decea2b2c824571d6f45723134edf50859`; leaf BUSY bench `fb27f5dccd812ed849f87919d8eccfc96549e8ba2b4543e86d1c1f2d9d79feac`. Step 1d resumes with the unhooked replacement artifacts; hooked images remain deferred until the >=95/100 gate passes.
- Replacement candidate `a8589fc` failed the first unhooked 60-second smoke before any RF TX. Fresh FICR/runtime identity checks passed on both exact UIDs, but the root accepted its first submission at 308 ms then latched `BLE_MESH_SCHED_FAULT_POLL_OVERRUN` at 311 ms (`scheduler_service_fault=1`, reason 3, requested/completed masks zero); root `tx_ok=0`, and the leaf observed no valid DATA/HACK. The five-minute and hooked runs were stopped. The strict 2 ms operation-return-to-next-poll contract is being tripped because the harness always adds a 1 ms task delay even when submission/dispatch work has already consumed the available slack. Proposed minimal follow-up is harness-only dynamic yielding: spend only remaining pre-poll slack on healthy cycles, preserve the bounded logger yield on idle/fault cycles, add wrap/boundary host tests, then re-review and republish without changing the 71 timers, HACK correction, retry/dwell/channel/power policy, or >=95/100 gate.
- Approved Step 1c3 harness correction is host-complete. The focused test first failed to compile on the absent `link_testbed_mesh_remaining_yield_ms` seam, then passed after implementation. Healthy cycles now yield only unconsumed service-gap slack; the initial bounded RX-start return seeds the same pre-poll gate; faulted cycles retain a bounded logger yield; runtime evidence reports `mesh_yield_max_ms=1 mesh_yield_policy=remaining_slack`. Focused state/profile tests, link-v2 production and exact-eight-assertion turnaround RED, packet 17/17, dedupe 12/12, PING/PONG 12/12 plus timing, scheduler/radio, both firmware builds, and diff hygiene passed. Adversarial re-review `ses_02d6ad08effeRX0y5W2mlzsqR1` reports no blocker/major and accepts commit plus clean candidate publication; fresh hardware remains required.
- Step 1c3 completed at immutable candidate commit `4afe7208f653a417dbfbab3ae3e5c150d33e179b` (tree `13c45934ff18d87770701ee46317007145174b20`) from clean detached worktree `/tmp/opencode/tavrn-linkv2-4afe720` with clean submodule `5606cfba1625350901ad2eb521572b0a3f7735cb`. Four BALANCED UID/AdvA-bound publications under `/tmp/opencode/tavrn-linkv2-4afe720-artifacts` passed sorted/unique manifest, clean source/submodule, exact configuration, role/hook eligibility, and all artifact/evidence hash verification. ELF SHA-256: root unhooked `741bc9cc4d8921010cebe65bd3d3b0c506ac9a68fcaff9897b2fc8d595f758b1`; leaf unhooked `b7cf8edabc0be5c0d3f9976eb30603072a4a85eeb2fbbd321250f8231738e31d`; leaf HACK-drop bench `517b556c3ac070dfc23fc9bf684617fd35884fa42cc9d2c694805b5922f41977`; leaf BUSY bench `96d1eec3783a0abdf7d4f770a4462c68ee1e54ef38f5fcd6362c9376b270fb18`. Step 1d resumes with only the unhooked candidate artifacts; hooked benches remain deferred until the >=95/100 unhooked gate passes.
- Step 1d and Phase 1 passed on candidate `4afe720`. Smoke completed 59/60 custody transfers with no faults. Five-minute qualification completed 100/100 and remained stable past 305 seconds. Controlled HACK-drop and BUSY runs exercised retry, duplicate custody, and deferred admission successfully. Results are summarized in `microbit/hardware-results/2026-08-05-linkv2-4afe720-passed-hardware.md`.
- Fresh confirmation runs passed 99/100 and 97/100 unhooked gates plus repeated HACK-drop and BUSY behavior with no terminal faults. Phase 1 is closed; keep future experiments lightweight and prioritize implementation delivery over evidence packaging.

## Status
- Overall status: in_progress
- Metadata file: .opencode/workplan/tavrn-ble-testbed.json
- Detailed plan file: .opencode/workplan/tavrn-ble-testbed.md
