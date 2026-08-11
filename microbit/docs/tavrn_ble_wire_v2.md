# TAVRN-BLE routed wire-v2 contract

Status: **frozen Phase 0 proof-of-concept contract**. This is an experimental
legacy-advertising transport. It is not Bluetooth Mesh, is not secure, and does
not replace the separately versioned legacy flood wire-v1 codec.

## 1. Inputs and decisions

This contract is grounded in:

- `microbit/app/protocol/README.md` and `tron_mesh_packet.{h,c}` for the proven
  Flags + Manufacturer Specific Data wrapper and little-endian local style;
- `microbit/app/drivers/ble_radio.{h,c}` for the exact raw
  `[S0][Length][AdvA(6)][AdvData]` buffer and FICR address order;
- TAVRN source commit `fc5f25662aa3bb63d65cb24a41461f19ed42ea4e`
  plus the inspected dirty `TAVRN_v2.md` and `tavrn-packet.h` changes;
- TAVRN v2.2's 16-bit sequence/request compression, fixed `k=1` deployment,
  control-message families, and compact metadata; and
- RFC 3561 for AODV field meaning. This BLE profile adds explicit RREP
  correlation because the RFC RREP-ACK intentionally does not carry it.

The Bluetooth Core v6.3 Link Layer specification confirms that legacy
`ADV_NONCONN_IND` carries a six-octet AdvA followed by AdvData, that TxAdd
selects a random address, and that multi-octet Link Layer fields are sent least
significant octet first. The custom PDU rules below are project rules, not a
Bluetooth SIG protocol.

Normative words `MUST`, `MUST NOT`, `SHOULD`, and `MAY` apply to routed
wire-v2. Requirement IDs are stable test handles.

Profile mapping: this document derives representation for `BEARER-01` through
`BEARER-04`, `IDENT-01` through `IDENT-04`, `SERIAL-01` through `SERIAL-05`,
`LINK-01` through `LINK-06`, `AODV-02` through `AODV-07`, `BOOT-01` through
`BOOT-06`, and `META-01` through `META-04`. Profile behavior wins if a derived
representation is later found to conflict.

## 2. Notation and common encodings

- `PDU[n]` is an offset in the custom PDU, not in AdvData.
- AdvData offset for `PDU[n]` is `7 + n`.
- `W` is the compressed identity width: `2` for pre-ESC and `1` for fixed
  `k=1`. The per-type `I` flag selects it (`0 -> W=2`, `1 -> W=1`).
- `SID16` is a two-octet little-endian identity suffix and `SID8` is its low
  octet. Exact derivation and reserved values are in
  `tavrn_ble_identity.md`.
- SID16 is never independently configured: local and direct-peer SID16 values
  are always `AdvA[0] | (AdvA[1] << 8)` from the effective/outer AdvA. Remote
  transit/origin/destination SID16 values remain standalone logical keys.
- `AdvA6` is six bytes in radio-buffer/on-air order, least-significant octet
  first. It is copied byte-for-byte; it is not byte-swapped.
- Every other multi-byte integer is unsigned little-endian (`u16le`).
- `ttl_hops` packs remaining TTL in bits 7..4 and traversed hops in bits 3..0.
  Both are `0..15`. An origin emits `hops=0`. A relay processes locally first;
  it relays only when TTL is nonzero, then emits `ttl-1` and `hops+1`. A frame
  at `hops=15` is never relayed.
- `ROUTED_NET_DIAMETER = ROUTED_TTL_MAX = 15`. Full-diameter RREQ, RERR,
  generic FLOOD, and TC_UPDATE originate with TTL 15. No routed-v2 path,
  configured diameter, or emitted TTL may exceed 15. Expanding-ring RREQ TTLs
  are exactly `1,3,5,7,15`.
- Sequence, request, and flood counters are 16-bit. Equality-only dedupe and
  serial freshness are separate operations. A dedupe lookup compares the
  complete key for equality; a different key is new regardless of any numeric
  half-range relation. Only a freshness comparison within the same subject and
  serial stream uses `a` newer than `b` iff `(uint16_t)(a-b)` is in
  `1..0x7fff`; `0x8000` is unordered and rejected only for that freshness
  comparison.
- `BCAST16 = ff ff`; `BCAST8 = ff`. Broadcast is never a valid unicast next
  hop or origin.

### 2.1 Compact topology metadata

When a type's `M` bit is set, the extension is:

| Relative offset | Size | Field |
| --- | ---: | --- |
| `0` | 1 | `meta_count`, `1..type_capacity` |
| `1 + 2*i` | 1 | entry `SID8` |
| `2 + 2*i` | 1 | `ttl_flags` |

`ttl_flags[7:4]` is `min(15, floor(ttl_remaining_seconds / 20))`.
`ttl_flags[0]` is `freshness_request`; bit 1 is `departed`; bits 3..2 MUST be
zero. Bucket zero is valid for an active entry with less than 20 seconds left;
the departed bit disambiguates it.

Metadata is legal only with `I=1` and only in E_RREQ, E_RREP, E_RERR, or the
two targeted-HELLO exceptions below. There is no AM byte because this profile
has exactly two modes and metadata exists only after fixed `k=1` activation.
Unknown or reserved SID8 values make the entire frame malformed. Metadata is
placed where each type below states; no receiver may scan for it heuristically.

On E_RREQ/E_RREP/E_RERR, `freshness_request=1` is a soft social query, not a
targeted request and not permission to originate a HELLO. A request-clear entry
on a later otherwise eligible metadata-bearing control is its social answer. The
subject itself is tier 1 and is selected first; an intermediary is tier 2 only
when its active, non-departed encoded remaining-TTL bucket is strictly greater
than twice the received request bucket. Tier 3 sources remain silent. This uses
the profile's fixed metadata-candidate store and starts no standalone control.

The one four-slot candidate pool is shared with delayed targeted tier-2
reservations. General classes select soft requests, subject-self answers,
eligible intermediary answers, then ordinary active dissemination, round-robin
within each class. A soft expiry creates only a request candidate; an admitted
request may create a request-clear subject-self or strictly-fresher intermediary
answer, and an admitted answer atomically merges and clears its resolved request.
There is no recursive answer, standalone metadata control, or general departed
entry. Selection and encoding are non-consuming. Only containing-control enqueue
admission starts cooldown, releases the selected entry, and advances its cursor;
BUSY, zero-channel failure, and local-not-attempted preserve exact work. A newer
semantic state invalidates obsolete cooldown. Every general claim is attributed
to immediate outer AdvA; the targeted response's preserved-full-origin rule is
the sole exception. A delayed targeted reservation is capacity-only: it remains
live while general RREQ/RREP/RERR selects only general entries and it may never
be serialized as a general metadata entry.

## 3. Full legacy advertising wrapper

**WIRE-V2-001:** An encoder emits exactly the following two AdvData AD
structures and no padding or third structure:

