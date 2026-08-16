# TAVRN-BLE layered architecture and build contract

Status: V2.3 architecture contract for a medium-correctness proof of
concept. This is not a Bluetooth Mesh, production-routing, security, clinical,
or certification claim.

### Routed bearer status

Sections 5, 8, 12, and 13 describe the implemented rolling-BU,
priority-banded-EDF, repeated-operation, fixed-response, and token-expiry
software. The queue deliberately has no generalized owner/reset layer. Routed
capacity variants 4/8/16/40 compile, but production capacity selection,
capacity-40 WCET/stack evidence, and hardware acceptance remain pending.

## 1. Authority and scope

This document owns module boundaries, dependency direction, public C seams,
fixed storage capacities, build composition, generated build metadata, artifact
naming, rollback, and lane ownership. It does **not** assign normative
requirement IDs or define byte offsets.

Authority is split deliberately:

1. `microbit/app/protocol/README.md` and its golden-vector tests own legacy
   flood wire-v1 behavior.
2. `microbit/docs/tavrn_ble_profile.md` and
   `microbit/docs/tavrn_ble_deviations.md` own routed protocol semantics and
   timer values after Phase 0 integration.
3. `microbit/docs/tavrn_ble_wire_v2.md`,
   `microbit/docs/tavrn_ble_ack_contract.md`, and
   `microbit/docs/tavrn_ble_identity.md` own routed wire-v2 fields, HACK
   statuses, and identity encoding after Phase 0 integration.
4. This document owns which module may use those contracts and how CMake
   selects it.

The sibling profile, wire, ACK, and identity documents are the current parallel
lane inputs. Section 8 freezes the Phase 1 C representation that derives from
their bytes/semantics; section 13 mirrors profile-owned timer keys without
becoming a second numeric registry. Parent integration must reconcile any
missing key or cross-document mismatch before Phase 1 red tests. No
implementation may invent a wire field, HACK meaning, timer, or identity rule
locally.

## 2. Baseline that must remain reversible

The clean baseline at commit `bf17bce301cae10df0819b3b6fe7a8c19256bdde`
has these properties:

- `ble_radio` emits and receives raw legacy BLE advertising PDUs with one
  nRF52833 radio.
- `ble_mesh_scheduler` is the radio owner for `ble_mesh_node`, uses one-shot RX
  snapshotting, interleaves bounded TX, restores RX, and has a fixed-size TX
  queue.
- `tron_mesh_packet`, `tron_mesh_dedupe`, and `tron_mesh_pingpong` implement the
  controlled-flood wire-v1 proving ground.
- `ble_mesh_node/src/main.c` currently contains both platform orchestration and
  legacy flood-node behavior.
- `build-ble-node.sh` builds a pinned-node artifact and a small manifest.

The target architecture may extract state from `main.c` and may extend shared
radio/scheduler APIs, but it must not silently change wire-v1 bytes, admission,
dedupe, TTL relay, queue preference, PING/PONG semantics, or legacy log meaning.
Any shared radio, queue, scheduler, clock, identity, or generated-config change
requires all legacy host tests, golden vectors, and the legacy firmware build.

## 3. Target dependency graph

Arrows mean "may include/call" and point from consumer to dependency. There is
no reverse include and no route-table backdoor.

```text
patient/application bridge
        |
        v
selected tron_node facade
        |
        +--> legacy_flood_node
        |       +--> wire-v1 / dedupe / pingpong
        |       +--> ble_mesh_scheduler (the shared instance below)
        |
        +--> routed binding
               |       |
               |       +--> tavrn_full (FULL_TAVRN only)
               |              +--> GTT / Smart TTL / fixed-k ESC
               |              +--> mentorship / maintenance / TC
               |              +--> optional repair
               |              +--> public router/AODV types
               v
          tavrn_router
             |     |
             |     +--> aodv_core (one route table)
             |
             +--> tavrn_link_v2 --> routed wire-v2 codecs
                       |
                       v
              ble_mesh_scheduler --> ble_mesh_tx_queue
                       |
                       v
                   ble_radio
```

`aodv_core` does not include `tavrn_link_v2.h` or any FULL_TAVRN header. It
accepts decoded inputs and emits by-value actions; `tavrn_router` translates
between those actions and link-v2. FULL_TAVRN modules depend on public AODV
snapshots/commands and generic router hooks. No FULL_TAVRN module is visible to
the AODV core, and none stores a forwarding next hop as a second route table.
The routed binding constructs `tavrn_full` first and injects its generic vtable
into the router; callback invocation does not create a router-to-full include.

The Phase 5 production routed binding is one `tavrn_router_t` instance in
`app/tavrn_routed_node/src/main.c`. Its application port adapts the firmware's
eight-entry delivery ring through opaque nonzero reserve/commit/cancel tokens;
it does not retain AODV actions, resolve candidates, or release transit custody
itself. Each mesh cycle is scheduler event, router scheduler handling, router
tick, optional application submission, mandatory typed `dispatch_ex` event
handling, then router link service. Router faults are copied to the logger/fault
counters without recreating protocol ownership in the binding.

### 3.1 V2.3 bounded-gossip ownership

The router/wire boundary owns exact ordinary-HELLO construction and validation;
it obtains the `GOSSIP-01` count from the GTT's occupied/nonself/nondeparted
enumeration at construction time. Maintenance owns ordinary HELLO dedupe,
direct GTT admission, and the required post-admission `remote_count >
local_count` decision. It may notify mentorship only after `RX_UNIQUE`; neither
the codec nor a duplicate/rejected frame may start repair.

Mentorship owns the one private active-RFI initiator/cooldown and responder
snapshot service. It reuses the existing bounded SYNC_PULL/SYNC_DATA by-value
controls and snapshot capacity, but must not transition public mentorship state,
change identity width/incarnation, gate application traffic, invoke bootstrap
activation/recovery, or originate JOIN. Bootstrap, rejoin, identity conflict,
and accepted/committed real TC work preempt and clear private RFI work. The
existing `timer.mentor_sync_dedupe_ms` is the RFI success/abort cooldown; no
new timer/config key, scheduler ownership, or background queue is introduced.
`SYNC_PULL` flag bit 0 is the RFI marker only on page zero; the existing
`SYNC_OFFER` flag byte remains zero, and continuation pulls use zero flags.

`aodv_core` owns terminal discovery cleanup. Before clearing discovery or any
matching pending DATA, it must enqueue the one existing destination-scoped
`PENDING_DATA_FAILED` action. Router dispatch preserves that typed event as the
application-facing failure; neither core nor router attaches a TC/LEAVE side
effect.

There are exactly two behavioral branches above the shared BLE foundation:

- legacy controlled flood using wire-v1; and
- the TAVRN routed node using wire-v2.

`AODV_ONLY` and `FULL_TAVRN` are build levels of the second branch. They are not
plugins and do not select different link, forwarding, AODV, or route-table
implementations.

## 4. Current-to-target file and module map

Paths in the target column are the source ownership contract for later slices.
Existing proven files stay at their current paths unless the integration writer
has a regression-proven reason to move them.

| Current file/area | Target ownership | Decision |
| --- | --- | --- |
| `app/drivers/ble_radio.{h,c}` | shared hardware primitive | Retain. It alone touches nRF RADIO/FICR radio-address registers. Mesh callers use bounded typed init/idle/listen/snapshot/TX only; compatibility wrappers are non-mesh and bounded internally. |
| queue embedded in `ble_mesh_scheduler` | `app/protocol/ble_mesh_tx_queue.{h,c}` | Extract as a pure fixed-size queue used by the shared scheduler. No codec or route knowledge. |
| `app/protocol/ble_mesh_scheduler.{h,c}` | shared single-radio scheduler | Retain and extend with one canonical local AdvA, structurally validated AdvA-preserving RX, serialized custody service, partial-channel TX accounting, and typed fault handling. It alone calls `ble_radio_try_*` during mesh-node operation. |
| `tron_mesh_packet`, `tron_mesh_dedupe`, `tron_mesh_pingpong` | legacy wire-v1 branch | Retain names and golden behavior. Routed code must not include these headers. |
| flood logic in `ble_mesh_node/src/main.c` | `app/protocol/legacy_flood_node.{h,c}` | Extract without semantic redesign. This is the complete legacy branch above the node facade. |
| no routed codec | `app/protocol/tavrn_wire_v2.{h,c}` and `app/protocol/aodv_codec.{h,c}` | Pure fixed-buffer encode/decode according to the wire-v2 lane. No radio, clocks, FICR, tables, or heap. |
| no routed link | `app/protocol/tavrn_link_v2.{h,c}` | Own two-phase DATA admission, custody, HACK/retry, routed duplicate state, controlled flood admission, RSSI observations, typed local/custody outcomes, and the sole `RETRY_EXHAUSTED` link-break outcome. |
| no AODV | `app/protocol/aodv_core.{h,c}` | Own the single AODV engine and single forwarding route table. No GTT, ESC, mentorship, maintenance, repair, scheduler, or radio include. |
| no routed orchestrator | `app/protocol/tavrn_router.{h,c}` | Compose one `aodv_core` with link-v2; translate inputs/actions; expose application submission, delivery, route snapshots, and generic augmentation hooks. |
| no full modules | `app/protocol/tavrn_gtt.*`, `tavrn_smart_ttl.*`, `tavrn_esc.*`, `tavrn_mentorship.*`, `tavrn_maintenance.*`, `tavrn_repair.*`, `tavrn_full.*` | FULL_TAVRN-only state and policy. `tavrn_full` is the only binder to generic router hooks. |
| no common node API | `app/protocol/tron_node.h`, `tron_node_legacy.c`, `tron_node_routed.c` | Firmware-facing facade. CMake compiles exactly one binding source; `main.c` does not select modes with preprocessor branches. Both routed feature levels use `tron_node_routed.c`. |
| future patient handling | `app/protocol/patient_bridge.{h,c}` | Optional application layer above `tron_node`; only the mesh-owner lane calls the canonical GTT API, so other tasks marshal copied commands/results through the existing application-event owner. It is never included by link, AODV, GTT, or radio code. |
| no test-hook port | `app/protocol/tron_test_hooks.h`, `tron_test_hooks_off.c`, `tron_test_hooks_bench.c` | One injected fault port. CMake compiles exactly one implementation; algorithms do not contain feature-selection preprocessor blocks. |
| `ble_mesh_node/src/main.c` | platform/task startup only | Create the dedicated bounded mesh task and fixed copied command/event/log queues. Logging/display/patient work runs outside the mesh task. No route/flood algorithm. |
| no generated profile config | `app/ble_mesh_node/config/tron_build_config.h.in`, `src/tron_build_info.c.in` | One validated CMake registry generates the effective C config and runtime build-info line; no algorithm supplies hidden defaults. |
| `ble_mesh_node/CMakeLists.txt` | integration writer | Validate profile combinations, import one centralized source manifest, generate effective config, and create the firmware target. |
| `build-ble-node.sh` | build/provenance writer | Validate CLI inputs, make a clean temporary build, publish renamed artifacts, and complete the external manifest. |
| no Phase 1 routed target | `app/ble_link_v2_testbed/` | Dedicated non-routing link-v2 harness only. It is not `AODV_ONLY`, `FULL_TAVRN`, or a third node mode. |

Host tests mirror module names under `microbit/tests/protocol/`. Pure modules
must remain buildable without μT-Kernel or hardware.

## 5. Shared radio, queue, and scheduler contract

### 5.1 Radio ownership

Mesh firmware uses only typed bounded operations. Existing calls remain
source-compatible wrappers for non-mesh callers, but the wrappers delegate to
these bounded primitives and cannot spin forever:

```c
typedef enum ble_radio_op_result {
    BLE_RADIO_OP_OK = 0,
    BLE_RADIO_OP_NO_EVENT,
    BLE_RADIO_OP_CRC_DROP,
    BLE_RADIO_OP_INVALID_ARGUMENT,
    BLE_RADIO_OP_STATE_TIMEOUT,
} ble_radio_op_result_t;

typedef struct ble_radio_tx_result {
    uint8_t requested_channel_mask;
    uint8_t completed_channel_mask;
    ble_radio_op_result_t fault;
} ble_radio_tx_result_t;

ble_radio_op_result_t ble_radio_try_init(UINT state_timeout_ms);
ble_radio_op_result_t ble_radio_try_idle(UINT state_timeout_ms);
ble_radio_op_result_t ble_radio_try_listen_once(
    UINT channel, UINT state_timeout_ms);
ble_radio_op_result_t ble_radio_try_poll_snapshot(
    UB *buf, UINT *len, UINT *rssi_magnitude_db,
    UINT state_timeout_ms);
ble_radio_tx_result_t ble_radio_try_advertise_channels(
    const UB *adv, UINT adv_len, const UB *addr6, UINT channel_mask,
    UINT state_timeout_ms);
ble_radio_op_result_t ble_radio_read_default_adva(UB out_adva[6]);

/* Compatibility wrappers: bounded internally, never used by mesh paths. */
void ble_radio_init(void);
void ble_radio_idle(void);
void ble_radio_advertise(const UB *adv, UINT adv_len, const UB *addr6);
void ble_radio_advertise_channels(const UB *adv, UINT adv_len,
                                  const UB *addr6, UINT channel_mask);
void ble_radio_listen(UINT channel);
void ble_radio_listen_once(UINT channel);
int ble_radio_poll(UB *buf, UINT *len, UINT *rssi_magnitude_db);
int ble_radio_poll_snapshot(UB *buf, UINT *len,
                            UINT *rssi_magnitude_db);
```

The typed seam never truncates or coerces: AdvData longer than 31 bytes, NULL
`addr6`/snapshot/default-AdvA output, NULL AdvData with nonzero length, a listen channel
outside 37/38/39, or a TX mask containing no 37/38/39 bit or any unknown bit
returns INVALID_ARGUMENT before starting RADIO.
Mesh queue admission enforces the same nonzero mask rule. Compatibility wrappers
may preserve the legacy zero-mask-means-all convention before delegating, but no
mesh caller may depend on that convention. NO_EVENT and CRC_DROP consume no
caller output bytes except the typed status.

At boot, platform orchestration selects one canonical local AdvA exactly once:
it copies the validated six generated `TRON_ADVA_OVERRIDE` bytes when configured,
otherwise it calls `ble_radio_read_default_adva()`. Failure or invalid
random-static/reserved identity fails closed. The same six-byte array is passed
to scheduler initialization and routed identity derivation; no codec or node
module re-reads FICR.

The scheduler stores that array and passes it as the non-NULL `addr6` argument
for every mesh TX, legacy or routed. The scheduler is the only mesh-node caller
after initialization. Mesh init, idle/disable, one-shot listen/restore, channel
hop, snapshot consumption, and TX all use the typed `try` operations. RX is
one-shot and disabled before the scheduler copies the raw PDU. The driver owns
its aligned 39-byte raw RX buffer; no pointer into that buffer escapes.

Every RADIO state wait in the bounded TX seam is limited by
`timer.radio_state_timeout_ms=2`. One pre-TX disable wait plus one wait for each
of the three selected primary channels gives
`timer.radio_tx_event_bound_ms=8`. A state timeout aborts remaining work,
prevents use of the failed channel, preserves any earlier completed-channel
bits, performs only bounded fail-closed recovery, and returns
`fault=BLE_RADIO_OP_STATE_TIMEOUT`. Compatibility wrappers use the same
canonical 2 ms bound and may not restore an unbounded register spin. The scheduler does not
precede the `try` call with `ble_radio_idle()`; the bounded `try` operation owns
the single pre-disable wait included in its 8 ms proof.

TX attempts visit selected channels in fixed 37, 38, 39 order and set a result
bit only after that channel completes. For a valid call,
`requested_channel_mask` is the validated input mask even on fault, and
`fault=OK` means `completed_channel_mask==requested_channel_mask`. A
later-channel timeout returns the requested mask, already completed mask, and
`STATE_TIMEOUT` without retransmitting completed channels inside the call.
Completed mask zero means no physical attempt completed; any nonzero completed
mask means one advertising attempt completed for link accounting even when a
later channel faulted.

The driver bounds raw `Length` before copying. Before emitting `RX_ADV`, the
scheduler additionally requires raw S0 exactly `0x42`
(`ADV_NONCONN_IND|TxAdd=random`), `Length` in `6..37`, exact raw snapshot length
`2 + Length`, and derived AdvData length `Length - 6 <= 31`. Invalid S0/Length
is counted and discarded with no AdvA/AdvData event. Valid events copy all six
outer AdvA bytes and exact AdvData before RX is restored.

The TX seam returns with RADIO disabled. On the successful path, RX restoration
can therefore program/start the next one-shot receive without another pending
state transition, so it does not add a fifth wait to the 8 ms TX-event bound.
Any unexpected non-disabled restore state is handled by the separately bounded
listen operation; timeout fails closed as RADIO_FAULT. The approved future
proof instead uses the 8/14/18 ms operation bounds and the 3048/9894 ms custody
bounds defined in the profile.

### 5.2 Physical TX queue

The extracted queue has this public shape. Exact integer values are stable so a
manifest and a host test can report them without including a node mode.

