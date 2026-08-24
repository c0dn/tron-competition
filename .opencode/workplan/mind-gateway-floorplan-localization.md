# MIND gateway floorplan and rough RSSI localization

## Outcome

Ship a competition-oriented, single-PC dashboard that treats the sole serial-connected backbone as the **Gateway**, lists remote nodes from that gateway's GTT, persists a floorplan and node coordinates on host disk, globally deduplicates wearable incidents, and places each logical incident approximately from the original RX RSSI reported by multiple positioned observer nodes.

Internal firmware and API root semantics remain `ROOT`; only visible product language and the 5x5 local indicator become `GATEWAY`/`G`.

## Frozen architecture decisions

### One gateway, GTT-owned roster

- Final launch accepts exactly one serial path and rejects zero or multiple
  `--serial` arguments before HTTP startup. Generic transport internals may
  remain for isolated regression tests, but production has one physical owner,
  `device=0`.
- The gateway still becomes active through existing `/api/root` and `ROOT ON/OFF`; no automatic or remote root designation is added.
- Every dashboard ROOT/GTT command targets physical owner device 0. Active-root
  source selection and the multi-source selector are removed.
- Gateway identity fails closed unless the complete GTT contains exactly one
  self entry matching `gtt.local`. If current root status exists,
  `root.local` must also equal `gtt.local`. Missing/duplicate/mismatched self
  identity makes the gateway roster and localization unavailable; no self node
  is synthesized.
- Every displayed backbone node comes from that validated complete gateway GTT.
- GTT AdvA is the stable identity. The local entry is labeled **Gateway**. Remote entries use compact/full AdvA and freshness; GTT indices and serial values are never presented as invented node numbers.
- Positions remain persisted by AdvA when a node temporarily disappears, but current roster/status comes only from GTT.

### Exact observer-RSSI wire record

The wearable's direct schema-v1 advertisement remains unchanged. Each backbone already captures a positive `rssi_magnitude_db` when it first admits that stable event identity; RSSI is currently discarded in the event forwarder.

Add application kind `MIND_REPORT_OBSERVED = 0x05` with exact 10-byte payload:

| Byte | Field |
|---:|---|
| 0..2 | `packet_id24` little-endian |
| 3 | schema version |
| 4 | event type |
| 5 | confidence |
| 6..7 | accel SVM little-endian |
| 8 | mic level |
| 9 | original observer `rssi_magnitude_db` (`0` unavailable, `1..127` valid) |

A dedicated observed-report packer first validates the original seven-byte
payload, including `seq == packet_id24 & 0xff`, before omitting `seq`. Unpack
reconstructs `seq` and exposes RSSI separately. The exact 10-byte observed
record is the sole RSSI carrier through both local publication and TAVRN
forwarding. `MIND_REPORT` 0x02 remains accepted and produces RSSI unavailable.
New homogeneous firmware emits 0x05 for direct ingress; mixed fleets are not
claimed compatible for enriched forwarding and must upgrade atomically.

The exact enriched UART record is:

```text
mind_event_v2 now=<u32> root=<hex12> wearable=<u8> packet=<hex6> schema=1 event=<0..5> confidence=<0..100> svm=<0..8000> mic=<0..255> seq=<u8> observer=<hex12> observer_rssi_dbm=<-127..-1> path=<local|tavrn>
```

Magnitude zero cannot produce v2 and is logged through the unchanged
`mind_event_v1` grammar. The host parses v1 and v2, then emits `mind.api.v2`
where every event has required `observer_rssi_dbm: number | null`; v1 becomes
`null`. Real end-to-end vectors must pass through SID8 DATA encode/decode and
root inbox. Ten-byte SID16 submission remains rejected.

### Durable floorplan state

Default state root:

```text
${XDG_DATA_HOME:-$HOME/.local/share}/tron-dashboard/
```

An explicit absolute `--state-dir` overrides it. Relative/empty
`XDG_DATA_HOME` is ignored per the XDG base-directory contract; fallback is
`$HOME/.local/share`. The directory is mode `0700`; one
`dashboard.sqlite3` file is mode `0600`.

SQLite is the complete durability boundary. One singleton row stores revision,
canonical positions JSON, and nullable floorplan SHA-256/MIME/dimensions/blob.
One process `RLock` plus `BEGIN IMMEDIATE` serializes compare-and-swap; the row
and image blob commit atomically. There is no custom transaction marker,
filesystem rollback protocol, content-addressed image directory, or orphan
cleanup. SQLite busy/corrupt/I/O failures map to `storage_unavailable`.

