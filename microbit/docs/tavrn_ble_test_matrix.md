# TAVRN-BLE requirement and evidence matrix

Status: planned evidence map for `TAVRN-BLE-PoC-0.1`

Frozen: 2026-08-05

Every normative profile requirement has exactly one primary row below. A row's
host/static evidence is required before its hardware evidence. `N/A` means the
requirement is non-radio and is closed by review/static evidence, not that it is
untested.

## Build and evidence legend

- `LF`: `LEGACY_FLOOD` wire-v1.
- `A`: `TAVRN_ROUTED/AODV_ONLY`.
- `F`: `TAVRN_ROUTED/FULL_TAVRN`, repair off.
- `FR`: `TAVRN_ROUTED/FULL_TAVRN`, repair on.
- `R*`: all applicable routed levels (`A`, `F`, `FR`).
- `ALL`: `LF` plus all applicable routed levels.
- `W2-LINK`: two-board custody/retry proof.
- `W2/3-AODV`: two-board direct and three-board forced-relay proof.
- `W3-8-FULL`: three-to-eight-board GTT/ESC/mentorship/maintenance proof.
- `W4-REPAIR`: four-board transit repair with alternate next hop.
- `W-SOAK`: finite profile-labelled soak.

## Authority, scope, modes, and bearer

| Test row | Requirement | Build | Planned host/static evidence | Planned hardware evidence |
| --- | --- | --- | --- | --- |
| TM-AUTH-01 | `AUTH-01` | ALL | Contract checker confirms profile/deviation/matrix references and no competing firmware spec. | N/A; evidence docs cite profile version. |
| TM-AUTH-02 | `AUTH-02` | ALL | Provenance audit checks source commit/tree, dirty diff, source hierarchy, and deviation references. | N/A. |
| TM-AUTH-03 | `AUTH-03` | ALL | Diff/license review checks no copied GPL source body or unrecorded reuse. | N/A. |
| TM-SCOPE-01 | `SCOPE-01` | ALL | Claims lint over docs, manifests, and serial banners. | All hardware summaries use PoC wording. |
| TM-SCOPE-02 | `SCOPE-02` | ALL | Codec/static check rejects AD type `0x2A` and confirms omitted security is not claimed. | Advertisement capture confirms lab manufacturer envelope only. |
| TM-MODE-01 | `MODE-01` | ALL | Build matrix proves LF, A, F, FR and rejects invalid mode/level combinations; link harness is separately labelled. | Candidate manifests identify one valid behavior/level. |
| TM-MODE-02 | `MODE-02` | R* | Linker/dependency and deterministic scenario comparison prove one AODV engine/DATA path. | A and F repeat the same direct/relay route scenario before extensions. |
| TM-BEARER-01 | `BEARER-01` | ALL | Golden vectors and radio mock check ADV_NONCONN_IND, 37/38/39, `0xFF/0xFFFF`, routed `TR/02`, and legacy `TM/01` isolation. Testbed aggregate telemetry covers all 12 valid wire-v2 types on each primary channel and rejects malformed, foreign-network, unsupported-type, identity-conflict, and invalid-channel inputs. | Sniffer/serial channel counters in W2-LINK. |
| TM-BEARER-02 | `BEARER-02` | R* | Public DATA buffer is 10; SID16 encode cap is 7, SID8 opaque cap is 10, patient kind `01` alone requires exactly 7, and all size/exact-length/oversize paths are guarded. | Seven-byte patient and ten-byte SID8 opaque maximum frames send without truncation/reset. |
| TM-BEARER-03 | `BEARER-03` | ALL | Init/idle/listen/restore/snapshot/pre-disable/channel waits each fault within the table bound; zero completed channels yields typed TX_FAILED/no attempt, one-or-more yields TX_DONE/one attempt despite later fault, with requested/completed masks. | Per-state/partial-channel hooks log masks/faults, never false-attempt zero-channel TX, and restore RX/control schedulability. |
| TM-BEARER-04 | `BEARER-04` | R* | Mutation tests cover admission-before-dedupe/state and exact outer `AdvA[0..5]` preservation through scheduler/link events. | Foreign network/malformed/test-ID injections leave counters/state unchanged; relay logs retain full immediate transmitter. |

