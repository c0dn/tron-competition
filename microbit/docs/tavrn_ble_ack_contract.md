# TAVRN-BLE hop-custody ACK and failure contract

Status: **frozen Phase 0 proof-of-concept contract**. This document defines
link-v2 ownership transfer. Frame bytes and identity modes are frozen in
`tavrn_ble_wire_v2.md` and `tavrn_ble_identity.md`.

Profile mapping: this document derives `LINK-01` through `LINK-05`,
`AODV-06`, `AODV-07`, `REPAIR-02`, and serial deadline behavior in
`SERIAL-05`.

## 1. Terms and ACKable classes

**ACK-001:** Only a logically unicast routed `DATA (type=10)` frame is
HACKable. This includes SID16/SID8 DATA, patient DATA, originated DATA, transit
DATA, and local-repair replay DATA.

The following are never HACKable: HACK, generic FLOOD, E_RREQ, E_RREP,
E_RERR, E_RREP_ACK, HELLO, SYNC_OFFER, SYNC_PULL, SYNC_DATA, and TC_UPDATE.
Their route-discovery, symmetry, flood, or bootstrap FSM owns any timeout.
There is no HACK-of-HACK and no ACK storm for controlled broadcasts.

`Custodian` means the node that currently owns exactly one DATA item and must
retain enough decoded context and payload to either:

1. transfer custody to the next hop;
2. deliver exactly once at the final destination; or
3. report a typed local failure without silently discarding it.

One multi-channel advertisement TX event over selected primary channels
37/38/39 is one **attempt**, not one per channel. An attempt is counted exactly
when the scheduler reports TX_DONE with a nonzero intersection of selected and
completed channel masks.

### HACK turnaround correction (`DEV-023`, extending `DEV-006`)

Every generated DATA HACK is scheduler-ineligible until
`now + config.hack_turnaround_ms`, where `now` is the receiver-side time of the
ACCEPTED, DUPLICATE, explicit BUSY, REJECTED, additional-DATA BUSY,
candidate-timeout BUSY, or dedupe-capacity BUSY decision. For a scheduler RX
event, that decision time is the observed event time captured immediately after
the synchronous scheduler poll returns, not the pre-poll scheduler-selection
time. The generated config sources this field directly from the existing
`timer.radio_tx_event_bound_ms=8`; it is not a 72nd timer key. This lets a
sender finish the bounded synchronous 37→38→39 TX event and restore RX before a
receiver's HACK can be selected. The delay applies with normal wrap-safe
deadline arithmetic and does not change HACK priority once due.

## 2. Exact HACK correlation

The pending sender key is:

```text
{
  network,
  expected_HACK_outer_AdvA,       // the DATA next hop, full canonical identity
  HACK.receiver,                  // this sender's SID at the DATA identity width
  DATA.origin,
  DATA.final_destination,
  DATA.data_seq,
  DATA.app_kind,
  DATA.app_source,
  identity_width
}
```

**ACK-002:** A HACK matches only when every field above matches and its outer
AdvA exactly equals the retained direct-next-hop binding. SID16 DATA
origin/destination fields need no remote full-identity resolution. For SID8
HACK, the full outer immediate transmitter and immediate receiver require current
unambiguous context validation. HACK origin and final destination are
custody-correlation keys: they require correct SID8 width, unicast syntax,
nonreserved values, and valid encoded form, but do not require current live GTT
resolution. This exception applies only to HACK; DATA and all other controls
retain normal identity admission. Network/type isolation, reserved bits, exact
HACK length, logical receiver, and status are validated first. Exact
active-custody correlation is the mutation boundary, not HACK authentication.
An unmatched or late HACK is counted and ignored. It cannot complete another
pending DATA item that happens to share a sequence number.

The HACK wire does not repeat TTL, hops, or patient bytes because those are not
correlation fields. The sender retains the entire failed DATA context while
pending.

## 3. Custody acceptance order

**ACK-003:** Receiver processing order for DATA is:

1. validate wrapper, wire version, network, type, exact length, flags,
   identities, logical next hop, and app shape;
2. retain outer AdvA for the direct peer; accept SID16 as standalone logical
   route identity or uniquely resolve SID8 through FULL_TAVRN context;
3. form the DATA dedupe key;
4. if the key is already **committed**, perform no second queue/delivery action
   and enqueue one `DUPLICATE` HACK for this received logical frame;
5. otherwise create one immutable, single-use **candidate token** containing
   the validated frame, complete dedupe key, outer AdvA, receiver mode, and a
   link generation. Candidate/application storage uses the public ten-byte DATA
   capacity, not the seven-byte patient schema size. Do not insert dedupe or
   send HACK yet;
6. invoke the router/application candidate handler. It validates
   TTL/route/policy, reserves either a non-evictable transit custody slot or a
   bounded final application-delivery slot, and MUST call
   `resolve_rx(candidate, ACCEPTED|BUSY|REJECTED)` synchronously before that
   handler returns;
7. ACCEPTED atomically transfers the reserved slot, records committed dedupe,
   and attempts ACCEPTED HACK enqueue. BUSY or REJECTED releases any
   provisional reservation, records no dedupe, and attempts that status HACK.

The link arms `timer.link_candidate_resolve_ms` (10 ms in BALANCED) before
invoking the handler as defensive containment, not as permission for normal
asynchronous resolve.
If a candidate remains unresolved when that timer expires, link-v2
automatically resolves it BUSY, records no dedupe, frees the candidate, and
attempts one BUSY HACK. While a candidate is pending, control, HACK, TX_DONE,
TX_FAILED, and timer processing continue. Any additional otherwise-valid new
or still-uncommitted DATA reception while the candidate slot is pending,
whether its key is the same or different, gets one BUSY HACK attempt and
creates no second candidate, reservation, or dedupe entry. A committed
duplicate still follows the automatic DUPLICATE path and creates no candidate.

A stale, wrong-generation, or already-consumed token is rejected locally and
cannot send HACK or mutate dedupe.

`resolve_rx` returns typed candidate-resolution status and writes an optional
link event output. HACK priority may evict one tracked lower-priority DATA
scheduler item. When that happens, the event output MUST be
`LOCAL_TX_NOT_ATTEMPTED` with the evicted DATA's immutable context/ownership;
the evicted item never receives TX_DONE and does not increment attempts.
Automatic BUSY resolution uses the same enqueue path and surfaces the same
typed outcome through `tick`. HACK enqueue refusal without eviction increments
`hack_enqueue_failed`; it does not alter the successful candidate decision or
invent a custody event. The enqueue site consumes the scheduler's eviction
token directly; unrelated APIs are not required to infer or synthesize this
outcome later.

Malformed, foreign-network, wrong-next-hop, ambiguous SID8/direct binding,
unsupported application, and policy-invalid frames are never inserted into
dedupe before their semantic decision. `BUSY` and `REJECTED` do not commit the
DATA dedupe key. A later retransmission can therefore be accepted.

An ACKed transit queue entry MUST NOT be evicted for control, relay, or newly
originated traffic. It leaves custody storage only after the next hop accepts
custody, final delivery succeeds, or a typed failure path takes ownership.

### Router fail-stop containment

The common router has sixteen ordered failure obligations and one copied overflow
preservation slot. Before it ingests candidate/control AODV input, accepts an
application submission, promotes link custody, or allows ordinary action work to
overtake a report, it attempts the oldest obligation first. An action dispatch
may drain one already-retained action only to make bounded AODV capacity
available; it cannot admit unbounded newer DATA work ahead of that obligation.

An application `reserve=OK` with token zero, delivery/action data mismatch,
non-BUSY post-ACK AODV ingest failure, failed required transit
`release_rx_custody`, permanent delivery callback result, invalid control
cancellation, failure-report invariant, or seventeenth distinct failure latches
one typed router fault. Accepted delivery/pending/overflow state remains exactly
retained; the faulted router invokes no further delivery callback, retry, or
link/AODV mutation and every later mutating router API returns `INVALID`. The
seventeenth event is copied into the overflow slot rather than released and
forgotten.

