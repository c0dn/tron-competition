# Link-v2 candidate `4afe720` passed hardware qualification

## Disposition

Candidate `4afe7208f653a417dbfbab3ae3e5c150d33e179b` passes the Phase 1
unhooked `>=95/100` custody gate, five-minute stability requirement, controlled
first-HACK suppression bench, and controlled BUSY-admission bench.

This remains a medium-correctness non-routing proof of concept. It is not a
production, security, clinical, Bluetooth Mesh, `AODV_ONLY`, or `FULL_TAVRN`
qualification.

## Candidate and setup

- Source commit: `4afe7208f653a417dbfbab3ae3e5c150d33e179b`
- Profile: BALANCED, with the frozen 71 timer values and `>=95/100` gate.
- Root and leaf used unhooked images for acceptance; leaf-only hook images were
  used for the HACK-drop and BUSY experiments.

## Board identity and flash boundary

| Role | Probe UID | Serial | Runtime AdvA |
| --- | --- | --- | --- |
| Root | `9906360200052820cf57b9f988a30e16000000006e052820` | `/dev/ttyACM1` | `18:42:de:52:4a:dd` |
| Leaf | `99063602000528205539bee7957c8dea000000006e052820` | `/dev/ttyACM0` | `dc:4b:0a:06:03:f8` |

Both boards reported the expected role, peer, profile, timers, hook state, and
remaining-slack yield policy at runtime.

## Unhooked smoke

The 60-second post-reset smoke passed:

- Root accepted 60 transactions and completed 59 custody transfers (`98.3%`).
- One transaction exhausted radio-loss retries; all others completed.
- Root observed valid HACKs on channels 37/38/39 as `17/20/22`.
- Both boards reported zero radio/service/scheduler faults, queue drops, hook
  activity, diagnostic drops, unresolved queue state, or latched fault.
- The corrected startup and dynamic-yield path did not produce a poll overrun.

The UART files intentionally include a short pre-reset prefix. The leaf reset
completed before the root reset and accepted stale-root sequence 14 after its
own boot but before the new root session. Its final `22/21/23` DATA aggregate
therefore includes that stale frame and is retained only as diagnostic evidence,
not as the paired smoke denominator. Smoke acceptance uses the root's clean
post-reset 59/60 custody result and fault counters.

## Unhooked five-minute stability

The five-minute run passed the full gate:

- Root final at 99.429 s: 100 accepted, **100 custody transfers**, zero retry
  exhaustion, zero rejection/BUSY expiry/local-not-attempted/fault terminal.
- Root remained stable through 305.364 s with the same 100/100 result and no
  reset, stall, queue remnant, or fault. It observed exactly 100 valid HACKs,
  distributed `28/38/34` across channels 37/38/39.
- Leaf reported 100 unique application deliveries and 10 committed duplicates.
  It observed 110 valid DATA frames, distributed `39/37/34` across channels
  37/38/39, and remained stable through 305.365 s.
- Both boards ended with zero radio/service/scheduler faults, queue/relay/length
  drops, hook counters, diagnostic drops, provisional deliveries, or HACK
  enqueue failures.

The 100/100 result exceeds the frozen `>=95/100` threshold without changing the
71 timers, retry count, dwell, channel order, response windows, or power policy.

## Confirmation reruns

Fresh flashes first repeated the 100-transaction gate:

- Root accepted 100 transactions, completed 99 custody transfers, and had one
  retry exhaustion (`99%`, above the frozen gate).
- Leaf recorded 100 unique deliveries and nine committed duplicates.
- Both runtime records again matched FICR, effective/scheduler identity, roles,
  peers, BALANCED timers, disabled hooks, and remaining-slack yield policy.
- Both boards reported zero radio/service/scheduler faults, queue/relay/length
  drops, diagnostic drops, provisional deliveries, or latched faults.

The recorded-flash five-minute run then independently passed and remained
stable past 305 seconds:

- Root completed 97/100 custody transfers with three retry exhaustions.
- Leaf recorded 99 unique deliveries and 19 committed duplicates.
- Root observed 97 valid HACKs as `28/33/36`; leaf observed 118 valid DATA
  frames as `42/39/37` across channels 37/38/39.
- All radio/service/scheduler, queue/relay/length, diagnostic, provisional,
  HACK-enqueue, and latched-fault counters remained zero.

Both confirmation runs exceed the frozen `>=95/100` gate.

## Controlled first-HACK suppression

The controlled run halted the root before resetting the leaf, then reset the
root after the leaf booted. It passed:

- Leaf startup reported `hook.enabled=1`, the exact root AdvA, and drop count 1.
- Leaf reported `hook_hack_suppressed=1`, one application delivery for sequence
  1, and duplicate admission without a second application delivery.
- Root completed sequence 1 custody on attempt 2 with
  `hack_status=DUPLICATE`.
- Neither board reported retry exhaustion, fault, queue/drop, or corruption.

A repeat produced the same outcome: exactly one HACK was suppressed, sequence
1 was delivered once with duplicate admission, and the root completed custody
on attempt 3 with `DUPLICATE` and no terminal fault.

## Controlled BUSY admission

The BUSY run used the same halt-before-reset orchestration and passed:

- Leaf startup reported `hook.enabled=1` and `hook.busy_admission_count=1`.
- At 1.111 s the leaf recorded the one-shot hook and `TAVRN_RX_BUSY`; at
  1.624 s it accepted and delivered sequence 1 once after the frozen 500 ms
  BUSY backoff.
- Root completed sequence 1 custody on attempt 2 and continued without
  `CUSTODY_BUSY_EXPIRED`, rejection, retry exhaustion, or any link-break/fault
  terminal outcome. Neighbor state is not implemented by this non-routing
  Phase 1 harness.
- Final hook count was exactly one, with zero scheduler/radio/service faults,
  queue drops, diagnostic drops, or HACK enqueue failures.

The repeat forced BUSY once, accepted and delivered sequence 1 after the
required 500 ms minimum, and completed root custody on attempt 3 with no BUSY
expiry, rejection, or terminal fault.

## Phase result

Phase 1 link-v2 hardware acceptance is complete. Phase 2 begins through its
planned assertion-level RED gate; this result does not relabel the non-routing
testbed or bypass the AODV contract.
