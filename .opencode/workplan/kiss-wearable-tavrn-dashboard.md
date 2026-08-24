# KISS wearable-to-runtime-root TAVRN dashboard PoC

## Status and intent

This is deliberately a **competition proof of concept**, not a production application stack. The difficult mesh work is already implemented in corrected FULL TAVRN. This plan adds bounded glue for a real wearable event to enter the backbone, fan out to every runtime-designated root, and appear once in a dashboard. Two simultaneous roots is the recommended competition configuration, not an architectural limit.

There is no C3, MQTT, broker, automatic election, or new RF work. TAVRN does not know what a wearable, root, or dashboard means. Root designation, root discovery, event fanout, UART records, and dashboard dedupe are application-layer policy.

## Pinned starting evidence

Planning used:

- `origin/master` at `648efe497624444e412cff28103fbabeffba5530`;
- wearable behavior from `origin/feat/fall-shout-transport` at `74d4931`;
- corrected FULL TAVRN at `49e2ee89ddc9d452fdec114cb5b57e0c0739a4aa`;
- corrected kernel submodule gitlink `19f30cd` and LLJY remote.

The wearable branch already provides:

- one 24-bit packet-ID space for incidents and heartbeats;
- stable identity across retransmitted copies;
- a 1000 ms event budget, about 100 ms per-event spacing, and at least eight copies;
- concurrent-event interleaving and non-starved 500 ms heartbeats.

Its current `TM/01 MIND_EVENT` uses the old flood-mesh relay. Integrated production changes that interpretation: `TM/01 MIND_EVENT` is a **direct wearable-to-application ingress envelope**. A TAVRN backbone node consumes it locally and never legacy-relays it.

## Two-plane architecture

```text
wearable TM/01 MIND_EVENT (same packet ID for 1000 ms)
       |
       v
+---------------- wearable RX / application plane ----------------+
| validate -> local seen cache 2000 ms -> fixed event queue       |
|                                      |                           |
|                          root table empty: consume silently      |
|                          active roots: local/routed fanout       |
|                                                                  |
| app-owned root registry + ROOT_STATE/ROOT_ACK application data   |
+------------------------------------------------------------------+
                                  |
                                  | ordinary opaque application DATA
                                  v
+---------------- TAVRN control and transport plane ---------------+
| TR/02 mentorship + GTT enumeration + AODV + HACK + repair       |
| no wearable membership; no TAVRN root concept                    |
+------------------------------------------------------------------+
                                  |
                                  v
root UARTs -> local HTTP bridge -> dashboard global dedupe
```

A root is a runtime Layer-7 role. `ROOT ON` can designate any backbone node until reboot. GTT is only a read-only topology/identity input punched through an application adapter; it has no root concept. Every node sends accepted wearable packets to every active root represented by the independent fixed MIND application registry. Zero roots is valid and silently consumes first-seen events. Two roots remains the normal competition/demo soft cap, but TAVRN, wire transport, API, and dashboard encode no two-root policy.

## Frozen application contracts

### 1. Direct wearable ingress

Reuse the compact `TM/01 MIND_EVENT` envelope:

- network ID: configured network;
- source: `0x0100 | DEVICE_ID`;
- packet ID: `seq24`, stable across the complete 1000 ms spray;
- type: `MIND_EVENT`;
- payload: exact seven-byte `mind_adv_payload_t` schema v1;
- TTL: exactly zero, explicitly forbidding legacy relay.

A recognized MIND frame is consumed by the application classifier whether valid or malformed; it never falls into TAVRN or legacy dispatch. Validation requires:

- complete TM wrapper, version, network, type, exact length, and TTL zero;
- `DEVICE_ID`/source in `01..fe` and source exactly `0x0100 | DEVICE_ID`;
- outer AdvA exactly `MIND_ADVA(DEVICE_ID)`;
- schema version 1, known event type, confidence/range validity;
- payload `seq == packet_id24 & 0xff`.

Non-MIND RX plus every TX/fault scheduler event passes unchanged to existing FULL mentorship/router handling. The old `ble_mesh_node` MIND relay, `tron_mesh_dedupe`, and ping/pong sources never enter the routed production source list.

### 2. Local seen-cache and ingress transaction

Key: `{network_id, wearable_src16, packet_id24}`.

- Capacity: 16 live keys, sufficient for the bounded competition workload but not claimed lossless for arbitrary fan-in.
- Retention: exactly 2000 ms from committed admission; deadline equality is expired.
- No live eviction: purge expired entries; a full live table counts/rejects without replacing a key.
- Ingress event queue: eight FIFO copied items; later fanout prioritizes control and urgent work without reordering the wearable seen guarantee.
- Ownership is one mesh-task transaction:
  1. fully validate;
  2. purge expired keys and detect duplicate;
  3. reserve a free/expired seen slot without mutation;
  4. reserve one event-queue slot;
  5. copy/publish the event item;
  6. commit the seen key at the same admission timestamp;
  7. roll back both reservations on every failure.
