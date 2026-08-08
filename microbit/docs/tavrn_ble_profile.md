# TAVRN-BLE proof-of-concept profile

Status: Phase 0 normative candidate for checker acceptance

Profile version: `TAVRN-BLE-PoC-0.1`

Frozen: 2026-08-05

Accepted correctness revisions: `DEV-023` extends the existing `DEV-006`
bearer adaptation without changing its timer registry or retry policy; `DEV-024`
freezes the separate liveness model and its two explicit freshness-response
timers; `DEV-025` freezes the local-expiry/demand contract without adding a
timer key.

This document and its companion deviation and test-matrix documents define the
executable TAVRN subset for the micro:bit BLE proving ground. Only statements in
rows with stable requirement IDs are normative. Later wire and architecture
contracts derive details from these requirements; they may not silently change
the behavior.

## Authority and provenance

The audited TAVRN repository was read at commit
`fc5f25662aa3bb63d65cb24a41461f19ed42ea4e` (tree
`5e19217296ccfe03a8be1c429864fe92968cbae4`). Its tracked working-tree changes
were limited to `TAVRN_v2.md` and comments in `tavrn-packet.h`; no executable
source differed from the commit. The packet-header comment change documents
3-bit address modes, while the implementation remains fixed at one-byte suffixes.

Audited inputs:

| Input | Role and observed state |
| --- | --- |
| `TAVRN_v2.md` | Design baseline for dual expiry and application-visible passive GTT intent. Current dirty-file SHA-256: `0078084d41f3c670e69c62114b0e7d5013fd4612aa7dbb94abe73a8fd0fbcc26`. |
| ns-3 source at `fc5f256` | Evidence of evaluated behavior, not firmware source. `src/tavrn/model/tavrn-gtt.{h,cc}` supplies copied dual-expiry candidates, nondeparted enumeration, and evaluated TTL jitter; `src/tavrn/model/tavrn-routing-protocol.{h,cc}` shows that `CheckGttExpiry()` actually verifies all hard-expired entries, while its header's demand comments are non-executable intent. Relevant blobs: routing `af25d91e...`, packet implementation `39c12305...`, GTT `84e1b4ae...`, route table `ddfe5b55...`. |
| `IMPLEMENTATION.md` | Explanatory implementation guide; §5.4 and §8.6 are provenance for `uniform(0,TTL/6)` jitter, maintenance cadence, and demand-gating intent. SHA-256 `2ffd7bd53584caa8468b81a977eea33e44a8f58c9d93cc855402158e11be5a7c`. |
| `LOCAL-REPAIR-SPEC.md` | Separate v2.3 proposal; SHA-256 `9938ba6a4f7104544819e8a117a0acfa8efc13c78af2b20568a8509d580ef827`. |
| `REPORT-AUDIT.md` | Guard against unsupported empirical claims; SHA-256 `e05e6d462ecc304c9b153860549f047d6e32cd31a8bb7d419576cc67e0b9b042`. |
| `CSC2106_ TAVRN.pdf` | Background only; SHA-256 `4b49608d2c694bd1d3f720dc88598bc913fb0f3647c0e257e645e75d23026ac6`. No paper result is an acceptance criterion here. |
| BLE foundation | Clean source commit `bf17bce301cae10df0819b3b6fe7a8c19256bdde`; evidence commit `f0cda9d`; five-minute best-effort baseline `98/150`, with no reliability claim. |

| ID | Normative requirement |
| --- | --- |
| **AUTH-01** | After Phase 0 checker acceptance, this profile and its deviations/matrix are the sole top-level behavioral authority for TAVRN firmware. Accepted wire-v2, ACK, identity, and architecture documents are binding derived contracts within their declared representation/ownership scopes and remain subordinate to this profile. External TAVRN prose and ns-3 source are provenance, not alternate firmware specifications. |
| **AUTH-02** | A conflict is resolved in this order: this profile; an accepted derived wire/architecture contract; then audited TAVRN v2.2 intent; then actual ns-3 behavior. Every intentional difference from the latter two appears in the deviation register. |
| **AUTH-03** | Firmware is a clean conceptual reimplementation from these requirements. GPL-2.0-only ns-3 source bodies must not be copied unless the firmware project's licensing is deliberately made compatible and recorded. |

## Scope and implementation shape

| ID | Normative requirement |
| --- | --- |
| **SCOPE-01** | All artifacts and claims identify this as a medium-correctness proof of concept. Bluetooth Mesh compliance, production readiness, security, certification, clinical readiness, and clinical efficacy are out of scope. |
| **SCOPE-02** | The profile implements neither Bluetooth Mesh provisioning/security/models nor official Mesh Message AD type `0x2A`. Cryptography, replay protection, persistent sequence state, and adversarial routing defenses are absent and must not be implied. |
| **MODE-01** | There are exactly two node behaviors above shared BLE primitives: `LEGACY_FLOOD` using golden wire-v1, and `TAVRN_ROUTED` using wire-v2. `TAVRN_ROUTED` has feature levels `AODV_ONLY` and `FULL_TAVRN`; a link-only harness is not a third behavior or feature level. |
| **MODE-02** | `AODV_ONLY` and `FULL_TAVRN` use the same AODV route engine, route table, routed link, router binding, and DATA path. The implemented Phase 5 `FULL_TAVRN` prefix adds GTT, Smart-TTL, ESC, mentorship, HELLO/SYNC/TC bootstrap, and adaptive SID8 ordinary-HELLO cadence/suppression around that engine. Expiry, verification, metadata, patient bridging, repair, and expiry-driven TC maintenance remain later additions. |

## BLE bearer

| ID | Normative requirement |
| --- | --- |
| **BEARER-01** | The bearer is legacy `ADV_NONCONN_IND` on primary advertising channels 37/38/39 with the lab Manufacturer Specific Data envelope (`0xFF`, company ID `0xFFFF`). Routed wire-v2 is isolated by custom prefix `54 52 02` (ASCII `TR`, version 2); legacy wire-v1 remains `54 4d 01` (`TM`, version 1). Routed logical unicast remains physically observable advertising. |
| **BEARER-02** | A wire-v2 custom PDU is at most 24 bytes. The public by-value DATA application buffer and wire semantic maximum are 10 bytes because SID8 opaque DATA admits 10; SID16 admits at most 7 in a frame. Only patient `app_kind=01` has a fixed application length, exactly 7 bytes. Every type/mode has a compile-time size guard and exact-length decoder; oversize input is rejected before advertising, never truncated, fragmented, or silently stripped. |
| **BEARER-03** | The single nRF52833 radio is passive-RX by default and interleaves bounded TX windows with RX. Every mesh-radio wait—initialization, idle/disable, listen, RX restore, snapshot, TX pre-disable, and each selected-channel TX wait—is bounded by `timer.radio_state_timeout_ms` and surfaces a typed scheduler-visible radio fault. For advertising, zero completed selected channels emits `TX_FAILED` and is no attempt; one or more completed channels emits `TX_DONE`, counts one attempt, and starts the HACK deadline even if a later channel/restore wait faults. Every TX result/log records requested and completed channel masks plus fault. Firmware/tests must not claim simultaneous TX/RX or an unbounded radio call. |
| **BEARER-04** | Version, network, message type, exact length, address context, and logical receiver admission occur before dedupe or state mutation. Every receive event preserves outer `AdvA[0..5]` by value as the full immediate-transmitter identity, not merely SID16/SID8, and rejects malformed, foreign-network, self-invalid, or ambiguous frames without partial effects. |