| AdvData offset | Size | Value / meaning |
| ---: | ---: | --- |
| 0 | 1 | `02`, Flags AD length |
| 1 | 1 | `01`, Flags AD type |
| 2 | 1 | `06`, Flags value |
| 3 | 1 | `3 + pdu_len`, range `08..1b` |
| 4 | 1 | `ff`, Manufacturer Specific Data AD type |
| 5 | 2 | `ff ff`, company ID `0xffff` little-endian |
| 7 | `pdu_len` | routed custom PDU, `5..24` bytes |

Therefore:

```text
adv_len = 7 + pdu_len <= 31
raw_length_field = 6 + adv_len <= 37
raw_buffer_len = 2 + raw_length_field <= 39
```

The raw nRF RADIO buffer is:

| Raw offset | Size | Value / meaning |
| ---: | ---: | --- |
| 0 | 1 | `42`: `ADV_NONCONN_IND (02)` plus `TxAdd=random (40)` |
| 1 | 1 | `6 + adv_len` |
| 2 | 6 | transmitter's canonical AdvA |
| 8 | `adv_len` | AdvData above |

The raw AdvA is the **immediate transmitter** for every frame. It is outside
AdvData but is mandatory decoder input. An RX API that preserves only AdvData
is insufficient for routed-v2.

Wire bytes are independent of radio completion evidence. Every mesh radio
operation used around them--init, idle/disable, listen, RX restore, snapshot,
and TX--runs through a bounded typed seam; state waits use
`timer.radio_state_timeout_ms` and multi-channel TX uses
`timer.radio_tx_event_bound_ms`.
A failed/timeout snapshot provides no partial AdvA or AdvData to this decoder;
a failed restore remains typed radio evidence and cannot turn malformed or
partial storage into an RX frame.

For a multi-channel advertising TX event, scheduler evidence carries the
requested/selected channel mask, completed channel mask, and any typed fault.
Zero completed selected channels is TX_FAILED. One or more completed selected
channels is TX_DONE even when a later selected channel or restore operation
times out, because a receiver could have observed a completed channel. This
out-of-band mask/fault evidence changes no AdvData/PDU byte or length.

### 3.1 Common routed prefix

| PDU offset | Size | Value / meaning |
| ---: | ---: | --- |
| 0 | 1 | `54`, ASCII `T` |
| 1 | 1 | `52`, ASCII `R` (routed family) |
| 2 | 1 | `02`, wire version |
| 3 | 1 | network ID, configured `01..fe` |
| 4 | 1 | type |

Network `00` is unconfigured and `ff` is reserved. They MUST NOT be sent.
The build default is `01`; every artifact records its effective value. Golden
vectors deliberately use `2a` so tests do not accidentally hard-code the
default.
The tuple `{54 52, version 02, expected network, known type}` isolates routed
wire-v2. `{54 4d, version 01}` remains legacy flood wire-v1 and is dispatched
to its existing decoder, never reinterpreted as routed-v2. A seven-byte MIND
patient MSD has no `54 52` prefix and remains a separate classifier result.

| Type | Value | Name |
| --- | ---: | --- |
| `E_RREQ` | `01` | AODV route request |
| `E_RREP` | `02` | AODV route reply |
| `E_RERR` | `03` | AODV route error |
| `HELLO` | `04` | full-identity topology hello |
| `SYNC_OFFER` | `05` | mentorship offer |
| `SYNC_PULL` | `06` | one-entry page request |
| `SYNC_DATA` | `07` | zero/one-entry page |
| `TC_UPDATE` | `08` | full-identity join/leave flood |
| `E_RREP_ACK` | `09` | AODV route-symmetry acknowledgment |
| `DATA` | `10` | routed application data |
| `HACK` | `11` | hop-custody acknowledgment |
| `FLOOD` | `12` | controlled generic flood |

Values not listed are unsupported and dropped before state mutation.

## 4. Link frames

### 4.1 DATA (`type=10`)

Flags at PDU[5]: bit 7 `I`; bit 5 `U` (urgent application traffic); all other
bits zero. Local repair reuses the identical DATA format and adds no wire flag.

| PDU offset | Size | Field |
| ---: | ---: | --- |
| 0 | 5 | common prefix |
| 5 | 1 | flags |
| 6 | 1 | `ttl_hops` |
| 7 | `W` | immediate receiver / next hop |
| `7+W` | `W` | route origin |
| `7+2W` | `W` | final destination |
| `7+3W` | 2 | `data_seq` |
| `9+3W` | 1 | `app_kind` |
| `10+3W` | 1 | `app_source` |
| `11+3W` | variable | application bytes |

There is no payload-length byte: the validated type length determines the
remaining application length.

`TAVRN_LINK_DATA_PAYLOAD_MAX = 10` is the public candidate/custody/failure
storage capacity because legal DATA8 carries ten application bytes. Every
public link DATA buffer and length guard MUST support `0..10`; seven is not a
public storage capacity.

- `app_kind=01` is MIND patient schema v1. `app_source` is the original
  wearable `DEVICE_ID` (`01..fe`) extracted from patient `AdvA[4]` under the
  authoritative `MIND_ADVA(id)` layout, and the
  application bytes MUST be exactly the unchanged seven bytes from
  `mind_adv_payload_t`: schema, event, confidence, `accel_svm` LE, mic, seq.
- `app_kind=7f` is test-only opaque data. `app_source=00`; its application
  length may be zero through the remaining type capacity.
- Other app kinds are unsupported at the final destination. Relays preserve
  them byte-for-byte but do not claim custody if local policy forbids them.

Pre-ESC patient DATA consumes the full PDU: `17 + 7 = 24`. Fixed-k patient DATA
is 21 bytes (`14 + 7`) but its three spare bytes remain unused: this profile
assigns DATA zero topology-metadata slots so repair on/off and AODV/FULL share
the same clean DATA representation. Patient bytes are never truncated,
rewritten, or displaced by metadata. The exact seven-byte rule is an
`app_kind=01` schema validation rule only; it does not resize the ten-byte
public link storage.

**DATA dedupe key:**
`{network, active-width route-origin logical ID, data_seq, app_kind,
app_source}`. In SID16 mode, the route-origin SID16 is the complete logical
dedupe identity; remote full-AdvA resolution is not required. In SID8 mode,
the SID8 MUST resolve uniquely through FULL_TAVRN context.
The immediate transmitter, next hop, TTL, and hops are not part of the key.
Custody behavior is frozen in
`tavrn_ble_ack_contract.md`.

### 4.2 HACK (`type=11`)

Flags at PDU[5]: bit 7 `I`; bits 6..0 zero.

| PDU offset | Size | Field |
| ---: | ---: | --- |
| 0 | 5 | common prefix |
| 5 | 1 | flags |
| 6 | `W` | immediate receiver (the prior DATA transmitter) |
| `6+W` | `W` | acknowledged DATA origin |
| `6+2W` | `W` | acknowledged DATA final destination |
| `6+3W` | 2 | acknowledged `data_seq` |
| `8+3W` | 1 | acknowledged `app_kind` |
| `9+3W` | 1 | acknowledged `app_source` |
| `10+3W` | 1 | status: accepted/duplicate/busy/rejected |