- A duplicate reserves nothing. Cache-full or queue-full leaves the key unseen so another spray copy can retry.

### 3. Layer-7 records carried by generic TAVRN DATA

Every PoC application record is defined and validated by `mind_application`, then carried as opaque bytes by the existing generic SID8 DATA seam. The application uses kinds `0x02..0x04`; no TAVRN protocol enum, validator, router policy, or GTT state learns these meanings. Oversized/SID16 submission fails through existing generic transport bounds; semantic malformed records are rejected only after Layer-7 delivery.

#### `MIND_REPORT = 0x02`

Exact ten-byte payload:

```text
byte 0..2  wearable packet_id24, little-endian (zero is legal)
byte 3..9  unchanged seven-byte mind_adv_payload_t
app_source wearable DEVICE_ID (01..fe)
urgent     event_type != HEARTBEAT
```

Wearable source is reconstructed as `0x0100 | app_source`; schema `seq` must equal the low packet-ID byte. Dashboard identity is `{app_source, packet_id24}`. TAVRN route origin and transport `data_seq` are not part of application identity.

#### `ROOT_STATE = 0x03`

Exact six-byte payload, `app_source=0`:

```text
byte 0     version = 1
byte 1     active = 0 or 1
byte 2..3  local TAVRN boot nonce, little-endian, nonzero
byte 4..5  root generation, little-endian, nonzero
```

#### `ROOT_ACK = 0x04`

The same six bytes plus byte 6 status:

```text
0 accepted
1 duplicate
2 capacity-rejected
3 stale
```

ROOT control uses ordinary unicast generic TAVRN DATA. TAVRN HACK still means immediate-hop custody only; application ROOT_ACK and all exact payload checks are Layer-7 concerns.

### 4. Runtime root state

- `ROOT ON`, `ROOT OFF`, and `ROOT STATUS` are exact UART commands terminated by CR or LF.
- Local designation is RAM-only and resets on reboot.
- Define independent Layer-7 `MIND_APP_ROOT_CAPACITY=16`. One application-owned registry stores local and remote canonical identities, current nonce/generation/requested state/disposition, plus one prior nonce. Its size is a node resource decision and has no equality/static assertion against GTT capacity.
- Accepted OFF remains as inactive high-water; it is not erased until explicit copied GTT departure. Capacity rejection occurs only when the application registry itself is full, never because a policy count reached two or because TAVRN imposed a root limit.
- The application reuses the nonzero boot nonce returned by `tavrn_router_incarnation_snapshot()`; it does not draw another root nonce.
- Every boot begins at generation 1, inactive. Once SID8 is established, that latest inactive state is announced to current and newly discovered members. This one-shot/event-driven withdrawal clears a rebooted former root without adding a lease.
- Every accepted local `ROOT ON` or `ROOT OFF` command advances to the next nonzero generation and starts a campaign. Repeated ON while active resynchronizes rejected/missing/new targets but does not replay the sound. Repeated OFF while inactive resynchronizes withdrawal.
- Commands received before SID8 immediately change local role/UI; only network campaign submission is deferred until SID8 is active.
- Same-nonce generation comparison is wrap-safe. A different nonce becomes the current session and shifts the old current nonce into the one-session tombstone; a message matching the tombstoned nonce is stale.
- Remote active-root entries clear on accepted newer OFF or explicit copied GTT `departed=true`. Mere absence, table eviction, soft stale, or hard expiry does not prove departure and does not clear application root history.
- There are no periodic application leases. TAVRN/GTT departure is the liveness authority so a missed application refresh cannot disconnect a valid root.

The recipient transition table is exact:

| Input for canonical root identity | History/result | Active-root effect | ACK |
| --- | --- | --- | --- |
| first/new-session OFF | store inactive high-water | remove matching prior active entry | `accepted` |
| first/new-session ON with history slot | store accepted ON | active in derived registry view | `accepted` |
| first/new-session ON with full retained history | preserve all retained state | unchanged | `capacity-rejected` |
| same nonce, newer OFF | store inactive high-water | remove active entry | `accepted` |
| same nonce, newer ON in retained history | store accepted ON | active in derived registry view | `accepted` |
| unknown same/new session with full retained history | preserve all retained state | unchanged | `capacity-rejected` |
| exact duplicate of accepted ON/OFF | preserve | preserve | `duplicate` |
| repeated previously rejected unknown state while registry remains full | no hidden rejection record; reevaluate | unchanged | `capacity-rejected` |
| repeated previously rejected unknown state after capacity frees | admit as a fresh candidate | apply ON/OFF normally | `accepted` |
| same nonce and generation but conflicting active bit | preserve | unchanged | `stale` |
| older generation or prior-nonce message | preserve | unchanged | `stale` |

