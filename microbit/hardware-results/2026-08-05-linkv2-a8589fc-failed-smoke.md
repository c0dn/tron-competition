# Link-v2 replacement candidate `a8589fc` failed smoke

## Disposition

Candidate `a8589fc586aa2889af241c84653cb64d38d0c8f1` is rejected for
hardware acceptance. The first unhooked smoke latched a service fault before
the root attempted RF transmission. The five-minute stability run and both
hooked runs were therefore not performed.

This failure is separate from the HACK turnaround correction. The strict
2 ms operation-return-to-next-poll contract exposed an unconditional harness
yield after initial submission/dispatch work had already consumed the available
slack.

## Immutable provenance

- Source commit: `a8589fc586aa2889af241c84653cb64d38d0c8f1`
- Source tree: `6b72b6a477760b846aaac94a79d0838a913068bd`
- Clean detached worktree: `/tmp/opencode/tavrn-linkv2-a8589fc`
- Clean submodule: `5606cfba1625350901ad2eb521572b0a3f7735cb`
- Artifact directory: `/tmp/opencode/tavrn-linkv2-a8589fc-artifacts`
- Root unhooked ELF SHA-256:
  `426ea4627ba8d64c0c8518cf7b686512680666d05a5589d348916df0faea93bd`
- Leaf unhooked ELF SHA-256:
  `ca2b03694b8ed14825c2c3b47784d5ce837d829924b759a2a72694a28c5517dc`
- Leaf HACK-drop bench ELF SHA-256, not flashed:
  `b378aef11b0eab3438a63d598b57f6decea2b2c824571d6f45723134edf50859`
- Leaf BUSY bench ELF SHA-256, not flashed:
  `fb27f5dccd812ed849f87919d8eccfc96549e8ba2b4543e86d1c1f2d9d79feac`

All four publications passed independent sorted/unique manifest, source,
submodule, role/hook eligibility, and artifact/evidence hash verification.

## Board identity and flash boundary

| Role | Probe UID | Serial | FICR words at `0x100000a4` | Derived/runtime AdvA |
| --- | --- | --- | --- | --- |
| Root | `9906360200052820cf57b9f988a30e16000000006e052820` | `/dev/ttyACM1` | `0x52de4218 0x00005d4a` | `18:42:de:52:4a:dd` |
| Leaf | `99063602000528205539bee7957c8dea000000006e052820` | `/dev/ttyACM0` | `0x060a4bdc 0x00003803` | `dc:4b:0a:06:03:f8` |

pyOCD 0.45.1 identified both probes as nRF52833 micro:bit V2 boards. Fresh
pre-flash FICR reads matched the board-bound manifests. Only the two unhooked
ELFs were mass-erased, loaded, and reset by exact UID. Runtime FICR, effective
local, scheduler, configured override, peer, role, profile, candidate, timer,
and disabled-hook evidence all matched on both boards.

## Reproduced smoke result

The exact UIDs were reset leaf first and root second while both UART captures
were open. The root then produced:

- 308 ms: harness boot diagnostic.
- 309 ms: first local DATA submission accepted.
- 311 ms: `BLE_MESH_SCHED_EVENT_SERVICE_FAULT`, reason
  `BLE_MESH_SCHED_FAULT_POLL_OVERRUN` (`3`), token zero, requested/completed
  masks zero.
- Final counters: one accepted submission, one scheduler service fault, one
  terminal service fault, zero TX, zero custody transfer, and no radio,
  queue, hook, or diagnostic-drop fault.

The leaf remained healthy but observed no valid DATA or HACK. At 60 seconds it
reported zero deliveries/duplicates/faults, `rx_ok=154`, and
`rx_crc_or_empty=5521`; valid-wire telemetry remained zero for all rows and
channels.

Capture SHA-256:

- Root `2026-08-05-linkv2-a8589fc-60s-root.log`:
  `dc7e6d9bf4e97c1e2181c26b3c51020ca9b4d9b6b595a1e869e058413cb3e816`
- Leaf `2026-08-05-linkv2-a8589fc-60s-leaf.log`:
  `b2787c796fd82b89e9751a0e654bd9746e8c883e60b2e468a5577281a1098dbd`

## Diagnosis and next gate

The dedicated mesh loop always called `tk_dly_tsk(1)` after scheduler-event,
link-tick, submission, and dispatch processing. On the initiating root, the
initial submission/dispatch work plus that delay produced a 3 ms gap from the
prior scheduler-operation return to the next poll start, exceeding the frozen
2 ms service bound. The passive leaf did not execute this initial work and did
not fault.

The approved follow-up is harness-only dynamic yielding: healthy cycles may
yield only for unconsumed pre-poll slack, while idle/fault cycles preserve a
bounded logger yield. Host boundary/wrap tests, full regressions, adversarial
review, a new immutable candidate, and a fresh unhooked smoke are required.
The 71 timer values, HACK turnaround, retry/dwell/channel/power policy, and
`>=95/100` hardware gate remain unchanged.