```c
#define TRON_BUILD_ROUTED_TX_QUEUE_CAPACITY  /* 4u, 8u, 16u, or 40u */
#define BLE_MESH_TX_QUEUE_CAPACITY TRON_BUILD_ROUTED_TX_QUEUE_CAPACITY
#define BLE_MESH_SCHED_LEGACY_TX_QUEUE_CAPACITY 4u
#define BLE_MESH_TX_TOKEN_NONE     0u

typedef uint16_t ble_mesh_tx_token_t;

typedef enum ble_mesh_tx_priority {
    BLE_MESH_TX_PRIORITY_RELAY   = 0,
    BLE_MESH_TX_PRIORITY_DATA    = 1,
    BLE_MESH_TX_PRIORITY_CONTROL = 2,
    BLE_MESH_TX_PRIORITY_RETRY   = 3,
    BLE_MESH_TX_PRIORITY_HACK    = 4,
} ble_mesh_tx_priority_t;

typedef enum ble_mesh_tx_service_class {
    BLE_MESH_TX_SERVICE_BEST_EFFORT = 0,
    BLE_MESH_TX_SERVICE_CUSTODY_DATA,
} ble_mesh_tx_service_class_t;

typedef struct ble_mesh_tx_item {
    uint8_t adv_len;
    uint8_t channel_mask;
    ble_mesh_tx_priority_t priority;
    ble_mesh_tx_service_class_t service_class;
    uint32_t not_before_ms;
    uint32_t expiry_ms;
    ble_mesh_tx_sweep_count_t sweep_count;
    ble_mesh_tx_budget_class_t budget_class;
    ble_mesh_tx_token_t token;
    uint8_t adv_data[BLE_ADV_MAX_DATA];
} ble_mesh_tx_item_t;
```

`enqueue` copies complete items. Routed entries retain release time, mandatory
expiry, priority band, insertion ordinal, one- or two-sweep cost, budget class,
and the existing token. Token zero is anonymous; nonzero is tracked by the
protocol that allocated it. A full queue may evict only the oldest strictly
lower-priority item and returns its token synchronously. LEGACY own/relay
behavior is unchanged and uses its independently fixed four-item queue. LINK
also compiles a fixed four-item queue; only ROUTED selects 4, 8, 16, or 40, and
no production capacity is selected yet.

Routed polling follows this exact algorithm:

1. Retire due expiry in one bounded pass: purge/count token-zero work, then emit
   at most one earliest nonzero-token `TX_EXPIRED` event per poll.
2. Test due promoted custody first. If it fits rolling total/general usage,
   reserve its full cost and run it.
3. If promoted custody does not fit, enter or retain custody hold. While held,
   only a higher-priority critical item with rolling critical usage plus cost at
   most 8 and total usage at most 40 may run; retest promoted custody first on
   the next poll. There is no general, lower-priority, or later-item fallback.
4. Without promoted custody, choose released work by priority band, earliest
   expiry within the band, then wrapped insertion ordinal. A selected item that
   lacks budget runs nothing and leaves RX active; it is never bypassed.
5. Advance custody bypass only after physical completion of a non-promoted,
   non-custody operation. Zero-completion failure is not a completed bypass.

One BU is one ordered 37/38/39 sweep. A selected operation reserves its full
one- or two-BU cost and executes without interleaving; total/general/critical
rolling accounting charges that reservation on zero, partial, and complete
physical outcomes. General use is limited to 32 BU in each true rolling 1000 ms
window and total use to 40; outside custody hold critical service may use the
total budget, while the 8-BU critical-reserve test applies during custody hold.
The radio operation is 8 ms for one sweep, 14 ms for two repeated sweeps, and
18 ms for two-sweep fault cleanup plus RX restore. Repeated transmission is not
FEC.

Only link-v2 may enqueue `CUSTODY_DATA`, and only for its one active initial or
retry attempt with a nonzero tracked token. Its other three custody slots remain
link-local and never enter the physical queue. Legacy wrappers, HACK, and normal
control use `BEST_EFFORT`. Tracked metadata, targeted, verification, and repair
controls also remain `BEST_EFFORT`; their existing token correlates scheduler
events and never changes service class.

The link allocates the low tracked range `0x0001..0x7fff`, shared by custody DATA
and routed-common tracked controls. Maintenance allocates `0x8000..0xffff` for
its targeted/verification paths and the one registered repair transaction.
Those are existing protocol allocation ranges, not scheduler owner domains. The
scheduler only copies and echoes tokens in eviction, `TX_DONE`, `TX_FAILED`, and
`TX_EXPIRED`; token zero remains anonymous and no third range exists. This makes a synchronously returned
nonzero eviction token unambiguous.

`tavrn_router` and its producers must apply the complete `BEARER-06` matrix in
the profile without moving protocol semantics into the radio driver. In
particular, DATA→HACK, RREQ→RREP, RREP→RREP_ACK, and bootstrap HELLO→SYNC_OFFER
retain one fixed first-copy response key/deadline; duplicate repeated copies do
not recreate or postpone the pending response. Ordinary HELLO cadence/sequence
and TC fact epoch remain enqueue-time admission semantics. AODV sent/ACK state
and mentorship response/page waits begin only at matching TX_DONE.

### 5.3 Scheduler events and API

The target event extends the current RX event rather than creating a second
scheduler:

```c
typedef enum ble_mesh_sched_event_type {
    BLE_MESH_SCHED_EVENT_NONE = 0,
    BLE_MESH_SCHED_EVENT_RX_ADV,
    BLE_MESH_SCHED_EVENT_TX_DONE,
    BLE_MESH_SCHED_EVENT_TX_FAILED,
    BLE_MESH_SCHED_EVENT_TX_EXPIRED,
    BLE_MESH_SCHED_EVENT_RADIO_FAULT,
    BLE_MESH_SCHED_EVENT_SERVICE_FAULT,
} ble_mesh_sched_event_type_t;

typedef enum ble_mesh_sched_fault {
    BLE_MESH_SCHED_FAULT_NONE = 0,
    BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT,
    BLE_MESH_SCHED_FAULT_RADIO_INVALID_ARGUMENT,
    BLE_MESH_SCHED_FAULT_POLL_OVERRUN,
    BLE_MESH_SCHED_FAULT_QUEUE_CORRUPT,
    BLE_MESH_SCHED_FAULT_INTERNAL_STATE,
} ble_mesh_sched_fault_t;

typedef struct ble_mesh_sched_event {
    ble_mesh_sched_event_type_t type;
    uint8_t channel;
    uint8_t rssi_magnitude_db;
    uint8_t adv_addr[6];
    uint8_t adv_len;
    uint8_t adv_data[BLE_ADV_MAX_DATA];
    ble_mesh_tx_token_t tx_token;
    uint8_t tx_requested_channel_mask;
    uint8_t tx_completed_channel_mask;
    uint8_t tx_requested_sweep_count;
    uint8_t tx_attempted_sweep_count;
    uint8_t tx_completed_channel_masks[BLE_RADIO_MAX_ADV_SWEEPS];
    ble_mesh_sched_fault_t fault;
} ble_mesh_sched_event_t;

typedef enum ble_mesh_sched_enqueue_status {
    BLE_MESH_SCHED_ENQUEUE_OK = 0,
    BLE_MESH_SCHED_ENQUEUE_FULL,
    BLE_MESH_SCHED_ENQUEUE_INVALID,
    BLE_MESH_SCHED_ENQUEUE_TOO_LONG,
    BLE_MESH_SCHED_ENQUEUE_REJECTED,
} ble_mesh_sched_enqueue_status_t;

typedef struct ble_mesh_sched_enqueue_result {
    ble_mesh_sched_enqueue_status_t status;
    ble_mesh_tx_token_t accepted_token;
    ble_mesh_tx_token_t evicted_token;
} ble_mesh_sched_enqueue_result_t;

ble_mesh_sched_enqueue_result_t ble_mesh_scheduler_enqueue_ex(
    ble_mesh_scheduler_t *sched, const ble_mesh_tx_item_t *item);

void ble_mesh_scheduler_init(ble_mesh_scheduler_t *sched, uint32_t now_ms,
                             const uint8_t local_adva[6]);
int ble_mesh_scheduler_copy_local_adva(
    const ble_mesh_scheduler_t *sched, uint8_t out_adva[6]);
int ble_mesh_scheduler_poll(ble_mesh_scheduler_t *sched, uint32_t now_ms,
                            ble_mesh_sched_event_t *event);
```

The public scheduler state adds `uint8_t local_adva[6]`, the queue's wrapped
insertion ordinal, the promoted-custody bypass count, and latched
radio/service-fault state. Initialization copies identity rather than retaining
the input pointer. A NULL/invalid local AdvA leaves the scheduler unstarted and
is a boot-fatal integration error. `copy_local_adva` returns one and copies all
six bytes only for a started scheduler; NULL/unstarted input returns zero and
does not write the output.

The existing `ble_mesh_scheduler_enqueue()` stays as a legacy compatibility
wrapper until all wire-v1 tests prove the extraction. TX classification uses the
radio result exactly:

- completed mask zero emits `TX_FAILED`, never increments a link attempt, and a
  tracked DATA token becomes LOCAL_TX_NOT_ATTEMPTED;
- completed mask nonzero emits `TX_DONE` with that mask. Link-v2 increments the
  attempt and starts the HACK deadline even if `fault` names a later-channel
  timeout; it must never relabel that DATA LOCAL_TX_NOT_ATTEMPTED; and
- a non-NONE fault is latched after the TX event. The following poll emits
  `RADIO_FAULT` and the scheduler accepts no further radio work. RX restoration
  occurs only through a successful bounded listen operation.

`RX_ADV` populates only its channel/RSSI/AdvA/AdvData fields. `TX_DONE` and
`TX_FAILED` populate `tx_token`, requested/completed channel masks, requested
and attempted sweep counts, per-sweep completion masks, and `fault`; a partial
`TX_DONE` therefore carries the full requested evidence, its nonzero completed
subset, and the later-channel radio fault. The following `RADIO_FAULT` repeats
that causal token/masks/fault. A bounded RX-restore failure immediately following
TX also carries the preceding TX evidence; an unrelated init/listen/snapshot
failure uses token zero and zero physical evidence.
`TX_EXPIRED` carries the queued token, requested mask, and requested sweep count;
attempted/completed counts and masks plus fault are zero.
`SERVICE_FAULT` uses token/masks zero and one of POLL_OVERRUN,
QUEUE_CORRUPT, or INTERNAL_STATE. Every producer zeroes all inactive fields.
Radio NO_EVENT produces no scheduler event, CRC_DROP increments the invalid-RX
counter and produces no RX event, and an unexpected driver INVALID_ARGUMENT
maps to RADIO_INVALID_ARGUMENT and fails closed.
Once either fault is latched, enqueue returns `BLE_MESH_SCHED_ENQUEUE_INVALID`.
The scheduler first emits any existing selected partial result as TX_DONE plus
fault, then emits the latched fault on the following poll; no further radio
operation is started. There is no queue-owner traversal or global reset callback.
Existing link and maintenance fault handlers retire only state they already own.
Every emitted TX diagnostic copies the same requested mask, completed mask, and
fault; a success-only log may not erase partial/fault state.

`TX_DONE` means at least one selected channel completed; it does not mean peer
receipt. Link-v2 starts a HACK deadline from the post-poll observed TX_DONE
time, never from queue admission or the pre-poll scheduler-selection time.
AdvA is the six bytes copied from the received PDU; the identity contract owns
its canonical display and suffix interpretation.

RSSI has one representation across driver, scheduler, link events, counters,
and tests: `uint8_t rssi_magnitude_db`, where received power for policy is
derived as `-(int16_t)rssi_magnitude_db` dBm. No public seam also stores a signed
`rssi_dbm` copy.

All scheduler-event data is by value and belongs to the caller after `poll`
returns. There is no scheduler event heap or hidden event queue.

### 5.4 Dedicated bounded mesh service task

One dedicated high-priority mesh task owns scheduler, link, router, timer, and
their mutable queues. Its loop performs one bounded scheduler operation, handles
the copied result, drains one due link/router transition, and begins the next
poll within `timer.scheduler_poll_max_ms=2` after the bounded radio operation
returns. It performs no blocking logging, display update, shell/serial output,
patient processing, or application callback. The initial bounded RX-start return
seeds the same pre-poll gate before the first loop iteration, so startup cannot
receive a one-operation exemption from the service-gap contract.

Other tasks communicate through one fixed eight-entry by-value command queue
into the mesh task and the existing fixed eight-entry node/application event
queue out. Diagnostics are copied into a separate fixed eight-entry best-effort
log queue; log overflow increments a counter and cannot delay mesh service. This
is the complete concurrency design, not a general actor/executor framework. A
separate routed-only one-slot retry-log mailbox copies a complete
`RETRY_EXHAUSTED` event for the lower-priority logger. It retains the oldest
pending event, drops and saturating-counts a newer one, and is intentionally not
part of fault-diagnostic pending/progress/wait liveness; a healthy link break
therefore cannot turn a logger drain delay into a scheduler fault.

The task measures operation-return to next-poll-start. Exceeding the 2 ms bound,
queue corruption, or an impossible scheduler state emits `SERVICE_FAULT`,
latches the mesh path fail-closed, and stops new route/DATA admission. A radio
operation fault similarly emits `RADIO_FAULT`. Link-v2 marks every custody slot
non-eligible immediately and surfaces one typed owned terminal outcome per
subsequent tick until all custody slots are returned. Recovery requires an
explicit firmware restart in this PoC. On a healthy cycle the preemptible logger yield consumes only the portion
of the poll-gap budget not already spent on copied-event handling, one due
transition, application admission, and dispatch. If that work consumes the
voluntary-yield budget, the mesh task proceeds directly to the next pre-poll
gate. Faulted cycles retain the bounded yield so terminal evidence can drain.

Static include/call checks forbid every mesh path from calling the unbounded
compatibility names. The typed init, idle, listen/restore/hop, snapshot, and TX
operations are the only radio imports allowed in `ble_mesh_scheduler.c`.

## 6. Firmware node facade

Exactly one selected binding owns all static firmware-node state and exposes
initialization, scheduler-event input, tick, application submission, and
application-event polling. Phase 1 does not compile a routed node binding: it
uses the dedicated link harness and the complete link API in section 8. The
Phase 2 red-test API contract freezes the exact routed `tron_node` status/event
union after AODV action and route-event types are known; this document does not
predeclare a generic failure event that would collapse the typed link outcomes.

Inputs accepted by the eventual facade are copied before return, and successful
poll moves a by-value event to the caller. Host multi-node tests instantiate the
underlying legacy/link/AODV/router state structs directly rather than sharing
the firmware singleton. `tron_application_data_t` itself is already frozen in
section 8 so Phase 1 application-byte/capacity tests do not depend on guessed
AODV types.

Legacy and routed bindings expose the same application seam but do not promise
the same delivery semantics. A legacy submit is flood delivery. A routed submit
requires a route/discovery and has no broadcast DATA fallback.

## 7. Legacy branch contract

`legacy_flood_node` owns the state and behavior currently embedded in `main.c`:

- wire-v1 decode and network admission;
- PING/PONG semantic admission before dedupe;
- the 16-entry `{net_id,msg_type,src,seq24}` duplicate cache;
- addressed local PING/PONG handling;
- TTL decrement, jitter, bounded relay scheduling, and counters.

It may include only `tron_mesh_packet.h`, `tron_mesh_dedupe.h`,
`tron_mesh_pingpong.h`, `ble_mesh_scheduler.h`, generated build/timer config,
and the common node facade. It must not include any routed wire-v2, AODV,
TAVRN, patient, or repair header.

Balanced legacy timing is frozen to the clean baseline unless a separately
reviewed shared-profile change proves legacy regression: 50 ms scheduler dwell,
200 ms relay spacing, 20--120 ms relay jitter, 10,000 ms dedupe lifetime,
2,000 ms PING interval, 1,500 ms PING timeout, 5,000 ms stats interval, and a
2 ms application-loop delay.

## 8. Routed link-v2 boundary

Phase 1 freezes the complete public bearer/link types below. These are fixed
storage contracts, not wire offsets; `tavrn_ble_wire_v2.md` remains the byte
authority.