Capacity rejection stores no hidden identity/nonce/generation tuple. Every repeated unknown ROOT_STATE is reevaluated against current application capacity, regardless of whether its generation changed. If capacity remains full it is rejected again; if a slot has become free it may be admitted normally.

Application root and campaign tables never evict live obligations. If an unknown root identity arrives while all 16 application registry entries are retained, existing state is preserved and the unknown ON **or OFF** receives `capacity-rejected`. A retained active root may outlive replacement of its GTT entry because absence is not departure; it remains unresolved and consumes application capacity until rediscovery, accepted OFF, or explicit departure. `MIND_APP_CAMPAIGN_CAPACITY=16` is another Layer-7 bound. If campaign storage is full, existing obligations remain intact, newly observed topology targets are counted/deferred, and later copied snapshots retry addition.

### 5. Read-only GTT topology projection and application campaigns

Every local current root state, active or inactive, campaigns after SID8 establishment. A `mind_topology_adapter` copies canonical identities, freshness, and explicit departure facts from `routed_full_telemetry_snapshot_gtt()` into application-owned records. It resolves destinations through existing `tavrn_full_resolve_unique_sid8()` and origins through `tavrn_esc_resolve_sid8()`. It never writes GTT and never defines application capacities from GTT constants.

- Up to `MIND_APP_CAMPAIGN_CAPACITY` application obligations. The adapter skips self and does not assume a control-plane maximum. The frozen snapshot seam reports only OK or non-OK: non-OK preserves all application state, increments snapshot-failure evidence, and retries later. A full OK snapshot is never interpreted as proof of truncation.
- The latest ROOT_STATE creates a campaign over current nondeparted GTT members and still-nondeparted prior targets.
- Newly discovered members are added while the current active or inactive state remains the latest local state.
- Each target receives a deterministic hashed initial due time of 20..120 ms. A separate global throttle guarantees at least 50 ms between actual ROOT_STATE submissions.
- Missing ACK targets retry no faster than every 3000 ms until ACK, explicit withdrawal completion, or GTT departure.
- Every recipient answers accepted, duplicate, capacity-rejected, or stale. Accepted/duplicate complete success; capacity-rejected/stale complete rejected; only no ACK remains retryable.
- An incoming ACK completes only when its SID8 origin resolves to the exact target canonical AdvA and all six echoed ROOT_STATE bytes match the current campaign tuple. Unresolved/wrong sender, mismatched/old tuple, and duplicate ACK after completion are counted and cannot complete another target.
- Resolver behavior is total: `UNIQUE` permits submission; `UNKNOWN` or `COLLIDING` retains the obligation, counts it, and retries after later GTT snapshots/rediscovery; `RESERVED` or `INVALID` is fail-closed with exact application-invariant evidence and no submission/state removal. Only accepted OFF or explicit departure clears a retained target.
- A later accepted ROOT ON/OFF advances generation and starts a fresh campaign for every current target, including previously rejected targets.
- The root exposes announced/acked/rejected/pending counts in status records.
- Leaves do not retransmit another root's announcement. This is application unicast fanout, not a second topology-control protocol.

### 6. Wearable event fanout

When application processing begins for an eight-slot ingress item, it snapshots every active registry entry into a fixed `MIND_APP_ROOT_CAPACITY` target set.

- zero roots: consume invisibly, increment only an internal saturating test/status counter, and retain no future-delivery queue;
- local root: publish directly to the final application inbox without self-routing;
- remote root: resolve canonical root AdvA to current SID8 through FULL/GTT and submit ordinary `MIND_REPORT`;
- any number of roots up to the bounded registry capacity: retain independent per-target progress in the same item; one target cannot erase or complete another.

One mesh-owner arbiter selects at most one application submission in each existing routed application-submit phase and correlates its result to one exact retained owner. Transient `NOT_READY`/mailbox/router `BUSY` retains that target. Resolver `UNKNOWN`/`COLLIDING` also retains and retries after snapshots/rediscovery; `RESERVED`/`INVALID` latches fail-closed application evidence without removing the target. Router `OK` or `QUEUED` completes the PoC forwarding handoff; only accepted OFF or explicit GTT departure cancels matching retained event targets. There is no end-to-end event ACK. Priority is:

1. `ROOT_ACK`;
2. urgent `MIND_REPORT`;
3. `ROOT_STATE`;
4. heartbeat `MIND_REPORT`.

Different observing backbone origins may deliver the same wearable key to each root. This is deliberate redundancy and remains visible in UART/API records; only dashboard state globally deduplicates it.

### 7. Final application reservation and HACK truth

Reuse the existing generic router `reserve/commit/cancel` contract unchanged. The reserved object is opaque transport DATA until the application consumes and validates it:

- destination reserves one of eight fixed final-inbox slots before `HACK_ACCEPTED`;
- full inbox returns BUSY and creates no accepted link-dedupe entry;
- commit retains the item until root-state processing or an eight-slot logger-record reservation succeeds; logger pressure cannot lose accepted DATA;
- retransmitted transport DATA produces `HACK_DUPLICATE`, no second reservation, and no second UART line;
- after ingress commit, downstream pressure never removes the 2000 ms seen key; direct-local BUSY retains and retries the original item/target;
- ROOT_STATE/ROOT_ACK state mutation occurs only under mesh ownership; the logger consumes copied records and never changes root state;
- reports from different TAVRN origins are separate UART reports even when they share one dashboard key.

DATA tests prove byte preservation. HACK tests prove correlation/status only because HACK carries no application bytes.

### 8. Persistent display and root cue

Add a production application node number `1..6`, independent of benchmark role labels.

- Leaf: persistently displays its number using application glyphs that reserve bottom-right `(4,4)` and `(3,4)` indicator pixels.
- Zero roots: both indicator pixels off.
- One root: rightmost indicator flashes at 500 ms.
- Two or more roots: both bottom-right indicators flash together at 500 ms. This is a deliberately saturated presentation state, not a routing or registry cap.
- Local root: persistently displays `R`; no leaf indicator overlay.
- Root designation transition plays one short cockpit-style two-tone cue on the micro:bit v2 built-in P0.00 speaker using PWM0. Idempotent ROOT ON does not replay it.
- Display/audio are application-owned, nonblocking to the mesh, and run at lower execution priority than mesh ownership (uT-Kernel numeric priority greater than 10). The existing priority-8 display task must not be reused unchanged above the priority-10 mesh task.
- PWM sequence and framebuffer storage are static; no heap and no radio/scheduler edits.

### 9. Exact firmware UART interface

#### Commands

Inherited startup already installs the sample UART0 vector but does not open `sera`. After that startup and before mesh/logger tasks, the application replaces the vector with `tk_def_int(INTNO(UART0_BASE), TA_HLNG handler)`, clears RXDRDY/ERROR/ERRORSRC, disables TXDRDY interrupt, enables only RXDRDY|ERROR at NVIC priority 5, and starts a 32-byte static RX ring. The handler drains RXDRDY and records/clears errors but never clears TXDRDY or writes TXD. T-monitor remains the sole UART TX owner; the kernel submodule/sample driver is unchanged and `sera` is never opened. A task below mesh priority parses a 16-byte line buffer and publishes copied commands. It must not call blocking `tm_getline`, `tm_getchar`, or `tm_rcv_dat`.

```text
ROOT ON\r
ROOT OFF\r
ROOT STATUS\r
```

Every complete command attempt produces this exact line:

```text
mind_command_v1 now=<u32> local=<12hex> command=<on|off|status|invalid> status=<accepted|duplicate|busy|malformed|overflow|rejected>
```

Malformed/overlong/ring-overflow/mailbox-full attempts are counted. Hardware acceptance includes receiving `ROOT STATUS` intact while mesh and logger traffic are active.

#### Event record

One line per local or routed observer report, exact key order:

```text
mind_event_v1 now=<u32> root=<12hex> wearable=<u8> packet=<6hex> schema=1 event=<u8> confidence=<u8> svm=<u16> mic=<u8> seq=<u8> observer=<12hex> path=<local|tavrn>
```

#### Root/status record

```text
mind_root_v1 now=<u32> local=<12hex> node=<1..6> role=<leaf|root> roots=<u8> announced=<u8> acked=<u8> rejected=<u8> pending=<u8> rootless_drop=<u32>
```

Values are decimal except fixed lowercase hex fields. One logger-owned queue emits records; mesh/application callbacks only copy fixed records. Unrelated diagnostics remain legal and the host parser ignores them. Overlong/partial firmware lines never mutate host state.

### 10. Local serial-to-HTTP bridge

Use one Python standard-library host process, no framework and no Web Serial:

- accept repeated `--serial /dev/serial/by-id/...` arguments at 115200 without a semantic two-device limit;
- configure POSIX serial with `termios`, poll/reconnect without external packages;
- parse only exact `mind_event_v1`, `mind_root_v1`, and command-status lines;
- retain a 256-record cursor ring; duplicates pass unchanged;
- serve built dashboard and API on `127.0.0.1:8787` same-origin;
- maximum API page size 100.

`GET /api/events?after=<cursor>&limit=<1..100>` returns:

```json
{
  "schema": "mind.api.v1",
  "gap": false,
  "oldest_cursor": 1,
  "current_cursor": 9,
  "events": [{
    "cursor": 9,
    "kind": "event",
    "device": 0,
    "now": 1234,
    "root": "1842de524add",
    "wearable": 1,
    "packet": "00002a",
    "schema": 1,
    "event": 3,
    "confidence": 75,
    "svm": 2400,
    "mic": 0,
    "seq": 42,
    "observer": "dc4b0a0603f8",
    "path": "tavrn"
  }]
}
```

