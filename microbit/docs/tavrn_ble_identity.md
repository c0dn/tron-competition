# TAVRN-BLE identity and compression contract

Status: **V2.3 normative proof-of-concept contract**. This defines identity
bytes, not authentication. There is no provisioning, cryptographic binding,
privacy address rotation, or Bluetooth Mesh identity claim.

Profile mapping: this document derives `IDENT-01` through `IDENT-04`,
`SERIAL-01` through `SERIAL-04`, `ESC-01` through `ESC-03`, `BOOT-01`
through `BOOT-05`, and the full-identity membership context used by
`GOSSIP-01` through `GOSSIP-03`.

## 1. Canonical identity

**ID-001:** Canonical node identity is:

```text
{ address_type = RANDOM_STATIC, AdvA[0..5] }
```

`AdvA[6]` is stored and printed by this project in the exact order found in the
nRF RADIO packet buffer at raw offsets 2..7: least-significant address octet
first. A colon-form string in these documents preserves that array order; it is
not the conventional human format that some Bluetooth tools reverse.

For canonical `18:42:de:52:4a:dd`:

```text
AdvA[0] = 18    // least-significant address octet
AdvA[1] = 42
AdvA[2] = de
AdvA[3] = 52
AdvA[4] = 4a
AdvA[5] = dd    // most-significant octet, random-static bits live here
```

The full six bytes are the authority for full-identity equality, logs,
bootstrap, collision detection, and configured AdvA overrides. SID16 route
tables intentionally use the derived standalone logical value without needing
a remote full mapping. A compressed ID is never canonical identity by itself.

The outer AdvA of each received advertisement is the immediate transmitter.
Forwarding changes outer AdvA to the relay's identity while preserving the
wire type's explicit route/control origin.

Identity consumes radio data only from bounded typed mesh seams. Init,
idle/disable, listen, RX restore, snapshot, and TX cannot expose partial
identity state; state waits use `timer.radio_state_timeout_ms` and TX uses
`timer.radio_tx_event_bound_ms`. A snapshot/length/state failure installs no
AdvA/SID binding.
For TX evidence, a completed-channel mask of zero is TX_FAILED; a nonzero mask
is TX_DONE even if a later selected channel or restore faults, and the mask plus
fault evidence is retained. No identity or custody path may classify a
potentially heard nonzero-mask event as not attempted.

## 2. Exact FICR/default byte order

The current `ble_radio.c` constructs a default address exactly as follows:

```text
AdvA[0] = DEVICEADDR0[7:0]
AdvA[1] = DEVICEADDR0[15:8]
AdvA[2] = DEVICEADDR0[23:16]
AdvA[3] = DEVICEADDR0[31:24]
AdvA[4] = DEVICEADDR1[7:0]
AdvA[5] = DEVICEADDR1[15:8] | c0
```

This matches the nRF FICR split (DEVICEADDR0 contains the low 32 address bits;
the low 16 bits of DEVICEADDR1 contain the high address bits) and Bluetooth's
least-significant-octet-first transmission. The `c0` operation makes address
bits 47:46 equal `11`, the random-static subtype.

**ID-002:** Identity code MUST use the same six-byte array supplied to
`ble_radio_advertise*()`. It MUST NOT independently re-read and reinterpret
FICR in packet codecs, and it MUST NOT derive identity from `DEVICEID`.
Runtime default-AdvA acquisition is itself a bounded typed initialization seam;
failure produces no usable identity and fails closed.

## 3. Pre-ESC SID16 and fixed-k SID8

The compressed identities are the low numeric suffixes of the canonical
48-bit address. Because canonical AdvA is already least-significant octet
first, their wire bytes are direct prefixes of the canonical array:

```text
SID16 bytes = { AdvA[0], AdvA[1] }
SID16 value = AdvA[0] | (AdvA[1] << 8)
SID8        = AdvA[0] = SID16 & ff
```

Thus all SID16 fields are ordinary little-endian 16-bit values and SID8 is the
low byte. They are not the final two bytes of the project colon string.