```c
#define TAVRN_ADVA_LEN                 6u
#define TAVRN_LINK_APP_BYTES           10u
#define TAVRN_LINK_CONTROL_PDU_MAX     24u
#define TAVRN_RX_CANDIDATE_TOKEN_NONE  0u
#define TAVRN_LINK_CUSTODY_CAPACITY     4u
#define TAVRN_CUSTODY_SLOT_NONE         0xffu
#define TAVRN_LINK_DATA_DEDUPE_CAPACITY 16u
#define TAVRN_LINK_FLOOD_DEDUPE_CAPACITY 16u

typedef struct tavrn_adva {
    uint8_t bytes[TAVRN_ADVA_LEN];
} tavrn_adva_t;

typedef enum tavrn_identity_width {
    TAVRN_IDENTITY_SID8 = 1,
    TAVRN_IDENTITY_SID16 = 2,
} tavrn_identity_width_t;

typedef struct tavrn_logical_id {
    tavrn_identity_width_t width;
    uint16_t value;
} tavrn_logical_id_t;

typedef struct tavrn_direct_peer {
    tavrn_logical_id_t logical_id;
    tavrn_adva_t adva;
} tavrn_direct_peer_t;

typedef struct tron_application_data {
    tavrn_logical_id_t final_destination;
    uint8_t app_kind;
    uint8_t app_source;
    uint8_t urgent;
    uint8_t app_len;
    uint8_t app_bytes[TAVRN_LINK_APP_BYTES];
} tron_application_data_t;

typedef enum tavrn_data_ownership {
    TAVRN_DATA_ORIGINATED = 0,
    TAVRN_DATA_TRANSIT = 1,
} tavrn_data_ownership_t;

typedef struct tavrn_link_data {
    tavrn_logical_id_t origin;
    tavrn_logical_id_t final_destination;
    uint16_t data_seq;
    uint8_t ttl;
    uint8_t hops;
    uint8_t app_kind;
    uint8_t app_source;
    uint8_t urgent;
    uint8_t app_len;
    uint8_t app_bytes[TAVRN_LINK_APP_BYTES];
    tavrn_data_ownership_t ownership;
} tavrn_link_data_t;

typedef enum tavrn_hack_status {
    TAVRN_HACK_ACCEPTED = 0,
    TAVRN_HACK_DUPLICATE = 1,
    TAVRN_HACK_BUSY = 2,
    TAVRN_HACK_REJECTED = 3,
} tavrn_hack_status_t;

typedef enum tavrn_wire_type {
    TAVRN_WIRE_E_RREQ = 0x01,
    TAVRN_WIRE_E_RREP = 0x02,
    TAVRN_WIRE_E_RERR = 0x03,
    TAVRN_WIRE_HELLO = 0x04,
    TAVRN_WIRE_SYNC_OFFER = 0x05,
    TAVRN_WIRE_SYNC_PULL = 0x06,
    TAVRN_WIRE_SYNC_DATA = 0x07,
    TAVRN_WIRE_TC_UPDATE = 0x08,
    TAVRN_WIRE_E_RREP_ACK = 0x09,
    TAVRN_WIRE_DATA = 0x10,
    TAVRN_WIRE_HACK = 0x11,
    TAVRN_WIRE_FLOOD = 0x12,
} tavrn_wire_type_t;

typedef enum tavrn_codec_result {
    TAVRN_CODEC_OK = 0,
    TAVRN_CODEC_INVALID_ARGUMENT,
    TAVRN_CODEC_MALFORMED_WRAPPER,
    TAVRN_CODEC_MALFORMED_EXACT_LENGTH,
    TAVRN_CODEC_MALFORMED_FLAGS,
    TAVRN_CODEC_MALFORMED_FIELD,
    TAVRN_CODEC_FOREIGN_NETWORK,
    TAVRN_CODEC_UNSUPPORTED_TYPE,
    TAVRN_CODEC_IDENTITY_CONFLICT,
    TAVRN_CODEC_OUTPUT_TOO_SMALL,
} tavrn_codec_result_t;

typedef int (*tavrn_codec_identity_conflict_fn)(
    void *context, const tavrn_logical_id_t *logical_id,
    const tavrn_adva_t *direct_adva_or_null);

typedef struct tavrn_codec_config {
    uint8_t network_id;
    tavrn_direct_peer_t local_peer;
    tavrn_codec_identity_conflict_fn identity_conflict;
    void *identity_context;
} tavrn_codec_config_t;

typedef struct tavrn_codec_data {
    tavrn_logical_id_t immediate_receiver;
    tavrn_link_data_t data;
} tavrn_codec_data_t;

typedef struct tavrn_codec_hack {
    tavrn_logical_id_t immediate_receiver;
    tavrn_logical_id_t data_origin;
    tavrn_logical_id_t final_destination;
    uint16_t data_seq;
    uint8_t app_kind;
    uint8_t app_source;
    tavrn_hack_status_t status;
} tavrn_codec_hack_t;

#define TAVRN_FLOOD_BODY_MAX 12u

typedef struct tavrn_codec_flood {
    tavrn_logical_id_t origin;
    uint16_t flood_seq;
    uint8_t ttl;
    uint8_t hops;
    uint8_t urgent;
    uint8_t flood_class;
    uint8_t body_len;
    uint8_t body[TAVRN_FLOOD_BODY_MAX];
} tavrn_codec_flood_t;

typedef struct tavrn_validated_control {
    tavrn_wire_type_t type;
    uint8_t pdu_len;
    uint8_t pdu[TAVRN_LINK_CONTROL_PDU_MAX];
} tavrn_validated_control_t;

typedef struct tavrn_decoded_frame {
    tavrn_wire_type_t type;
    uint8_t network_id;
    tavrn_direct_peer_t transmitter;
    union {
        tavrn_codec_data_t data;
        tavrn_codec_hack_t hack;
        tavrn_codec_flood_t flood;
        tavrn_validated_control_t control;
    } detail;
} tavrn_decoded_frame_t;

tavrn_codec_result_t tavrn_wire_v2_decode(
    const tavrn_codec_config_t *config, const uint8_t outer_adva[6],
    const uint8_t *adv_data, size_t adv_len,
    tavrn_decoded_frame_t *frame_out);

tavrn_codec_result_t tavrn_wire_v2_encode(
    const tavrn_codec_config_t *config,
    const tavrn_decoded_frame_t *frame,
    uint8_t *adv_data_out, size_t adv_capacity, size_t *adv_len_out);

typedef uint16_t tavrn_rx_candidate_token_t;

typedef enum tavrn_rx_decision {
    TAVRN_RX_ACCEPTED = 0,
    TAVRN_RX_BUSY,
    TAVRN_RX_REJECTED,
} tavrn_rx_decision_t;

typedef enum tavrn_link_send_status {
    TAVRN_LINK_SEND_OK = 0,
    TAVRN_LINK_SEND_LOCAL_NOT_ATTEMPTED,
    TAVRN_LINK_SEND_BUSY,
    TAVRN_LINK_SEND_NO_SLOT,
    TAVRN_LINK_SEND_INVALID,
    TAVRN_LINK_SEND_IDENTITY_CONFLICT,
    TAVRN_LINK_SEND_MESH_FAULTED,
} tavrn_link_send_status_t;

typedef enum tavrn_link_init_status {
    TAVRN_LINK_INIT_OK = 0,
    TAVRN_LINK_INIT_INVALID_ARGUMENT,
    TAVRN_LINK_INIT_INVALID_LOCAL_IDENTITY,
    TAVRN_LINK_INIT_INVALID_CONFIG,
} tavrn_link_init_status_t;

typedef enum tavrn_link_resolve_status {
    TAVRN_LINK_RESOLVE_OK = 0,
    TAVRN_LINK_RESOLVE_TOKEN_INVALID,
    TAVRN_LINK_RESOLVE_ALREADY_RESOLVED,
    TAVRN_LINK_RESOLVE_INVALID_DECISION,
    TAVRN_LINK_RESOLVE_MESH_FAULTED,
} tavrn_link_resolve_status_t;

typedef enum tavrn_link_step_status {
    TAVRN_LINK_STEP_NO_EVENT = 0,
    TAVRN_LINK_STEP_EVENT,
    TAVRN_LINK_STEP_ADDITIONAL_DATA_BUSY,
    TAVRN_LINK_STEP_CANDIDATE_TIMEOUT_BUSY,
    TAVRN_LINK_STEP_INVALID,
} tavrn_link_step_status_t;

typedef enum tavrn_link_event_type {
    TAVRN_LINK_EVENT_NONE = 0,
    TAVRN_LINK_EVENT_RX_DATA_CANDIDATE,
    TAVRN_LINK_EVENT_RX_CONTROL,
    TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED,
    TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED,
    TAVRN_LINK_EVENT_CUSTODY_BUSY_EXPIRED,
    TAVRN_LINK_EVENT_CUSTODY_REJECTED,
    TAVRN_LINK_EVENT_RETRY_EXHAUSTED,
    TAVRN_LINK_EVENT_RADIO_FAULT_TERMINAL,
    TAVRN_LINK_EVENT_SERVICE_FAULT_TERMINAL,
} tavrn_link_event_type_t;

typedef enum tavrn_local_tx_reason {
    TAVRN_LOCAL_TX_ENCODE_FAILED = 0,
    TAVRN_LOCAL_TX_SCHEDULER_FULL,
    TAVRN_LOCAL_TX_SCHEDULER_EVICTED,
    TAVRN_LOCAL_TX_RADIO_FAILED,
    TAVRN_LOCAL_TX_DEADLINE_EXPIRED,
} tavrn_local_tx_reason_t;

typedef enum tavrn_mesh_fault_reason {
    TAVRN_MESH_FAULT_NONE = 0,
    TAVRN_MESH_FAULT_RADIO_AFTER_TX,
    TAVRN_MESH_FAULT_RADIO_UNAVAILABLE,
    TAVRN_MESH_FAULT_POLL_OVERRUN,
    TAVRN_MESH_FAULT_INTERNAL_STATE,
} tavrn_mesh_fault_reason_t;

typedef struct tavrn_rx_data_candidate {
    tavrn_rx_candidate_token_t token;
    tavrn_direct_peer_t transmitter;
    uint8_t rssi_magnitude_db;
    tavrn_link_data_t data;
    uint32_t deadline_ms;
} tavrn_rx_data_candidate_t;

typedef struct tavrn_rx_control_event {
    tavrn_direct_peer_t transmitter;
    uint8_t rssi_magnitude_db;
    tavrn_validated_control_t control;
} tavrn_rx_control_event_t;

typedef struct tavrn_owned_data_event {
    tavrn_direct_peer_t next_hop;
    tavrn_link_data_t data;
    uint8_t requested_channel_mask;
    uint8_t completed_channel_mask;
    uint8_t attempt_count;
    uint8_t busy_response_count;
    uint32_t first_tx_ms;
    uint32_t last_tx_ms;
    uint32_t final_deadline_ms;
    tavrn_local_tx_reason_t local_reason;
    tavrn_mesh_fault_reason_t mesh_fault_reason;
} tavrn_owned_data_event_t;

typedef struct tavrn_transferred_data_event {
    tavrn_direct_peer_t next_hop;
    tavrn_link_data_t data;
    tavrn_hack_status_t status;
    uint8_t requested_channel_mask;
    uint8_t completed_channel_mask;
    uint8_t attempt_count;
    uint32_t first_tx_ms;
    uint32_t last_tx_ms;
} tavrn_transferred_data_event_t;

typedef struct tavrn_link_event {
    tavrn_link_event_type_t type;
    union {
        tavrn_rx_data_candidate_t candidate;
        tavrn_rx_control_event_t control;
        tavrn_transferred_data_event_t transferred_data;
        tavrn_owned_data_event_t owned_data;
    } detail;
} tavrn_link_event_t;

typedef struct tavrn_link_config {
    tavrn_direct_peer_t local_peer;
    uint8_t network_id;
    uint8_t hack_max_attempts;
    uint8_t busy_max_responses;
    uint32_t hack_response_ms;
    uint32_t hack_turnaround_ms;
    uint32_t retry_backoff_ms;
    uint32_t busy_backoff_ms;
    uint32_t data_forward_deadline_ms;
    uint32_t candidate_resolve_ms;
    uint32_t data_dedupe_ms;
    uint32_t flood_dedupe_ms;
    uint32_t flood_jitter_min_ms;
    uint32_t flood_jitter_max_ms;
} tavrn_link_config_t;

typedef struct tavrn_link_counters {
    uint32_t rx_candidate;
    uint32_t rx_candidate_accepted;
    uint32_t rx_candidate_busy;
    uint32_t rx_candidate_rejected;
    uint32_t rx_candidate_timeout_busy;
    uint32_t rx_additional_data_busy;
    uint32_t rx_committed_duplicate;
    uint32_t rx_control;
    uint32_t rx_malformed;
    uint32_t rx_wrong_next_hop;
    uint32_t rx_identity_conflict;
    uint32_t hack_accepted;
    uint32_t hack_duplicate;
    uint32_t hack_busy;
    uint32_t hack_rejected;
    uint32_t hack_unmatched;
    uint32_t hack_enqueue_failed;
    uint32_t tx_admitted;
    uint32_t tx_done;
    uint32_t tx_partial_done;
    uint32_t tx_failed;
    uint32_t retry_due;
    uint32_t custody_promoted;
    uint32_t custody_dispatch_blocked;
    uint32_t custody_transferred;
    uint32_t local_tx_not_attempted;
    uint32_t custody_busy_expired;
    uint32_t custody_rejected;
    uint32_t retry_exhausted;
    uint32_t radio_fault_terminal;
    uint32_t service_fault_terminal;
} tavrn_link_counters_t;

typedef enum tavrn_custody_phase {
    TAVRN_CUSTODY_FREE = 0,
    TAVRN_CUSTODY_READY_NOT_ELIGIBLE,
    TAVRN_CUSTODY_ACTIVE_QUEUED,
    TAVRN_CUSTODY_ACTIVE_WAIT_HACK,
    TAVRN_CUSTODY_BUSY_WAIT,
    TAVRN_CUSTODY_FAULT_PENDING,
} tavrn_custody_phase_t;

typedef struct tavrn_data_dedupe_entry {
    uint8_t valid;
    uint8_t custody_pinned;
    tavrn_logical_id_t origin;
    uint16_t data_seq;
    uint8_t app_kind;
    uint8_t app_source;
    uint32_t expires_at_ms;
} tavrn_data_dedupe_entry_t;

typedef struct tavrn_flood_dedupe_entry {
    uint8_t valid;
    uint8_t frame_type;
    tavrn_logical_id_t origin;
    uint16_t sequence;
    uint32_t expires_at_ms;
} tavrn_flood_dedupe_entry_t;

typedef struct tavrn_custody_slot {
    tavrn_custody_phase_t phase;
    tavrn_direct_peer_t next_hop;
    tavrn_link_data_t data;
    ble_mesh_tx_token_t scheduler_token;
    uint8_t attempt_count;
    uint8_t busy_response_count;
    uint8_t attempt_requested_channel_mask;
    uint8_t attempt_completed_channel_mask;
    uint8_t adv_len;
    uint8_t adv_data[BLE_ADV_MAX_DATA];
    uint32_t transaction_deadline_ms;
    uint32_t admission_order;
    uint32_t response_deadline_ms;
    uint32_t retry_not_before_ms;
    uint32_t first_tx_ms;
    uint32_t last_tx_ms;
} tavrn_custody_slot_t;

typedef struct tavrn_link_v2 {
    ble_mesh_scheduler_t *scheduler;
    tavrn_link_config_t config;
    tavrn_link_counters_t counters;
    tavrn_custody_slot_t custody[TAVRN_LINK_CUSTODY_CAPACITY];
    tavrn_data_dedupe_entry_t
        data_dedupe[TAVRN_LINK_DATA_DEDUPE_CAPACITY];
    tavrn_flood_dedupe_entry_t
        flood_dedupe[TAVRN_LINK_FLOOD_DEDUPE_CAPACITY];
    tavrn_rx_data_candidate_t candidate;
    uint16_t next_scheduler_token;
    uint16_t next_candidate_token;
    uint32_t next_custody_order;
    uint8_t active_custody_index;
    uint8_t candidate_valid;
    uint8_t mesh_fault_latched;
    tavrn_mesh_fault_reason_t latched_mesh_fault_reason;
} tavrn_link_v2_t;

tavrn_link_init_status_t tavrn_link_v2_init(
    tavrn_link_v2_t *link, ble_mesh_scheduler_t *sched,
    const tavrn_link_config_t *config, uint32_t now_ms);
tavrn_link_send_status_t tavrn_link_v2_send_unicast(
    tavrn_link_v2_t *link, const tavrn_direct_peer_t *next_hop,
    const tavrn_link_data_t *data, uint32_t now_ms,
    tavrn_link_event_t *local_outcome);
tavrn_link_send_status_t tavrn_link_v2_send_flood(
    tavrn_link_v2_t *link, const tavrn_codec_flood_t *flood,
    uint32_t now_ms, tavrn_link_event_t *local_outcome);
tavrn_link_resolve_status_t tavrn_link_v2_resolve_rx(
    tavrn_link_v2_t *link, tavrn_rx_candidate_token_t token,
    tavrn_rx_decision_t decision, uint32_t now_ms,
    tavrn_link_event_t *local_outcome);
tavrn_link_resolve_status_t tavrn_link_v2_release_rx_custody(
    tavrn_link_v2_t *link, const tavrn_link_data_t *data,
    uint32_t now_ms);
tavrn_link_step_status_t tavrn_link_v2_on_scheduler_event(
    tavrn_link_v2_t *link, const ble_mesh_sched_event_t *input,
    uint32_t now_ms, tavrn_link_event_t *output);
tavrn_link_step_status_t tavrn_link_v2_tick(
    tavrn_link_v2_t *link, uint32_t now_ms, tavrn_link_event_t *output);
tavrn_link_step_status_t tavrn_link_v2_dispatch(
    tavrn_link_v2_t *link, uint32_t now_ms,
    tavrn_link_event_t *local_outcome);
const tavrn_link_counters_t *tavrn_link_v2_counters(
    const tavrn_link_v2_t *link);
```

`tavrn_logical_id_t` is the route/origin/destination/dedupe identity. SID8 uses
`width=SID8` with the high byte of `value` zero. A
`tavrn_direct_peer_t` is legal only for self, an observed immediate transmitter,
an immediate next hop, or HACK correlation. For a direct peer, SID16 is always
`AdvA[0] | (AdvA[1] << 8)` and SID8 is `AdvA[0]`; there is no routed
logical-ID build input. A valid direct full-AdvA N=1 announcement
creates/refreshes that exact logical-ID-to-AdvA binding. Remote
route origins, destinations, and dedupe keys remain logical IDs and do not grow
fabricated full identities.

Public application arrays are ten bytes. Patient `app_kind=01` is exactly seven
bytes in both modes. SID16 DATA permits at most seven application bytes because
of the 24-byte PDU; SID8 opaque test `app_kind=7f` permits `0..10`. Every other
mode/type/length combination is `MALFORMED_EXACT_LENGTH` or `MALFORMED_FIELD`
as the wire contract requires. `urgent` is exactly zero or one; TTL and hops are each
`0..15`; unused application/body/control tails are zeroed.

The codec consumes caller-owned AdvA/AdvData buffers and zero-initializes the
by-value output before validation. DATA, HACK, and FLOOD use typed members.
Every other supported Phase-0 control vector is fully wrapper/length/flag/field
validated and stored byte-for-byte in `detail.control`; encoding that member
round-trips the vector without a Phase-1 AODV signature. The identity-conflict
callback is NULL only for collision-free SID16 operation; SID8 requires it, and
a nonzero callback result returns `TAVRN_CODEC_IDENTITY_CONFLICT`. The callback's
third argument is the outer/direct AdvA for direct binding checks and NULL for
remote logical-only fields. Encoders never truncate and write no output on
error. HACK is consumed internally by link-v2
and is not surfaced as `RX_CONTROL`; other raw controls reach Phase 2 through
the generic validated representation.