A `root` ring record has exact fields `{cursor,kind:"root",device,now,local,node,role,roots,announced,acked,rejected,pending,rootless_drop}`. A `command` record has `{cursor,kind:"command",device,now,local,command,status}`. A stale cursor returns `gap=true` and starts at the oldest retained record.

`GET /api/health` returns HTTP 200:

```json
{
  "schema": "mind.health.v1",
  "oldest_cursor": 1,
  "current_cursor": 9,
  "devices": [{
    "device": 0,
    "path": "/dev/serial/by-id/example",
    "connected": true,
    "parse_errors": 0,
    "overlong_lines": 0,
    "reconnects": 0,
    "last_record_cursor": 9,
    "root": null
  }]
}
```

Repeated `--serial` arguments are assigned stable nonnegative device indices in command-line order for the process lifetime. `POST /api/root` accepts only exact JSON `{"device":0,"active":true}` or `false`: HTTP 202 returns `{"schema":"mind.command.v1","accepted":true,"device":0,"command":"on"}` after the full command is written; malformed JSON/fields returns 400, unknown device 404, and disconnected/write failure 503 with `accepted:false` and a stable `error` string. Command-status UART records also enter the cursor ring.

GET defaults are `after=0` and `limit=100`; results contain records with cursor strictly greater than `after`. Nondecimal/negative `after`, `limit` outside `1..100`, duplicate or unknown query fields return HTTP 400:

```json
{"schema":"mind.error.v1","accepted":false,"error":"invalid_query"}
```

POST errors use the same exact shape with `error` in `invalid_json`, `invalid_body`, `unknown_device`, `disconnected`, or `write_failed`. A non-null health `root` is the exact latest root-ring object without its cursor/device fields. Each device has one bounded serialized command-writer queue; concurrent POSTs cannot interleave bytes. Partial writes remain queued until complete or reconnect/failure produces the exact 503 response.

### 11. KISS dashboard

Selectively port only useful historical visual components. Remove MQTT, broker/C3 concepts, mesh panels, operator acknowledgments, and decorative analytics.

The dashboard shows:

- bridge/serial health and dynamic root controls for every configured serial device;
- current root state;
- latest status per wearable;
- one event feed with observer/root/path evidence.

Session-global state is keyed by `{wearable_id, packet_id24}`:

- first report creates one logical record;
- later API duplicates, observer duplicates, and second-root duplicates aggregate bounded observer/root/path evidence without a second event;
- conflicting payloads for one key are visibly flagged and counted;
- reconnect overlap is idempotent;
- different packet IDs or wearable IDs remain distinct;
- cursor gaps, loading, empty, malformed API, stale, offline, and reconnect states are visible.

Vite development uses a proxy; production assets are served same-origin by the bridge.

## Reproducible history reconciliation

1. Create a dedicated integration branch from `origin/master@648efe4`, not stale local `master`.
2. Merge corrected TAVRN `49e2ee8`.
3. Retain corrected `.gitmodules` and kernel gitlink `19f30cd`.
4. Preserve main's wearable and seven-byte `shared/schema.h` binary layout through add/delete conflicts.
5. Assert these are byte-identical to `49e2ee8` after reconciliation:
   - `microbit/app/drivers/ble_radio.{c,h}`;
   - `microbit/app/protocol/ble_mesh_scheduler.{c,h}`;
   - all `microbit/app/protocol/tavrn_*.{c,h}`, including wire, router, GTT, FULL, and ESC implementation.
6. Selectively port wearable source behavior/tests from `74d4931`; do not cherry-pick driver relocation `4266b13`, old `ble_mesh_node` MIND relay, or `wearable_mesh_demo.kdl`.
7. Adapt wearable CMake to the integrated `microbit/app/drivers` layout; do not import `microbit/lib` solely for the wearable branch.
8. Update `shared/schema.h` comments to remove C3 authority and clarify that `seq` is compatibility data; preserve exact seven-byte layout.
9. Use historical dashboard commit `0c1557ed16aa50bea298a4d2dfe4d8cb2ff36604` only as a visual reference. Retain `.gitignore`, `index.html`, package/TypeScript/Vite scaffolding, `main.tsx`, `Header.tsx`, wearable/incident presentation components, `format.ts`, and `styles.css`. Author `App`, local API client, state store, and tests afresh. Remove `mqtt`, `useMqtt`, connection drawer, node panels/cards, sparklines, acknowledgments, broker assets/configuration, and the MQTT dependency.

## Execution workflow: slices and waves

A **slice** is a higher-level deliverable. A **wave** is one planned set of non-overlapping implementation subagents within a slice. Writers in a wave may run in parallel only when file ownership is disjoint. After every writer in that wave settles, exactly **one code-checker** reviews the combined wave output. A checker does not review individual writers independently.