| Board / canonical AdvA | SID16 wire | SID16 numeric | fixed-k SID8 |
| --- | --- | ---: | ---: |
| clean board A `18:42:de:52:4a:dd` | `18 42` | `4218` | `18` |
| clean board B `dc:4b:0a:06:03:f8` | `dc 4b` | `4bdc` | `dc` |

**ID-003:** `I=0` in a type-specific wire flag selects SID16 (`W=2`) and is
the mandatory AODV_ONLY/pre-ESC mode. SID16 is a standalone logical routing
namespace: route keys, remote origins, and remote destinations use the 16-bit
value directly and do not require a network-wide SID16-to-full-AdvA mapping.
For a direct peer, the receiver derives SID16 from outer `AdvA[0..1]` and
retains `{SID16,AdvA}` as the next-hop/HACK binding. The peer does not advertise
or configure a separate SID16 value.

`I=1` selects SID8 (`W=1`) and is legal only after FULL_TAVRN mentorship has
produced a collision-free full-identity GTT. Every remote SID8 lookup requires
exactly one full-identity context match. The common wrapper, type values, role
semantics, sequence widths, and route core remain routed wire-v2 in both modes.
A SID8 decoder context requires its identity conflict/resolution callback. The
sole HACK exception is that its full outer immediate transmitter and immediate
receiver remain current unambiguous context checks, while HACK origin and final
destination are structurally validated SID8 custody-correlation keys (correct
width, unicast syntax, nonreserved value, valid encoded form) and are not live
GTT lookups. DATA and every other control retain normal identity admission.
Exact active-custody correlation is the mutation boundary; this is not HACK
authentication.

Dynamic entropy and `k=2..6` are not encoded by this proof of concept. A future
wire version must add an explicit mode before using another width; it may not
reinterpret `I` or silently widen a v2 field.

## 4. Reserved identities and address validity

| Namespace | Invalid / unassigned | Broadcast | Valid unicast |
| --- | --- | --- | --- |
| SID16 | `00 00` | `ff ff` | all other standalone logical values; direct bindings must be unique |
| SID8 | `00` | `ff` | `01..fe`, uniquely mapped |
| full AdvA | all zero | all `ff` is never a node | valid random-static address |

An AdvA is valid for this profile only when:

- `(AdvA[5] & c0) == c0`;
- the 46-bit random part is neither all zero nor all one, as required for a
  Bluetooth random static address;
- full AdvA is not all zero or all `ff`;
- local SID16 is not `0000` or `ffff`; and
- fixed-k activation additionally requires local SID8 not `00` or `ff`.

A device whose factory or configured address produces a reserved suffix cannot
participate under that width. A valid remote standalone SID16 is not rejected
merely because its full AdvA is unknown. No suffix is remapped because such
remapping would create hidden context.

Broadcast is legal only in fields explicitly described as receiver/target
broadcast by the wire contract. It is malformed as transmitter, origin,
subject, DATA destination, route destination, or HACK correlation identity.

## 5. Configured AdvA override; no configured SID

**ID-004:** A build may provide one six-byte `TRON_ADVA_OVERRIDE` in canonical
array order. The integration syntax may be CMake-generated, but the effective
value is exactly:

```c
static const uint8_t tron_adva[6] = {
    TRON_ADVA_OVERRIDE_BYTE0,
    TRON_ADVA_OVERRIDE_BYTE1,
    TRON_ADVA_OVERRIDE_BYTE2,
    TRON_ADVA_OVERRIDE_BYTE3,
    TRON_ADVA_OVERRIDE_BYTE4,
    TRON_ADVA_OVERRIDE_BYTE5,
};
```

When present, the override replaces the FICR default for **both** radio AdvA
and all full/SID identity derivation. It is validated by the rules above at
boot. Byte 5 already MUST contain the `c0` random-static bits; code MUST NOT
quietly mutate a configured override because logs/manifests must identify the
actual bytes flashed.