## 4. HACK statuses

HACK status is the final HACK byte:

| Value | Name | Receiver statement | Sender action | Link break? |
| ---: | --- | --- | --- | --- |
| `00` | `ACCEPTED` | First candidate `resolve_rx(ACCEPTED)`: a reserved transit custody or final delivery slot now owns DATA exactly once | Complete transfer; remove local pending copy | No |
| `01` | `DUPLICATE` | This exact DATA key was previously accepted/delivered and is still in committed dedupe history | Complete transfer exactly as ACCEPTED | No |
| `02` | `BUSY` | Valid DATA, but transient capacity prevents custody now | Retain custody; stop the current no-response episode; defer and retry under busy policy | No |
| `03` | `REJECTED` | Permanent semantic/policy rejection (unsupported app, TTL cannot be forwarded, prohibited destination, or identity policy) | Retain/return custody to caller and emit `CUSTODY_REJECTED`; do not retry this next hop unchanged | No |

Other values are malformed.

**ACK-004:** Every valid duplicate DATA reception for a committed key generates
one fresh `DUPLICATE` HACK enqueue attempt, even if an earlier HACK was sent.
Duplicate reception never repeats application delivery or transit insertion.
If the HACK queue itself is temporarily full, the receiver records
`hack_enqueue_failed`; it does not undo committed custody. A later DATA retry
again produces a DUPLICATE HACK.

`ACCEPTED` means successful candidate resolve into reserved transit custody or a
reserved final exactly-once delivery slot. Merely decoding, seeing, producing a
candidate, or placing the item in an evictable scheduler queue is not custody.

The public candidate, custody, retry, eviction, and terminal-outcome DATA
containers all hold up to `TAVRN_LINK_DATA_PAYLOAD_MAX=10` application bytes.
Patient DATA is exactly seven bytes only after `app_kind=01` schema validation;
no public link container may be sized to seven.

BUSY and REJECTED prove only that a valid HACK came from the exactly correlated
peer. They do not refresh AODV route lifetime, GTT membership/freshness, or
neighbor liveness state.

## 5. No-response retry episode

Protocol FSM behavior uses canonical manifest references, not local literals:

```text
attempt limit                 = timer.link_max_attempts
response deadline             = timer.link_hack_timeout_ms
retry backoff                 = timer.link_retry_backoff_ms
BUSY backoff                  = timer.link_busy_backoff_ms
BUSY response limit           = timer.link_busy_max_responses
selected-next-hop deadline    = timer.link_data_deadline_ms
```

BALANCED examples are three attempts, 250 ms response, zero retry backoff,
500 ms BUSY backoff, three BUSY responses, and a 5000 ms transaction deadline.
`hack_turnaround_ms=8` is a link-config field sourced from the existing radio
TX-event bound, not a separately manifested timer.

The canonical hardware/manifest keys and values are exactly:

| Manifest key | Value | Contract meaning |
| --- | ---: | --- |
| `timer.scheduler_custody_bypass_max` | 2 | At most two higher-priority custody/HACK bypasses before a due tracked DATA receives scheduler service. |
| `timer.scheduler_poll_max_ms` | 2 | Maximum scheduler poll/service gap used in the attempt bound. |
| `timer.radio_state_timeout_ms` | 2 | Bound for each required RADIO state transition. |
| `timer.radio_tx_event_bound_ms` | 8 | Bound for the selected advertising TX event. |
| `timer.link_tx_scheduler_attempt_bound_ms` | 30 | Bound from one attempt becoming eligible through valid TX_DONE. |
| `timer.link_response_window_sum_ms` | 750 | Three 250 ms HACK response windows; not scheduler/radio wall time. |
| `timer.link_no_response_wall_bound_ms` | 840 | Complete no-BUSY, no-response hardware wall bound. |
| `timer.link_candidate_resolve_ms` | 10 | Defensive unresolved-candidate BUSY timeout. |