```text
slice
  wave 1: writer(s) -> one combined-wave checker -> valid fixes/recheck
  wave 2: writer(s) -> one combined-wave checker -> valid fixes/recheck
next slice
```

The structured workplan defines eight waves across six slices:

| Slice | Wave | Planned writers | Checker boundary |
| --- | --- | --- | --- |
| A | A1 | one serial reconciliation/profile writer | complete baseline wave |
| B | B1 | Layer-7 record codec; wearable; ingress/cache | combined ingress wave |
| C | C1 | one topology/root/UART/UI integration writer | complete application root-plane wave |
| D | D1 | one application fanout/final-delivery writer | complete forwarding wave |
| D | D2 | one measured resource-closure writer | complete resource evidence |
| E | E1 | serial API; dashboard | combined host wave |
| F | F1 | firmware gates/fixes; host gates/fixes | complete software evidence |
| F | F2 | hardware execution/evidence | final hardware wave and complete diff |

Valid checker findings are fixed within the same wave and receive only the necessary consolidated recheck. There is no checker after each writer and no redundant slice-level checker after all constituent waves already passed.

## Required validation gates

1. Frozen radio/scheduler hashes match `49e2ee8`; submodule URL/gitlink match the reconciliation contract.
2. Selected routed production sources exclude old flood-node MIND relay/dedupe/pingpong and benchmark sources.
3. Production manifest proves FULL TAVRN, repair ON, wearable ingress ON, node number set, benchmark/hooks/diagnostic initiator/RF block OFF.
4. Wearable host tests and exact TTL-zero advertisement vectors pass.
5. Classifier isolation, two-resource seen/outbox rollback, 2000 ms equality/wrap, capacity/churn, and no TAVRN mutation tests pass.
6. Layer-7 application record golden vectors and malformed-record rejection pass over unchanged generic SID8 transport; existing router custody, duplicate DATA/HACK, and final BUSY regressions pass without TAVRN source changes.
7. Application root transition/high-water table, inactive boot withdrawal, pre-SID8 deferral, ON/OFF/status, 0/1/2/16/full+1 registry matrix, repeated capacity rejection followed by acceptance after a slot frees, exact active count, full campaign/event targets, topology snapshot OK/non-OK preservation/retry, unresolved rediscovery, existing GTT/SID8 APIs, backoff/throttle, typed application ACK outcomes, arbiter, saturated UI, cue, and RX-interrupt UART tests pass.
8. Existing link/router/GTT/mentorship/maintenance/repair/build-profile regressions pass.
9. Wearable and production FULL firmware build. Compile-time `sizeof` evidence names every new application structure and aggregate; measured production `.data`/`.bss` delta matches the declared application fixed-state allowance. The callsite-complete inherited mesh chain uses an explicitly revised 4096-byte measured-chain ceiling and 4864-byte task allocation; the independent >=1024-byte mesh headroom gate makes 3840 bytes the effective maximum. Runtime reserve is 13360 bytes and post-reserve RAM remains >=8192 bytes. Every production FULL `routed_mesh_task` invocation—ingress OFF/ON, stack-baseline-only, live-resource, and retained legacy FAST/BALANCED—must reject v2 and use a published/hashed callsite-complete v3 contract. D2 uses an explicit paired `D2_APPLICATION_INCREMENTAL` acceptance: build FULL+repair BALANCED node 6 ingress OFF, pass its current gate, externally seal it, then build/validate ingress ON. OFF and ON must both record `resource.gate=PASSED`. Common selected-source file hashes cannot change; ON may add only `display.c` and the 13 `mind_application` sources (`wire`, `ingress`, `event_forwarder`, `topology_adapter`, `root_plane`, `root_coordinator`, `root_inbox`, `command`, `log`, `log_formatter`, `uart`, `audio`, `ui`), with no removals. The generated config header may change only `TRON_BUILD_ENABLE_WEARABLE_INGRESS` 0->1; semantic build-manifest differences are limited to adding the `mind-root-plane` capability, wearable-ingress requested/effective OFF->ON, root-plane OFF->ON, and the exact source list/hash additions. Repair, timer, node number, capacities, hooks, benchmark, stack allocations, reserve, source state, and all other semantic fields remain identical. The paired report hashes both artifacts, the external seal, checker outputs, preprocessed mains, contracts, and size report, states `acceptance.scope=D2_APPLICATION_INCREMENTAL` and `historical_expiry_fixed_state=USER_WAIVED_NOT_REPRODUCED`, and never claims to reproduce historical expiry fixed-state evidence. No TAVRN capacity or protocol source changes. New application modules allocate dynamically nowhere; inherited kernel/device startup allocation remains unchanged apart from the measured mesh task reservation.
10. Bridge parser/API cursor/reconnect/write tests and dashboard state/build/rendered accessibility checks pass with 16 mocked serial/root devices as well as the normal one/two-device flow.
11. `git diff --check` and complete source/config review pass. Any unrelated pre-existing red gate is explicitly named.