## Identity and serial arithmetic

| ID | Normative requirement |
| --- | --- |
| **IDENT-01** | Canonical physical identity is the full six-byte BLE AdvA/FICR random-static address in raw radio-buffer/on-air array order, least-significant octet first. Logs, direct-neighbor mappings, collision diagnostics, mentorship ownership, radio transmission, AdvA overrides, and reboot handling use the same six bytes. Derived SID16/SID8 values never replace canonical AdvA for physical-peer equality. |
| **IDENT-02** | `AODV_ONLY` derives <code>SID16 = AdvA[0] &#124; (AdvA[1] &lt;&lt; 8)</code>, encoded little-endian and restricted to `0001..fffe`. SID16 is nevertheless a standalone logical route namespace: remote route/origin/destination keys require no remote full-identity or GTT mapping. Every controlled-fleet inventory and artifact manifest proves all derived SID16 values unique and nonreserved. `FULL_TAVRN` fixed `k=1` uses `SID8=AdvA[0]`, restricted to `01..fe`, only with full-identity mentorship/GTT context. |
| **IDENT-03** | A direct peer derives SID16 from observed outer AdvA and retains both values in one binding so HACK correlation names the exact physical next hop. Full identity is mandatory for SID8 lookup, bootstrap HELLO, SYNC_OFFER, SYNC_PULL, every SYNC_DATA membership record, and TC_UPDATE; an unknown SID8 is never inferred from SID16 or first observation. |
| **IDENT-04** | If two direct outer AdvAs derive the same SID16, AODV_ONLY fails that direct binding closed, invalidates routes through it, and diagnoses both AdvAs/SID16; remote uniqueness remains a controlled-fleet inventory precondition. Any reserved/colliding SID8 enters `IDENTITY_CONFLICT`, blocks affected compressed routing/DATA/mentorship, invalidates affected routes, and admits only full-identity bootstrap/diagnostic traffic. Dynamic entropy recovery and arbitrary winners are forbidden. |
| **SERIAL-01** | Route destination sequences, origin sequences, RREQ IDs, GTT sequences, TC origin sequences, routed transaction serials, and boot nonce are unsigned 16-bit wire values. TC uniqueness is `{full origin identity, 16-bit TC serial}`; boot nonce is a nonzero incarnation discriminator, not a freshness sequence. |
| **SERIAL-02** | Route and GTT high-water freshness use modulo-`2^16` half-range arithmetic: `a` is newer than `b` iff `0 < uint16_t(a-b) < 0x8000`. Equality is equal, not newer; ordinary integer `<`/`>` comparisons are forbidden for route/GTT serial freshness. |
| **SERIAL-03** | A route/GTT serial difference of exactly `0x8000` is unordered and cannot replace high-water state. Dedupe caches do not use half-range ordering: they are bounded equality-key caches whose exact tuple, retention, and deterministic replacement policy are independently defined. |
| **SERIAL-04** | On every routed boot, `tavrn_router` generates a nonzero 16-bit `boot_nonce`, clears local volatile protocol state, enters `REJOINING`, and emits one-hop full-AdvA `HELLO(N=1, node_sequence=boot_nonce)` in both AODV_ONLY and FULL_TAVRN. The first directly received new tuple `{outer AdvA,boot_nonce}` bypasses ordinary HELLO equality dedupe once, invalidates pending HACK/dedupe state and every route through that neighbor, and propagates normal precursor RERR; repeats of the same tuple are idempotent. No routed DATA is originated or forwarded until the level-specific `BOOT-06` establishment rule completes. |
| **SERIAL-05** | Firmware deadlines use unsigned 32-bit monotonic milliseconds and wrap-safe signed-difference checks. Every configured duration is strictly below `2^31` ms; expiry and retry tests cross the timer wrap boundary. |

## Routed link and acknowledgment semantics

| ID | Normative requirement |
| --- | --- |
| **LINK-01** | Legacy advertising supplies no delivery acknowledgment. Wire-v2 therefore defines a TAVRN link `HACK` as a new logical, per-hop acknowledgment for logically unicast DATA only; HACK, FLOOD, E_RREQ/E_RREP/E_RERR/E_RREP_ACK, HELLO, SYNC_*, and TC_UPDATE are never HACKable. HACK is not a Bluetooth controller ACK and not `E_RREP_ACK`. |
| **LINK-02** | A new inbound DATA candidate is returned and resolved synchronously as ACCEPTED/BUSY/REJECTED before its router/application dispatch returns. Defensive `timer.link_candidate_resolve_ms=10` expiry auto-resolves an invariant-violating outstanding candidate as BUSY and releases provisional state. Candidate presence cannot stop clock/timer advancement, control admission, scheduler TX, or HACK processing. ACCEPTED establishes exactly-once custody and HACK; DUPLICATE confirms prior custody without second acceptance and is re-HACKed once per retry reception. Every generated DATA HACK—ACCEPTED, DUPLICATE, explicit BUSY, REJECTED, additional-DATA BUSY, candidate-timeout BUSY, or dedupe-capacity BUSY—has `not_before_ms = now + hack_turnaround_ms`, where `now` is the receiver decision time and, for a scheduler RX event, the post-poll observed event time; link config sources `hack_turnaround_ms=8` directly from existing `timer.radio_tx_event_bound_ms`; this is not a timer key. |
| **LINK-03** | `HACK(busy)` and `HACK(rejected)` do not transfer custody, refresh route/GTT state, or declare a neighbor dead. BUSY defers by `timer.link_busy_backoff_ms` and is bounded by `timer.link_busy_max_responses` plus `timer.link_data_deadline_ms`; REJECTED terminates unchanged retransmission as `CUSTODY_REJECTED`. Neither outcome is a link break. BALANCED examples are 500 ms, three responses, and 5000 ms respectively; the timer table remains authoritative. |
| **LINK-04** | Four custody slots may hold DATA transactions, but link-v2 promotes exactly one initial/retry attempt into the physical scheduler and marks it eligible at a time; other slots remain link-owned, not eligible, and are bounded by `timer.link_data_deadline_ms`. An eligible attempt may be bypassed by at most `timer.scheduler_custody_bypass_max` completed best-effort events and reaches valid `TX_DONE` within `timer.link_tx_scheduler_attempt_bound_ms` or terminates `LOCAL_TX_NOT_ATTEMPTED`. Observed TX_DONE sets `response_deadline_ms = observed_TX_DONE_time + timer.link_hack_timeout_ms` with wrap-safe arithmetic; the event time is captured after the synchronous scheduler poll returns. A no-response episode uses `timer.link_max_attempts` and `timer.link_hack_timeout_ms`, with unchanged total `timer.link_response_window_sum_ms=750` and wall `timer.link_no_response_wall_bound_ms=840`; its all-profile 840 ms table value starts at first eligibility and is never submit-to-terminal latency. Only final no-response expiry produces peer-link break `RETRY_EXHAUSTED`. A latched global radio/service fault instead drains every remaining occupied slot exactly once as non-link-break `RADIO_FAULT_TERMINAL` or `SERVICE_FAULT_TERMINAL`, preserving real attempt/channel evidence and transferring custody. Every terminal event contains full failed immediate-next-hop AdvA plus logical origin/final IDs only—no duplicated remote full identities. In SID8 FULL mode, the router must synchronously resolve unique full origin/destination context through GTT before accepting event custody. |
| **LINK-05** | `E_RREP_ACK` is retained solely to acknowledge an `E_RREP` that requested one-hop return-path confirmation. It can cancel or expire the corresponding AODV unidirectional-link probe, but never transfers DATA custody or completes an application transaction. |
| **LINK-06** | A directed advertisement is accepted only by its logical next hop. Flooded control is explicitly marked and uses controlled admission/dedupe; no valid-route application DATA falls back to broadcast flooding. |