Length is 17 bytes for SID16 and 14 for SID8. HACK is direct, has implicit
TTL=1/hops=0, is never relayed, and is never itself acknowledged.

### 4.3 Generic controlled FLOOD (`type=12`)

Flags at PDU[5]: bit 7 `I`; bit 6 `U`; bits 5..0 zero.

| PDU offset | Size | Field |
| ---: | ---: | --- |
| 0 | 5 | common prefix |
| 5 | 1 | flags |
| 6 | 1 | `ttl_hops` |
| 7 | `W` | flood origin |
| `7+W` | 2 | `flood_seq` |
| `9+W` | 1 | class: `01` diagnostic, `02` application control |
| `10+W` | 1 | body length `L` |
| `11+W` | `L` | body |

`L<=11` for SID16 and `L<=12` for SID8. Route and TAVRN control messages MUST
use their own types, not an opaque FLOOD body. Immediate receiver is implicit
broadcast. Dedupe key is
`{network, type, active-width origin logical ID, flood_seq}`.

FLOOD, E_RREQ, E_RERR, and TC_UPDATE use the controlled-flood admission
discipline: validate first; refresh direct transmitter evidence; dedupe; process
locally once; and, if permitted by the type and TTL, enqueue one jittered relay
preserving origin, correlation, and body while changing only TTL/hops and
type-permitted hop-local metadata. Ordinary HELLO is direct one-hop control, not
a controlled flood; targeted HELLO is forwarded only along its selected valid
route.

## 5. AODV control frames

### 5.1 E_RREQ (`type=01`)

Flags: bit 7 `I`; bit 6 `G` gratuitous RREP; bit 5 `D` destination only; bit
4 `U` destination sequence unknown; bit 3 `M`; bits 2..0 zero.

| PDU offset | Size | Field |
| ---: | ---: | --- |
| 0 | 5 | common prefix |
| 5 | 1 | flags |
| 6 | 1 | `ttl_hops` |
| 7 | `W` | RREQ origin |
| `7+W` | 2 | `request_id` |
| `9+W` | `W` | route destination |
| `9+2W` | 2 | destination sequence |
| `11+2W` | 2 | origin sequence |
| `13+2W` | variable | optional metadata |

Immediate receiver is implicit broadcast. `U=1` requires destination sequence
zero. The request dedupe and reverse-route key is
`{network, active-width origin logical ID, request_id}`.

Every locally emitted expanding-ring transmission, including the next ring or
a retry at full diameter, allocates a **fresh request ID**. The local route
engine groups those request IDs under an off-wire `discovery_generation`; that
generation is never serialized. A relay preserves one received request ID and
relays that key at most once. A RREP echoes the particular request ID that
created its reverse path; the origin accepts it when that ID is still mapped to
the pending discovery generation.

Base length is 17 (SID16) or 15 (SID8). SID8 has capacity for four metadata
entries (`15 + 1 + 4*2 = 24`). SID16 does not permit metadata.

### 5.2 E_RREP (`type=02`)

Flags: bit 7 `I`; bit 6 `A` E_RREP_ACK required; bit 3 `M`; all others zero.