Routed-v2 has no independently configured SID16 or SID8 input. Every local SID
is recomputed from the one effective AdvA, including when AdvA is overridden.
A build variable, label, or test input named as a node/SID value MUST NOT alter
the routed on-wire SID independently. A deployment that needs chosen suffixes
chooses full valid `TRON_ADVA_OVERRIDE` values whose derived suffixes are the
desired values.

Generic non-hardware runtime-FICR artifacts carry the exact manifest sentinel
`RUNTIME_FICR` instead of claiming an address before boot. At runtime the
bounded default-address seam derives and logs canonical AdvA, SID16, and SID8.
Such an artifact is ineligible for routed hardware claims.

Every routed **hardware candidate** instead requires an explicit six-byte
`TRON_ADVA_OVERRIDE` equal to an independently observed target FICR-derived
canonical AdvA. Its manifest binds the full override and expected board UID.
Immediately before flashing, an independent verifier reads the target UID and
FICR, derives canonical AdvA with the exact byte-5 random-static mask, and
requires equality with both manifest UID and all six override bytes. Any
mismatch aborts flashing; a hardware candidate never falls back to
`RUNTIME_FICR`.

The generic manifest marks full identity and derived SID fields
`RUNTIME_FICR`; its boot log supplies the concrete canonical AdvA, numeric
SID16, and SID8. A hardware-candidate manifest and boot log both include the
concrete values, identity source, node mode, feature level, and fixed-k state.
An invalid override is a boot-time fail-closed error, not a fallback to FICR.

Before flashing a fleet candidate, inventory validation derives SID16 and SID8
from every effective full AdvA and requires global uniqueness for the selected
fleet/profile. Runtime AODV_ONLY still needs only direct `{SID16,AdvA}`
bindings; the inventory gate does not add a remote SID16 mapping requirement.

## 6. Full identity during bootstrap

**ID-005:** A node with no validated GTT cannot send or resolve SID8 route
traffic. Bootstrap uses full identity as follows:

- HELLO always carries the origin's full AdvA; `N=1` bootstrap HELLO uses
  SID16 broadcast receiver/target fields and a nonzero boot nonce.
- SYNC_OFFER carries full mentor and mentee AdvAs and echoes the mentee nonce.
- SYNC_PULL carries full mentee and selected mentor AdvAs.
- SYNC_DATA uses outer mentor AdvA, full mentee AdvA, and one full AdvA per
  page entry.
- TC_UPDATE always carries full origin and subject AdvAs, including JOIN.

The mentor freezes a snapshot sorted lexicographically by canonical
`AdvA[0]`, then `[1]` through `[5]`. It computes SID16/SID8 indexes from the
full entries and offers the snapshot only if its represented identity set is
valid. The mentee recomputes both indexes after every page and does not enable
`I=1` until the final page and all collision checks pass.

This full-identity mentorship requirement is FULL_TAVRN-only. AODV_ONLY uses
the routed-common full-identity N=1 announcement to establish the direct-peer
AdvA/SID16 binding, then routes in the standalone SID16 namespace without a
SYNC snapshot.

For a direct N=1 HELLO, the full origin AdvA MUST equal outer AdvA. The receiver
derives SID16 from those same outer bytes and installs `{SID16,AdvA}`; no SID16
field is added to HELLO.

One-entry SYNC_DATA pages are a deliberate cost of carrying full identity
inside the 24-byte custom PDU. There is no suffix-only bootstrap page.

After SID8 activation, the same full-identity SYNC_PULL/SYNC_DATA roles also
serve the private active-RFI path. The RFI initiator remains the full mentee
identity in SYNC_PULL and its selected direct mentor remains the full receiver;
each returned page still contains one full canonical identity. Thus a SID8
`known_remote_count` can only hint at a missing member; it cannot identify,
resolve, or establish that member without the existing full-identity page
merge/collision checks. Active RFI neither changes the active SID8 identity nor
re-enters bootstrap.

## 7. Collision handling: fail closed