## AODV base route engine

| ID | Normative requirement |
| --- | --- |
| **AODV-01** | The fixed-capacity route table stores logical destination, next hop, hop count, serial validity/value, lifetime, state (`VALID`, `INVALID`, `IN_SEARCH`), and bounded precursors. It answers reachability only and is independent of GTT membership; AODV_ONLY keys routes by AdvA-derived SID16, while FULL_TAVRN keys fixed-k routes by SID8 backed by full-identity GTT context. |
| **AODV-02** | Every expanding-ring RREQ transmission allocates a fresh request ID and deduplicates `{selected logical origin namespace, request ID}`. Local discovery state correlates all ring IDs for one destination off-wire and accepts a matching RREP against any still-active ring. Receive processing establishes a reverse route, rejects immediate next-hop loops, increments hop count once, and forwards only within the request TTL and controlled-flood policy. |
| **AODV-03** | RREP processing installs or replaces a route only for a newer destination serial, or for equal serial with an invalid route or strictly shorter path. It establishes forward routes and forwards toward the recorded reverse route without changing the destination serial incorrectly. |
| **AODV-04** | Unknown destinations use expanding-ring TTL `1,3,5,7`, then `timer.aodv_net_diameter`; each transmission uses the fresh request ID required by `AODV-02`. RREQ/RERR origination obey `timer.aodv_rreq_rate`/`timer.aodv_rerr_rate`. All profiles currently use diameter 15 and rate 10/s as table values. A fresh GTT estimate may replace only the first ring under `GTT-06`; later fallback remains locally correlated. |
| **AODV-05** | Locally originated DATA without a route enters the bounded pending queue and triggers one route search per destination. Queue full/expiry has explicit failure evidence; DATA is neither leaked to another destination nor delivered through legacy flood. |
| **AODV-06** | Route break processing invalidates all routes using the failed next hop, increments/retains destination serials with half-range rules, and sends bounded RERR to precursors. Unreachable entries are sorted by logical destination (numeric SID16 in AODV_ONLY; resolved canonical identity in FULL_TAVRN), segmented to at most three SID16 or four SID8 entries per wire frame, and consume at most eight pending AODV actions. External overflow backpressures before mutation; timer/link-driven overflow remains explicitly due and emits a counter/event, never truncates silently. For DATA-link evidence, only `RETRY_EXHAUSTED` invokes this path. |
| **AODV-07** | Intermediate RREP generation may request `E_RREP_ACK` for the AODV unidirectional-link condition. The exact correlation tuple includes network, expected outer AdvA, identity width/receiver, route destination, **destination sequence**, RREQ origin, and request ID; wait and blacklist use `timer.aodv_rrep_ack_wait_ms` and `timer.aodv_blacklist_ms`. Missing ACK blacklists only that return neighbor and alone emits neither RERR, TC LEAVE, DATA `RETRY_EXHAUSTED`, nor local repair. |
| **AODV-08** | The single `aodv_core` exposes a typed deferred-RERR command for optional local repair. Begin immediately invalidates routes through the failed next hop and records one bounded repair token while withholding only the affected precursor RERR; success cancels that deferred destination, while timeout, disabled repair, capacity failure, or explicit release resumes normal segmented RERR. Repair calls this public command/action seam and never edits a route table, stores a second route, or creates another discovery engine. |

## Passive GTT and Smart TTL

| ID | Normative requirement |
| --- | --- |
| **GTT-01** | GTT is a passive, fixed-capacity membership store owned by the router. It schedules no timer, transmits no frame, owns no route or send decision, and is the direct canonical-context query surface for applications. |
| **GTT-02** | GTT includes canonical identity, latest serial, last evidence/deadlines, hop estimate, departed state, directness with last direct evidence time, one-shot application-verification state, and a checked revision. `tavrn_gtt_storage_t` owns a table-global nonzero monotonic generation; every semantic mutation or replacement—including raw imported-deadline adjustment, serial clear, request/cancel, directness change, and departure—receives its next revision, skipping zero on wrap. Duplicate/no-op operations do not revise. Central GTT APIs perform all raw mutation. `tavrn_gtt_sync_merge` accepts records and time only: GTT atomically allocates nonzero monotonic revisions/generation for every changed row and candidate commit, duplicate bulk merge is a no-op, and no caller may jump generation. Active SYNC preserves transferred hard lifetime without jitter and clamps soft to `min(local_soft,remaining_hard)`; departed SYNC sets fixed local retention. Mentorship calls this centralized merge and never writes raw candidate deadlines/generation/storage. At capacity GTT replaces the oldest departed entry, then the oldest stale entry, but never self or fresher active state; if no legal victim exists it rejects the new member with a counter. |
| **GTT-03** | Valid passive evidence refreshes only identities actually evidenced by frame role: full immediate transmitter/previous hop, semantically admitted origin/destination fields, and accepted topology records. Imported evidence is non-direct by default. `DIRECT_HELLO`, `DIRECT_BOOTSTRAP`, `DIRECT_INCARNATION`, and distinct `DIRECT_OUTER_TRANSMITTER` provenance can establish directness only for that exact locally observed full AdvA at hop 1; the latter is committed generic non-HELLO outer-transmitter evidence, accepts serial-absent liveness using canonical zero serial hash bytes while preserving an existing latest stored serial/presence, and never claims payload origin/destination/other identities direct. A newly created serial-absent row may remain serial-absent. Imported metadata, SYNC pages, TC subjects, JOIN, and hop estimates cannot establish directness. Confirmed departure clears directness; imported resurrection remains non-direct, while direct resurrection sets the direct-evidence time; a new direct incarnation replaces that time. The table stores that time, never a fixed direct-departure deadline. Same-timestamp admitted direct RX is processed before the maintenance sweep, and direct-deadline equality is due. Malformed, ambiguous, stale, unordered, rejected, hop-not-one, duplicate/noncommitted, or merely overheard addressed DATA is not liveness evidence for its payload identities. |
| **GTT-04** | GTT merges use `SERIAL-02`/`SERIAL-03`; accepted equal serial may refresh evidence but cannot worsen a known hop estimate without explicit departure/rejoin. Equality is due for expiry. Departed entries remain tombstones through `timer.gtt_departed_ms`, derived as `2 * timer.gtt_hard_expiry_ms`, and can be resurrected only by valid fresh evidence or the reboot rule. |
| **GTT-05** | Applications access the direct canonical GTT API only on the single mesh-owner lane. Other tasks marshal by-value application GTT query/request/cancel commands through a capacity-one FULL mailbox and receive copied results; mutable GTT storage is never exposed. Its only states are empty, command pending, owner processing, and result ready; submit is BUSY except from empty, repeated owner take while processing/result-ready is BUSY, owner take is legal only from pending, publish only from processing, and consumer take only from ready. INVALID is reserved for null/invalid arguments and wrong publish transitions; all rejected transitions preserve the first transaction. The one-shot request is the only application demand state in this slice. The existing narrower active enumeration remains separate. The known/nondeparted enumeration and query return every retained nondeparted member, including a hard-expired demanded member, with a copied freshness snapshot and canonical AdvA. Membership means “known to exist,” not “has a valid route,” and route expiry does not erase membership. |
| **GTT-06** | Ordinary route discovery uses Smart TTL only for a non-departed, non-hard-expired GTT entry with positive hop estimate: initial RREQ scope is `min(hop+2, net diameter)`. The retained positive hop estimate of a demanded, non-departed hard-expired subject is usable only for `MAINT-06` stage 1 verification. Failure falls back to full-diameter discovery; GTT never suppresses that fallback. |