These names are canonical; ACK-only aliases for manifested timing keys are not
permitted.

All 32-bit millisecond deadlines use wrap-safe half-range comparison; every
constant is less than `0x80000000`.

Link-v2 may own four custody slots, but exactly one custody DATA attempt is
physically eligible/in-flight in the scheduler at a time. The other three slots
may wait without scheduler tokens. Completion, typed failure, BUSY deferral, or
terminal resolution of the eligible attempt permits link-v2 to make the next
slot eligible. Every slot, including time waiting before first eligibility, is
bounded by its `timer.link_data_deadline_ms`; four slots do not create four
simultaneous attempt-wall guarantees.

**ACK-005:** The sender FSM is deterministic:

1. Encoding failure, scheduler rejection, or expiry before a required actual
   transmission terminates with `LOCAL_TX_NOT_ATTEMPTED`; that unperformed TX
   does not increment attempt count and the link remains valid.
2. On observed actual TX_DONE, increment attempt count and set
   `response_deadline_ms = observed_TX_DONE_time + timer.link_hack_timeout_ms`
   (BALANCED 250 ms). The observed TX_DONE time is captured immediately after
   the synchronous scheduler poll returns, not the pre-poll scheduler-selection
   time.
3. Matching ACCEPTED or DUPLICATE completes custody.
4. Matching BUSY cancels the response deadline, clears the radio-loss attempt
   episode, and schedules the same DATA no earlier than
   `timer.link_busy_backoff_ms` (BALANCED 500 ms). BUSY count is independent of
   no-response attempt count.
5. Matching REJECTED stops unchanged retransmission immediately.
6. At an unmatched response deadline, retransmit the exact same DATA PDU if
   attempt count is below `timer.link_max_attempts`. DATA carries no topology
   metadata and no repair-only wire field.
7. After `timer.link_max_attempts` actual TX completions, if the final
   `timer.link_hack_timeout_ms` deadline expires with no matching HACK, emit
   `RETRY_EXHAUSTED` exactly once.

Retries become due at the expired response deadline. If local scheduler failure
prevents an intended retry from becoming an actual TX, the terminal result is
`LOCAL_TX_NOT_ATTEMPTED`, not RETRY_EXHAUSTED.

Every mesh radio state operation--init, idle/disable, listen, RX restore,
snapshot, and TX--uses a bounded typed seam. Each state wait is bounded by
`timer.radio_state_timeout_ms=2`; a selected multi-channel TX event is bounded
by `timer.radio_tx_event_bound_ms=8`. Scheduler evidence includes the
requested/selected channel mask, `completed_channel_mask`, fault operation or
channel when available, and typed fault/result.

- If no selected channel completed, the scheduler emits TX_FAILED. Tracked DATA
  receives `LOCAL_TX_NOT_ATTEMPTED`, attempt count is unchanged, and no HACK
  deadline is armed.
- Once at least one selected channel completed, the scheduler emits TX_DONE and
  preserves the completed mask/fault evidence even if a later selected channel
  or RX restore times out. The DATA sender increments attempt count and arms
  `timer.link_hack_timeout_ms` because a receiver could have heard the completed
  channel.
- A nonzero completed mask MUST NOT later become
  `LOCAL_TX_NOT_ATTEMPTED`. Later-channel/restore fault evidence remains visible
  for diagnostics and recovery but cannot erase the actual attempt.

Bounded init/idle/listen/snapshot/restore failures outside a completed tracked
TX remain typed radio/scheduler failures and leave the system schedulable; they
never fabricate TX_DONE or partial RX identity/data.

The 750 ms value is only `timer.link_response_window_sum_ms`. The complete
no-BUSY hardware wall bound from first-attempt eligibility is exactly:

```text
timer.link_no_response_wall_bound_ms
    = timer.link_response_window_sum_ms
    + timer.link_max_attempts * timer.link_tx_scheduler_attempt_bound_ms
    = 750 + 3 * 30
    = 840
```