For generic control, `frame.type`, `detail.control.type`, and the validated PDU
type byte must match; `pdu_len` is the exact custom-PDU length and unused bytes
are zero. Decode returns no partially typed control on error. Encode revalidates
the complete member, wrapper budget, selected identity width, and output capacity
instead of trusting a prior decode.

Initialization zeroes all arrays/state/counters, then sets
`active_custody_index=TAVRN_CUSTODY_SLOT_NONE` and
`latched_mesh_fault_reason=TAVRN_MESH_FAULT_NONE`. It validates network `01..fe`,
the full local direct-peer relationship, all timer values below the 32-bit half
range, exact profile attempt/response limits, and
`flood_jitter_min_ms <= flood_jitter_max_ms`. It returns a typed failure and
leaves the instance unusable rather than applying local defaults.

### 8.1 Two-phase inbound DATA admission

Receiver processing is intentionally two phase:

1. Link-v2 validates the raw/wire wrapper, full immediate transmitter,
   identities, logical next hop, exact application shape, and policy that is
   link-local. It forms the dedupe key but does not mutate the committed dedupe
   cache and does not enqueue HACK.
2. If that key is already committed, link-v2 emits no candidate and
   automatically attempts one `DUPLICATE` HACK for this received logical frame.
   It never repeats route custody or application delivery.
3. Otherwise link-v2 retains one candidate copy, allocates a nonzero opaque
   token, and returns `RX_DATA_CANDIDATE`. The candidate remains link-owned.
4. The router synchronously reserves a non-evictable transit/pending custody
   slot, or the final application path reserves one delivery-event slot. Only
   after that reservation succeeds does it call
   `tavrn_link_v2_resolve_rx(token, TAVRN_RX_ACCEPTED)`.
5. `ACCEPTED` atomically commits the DATA dedupe key and attempts an ACCEPTED
   HACK. `BUSY` and `REJECTED` do not commit dedupe and attempt the corresponding
   HACK. Every decision releases the candidate slot.

If HACK scheduler admission fails after ACCEPTED, `hack_enqueue_failed`
increments but committed custody is not rolled back; a later received duplicate
again auto-attempts one DUPLICATE HACK.

An accepted transit key is pinned in committed dedupe while router custody is
held. After successful onward transfer or explicit terminal failure, the router
calls `tavrn_link_v2_release_rx_custody()`; the entry remains committed through
its minimum retention deadline and then becomes reclaimable. Final-destination
acceptance is not custody-pinned because delivery-event storage was reserved
before ACCEPTED. Release with a missing/nonmatching key fails without mutation.

There is exactly one candidate slot. The routed node's production handler must
reserve storage and call `resolve_rx` before returning from the
`RX_DATA_CANDIDATE` dispatch. A token is valid exactly once and only for the
currently held candidate. Failure to reserve storage resolves BUSY for transient
capacity or REJECTED for permanent semantic/policy failure. Thus decoding or
temporary candidate storage can never prematurely assert custody.

`timer.link_candidate_resolve_ms=10` is a defensive deadline, not permission
for normal asynchronous resolution. While a candidate exists, TX_DONE,
TX_FAILED, matching HACK, validated control RX, sender custody timers, and
retries continue normally. An additional valid new DATA receives one BUSY HACK
without candidate creation or dedupe commit and returns
`TAVRN_LINK_STEP_ADDITIONAL_DATA_BUSY`; committed duplicates still receive their
automatic DUPLICATE HACK. At the defensive deadline, `tick` releases the
candidate without dedupe, attempts BUSY HACK, increments the timeout counter,
and returns `TAVRN_LINK_STEP_CANDIDATE_TIMEOUT_BUSY`.

Every public operation with a `tavrn_link_event_t` output (`send_unicast`,
`send_flood`, `resolve_rx`, scheduler-event handling, `tick`, and `dispatch`)
has exactly one output and zeroes it before work. Any synchronous HACK/control
enqueue that evicts one tracked custody item reports that item as
LOCAL_TX_NOT_ATTEMPTED in the output. Candidate resolution/timeout progress is
reported by the typed return status, so it does not consume a second event slot.
An operation performs at most one scheduler admission and therefore cannot
create two tracked evictions. No candidate timeout, BUSY/DUPLICATE response, or
ACCEPTED resolution can hide an eviction outcome.

`tick` executes at most one due state transition per call, with candidate
defensive cleanup before sender-custody expiry/retry at the same timestamp. The
caller repeats `tick(now_ms, ...)` until `NO_EVENT`; deadlines remain due between
calls, so timeout BUSY cleanup and a simultaneous custody outcome are serialized
without loss.

### 8.2 Sender custody and typed outcomes

Four logical custody slots remain, but physical attempt service is serialized.
`send_unicast` copies accepted DATA into `READY_NOT_ELIGIBLE`, assigns a stable
`admission_order`, starts the 5000 ms selected-next-hop transaction deadline,
and does not directly place it in the scheduler. When
`active_custody_index==TAVRN_CUSTODY_SLOT_NONE`, `dispatch` selects the oldest
admission-order slot whose retry/BUSY delay is due, promotes exactly that slot to
`ACTIVE_QUEUED`, sets its current requested mask and zero completed mask, and
enqueues its one initial/retry attempt as CUSTODY_DATA. The other three slots
remain link-local and cannot be selected or evicted by the physical scheduler.

An active attempt resolves only on TX_FAILED/eviction-before-completion,
matching HACK, response timeout, transaction deadline, or terminal mesh fault.
TX_DONE moves it to `ACTIVE_WAIT_HACK` and keeps it the sole active slot. A
no-response timeout below three attempts returns it to READY_NOT_ELIGIBLE with
the retry due immediately and clears active; deterministic oldest-due dispatch
therefore selects that same oldest transaction before later admissions. BUSY
clears active, changes the slot to `BUSY_WAIT`, and sets its retry-not-before to
the end of the 500 ms delay. Every waiting slot remains governed by the original
5000 ms transaction deadline and returns CUSTODY_BUSY_EXPIRED/explicit local
failure on expiry.

This serialization is what makes the future bounds composable: from a
transaction's first promotion to eligibility, each attempt is bounded by
`(2 + 1) * (1000 + 14 + 2) = 3048 ms`. Three attempts plus three 250 ms
response windows make an unconstrained wall of `3 * 3048 + 750 = 9894 ms`.
The fixed 5000 ms transaction deadline is absolute, so it can terminate custody
before every permitted retry; three is a ceiling, not a guarantee. Time spent
READY behind older work or in BUSY backoff is not a separate attempt guarantee.

- The active slot remains link-owned through scheduler queueing, TX, HACK wait,
  and bounded retry.
- Matching ACCEPTED or DUPLICATE returns `CUSTODY_TRANSFERRED` with a by-value
  informational DATA copy and releases the sender slot. The event does not own
  DATA because custody now belongs to the correlated next hop. A transit router
  uses this event to release its inbound/pending slot and unpin committed RX
  dedupe; an origin uses it as the per-hop completion observation.
- A valid BUSY retains custody and starts the bounded BUSY policy. Three BUSY
  responses or the selected-next-hop deadline returns
  `CUSTODY_BUSY_EXPIRED`.
- A valid REJECTED returns `CUSTODY_REJECTED` immediately.
- Encode failure, local deadline, scheduler-full admission, scheduler eviction,
  or scheduler TX_FAILED with completed mask zero returns
  `LOCAL_TX_NOT_ATTEMPTED`; attempt count remains unchanged.
- Scheduler TX_DONE with any nonzero completed mask increments attempt count,
  stores both requested and completed masks, and arms HACK even when a
  later-channel radio fault is also present. It is never
  LOCAL_TX_NOT_ATTEMPTED.
- Only a third completed no-response attempt followed by its deadline returns
  `RETRY_EXHAUSTED`; the absolute custody deadline may instead terminate before
  that ceiling is reached. `RETRY_EXHAUSTED` is the only one of these outcomes
  that reports a broken link.

For `LOCAL_TX_NOT_ATTEMPTED`, `CUSTODY_BUSY_EXPIRED`, `CUSTODY_REJECTED`,
`RETRY_EXHAUSTED`, `RADIO_FAULT_TERMINAL`, and `SERVICE_FAULT_TERMINAL`,
`detail.owned_data` is the complete by-value DATA context. Returning the event
transfers that custody from link-v2 to the caller and frees the link slot; there
is no pointer into scheduler/wire storage. The caller must copy it into
application pending, standard AODV failure, repair, or explicit local-fault
disposal storage before its next operation. `local_reason` is meaningful only
for `LOCAL_TX_NOT_ATTEMPTED`; fault tags use `mesh_fault_reason`; timing,
attempt, and channel-mask fields preserve the actual episode values for every
outcome.

RADIO_FAULT or SERVICE_FAULT latches link fail-closed, clears the active
promotion, and changes every occupied slot to FAULT_PENDING. A zero-mask DATA
TX_FAILED has already returned LOCAL_TX_NOT_ATTEMPTED. Any selected partial
operation first produces TX_DONE plus its fault. RADIO_FAULT
makes every remaining slot return RADIO_FAULT_TERMINAL; SERVICE_FAULT makes every
remaining slot return SERVICE_FAULT_TERMINAL. A slot whose latest attempt
completed at least one channel preserves both masks and its attempt count. An
ACTIVE_QUEUED slot preserves its nonzero requested mask and zero completed mask
without incrementing attempt count; a never-promoted READY slot uses both masks
zero. The scheduler fault maps to
`mesh_fault_reason`: a radio fault after nonzero TX is RADIO_AFTER_TX, any other
radio fault is RADIO_UNAVAILABLE, POLL_OVERRUN remains POLL_OVERRUN, and queue or
state corruption is INTERNAL_STATE. Link may externally surface one owned event
per tick until its custody slots are drained. The scheduler has no global owner
callback and does not invent cross-protocol reset ownership.

Fault latching also discards any uncommitted RX candidate without dedupe/HACK,
rejects new unicast/flood work with `TAVRN_LINK_SEND_MESH_FAULTED`, and rejects a
late candidate decision with `TAVRN_LINK_RESOLVE_MESH_FAULTED`. It does not
discard accepted router/application custody; those owners receive the typed
terminal events or retain their already accepted inbound DATA as applicable.

Queue expiry is token-preserving but deliberately narrower than link fault
handling. Anonymous work is purged and counted; one nonzero token may surface as
`TX_EXPIRED` per poll. Existing custody, metadata, targeted, verification, and
repair handlers consume only matching tokens and apply their protocol-specific
unsent-deadline result.

Every terminal `detail.owned_data` contains only the failed immediate next-hop
`tavrn_direct_peer_t` (including its authoritative full AdvA) plus
`tavrn_link_data_t`, whose origin and final destination are logical IDs. It never
stores remote origin/destination full identities. In SID8 mode the router must
synchronously resolve those logical IDs through GTT before accepting event
custody into AODV/repair storage; zero or multiple matches fail closed through
the explicit RERR/drop path without fabricating identity.

Every public operation with an event output writes `local_outcome->type`, using
`NONE` when it has no synchronous outcome. `send_unicast` validates/encodes
before occupying a slot: validation, no-capacity, or a latched mesh fault leaves
custody with the caller and returns INVALID/NO_SLOT/MESH_FAULTED with `NONE`; an encode failure returns
`TAVRN_LINK_SEND_LOCAL_NOT_ATTEMPTED` with the copied DATA; and a successful
admission returns `SEND_OK`, `NONE`, and only the link-local READY slot.
`dispatch` performs the later scheduler admission. Scheduler full/invalid or
transaction-deadline expiry returns `TAVRN_LINK_STEP_EVENT` with
LOCAL_TX_NOT_ATTEMPTED and frees that slot; successful admission returns
`NO_EVENT` and leaves it ACTIVE_QUEUED.
Control/HACK admission that evicts the one older tracked token may return success
for the new untracked item while simultaneously returning the older DATA item's
LOCAL_TX_NOT_ATTEMPTED event. A NULL output is invalid. Timer-driven retry
failures return through `tavrn_link_v2_tick()`. No local outcome needs a hidden
event queue and none can be overwritten.

Owned-data custody/fault outcome tags are DATA-only. Failure to enqueue a control
frame returns the send status/counter with `local_outcome=NONE`; it has no DATA
custody to transfer. A control admission may nevertheless synchronously return
LOCAL_TX_NOT_ATTEMPTED for an older DATA item that it evicted.

Every event producer zero-initializes the full `tavrn_link_event_t` before
setting its tag and active union member. Tests may therefore assert that inactive
storage and unused fixed-array tails do not leak prior frame state.

Retries enter the shared queue at `BLE_MESH_TX_PRIORITY_RETRY`, above ordinary
DATA/control and below HACK. Scheduler `TX_DONE`, not enqueue success, increments
attempt count and starts the response deadline.

Every receiver-side DATA HACK producer uses the first DATA-copy RX timestamp as
the response anchor. One canonical pending HACK releases at
`first_rx + 14 ms + config.hack_turnaround_ms(8 ms) = first_rx + 22 ms`.
Decision latency and later duplicate copies cannot postpone it; resolution after
that time is immediately eligible. After transmission or expiry, a later DATA
retry may create a duplicate HACK. The link-v2 testbed initializes the 8 ms guard
from `tron_timer_config.radio_tx_event_bound_ms`; it is a validated wrap-safe
link-config duration, not a timer-registry field. The bearer retains HACK
priority and the 250 ms response window alongside the 8/14/18 ms operation,
3048 ms conservative-attempt, and 9894 ms unconstrained-wall bounds.

`timer.link_response_window_sum_ms=750` is exactly the sum of three 250 ms
response windows, not the total wall bound. With rolling budget enforcement, the
future conservative attempt bound and unconstrained wall are:

```text
link_tx_scheduler_attempt_bound_ms =
    (scheduler_custody_bypass_max + 1)
  * (1000 + two_sweep_success + scheduler_poll_max_ms)
   = (2 + 1) * (1000 + 14 + 2)
   = 3048

link_no_response_wall_bound_ms =
    3 * link_tx_scheduler_attempt_bound_ms + link_response_window_sum_ms
   = 3 * 3048 + 750
   = 9894
```

A radio timeout before any channel completes exits through
TX_FAILED/LOCAL_TX_NOT_ATTEMPTED. A later-channel timeout first reports one
TX_DONE attempt with requested/completed masks, then drains through
RADIO_FAULT_TERMINAL. Neither path continues to the three-window no-response
terminal proof. The 5000 ms absolute custody deadline remains authoritative over
either path and may prevent later retries.

Controlled flood never creates a forwarding route or bypasses routed
network/version/type admission. The link owns RSSI and ACK observations, not
route selection. All RSSI event fields use magnitude; policy derives negative
dBm only at comparison time. Link-v2 owns the 16-entry generic-FLOOD cache;
Phase 2 AODV controls are surfaced by value in `RX_CONTROL` and are deduped by
the one AODV core under their type-specific keys, not by a second generic route
cache.

Phase 1 uses this API in `ble_link_v2_testbed`; it does not provide a router and
cannot be labelled `AODV_ONLY`. AODV control structs, action unions, route
snapshots, and router hook typedefs are deliberately not part of this Phase 1
header; the Phase 2 red-test API contract freezes them before AODV implementation.
The testbed owns a separate fixed `12 x 3` valid-wire/channel aggregate state:
one row for each wire-v2 type and columns for channels 37/38/39. It receives a
successful post-decode frame plus RX channel and records no malformed,
foreign-network, unsupported-type, identity-conflict, or invalid-channel input.
It is testbed-only state, not a link/scheduler production counter or UART
contract.

## 9. One AODV core and one route table

`aodv_core_t` contains the only forwarding table in a routed firmware image.
The table is private to `aodv_core.c`; callers receive immutable snapshots.
Only AODV core functions create, refresh, invalidate, or expire a forwarding
route. GTT entries are membership observations and hop estimates; they cannot
contain a usable forwarding next hop or replace discovery.

Phase 1 does not guess AODV inputs, actions, route snapshots, or router event
unions. The Phase 2 red-test API contract freezes those typedefs and the exact
`init`, application submit, decoded-control ingest, `tick`, action poll, and
route-snapshot signatures before implementation. Architecture freezes only the
link-failure mode seam now because repair and standard AODV must share it:

```c
typedef enum aodv_link_failure_mode {
    AODV_LINK_FAILURE_IMMEDIATE_RERR = 0,
    AODV_LINK_FAILURE_DEFER_REPAIR_DESTINATION_RERR,
} aodv_link_failure_mode_t;

typedef enum aodv_failure_status {
    AODV_FAILURE_OK = 0,
    AODV_FAILURE_BUSY,
    AODV_FAILURE_INVALID,
} aodv_failure_status_t;

typedef enum aodv_deferred_rerr_decision {
    AODV_DEFERRED_RERR_ROUTE_REPAIRED = 0,
    AODV_DEFERRED_RERR_REPAIR_FAILED,
} aodv_deferred_rerr_decision_t;

aodv_failure_status_t aodv_core_report_link_failure(
    aodv_core_t *core, const tavrn_direct_peer_t *failed_next_hop,
    const tavrn_logical_id_t *repair_destination,
    aodv_link_failure_mode_t mode, uint32_t now_ms);

aodv_failure_status_t aodv_core_finish_deferred_rerr(
    aodv_core_t *core, const tavrn_logical_id_t *repair_destination,
    aodv_deferred_rerr_decision_t decision, uint32_t now_ms);
```