## Identity, serials, and acknowledgments

| Test row | Requirement | Build | Planned host/static evidence | Planned hardware evidence |
| --- | --- | --- | --- | --- |
| TM-IDENT-01 | `IDENT-01` | R* | Canonical-AdvA tests prove radio/full identity is preserved and both SID widths derive from the same array. | Logs map UID, physical AdvA, derived SID16, and SID8 where applicable. |
| TM-IDENT-02 | `IDENT-02` | R* | AdvA vectors prove little-endian low-two-byte SID16, reserved/duplicate fleet rejection, remote SID16 without full mapping in A, and `SID8=AdvA[0]` only in F context. | A inventory proves unique derived SID16; F manifests prove collision-free full-identity SID8 context. |
| TM-IDENT-03 | `IDENT-03` | R* | Direct outer-AdvA-to-derived-SID16 binding/HACK tests plus FULL empty-GTT full-identity bootstrap vectors. | W2/3-AODV resolves HACK peer physically without remote GTT; W3-8-FULL captures full identities. |
| TM-IDENT-04 | `IDENT-04` | R* | Distinct outer AdvAs deriving one direct SID16 fail that binding; reserved/colliding SID8 blocks FULL state with diagnostics. | Direct SID16 and FULL SID8 collision hooks fail closed at their respective gates. |
| TM-SERIAL-01 | `SERIAL-01` | R* | Codec widths cover every serial and reject boot nonce zero while keeping nonce outside freshness comparison. | Manifests/counters show wrap and boot-nonce hook configuration. |
| TM-SERIAL-02 | `SERIAL-02` | R* | Route/GTT high-water comparator tests around `0,1,0x7fff,0x8000,0xffff`. | Near-wrap route/GTT exchange completes without stale replacement. |
| TM-SERIAL-03 | `SERIAL-03` | R* | Exact-half route/GTT updates are unordered; separate equality-cache tests prove full-key hit/miss/expiry without serial ordering. | N/A; deterministic host boundaries. |
| TM-SERIAL-04 | `SERIAL-04` | R* | Router nonce FSM proves new-tuple one-time dedupe bypass, same-nonce idempotence, HACK/cache clear, route-through-neighbor invalidation, normal RERR, and DATA gate. | Reboot/rejoin script recovers in A and F with same AdvA/new nonce and one route-break propagation. |
| TM-SERIAL-05 | `SERIAL-05` | ALL | Virtual timer tests cross `UINT32_MAX`; compile/runtime guards reject half-range durations. | Timer-wrap hook smoke has no early/late timeout. |
| TM-LINK-01 | `LINK-01` | R* | Codec/API matrix proves DATA is the only HACKable class and distinguishes HACK, E_RREP_ACK, and controller behavior. | W2-LINK logs label logical DATA HACK only. |
| TM-LINK-02 | `LINK-02` | R* | Candidate FSM proves synchronous ACCEPTED/BUSY/REJECTED, exactly-once commit/re-HACK, 10 ms defensive BUSY cleanup, and uninterrupted timers/control/TX while a fault-held candidate exists. ACCEPTED, DUPLICATE, explicit BUSY, REJECTED, additional-DATA BUSY, candidate-timeout BUSY, and all-pinned dedupe-capacity BUSY each queue a HACK at exact post-poll observed decision-time `now+8`, including wrap. | Suppressed first HACK causes one retry/one delivery; candidate fault hook cannot stall unrelated traffic. |
| TM-LINK-03 | `LINK-03` | R* | BUSY/rejected scenarios read backoff/max/deadline from selected table profile and prove typed terminal result, custody retention, and unchanged neighbor/GTT state. | Busy hook causes no false departure and honors manifested values in W2-LINK. |
| TM-LINK-04 | `LINK-04` | R* | Four held transactions serialize to exactly one eligible attempt; others stay link-owned under DATA deadline. Prove due-1/due queue selection, future high-priority HACK non-blocking of due lower-priority work, 30 ms service, three attempts, unchanged 250/750/840 ms response/episode bounds (not submit latency), and zero/nonzero partial-TX semantics. Post-poll observed TX_DONE starts `response_deadline_ms` exactly 250 ms later, including wrap. Global radio/service fault drains every occupied slot exactly once through the matching non-link-break terminal tag while preserving real attempt/mask evidence; only RETRY_EXHAUSTED breaks a peer link. Every owned event contains failed-hop AdvA plus logical IDs only, and SID8 router resolves unique GTT context before accepting event custody. | Suppressed-HACK/queued-slot/partial-radio/service-overrun hooks meet the per-episode bound or emit the typed global fault, drain all slots without false RERR/repair/LEAVE, and log no duplicate remote full identity. |
| TM-LINK-05 | `LINK-05` | R* | Independent RREP-ACK state machine cannot satisfy DATA custody or application pending state. | RREP-ACK and HACK counters are separately observable in AODV proof. |
| TM-LINK-06 | `LINK-06` | R* | Logical-next-hop, self, flood, and no-DATA-fallback admission scenarios. | Forced chain proves only selected next hop accepts directed DATA. |