Acceptance evidence uses the manifested 840 ms bound, never a bare 750 ms
claim. `timer.scheduler_custody_bypass_max=2` is part of proving each 30 ms
attempt-service bound. The 30 ms derived bound is exactly:

```text
timer.link_tx_scheduler_attempt_bound_ms
    = (timer.scheduler_custody_bypass_max + 1)
    * (timer.radio_tx_event_bound_ms + timer.scheduler_poll_max_ms)
    = (2 + 1) * (8 + 2)
    = 30
```

The 840 ms no-response wall starts when the first attempt becomes physically
eligible, not when the application initially submits DATA or when it first
occupies one of the four custody slots. Pre-eligibility waiting remains covered
only by `timer.link_data_deadline_ms`.

`timer.link_busy_max_responses` correlated BUSY responses or expiry of
`timer.link_data_deadline_ms` while busy/deferred emits
`CUSTODY_BUSY_EXPIRED`. A REJECTED response emits `CUSTODY_REJECTED`
immediately. BALANCED values are three BUSY responses and 5000 ms.

### 5.1 Typed terminal outcomes

Every terminal outcome carries immutable DATA context and current custody
ownership to its consumer. Exactly these outcomes exist:

| Outcome | Trigger | Actual attempts | Link break? |
| --- | --- | ---: | --- |
| `LOCAL_TX_NOT_ATTEMPTED` | A required initial/retry TX has zero completed selected channels/TX_FAILED; or `resolve_rx` HACK priority evicts tracked lower-priority DATA before TX_DONE | current no-response episode below the manifested limit; prior BUSY-confirmed TXs are separate; failed/evicted TX is not counted | No |
| `CUSTODY_BUSY_EXPIRED` | `timer.link_busy_max_responses` correlated BUSY HACKs, or `timer.link_data_deadline_ms` while busy/deferred | any completed episode count | No |
| `CUSTODY_REJECTED` | One correlated REJECTED HACK or permanent local next-hop policy rejection before TX | any prior count | No |
| `RETRY_EXHAUSTED` | `timer.link_max_attempts` actual TX_DONE attempts in one no-response episode and the final `timer.link_hack_timeout_ms` deadline with no correlated terminal HACK | exactly the manifested attempt limit in that episode | **Yes** |
| `RADIO_FAULT_TERMINAL` | A global typed radio fault latches after zero-channel failure outside the already-returned active item, after a partial/completed TX, or during bounded RX/init/restore operation; drain each remaining occupied custody slot once | preserve that slot's actual completed-attempt count and latest requested/completed masks; a partial TX remains counted | No |
| `SERVICE_FAULT_TERMINAL` | Scheduler poll overrun, queue corruption, or impossible mesh-service state latches globally; drain each remaining occupied custody slot once | preserve that slot's actual completed-attempt count; never invent TX_DONE | No |

`LOCAL_TX_NOT_ATTEMPTED`, `CUSTODY_BUSY_EXPIRED`, and `CUSTODY_REJECTED` are
per-item queue/service/policy outcomes. The two fault-terminal outcomes report a
global local mesh-path failure, not evidence about the selected peer. None of
those five may invalidate the neighbor, start local repair, or originate LEAVE.
Only `RETRY_EXHAUSTED` is a peer-link break.

Fault latching makes every occupied slot fault-pending and `tick` transfers one
owned terminal event per call until all are drained. `RADIO_FAULT_TERMINAL`
preserves a nonzero completed-channel mask and counted attempt when a receiver
could have heard a partial TX; `SERVICE_FAULT_TERMINAL` never fabricates a
completion. Both carry the same immutable DATA/direct-next-hop/logical-identity
context below and require explicit caller custody acceptance or disposal before
the slot is reused.

Every terminal event identifies the failed direct next hop exactly as
`{logical_id(width,value), full_AdvA[6]}` from the retained binding. It carries
DATA's logical route origin and logical final destination at their encoded
width; it does **not** require or fabricate remote full-origin/full-destination
copies.