## Fixed-k ESC and mentorship

| ID | Normative requirement |
| --- | --- |
| **ESC-01** | `FULL_TAVRN` implements ESC at fixed `k=1` only, enabled after successful mentorship/self-bootstrap and only after the AODV_ONLY hardware gate. Dynamic entropy, sticky decay, multi-byte suffix modes, and ambiguity-RERR recovery are roadmap-only. |
| **ESC-02** | Fixed-k decompression uses the local GTT as context and succeeds only on one canonical-identity match. Unknown and multiple matches are failures; neither can create route, GTT, custody, or application state. |
| **ESC-03** | Full canonical identities are used for bootstrap/collision checks; one-byte IDs are used only after context establishment. A node losing valid context returns to full-identity bootstrap instead of guessing or widening `k` dynamically. |

| ID | Normative requirement |
| --- | --- |
| **BOOT-01** | Every routed node sends full-identity `HELLO(N=1)` with the nonzero boot nonce from `SERIAL-04` in the HELLO node-sequence field. Mentor and mentee fields in offers and membership identity in sync pages remain full identity throughout FULL_TAVRN bootstrap. |
| **BOOT-02** | Bootstrapped neighbors interpolate offer delay from `timer.mentor_rssi_weak_magnitude_db`/`timer.mentor_rssi_weak_delay_ms` to `timer.mentor_rssi_strong_magnitude_db`/`timer.mentor_rssi_strong_delay_ms`, then add `timer.mentor_jitter_min_ms..timer.mentor_jitter_max_ms`. Overheard valid offers cancel pending offers for that mentee; duplicate offers use `timer.mentor_offer_suppression_ms`. BALANCED examples are -90 dBm→500 ms, -30 dBm→10 ms, 0..50 ms jitter, and 10 s suppression only. |
| **BOOT-03** | The mentee collects offers for `timer.mentor_offer_window_ms`, then chooses largest advertised GTT, strongest observed RSSI, and lexicographically lowest full mentor identity. BALANCED uses 2 s; FAST_TEST/SOAK use their table values. This total order removes source iteration-order dependence. |
| **BOOT-04** | The selected mentor creates one immutable, canonical-identity-sorted snapshot of at most `TAVRN_MAX_NODES` entries for the session. Join/leave mutation during paging applies after the snapshot and never changes page indices or total count. |
| **BOOT-05** | Each nonempty BLE SYNC_DATA page carries exactly one complete snapshot record with full AdvA, 16-bit serial, active/departed TTL bucket, and four-bit mentor hop estimate; an empty snapshot has one explicit empty final page. `lastSeen` is not transferred: receipt time becomes local evidence. Pull index/count and the SYNC_OFFER snapshot total are bounds checked; the mentee stores `min(15, mentor_hop+1)`. |
| **BOOT-06** | `tavrn_router` owns routed-common incarnation establishment. AODV_ONLY repeats its reboot announcement during `timer.router_reboot_announce_ms`, then self-establishes without GTT/mentorship. FULL_TAVRN continues mentorship; each pull uses `timer.mentor_page_timeout_ms` and at most `timer.mentor_page_attempts`, then clears mentor state and restarts HELLO. With no usable offer by `timer.mentor_self_bootstrap_ms`, FULL self-bootstraps with self only and originates JOIN. |

## Maintenance, topology metadata, and local repair

Current implementation status is narrower than the complete maintenance
requirements below: Phase 5 has post-SID8 adaptive ordinary HELLO cadence,
bounded equality dedupe, passive one-hop GTT observation, copied telemetry, and
local-broadcast suppression. Cadence begins at change, advances by deterministic
EMA/snap, and a sampled one-hop gain/loss resets it while retaining a decaying
three-times-old-interval liveness floor. A committed direct-peer nonce starts a
fresh local ordinary-HELLO serial/dedupe epoch. It does not claim expiry,
verification, metadata, or expiry-driven TC behavior. Existing mentorship
bootstrap JOIN encoding, origination, relay, and dedupe remain accepted and
unchanged.

The complete liveness design deliberately combines complementary mechanisms:
passive learning from semantically admitted DATA/control, mentorship snapshot
bootstrap, adaptive direct-neighbor HELLO hysteresis, later TC JOIN/LEAVE, and
resurrection from valid fresh evidence. Soft expiry is social freshness, not a
HELLO keepalive; hard expiry is demand-gated verification. In particular,
ordinary HELLO is never flooded.