## AODV and passive GTT

| Test row | Requirement | Build | Planned host/static evidence | Planned hardware evidence |
| --- | --- | --- | --- | --- |
| TM-AODV-01 | `AODV-01` | R* | Fixed route-table CRUD/state/precursor/capacity tests prove AdvA-derived standalone SID16 keys in A, GTT-backed SID8 in F, and one common core. | Route and GTT dumps differ appropriately after route expiry. |
| TM-AODV-02 | `AODV-02` | R* | Multi-ring simulation allocates a distinct ID per transmission, equality-dedupes each, correlates rings locally, accepts any active-ring RREP, and tests reverse-route/TTL/loop rules. | W2/3-AODV counters show distinct ring IDs and one processing per `{logical origin,id}`/node. |
| TM-AODV-03 | `AODV-03` | R* | Newer/equal/shorter/stale RREP matrix including wrap. | Forced chain installs reciprocal forward/reverse routes. |
| TM-AODV-04 | `AODV-04` | R* | Virtual-time TTL progression `1,3,5,7,15`, fresh ID per TX, off-wire local correlation, Smart-TTL substitution, and 10/s limiter tests. | Discovery counters match diameter-15 topology and full fallback. |
| TM-AODV-05 | `AODV-05` | R* | Pending queue full/expiry/correlation tests and explicit no-flood assertion. | Direct and chain DATA are exactly-once or explicitly failed. |
| TM-AODV-06 | `AODV-06` | R* | Retry-exhausted invalidation plus deterministic SID16/SID8 sorting, 3/4-entry segmentation, eight-action queue/backpressure, due-work overflow, and no truncation tests. | ACK-loss route break emits all expected ordered RERR segments without false queue-drop leave. |
| TM-AODV-07 | `AODV-07` | R* | ACK-required tuple mutations include destination sequence; four-wait capacity, untracked-send prevention, 250 ms cancel/expiry, scoped blacklist, and no RERR/TC/HACK/repair side effects. | Correlated ACK succeeds; suppression blacklists only implicated neighbor. |
| TM-AODV-08 | `AODV-08` | R* | Typed begin/success/release/timeout/capacity/disabled deferred-RERR commands compile in the one common core and never expose a second table/engine; A has no repair caller. | W4-REPAIR success cancels only deferred destination; repair-off/failure releases normal segmented RERR. |
| TM-GTT-01 | `GTT-01` | F/FR | Dependency test proves passive methods only and no send/timer/route ownership. | N/A; static architecture gate. |
| TM-GTT-02 | `GTT-02` | F/FR | Capacity-16, self protection, departed-then-oldest-stale replacement, fresher-state protection, and no-victim rejection tests. | Up-to-eight-board set remains stable below capacity. |
| TM-GTT-03 | `GTT-03` | F/FR | Frame-role evidence matrix includes malformed/ambiguous/overheard negative cases. | Directed/flood traffic yields expected membership evidence. |
| TM-GTT-04 | `GTT-04` | F/FR | Merge/wrap/equal-hop/tombstone/retention/rejoin virtual-time tests. | Leave/rejoin converges without stale serial lockout. |
| TM-GTT-05 | `GTT-05` | F/FR | Application enumeration and route-expiry independence tests. | Queried active set equals scripted set after convergence. |
| TM-GTT-06 | `GTT-06` | F/FR | Fresh/soft-stale positive hints, hard-expired/departed/zero-hop negative hints, and mandatory full-fallback simulations. | Known-destination initial scope shrinks; forced miss still discovers. |