For SID8 FULL_TAVRN, the router MUST resolve the logical route origin and final
destination uniquely through current GTT before accepting transferred event
custody. Unresolved or ambiguous context fails closed through the router's
normal unresolved/identity-conflict handling, which must explicitly accept and
dispose/retain event ownership; link-v2 never manufactures full identities to
make transfer succeed. SID16 remains standalone logical context.

## 6. The only link-v2 break event

**ACK-006:** `RETRY_EXHAUSTED` is the only link-v2 event that reports a broken
link upward. It is legal only when:

- the frame class is DATA;
- the DATA was semantically admitted into a sender-owned custody slot;
- attempt count is exactly `timer.link_max_attempts`;
- every allowed attempt reached actual TX completion;
- the final `timer.link_hack_timeout_ms` deadline elapsed; and
- no syntactically valid, exactly correlated ACCEPTED, DUPLICATE, BUSY, or
  REJECTED HACK was received.

Queue full, encode failure, local expiry before TX, scheduler eviction, BUSY,
REJECTED, global radio/service fault, E_RREP_ACK timeout, RREQ timeout, and
SYNC_PULL timeout cannot directly emit this event. A malformed or unmatched
HACK does not cancel the pending response deadline; only
`timer.link_hack_timeout_ms` expiry after `timer.link_max_attempts` completed
attempts, if all other preconditions hold, may emit RETRY_EXHAUSTED.

The event transfers the still-owned failed DATA context to the route/local
repair layer. It MUST contain, without pointers to transient RX/TX buffers:

```text
failed direct next-hop logical ID (width and value)
failed direct next-hop full AdvA[6]
route origin logical ID (width and value)
final destination logical ID (width and value)
data_seq, app_kind, app_source
flags, TTL, hops
application length and exact application bytes
attempt_count (=timer.link_max_attempts; BALANCED 3), first_tx_ms, last_tx_ms,
final_deadline_ms
completed channel mask plus typed fault evidence from the latest/terminal
scheduler operation
originated-versus-transit ownership marker
```

The link module may not free or reuse the custody slot until the event consumer
accepts this context. Transit local repair therefore receives the exact DATA
that failed; it does not reconstruct a packet from a destination-only error.

`RETRY_EXHAUSTED` proves only this local failed-hop DATA/HACK episode for the
retained immediate hop H. It may invalidate routes through H and initiate local
repair for final destination D, but it proves neither H nor D departed and MUST
NOT directly originate a LEAVE. If repair fails, or if the event cannot be
owned by repair, the FULL maintenance owner schedules one bounded, coalesced
hard-expiry/liveness verification for H. That independent episode uses the
existing optional targeted Stage 0 HELLO, retained-hop+2 Stage 1 RREQ,
full-diameter Stage 2 RREQ, and direct-deadline/check-departure path. Only its
checked terminal departure or checked direct timeout may originate LEAVE.

## 7. E_RREP_ACK is not HACK

RFC 3561 uses RREP-ACK to test whether an RREP path is bidirectional. Routed
wire-v2 preserves that separate mechanism as `E_RREP_ACK (type=09)` and adds
the exact RREP tuple to avoid ambiguous correlation in an asynchronous
advertising transport.

**ACK-007:** When E_RREP has `A=1`, its immediate receiver sends one direct
E_RREP_ACK to the outer E_RREP transmitter. The sender waits
`timer.aodv_rrep_ack_wait_ms` (BALANCED 250 ms). A matching E_RREP_ACK confirms
current route symmetry.
Its pending key is exactly
`{network, expected_ACK_outer_AdvA, receiver, route_destination,
destination_sequence, RREQ_origin, request_id, identity_width}`; every field,
including destination sequence, must match.
Timeout may mark that neighbor `UNIDIRECTIONAL` for the AODV blacklist and may
cause route discovery to try another reverse path. It MUST NOT:

- generate DATA ACCEPTED/DUPLICATE semantics;
- remove a pending DATA custody item;
- emit RETRY_EXHAUSTED;
- trigger DATA local repair;
- emit TC_UPDATE(LEAVE) or RERR without an independent failed-DATA or route
  condition.