| ID | Normative requirement |
| --- | --- |
| **MAINT-01** | GTT soft/hard expiry and maintenance use `timer.gtt_soft_expiry_ms`, `timer.gtt_hard_expiry_ms`, and `timer.gtt_maintenance_ms`; departed retention uses `timer.gtt_departed_ms`. On each ordinary accepted remote liveness refresh, compute deterministic TAVRN-style desynchronization with FNV-1a 32-bit: start offset `2166136261`; for every fed byte, XOR then multiply modulo `2^32` by prime `16777619`; feed local AdvA bytes 0..5, subject AdvA bytes 0..5, serial-present byte exactly `0` or `1`, serial little-endian, then exactly four little-endian `now_ms` bytes. When serial is absent, hash two canonical zero serial bytes regardless of ignored struct contents. Set `jitter_ms = hash % (floor(hard_expiry_ms/6)+1)`, `hard_offset = hard_expiry_ms + jitter_ms`, `soft_offset = floor(hard_offset * soft_expiry_ms / hard_expiry_ms)` using 64-bit arithmetic, and deadlines from `now_ms` plus those offsets. SYNC bulk import/merge preserves its transferred remaining-lifetime bucket and adds no jitter; departed retention and local departure remain fixed and unjittered. This deterministic embedded adaptation preserves TAVRN's `uniform(0,TTL/6)` desynchronization intent. Validated same-timestamp request/cancel commands are consumed before router tick and sweep. Each due cadence then makes one bounded deterministic slot-order pass of at most 16 entries after router tick and before topology sampling: every occupied retained slot is visited at most once, and it copies/processes only one soft/hard candidate at a time, never a stacked table snapshot, so demanded candidates cannot starve later undemanded candidates. Local soft-expiry work only exposes/selects copied soft-stale state; it emits no metadata or control in this slice. BALANCED examples are 150/300 s soft/hard with 25 s maintenance and 600 s departed retention only. FAST_TEST/SOAK remain table-authoritative. |
| **MAINT-02** | Periodic HELLO adapts from `timer.hello_change_ms` to `timer.hello_stable_ms` using `timer.hello_alpha` and `timer.hello_snap_ratio`, resets to change cadence on one-hop gain/loss, and suppresses/defers after recent broadcast. An ordinary periodic HELLO is exactly `N=0,T=0,Q=0,M=0`: it is direct one-hop only with `TTL=1,hops=0`, full origin equal to outer immediate transmitter, no relay, and no reply; it refreshes direct-neighbor evidence only. Bootstrap `N=1,T=0,Q=0,M=0` remains direct one-hop and non-relayed with the same origin rule, and may elicit the separate mentorship `SYNC_OFFER` control, never a HELLO reply. BALANCED examples are 24/120 s and alpha 0.8 only. Direct-neighbor liveness uses `max(3 * current HELLO interval, decaying hysteresis floor)` calculated from the stored last direct evidence time. A direct neighbor is eligible for local departure only when both hard expiry and that calculated direct-evidence deadline are due; equality is due, while a saturated or unrepresentable deadline fails closed. Imported hop counts do not satisfy either condition. |
| **MAINT-03** | The timer registry below is the exact key-and-value authority: every row has one unique manifest key and owning module, and its key set must equal the architecture registry set exactly; aliases, architecture-only keys, profile-only keys, missing keys, and duplicates are checker/configure errors. Active-route and maintenance values use `timer.aodv_active_route_ms` and `timer.gtt_maintenance_ms`; all traversal, GTT, verification, mentorship, repair, scheduler, and link-wall relationships are derived below. |
| **MAINT-04** | Demand is a per-subject reason-bearing snapshot, not a boolean. Final-destination reasons compare the subject logical ID: pending or queued DATA, live link custody, router pending ingest, and retained DATA action. Next-hop reasons compare logical ID and, when retained, canonical AdvA: an unexpired valid route, live link custody, and applicable pending/retained DATA. A precursor compares the subject logical ID on an unexpired valid route; deferred repair compares repair destination. The snapshot also includes an unexpired valid route whose destination is the subject (`valid_route_to_subject`, the only later stage-0 eligibility) and the GTT one-shot application-verification request. Expired-at-equality owners do not count; pending/retained occupancy alone does not count. Discovery slots, scheduler copies, unpinned dedupe, historical delivery, and GTT occupancy do not count. In FULL, maintenance resolves canonical AdvA to one SID8 through GTT/ESC, then asks generic router/AODV/link state by logical ID plus canonical next-hop identity; those generic components remain GTT-independent. Checked departure synchronously recomputes that current aggregate immediately before mutation and cannot receive a caller demand snapshot; a route/custody/router owner created after an earlier no-demand read therefore defers. Snapshot-unavailable or fail-stop state defers departure. An application requests the one-shot directly through its canonical GTT context on the single mesh-owner lane: duplicate request is idempotent and does not revise, while self, missing, and departed subjects are rejected. Accepted same-subject liveness clears the request when its present serial is equal or newer, or when valid liveness has no serial; stale, unordered, or rejected evidence does not. Imported or confirmed departure, checked local departure, replacement, purge, reinitialization, or explicit cancel clears it. A non-no-op request/cancel mutation increments the entry revision and is itself a demand reason. |
| **MAINT-05** | A demanded hard-expired entry remains deferred for later verification; a hard-expired entry with no demand is marked departed locally without verification transmission or TC-UPDATE flood. The checked local-departure mutation requires the copied subject's same canonical identity and revision, a current hard expiry that is due, and a nonself nondeparted entry; a direct subject also requires the `MAINT-02` direct-evidence deadline. The table assigns every semantic mutation or replacement a nonzero per-entry revision from a table-global monotonic generation, skipping zero on wrap. In an armed post-tick, sweep runs before topology/HELLO, so a same-timestamp direct departure changes public direct count and topology reset/cadence in that same tick. A checked local departure preserves last real evidence, serial, and hop, clears directness and application request, sets fixed departed retention, and never masquerades as received evidence. Later valid evidence may resurrect it under `GTT-04`. Existing mentorship bootstrap JOIN remains unchanged; local checked departure originates no LEAVE. |
| **MAINT-06** | A demanded hard-expiry verification has exactly three total stages, never an inner retry loop: `valid_route_to_subject` is the only demand reason that enables stage 0, which revalidates an unexpired valid route to that subject immediately before admission and then sends one route-assisted targeted freshness request. Route loss skips stage 0 unconsumed; other demand also skips it without transmission. Stage 1 sends exactly one verification RREQ scoped to the retained hard-expired hop estimate plus two, capped at diameter; stage 2 sends exactly one full-diameter verification RREQ with a fresh request ID. Missing or zero retained hop skips stage 1 unconsumed and proceeds directly to stage 2 without synthesizing a hint. If retained hop plus two reaches diameter, stage 1 still sends its one fresh-ID diameter RREQ and stage 2 sends a second fresh-ID full-diameter RREQ. A consumed stage 0 waits at most `timer.aodv_net_traversal_ms`; each consumed RREQ stage waits at most `timer.aodv_path_discovery_ms`. Stage 1 and stage 2 each allocate their own fresh request ID and obey `timer.aodv_rreq_rate`. If stage-2 remote silence finishes before a direct subject's `MAINT-02` deadline, its FSM enters `WAIT_DIRECT_DEADLINE` without further traffic, remains active, and is canceled only by valid liveness evidence for that same canonical subject; checked departure occurs only at that deadline. Otherwise, only exhausted stage-2 remote silence may mark the subject departed. Only valid liveness evidence for the FSM's same canonical subject cancels it; evidence for another member never does. |
| **MAINT-07** | The fixed four new/active verification contexts, tracked nonzero-mask `TX_DONE` stage consumption, and local-admission retry behavior belong exclusively to the later targeted-freshness and retained-hop/full-RREQ RED slices. They are not implemented by the local-expiry/demand slice. When that later work is accepted, its limits remain `timer.verification_new_cap` and `timer.verification_active_cap`, existing progress including `WAIT_DIRECT_DEADLINE` remains active, and local BUSY, rate, capacity, or zero-channel `TX_FAILED` remains unconsumed and cannot authorize departure. |
| **MAINT-08** | Confirmed JOIN/LEAVE uses TTL-decremented TC-UPDATE gossip with UUID `{full origin identity, 16-bit serial}` retained by `timer.tc_uuid_ms` and subject key `{full subject identity,event}` retained by `timer.tc_subject_ms`. BALANCED examples are 30 s and 1 s only. Forwarding is implicit gossip acknowledgment; busy/rejected/enqueue/expiry never originates LEAVE. |

### Timer registry

All values below are generated build inputs and manifest fields. Durations are
milliseconds unless marked count, rate, or ratio. `FAST_TEST` is for deterministic
host/bench gates, `BALANCED` is the normal PoC profile, and `SOAK` extends idle
membership/logging while retaining BALANCED traversal behavior. Every configured
duration is below the 32-bit half range.