Both modes atomically invalidate every route whose direct next hop is the failed
peer. Immediate mode sorts unreachable logical destinations numerically by
SID16 in AODV_ONLY and by resolved canonical full identity in FULL_TAVRN, then
queues segmented RERR actions. Deferred mode does the same for unrelated destinations
but holds the named repair destination's RERR/precursor notification in the AODV
core; `repair_destination` is mandatory only in that mode. On repair success,
`finish_deferred_rerr(..., ROUTE_REPAIRED)` discards only that deferred
notification after the same route table contains the alternate route. On
`REPAIR_FAILED`, it queues the notification. There is no repair route cache.

If the action queue cannot reserve every segment required for the atomic
operation, the API returns `AODV_FAILURE_BUSY` before route mutation. The router
retains the owned link outcome and retries; it does not silently lose the link
failure or partially invalidate routes.

For a wire-validated metadata-augmented SID8 E_RERR, `aodv_core` parses the
base RERR prefix and entries according to the base count/width while retaining
the validated metadata suffix bytes in the copied control for router/FULL
receive processing and relay. The extension is not an exact-base-length error:
it MUST NOT map to AODV `INVALID`, router `INVALID`, or router fault-idle.

### 9.1 Routed-common incarnation owner

`tavrn_router.c`, not a FULL_TAVRN module, owns the routed incarnation FSM in
both feature levels. The existing HELLO node-sequence field carries a 16-bit
per-boot nonce while `N=1`; this adds no second route engine or feature-specific
wire. Platform startup supplies one nonzero nonce in the Phase 2 router config;
generation failure is fail-closed. Every repeated announcement in that boot
uses the same value, and the boot log records it. Deterministic host tests inject
the config value rather than adding a suffix/identity override. This is a
collision-reduction nonce for medium-correctness reboot separation, not
authentication or replay protection.

A bootstrap announcement is direct only when TTL/hops are direct, full HELLO
origin AdvA equals outer AdvA, and the full canonical identity is valid. Router
common keys an incarnation as `{full AdvA, boot_nonce}`:

- repeated direct `N=1` with the same tuple is idempotent;
- a direct `N=1` with a new nonce atomically clears that peer's pending HACK,
  DATA/control dedupe, serial freshness, and compressed binding; invalidates
  routes to or through the peer in the one AODV core; and queues the resulting
  sorted RERR segments before route traffic for that peer resumes; and
- an indirect, malformed, ambiguous, or stale announcement cannot reset peer
  state.

Direct N=1 processing derives SID16 from the full AdvA low two bytes (or SID8
from byte zero after FULL activation) and installs the resulting
`tavrn_direct_peer_t`. Only that direct binding contains full AdvA; remote
route/dedupe keys remain logical IDs derived under the selected width.

Router common has one pending-incarnation-reset slot. If RERR action reservation
temporarily returns BUSY, it copies the new `{AdvA,nonce}`, bars all route use for
that peer immediately, and retries the atomic clear/invalidate/queue operation
before consuming another reset. A second distinct reset while that slot is full
fails routed admission closed and increments overflow; it cannot leave an old
route usable or partially apply a reboot.

On local boot, both feature levels remain `REJOINING`, emit the bounded N=1
announcement, and originate/forward no DATA or AODV control. `AODV_ONLY`
self-establishes only after at least one N=1 scheduler `TX_DONE` and expiry of
`timer.router_reboot_announce_ms` measured from that completion, emits normal
N=0, and then enables route traffic. If no announcement actually transmits it
remains REJOINING. `FULL_TAVRN` uses the same common announcement and
invalidation path but delegates the later establishment decision to the
mentorship FSM. FULL_TAVRN never substitutes a second incarnation or AODV
implementation.

### 9.2 Local repair over the same AODV core

Repair starts only for an owned transit `RETRY_EXHAUSTED` event. The router
first copies DATA into the one-context/four-DATA repair store, then calls
`aodv_core_report_link_failure(...,
AODV_LINK_FAILURE_DEFER_REPAIR_DESTINATION_RERR)`. Repair-disabled,
locally-originated, capacity-excess, and setup-failure cases call immediate mode.

One repair episode permits at most two rate-limited RREQ emissions: one
Smart-TTL scope when a fresh hint exists (otherwise this first emission is full
scope), then at most one full-scope emission. Each is one RREQ attempt with no
inner AODV RREQ retries. The profile-derived bound is exactly:

```text
repair_timeout_ms = 2 * aodv_path_discovery_ms + 500
```

Success installs the alternate next hop in the same route table, cancels the
deferred RERR, and flushes each owned DATA once. Expiry or full-scope failure
queues the deferred RERR and releases each owned DATA through the explicit
failure/drop path. Repair never changes DATA wire bytes or starts steady-state
traffic.

The owned `RETRY_EXHAUSTED` proves only a local failed-hop episode for immediate
H. It can invalidate routes through H and starts repair only for the final
destination D; it proves neither H nor D departed. On repair failure, the repair
binding schedules exactly one maintenance-owned, duplicate-coalesced hard-expiry
verification obligation for H. If retry exhaustion is not repair-owned, the
router/FULL binding schedules the same H obligation directly. The maintenance
context atomically marks H due, then reuses the existing optional targeted Stage
0 HELLO, retained-hop+2 Stage 1 RREQ, full-diameter Stage 2 RREQ, independent
direct deadline when applicable, checked departure, and verified-LEAVE path.
Fresh committed direct/subject evidence cancels and revives the H context.
Missing, departed, unknown, or colliding H cannot become a LEAVE subject. Stage
0 is not an RREQ, so repair for D plus verification for H has at most four
semantic RREQ attempts. No repair/router hook originates LEAVE directly.

Maintenance, not repair, owns the failed-hop handoff after it accepts H. It has
one fixed pending-H slot in addition to the bounded verification contexts. The
handoff atomically admits H, coalesces a duplicate H with pending/active work,
or retains H through verification-cap or control-admission BUSY; missing or
already-departed H is terminally rejected, and a distinct-H pending-slot
overflow is rejected before it becomes an obligation. An admitted or retained H
remains an authoritative maintenance demand after deferred-RERR release and
failed-DATA drop. It is not `application_requested` and may end only on fresh
committed direct/same-subject evidence cancellation, missing/already-departed
terminal handling, or checked terminal completion after its verified LEAVE fact
is retained for retry.

## 10. FULL_TAVRN narrow hooks

The common router header defines an optional generic vtable but does not include
a FULL_TAVRN header. Its exact observation/action typedefs are frozen only after
the Phase 2 AODV red-test API contract exists; this Phase 0 architecture does
not guess AODV action or route-snapshot members. `AODV_ONLY` passes `NULL` and
compiles no full source/header. `FULL_TAVRN` constructs the vtable in
`tavrn_full.c`. Required hook roles remain narrow:

- GTT observes only committed/semantically accepted frame roles and copied route
  changes. An unresolved DATA candidate, BUSY, or REJECTED decision is not GTT
  evidence. GTT cannot mutate AODV storage.
- Smart TTL returns only an initial scope. Unknown, hard-expired, or departed
  state returns the caller's full scope; soft-stale non-departed membership
  remains eligible under `GTT-06`. The sole hard-expired exception is the
  maintenance stage-1 verifier's retained hop plus two; ordinary discovery never
  receives that hint, and fallback remains AODV-owned. Missing/zero retained hop
  skips stage 1 unconsumed; a diameter-capped stage 1 remains distinct from the
  fresh-ID full-diameter stage 2.
- FULL benchmark/application destination resolution is independent of Smart-TTL
  eligibility: a uniquely resolved nondeparted destination, including a
  soft-stale or hard-expired one, reaches the common AODV submission/fallback
  path. Only unknown, colliding, reserved, or departed destinations are blocked.
- ESC changes type-specific identity encoding through the wire-v2 codec seam,
  never route selection or route storage.
- Mentorship and maintenance emit typed control actions; the router decides how
  they enter link-v2.
- Repair must copy owned failed DATA into its fixed buffer before asking router
  common for deferred-RERR mode. If it cannot copy, router common uses immediate
  RERR. Repair requests discovery/invalidation only through the public mode API
  in section 9 and never edits the table.
- A hook is called synchronously. A pointer passed to it is valid only until the
  call returns. Persistent state requires an explicit fixed-size copy.

This direction prevents a cycle: `tavrn_full` may include public router/AODV
types to construct hooks, while `tavrn_router.c` and `aodv_core.c` never include
`tavrn_full.h`.

### 10.1 Implemented local expiry/demand boundary

The local-expiry/demand slice is implemented through the FULL-only binding and
accepted resource evidence. Its only top-level requirement tags are `GTT-02`,
`GTT-03`, `GTT-05`, `MAINT-01`, `MAINT-02`, `MAINT-04`, and `MAINT-05`; it does
not itself claim targeted request/response/relay or RREQ verification.

GTT remains passive. Its public application seam retains the existing active
enumeration and separately adds a known/nondeparted enumeration/query returning
a copied snapshot for every retained nondeparted member, including a demanded
hard-expired member. The copied result carries canonical AdvA and freshness; it
does not claim a route. Only the mesh-owner lane calls that direct canonical GTT
API. Other tasks use a fixed-capacity-one by-value FULL application mailbox:
submit copies an application GTT query/request/cancel command, the mesh owner
takes it before router tick, publishes a copied result, and the consumer takes
it. The mailbox exposes only
empty, command-pending, owner-processing, and result-ready states. Submit while
pending/processing/result-ready is BUSY; repeated owner take while processing or
result-ready is BUSY; owner take moves only pending to processing; publish moves
only processing to result-ready; and consumer take returns only a ready result to
empty. INVALID is reserved for null/invalid arguments and wrong publish
transitions, all immutable. The mailbox contains no pointers and every main call
stays under the existing `queue_guard_begin/end`. A same-timestamp request
therefore wins over departure; it is the sole application demand state in this
slice.

Each GTT entry carries directness, last direct evidence time, one application
request, and a revision in addition to membership facts. The generation lives in
`tavrn_gtt_storage_t`; every semantic mutation, raw imported-deadline adjustment,
serial clear, replacement, request/cancel, directness change, and departure uses
the next nonzero revision, skipping zero on wrap, while duplicate/no-op work does
not revise. `tavrn_gtt_sync_merge` takes no caller generation: GTT atomically
allocates its own monotonic nonzero generation/revision for every changed row and
the candidate commit, while duplicate bulk import is a no-op. Central GTT APIs
own raw mutation, and mentorship invokes only that merge; it never writes
candidate deadlines, generation, or raw storage. Directness is set only by the
distinct locally committed hop-1 provenance kinds `DIRECT_HELLO`,
`DIRECT_BOOTSTRAP`, `DIRECT_INCARNATION`, or `DIRECT_OUTER_TRANSMITTER`; the last
is generic non-HELLO immediate-transmitter evidence for exactly that AdvA and
accepts serial-absent liveness with canonical zero serial hash bytes while
preserving an existing latest serial/presence; a newly created serial-absent row
may remain serial-absent.
Imported metadata, SYNC, TC subjects, JOIN, and hop estimates are non-direct. A
direct deadline is calculated from
stored time and the current HELLO liveness threshold, never stored as a stale
fixed deadline.

| Future caller/path | Compatibility and centralized mutation rule |
| --- | --- |
| Legacy `tavrn_gtt_observe` / `tavrn_gtt_enumerate_active` | Retain Phase 3 no-jitter observation and active-enumeration compatibility. |
| Phase 5 FULL runtime observation | Uses provenance API only: ordinary HELLO/bootstrap/incarnation, generic direct outer transmitter, imported metadata/TC/JOIN, and imported SYNC. |
| Central GTT transitions | Serial clear; confirmed/checked departure; replacement; purge; request/cancel; and all ordinary/direct/imported refreshes revise centrally. |
| Mentorship | Uses centralized atomic SYNC merge only; no raw candidate deadline or storage-generation mutation. |

`tavrn_full_maintenance_binding.{h,c}` is the implemented FULL-only executable
boundary called exactly once by routed main. Routed main alone performs short
guarded mailbox owner-take/owner-publish operations; the binding receives only
one copied application GTT query/request/cancel command plus exact 0/1 presence,
and returns only its copied command result plus exact 0/1 presence. Given real
router, mentorship, and maintenance state, it also returns copied pre/post
results, `tavrn_router_tick_ex` phase trace/status, mentorship, local-broadcast,
and activation statuses. It calls `tavrn_maintenance_owner_pre_tick`
(whose observe/application result fields carry explicit presence bits), then
`tavrn_router_tick_ex`, `tavrn_mentorship_tick`, router local-broadcast snapshot
and maintenance observation, activation, and
`tavrn_maintenance_owner_post_tick`. Scheduler RX evidence is committed before
this boundary. Post-tick performs armed-only expiry sweep then topology/HELLO
maintenance; gated/unarmed post-tick performs no sweep or expiry-state mutation.
The exact main order is committed RX evidence → copied application GTT
query/request/cancel command → FULL binding → existing application DATA
submit/dispatch/link service. Generic `routed_cycle.{h,c}` remains GTT/FULL-free
and contains no duplicate router/mentorship/maintenance tick. Each due cadence makes one
bounded deterministic slot-order pass of at most 16 entries, visits every
occupied retained slot at most once, and copies/rechecks one candidate at a time;
there is no stacked table snapshot and demanded candidates cannot starve later
undemanded ones. Same-timestamp admitted direct RX wins before the sweep. It
computes the specified FNV-1a TTL jitter only for ordinary accepted remote
liveness refreshes; absent serial hashes two zero bytes and `now_ms` always
hashes exactly four little-endian bytes. Active imported SYNC uses no jitter:
`hard=now+remaining_hard` and
`soft=now+min(local_soft_expiry, remaining_hard)`; departed import uses fixed
local departed retention. Mentorship uses centralized atomic SYNC merge only,
never raw candidate-deadline/generation mutation. Local/checked departure and
departed retention are not jittered. Soft stale state only exposes/selects copied state and emits no
metadata or control in this slice.
Because sweep precedes topology sampling, a same-timestamp no-demand direct
departure is reflected in that post-tick's public direct-peer count and
topology-reset/cadence result; topology never retains an already departed peer.

For one copied canonical subject, maintenance resolves a unique SID8 through
GTT/ESC, then asks generic router/AODV/link snapshots by logical ID plus the
canonical subject AdvA. Shared reason masks and snapshot types belong in a tiny
future GTT-free `tavrn_subject_demand.h` (not created by this clarification).
Those generic components do not include or query GTT. Final-destination demand
reasons compare logical subject ID; AODV `VALID_ROUTE_VIA_SUBJECT` and next-hop
reasons compare logical ID and retained canonical AdvA; precursor compares its
logical subject on an unexpired valid route; deferred repair compares its
destination; pending ingest/retained DATA compare only final destination and
applicable next hop. The aggregate demand snapshot includes
`valid_route_to_subject` but no caller generation token; checked departure
synchronously recomputes authoritative mesh-owner demand immediately before
mutation from current AODV/link/router state—not a caller-supplied or cached
demand observation—and fails closed on unavailable/fail-stop state.

### 10.2 Targeted hard-expiry stage-0 delivery boundary

The targeted hard-expiry slice is FULL_TAVRN-only and follows the completed
local-expiry/demand binding. It does not create an AODV engine, RREQ action, timer
key, scheduler/radio API, or metadata format. `tavrn_maintenance` owns the four
verification contexts, their exact request/response/relay work, cancellation,
stage transitions, and verification-domain token allocation. `tavrn_router`
revalidates route snapshots immediately before local admission, submits copied
best-effort control, and returns every scheduler eviction, `TX_DONE`, `TX_FAILED`,
and terminal fault to maintenance. `aodv_core` remains the sole later
RREQ/request-ID owner and is not called by stage 0.

Maintenance verification tokens are allocated only from `0x8000..0xffff`; the
link-owned `0x0001..0x7fff` domain is shared by custody DATA and routed-common
tracked bootstrap/incarnation HELLO. The link allocator checks every retained,
queued, and in-flight low-domain item; a caller-owned high token is checked
against every queued/in-flight token before admission. Token zero is untracked
and no third domain exists. Initiator pending request, request relay, response
relay, immediate target response, and delayed intermediary response preserve
exact copied bytes and token across BUSY or zero-channel `TX_FAILED`, retry only
from their owner tick while the selected route remains valid, and drop on route
loss or the wrap-safe equality deadline
`obligation_started_ms + timer.hello_dedupe_ms`. `obligation_started_ms` is the
owner tick that creates/adopts a locally initiated stage-0 context after current
hard-expiry/demand/identity validation; the post-dedupe normal request
RX-admission time for a request relay, immediate target response, or delayed
intermediary response; or the post-dedupe normal response RX-admission time for a
response relay that need not have observed the request. The deterministic
intermediary delay consumes bounded lifetime.
BUSY before first scheduler enqueue is also bounded by this deadline.
The target and eligible intermediary policies, including bucket selection and
deterministic FNV-1a delay, are representation rules in wire-v2; a delayed
intermediary consumes one of the four metadata/candidate slots, never replaces a
live entry, and suppression can cancel only matching delayed-not-yet-enqueued
work.

Only matching-token `TX_DONE` with a nonzero completed-channel mask consumes
stage 0. The observed completion time starts the wrap-safe remote-silence
deadline `+ timer.aodv_net_traversal_ms`, due at equality. Wrong-token completion
and zero-channel `TX_FAILED` do not consume it and no remote-silence timer starts
before a matching completion. Route loss before consumption retires stage 0 as
unconsumed and retains `STAGE1_READY`; other demand enters that retained handoff
directly. At stage-0 timeout, maintenance retains active `STAGE1_READY` without
emitting RREQ, departure, TC, or LEAVE. Same-subject valid liveness exactly
cancels queued work when possible and returns its token without stage advance.
Selected/in-flight work becomes a canceled
tombstone that retains its token through terminal TX_DONE/TX_FAILED/eviction/fault;
that late terminal event only retires the tombstone and never advances a stage.
Tombstones are bounded by the same four verification contexts and fail closed.