E_RREP_ACK itself is not HACKable. HACK cannot satisfy an E_RREP_ACK wait, and
E_RREP_ACK cannot satisfy a DATA HACK wait.

Unacknowledged ALOHA advertising, including a bare TX_DONE, establishes neither
delivery nor transmission failure. DATA/HACK and requested E_RREP/E_RREP_ACK
remain separate evidence paths. Future radio/MAC hardening may use both paths
according to their own contracts, but cannot call an RREP HACKable.

## 8. Required state-machine assertions

At minimum, later red/green tests must prove:

1. new DATA emits one candidate without dedupe or HACK mutation;
2. handler resolves synchronously before return after transit/final reservation;
3. BUSY/REJECTED resolve records no DATA dedupe and releases provisional state;
4. unresolved candidate reaches `timer.link_candidate_resolve_ms`, auto-resolves
   BUSY, frees its slot, and attempts one BUSY HACK;
5. while candidate is pending, control/HACK/TX/timers progress and each
   additional new DATA gets BUSY without a second candidate/reservation;
6. stale/wrong-generation/double candidate resolve has no wire/state effect;
7. resolve_rx HACK priority eviction returns LOCAL_TX_NOT_ATTEMPTED carrying
   the evicted lower-priority DATA context;
8. lost ACCEPTED causes same DATA retry, automatic DUPLICATE HACK, and one
   total delivery;
9. duplicate HACK is idempotent at sender;
10. BUSY does not commit receiver dedupe or mark a link broken;
11. REJECTED stops unchanged retry and does not mark a link broken;
12. enqueue/scheduler/radio failure with zero completed selected channels
    produces LOCAL_TX_NOT_ATTEMPTED without incrementing attempt count;
13. fewer than `timer.link_max_attempts` actual no-response attempts do not
    emit RETRY_EXHAUSTED;
14. exactly `timer.link_max_attempts` actual attempts plus final timeout emit
    one RETRY_EXHAUSTED carrying byte-identical failed DATA context;
15. BUSY exhaustion and REJECTED produce their typed non-break outcomes;
16. a late/unmatched HACK cannot complete another pending item;
17. E_RREP_ACK destination-sequence mismatch fails correlation and HACK cannot
    cross-satisfy it;
18. one completed channel followed by later-channel/restore timeout emits
    TX_DONE, increments one attempt, arms HACK, and preserves completed-mask and
    fault evidence without LOCAL_TX_NOT_ATTEMPTED;
19. exact manifest timing keys equal `2,2,2,8,30,750,840,10` as declared,
    zero-channel bounded radio failure never emits TX_DONE, and no-response
    hardware completes by 840 ms from first-attempt eligibility;
20. exactly one custody attempt is physically eligible at once while four slots
    can wait under `timer.link_data_deadline_ms`;
21. every terminal event carries exact failed-next-hop `{logical ID,full AdvA}`
    plus logical route origin/final destination and requires no remote full
    copies;
22. SID8 event-custody acceptance uniquely resolves origin/final through GTT,
    with unresolved/ambiguous cases following normal fail-closed handling;
23. public candidate/custody/failure storage handles ten-byte DATA8 while
    patient validation remains exactly seven bytes; and
24. timer comparisons remain correct across 32-bit millisecond wrap.
25. every DATA HACK producer queues its HACK at exactly `now + 8`, including
    due-1/due selection and `UINT32_MAX` wrap; a future high-priority HACK does
    not block due lower-priority work;
26. TX_DONE sets `response_deadline_ms` exactly 250 ms later, including wrap,
    while the 250/750/840 values and formulas remain unchanged.

The ACCEPTED golden vector for the wire document's patient DATA is:

```text
02 01 06 14 ff ff ff 54 52 02 2a 11 00 18 42 18 42 11 22
34 12 01 07 00
```

Changing the final byte to `01`, `02`, or `03` produces the DUPLICATE, BUSY,
or REJECTED vectors without changing correlation.