| Meaning | Manifest key | Owner | FAST_TEST | BALANCED | SOAK |
| --- | --- | --- | ---: | ---: | ---: |
| Scheduler channel dwell | `timer.scheduler_dwell_ms` | shared scheduler | 50 | 50 | 50 |
| Scheduler relay spacing | `timer.scheduler_relay_spacing_ms` | shared scheduler | 200 | 200 | 200 |
| Scheduler custody bypass maximum (count) | `timer.scheduler_custody_bypass_max` | shared scheduler | 2 | 2 | 2 |
| Dedicated mesh scheduler poll maximum | `timer.scheduler_poll_max_ms` | scheduler/mesh loop | 2 | 2 | 2 |
| Radio state wait timeout | `timer.radio_state_timeout_ms` | radio driver/scheduler | 2 | 2 | 2 |
| Radio TX event bound (derived) | `timer.radio_tx_event_bound_ms` | radio driver/scheduler | 8 | 8 | 8 |
| Legacy relay jitter minimum | `timer.legacy_relay_min_ms` | legacy flood | 20 | 20 | 20 |
| Legacy relay jitter maximum | `timer.legacy_relay_max_ms` | legacy flood | 120 | 120 | 120 |
| Legacy dedupe retention | `timer.legacy_dedupe_ms` | legacy flood | 10000 | 10000 | 10000 |
| Legacy PING interval | `timer.legacy_ping_interval_ms` | legacy ping/pong | 2000 | 2000 | 2000 |
| Legacy PING timeout | `timer.legacy_ping_timeout_ms` | legacy ping/pong | 1500 | 1500 | 1500 |
| HACK response deadline | `timer.link_hack_timeout_ms` | routed link-v2 | 250 | 250 | 250 |
| HACK maximum attempts (count) | `timer.link_max_attempts` | routed link-v2 | 3 | 3 | 3 |
| HACK retry backoff | `timer.link_retry_backoff_ms` | routed link-v2 | 0 | 0 | 0 |
| Eligibility-to-TX_DONE attempt bound (derived) | `timer.link_tx_scheduler_attempt_bound_ms` | scheduler/link-v2 | 30 | 30 | 30 |
| HACK response-window sum (derived) | `timer.link_response_window_sum_ms` | routed link-v2 | 750 | 750 | 750 |
| No-response wall bound (derived) | `timer.link_no_response_wall_bound_ms` | routed link-v2 | 840 | 840 | 840 |
| RX candidate defensive resolution | `timer.link_candidate_resolve_ms` | routed link-v2/router | 10 | 10 | 10 |
| BUSY backoff | `timer.link_busy_backoff_ms` | routed link-v2 | 500 | 500 | 500 |
| BUSY maximum responses (count) | `timer.link_busy_max_responses` | routed link-v2 | 3 | 3 | 3 |
| Selected-next-hop DATA deadline | `timer.link_data_deadline_ms` | routed link-v2 | 5000 | 5000 | 5000 |
| DATA equality-dedupe retention minimum | `timer.link_data_dedupe_ms` | routed link-v2 | 10000 | 10000 | 10000 |
| FLOOD equality-dedupe retention | `timer.link_flood_dedupe_ms` | routed link-v2 | 10000 | 10000 | 10000 |
| Routed flood jitter minimum | `timer.link_flood_jitter_min_ms` | routed link-v2 | 20 | 20 | 20 |
| Routed flood jitter maximum | `timer.link_flood_jitter_max_ms` | routed link-v2 | 120 | 120 | 120 |
| AODV node traversal | `timer.aodv_node_traversal_ms` | AODV core | 10 | 40 | 40 |
| AODV net diameter (count) | `timer.aodv_net_diameter` | AODV core | 15 | 15 | 15 |
| AODV net traversal (derived) | `timer.aodv_net_traversal_ms` | AODV core | 300 | 1200 | 1200 |
| AODV path/RREQ response window (derived) | `timer.aodv_path_discovery_ms` | AODV core | 600 | 2400 | 2400 |
| RREQ equality-dedupe retention | `timer.aodv_rreq_seen_ms` | AODV core | 10000 | 10000 | 10000 |
| AODV RREQ retries (count) | `timer.aodv_rreq_retries` | AODV core | 2 | 2 | 2 |
| AODV RREQ origin rate (per second) | `timer.aodv_rreq_rate` | AODV core | 10 | 10 | 10 |
| AODV RERR origin rate (per second) | `timer.aodv_rerr_rate` | AODV core | 10 | 10 | 10 |
| AODV active route | `timer.aodv_active_route_ms` | AODV core | 36000 | 360000 | 720000 |
| AODV pending DATA | `timer.aodv_pending_data_ms` | AODV core | 5000 | 30000 | 60000 |
| AODV unidirectional blacklist (derived) | `timer.aodv_blacklist_ms` | AODV core | 600 | 2400 | 2400 |
| E_RREP equality-dedupe retention | `timer.aodv_rrep_dedupe_ms` | AODV core | 10000 | 10000 | 10000 |
| E_RERR equality-dedupe retention | `timer.aodv_rerr_dedupe_ms` | AODV core | 10000 | 10000 | 10000 |
| E_RREP_ACK wait | `timer.aodv_rrep_ack_wait_ms` | AODV core | 250 | 250 | 250 |
| GTT soft expiry | `timer.gtt_soft_expiry_ms` | GTT/maintenance | 15000 | 150000 | 300000 |
| GTT hard expiry | `timer.gtt_hard_expiry_ms` | GTT/maintenance | 30000 | 300000 | 600000 |
| GTT departed retention | `timer.gtt_departed_ms` | GTT/maintenance | 60000 | 600000 | 1200000 |
| GTT maintenance interval | `timer.gtt_maintenance_ms` | GTT/maintenance | 2500 | 25000 | 50000 |
| HELLO change interval | `timer.hello_change_ms` | router/maintenance | 2400 | 24000 | 60000 |
| HELLO stable interval | `timer.hello_stable_ms` | router/maintenance | 12000 | 120000 | 300000 |
| HELLO EMA alpha (ratio) | `timer.hello_alpha` | router/maintenance | 0.8 | 0.8 | 0.8 |
| HELLO snap ratio | `timer.hello_snap_ratio` | router/maintenance | 0.95 | 0.95 | 0.95 |
| HELLO equality-dedupe retention | `timer.hello_dedupe_ms` | routed router | 10000 | 10000 | 10000 |
| Routed reboot-announcement window | `timer.router_reboot_announce_ms` | routed router | 1000 | 3000 | 3000 |
| Freshness-response delay minimum | `timer.freshness_response_min_ms` | maintenance | 10 | 10 | 10 |
| Freshness-response delay maximum | `timer.freshness_response_max_ms` | maintenance | 100 | 100 | 100 |
| Verification total bound (derived) | `timer.verification_window_ms` | maintenance | 1500 | 6000 | 6000 |
| Verification new-start cap (count) | `timer.verification_new_cap` | maintenance | 4 | 4 | 4 |
| Verification active cap (count) | `timer.verification_active_cap` | maintenance | 4 | 4 | 4 |
| Mentor offer-collection window | `timer.mentor_offer_window_ms` | mentorship | 500 | 2000 | 2000 |
| Mentor page timeout (derived) | `timer.mentor_page_timeout_ms` | mentorship | 600 | 2400 | 2400 |
| Mentor page attempts (count) | `timer.mentor_page_attempts` | mentorship | 3 | 3 | 3 |
| Mentor no-offer self-bootstrap | `timer.mentor_self_bootstrap_ms` | mentorship | 1000 | 3000 | 3000 |
| SYNC equality-dedupe post-completion retention | `timer.mentor_sync_dedupe_ms` | mentorship | 10000 | 10000 | 10000 |
| Mentor weak-RSSI magnitude endpoint (dB) | `timer.mentor_rssi_weak_magnitude_db` | mentorship | 90 | 90 | 90 |
| Mentor strong-RSSI magnitude endpoint (dB) | `timer.mentor_rssi_strong_magnitude_db` | mentorship | 30 | 30 | 30 |
| Mentor weak-RSSI delay endpoint | `timer.mentor_rssi_weak_delay_ms` | mentorship | 500 | 500 | 500 |
| Mentor strong-RSSI delay endpoint | `timer.mentor_rssi_strong_delay_ms` | mentorship | 10 | 10 | 10 |
| Mentor offer jitter minimum | `timer.mentor_jitter_min_ms` | mentorship | 0 | 0 | 0 |
| Mentor offer jitter maximum | `timer.mentor_jitter_max_ms` | mentorship | 50 | 50 | 50 |
| Mentor duplicate-offer suppression | `timer.mentor_offer_suppression_ms` | mentorship | 10000 | 10000 | 10000 |
| Topology metadata cooldown | `timer.metadata_cooldown_ms` | metadata/maintenance | 1000 | 5000 | 10000 |
| TC UUID dedupe | `timer.tc_uuid_ms` | topology maintenance | 5000 | 30000 | 60000 |
| TC subject dedupe | `timer.tc_subject_ms` | topology maintenance | 1000 | 1000 | 1000 |
| Local repair total timeout (derived) | `timer.repair_timeout_ms` | optional repair | 1700 | 5300 | 5300 |
| Local repair cooldown | `timer.repair_cooldown_ms` | optional repair | 1000 | 1000 | 1000 |
| Statistics interval | `timer.stats_ms` | firmware loop | 1000 | 5000 | 30000 |
| Firmware loop delay | `timer.loop_delay_ms` | firmware loop | 2 | 2 | 2 |