Exact layout response (`GET /api/layout`, no query):

```ts
{
  schema: 'mind.dashboard.layout.v1';
  status: 'ready' | 'corrupt';
  error: null | 'corrupt_state';
  revision: number;
  floorplan: null | {
    sha256: string;
    mime: 'image/png' | 'image/jpeg' | 'image/webp';
    width: number;
    height: number;
    url: string;
  };
  positions: Array<{ adva: string; x: number; y: number }>;
}
```

Positions are sorted by AdvA, unique, finite, normalized, and capped at 16.
Fresh state starts at revision zero. A database that cannot be opened or read
returns `503 storage_unavailable`; the bridge never silently overwrites it.

Exact position replacement:

```text
PUT /api/layout
{"schema":"mind.dashboard.layout.update.v1","base_revision":N,"positions":[...]}
```

It returns `200` with the new layout. The array is a complete replacement:
omission unplaces a node and an empty array clears all positions. A stale base
returns `409`:

```ts
{
  schema: 'mind.dashboard.layout.conflict.v1';
  error: 'revision_conflict';
  current: LayoutResponse;
}
```

Every state-changing successful mutation
increments revision by exactly one. After base-revision validation, a canonical
semantic no-op returns `200` with the current state, performs no disk write, and
does not increment revision. No-op cases include an identical complete position
array, removing an absent floorplan, and uploading identical committed bytes,
MIME, and dimensions.

Exact floorplan mutations are separate routes so upload admission occurs before
body read:

```ts
POST /api/floorplan/upload
{ schema:'mind.dashboard.floorplan.upload.v1', base_revision:N,
  mime:'image/png'|'image/jpeg'|'image/webp',
  data_base64:string }

POST /api/floorplan/remove
{ schema:'mind.dashboard.floorplan.remove.v1', base_revision:N }
```

Upload/remove returns the new layout or the same `409` conflict. Image
replacement/removal explicitly retains normalized positions; the UI announces
that mapping. The operator can unplace one node or clear all to admit a new
identity after the 16-position cap.

`PUT /api/layout` has a 65536-byte body cap; floorplan remove has a 256-byte cap;
floorplan upload has a 7340032-byte encoded `Content-Length` cap and 5242880-byte
decoded cap. Base64 is canonical standard alphabet only: no whitespace or data
URL prefix, length divisible by four, at most two trailing `=`, strict decoder
validation, and byte-for-byte re-encoding equality. Malformed canonical form is
`400 invalid_base64`.

Only one upload is admitted before body read; another returns `503 upload_busy`.
PNG/JPEG/WebP dimensions are parsed structurally, must be
static, complete, nonanimated, at most 8192x8192 and 64 million pixels, and MIME
must match magic. SVG, truncation, animation, dimension overflow, and user paths
are rejected.

All non-conflict layout/floorplan errors use exactly:

```ts
{
  schema:'mind.dashboard.error.v1';
  accepted:false;
  error:
    | 'invalid_json' | 'invalid_body' | 'invalid_base64'
    | 'forbidden_request' | 'request_too_large' | 'image_too_large'
    | 'unsupported_media_type' | 'image_type_mismatch'
    | 'invalid_image' | 'animated_image' | 'image_dimensions'
    | 'upload_busy' | 'storage_unavailable' | 'not_found';
}
```

Status mapping is exact: `400` invalid JSON/body/base64; `403` forbidden
Host/origin/fetch metadata; `413` request/image size; `415` media/magic mismatch;
`422` malformed/truncated/animated/dimension image; `404` absent floorplan hash;
`503` upload busy or storage unavailable. Revision conflict uses only the
separate `mind.dashboard.layout.conflict.v1` body above.

`GET /api/floorplan/<sha256>` reads the immutable blob from one SQLite snapshot
and serves exact `Content-Length` in bounded 65536-byte writes, plus exact MIME,
`X-Content-Type-Options: nosniff`, strong quoted ETag, immutable private caching,
`If-None-Match`/`304`, or `404`.

### Floorplan interaction

- Blank canvas is a fixed 16:9 content rectangle. An uploaded image replaces
  that content rectangle at its intrinsic aspect ratio; overlays align to the
  image content, never the surrounding letterbox/container.