## ESC, mentorship, and maintenance

| Test row | Requirement | Build | Planned host/static evidence | Planned hardware evidence |
| --- | --- | --- | --- | --- |
| TM-ESC-01 | `ESC-01` | F/FR | Build/dependency test contains fixed k=1 only and gates enable after bootstrap/AODV evidence. | F candidate begins compressed traffic only after bootstrap. |
| TM-ESC-02 | `ESC-02` | F/FR | Unique/unknown/multiple-context decompression tests assert no partial state. | Duplicate-suffix hook remains fail-closed. |
| TM-ESC-03 | `ESC-03` | F/FR | Context-loss FSM returns to full bootstrap; no dynamic-AM transition exists. | Mentor/context reset visibly reboots bootstrap flow. |
| TM-BOOT-01 | `BOOT-01` | R* | HELLO N=1 vectors reinterpret node-sequence as nonzero boot nonce for both levels; FULL offer/SYNC remains full identity. | A and F captures show full AdvA/nonzero nonce before establishment. |
| TM-BOOT-02 | `BOOT-02` | F/FR | Each profile reads canonical RSSI magnitude/delay, jitter, and suppression keys; seeded endpoint/interpolation/cancellation tests reject private literal defaults. | Competing offers/cancellation follow manifested values on three boards. |
| TM-BOOT-03 | `BOOT-03` | F/FR | Offer-window tests use `timer.mentor_offer_window_ms` per profile plus complete three-key tie-break permutations. | W3-8-FULL selects the manifest-predicted mentor/window. |
| TM-BOOT-04 | `BOOT-04` | F/FR | Sorted capacity-16 frozen snapshot with concurrent join/leave mutation test. | Mentor mutation during sync does not duplicate/skip page identity. |
| TM-BOOT-05 | `BOOT-05` | F/FR | Empty/one-record page vectors, full AdvA/serial/TTL-state-hop fields, no-lastSeen assertion, offered-count/index checks, duplicate idempotence, and hop saturation. | Multi-page pull imports expected identities/hops without foreign-clock timestamps. |
| TM-BOOT-06 | `BOOT-06` | R* | FAST/BAL/SOAK router announcement windows self-establish A without FULL modules; F continues offer/page timeout/three-attempt/restart/self-bootstrap FSM. | A self-establishes after bound; F mentor removal/no-mentor paths recover within manifested bounds. |
| TM-MAINT-01 | `MAINT-01` | F/FR | BALANCED exact values; seeded jitter bounds; FAST/SOAK ordering checks. | Manifests publish effective values for maintenance runs. |
| TM-MAINT-02 | `MAINT-02` | F/FR | EMA growth/snap/reset/suppression/liveness tests use selected hello change/stable/alpha/snap keys; BALANCED examples are not defaults. | Stable/churn counters follow manifested profile cadence. |
| TM-MAINT-03 | `MAINT-03` | ALL | Registry checker enforces the exact 71-key set, owners/values, dedicated `scheduler_poll_max_ms=2` distinct from loop delay, and formula `(2+1)*(8+2)=30` plus all traversal/repair/840 ms formulas. It confirms that `hack_turnaround_ms` sources the existing radio TX-event bound rather than adding a timer key. | Manifests expose exactly the authoritative profile keys; route/link bounds use selected values. |
| TM-MAINT-04 | `MAINT-04` | F/FR | Demand predicate truth table for queue/route/precursor/repair/app versus slot-only. | Active destination is probed; off-path expired destination is not. |
| TM-MAINT-05 | `MAINT-05` | F/FR | No-demand expiry emits no HELLO/RREQ/TC and allows later resurrection. | Idle expiry counters show no verification flood. |
| TM-MAINT-06 | `MAINT-06` | F/FR | Stage-0/stage-1/three-RREQ/cancel/fallback scenarios complete within exact 2100/8400 total bound. | Route-assisted success and forced full fallback complete within manifest bound. |
| TM-MAINT-07 | `MAINT-07` | F/FR | Five simultaneous expiries start four/defer one; active/rate limits remain bounded. | Mass-expiry script respects per-cycle cap and later drains. |
| TM-MAINT-08 | `MAINT-08` | F/FR | TC TTL, UUID/subject equality retention from selected `tc_*` keys, serial wrap, and non-link-failure negative scenarios. | Join/leave converges at manifested retention without duplicate flood or false leave. |
| TM-META-01 | `META-01` | F/FR | Four-candidate priority/round-robin/fairness tests use selected `metadata_cooldown_ms`; BALANCED 5 s is example-only. | Metadata counters follow manifested cooldown without address-order starvation. |
| TM-META-02 | `META-02` | F/FR | Compile guards/golden vectors prove SID16 zero, SID8 RREQ 4, RREP 3, and RERR 4/3/1/0 capacities plus zero-slot controls. | Maximum eligible controls advertise without truncation/reset. |
| TM-META-03 | `META-03` | F/FR | DATA/noneligible no-metadata assertions; targeted HELLO exactly-one vector. | DATA remains base-sized; verification HELLO shows one request. |
| TM-META-04 | `META-04` | F/FR | Transmitter attribution, strip-before-delivery, overflow, and atomic malformed tests. | Transit metadata refreshes immediate transmitter only. |

