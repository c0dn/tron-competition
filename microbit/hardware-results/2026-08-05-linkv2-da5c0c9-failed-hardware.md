# Link-v2 candidate `da5c0c9` failed hardware qualification

Date: 2026-08-05

This records a failed proof-of-concept hardware gate. It does not qualify the
candidate, claim reliable delivery, or claim production or Bluetooth Mesh
readiness. Hooked HACK-drop and BUSY runs were intentionally deferred because
the unhooked baseline did not pass.

## Immutable source and build

- Source commit: `da5c0c9713c41d96c8b1100d85ac00aaec58ede5`
- Source tree: `e0f5633c95de181ed80aa6a7d8289c2e171e4aba`
- Kernel submodule: `5606cfba1625350901ad2eb521572b0a3f7735cb`
- Source state during build: clean detached worktree
- Target: `ble_link_v2_testbed`
- Behavior: `LINK_V2_HARNESS`
- Timer profile: `BALANCED`
- Test hooks: disabled
- HACK timeout/attempts: 250 ms / 3
- Scheduler dwell: 50 ms
- Derived radio TX-event bound: 8 ms
- pyOCD: 0.45.1

## Board and artifact mapping

| Role | Probe UID | AdvA | ELF SHA-256 |
| --- | --- | --- | --- |
| Root/initiator | `9906360200052820cf57b9f988a30e16000000006e052820` | `18:42:de:52:4a:dd` | `fea3b76b50cc677b6eb6a62dd5170ca2bdb6a48e065750d23a55c32a5175285b` |
| Leaf/receiver | `99063602000528205539bee7957c8dea000000006e052820` | `dc:4b:0a:06:03:f8` | `8a621542f008c62f85e2dec455b7c54b745d778825014bbf50009c54f0e9bf8b` |

Both probes were detected as BBC micro:bit V2 / nRF52833 targets. FICR AdvA,
configured identity, scheduler identity, strict two-record inventory, serial
links, source provenance, and artifact hashes were verified before UID-targeted
operations. The leaf and root were flashed sequentially with their exact
unhooked artifacts.

## Results

### 60-second smoke

Only the post-reset section of each log is evaluated; the capture opened before
the synchronized reset and therefore contains a short prefix from the prior
run.

- Root accepted 60 submissions, transferred custody for 25, and exhausted 35.
- Leaf delivered 45 unique DATA and observed 12 committed duplicates.
- No radio/service fault, queue drop, HACK enqueue failure, reset, or stall was
  observed.
- Logs:
  - `2026-08-05-linkv2-da5c0c9-60s-root.log`
  - `2026-08-05-linkv2-da5c0c9-60s-leaf.log`

### Five-minute stability

- Root accepted exactly 100 submissions, transferred custody for 43, and
  exhausted 57.
- Leaf delivered 82 unique DATA and observed 28 committed duplicates.
- Counters remained stable after the 100th transaction through five minutes.
- All scheduler/radio/service faults, submission failures, queue remnants,
  provisional deliveries, diagnostic drops, and HACK enqueue failures remained
  zero. Exactly one startup occurred in each post-reset capture.
- Logs:
  - `2026-08-05-linkv2-da5c0c9-5m-root.log`
  - `2026-08-05-linkv2-da5c0c9-5m-leaf.log`

### Boot-phase diagnostics

Two additional unhooked 100-transaction runs tested whether the 250 ms retry
and 1000 ms submission periods were phase-locked to the 50 ms RX dwell.

| Run | Measured root-minus-leaf startup | Root transfer/exhaust | Leaf unique/duplicate |
| --- | ---: | ---: | ---: |
| Nominal 25 ms delay | about 643 ms | 44 / 56 | 81 / 23 |
| Nominal 75 ms delay | about 686 ms | 42 / 58 | 85 / 26 |

The first reset-latency variation did not materially shift startup phase. The
second moved it by about 43 ms but did not materially improve completion, so a
simple boot-phase lock is not the dominant loss mechanism.

- Logs:
  - `2026-08-05-linkv2-da5c0c9-phase25ms-root.log`
  - `2026-08-05-linkv2-da5c0c9-phase25ms-leaf.log`
  - `2026-08-05-linkv2-da5c0c9-phase75ms-root.log`
  - `2026-08-05-linkv2-da5c0c9-phase75ms-leaf.log`

## Finding and disposition

Result: **FAIL** for the Step 1d requirement of at least 95 complete custody
transactions out of 100.

Read-only implementation and trace review found a deterministic half-duplex
turnaround race. A DATA attempt is synchronously transmitted on channels
37, 38, and 39. On reception, every HACK is immediately eligible. If DATA is
captured before the sender finishes its remaining channels and restores RX, the
receiver can transmit the HACK while the sender cannot listen. This explains
the dominant gap between receiver delivery and sender custody transfer.

The 15--19 unique DATA misses across the 100-transaction runs are separate from
that HACK race and are consistent with the user's report that the current RF
environment is more hostile. The replacement candidate will delay HACK
eligibility by the existing 8 ms radio TX-event bound and add aggregate
wire-type/channel and scheduler RX telemetry. The >=95/100 gate remains
unchanged; retry, dwell, channel-order, timeout, and power policy will not be
tuned without corrected telemetry.