Collision scope depends on identity width:

- SID16 is standalone routing identity. AODV_ONLY does not require full
  resolution of remote transit/origin/destination SID16 values. A SID16
  collision is actionable when two distinct **direct-peer outer AdvAs** derive
  the same SID16, when the local node's own SID16 is duplicated by a direct
  peer, or when FULL_TAVRN full-identity bootstrap reveals duplicate active
  SID16 values.
- SID8 is contextual. Two different active full AdvAs mapping to one SID8, no
  match, or a reserved SID8 makes that compressed lookup unusable.
- A local full identity mapping to a reserved value cannot participate under
  that width.

**ID-006:** Collision checks occur:

1. at local boot/override validation;
2. before accepting a bootstrap HELLO or TC_UPDATE JOIN;
3. while merging each SYNC_DATA page;
4. before transitioning from SID16 to SID8;
5. when binding a direct outer AdvA to its SID16 next-hop identity;
6. before resolving any SID8 receiver, origin, destination, subject, or
   metadata entry, except structurally validated HACK origin/final
   custody-correlation keys; and
7. whenever a later full-identity update changes the active set.

Required behavior:

- a remote SID16 with no full mapping: proceed as logical SID16;
- two direct outer AdvAs deriving one SID16: reject the affected direct binding
  and enter `IDENTITY_CONFLICT_SID16` without invalidating unrelated SID16
  routes;
- SID8 zero matches: unresolved drop;
- SID8 exactly one full-identity match: proceed;
- SID8 multiple matches: ambiguity drop and enter `IDENTITY_CONFLICT_SID8`;
- local SID collision/reserved ID: do not originate compressed DATA/control;
- do not accept DATA custody addressed through an ambiguous ID;
- do not mutate route/GTT/dedupe state from the ambiguous frame;
- invalidate routes whose required direct SID16 binding or SID8 context can no
  longer be resolved uniquely;
- preserve and log both/all full AdvAs and the colliding SID; and
- continue only full-identity bootstrap/diagnostic traffic needed to expose the
  conflict.

An E_RERR ambiguity bit may notify already-unambiguous precursors, but it does
not make the ambiguous frame acceptable and does not trigger dynamic entropy.
There is no arbitrary winner, first-seen winner, AdvA tie-break, byte remap, or
fallback from SID8 to a guessed SID16 peer. Dynamic `k` selection, ambiguity
metadata, and a new wider wire mode are roadmap work.

For the two known boards, both SID16 (`4218`, `4bdc`) and SID8 (`18`, `dc`) are
distinct and non-reserved. This proves only those two identities, not a future
fleet.

## 8. Reboot, sequence reset, and dedupe safety

Wire-v2 counters are RAM-only 16-bit values. Reboot is not treated as normal
serial wrap.

At boot, the platform creates one nonzero 16-bit `boot_nonce`. The value is
held unchanged for the entire incarnation and copied into every repeated
`HELLO N=1`; zero is malformed. This nonce is an incarnation discriminator,
not a freshness serial and not a security token.

Counter initialization/allocation is exact:

- after establishment, DATA sequence, generic flood sequence, AODV request ID,
  RERR sequence, HELLO node sequence, TC sequence, and mentor snapshot ID each
  initialize independently to `0001`. The one HELLO node-sequence stream covers
  ordinary `N=0` HELLO and every newly originated targeted request/response;
- an ordinary HELLO pending on BUSY retains its exact
  `snapshot.next_node_sequence`; it advances neither cadence nor that visible
  cursor until queue admission. One internal reservation frontier makes the
  shared stream collision-free. With no ordinary pending, a newly created/adopted
  targeted context reserves current `next_node_sequence` and immediately advances
  it and the frontier modulo 65536 while skipping active targeted reservations.
  With an ordinary pending, that ordinary value is reserved first and targeted
  contexts reserve subsequent frontier values without changing the pending bytes
  or visible cursor. On ordinary admission, merge `next_node_sequence` to the
  frontier, or ordinary+1 when no targeted reservation exists, skipping still
  active targeted reservations. BUSY or zero-channel `TX_FAILED` retries retain
  exact target bytes/value, relays preserve origin value, four contexts reserve
  distinct values, and canceled unsent targeted values are legal gaps that are
  never reused during that incarnation;