Targeted acceptance therefore requires targeted dedupe/capacity and source-list
closure, a fresh FULL FAST stack/RAM comparison against the accepted local-expiry
baseline, and legacy/AODV_ONLY isolation. It does not require a new 300-second
hardware capture. No shared radio/scheduler source or timer/generated-config
change is authorized by this slice.

#### 10.2.1 Retained-hop/full-diameter verification

The implemented maintenance pass keeps the same four maintenance contexts and invokes
the one `aodv_core` through the existing router action and link admission path;
it may not submit ordinary DATA, create an ordinary discovery slot, use an
expanding-ring retry, or invoke ordinary Smart-TTL fallback. The local
maintenance action carries a non-wire purpose (`STAGE1_RETAINED_HOP` or
`STAGE2_FULL_DIAMETER`), canonical subject, AODV attempt/request ID, and tracked
high-domain token. The router/link callback returns the same discriminators plus
terminal kind and completed-channel mask. Maintenance validates all of them
before changing stage state.

At `STAGE1_READY`, a retained positive hard-expired hop emits exactly one
`min(hop+2,diameter)` RREQ; no/zero hop skips directly to Stage 2 with no
consumed RREQ. A capped Stage 1 is still distinct from Stage 2. Stage 2 emits
exactly one full-diameter RREQ with a fresh request ID. Shared `aodv_rreq_rate`
defers exact ready work before an ID is allocated. BUSY, eviction, local
non-attempt, zero-mask failure, and mismatched completion retain or retry exact
work; a matching nonzero-mask completion consumes one stage and opens only its
`aodv_path_discovery_ms` window. Two remote response windows are bounded by
`verification_window_ms` using wrap-safe equality comparisons.

Committed valid evidence for the exact canonical subject cancels a live context;
other-subject, stale, malformed, and ambiguous evidence cannot. After silent
Stage 2, direct contexts wait for the independent direct-evidence deadline.
Terminal departure remains the existing checked mutation and must re-read current
demand and revision; DEV-027 routes only its confirmed terminal result through
the TC/LEAVE owner. Four active contexts remain bounded,
a fifth defers, and owner traversal is deterministic. This behavior is accepted
by the focused `--green` runner. The `--red` runner remains the isolated
missing-policy contract regression.

#### 10.2.2 General metadata and confirmed TC boundary (DEV-027, production-composed GREEN)

The FULL maintenance API exposes one fixed four-slot candidate pool. Its
production implementation makes that owner
for general soft-request/self-answer/intermediary-answer work and the already
frozen delayed targeted tier-2 reservation. No slot replaces live work. The
maintenance owner alone selects candidate copies and asks the existing router
control path to enqueue a containing SID8 RREQ/RREP/RERR; router/link return
admission/terminal facts. Selection and encoding do not mutate the pool. Only
enqueue admission commits cooldown, cursor, and release; BUSY, zero-channel, or
local-not-attempted return exact work to the owner unchanged. No host adapter may
implement a parallel metadata store or bypass this router→link admission seam.
Delayed targeted reservation is capacity-only: general selection/attachment never
serializes it, and an admitted general control releases/cools only its selected
general entries. Attachment starts with real validated RREQ/RREP/RERR controls;
the corresponding admitted `tavrn_rx_control_event` is parsed by the FULL
maintenance receive seam before metadata/GTT merge.

The same owner must maintain a local nonzero 16-bit TC sequence stream and fixed UUID
`{network,origin,seq}` plus subject/event equality stores. A generic sequence
ticket prepare/commit seam is shared by mentorship JOIN and maintenance LEAVE:
`originate_join` obtains the ticket and its successful JOIN enqueue commits it;
mentorship has no independent join-sequence field. It prepares a local TC only
for completed mentorship/self-bootstrap JOIN, confirmed terminal three-stage
departure, or confirmed direct timeout. DATA `RETRY_EXHAUSTED` for immediate H
only schedules its independent maintenance verification. The owner validates incoming TC fully before cache mutation. A retained
UUID ignores any later event mutation; a fresh UUID is retained before the
subject/event check, so matching live subject/event suppresses apply/relay while
retaining that UUID. Only a fresh subject/event applies once through GTT as
imported/non-direct evidence and returns a copied controlled-flood relay
obligation. Distinct local LEAVEs and relays use bounded FIFO retention across
BUSY; neither later fact may overwrite an earlier one. An accepted mentorship
JOIN ticket arbitrates before a retained LEAVE, and each commits one distinct
nonzero sequence. The router/link path retains a BUSY relay exactly and must not
synthesize HACK/TC ACK.

The corrective production closure has only two direct LEAVE origins, not a
generic host callback: the terminal verification checked-departure path and the
direct-deadline checked-departure path. The router retry-exhausted hook carries
the link event's full failed-next-hop AdvA (not final D) only to schedule the
independent maintenance verification; it has no TC origin call. No-demand expiry,
ordinary RREQ, BUSY/rejection/enqueue/zero-channel/local-not-attempted/deadline/
RREP-ACK, radio/service, and repair paths have no TC origin call. The production
implementation binds these APIs from `tavrn_full_maintenance_binding` on the mesh owner;
AODV_ONLY, radio/scheduler, and host-only test backends remain outside that
boundary.

### 10.3 Local expiry resource and hardware acceptance

`build-tavrn-ble.sh` supports
`--stack-usage` and `--resource-baseline <manifest>` case arms, build FULL
FAST_TEST and BALANCED for `--target tavrn_routed_node`, and retains matching map,
ELF, `.su`, and resource-manifest artifacts. The production runner then
invokes `python3 microbit/scripts/check_tavrn_expiry_resources.py` with
the pre/post map, ELF, `.su` files, declared fixed-state manifest, and the
`routed_mesh_task` root; it reports pre/post `.data/.bss`,
the exact declared fixed-state delta, unexplained RAM delta, heap-use verdict,
link verdict, and static owner-binding mesh-task call-chain stack. The accepted
production command forms are:

```text
./microbit/build-tavrn-ble.sh --target tavrn_routed_node --feature FULL_TAVRN \
  --timer FAST_TEST --out out/full-fast --stack-usage \
  --resource-baseline resources/pre-expiry-fast.json
./microbit/build-tavrn-ble.sh --target tavrn_routed_node --feature FULL_TAVRN \
  --timer BALANCED --out out/full-balanced --stack-usage \
  --resource-baseline resources/pre-expiry-balanced.json
python3 microbit/scripts/check_tavrn_expiry_resources.py \
  --before-map "$TAVRN_EXPIRY_BASELINE_DIR/fast.before.map" \
  --after-map out/full-fast/tavrn_routed_node.map \
  --baseline-manifest "$TAVRN_EXPIRY_BASELINE_DIR/fast.baseline.manifest" \
  --baseline-sha256 "$TAVRN_EXPIRY_BASELINE_DIR/fast.baseline.sha256" \
  --elf out/full-fast/tavrn_routed_node.elf --su-glob 'out/full-fast/**/*.su' \
  --stack-root routed_mesh_task \
  --required-edge-manifest microbit/tests/protocol/fixtures/tavrn_expiry_resources/required-stack-edges.json \
  --resolve-operation-edge routed_cycle_operations.router_tick=routed_cycle_router_tick \
  --resource-manifest out/full-fast/tron.tavrn.expiry.resources.v1.json
```

After host success, GREEN requires
`TAVRN_EXPIRY_BASELINE_DIR=<provenance-bound-pre-expiry-dir>`,
`TAVRN_EXPIRY_ARM_CAPTURE_MANIFEST=<capture.json>`, and
`TAVRN_EXPIRY_ARM_CAPTURE_ARTIFACT_MANIFEST=<hooks-on-artifact.manifest>`.
The capture artifact is the hooks-ON FULL FAST_TEST artifact, never the
hooks-OFF resource build manifest. The checker hashes that manifest, compares
its ELF hash exactly with capture provenance, requires its hooks-ON fields, and
compares it with the hooks-OFF FAST resource artifact. Their selected-source
inventory hashes must match; their generated build-config manifests may differ
only in `hook.enabled` and `hook.expiry_full_table`. The baseline directory contains
exactly `fast.before.map`, `fast.baseline.manifest`, `fast.baseline.sha256`,
`balanced.before.map`, `balanced.baseline.manifest`, and
`balanced.baseline.sha256`; checker validation rejects missing, duplicate, stale,
or source-inventory-mismatched baseline content. FAST and BALANCED each pass
their own `--before-map`, returned fresh `--after-map`, returned stack-usage
`--su-glob`, `--stack-root routed_mesh_task`, and the required-edge manifest to
every checker invocation. The manifest resolves both indirect routed-cycle
operation edges, `routed_cycle_operations.router_scheduler_event` and
`routed_cycle_operations.router_tick`, evaluates every reachable operation
branch, and selects the true maximum-byte chain. The <=3072-byte result sums
all caller frames and by-value/hidden result storage, not merely the binding
leaf. Kernel sources compiled in resource mode also emit `.su` evidence, so a
scheduler-event branch cannot disappear behind an undeclared kernel frame.

The manifest schema is `tron.tavrn.expiry.resources.v1` and contains target,
feature, timer, `.data`, `.bss`, unallocated RAM, declared fixed-state bytes,
unexplained delta, true maximum routed-cycle operation chain, stack headroom,
scheduler capture duration/pass count/maximum gap,
hard-selected+demand-deferred complete-pass count, unavailable checked-candidate
count, fault fields, and targeted/RREQ/TC control fields. `unavailable_count`
is a capture fault. Build-profile tests include passing and threshold-failing
fixtures for every reported limit.