## Competition hardware acceptance

Use one test-injector wearable, at least three backbone nodes, and **two concurrent root UART captures** for the recommended/mandatory competition hardware pass. Separate Layer-7 host/application/resource tests prove the independent 16-entry application registry, fanout, serial indexing, and dashboard behavior; this does not alter or claim a matching TAVRN topology capacity:

1. Wearable sends at least eight copies with one stable packet ID over about 1000 ms.
2. Each receiver admits that key at most once inside 2000 ms and counts later copies.
3. No wearable identity appears in GTT, mentorship, AODV, or route state.
4. With zero roots, receiver/application counters prove ingress and silent rootless consume; no event appears in UART/API/dashboard.
5. `ROOT ON` over UART creates one local root, plays one audible cue, displays `R`, sends paced ROOT_STATE to every current GTT member, and receives typed ACK from each.
6. Designating the second root makes every leaf preserve its number while flashing both bottom-right LEDs, creates two event destinations, and produces records on both UARTs. Layer-7 tests prove all 16 application registry entries receive independent targets and exact counts, the next unknown application root is rejected only because the node's application array is full, and UI remains in the saturated two-or-more indication.
7. At least one non-root observer routes MIND_REPORT through TAVRN; direct root reception uses the local path; forced multihop may use existing hooks only in test artifacts.
8. Both root UART/API streams contain reports for the same wearable key, while dashboard shows one logical event and aggregates both roots/observers.
9. ROOT OFF returns the system to one-root behavior; a separate explicit GTT-departure exercise clears a root and retained event targets. A rebooted former root's inactive boot announcement also withdraws stale state. No periodic lease is involved.
10. Production FULL nodes show no terminal router/link/scheduler faults during the bounded demonstration.

This evidence does not claim RF coverage, RX availability under saturation, exactly-once delivery, reboot durability, security, or production readiness.

## F2 hardware-remediation addendum

Hardware execution found three blockers after F1: later UART commands were not promoted after a successful `ROOT ON`, only heartbeat traffic was available for two-root proof, and the routed wrapper eventually reported phase-5 `AODV_STATUS_INVALID` without a terminal router fault. User approval is limited to Layer-7 diagnosis and remediation; all frozen TAVRN/protocol files remain unchanged.

1. **Diagnose ROOT OFF before changing ownership:** preserve the existing ISR-tail/parser-head+mailbox-tail/coordinator-mailbox-head SPSC split. Add a production-lifecycle test that drives the real ISR/ring/parser mailbox and coordinator transactional peek/consume for immediate and delayed OFF, logger reservation pressure, BUSY/NOT_READY campaign work, and mailbox backpressure. Add bounded byte/parse/publish/peek/consume evidence and correlate it with the first phase-5 invalid timestamp. Do not use `mind_uart_take_attempt()` as the production seam and do not change ownership unless the evidence proves it defective.
2. **Test-only wearable injector:** add a dedicated `wearable_test_injector` build target rather than a cached production option. Only that target defines the injector macro; ordinary `wearable_app` never receives it. Seed one deterministic fused non-heartbeat incident into the existing fusion -> admission -> 1000 ms `tx_adapter` spray path. Seal the pre-change wearable ELF/HEX and prove the post-change production target is byte-identical. Hardware triggers the test by resetting/flashing the injector only after roots and typed ACKs are ready.
3. **Exact phase-5 diagnosis:** in application-owned `main.c`, latch one static first-invalid snapshot with a stable branch enum, validity bits, raw router status, FULL binding/repair statuses, mailbox take/publish status, expiry-sweep enqueue outcome/queue occupancy, and existing link-step/link-event fields. Print it once outside the mesh callback; do not add recurring phase-10 output or a mesh-stack local aggregate. Any new static symbol/formatter must enter target-ABI/ELF component accounting and rerun D2. If evidence identifies inherited expiry telemetry, FULL mailbox, maintenance/repair binding, or raw router behavior, stop and escalate; Layer-7 approval does not authorize changing those semantics.
4. **Validation order:** seal baseline; focused UART lifecycle and injector wiring tests; default/injector builds and exact default equivalence; source-freeze; affected application/profile/resource/D2 gates; diagnostic hardware classification; then final UID-targeted pyOCD hardware with one injector wearable and exactly three active backbone radios. Retain every acceptance item at lines 420-429, wait for root-state typed ACK readiness before injector reset, and monitor all nodes continuously for at least 180 seconds after root stabilization, covering the previously observed 153-second failure window.

## Historical F2 observations — unpreserved, rerun required

The paths below recorded useful observations at the time, but they were stored
only on tmpfs and were lost after a workstation reboot. They are retained here
as historical provenance, not as independently auditable acceptance evidence.
Current hardware acceptance remains open until the same flows are rerun and a
redacted, checksum-bound bundle is committed under `microbit/hardware-results/`.

