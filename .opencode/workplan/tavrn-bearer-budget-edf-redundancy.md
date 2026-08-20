# KISS TAVRN budgeted EDF redundant bearer

## Authoritative scope

- Raw nRF52833 RADIO remains BLE 1M `ADV_NONCONN_IND`; no SoftDevice, connections, Bluetooth Mesh, GATT, or Link Layer ARQ.
- One BU is one complete ordered channel 37/38/39 sweep.
- One-sweep and two-sweep operations are atomic. A two-sweep operation reserves both BU before TX starts.
- Rolling window: 1000 ms, 40 total BU, 32 general BU, 8 reply-critical reserve BU. Nothing bypasses the total cap.
- Queue selection is priority band, then earliest expiry, then oldest ordinal. No global EDF and no tree/heap queue.
- Two sweeps: every wire RREQ, every wire RREP (adopting the recommendation), DATA, and bootstrap HELLO.
- One sweep: HACK, RREP_ACK, RERR, ordinary HELLO/TC, SYNC, other maintenance/repair control, FLOOD, and relay traffic.
- Critical-reserve eligibility: HACK, RREP, RREP_ACK, SYNC_OFFER, and SYNC_DATA only. RREQ, DATA, and bootstrap HELLO are general.
- No BLE LBT/CCA in this work.
- Preserve producer ownership exactly. Token NONE stays anonymous; existing low/high token ranges continue through current scheduler events. Do not add owner or token domains.

## Completed mechanics

- Routed capacity variants 4/8/16/40; LEGACY and LINK remain 4.
- Mandatory expiry metadata, wrap-safe priority-banded EDF selection, and bounded expiry retirement through existing token events.
- Fixed rolling BU accounting and custody hold behavior.
- Atomic one/two-sweep radio operations with per-sweep completion evidence.
- Canonical 8/14/18 ms radio bounds and budget-aware custody timing formulas.
- Final frame/reserve mapping and first-copy response release.
- Over-scoped owner/global-reset scaffolding removed; normative docs reconciled.

## Completed observer hardening

- Observer-v3 records one compact accepted-source event and one compact final-delivery event, with exact workload-split offered/accepted/rejected/not-ready checkpoints.
- A benchmark-only 16 KiB UARTE/EasyDMA transport commits complete LF-terminated records asynchronously; saturation invalidates evidence but never backpressures router delivery.
- Logger service requests are explicit, and benchmark clock timestamps are sampled immediately before the LF-adjacent suffix so host/device clock fits remain valid.
- Source and destination checkpoints are matched by `(origin_session, identity)` over complete prefixes. Terminal partial UART records are right-censored.
- Application, control, GTT, clock, transport-health, and queue-accounting validation remain separate domains. Production MIND behavior is unchanged.
- Original hardware-tested commits `4069d29`, `9f76264`, and `beb776a` remain reachable from the consolidated branch.

## Benchmark and capacity selection

The 18c94cb queue-40/two-sweep 1200-second run is invalid evidence: source telemetry reached 1024/1024 and dropped 3985 records, while the destination final queue reached 8 and could feed `BUSY` into router delivery. Queue 4 only rejected load early, and the one-sweep DATA override admitted no link transmissions. Neither is a production fix.

Observer-v3 closure produced a valid 130-second smoke run and a recovered 1200-second FULL_TAVRN run. The canonical run contains 18,234 complete records and zero invalid telemetry intervals. Application, transport, control, and all six clock fits are valid; heartbeat delivered/accepted PDR is 0.990385 and throughput delivered/accepted PDR is 0.921586. GTT remains formally incomplete because role E exceeded the strict cadence limit once by 34 ms, although reporting resumed and the final nine fleet windows were complete and converged.

The checksummed bundle is preserved under `microbit/hardware-results/2026-08-20-tavrn-observer-v3-benchmark-1200-recovered/`. The remaining benchmark step is to rerun AODV with the same observer-v3 contract after reboot and generate the canonical comparison with `compare_tavrn_benchmarks.py`.

## Baseline evidence limitation

The recorded base is `origin/master@648efe497624444e412cff28103fbabeffba5530`; available D2 evidence includes `/tmp/opencode/d2-idempotent-custody.report` and its selected-source hashes. No restorable pre-bearer patch or index snapshot was captured. These artifacts cannot reconstruct the mixed worktree. Final closure must review shared-file hunks manually against the KISS scope and preserve known pre-bearer idempotent custody/router behavior; it must not claim automatic attribution.

## Safety

- Preserve the current 5000 ms absolute DATA deadline; three attempts remain a ceiling under budget pressure.
- Preserve the existing unused originated-FLOOD API and add no production caller.
- Preserve pre-bearer custody/router changes and evidence; bearer work must not rewrite them.
- No reset, checkout, clean, commit, deletion, hardware flash, or rollout without explicit approval.