- every other new semantic transaction uses the current value from its own stream
  and then increments modulo 65536; zero is legal after normal wrap;
- each locally emitted expanding-ring RREQ transmission is a new semantic
  request and therefore allocates a fresh request ID; the off-wire discovery
  generation groups those IDs;
- a link retry or relay of one already-emitted frame keeps its original value;
  and
- destination sequence zero means unknown only where the E_RREQ `U` bit is
  set. It is otherwise an ordinary serial value within an incarnation.

Scheduler tracking tokens are separate from node sequences. `0x0001..0x7fff` is
the link-owned tracked domain shared by custody DATA and routed-common tracked
bootstrap/incarnation HELLO; its allocator checks every retained, queued, and
in-flight low-domain item. `0x8000..0xffff` is maintenance verification only. A
caller-owned high token is checked against every queued/in-flight token before
admission; token zero is untracked and no third domain exists.

The implemented FULL runtime routes targeted stage-0 `TX_DONE`, `TX_FAILED`, and
terminal scheduler-fault events back to that high-token owner. Retained-hop and
full-diameter RREQ verification remain deferred and consume no high token here.

**ID-007:** Rejoin is a routed-common incarnation barrier shared by AODV_ONLY
and FULL_TAVRN:

1. A booted node starts `REJOINING`, emits full-identity `HELLO N=1` with its
   nonzero nonce, and does not originate or forward DATA/AODV traffic.
2. N=1 is admitted before the ordinary HELLO dedupe/high-water path. The key
   `{full AdvA,boot_nonce}` is equality-only. Repeats of the same pair are
   idempotent and do not clear state again. A valid different nonce for that
   full AdvA atomically starts a new routed incarnation and clears old pending
   HACK/custody, routed dedupe, route, request-generation, serial freshness,
   and direct-peer binding state for that identity. In the implemented FULL
   maintenance foundation, the router-committed first/new nonce also clears
   only that nonself GTT serial and ordinary-HELLO equality keys before the
   next N=0 is evaluated; retained identity, hop, departed, and lifetime facts
   are not rewritten.
3. Exact-half serial rejection never applies between different incarnation or
   dedupe keys. It applies only to freshness values within the same established
   key/stream.
4. AODV_ONLY repeats the same N=1 announcement during
   `timer.router_reboot_announce_ms` (BALANCED 3000 ms), then self-establishes,
   emits normal `HELLO N=0`, and may start SID16 routing without a GTT.
5. FULL_TAVRN uses the same routed-common reset, but remains REJOINING through
   offer collection and full-identity SYNC. It self-bootstraps after
   `timer.mentor_self_bootstrap_ms` without a usable offer, or establishes after
   the final valid page, then emits normal N=0 and may activate SID8 only after
   context validation.
6. All old local TX/custody queue items from before boot are lost and cannot be
   claimed as delivered. Neighbor state treats nonce change as route
   invalidation, not sequence freshness.

Equality-only dedupe entries have bounded retention; rate limits must make it
impossible to allocate 65536 values while one complete key can remain cached.

This barrier is medium-correctness reboot handling, not replay protection. In
the no-security proof of concept, a forged full-identity N=1 with a different
nonce could force a reset; production authentication is explicitly out of
scope.

## 9. Golden identity vectors

### 9.1 FICR/default derivation shape

If `DEVICEADDR0=52de4218` and low 16 bits of `DEVICEADDR1=1d4a`, the current
radio algorithm yields:

```text
AdvA = 18 42 de 52 4a dd
SID16 wire = 18 42
SID16 value = 4218
SID8 = 18
```

`AdvA[5]` changes from `1d` to `dd` because of `| c0`. No other byte changes.