- Request-driven quiet logger dispatch removed the deterministic scheduler-yield failure without modifying frozen TAVRN. Quiet production now drains but does not format valid heartbeat MIND records; command, root, incident, and terminal-fault evidence remains visible.
- Application, scheduler, telemetry, full build/profile+D2, wearable/injector, host, and dashboard gates pass. The final D2 report is `/tmp/opencode/d2-heartbeat-filter.report`; paired artifacts are `/tmp/tron-d2-application-incremental.U7iQ10`.
- `/tmp/opencode/f3-heartbeat-filter-captures/root-off-sequence.log` proves zero/one/two/one/zero runtime roots. All 19 writes produced command and root records, all final status records report `acked=2`, and no heartbeat, cycle-fault, or router-fault record appeared.
- `/tmp/opencode/f3-heartbeat-filter-captures/api-events.json` proves the deterministic fall-and-shout incident reached both roots with two local and two TAVRN observer records. `/tmp/opencode/f3-heartbeat-filter-captures/dashboard-two-root-incident.png` proves global dashboard dedupe and four-observer aggregation.
- Fresh firmware artifacts are under `/tmp/opencode/f3-heartbeat-filter-builds`. Nodes 1-3 and the injector wearable were flashed by explicit UID; nodes 4-5 remained parked and untouched.

## Five-node production stress extension

- Production `tavrn_routed_node` treats ordinary timing-bound crossings as observe-only (`ROUTED_CYCLE_TIMING_OVERRUN_TERMINAL=0`). The 2 ms target and phase attribution remain diagnostic; genuine router, scheduler, radio, invalid-state, and structural timestamp failures remain terminal. Generic host tests retain strict timing by default.
- The policy changes no state layout or RAM. Full profile and paired D2 acceptance pass with `/tmp/opencode/d2-observe-only-timing.report` and `/tmp/tron-d2-application-incremental.zc4Lxw`.
- `/tmp/opencode/f5-observe-only-stress/thirty-minute.log` proves five production backbone nodes, node 1 as the sole root, and one production wearable for 1800 seconds. Every node reported `roots=1 announced=4 acked=4 rejected=0 pending=0`; six sparse status commands completed; no cycle/router fault or heartbeat log appeared; the wearable produced 1,443 accelerometer samples with zero errors.
- Fresh five-node artifacts are under `/tmp/opencode/f5-observe-only-builds`. All five backbone nodes were flashed by explicit UID; the wearable remains on production patient firmware.

## Long-run router-terminal recovery extension

- The indefinite five-node run exposed genuine `DATA_TERMINAL_HOOK_INVALID` latches in node 5 and node 3. Peer removal across surviving nodes proved these were target-side link retry/custody failures at the router/local-repair boundary, not UART corruption or Layer-7 behavior.
- A known hook `INVALID` before ownership handoff now degrades to `DECLINED`, increments the existing invariant counter, and follows normal router cleanup for immediate terminals, retained retry obligations, and successful-transfer observation. Ownership-split, impossible-disposition, and corrupted retained-state cases remain terminal.
- Every remaining reason-11 terminal site carries a compact subreason in existing router padding and prints once as `routed router_fault reason=<n> subreason=<n>`. This changes no fixed-state size.
- Phase 3/4/5/6, application, full profile, and paired D2 gates pass. Evidence: `/tmp/opencode/d2-terminal-recovery.report`, `/tmp/tron-d2-application-incremental.jM8aPg`; replacement node builds: `/tmp/opencode/f6-terminal-recovery-builds`.
- The old faulted run stopped cleanly after 18,101.962 seconds and remains archived under `/tmp/opencode/f5-observe-only-stress/indefinite`. All five nodes now run F6 terminal-recovery firmware. A clean `tron-indefinite-stress` session started at `2026-08-16T05:01:41.799Z`; its initial five-node sweep reports `announced=4 acked=4` everywhere with zero failures. New evidence is under `/tmp/opencode/f5-observe-only-stress/indefinite-f6`.

## Explicit deferred risk

The nRF52833 has one radio. Heavy TAVRN/application TX can reduce wearable RX opportunities, and high wearable fan-in can increase TAVRN TX. This workplan intentionally does **not** change radio or scheduler behavior, tune duty cycles, or solve RX exhaustion. The 1000 ms wearable spray and 2000 ms local cache are the only current PoC mitigations. RF/scheduler hardening remains a separate later workplan.

## Handoff

Software implementation and current host/resource gates are complete. Competition hardware validation and the five-node stress extension are not closed because their cited tmpfs evidence was lost. Rerun rootless, one-root, two-root, withdrawal, injector, fanout, dashboard-dedupe, and sustained fault checks on the current commits; persist logs, manifests, API output, screenshots, hashes, and reproduction instructions before marking Slice F or this workplan completed.