- Uploaded images preserve aspect ratio and never carry executable content.
- New GTT identities remain **unpositioned** in a staging roster outside the
  map. No deterministic/default placeholder creates coordinates. Only an
  explicit pointer drop or keyboard placement persists a node and makes it
  eligible for localization.
- Positioned nodes are absolute overlays with normalized coordinates; pointer
  drag uses capture and clamps to the content rectangle.
- Keyboard arrows move a focused node in small normalized steps; Shift uses a larger step.
- Layout is persisted on drag end, not every pointer move. Save state is visible but explanations are moved out of the primary workflow.
- Every node is also represented in a semantic list/table with identity,
  positioned/unpositioned state, normalized coordinates, movement instructions,
  unplace action, and save/error announcements. Incident estimates have an
  equivalent table with state, contributors, geometry warning, spread, and
  normalized coordinates.
- Focus survives successful save, 409 refetch/reapply, rollback, image
  replacement, and responsive reflow.
- Wide and narrow layouts retain comparison and touch targets; reduced motion is respected.

### Localization provider boundary and graph scope

The fire-evac reference under `/home/lucas/AndroidStudioProjects/MyApplication`
confirms the useful seam: a small BLE localization interface consumes positioned
anchors and observations, while graph editing, ARCore fusion, pathfinding, and
persistence remain separate systems. Its admin web edits floor coordinates and
adjacency; Android implements BLE/AR localization.

Adopt that boundary, not its Firebase/ARCore stack:

```ts
interface LocalizationProvider {
  readonly id: string;
  estimate(input: {
    anchors: ReadonlyArray<{ id: string; x: number; y: number }>;
    observations: ReadonlyArray<{ anchorId: string; rssiDbm: number }>;
    coordinateSpace: { aspectRatio: number };
  }):
    | { status:'estimated'; providerId:string; x:number; y:number;
        contributors:ReadonlyArray<string>; geometryWarning:boolean;
        normalizedSpread:number|null }
    | { status:'insufficient'; providerId:string;
        contributors:ReadonlyArray<string>; required:3 }
    | { status:'invalid_input'; providerId:string; reason:string };
}
```

`WeightedCentroidProvider` is the only deployment implementation in this slice.
React, GTT projection, persistence, and event dedup depend only on the interface
and normalized result. A future `LeastSquaresProvider` can therefore replace or
compare the estimator without changing floorplan state, observer identity, or
incident rendering.

The provider is synchronous and pure. It never owns timers, collecting state,
GTT joins, persistence, or React state. Path-loss exponent `n=2.0` is supplied
at provider construction/composition, not through UI state.

Floorplan arrangement **is** required by localization because the manually
placed backbone nodes are the anchor geometry. Full graph adjacency is not:

- GTT supplies current membership/freshness, not parent-child edges.
- No edge is inferred from hop count, RSSI, or visual proximity.
- The persisted v1 layout stores positioned AdvA nodes only.
- A future v2 graph may add explicit edges keyed by the same AdvA IDs and reuse
  the reference project's Konva interaction ideas if evacuation routing becomes
  in scope.
- Firebase, IndexedDB, Room, ARCore transforms, zones, A*, Kalman filtering,
  and Konva remain deferred and add no dependency now.

### Ballpark RSSI estimate

The dashboard keeps the logical dedup key `(wearable, packet_id24)`. “Global”
means all observers/paths in the active bounded process session, not indefinite
packet-ID history. Logical feed/events are capped at 256, FIFO tombstones at
512, processed cursors at 256, observer evidence at 16 per event, and map
markers at 10; overflow/conflict/sample counters saturate deterministically.
Every saturating counter ceiling is `0xffffffff`; increments at the ceiling are
no-ops.

API replay of an already processed cursor changes nothing. A distinct record
from the same observer increments a saturating sample count and retains the
strongest valid RSSI as the deterministic representative; it does not add a
second contributor. Strongest-four ties break by canonical anchor ID.

For each logical incident:

1. Join observer AdvA directly to explicit persisted coordinates. Current GTT
   freshness annotates the event but cannot retroactively invalidate an
   observation-time sample.