### 9.2 Routed-common reboot incarnation

Board A's bootstrap HELLO with nonzero boot nonce `0001` is:

```text
02 01 06 16 ff ff ff 54 52 02 2a 04 40 10 ff ff ff ff 18
42 de 52 4a dd 01 00
```

Repeating those bytes with outer board A is the same incarnation and is
idempotent. Changing only the final nonce bytes to `02 00` is a different
incarnation admitted before ordinary HELLO dedupe; changing them to `00 00` is
malformed.

### 9.3 Maximum raw DATA advertisement from board A

The wire document's 31-byte patient DATA AdvData, sent by board A, starts with:

```text
42 25 18 42 de 52 4a dd 02 01 06 1b ff ff ff 54 52 02 2a 10 ...
```

- `42`: nonconnectable advertising plus random TxAdd;
- `25`: raw payload length 37 decimal = AdvA 6 + AdvData 31;
- next six bytes: canonical immediate transmitter;
- `02 01 06`: Flags AD structure;
- `1b ff ff ff`: Manufacturer AD length/type/company;
- `54 52 02 2a 10`: routed magic/version/network/DATA type.

The complete raw buffer is 39 bytes. A receiver must preserve bytes 2..7 as
the immediate transmitter rather than returning only bytes 8 onward.

## 10. Required identity assertions

Later tests must cover at least:

- both known AdvA -> SID16/SID8 vectors;
- exact FICR little-endian extraction and byte-5 `c0` mask;
- valid configured override used by both radio and codec;
- invalid override fails without FICR fallback;
- generic runtime-FICR manifest fields use exact `RUNTIME_FICR`, while every
  routed hardware candidate has concrete full override and expected UID;
- UID-bound preflash verification rejects any independently observed
  UID/FICR-to-override mismatch;
- no configured SID input can change SID16/SID8 independently of effective
  AdvA;
- reserved full/SID16/SID8 rejection;
- two direct AdvAs deriving one SID16 fail their binding, while an unmapped
  remote transit SID16 remains a valid standalone logical route ID;
- fleet inventory rejects duplicate derived SID16/SID8 values before flashing;
- SID8 zero/one/multiple full-context lookup outcomes;
- no fixed-k traffic before completed full-identity bootstrap;
- one-entry full-identity SYNC pagination and deterministic sorting;
- V2.3 ordinary SID8 HELLO is exactly 20 bytes with count `0..15`, reserved
  zero bytes, occupied/nonself/nondeparted membership semantics, retained
  hard-expired inclusion, legacy 17-byte rejection, and no equal-count
  consensus inference;
- homogeneous FULL-fleet upgrade rejects old/new ordinary-HELLO mixing without
  a fallback or per-peer downgrade;
- RFI-marked page-zero SYNC_PULL retains full initiator/mentor roles, freezes a
  bounded snapshot, serves contiguous SYNC_DATA pages, and leaves public
  SID8_ACTIVE identity/application admission unchanged;
- collision discovered during page merge prevents activation;
- later TC_UPDATE full identity collision invalidates affected routes;
- zero boot nonce rejection, same `{AdvA,nonce}` idempotency, and different
  nonce admission before ordinary HELLO dedupe;
- routed-common reboot clearing in both feature levels, AODV_ONLY bounded
  self-establishment via `timer.router_reboot_announce_ms`, and FULL_TAVRN
  continued SYNC/`timer.mentor_self_bootstrap_ms`;
- equality-only dedupe versus same-stream exact-half freshness behavior;
- one shared post-establishment HELLO node-sequence stream across ordinary HELLO
  and newly originated targeted request/response, including modulo wrap, four
  distinct concurrent reservations, legal canceled-unsent gaps, exact BUSY and
  zero-channel-failure retry bytes, and relay preservation;
- bounded init/idle/listen/restore/snapshot/TX failures install no partial
  identity, while nonzero completed-channel TX evidence remains an attempt even
  with a later fault;
- the raw RX event retains the exact outer AdvA.