## Local repair, capacities, and provenance

| Test row | Requirement | Build | Planned host/static evidence | Planned hardware evidence |
| --- | --- | --- | --- | --- |
| TM-REPAIR-01 | `REPAIR-01` | F/FR | Repair-off/on build and wire-vector identity; idle trace has zero repair traffic. | Paired F/FR idle run has identical steady-state repair count zero. |
| TM-REPAIR-02 | `REPAIR-02` | FR | Trigger truth table: transit RETRY_EXHAUSTED only; all other failures negative. | B in A-B-C receives transit retry exhaustion in W4-REPAIR. |
| TM-REPAIR-03 | `REPAIR-03` | FR | Failed-neighbor versus destination route/GTT/TC state assertions. | B marks failed C-link neighbor as applicable without falsely departing destination. |
| TM-REPAIR-04 | `REPAIR-04` | FR | One-context/four-buffer boundary, ownership, excess fallback, and immediate-RERR tests. | Capacity hooks produce bounded explicit outcomes without reset. |
| TM-REPAIR-05 | `REPAIR-05` | F/FR | One Smart-TTL TX then one full-scope TX, no inner retries, 1700/5300 timeout, deferred-RERR cancel/release, exactly-once flush, and repair-off tests. | W4-REPAIR reroutes B-D-C; forced failure and repair-off release RERR. |
| TM-BUILD-01 | `BUILD-01` | R* | Allocation/static scan plus all capacity/full-policy boundaries, especially four ACK waits, one mentorship session/16-entry snapshot, and shared eight-action sorted RERR segmentation/backpressure. | Capacity hooks complete without corruption, silent truncation, or untracked protocol work. |
| TM-BUILD-02 | `BUILD-02` | ALL | Generic/runtime-FICR manifests require exact `RUNTIME_FICR` identity sentinels and candidate-ineligible status. Routed candidate manifests require board-specific override, exact AdvA/SIDs/fleet inventory, timer/capacity/source/artifact provenance, and reject mismatches. | UID-bound pre-flash evidence independently observes FICR AdvA and proves exact override/manifest/board match. |
| TM-BUILD-03 | `BUILD-03` | ALL | Complete LF/A/F/FR host/build/linker matrix after each applicable slice. | Lower accepted candidates are smoke-regressed at final gates. |
| TM-BUILD-04 | `BUILD-04` | ALL | Evidence checker ties clean candidate commit/tree and artifact hashes to each gate. | W2-LINK, W2/3-AODV, W3-8-FULL, W4-REPAIR, and W-SOAK summaries. |

## Coverage rule

The checker extracts requirement tokens matching
`(AUTH|SCOPE|MODE|BEARER|IDENT|SERIAL|LINK|AODV|GTT|ESC|BOOT|MAINT|META|REPAIR|BUILD)-NN`
from the profile and this matrix. The two sets must be equal. A future profile
requirement is incomplete until a primary row with build, host/static evidence,
and hardware evidence or an explicit `N/A` rationale is added here.