The exact fixtures are
`microbit/tests/protocol/fixtures/tavrn_expiry_resources/{baseline,pass,fail-stack,fail-headroom,fail-ram,fail-delta,fail-capture}.json`.
BALANCED independently uses
`microbit/tests/protocol/fixtures/tavrn_expiry_resources/{balanced-baseline,balanced-pass,balanced-fail-stack,balanced-fail-headroom,balanced-fail-ram,balanced-fail-delta,balanced-fail-capture}.json`.
The build-profile runner's
`--expiry-resource-acceptance-report <fresh-report.env>` creates fresh temporary
FULL FAST_TEST, FULL BALANCED, and AODV_ONLY artifacts and returns only their
ELF/map/resource-manifest/selected-source-inventory/**compile_commands.json** paths plus
`full_fast.source_inventory_sha256`, `full_balanced.source_inventory_sha256`,
and `aodv_only.source_inventory_sha256`. Each inventory is one selected relative
source path per line (including untracked selected production source), hashed as
the deterministic ordered `<sha256>  <relative-path>` digest list. The checker rejects missing
or duplicate schema keys, stale artifact hashes, wrong target/feature/timer,
binding symbol count mismatch, stack above 3072, headroom below 1024,
post-reserve RAM below 8192, unexplained delta above 512, and every
failing-fixture threshold.

### Routed initial-task and RAM safety budget

The routed boot initial task, the dedicated routed mesh task, and the map-level
RAM reserve are separate quantities. `TRON_ROUTED_INITIAL_TASK_STACK_BYTES=4096`
is the one CMake profile authority for the initial task's logical capacity. The
routed application target and its target-specific μT-Kernel object target both
compile with that exact `INITTASK_STKSZ` definition; `inittask.h` retains its
overrideable 1024-byte default for legacy and link targets. Generated routed
config/manifest evidence records `build.initial_task_stack_bytes=4096`,
`capacity.routed_initial_task_stack_bytes=4096` with state `IMPLEMENTED`, and
`resource.runtime_ram_reserve_bytes=12592`. Legacy and link evidence instead
records initial-task default 1024, routed-capacity state `NOT_APPLICABLE`, and
reserve zero.

The 4096-byte initial-task capacity is not the existing 4096-byte
`routed_mesh_task` stack: the latter retains the full routed-cycle static-chain
check (at most 3072 bytes and at least 1024 bytes headroom). Separately, the
resource gate locates exactly the production
`app/tavrn_routed_node/src/main.c:usermain` GCC `.su` record rather than the
weak kernel `usermain`, and requires at least 1024 bytes of logical capacity
above that function's static frame. This is deliberately only a direct-frame
sanity check; it does **not** claim that the frame is a complete startup call
chain.

`TRON_ROUTED_RUNTIME_RAM_RESERVE_BYTES=12592` is the conservative routed runtime
allocation allowance for the initial task, mesh task, logger task, exception
stack, and allocator overhead. The checker keeps the existing map-free-RAM gate,
then subtracts that reserve without underflow and requires
`post_reserve_ram = map_unallocated_ram - runtime_ram_reserve_bytes >= 8192`.
Checker and acceptance-report output publish map-unallocated, reserve, and
post-reserve byte values so the two headroom concepts cannot be conflated.

For each fresh FULL profile, the checker selects the exact `main.c` command from
that profile's `compile_commands.json`, replays it with `-E -P` and the generated
defines/includes, and parses the resulting preprocessed main artifact. That
artifact—not repository `main.c` text—is authoritative for the exact callback
assignment, two fail-closed guarded mailbox blocks, one active binding call,
presence/result validation before publish, no loop/recursion/dead branch/direct
owner tick, and AODV exclusion. Pass/fail preprocessed-main fixtures cover an
inactive-only binding and an ignored guard failure. Fresh FULL ELF/disassembly
must define and call the binding; AODV_ONLY must neither define nor reference it.

The completed local-expiry acceptance invoked this fresh check after zero host
failures and required both capture environment paths above. Missing, stale,
shorter-than-300-second, fewer-than-ten-pass, no hard-selected+demand-deferred
complete pass, over-2-ms, unavailable, faulted, or targeted/RREQ/TC-control-
emitting capture manifests were nonzero failures; there was no pending-success
mode. The targeted stage-0 slice reuses that accepted capture as baseline evidence
and requires only its fresh FULL FAST stack/RAM comparison, not another 300-second
capture.

Completed local-expiry production first established the tooling and baseline:
before its semantic GTT, AODV, link, router, maintenance, FULL, or main change,
it implemented the build options/checker, generated the provenance-bound
FAST/BALANCED before maps, baseline manifests, hashes, and selected-source
inventories, then froze those artifacts read-only. A later source-inventory
mismatch invalidates comparison and restarts from that untouched baseline; it
must never regenerate a baseline after semantic edits.

Both links must succeed with no heap, at least 8 KiB post-reserve RAM, static
worst-case mesh-task operation chain at most 3072 bytes (leaving at least 1024
bytes of the 4096-byte mesh stack), and unexplained fixed-state growth no more
than 512 bytes beyond declared fixed structs/alignment. A separate test-only FULL
FAST_TEST full-table hook seeds exactly once after GTT and maintenance
initialization and before the mesh task begins: self plus 15 deterministic
synthetic identities. It uses the real ESC mapping to reject full-AdvA and SID8
collisions, marks every synthetic entry application-requested on the mesh-owner
lane, and never refreshes entries per router tick. The entries therefore age
naturally through soft selected and hard selected/demand deferred states without
departure, purge, or periodic hook work. Seed failure aborts startup; transient
runtime statuses do not latch a hook fault. The 300-second capture requires at
least ten due complete 16-slot passes, at least one complete
hard-selected+demand-deferred pass, a nonzero monotonic scheduler-return-gap
sample no greater than 2 ms, unavailable_count=0, no
routed-cycle/router/link/radio/service fault, and no targeted/RREQ/TC expiry
traffic. Required artifacts are both map/ELF/.su sets, checker JSON/text output,
exact build/check command transcript, hooks-ON test-hook manifest, and capture
log; hooks are excluded from normal production behavior.
That bit alone authorizes later stage 0, which must immediately revalidate an
unexpired route to the subject and skips unconsumed on route loss. Other demand
can retain a hard-expired member but skips stage 0. A checked local departure
must match canonical identity, revision, and current hard expiry, must be
nonself/nondeparted, and for a direct entry must also meet the calculated direct
deadline. It preserves last real evidence/serial/hop, clears directness and
application request, sets fixed departed retention, and emits neither a
received-evidence record nor a LEAVE.

The completed local-expiry/demand implementation allocates or advances no
verification context, sends no targeted control or RREQ, and originates no
expiry-driven TC. The next non-overlapping targeted slice owns stage-0 contexts,
scheduler-token consumption, targeted request/response/relay, and the retained
`STAGE1_READY` handoff under `MAINT-06`/`MAINT-07`; retained-hop/full-RREQ
attempts remain a later slice. Existing mentorship bootstrap JOIN remains
unchanged.

## 11. Patient bridge position

The patient bridge is an optional application classifier and queue above the
selected node facade. The mesh task performs only constant-time wire-family
dispatch; for a non-mesh observation it copies AdvA, RSSI, and AdvData into the
fixed outbound event queue. A lower-priority application task invokes the
patient bridge, which therefore does not include the scheduler header or run in
the bounded mesh loop:

```c
typedef enum patient_bridge_claim {
    PATIENT_BRIDGE_NOT_PATIENT = 0,
    PATIENT_BRIDGE_CONSUMED,
    PATIENT_BRIDGE_REJECTED,
} patient_bridge_claim_t;

patient_bridge_claim_t patient_bridge_observe(
    patient_bridge_t *bridge, const tron_rx_observation_t *observation,
    uint32_t now_ms);
void patient_bridge_on_node_event(patient_bridge_t *bridge,
                                  const tron_node_event_t *event,
                                  uint32_t now_ms);
int patient_bridge_poll_submission(patient_bridge_t *bridge,
                                   tron_application_data_t *data);
```

The mesh task sends routed/legacy families directly to the selected node and
copies only the exact patient envelope to the application queue; a later
`NOT_PATIENT` result is an invariant/counter drop, not a second mesh dispatch. A
consumed patient event is copied into bridge-owned fixed storage; the application
task copies each value returned by `poll_submission` into the fixed mesh command
queue, and only the mesh task calls the selected node submit seam. The bridge consumes routed/flood delivery through copied
`tron_node_event_t` values. It marshals GTT query/request/cancel by-value commands
through the application-event owner and receives copied results; it must not call
link, AODV, scheduler enqueue, radio, or mutable GTT APIs directly, and patient
identity must never be inserted as a mesh-node identity or route.

Single-radio truth remains visible: patient observation and mesh TX share the
same scheduler. There is no simultaneous TX/RX claim.

## 12. Fixed storage capacities and overflow ownership

All radio, queue, wire, link, routing, GTT, mentorship, maintenance, repair, and
patient state is statically or caller allocated. None may call `malloc`,
`calloc`, `realloc`, `free`, C++ allocation, or a μT-Kernel variable memory pool.

| Owner | Capacity | Full behavior |
| --- | ---: | --- |
| raw legacy AdvData | 31 bytes | codec rejects before driver truncation |
| routed custom PDU | 24 bytes | wire-v2 codec rejects; exact per-type budgets belong to wire doc |
| public routed application array | 10 bytes | patient exactly 7; SID16 DATA max 7; SID8 opaque test max 10; reject invalid mode/type length |
| scheduler physical TX queue | ROUTED 4/8/16/40; LEGACY/LINK 4 | routed priority-banded EDF/expiry/budget admission with synchronous typed eviction; production routed capacity is deferred pending benchmark selection |
| legacy dedupe | 16 keys | reclaim expired, otherwise deterministic oldest-expiry replacement as existing tests require |
| link synchronous RX candidate | 1 DATA | caller must resolve before another link step; no dedupe/HACK before resolution |
| link custody TX | 4 DATA items, 1 active physical attempt | excess returns `SEND_NO_SLOT`; only oldest due slot is promoted, other slots remain READY/waiting under transaction deadline |
| link accepted/duplicate cache | 16 keys | deterministic expired/oldest replacement; never duplicate-deliver |
| link generic-FLOOD dedupe | 16 keys | purge expired then deterministic oldest; no second local process/relay |
| router pending incarnation reset | 1 peer tuple | bar affected peer and retry atomic AODV invalidation/RERR reservation; second reset fails routed admission closed |
| router failure obligations | 16 peers | coalesce exact direct peers and process only the oldest wrap-safe admission order before new AODV/application/link admission |
| router failure overflow preservation | 1 owned DATA event | the seventeenth distinct retry-exhausted event is copied here and latches fail-stop; no later router API produces an event |
| router post-ACK accepted DATA | 1 input | retain until its one AODV ingest succeeds; non-BUSY ingest failure latches fail-stop without dropping it |
| router application delivery reservation | 1 opaque token | reserve before ACK; token zero after `OK`, cancellation/commit inconsistency, or action/data mismatch latches fail-stop with accepted state retained |
| router retained AODV action | 1 action | retain exact action over transient link/application BUSY; typed dispatch output is mandatory |
| AODV routes | 16 | reject worse/new route; never evict an active route for a stale candidate |
| AODV RREQ seen cache | 32 | expired/oldest deterministic replacement |
| AODV pending application/transit DATA | 8 | `BUSY`; failed transit DATA follows RERR/repair fallback, never silent overwrite |
| AODV precursor IDs per route | 8 | ignore duplicate; on full retain existing set and count truncation |
| pending E_RREP_ACK waits | 4 | refuse/backpressure a new A=1 transmission before emission; never create an untracked wait |
| AODV RERR sorted batch | 16 unreachable logical IDs | numeric SID16 order in AODV_ONLY, canonical full-identity order in FULL; segment at max 3 SID16 or 4 SID8 entries, therefore at most 6 or 4 action items |
| AODV pending action queue | 8 | atomically reserve all RERR segments before mutation; external input returns `AODV_FAILURE_BUSY`, timer action remains due, and no item is overwritten |
| GTT membership entries | 16 plus one storage-owned generation | deterministic departed/oldest-stale replacement; never overwrite fresher active state; central APIs assign a fresh nonzero revision for every semantic/raw-deadline/serial/directness/request/departure mutation, while no-op operations do not revise; each entry retains directness/time and one application request without creating a GTT timer/send/route owner |
| mentorship competing offers | 8 | retain best deterministic offers; count overflow |
| mentorship frozen snapshot | 16 GTT entries | reject bootstrap snapshot creation if it cannot represent local GTT |
| mentorship active snapshot/session | 1 | reject/defer another mentee session until completion/expiry; immutable snapshot remains owned by the active session |
| maintenance ordinary/targeted HELLO equality dedupe | 16 keys | purge expired; reject/backpressure a new live key rather than overwrite equality protection; targeted work retains its originating key through wrap-safe `obligation_started_ms + timer.hello_dedupe_ms`, due at equality |
| maintenance direct-peer boot epochs | 16 full-AdvA/nonce records | follow router-committed boot nonces; clear only the matching nonself GTT serial and ordinary-HELLO keys before N=0 evaluation |
| maintenance pending ordinary HELLO | 1 control | retain exact control and visible `snapshot.next_node_sequence` on BUSY; cadence and that cursor advance only after queue admission. Targeted reservations use the internal frontier after the pending value; admission merges the visible cursor to the frontier while skipping active reservations. |
| maintenance active verification FSMs | 4, targeted/RREQ slices only | retain subject, demand, retained hop, current stage (including `STAGE1_READY`/`WAIT_DIRECT_DEADLINE`), `obligation_started_ms`, exact pending or canceled-tombstone work, and one unique `8000..ffff` verification token checked against all queued/in-flight work; defer a fifth start and fail closed rather than exceed the same four tombstones. The completed local-expiry/demand slice allocates none. |
| maintenance metadata/freshness candidates | 4 | round-robin social metadata plus delayed tier-2 response state; an eligible free-slot intermediary must schedule one response, while no free slot suppresses it rather than replacing live state or discovering a route |
| concurrent repair contexts | 1 | immediate normal AODV failure/RERR path |
| repair buffered DATA | 4 | immediate normal AODV failure/RERR path for the unbuffered item |
| patient dedupe | 16 keys | deterministic expired/oldest replacement |
| patient outbound queue | 8 events | incidents outrank heartbeat; count every dropped item |
| mesh inbound command queue | 8 copied commands | reject/backpressure producer; mesh task never blocks |
| mesh diagnostic log queue | 8 copied records | drop/count diagnostics only; never delays scheduler poll |
| retry-exhaustion log mailbox | 1 copied `tavrn_link_event_t` | routed-only retain-oldest/drop-newest; saturating dropped telemetry; logger-only and never a fault-liveness dependency |
| node/router application event queue | 8 events | backpressure producer; no silent overwrite |

All capacities are emitted in the build manifest and guarded with compile-time
assertions that count fields fit their index types. ROUTED 4/8/16/40 variants are
explicit benchmark configurations, not a selected production capacity; LEGACY
and LINK remain fixed at 4 with sizeof/map equivalence evidence. Increasing or
selecting a routed capacity is an architecture and memory-budget decision, not a
bench-only CMake override.

Historical capacity evidence required measured maximum use and remaining
headroom but imposed no numeric threshold. The completed local-expiry production
gate is explicit: ARM `-fstack-usage`/map evidence for the complete
`routed_mesh_task` chain must be at most 4096 bytes and preserve at least 1024
bytes of the independent 4864-byte mesh task allocation (effective maximum
3840); fixed-size guards alone remain
insufficient.

There is one 16-entry AODV route table. The 16-entry GTT and caches above are
not alternate route tables.

## 13. Timer profile ownership

`TRON_TIMER_PROFILE` selects one immutable generated
`tron_timer_config_t`. Algorithms receive that config during initialization;
they do not read CMake macros throughout implementation files. Every deadline
uses unsigned 32-bit millisecond serial arithmetic and every configured interval
must be strictly below `0x80000000` ms.

The profile lane owns the final routed numeric values. This architecture owns
the required manifest keys and consumers:

| Manifest key group | Consumer/owner |
| --- | --- |
| `timer.scheduler_dwell_ms`, `timer.scheduler_relay_spacing_ms`, `timer.scheduler_custody_bypass_max`, `timer.scheduler_poll_max_ms`, `timer.radio_state_timeout_ms`, `timer.radio_tx_event_bound_ms`, `timer.radio_tx_repeated_event_bound_ms`, `timer.radio_tx_fault_cleanup_bound_ms` | shared scheduler/radio service and hardware bounds |
| `timer.legacy_relay_min_ms`, `timer.legacy_relay_max_ms`, `timer.legacy_dedupe_ms`, `timer.legacy_ping_interval_ms`, `timer.legacy_ping_timeout_ms` | legacy branch; BALANCED values are frozen in section 7 |
| `timer.link_hack_timeout_ms`, `timer.link_max_attempts`, `timer.link_retry_backoff_ms`, `timer.link_tx_scheduler_attempt_bound_ms`, `timer.link_response_window_sum_ms`, `timer.link_no_response_wall_bound_ms`, `timer.link_candidate_resolve_ms`, `timer.link_busy_backoff_ms`, `timer.link_busy_max_responses`, `timer.link_data_deadline_ms` | link-v2 scheduler/candidate/custody response and wall bounds |
| `timer.link_data_dedupe_ms`, `timer.link_flood_dedupe_ms`, `timer.link_flood_jitter_min_ms`, `timer.link_flood_jitter_max_ms` | routed link receive retention and controlled-flood scheduling |
| `timer.aodv_node_traversal_ms`, `timer.aodv_net_diameter`, `timer.aodv_net_traversal_ms`, `timer.aodv_path_discovery_ms`, `timer.aodv_rreq_seen_ms`, `timer.aodv_rreq_retries`, `timer.aodv_rreq_rate`, `timer.aodv_rerr_rate`, `timer.aodv_active_route_ms`, `timer.aodv_pending_data_ms`, `timer.aodv_blacklist_ms` | AODV traversal/search/lifetime/rate registry |
| `timer.aodv_rrep_dedupe_ms`, `timer.aodv_rerr_dedupe_ms`, `timer.aodv_rrep_ack_wait_ms` | exact AODV control dedupe and independent E_RREP_ACK wait |
| `timer.gtt_soft_expiry_ms`, `timer.gtt_hard_expiry_ms`, `timer.gtt_departed_ms`, `timer.gtt_maintenance_ms` | GTT/maintenance |
| `timer.hello_change_ms`, `timer.hello_stable_ms`, `timer.hello_alpha`, `timer.hello_snap_ratio`, `timer.hello_dedupe_ms`, `timer.router_reboot_announce_ms` | Phase 5 adaptive HELLO cadence/suppression and reboot announcement |
| `timer.freshness_response_min_ms`, `timer.freshness_response_max_ms`, `timer.verification_window_ms`, `timer.verification_new_cap`, `timer.verification_active_cap` | tiered targeted-freshness response and maintenance verification bound/caps |
| `timer.mentor_offer_window_ms`, `timer.mentor_page_timeout_ms`, `timer.mentor_page_attempts`, `timer.mentor_self_bootstrap_ms`, `timer.mentor_sync_dedupe_ms` | mentorship collection, paging, self-bootstrap, and SYNC retention |
| `timer.mentor_rssi_weak_magnitude_db`, `timer.mentor_rssi_strong_magnitude_db`, `timer.mentor_rssi_weak_delay_ms`, `timer.mentor_rssi_strong_delay_ms`, `timer.mentor_jitter_min_ms`, `timer.mentor_jitter_max_ms`, `timer.mentor_offer_suppression_ms` | mentorship RSSI magnitude/delay endpoints, jitter, and offer suppression |
| `timer.metadata_cooldown_ms`, `timer.tc_uuid_ms`, `timer.tc_subject_ms` | metadata/topology maintenance |
| `timer.repair_timeout_ms`, `timer.repair_cooldown_ms` | optional repair; timeout is `2 * aodv_path_discovery + 500` |
| `timer.stats_ms`, `timer.loop_delay_ms` | firmware application loop/logging |

`tavrn_link_config_t.hack_turnaround_ms` is initialized from the existing
one-sweep `timer.radio_tx_event_bound_ms`; it deliberately has no `timer.*`
manifest key. The 75-key registry includes the canonical repeated-event and
fault-cleanup bounds, and generated timer configuration emits their manifest
formulas. Capacity/WCET selection and hardware evidence remain separate gates.

Profile intent is fixed:

- `FAST_TEST`: deterministic short host/bench gates; never used as soak evidence.
- `BALANCED`: normal PoC demonstrations and the legacy-compatible default.
- `SOAK`: finite approximately eight-hour stability run with reduced control
  cadence and expiry values long enough to avoid test-induced churn.

The exact values for every key in all three profiles are frozen in the normative
profile's timer registry. This architecture mirrors names and consumers but is
not an alternate numeric registry. A selected profile with a missing key is a
CMake fatal error; no C fallback default is permitted. Generated derived values
include one/two-sweep success and fault cleanup `8/14/18 ms`, the custody
attempt bound `(2 + 1) * (1000 + 14 + 2) = 3048 ms`, the 750 ms response-window
sum, the unconstrained three-attempt wall `3 * 3048 + 750 = 9894 ms`, and the
fixed 5000 ms absolute custody truncation. They are generated once at configure
time and emitted to the manifest; algorithms do not recompute a different
private default. Firmware `timer.loop_delay_ms` remains a separate application
loop value and is not an input to the custody proof.

## 14. Central build configuration

### 14.1 Cache variables

The integration writer defines and validates these in one file,
`app/ble_mesh_node/cmake/TronBleProfiles.cmake`:

| Variable | Values/default | Rule |
| --- | --- | --- |
| `TRON_NODE_MODE` | `LEGACY_FLOOD` (default), `TAVRN_ROUTED` | Exactly one selected. |
| `TAVRN_FEATURE_LEVEL` | empty (default), `AODV_ONLY`, `FULL_TAVRN` | Must be empty for legacy and nonempty for routed. |
| `TRON_ROUTED_TX_QUEUE_CAPACITY` | `4` (default), `8`, `16`, `40` | Accepted only for ROUTED; LEGACY and LINK compile fixed queue capacity 4. Default is a control variant, not a production selection. |
| `TRON_TIMER_PROFILE` | `BALANCED` (default), `FAST_TEST`, `SOAK` | Applies to shared and selected branch timers. |
| `TAVRN_ENABLE_LOCAL_REPAIR` | `OFF` (default), `ON` | `ON` valid only for routed `FULL_TAVRN` after repair exists. |
| `TRON_ENABLE_PATIENT_BRIDGE` | `OFF` (default), `ON` | Optional layer above either completed node branch after Phase 7. |
| `TRON_ENABLE_TEST_HOOKS` | `OFF` (default), `ON` | Master gate for all bench fault injection. |
| `TRON_HARDWARE_CANDIDATE` | `OFF` (default), `ON` | Build-purpose gate; ON enables strict routed board-bound provenance checks. |
| `TRON_NODE_ID` | `0` default or valid non-reserved 16-bit value | Legacy-only node label/override. It is invalid as a routed identity override. |
| `TRON_ADVA_OVERRIDE` | empty (FICR) or six colon-separated canonical bytes | Routed/link-harness chosen identity; must be empty for legacy. Generates all six override bytes and is used by both radio AdvA and SID derivation; invalid/reserved values fail configure or boot closed. |
| `TRON_TARGET_PROBE_UID` | empty by default | Exact CMSIS-DAP probe/board UID; required for routed hardware candidates. |
| `TRON_TARGET_INVENTORY_FILE` | empty by default | Immutable inventory input mapping target UID to canonical FICR AdvA; required and hashed for routed hardware candidates. |
| `TRON_NETWORK_ID` | profile-defined default | Must fit the wire contract. |
| `TRON_BENCH_ROLE` | `generic` default, sanitized label | Manifest/artifact label only; it must not alter routing. |

Routed/link-harness configuration accepts no independent logical-ID cache
variable. The generated canonical AdvA is the only identity input; local SID16
and SID8 are derived from it, while remote logical IDs arrive from validated
wire fields and direct N=1 bindings.

With `TRON_HARDWARE_CANDIDATE=OFF`, an empty AdvA override is a reusable generic
development build. Generated config/manifest uses the literal sentinel
`RUNTIME_FICR` in every build-time AdvA, SID16, SID8, and fleet-identity field;
no exact SID or board identity is claimed, and that artifact is ineligible to
substantiate routed hardware evidence. OFF may still use an explicit override
for deterministic development, but remains ineligible because build purpose is
OFF.

For routed/link-harness `TRON_HARDWARE_CANDIDATE=ON`, configure requires a
nonempty valid `TRON_ADVA_OVERRIDE`, `TRON_TARGET_PROBE_UID`, and
`TRON_TARGET_INVENTORY_FILE`. The inventory record for that UID must contain the
same canonical AdvA; its content hash and selected record are manifested.
Missing, duplicate, or mismatched records are fatal. Legacy builds retain their
existing identity contract and do not become a routed candidate through this
gate.

`CACHE STRING` variables advertise allowed values with the CMake `STRINGS`
cache property, and every invalid combination fails at configure time with
`message(FATAL_ERROR)`. CMake source selection occurs through centralized
`target_sources(... PRIVATE ...)`; compile definitions do not select whole
algorithms. `configure_file(... @ONLY)` generates the effective config header
and configure-time manifest fragment from the same validated variables.

`AODV_ONLY` is not selectable until Phase 2 links the real `aodv_core` and its
tests. Phase 1 exposes only `ble_link_v2_testbed`. `FULL_TAVRN` is selectable
only when its then-required real modules exist. An empty/no-op AODV, fake route
table, or TAVRN stub must cause configure/link failure rather than create a
mislabelled artifact.

### 14.2 Source manifests

The final source lists are additive and centralized. Feature selection is made
by CMake source closure, not source-name heuristics. The one routed binding may
use the generated `TRON_BUILD_ROUTED_FULL_TAVRN` boolean solely to declare and
initialize the selected optional GTT/FULL objects; it cannot select a second
link/AODV/DATA algorithm.

| Manifest | Sources |
| --- | --- |
| `SHARED_BLE_SOURCES` | `ble_radio.c`, `ble_mesh_tx_queue.c`, `ble_mesh_scheduler.c`, generated build-info source |
| `LEGACY_FLOOD_SOURCES` | `tron_mesh_packet.c`, `tron_mesh_dedupe.c`, `tron_mesh_pingpong.c`, `legacy_flood_node.c`, `tron_node_legacy.c` |
| `ROUTED_COMMON_SOURCES` | `tavrn_wire_v2.c`, `tavrn_link_v2.c`, `aodv_core.c`, `tavrn_router.c`, `tavrn_routed_node/src/routed_cycle.c`, and `tavrn_routed_node/src/main.c` |
| `FULL_TAVRN_SOURCES` (implemented Phase 5 hard-expiry verification closure) | `tavrn_gtt.c`, `tavrn_smart_ttl.c`, `tavrn_full.c`, `tavrn_esc.c`, `tavrn_mentorship.c`, `tavrn_maintenance.c`, and `routed_full_telemetry.c` only |
| `FULL_EXPIRY_BINDING_SOURCE` (implemented local expiry/demand and RREQ verification) | `tavrn_full_maintenance_binding.c` plus its public header; compiled only by FULL_TAVRN and called once by routed main. |
| `REPAIR_SOURCE` | `tavrn_repair.c` only when repair is on |
| `PATIENT_SOURCE` | `patient_bridge.c` only when patient bridge is on |
| `TEST_HOOK_SOURCE` | `tron_test_hooks_bench.c` only when test hooks are on; otherwise `tron_test_hooks_off.c` |

The table is the final source manifest. During incremental execution,
`FULL_TAVRN_SOURCES` contains only the accepted prefix reached in workplan order
and the external manifest lists those capabilities explicitly; it never names
or compiles a placeholder for a later module.

Final profile composition:

```text
LEGACY_FLOOD = main + SHARED_BLE + LEGACY_FLOOD + hook adapter
AODV_ONLY    = main + SHARED_BLE + ROUTED_COMMON + hook adapter
FULL_TAVRN   = AODV_ONLY sources + FULL_TAVRN [+ REPAIR]
patient      = selected node profile + PATIENT (optional, above node facade)
```

Linker maps and the sorted source manifest must prove that legacy contains no
routed source; AODV_ONLY contains no `tavrn_full`, GTT, Smart-TTL, ESC,
mentorship, maintenance, or repair source/header dependency; and the implemented
Phase 5 FULL closure contains only the common closure plus GTT, Smart-TTL,
`tavrn_full`, ESC, mentorship, adaptive ordinary-HELLO maintenance, implemented
local expiry/demand binding, targeted stage-0 and retained-hop/full-RREQ
verification runtime binding, copied FULL telemetry, and production-composed
general metadata/confirmed TC. Repair remains deferred. Accepted mentorship
bootstrap JOIN behavior remains unchanged.

### 14.3 Step 5f liveness delivery boundary

Local expiry/demand/direct evidence (`GTT-02`, `GTT-03`, `GTT-05`, `MAINT-01`,
`MAINT-02`, `MAINT-04`, `MAINT-05`), targeted freshness stage 0, and retained-hop
and full-diameter RREQ verification (`MAINT-06`, `MAINT-07`, `META-01`,
`META-03`, `META-04`) are implemented in the FULL-only closure.
`tavrn_maintenance` owns the fixed contexts and emits
typed actions; `tavrn_router` owns route lookup, control-token domain handoff,
and matching scheduler completion; `aodv_core` remains the sole RREQ/request-ID
owner. Targeted stage 0 hands the retained context to the implemented stage-1
and stage-2 RREQ slices. No slice adds expiry-driven LEAVE or other new TC behavior:
accepted mentorship bootstrap JOIN encoding/origination/relay/dedupe remains
unchanged, and later TC JOIN/LEAVE maintenance remains separate from hard-expiry
local departure. Before targeted RED acceptance, manifest/source closure proved
the then-current registry and lower-profile isolation. This timer slice now
proves the exact 75-key registry and canonical formulas without changing that
lower-profile isolation boundary.

## 15. Test-hook isolation

Bench hooks are one injected port, not conditionals inside radio/link/AODV
algorithms. `tron_test_hooks_off.c` implements the same interface with no-op
results. Only the selected node binding receives the port and passes it to the
boundary being faulted.

The exact build inputs are:

| Input | Default | Meaning |
| --- | ---: | --- |
| `TRON_TEST_RX_BLOCK_PEER_ID` | `0` | Legacy-only peer ID blocked under the existing full-TTL origin rule. |
| `TRON_TEST_RX_BLOCK_ADVA` | empty | Routed full canonical transmitter AdvA blocked at the admission seam. |
| `TRON_TEST_HACK_DROP_PEER_ADVA` | empty | Full canonical peer AdvA whose outgoing HACKs may be suppressed. |
| `TRON_TEST_HACK_DROP_COUNT` | `0` | Number of matching HACKs to suppress, including deterministic first-HACK tests. |
| `TRON_TEST_BUSY_ADMISSION_COUNT` | `0` | Number of otherwise valid DATA candidates whose router/application reservation is forced to resolve BUSY; no dedupe commit occurs. |
| `TRON_TEST_COLLISION_PEER_ADVA` | empty | FULL-only bench hook: combines the selected peer's nonreserved SID8 byte with local override bytes 1..5, producing a unique full AdvA/SID16 that deliberately duplicates only that peer's SID8. |

Any nondefault test input with `TRON_ENABLE_TEST_HOOKS=OFF` is a configure-time
error. Hooks are invalid for acceptance artifacts unless the specific hardware
gate requires them. Runtime startup logs and the external manifest print the
master flag and every hook value. Hook counters prove activation count; a hook
must not silently trigger.

The historical `TRON_NODE_BLOCK_DIRECT_PEER_ID` CLI option may remain only as a
temporary legacy alias that maps to `TRON_TEST_RX_BLOCK_PEER_ID`; it cannot
select a routed peer. Routed hooks use full AdvA so a SID collision cannot fault
the wrong board. No routed test hook accepts a SID without its full AdvA. The
manifest records every canonical field.

## 16. Artifact naming, effective manifest, and provenance

The publishing name is deterministic:

```text
tron-ble-legacy-na-<timer>-candidate<0|1>-repair0-patient<0|1>-<role>-id<id>-<commit12>.<ext>
tron-ble-routed-<aodv-only|full-tavrn-phase5-adaptive-hello>-<timer>-<role>-adva<adva>-<commit12>.<ext>
```

Tags are lowercase: `legacy` with feature `na`, `routed-aodv-only`, or the
fixed-k=1 mentorship plus adaptive HELLO build
`routed-full-tavrn-phase5-adaptive-hello`; timers are
`fast`/`balanced`/`soak`. Legacy uses `idficr` or a four-digit legacy
`TRON_NODE_ID`. Routed development with empty override uses filename tag
`advaruntime-ficr` and manifest identity exactly `RUNTIME_FICR`; an override uses
`adva` followed by all 12 lowercase canonical-array-order hex digits. A
collision-hook artifact uses the generated effective synthetic AdvA in its
filename while preserving the base override separately in the manifest. A routed
candidate can use only the latter and never a SID-only artifact tag.
The Phase 1 non-routing artifact is
`tron-ble-linkv2-harness-<timer>-candidate<0|1>-<role>-adva<adva>-<commit12>.<ext>`
with the same runtime-FICR/12-digit rule.
Published extensions are `.elf`, `.hex`, `.map`, and `.manifest` as available.

The manifest is newline-delimited `key=value`, UTF-8, keys sorted
lexicographically, with `manifest.schema=1`. It contains at least:

- `artifact.name`, `artifact.size`, `artifact.sha256`, and hashes/names for HEX
  and map files;
- `build.utc`, `build.target`, generator, build type, complete effective C/ASM
  flags, and `compile_commands.sha256` when generated;
- compiler executable, compiler version, CMake version, Ninja version, toolchain
  file path and SHA-256, build-script path and SHA-256;
- source commit, source tree, `source.dirty`, recursive submodule commit and dirty
  state, and a sorted selected-source list plus its SHA-256;
- requested/effective node mode, feature level (`NOT_APPLICABLE` for legacy),
  timer profile, hardware-candidate gate, repair, patient bridge, role, network,
  legacy node ID, canonical routed identity (`RUNTIME_FICR` or exact override),
  identity source, all configured override bytes, derived SID16/SID8 and fleet
  identity (`RUNTIME_FICR` rather than omission when runtime-only), target probe
  UID, selected inventory record/hash, wire-v1/v2 profile version, and
  implemented FULL_TAVRN capability list;
- every fixed capacity in section 12;
- every effective timer key in section 13, including derived discovery,
  mentorship, verification, response-window, scheduler/radio wall, incarnation,
  retry, and repair bounds used by acceptance;
- test-hook master, every hook input, and whether the build is eligible as an
  unhooked acceptance candidate.

The build script uses a clean temporary build directory. Hardware-candidate
publication fails on a dirty source or dirty submodule; an explicit exploratory
override may build, but records `source.dirty=yes` and
`candidate.eligible=no`. The artifact hash is computed after linking and cannot
be provided only as a CMake-cache value.

Before flashing a routed candidate, the evidence tool addresses the exact probe
UID, independently reads that board's FICR, reconstructs canonical AdvA, and
requires byte equality with both override and selected inventory record. A
mismatch aborts before erase/load and cannot be waived by the build manifest.
After boot, the candidate reports its runtime FICR-derived AdvA and active
scheduler AdvA; both must byte-equal the manifest identity and the preflash
observation or the run is invalid. Observed full AdvA/FICR, runtime boot nonce,
stable serial path, pyOCD version, flash command, and capture timestamps then
join the exact manifest and ELF SHA-256 as run evidence.

## 17. Build matrix and rollback

Required independently buildable cells after their implementation gate:

| Node mode | Feature | Repair | Wire | Required sources absent |
| --- | --- | --- | --- | --- |
| `LEGACY_FLOOD` | N/A | off | v1 | all link-v2/AODV/FULL sources |
| `TAVRN_ROUTED` | `AODV_ONLY` | off | v2 | GTT/ESC/mentorship/maintenance/repair |
| `TAVRN_ROUTED` | `FULL_TAVRN` Phase 5 adaptive HELLO | off | v2/SID8 bootstrap to SID16 | expiry, verification, metadata, expiry-driven TC maintenance, repair, patient |
| `TAVRN_ROUTED` | `FULL_TAVRN` later repair-on | on | v2 | none of the final routed set (not a Phase 5 build cell) |

Each valid cell builds with each timer profile once that profile's numeric
manifest is frozen. Patient bridge and test hooks are orthogonal additions and
must not be required to build any lower cell.

Rollback is a new build from the same accepted source commit with a lower
configuration: repair on to off, FULL_TAVRN to AODV_ONLY, or TAVRN_ROUTED to
LEGACY_FLOOD. It does not revert commits, load alternative route plugins, or
teach one image to reinterpret both wire versions. Legacy rollback must still
pass wire-v1 golden vectors; AODV_ONLY rollback must prove no FULL_TAVRN object
or header dependency in its linker/source manifests.

## 18. Forbidden includes and dependencies

The following are configure/checker failures:

1. `ble_radio` including scheduler, queue, wire, node, route, TAVRN, patient, or
   test-hook headers.
2. scheduler/queue including any wire-v1, wire-v2, node, route, TAVRN, or patient
   header.
3. legacy sources including routed, AODV, FULL_TAVRN, or patient internals.
4. wire codecs reading FICR, clocks, scheduler/radio state, route/GTT state, or
   allocating memory.
5. `aodv_core` including link-v2, GTT, Smart TTL, ESC, mentorship, maintenance,
   repair, patient, scheduler, or radio headers.
6. any module other than `aodv_core.c` storing or mutating a forwarding next hop
   and route lifetime/sequence tuple.
7. FULL_TAVRN modules calling scheduler/radio directly or replacing AODV
   forwarding/discovery fallback.
8. patient code calling link/AODV/GTT/scheduler enqueue/radio directly.
9. mode/feature `#if` blocks inside algorithms, duplicate source lists in build
   scripts, or C defaults that hide a missing generated profile key.
10. heap allocation in any firmware path named by this document.
11. any mesh source calling a compatibility/unbounded radio name instead of the
    typed `ble_radio_try_*` seam, or any driver wrapper containing an unbounded
    peripheral-state spin.
12. logging, display, serial I/O, patient/application callback, or other blocking
    work in the dedicated mesh task.

An include-dependency checker should enforce these path-level rules, and the
profile matrix should inspect source manifests/linker maps for accidental
inclusion.

## 19. Parallel lane ownership and integration order

Writers work from the same immutable phase base in separate worktrees and never
share an index or edit overlapping files.

| Lane | Owns | Must not edit |
| --- | --- | --- |
| shared foundation | `ble_radio`, scheduler, queue and their host tests | node algorithms, wire codecs, AODV, top-level CMake |
| legacy extraction | `legacy_flood_node` and legacy regression tests | wire-v1 codec semantics, shared files, routed files, CMake |
| wire-v2 | routed codecs and golden-vector tests | scheduler, link state, AODV behavior, CMake |
| link-v2 | custody/retry/flood state and virtual-time tests | wire offsets, AODV, FULL modules, shared files except through an agreed integration seam |
| AODV | `aodv_core`, control codec tests, deterministic simulator | link internals, FULL modules, CMake |
| each FULL feature | its one module and focused tests | AODV table/core internals, sibling FULL files, CMake |
| patient | classifier/bridge and focused tests | radio/link/routing internals, patient detection algorithm |
| build/provenance | profile registry, generated config/manifest tooling tests | protocol algorithms |
| integration writer | `tron_node` bindings, `main.c`, app CMake, build script, conflict resolution | changing frozen wire/profile semantics without parent reconciliation |

Integration is serial even when module writing is parallel:

1. Parent reconciles normative, wire/ACK/identity, and this architecture
   contract; contract checker clears cross-document conflicts.
2. Shared scheduler/queue extensions merge with legacy regressions.
3. Wire-v2 codec and link-v2 state merge into the dedicated non-routing harness.
4. Link hardware gate passes.
5. AODV core/router merges; `AODV_ONLY` becomes a valid profile only here.
6. Mandatory direct and forced-relay AODV hardware gate passes.
7. FULL_TAVRN modules merge in workplan order around the same AODV core: passive
   GTT/Smart TTL, fixed-k ESC/mentorship, maintenance, then optional repair.
8. Patient bridge merges above the accepted node facade.
9. Whole-matrix checker validates dependency paths, source/link manifests,
   rollback builds, and provenance before candidate hardware use.

Only an integrated, checker-accepted, clean commit is a hardware candidate.
Lane commits and the Phase 1 harness are never relabelled as routed profile
proof.

## 20. Architecture self-review gate

Before Phase 1, the parent integration/checker must confirm:

- the dependency graph is acyclic and forbidden includes are mechanically
  checkable;
- every later lower mode selects its own complete source set and remains
  independently buildable;
- one `aodv_core_t` and one 16-entry route table exist in every routed image;
- AODV_ONLY has no FULL_TAVRN header/object dependency;
- Phase 1 headers match the complete fixed types/statuses/events in section 8,
  DATA acceptance cannot commit dedupe/HACK before candidate resolution, and no
  direct-acceptance event path remains;
- route/origin/destination/dedupe types are logical-only, direct peers alone
  carry AdvA, local SID values derive from AdvA, and the public application
  array is ten bytes with SID16/patient limits enforced by codec validation;
- every Phase-0 vector decodes and round-trips through the typed DATA/HACK/FLOOD
  or generic validated-control codec API without a guessed AODV API;
- scheduler TX always uses its one stored canonical AdvA, raw RX validation
  precedes `RX_ADV`, every RSSI seam uses unsigned magnitude, RADIO waits are
  bounded, every TX result records requested/completed masks plus fault, and
  TX_FAILED can never masquerade as TX_DONE;
- routed custody service satisfies the 1000 ms rolling 40/32/8 BU policy,
  exact promoted-custody hold/EDF algorithm, 8/14/18 ms operation bounds,
  3048 ms conservative attempt bound, 9894 ms unconstrained wall, and fixed
  5000 ms absolute deadline, with four logical slots but exactly one
  promoted/queued/in-flight DATA attempt;
- one bounded expiry pass purges/counts anonymous work and emits at most one
  token-bearing `TX_EXPIRED` with zero physical evidence per poll;
- routed capacity variants 4/8/16/40 retain fixed-4 LEGACY/LINK equivalence, and
  pending capacity-40 poll/stack/benchmark evidence must meet `BUILD-05`;
- every owned DATA local/custody/radio/service terminal outcome preserves
  ownership; only `RETRY_EXHAUSTED` enters broken-link handling, while typed
  radio/service faults fail the complete mesh path closed and drain all slots;
- router common, not mentorship, owns N=1/boot-nonce invalidation in both routed
  feature levels;
- all build flags and profile values enter through the centralized registry and
  generated config;
- every capacity, timer, feature, hook, selected source, source revision,
  toolchain, and artifact hash needed to identify a candidate is manifested;
- generic routed builds use only `RUNTIME_FICR`, while every candidate is
  UID/inventory/override-bound and passes independent preflash plus runtime FICR
  equality checks;
- section 13's unique `timer.*` key set exactly equals the normative profile
  registry, wire/ACK/identity bytes agree with the complete Phase 1 public
  types, and Phase 2 freezes AODV-specific typedefs before implementation; and
- rollback changes only build selection, never source history or runtime wire
  interpretation.