| PDU offset | Size | Field |
| ---: | ---: | --- |
| 0 | 5 | common prefix |
| 5 | 1 | flags |
| 6 | 1 | `ttl_hops` |
| 7 | `W` | immediate receiver / reverse-route next hop |
| `7+W` | `W` | route destination (RREP generator's advertised destination) |
| `7+2W` | 2 | destination sequence |
| `9+2W` | `W` | original RREQ origin |
| `9+3W` | 2 | correlated `request_id` |
| `11+3W` | 2 | encoded route lifetime |
| `13+3W` | variable | optional metadata |

Lifetime encoding is deterministic: `0..16383 ms` is encoded directly with bit
15 clear; larger values encode `0x8000 | min(0x7fff, floor(ms/100))`.
Codes `0x4000..0x7fff` are malformed. The decode of a scaled value is the low
15 bits times 100 ms.

The RREP semantic/correlation key is
`{destination, destination_sequence, RREQ_origin, request_id}`. Base length is
19 (SID16) or 16 (SID8). SID8 has capacity for three metadata entries
(`16 + 1 + 3*2 = 23`); the final byte remains unused rather than inventing a
field.

### 5.3 E_RERR (`type=03`)

Flags: bit 7 `I`; bit 6 `N` no-delete; bit 5 `A` compressed-ID ambiguity; bit
4 `M`; bits 3..0 zero.

| PDU offset | Size | Field |
| ---: | ---: | --- |
| 0 | 5 | common prefix |
| 5 | 1 | flags |
| 6 | 1 | `ttl_hops` |
| 7 | `W` | reporter/origin |
| `7+W` | 2 | `rerr_seq` |
| `9+W` | 1 | unreachable destination count `D` |
| `10+W` | `D*(W+2)` | ordered `{destination, destination_sequence}` entries |
| after entries | variable | optional metadata |

`D` is `1..3` for SID16 and `1..4` for SID8. Entries MUST be ascending by
unsigned active-width destination value; duplicates are malformed. This order
does not require remote SID16 full resolution. SID16 has no metadata. SID8
metadata capacities are:

| Unreachable `D` | No-metadata PDU | Max metadata entries | Largest PDU with metadata |
| ---: | ---: | ---: | ---: |
| 1 | 14 | 4 | 23 |
| 2 | 17 | 3 | 24 |
| 3 | 20 | 1 | 23 |
| 4 | 23 | 0 | not legal with `M=1` |

Immediate receiver is implicit controlled broadcast. Dedupe key is
`{network, active-width reporter logical ID, rerr_seq}`. `A=1` reports an
ambiguity but does not authorize dynamic `k`; affected compressed identities
fail closed as specified by the identity contract.

If one RERR action contains more unreachable destinations than fit, the
originator first coalesces duplicate destinations using the freshest
non-ambiguous destination sequence, sorts the remaining entries by unsigned
active-width destination value, and emits contiguous chunks of at most three
SID16 or four SID8 entries. Every fragment allocates a fresh `rerr_seq`; relays
preserve it and MUST NOT resegment. Fragment origination obeys
`timer.aodv_rerr_rate` (10/s in BALANCED) and the profile/manifest fixed
action-queue bound.
Unschedulable remaining chunks stay as one pending action with explicit
overflow/expiry evidence; they are never silently truncated or assigned the
same sequence.

### 5.4 E_RREP_ACK (`type=09`)

Flags: bit 7 `I`; bits 6..0 zero.

| PDU offset | Size | Field |
| ---: | ---: | --- |
| 0 | 5 | common prefix |
| 5 | 1 | flags |
| 6 | `W` | immediate receiver (the E_RREP transmitter) |
| `6+W` | `W` | acknowledged route destination |
| `6+2W` | 2 | acknowledged destination sequence |
| `8+2W` | `W` | acknowledged RREQ origin |
| `8+3W` | 2 | acknowledged `request_id` |

Length is 16 (SID16) or 13 (SID8). The exact pending key is
`{network, expected outer AdvA, receiver, destination, destination_sequence,
RREQ_origin, request_id, identity_width}`. It is direct, one-hop, non-relayed,
and non-HACKable. Its exact correlation is a deliberate BLE profile extension
over RFC 3561's reserved-only payload. Its route-symmetry meaning is distinct
from DATA custody; see the ACK contract.

## 6. TAVRN control frames

### 6.1 HELLO (`type=04`)

Flags: bit 7 `I`; bit 6 `N` new/bootstrap; bit 5 `T` targeted; bit 4 `Q`
freshness request; bit 3 `M`; bits 2..0 zero.

| PDU offset | Size | Field |
| ---: | ---: | --- |
| 0 | 5 | common prefix |
| 5 | 1 | flags |
| 6 | 1 | `ttl_hops` |
| 7 | `W` | immediate receiver, or broadcast |
| `7+W` | `W` | final target, or broadcast |
| `7+2W` | 6 | origin's full canonical AdvA |
| `13+2W` | 2 | `boot_nonce` when `N=1`; node sequence when `N=0` |
| `15+2W` | variable | optional metadata |

An ordinary HELLO is exactly `N=0,T=0,Q=0,M=0`, with broadcast receiver/final
target, TTL=1/hops=0, and full origin AdvA equal to outer AdvA. It is direct
one-hop control: it is never relayed, never answered, and refreshes
direct-neighbor evidence only. A bootstrap HELLO is `N=1,T=0,Q=0,M=0`, SID16,
direct one-hop/non-relayed, with a nonzero boot nonce, no metadata, and the same
full-origin/outer-AdvA rule. It may elicit a separate mentorship `SYNC_OFFER`,
which is not a HELLO reply. `{full origin AdvA,boot_nonce}` is routed-common
incarnation admission and is handled before ordinary HELLO dedupe: repeats of
the same pair are idempotent; a different nonce triggers a new incarnation. That
direct N=1 also derives SID16 from outer `AdvA[0..1]` and installs `{SID16,AdvA}`
without adding a SID16 HELLO field.

Targeted HELLO is fixed-k SID8, `N=0`, and has unicast immediate receiver and
final target. It is only the `MAINT-06` stage-0 demanded hard-expiry exchange;
soft social metadata never originates this form. It is implemented only in the
FULL runtime. Retained-hop/full RREQ verification is also implemented but reuses
the unchanged `E_RREQ` shape; DEV-027 implements general metadata on eligible
SID8 RREQ/RREP/RERR controls and confirmed TC JOIN/LEAVE. After establishment, ordinary
`N=0` HELLO and every newly originated targeted request/response share one local
HELLO node-sequence stream. A semantic targeted transaction/context reserves one
current reservation-frontier value at creation, then advances that frontier modulo
65536. BUSY or zero-channel `TX_FAILED` retries retain exact bytes and the reserved sequence;
relays preserve the request/response origin sequence; four concurrent contexts
reserve distinct values; and gaps after canceled unsent work are legal. A
pending ordinary HELLO retains its exact visible node sequence until queue
admission; targeted reservations use the internal frontier after that pending
value without changing pending bytes or the visible cursor. Ordinary admission
then merges the visible cursor to that frontier while skipping active targeted
reservations. The exact shared-stream ordering and nonreuse rules are in
`SERIAL-01` and the identity contract. A targeted origin uses the valid route's
positive hop count as its initial TTL and `hops=0`; a newly originated targeted
request or response must have full origin
equal to outer AdvA. Every relay changes only immediate receiver and TTL/hops
while preserving full origin and node sequence. A targeted control never starts
route discovery.

The implemented continuation reuses the existing `E_RREQ` wire shape unchanged:
Stage-1/Stage-2 purpose is retained only with the local maintenance context and
the copied AODV action, never serialized into a new flag or metadata record. The
future action must retain `{purpose, request ID, token, subject}` until the
router/link reports its physical completion. A test may synthesize only that
completion callback fact; it must not fabricate a lifecycle/telemetry record.
Wrong purpose, token, or request ID, a zero completion mask, and a local
non-attempt are non-consuming. The frozen RED contract for this unchanged wire
continuation remains `run_tavrn_phase5_rreq_verification_tests.sh --red`;
production verification is accepted by the corresponding `--green` runner.

- A targeted freshness request is exactly `I=1,N=0,T=1,Q=1,M=1` (`0xb8`). Its
  full origin is the requester, its final target is the subject, and it has
  exactly one metadata entry naming that subject with remaining-TTL bucket zero,
  `freshness_request=1`, and `departed=0`; its immediate receiver is the first
  next hop of the valid route to that subject.
- A targeted freshness response is exactly `I=1,N=0,T=1,Q=0,M=1` (`0xa8`). It
  is distinct control, never an ordinary HELLO reply. Its full origin is the
  evidence source, its final target is the SID8 derived from the original
  requester's full origin, its immediate receiver is the first next hop of the
  valid return route, and it has
  exactly one metadata entry for the requested subject with
  `freshness_request=0` and `departed=0`. The target subject responds immediately
  when it has a valid return route to the requester and encodes its fresh local
  self evidence as `min(15, floor(timer.gtt_hard_expiry_ms / 20000))`; this is
  deliberately not a countdown of a target-local remaining deadline. A
  non-target intermediary is eligible only with active, non-departed evidence
  whose encoded actual remaining-hard-lifetime bucket is strictly greater than
  twice the request bucket. Because this targeted request is normatively bucket
  zero, the targeted boundary is exactly response bucket greater than zero:
  active bucket zero remains silent and bucket one is eligible. The general
  twice-the-request-bucket rule remains material for non-targeted social
  metadata. An eligible intermediary with a free candidate slot MUST schedule
  exactly one response. Its delay is deterministic FNV-1a 32-bit:
  initialize `2166136261`, then XOR/multiply modulo `2^32` by prime `16777619`
  for the network byte, requester full AdvA bytes `0..5`, request sequence
  little-endian bytes, final-target SID8, and subject SID8, in that exact order.
  Map the result to the inclusive response interval as
  `min_ms + hash % (max_ms - min_ms + 1)`. After normal structural, receiver,
  identity, and dedupe admission, a same requester/subject response suppresses
  only matching delayed-not-yet-enqueued intermediary work. Queued or in-flight
  work is not retractable through suppression. A tier-3 or stale source remains
  silent.

For an admitted targeted freshness response, the preserved full origin is the
evidence source for its one subject claim; outer AdvA is direct evidence only for
the immediate relay. This is the targeted-response exception to ordinary metadata
attribution, not permission to use a relay as the subject evidence source.

For either targeted form, missing selected route, local BUSY, capacity denial, or
zero-channel TX failure sends no response immediately, starts no discovery, and
proves no departure. A retained request relay, response relay, immediate target
response, delayed intermediary response, or initiator pending request retries
its exact work only on owner ticks while its selected route exists. Its
`obligation_started_ms` is the owner tick that creates/adopts the locally
initiated stage-0 context after current hard-expiry/demand/identity validation;
the post-dedupe normal request RX-admission time for a request relay, immediate
target response, or delayed intermediary response; or the post-dedupe normal
response RX-admission time for a response relay that need not have observed the
request. The wrap-safe equality deadline is
`obligation_started_ms + timer.hello_dedupe_ms`; BUSY before first scheduler
enqueue is within that bound, and the delayed response's deterministic wait
consumes part of it. Route loss or deadline expiry drops work without discovery
or departure. Stage 0 has no independent rate limiter or timer key;
`timer.aodv_rreq_rate` applies only to later RREQ stages.
A response with `Q=0` is incapable of recursively eliciting another response.
Base length is 19 (SID16) or 17 (SID8). HELLO has zero general metadata slots; the
only exceptions are the two 20-byte (`17 + 1 + 2`) targeted forms above. Any other
HELLO metadata count or flag combination is malformed. Ordinary `N=0` dedupe key
is `{network, full origin AdvA, node_sequence, T=0}`; targeted request and
response keys are respectively `{network, full origin AdvA, node_sequence,
final_target, subject, Q=1}` and `{network, full origin AdvA, node_sequence,
final_target, subject, Q=0}`. N=1 uses the incarnation pair above, not the
ordinary HELLO dedupe/high-water path.

### 6.2 SYNC_OFFER (`type=05`)

SYNC_OFFER is exactly 24 bytes:

| PDU offset | Size | Field |
| ---: | ---: | --- |
| 0 | 5 | common prefix |
| 5 | 1 | flags, MUST be zero |
| 6 | 1 | `ttl_hops`, MUST be `10` (one-hop offer) |
| 7 | 6 | mentor full AdvA, MUST equal outer AdvA |
| 13 | 6 | target mentee full AdvA |
| 19 | 1 | frozen snapshot entry count, `0..16` |
| 20 | 2 | `snapshot_id` |
| 22 | 2 | nonzero mentee `boot_nonce` being answered |

The physical receiver is broadcast; only the exact mentee collects the offer.
Other nodes may use the full target identity for offer dampening. The mentor
freezes an immutable snapshot of at most the profile's 16 entries, sorted by
canonical AdvA, before sending the offer. Dedupe key is
`{mentor AdvA, mentee AdvA, boot_nonce, snapshot_id}`. A zero or mismatched
nonce is malformed for that bootstrap session. No metadata fits.

### 6.3 SYNC_PULL (`type=06`)

SYNC_PULL is exactly 22 bytes:

| PDU offset | Size | Field |
| ---: | ---: | --- |
| 0 | 5 | common prefix |
| 5 | 1 | flags, MUST be zero |
| 6 | 6 | mentee full AdvA, MUST equal outer AdvA |
| 12 | 6 | selected mentor full AdvA (immediate/final receiver) |
| 18 | 2 | `snapshot_id` |
| 20 | 1 | zero-based page/index |
| 21 | 1 | requested count, MUST be `01` |

Correlation is exactly `{mentor AdvA, mentee AdvA, snapshot_id, index}`.
For a nonempty snapshot, index MUST be below the offered count. Retransmission
repeats the same tuple. SYNC_PULL is not HACKable: the bootstrap FSM waits
`timer.mentor_page_timeout_ms` and permits `timer.mentor_page_attempts` for the
same tuple, then clears the mentor and restarts full-identity HELLO (BALANCED:
2400 ms and three attempts). No metadata fits.

### 6.4 SYNC_DATA (`type=07`)

Flags: bit 7 `P` entry present; bit 6 `L` last page; bits 5..0 zero.

| PDU offset | Size | Field |
| ---: | ---: | --- |
| 0 | 5 | common prefix |
| 5 | 1 | flags |
| 6 | 6 | target mentee full AdvA |
| 12 | 2 | `snapshot_id` |
| 14 | 1 | page/index |
| 15 | 6 | entry full AdvA, only if `P=1` |
| 21 | 2 | entry sequence, only if `P=1` |
| 23 | 1 | entry `ttl_state_hops`, only if `P=1` |

The outer AdvA is the selected mentor/immediate transmitter. An empty snapshot
is a 15-byte frame with `P=0,L=1,index=0`. Otherwise `P=1`, length is exactly
24, and `L=1` only for the final offered index. For SYNC_DATA only,
`ttl_state_hops[7:4]` is zero for departed or
`min(15,max(1,ceil(active_ttl_seconds/20)))` for an active entry;
`ttl_state_hops[3:0]` is the mentor's hop estimate `0..15`. The mentee stores
`min(15, mentor_hop + 1)`.
The one-entry page capacity is deliberate: full identity is mandatory during
bootstrap. `lastSeen` is not transferred: on merge, the receiver uses receipt
time as local last evidence and the bucket as remaining lifetime. Entries merge
idempotently by the correlation tuple; a duplicate page must not append a
second entry. No metadata fits.

### 6.5 TC_UPDATE (`type=08`)

TC_UPDATE is exactly 24 bytes and always uses full identities:

| PDU offset | Size | Field |
| ---: | ---: | --- |
| 0 | 5 | common prefix |
| 5 | 1 | flags, MUST be zero |
| 6 | 1 | `ttl_hops` |
| 7 | 6 | full origin AdvA |
| 13 | 2 | origin `tc_seq` |
| 15 | 6 | full subject AdvA |
| 21 | 1 | event: `00` join, `01` leave |
| 22 | 2 | advisory origin uptime seconds modulo 65536 |

Immediate receiver is implicit controlled broadcast; outer AdvA identifies
the current transmitter and may differ from origin after relay. Dedupe UUID is
`{network, full origin AdvA, tc_seq}`. Subject suppression key is
`{full subject AdvA, event}`. Timestamp is never a UUID or freshness authority.
Full subject identity permits collision detection for a newly joined node.
UUID and subject retention use `timer.tc_uuid_ms` and `timer.tc_subject_ms`;
they are not wire literals (BALANCED: 30 s and 1 s). There is no room for
metadata; none may be appended.

`ttl_hops` is origin `15/0`; a relay validates the complete frame before any
dedupe mutation. A retained UUID ignores every later copy, including a changed
event byte. For a fresh UUID, retain UUID first, then test subject/event: a live
matching subject/event suppresses local apply and relay but keeps the fresh UUID;
a fresh subject/event applies locally once and then may relay. JOIN and LEAVE are
different subject/event keys. TTL zero applies locally but does not forward. For
nonzero TTL a fresh applicable event preserves bytes 7..23 other than
`ttl_hops`, emits TTL-1/hops+1, and its own outer AdvA is the next receiver's
immediate transmitter. Relay BUSY preserves exact work. Overhearing an already
retained UUID is duplicate/implicit gossip ACK: TC has no HACK or explicit
acknowledgment. `uptime` is advisory and must not affect UUID, dedupe, or
freshness. The Phase-4 mentorship self-JOIN keeps its existing enqueue-tied
local GTT/dedupe compatibility exception; general TC maintenance must not
rewrite that commit point.

## 7. Identity-role matrix

**WIRE-V2-020:** A decoder and tests MUST distinguish these roles rather than
using one ambiguous `src` field.

| Type | Immediate transmitter | Immediate receiver | Origin / correlation owner | Final target / subject |
| --- | --- | --- | --- | --- |
| DATA | outer AdvA | `next_hop` | `origin` + `data_seq` | `final_destination` |
| HACK | outer AdvA | `receiver` | acknowledged DATA origin tuple | acknowledged DATA destination |
| FLOOD | outer AdvA | implicit broadcast | `origin` + `flood_seq` | none; class/body |
| E_RREQ | outer AdvA | implicit broadcast | RREQ origin + request ID | route destination |
| E_RREP | outer AdvA | `next_hop` | route destination/RREQ tuple | RREQ origin |
| E_RERR | outer AdvA | implicit broadcast | reporter + RERR sequence | unreachable entries |
| E_RREP_ACK | outer AdvA | `receiver` | acknowledged RREP tuple including destination sequence | RREQ origin |
| HELLO | outer AdvA | receiver or broadcast | ordinary: full direct node AdvA + boot nonce/node sequence; targeted: requester/evidence-source full AdvA + its node sequence | ordinary: broadcast; targeted request: subject; targeted response: requester |
| SYNC_OFFER | outer/mentor AdvA | physical broadcast, logical mentee | mentor + snapshot | mentee |
| SYNC_PULL | outer/mentee AdvA | mentor full AdvA | mentee + snapshot/index | mentor |
| SYNC_DATA | outer/mentor AdvA | mentee full AdvA | mentor + snapshot/index | mentee / page entry |
| TC_UPDATE | outer AdvA | implicit broadcast | full origin + TC sequence | full subject |

## 8. Size proof and compile-time guards

`adv_len` below already includes all seven wrapper bytes.

| Type / mode | Base or exact PDU | Variable **wire** capacity | Largest legal PDU | Largest AdvData |
| --- | ---: | --- | ---: | ---: |
| DATA SID16 | 17 | app `0..7`; patient exactly 7; no metadata | 24 | 31 |
| DATA SID8 | 14 | app `0..10`; patient exactly 7; no metadata | 24 | 31 |
| HACK SID16 / SID8 | 17 / 14 | none | 17 / 14 | 24 / 21 |
| FLOOD SID16 | 13 | body `0..11` | 24 | 31 |
| FLOOD SID8 | 12 | body `0..12` | 24 | 31 |
| E_RREQ SID16 | 17 | no metadata | 17 | 24 |
| E_RREQ SID8 | 15 | metadata `0..4` | 24 | 31 |
| E_RREP SID16 | 19 | no metadata | 19 | 26 |
| E_RREP SID8 | 16 | metadata `0..3` | 23 | 30 |
| E_RERR SID16 | 12 + 4D | `D=1..3` | 24 | 31 |
| E_RERR SID8 | 11 + 3D | `D=1..4`, conditional metadata table above | 24 | 31 |
| E_RREP_ACK SID16 / SID8 | 16 / 13 | none | 16 / 13 | 23 / 20 |
| HELLO SID16 | 19 | no metadata | 19 | 26 |
| HELLO SID8 | 17 | targeted freshness request or response metadata exactly 1 | 20 | 27 |
| SYNC_OFFER | exact 24 | zero entries inline; snapshot count only | 24 | 31 |
| SYNC_PULL | exact 22 | requested count fixed at 1 | 22 | 29 |
| SYNC_DATA | 15 empty / 24 present | zero or one full entry | 24 | 31 |
| TC_UPDATE | exact 24 | no metadata | 24 | 31 |

The future codec MUST express offsets and lengths as integer constants and
include at least these guards (names may be prefixed, arithmetic may not
change):

```c
_Static_assert(PDU_MAX == 24u, "legacy custom PDU budget");
_Static_assert(ADV_OVERHEAD + PDU_MAX == 31u, "legacy AdvData budget");
_Static_assert(DATA16_BASE + 7u == PDU_MAX, "patient DATA16 fits exactly");
_Static_assert(DATA8_BASE + 7u == 21u,
               "patient DATA8 remains clean and leaves three bytes unused");
_Static_assert(TAVRN_LINK_DATA_PAYLOAD_MAX == 10u,
               "public link storage covers maximum DATA8 application bytes");
_Static_assert(DATA8_BASE + TAVRN_LINK_DATA_PAYLOAD_MAX == PDU_MAX,
               "maximum DATA8 consumes the PDU budget");
_Static_assert(RREQ8_BASE + META_HEADER + 4u * META_ENTRY == PDU_MAX,
               "RREQ metadata capacity");
_Static_assert(RERR16_BASE + 3u * RERR16_ENTRY == PDU_MAX,
               "RERR16 capacity");
_Static_assert(RREP_ACK16_LEN == 16u && RREP_ACK8_LEN == 13u,
               "RREP ACK includes destination sequence correlation");
_Static_assert(HELLO8_BASE + META_HEADER + META_ENTRY == 20u,
                "targeted HELLO carries exactly one freshness entry");
_Static_assert(SYNC_DATA_BASE + SYNC_DATA_ENTRY == PDU_MAX,
               "one full bootstrap entry");
_Static_assert(SYNC_OFFER_LEN == PDU_MAX, "full bootstrap identities");
_Static_assert(TC_UPDATE_LEN == PDU_MAX, "full topology identities");
```

Encoders MUST fail before calling the radio if any computed PDU exceeds 24 or
AdvData exceeds 31. They MUST NOT rely on `ble_radio_advertise()` truncation.

## 9. Malformed, admission, and dedupe policy

**WIRE-V2-030:** Processing order is fixed:

1. snapshot the raw PDU, including outer AdvA;
2. validate raw advertising type/length and canonical AdvA;
3. validate the exact two-AD wrapper and all length equations;
4. classify magic/version/network/type;
5. validate exact type length, reserved bits, enums, counts, IDs, TTL/hops,
   metadata, and cross-field rules;
6. apply logical receiver and identity admission: SID16 is the standalone
   route namespace and needs no remote full-identity lookup; SID8 requires
   exactly one FULL_TAVRN context match; direct outer-AdvA/SID bindings still
   derive SID16 only from outer AdvA and enforce the identity contract's
   collision rules;
7. record the direct-transmitter RSSI observation and apply only the
   type/status-permitted liveness update (BUSY/REJECTED HACK never refreshes
   route or GTT state);
8. check the type-specific equality-only dedupe/correlation key; after successful
   normal admission, a newly admitted targeted freshness response suppresses a
   matching delayed-not-yet-enqueued intermediary response for the same
   requester/subject, but never retracts queued or in-flight work;
9. for new DATA emit a candidate token without dedupe/HACK/state commit; the
   router/application performs the ACK contract's synchronous
   reserve-and-`resolve_rx` phase, with defensive
   `timer.link_candidate_resolve_ms` BUSY timeout (BALANCED 10 ms);
   other types perform their validated route, GTT, or relay side effects.

A SID8 decoder context requires its identity conflict/resolution callback. For a
SID8 HACK only, the full outer immediate transmitter and immediate receiver
require current unambiguous context validation. `data_origin` and
`final_destination` are custody-correlation keys: they require the encoded SID8
width, unicast syntax, nonreserved value, and otherwise valid encoded form, but
do not require current live GTT resolution. DATA and every other control retain
normal SID8 identity admission. Exact active-custody correlation, including the
outer AdvA and every HACK custody key, is the state-mutation boundary; this
exception is not HACK authentication.

No partial metadata, route, GTT, or duplicate-cache mutation is permitted for
a malformed or foreign frame. A routed magic match with unsupported version,
network, or type is an isolated drop. A non-routed magic is returned to the
classifier so legacy wire-v1 or exact seven-byte patient advertisements can be
considered; it is not counted as a malformed routed frame.

Reserved flag bits, invalid/broadcast origins, invalid unicast receivers,
illegal metadata mode/count, mismatched fixed length, unsupported event/status,
duplicate RERR entries, unresolved SID8, or an ambiguous active identity make
the whole frame malformed. A valid standalone remote SID16 is not malformed
merely because no full AdvA is known for it. Semantic validation occurs before
inserting a dedupe key. Duplicate committed DATA is the one deliberate special
case after validation: it emits no candidate and performs no second
custody/application action, but generates a fresh DUPLICATE HACK.

### 9.1 Dedupe retention

| Type | Key | Minimum retention / disposition |
| --- | --- | --- |
| DATA | key in section 4.1 | `timer.link_data_dedupe_ms` and for as long as this node still owns custody, whichever is longer (BALANCED minimum 10 s) |
| HACK | exact pending key in ACK contract | no receive-cache insertion; completion is idempotent and late copies are unmatched |
| FLOOD | `{network,type,origin,flood_seq}` | `timer.link_flood_dedupe_ms` (BALANCED 10 s) |
| E_RREQ | `{network,active-width origin ID,request_id}` | `timer.aodv_rreq_seen_ms`; every local ring TX uses a fresh ID (BALANCED 10 s) |
| E_RREP | semantic/correlation tuple in section 5.2 | through the correlated discovery/route-install decision and then `timer.aodv_rrep_dedupe_ms` (BALANCED 10 s) |
| E_RERR | `{network,reporter,rerr_seq}` | `timer.aodv_rerr_dedupe_ms` (BALANCED 10 s) |
| E_RREP_ACK | exact pending RREP tuple | no receive-cache insertion; consume at most one pending wait |
| HELLO | key in section 6.1 | Ordinary HELLO uses `timer.hello_dedupe_ms`; targeted work retains its originating equality key through wrap-safe `obligation_started_ms + timer.hello_dedupe_ms`, due at equality, except the identity contract's idempotent rejoin barrier (BALANCED 10 s) |
| SYNC_* | full mentor/mentee snapshot/index tuple | through bootstrap completion plus `timer.mentor_sync_dedupe_ms` (BALANCED 10 s) |
| TC_UPDATE | `{network,full_origin,tc_seq}` | `timer.tc_uuid_ms`; subject key `{full_subject,event}` additionally uses `timer.tc_subject_ms` (BALANCED 30 s / 1 s) |

Retries and relays preserve the relevant key. Cache deadlines are wrap-safe
32-bit milliseconds. Rate limits MUST prevent one origin from reusing all
65536 values inside the key's retention interval. Dedupe is complete-key
equality only: serial freshness and exact-half rejection never compare two
different dedupe keys.

## 10. Golden vectors

All vectors use network `2a`. Known board A is canonical AdvA
`18 42 de 52 4a dd` (`SID16=18 42`, `SID8=18`); board B is
`dc 4b 0a 06 03 f8` (`SID16=dc 4b`, `SID8=dc`). Invented board C is
`11 22 33 44 55 c1` (`SID16=11 22`, `SID8=11`). Values shown are complete
AdvData, beginning at the Flags length byte.

### Link DATA16: seven-byte patient payload, urgent

`data_seq=1234`, next B, origin A, final C, patient DEVICE_ID `07`; patient
bytes are `01 05 64 34 12 50 7e`.

```text
02 01 06 1b ff ff ff 54 52 02 2a 10 20 30 dc 4b 18 42 11 22
34 12 01 07 01 05 64 34 12 50 7e
```

PDU=24, AdvData=31. With outer A, raw prefix is
`42 25 18 42 de 52 4a dd` and raw buffer length is 39.

### HACK16: ACCEPTED for that DATA

```text
02 01 06 14 ff ff ff 54 52 02 2a 11 00 18 42 18 42 11 22
34 12 01 07 00
```

PDU=17, AdvData=24. Outer transmitter is B; receiver is A.

### Generic controlled FLOOD16

Origin A, TTL 4/hops 0, sequence `0102`, diagnostic body `aa bb cc`:

```text
02 01 06 13 ff ff ff 54 52 02 2a 12 00 40 18 42 02 01 01
03 aa bb cc
```

PDU=16, AdvData=23.

### AODV E_RREQ8 with two metadata entries

Origin A, request `1001`, destination C, TTL 5, destination sequence `0203`,
origin sequence `0405`; metadata describes B (`bucket=10,request=1`) and C
(`bucket=5`).

```text
02 01 06 17 ff ff ff 54 52 02 2a 01 88 50 18 01 10 11 03
02 05 04 02 dc a1 11 50
```

PDU=20, AdvData=27.

### AODV E_RREP8 with required E_RREP_ACK and one metadata entry

Next B, route destination C, RREQ origin A, request `1001`, destination
sequence `0203`, 30000 ms lifetime (`812c` encoded, LE `2c 81`).

```text
02 01 06 16 ff ff ff 54 52 02 2a 02 c8 42 dc 11 03 02 18
01 10 2c 81 01 dc a0
```

PDU=19, AdvData=26.

### AODV E_RERR8 with two unreachable destinations

```text
02 01 06 14 ff ff ff 54 52 02 2a 03 80 30 18 01 20 02 11
04 02 dc 05 03
```

PDU=17, AdvData=24.

### AODV E_RREP_ACK8

For the RREP above, ACK sender B (outer AdvA) replies to receiver C for route
destination C, destination sequence `0203`, RREQ origin A, request `1001`:

```text
02 01 06 10 ff ff ff 54 52 02 2a 09 80 11 11 03 02 18 01 10
```

PDU=13, AdvData=20.

### Full-identity bootstrap HELLO16 with boot nonce 0001

```text
02 01 06 16 ff ff ff 54 52 02 2a 04 40 10 ff ff ff ff 18
42 de 52 4a dd 01 00
```

PDU=19, AdvData=26.

### Fixed-k targeted freshness request HELLO8

```text
02 01 06 17 ff ff ff 54 52 02 2a 04 b8 10 dc dc 18 42 de
52 4a dd 02 00 01 dc 01
```

PDU=20, AdvData=27.

### Fixed-k targeted freshness response HELLO8

Board B responds directly to requester A about subject B, with response node
sequence `0003` and target-self bucket 15 (`min(15, floor(300000/20000))` for
the BALANCED hard expiry):

```text
02 01 06 17 ff ff ff 54 52 02 2a 04 a8 10 18 18 dc 4b 0a
06 03 f8 03 00 01 dc f0
```

PDU=20, AdvData=27. The full origin and outer AdvA are B; the final target and
immediate receiver are A. This is `0xa8`, not an ordinary HELLO reply.

### SYNC_OFFER (full)

Mentor A offers a two-entry snapshot `3344` to mentee B in response to boot
nonce `0001`:

```text
02 01 06 1b ff ff ff 54 52 02 2a 05 00 10 18 42 de 52 4a
dd dc 4b 0a 06 03 f8 02 44 33 01 00
```

PDU=24, AdvData=31.

### SYNC_PULL index 0

```text
02 01 06 19 ff ff ff 54 52 02 2a 06 00 dc 4b 0a 06 03 f8
18 42 de 52 4a dd 44 33 00 01
```

PDU=22, AdvData=29.

### SYNC_DATA one-entry final page

Mentor A sends C with sequence `0405`, TTL bucket 10 and hop count 2:

```text
02 01 06 1b ff ff ff 54 52 02 2a 07 c0 dc 4b 0a 06 03 f8
44 33 00 11 22 33 44 55 c1 05 04 a2
```

PDU=24, AdvData=31.

### TC_UPDATE JOIN

Origin A sequence `7788` announces full subject C at uptime `1234` seconds:

```text
02 01 06 1b ff ff ff 54 52 02 2a 08 00 30 18 42 de 52 4a
dd 88 77 11 22 33 44 55 c1 00 34 12
```

PDU=24, AdvData=31.

## 11. Maximum-capacity and exact-length rejection vectors

These are additional complete AdvData vectors. They freeze every checker-found
capacity edge rather than relying only on formulas.

### 11.1 DATA8 maximum opaque payload

SID8, full-diameter TTL 15, and ten opaque application bytes:

```text
02 01 06 1b ff ff ff 54 52 02 2a 10 80 f0 dc 18 11 34 56
7f 00 00 01 02 03 04 05 06 07 08 09
```

PDU=24, AdvData=31.

### 11.2 FLOOD8 maximum body

Twelve-byte body `a0..ab`:

```text
02 01 06 1b ff ff ff 54 52 02 2a 12 80 f0 18 02 01 01 0c
a0 a1 a2 a3 a4 a5 a6 a7 a8 a9 aa ab
```

PDU=24, AdvData=31.

### 11.3 E_RREQ8 with four metadata entries

This ring transmission has its own request ID `1001` and full-diameter TTL 15:

```text
02 01 06 1b ff ff ff 54 52 02 2a 01 88 f0 18 01 10 11 03
02 05 04 04 dc a1 11 50 aa 42 bb 32
```

PDU=24, AdvData=31.

### 11.4 E_RREP8 with three metadata entries

```text
02 01 06 1a ff ff ff 54 52 02 2a 02 c8 42 dc 11 03 02 18
01 10 2c 81 03 dc a0 11 50 aa 41
```

PDU=23, AdvData=30. This is the largest legal E_RREP8; byte 24 is not padded.

### 11.5 E_RERR8 metadata capacity by unreachable count

One unreachable destination plus four metadata entries:

```text
02 01 06 1a ff ff ff 54 52 02 2a 03 90 f0 18 01 20 01 11
01 01 04 dc a1 11 50 aa 42 bb 32
```

PDU=23, AdvData=30.

Two unreachable destinations plus three metadata entries:

```text
02 01 06 1b ff ff ff 54 52 02 2a 03 90 f0 18 02 20 02 11
01 01 aa 02 01 03 dc a1 11 50 aa 42
```

PDU=24, AdvData=31.

Three unreachable destinations plus one metadata entry:

```text
02 01 06 1a ff ff ff 54 52 02 2a 03 90 f0 18 03 20 03 11
01 01 aa 02 01 bb 03 01 01 dc a1
```

PDU=23, AdvData=30.

Four unreachable destinations and zero metadata slots:

```text
02 01 06 1a ff ff ff 54 52 02 2a 03 80 f0 18 04 20 04 11
01 01 aa 02 01 bb 03 01 dc 04 01
```

PDU=23, AdvData=30. Setting `M=1` for this shape is malformed even if no
metadata bytes follow.

### 11.6 Empty SYNC_DATA

Snapshot `3344`, mentee B, present clear, last set, index zero:

```text
02 01 06 12 ff ff ff 54 52 02 2a 07 40 dc 4b 0a 06 03 f8
44 33 00
```

PDU=15, AdvData=22.

### 11.7 E_RREP_ACK8 with destination sequence

```text
02 01 06 10 ff ff ff 54 52 02 2a 09 80 11 11 03 02 18 01
10
```

PDU=13, AdvData=20.

### 11.8 Mandatory exact-length rejection mutations

For each row, start from the complete legal vector named above, change only
AdvData offset 3 as shown, and keep the actual bytes unchanged. The decoder
MUST reject the whole frame for an exact wrapper/type-length mismatch before
dedupe or state mutation.

| Legal vector | Legal offset 3 | Mutation | Required result |
| --- | ---: | ---: | --- |
| DATA8 maximum | `1b` | `1a` | reject |
| FLOOD8 maximum | `1b` | `1a` | reject |
| E_RREQ8 metadata 4 | `1b` | `1a` | reject |
| E_RREP8 metadata 3 | `1a` | `19` | reject |
| E_RERR8 D=1, metadata 4 | `1a` | `19` | reject |
| E_RERR8 D=2, metadata 3 | `1b` | `1a` | reject |
| E_RERR8 D=3, metadata 1 | `1a` | `19` | reject |
| E_RERR8 D=4, metadata 0 | `1a` | `19` | reject |
| empty SYNC_DATA | `12` | `11` | reject |
| E_RREP_ACK8 with destination sequence | `10` | `0f` | reject |

These byte strings, exact lengths, rejected mutations, and the size equations
are mandatory codec test vectors.