Derived relationships are normative: net traversal is
`2 * 15 * node_traversal`; path discovery is `2 * net_traversal`; blacklist is
`2 * net_traversal`; active route is `1.2 * GTT hard expiry`; departed retention
is `2 * GTT hard expiry`; maintenance interval is
`max(1000, GTT hard expiry / 12)`. Verification total is one net traversal plus
two path-discovery windows: `net_traversal + 2 * path_discovery`, therefore
1500 ms FAST_TEST and 6000 ms BALANCED/SOAK. It bounds remote silence only after
the first consumed probe; local admission failure suspends it and cannot mark a
subject departed. Mentor page timeout is `2 * net_traversal`. Local
repair timeout is `2 * path_discovery + 500`. The RREQ equality cache is retained
for `max(path_discovery, 10000)`, so its actual manifest value is 10000 in all
three profiles even though the path/RREQ response window is 600/2400 ms. DATA
dedupe remains live beyond its 10000 ms minimum while custody is held; SYNC
dedupe remains live through the active session plus the listed post-completion
retention. Radio TX event bound is
`(1 pre-disable + 3 selected-channel waits) * radio_state_timeout = 8`.
Eligibility-to-TX_DONE bound is
`(scheduler_custody_bypass_max + 1) * (radio_tx_event_bound + scheduler_poll_max) = 30`.
`timer.scheduler_poll_max_ms` is the maximum from completion of one bounded
scheduler operation to the start of the next scheduler poll in the dedicated
mesh loop; it is not `timer.loop_delay_ms`, which remains a generic firmware-loop
setting and is not used in this service proof. The testbed's preemptible logger
yield uses only the operation-return gap still unconsumed by healthy-cycle mesh
work; it is skipped when that work has spent the voluntary-yield budget.
HACK response-window sum is `3 * 250 = 750`; no-response wall bound is
`response_window_sum + 3 * link_tx_scheduler_attempt_bound = 840`.
`hack_turnaround_ms` is a validated link-config field set from the existing
8 ms radio TX-event bound, not a new timer-table row or timer formula input.
Configure-time validation recomputes every derived row, rejects min/max or
timeout-order violations, and requires exact timer-key set equality between this
table, architecture, generated configuration, and artifact manifest.

| ID | Normative requirement |
| --- | --- |
| **META-01** | Metadata candidate selection is capped by the fixed four-entry capacity, prioritizes soft-expired freshness requests, uses a round-robin cursor, and applies `timer.metadata_cooldown_ms` per entry. A soft request asks social peers when they last saw its subject; it starts no control by itself. A social answer is a request-clear entry on an otherwise eligible E_RREQ/E_RREP/E_RERR, never a targeted HELLO. The target's own evidence outranks an intermediary's; an intermediary may advertise only materially fresher evidence under the tiered policy in the wire contract. A delayed tier-2 targeted response reuses one candidate slot; if no slot is free it is suppressed rather than replacing a live entry or discovering a route. BALANCED cooldown is 5 s only; FAST_TEST/SOAK remain table-authoritative. The four-entry cap resolves v2.2 prose value 5 versus source value 4. |
| **META-02** | Metadata may be emitted only in fixed-k SID8 E_RREQ/E_RREP/E_RERR. Static capacities are RREQ 4, RREP 3, and RERR 4/3/1/0 for 1/2/3/4 unreachable destinations; SID16 has zero slots. Emitted count is `min(candidate count, static capacity)`; zero-slot control frames remain valid. No encoder truncates base control fields to make room. |
| **META-03** | BLE DATA has zero topology-metadata slots. HELLO has zero general slots, except the targeted verification request and its distinct targeted freshness response each carry exactly one subject entry. E_RREP_ACK, SYNC_*, and TC-UPDATE carry no general metadata. |
| **META-04** | Metadata is attributed to the explicit immediate transmitter, bounds checked before merge, and consumed by the routing layer before application delivery. The admitted targeted freshness-response exception attributes its one subject claim to the preserved full origin/evidence source; outer AdvA remains direct evidence only for the immediate relay. A newly originated targeted freshness request or response (`hops=0`) requires full origin equal to outer AdvA; relays preserve that origin. Unsupported or excess metadata rejects the frame rather than partially applying entries. |