2. Exclude unavailable RSSI, unpositioned, or unresolved observers.
3. Sort by strongest RSSI and retain at most four unique observers.
4. Require at least three.
5. With signed dBm `r_i`, strongest `r_max`, and fixed path-loss exponent `n=2.0`, compute stable relative inverse-distance-squared weights:

   `w_i = 10 ^ ((r_i - r_max) / (5 * n))`

6. Return normalized weighted centroid `sum(w_i * p_i) / sum(w_i)`.
7. For finite positive image aspect ratio `a`, every geometry/error/spread test
   uses `d_norm = hypot(a*dx,dy)/hypot(a,1)`, bounded `[0,1]`.
   Normalized doubled triangle area is
   `abs(cross((a*x,y)))/a`, also bounded `[0,1]`; geometry is degenerate when
   every contributor triangle has area <= `1e-6`.
   With four contributors, compute all four leave-one-out three-observer
   estimates; normalized spread is their maximum pairwise `d_norm`. Three
   contributors have `normalizedSpread=null`.
8. Never display meters or a position with fewer than three contributors.

The composition/store starts a two-second **collecting** window at first host
receipt, then asks the provider. Late observer evidence may recompute the
estimate without remounting existing content. UI vocabulary is only
`collecting`, `insufficient`, or `ballpark`; ballpark additionally exposes
contributor count, geometry warning, and normalized spread. Indoor multipath,
body attenuation, orientation, anchor geometry, and first-seen-channel bias are
documented. Calibration and numerical accuracy thresholds are deferred because
no suitable hallway/ground-truth dataset is currently available.

## HCI cleanup

- Visible `ROOT` becomes `GATEWAY`; internal/API names remain unchanged.
- The micro:bit local-root framebuffer becomes a recognizable five-row G, proposed rows: `0x0e, 0x10, 0x17, 0x11, 0x0e`, subject to rendered hardware review.
- Remove redundant explanatory paragraphs and machine-centric metrics from primary cards. Keep short labels, statuses, direct actions, accessible names, and errors with recovery.
- Gateway control is one truthful toggle. Node roster/map prioritizes AdvA identity, freshness, and position; diagnostics remain details-on-demand.

## Execution workflow

Execution must use the required sequence:

1. **Slice A:** parallel non-overlapping firmware RSSI and glyph code-writers -> one Slice A code-checker -> correction writer if needed.
2. **Slice B:** host persistence/API code-writer -> Slice B code-checker -> correction writer if needed.
3. **Slice C:** dashboard gateway/floorplan code-writer -> Slice C code-checker -> correction writer if needed.
4. **Slice D:** localization code-writer -> Slice D code-checker -> correction writer if needed.
5. **Slice E:** clean committed software/resource review -> separately approved
   fleet flash/reset and PC-reboot gate -> persistent HIL evidence commit ->
   final PR-level code-checker -> push/PR update with no post-checker mutation.

Do not launch dependent slices in parallel: B depends on A schemas; C depends on
B APIs; D depends on A/C evidence and map state. Within Slice A, RSSI wire work
and the isolated G glyph may proceed in parallel with disjoint files. After
plan approval, commit this plan first. Each Slice A-D follows writer(s) ->
combined checker -> correction writer/recheck -> reviewed slice commit. E1 runs
from that clean committed source. E2 requires separate approval covering both
fleet hardware actions and the planned reboot. E3 commits evidence/docs, then
the final checker reviews `origin/master...HEAD`; findings loop through scoped
fix/recheck/affected gates. Push once only after the clean final checker.

## Acceptance summary

Software acceptance requires exact wire vectors, legacy decode, allocation/resource closure, durable/revisioned host storage, security matrices, GTT-only roster, accessible floorplan drag, deterministic dedup/estimator tests, rendered wide/narrow HCI, and a clean full-range review.

Hardware acceptance requires explicit user approval and a persistent repository
evidence bundle proving one serial gateway, exact self identity, remote GTT
roster, gateway toggle, floorplan survival across reboot, and zero critical
faults. One incident must have four positioned RSSI contributors to exercise
leave-one-out spread; one must have three contributors with spread unavailable
or a geometry warning; a fewer-than-three composition case must show
`insufficient`. Every case remains one deduplicated logical marker. The first
dataset records normalized error and body-orientation sensitivity but has no
numeric pass threshold; calibration is deferred.

## Open questions

None. Host-disk persistence, weighted centroid, UI-only gateway terminology, and no initial numeric accuracy gate were explicitly selected.