| ID | Normative requirement |
| --- | --- |
| **REPAIR-01** | Local repair is a separately switchable TAVRN v2.3 extension of `FULL_TAVRN`, disabled for the initial full-profile proof. It adds no message type, header field, or steady-state traffic and never replaces AODV. |
| **REPAIR-02** | Repair starts only from routed-link `RETRY_EXHAUSTED` for a transit DATA frame whose custody is held by the repairer. Locally originated DATA, control loss, busy/rejected HACK, and queue failure use existing paths and do not start repair. |
| **REPAIR-03** | On failed next hop H toward destination D, H is the failed neighbor and D remains unknown/alive unless separately proven departed. Routes via H are invalidated; LEAVE may name H, never D solely because H failed. |
| **REPAIR-04** | Repair state is fixed at one concurrent destination and four buffered transit DATA frames. It owns immutable DATA context; limit or buffer excess immediately returns to the normal AODV RERR/failure path without duplicate custody or silent overwrite. |
| **REPAIR-05** | One repair transaction invokes `AODV-08`, sends at most one Smart-TTL RREQ then one full-scope RREQ, and allows no inner retry loop. Total timeout uses derived `timer.repair_timeout_ms`; table values are 1700 ms FAST_TEST and 5300 ms BALANCED/SOAK. Success installs the alternate route through the same core, cancels deferred RERR, and flushes each buffered DATA exactly once; timeout/failure releases RERR and drops buffers. |

## Fixed capacities and build evidence

`BUILD-01` freezes these logical capacities. A derived architecture may allocate
more bytes for alignment, but may not introduce heap allocation or change the
observable limits without revising this profile.

| Resource | Capacity | Deterministic full policy |
| --- | ---: | --- |
| Raw AdvData / routed custom PDU | 31 / 24 bytes | Reject before driver truncation. |
| Public DATA application bytes | 10 bytes | SID16 encoder admits at most 7; SID8 opaque admits at most 10; patient kind `01` requires exactly 7. |
| Scheduler physical TX queue | 4 items | Strict-priority admission; return full or the synchronously evicted lower-priority token. |
| Routed link custody TX | 4 DATA | `SEND_NO_SLOT`; caller retains input. |
| Link accepted/duplicate cache | 16 keys | Purge expired, then deterministic oldest; never duplicate-deliver. |
| AODV routes | 16 destinations | Purge expired/invalid; reject a worse/new route rather than evict active fresher state. |
| AODV RREQ seen cache | 32 keys | Purge expired, then deterministic oldest. |
| AODV pending application/transit DATA | 8 items | Return busy or normal RERR/repair fallback; never overwrite. |
| AODV precursor IDs | 8 per route | Ignore duplicate; retain existing set and count truncation. |
| Pending E_RREP_ACK waits | 4 exact tuples | Backpressure E_RREP action before setting ACK-required/sending; never transmit an untracked ACK request. |
| AODV pending action/RERR segment queue | 8 actions total | Apply `AODV-06`: reject external input before persistent mutation; internal due work remains explicit and retries next tick. |
| Router failure obligations | 16 peers plus 1 preserved overflow event | Retry the oldest first with wrap-safe admission order; coalesce exact peers. A seventeenth event is copied into the one overflow slot and latches router fail-stop rather than being released silently. |
| Router post-ACK accepted DATA / delivery reservation / retained AODV action | 1 / 1 / 1 | Retain exact copied state until completion. A delivery-token/action mismatch, non-BUSY post-ACK ingest failure, or required transit dedupe-release failure latches fail-stop without retrying callbacks. |
| GTT membership | 16 entries in the Phase 3 `FULL_TAVRN` build; not implemented elsewhere | Apply `GTT-02`; never overwrite fresher active state. |
| Active mentorship session / frozen snapshot | 1 session / 16 entries | Busy/suppress a second session; reject snapshot creation if local GTT cannot fit; never replace an active session. |
| Mentorship competing offers | 8 | Retain the best offers under `BOOT-03`; count overflow. |
| Mentorship JOIN dedupe | 16 keys | Purge expired entries, then deterministically replace the oldest live key and count replacement. |
| Pending mentorship JOIN obligations | 3 controls | Coalesce exact JOINs; return busy and count overflow rather than overwrite retained work. |
| Completed mentorship SYNC dedupe | 8 session tuples | Purge expired tuples; preserve every unexpired tuple and backpressure final-page commit when full. |
| Maintenance equality dedupe | 16 keys | Phase 5 ordinary HELLO: purge expired, then reject/backpressure a new live key rather than overwrite equality protection. Later targeted freshness request/response keys use the same fixed store; TC use remains later work. |
| Maintenance peer boot epochs | 16 full-AdvA/nonce records | Retain each router-committed direct peer nonce; a first/new nonce clears only that nonself GTT serial and HELLO equality keys before N=0 admission. |
| Active verification FSMs | 4 | Each retains subject, demand, retained hop, current stage, and tracked control token; defer new work. |
| Metadata candidates | 4 | Round-robin selection; soft social request/reply and delayed tier-2 response state use this fixed store, and wire capacity may emit fewer. |
| Local repair | 1 destination, 4 DATA | Immediate normal AODV RERR/failure path for excess. |
| Node/router application event queue | 8 events | Backpressure producer; never overwrite silently. |

| ID | Normative requirement |
| --- | --- |
| **BUILD-01** | Radio, routed link, AODV, router, and implemented GTT paths use the fixed capacities and full policies above with no heap allocation. In particular, four exact E_RREP_ACK waits, eight total AODV action/RERR segments, sixteen ordered router failure obligations plus one preserved fail-stop overflow event, one post-ACK DATA slot, one delivery reservation, and one retained action cannot silently truncate, overwrite, or create untracked protocol work. |
| **BUILD-02** | Generic/runtime-FICR builds put exact sentinel `RUNTIME_FICR` in every build-time AdvA/SID16/SID8/fleet-identity field and are ineligible for routed hardware claims. Every routed hardware candidate instead requires a board-specific `TRON_ADVA_OVERRIDE` byte-equal to that target board's independently observed FICR AdvA; its manifest records exact AdvA, derived SID16/SID8, fleet inventory/hash and uniqueness/nonreserved result, source/tool/config/timer/capacity/hook provenance, and artifact hashes. UID-bound run evidence verifies board UID plus independently observed FICR AdvA against the override/manifest before flash; mismatch fails closed. |
| **BUILD-03** | Every feature slice keeps legacy wire-v1, routed AODV_ONLY, and applicable FULL_TAVRN builds/tests green. The implemented Phase 5 FULL build adds ESC, mentorship, HELLO/SYNC/TC bootstrap, copied FULL telemetry, and adaptive SID8 ordinary-HELLO cadence/suppression to the Phase 3 GTT/Smart-TTL prefix; AODV_ONLY contains no FULL dependency, and later repair-on/off remain separately testable. |
| **BUILD-04** | Hardware claims attach to immutable clean candidate commits. Mandatory gates are two-board routed link, two-board direct and three-board forced-relay AODV, three-to-eight-board full-profile behavior, four-board transit repair, and finite soak; the `98/150` foundation remains only a best-effort baseline. |

## Derived-contract boundary

Wire-v2 owns byte offsets, type codes, exact golden vectors, and the size proof
behind `META-02`; identity owns canonical byte order and compressed lookup; ACK
owns DATA custody timing; architecture owns source-file boundaries and fixed
storage allocation. Those accepted contracts derive `TR/02`, AdvA, SID16/SID8,
three-attempt/250 ms HACK, and capacity representation from this profile.
Neither may change a requirement above; an impossible fit or ownership conflict
returns to this profile as an explicit revision rather than an undocumented
implementation exception.
