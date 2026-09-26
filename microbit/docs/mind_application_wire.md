# MIND Layer-7 wire records

`mind_application_wire` defines MIND application records after generic TAVRN
DATA has arrived. TAVRN transports the existing generic fields unchanged and
does **not** include these kinds, validators, or root policy. Consequently,
generic DATA can deliver a semantically malformed record; Layer 7 rejects it
only when `mind_application_wire_validate()` runs.

The application-owned input is `mind_application_wire_record_t`, a copied
counterpart of generic DATA's `{app_kind, app_source, urgent, app_len,
app_bytes}`. Its ten-byte buffer is an application boundary, not a new
transport contract. No record uses heap allocation.

| Kind | `app_source` | `urgent` | Exact `app_len` | `app_bytes` |
| --- | --- | --- | --- | --- |
| `MIND_REPORT` (`0x02`) | wearable `01..fe` | `event_type != HEARTBEAT` | 10 | packet ID24 little-endian at bytes `0..2`; unchanged seven-byte schema-v1 payload at `3..9` |
| `ROOT_STATE` (`0x03`) | `0` | `0` | 6 | version `1`, active `0` or `1`, nonzero nonce16 little-endian, nonzero generation16 little-endian |
| `ROOT_ACK` (`0x04`) | `0` | `0` | 7 | exact `ROOT_STATE` bytes followed by status: accepted `0`, duplicate `1`, capacity-rejected `2`, stale `3` |
| `MIND_REPORT_OBSERVED` (`0x05`) | wearable `01..fe` | `event_type != HEARTBEAT` | 10 | packet ID24 little-endian at `0..2`; schema version/event/confidence/accel-SVM/mic at `3..8`; original observer RSSI magnitude at `9` |

For reports, `packet_id24` is an exact application identity in `0..0xffffff`;
the pack API rejects wider values rather than silently masking them. Zero is
legal. The schema payload is preserved byte-for-byte, but validation requires
schema version 1, an event type known to schema v1, confidence `0..100`,
acceleration SVM `0..8000` little-endian, heartbeat confidence/mic level zero,
and `payload.seq == packet_id24 & 0xff`.

`MIND_REPORT_OBSERVED` carries the same logical report without the redundant
direct `seq` byte: bytes `3..8` are direct schema bytes `0..5`, and unpacking
reconstructs `seq` from `packet_id24 & 0xff`. Its dedicated packer validates the
complete original seven-byte direct schema, including that sequence, before
omission. Byte `9` is the observing backbone's original RX RSSI magnitude:
`0` means unavailable and `1..127` are valid. Values above `127` are rejected.
Legacy `MIND_REPORT` remains accepted and unpacks with RSSI unavailable. The
ten-byte observed record is the sole observer-RSSI carrier for local publication
and SID8 TAVRN DATA; SID16 cannot carry a ten-byte application payload.

For a valid nonzero observed magnitude, the logger emits exactly:

```text
mind_event_v2 now=<u32> root=<hex12> wearable=<u8> packet=<hex6> schema=1 event=<0..5> confidence=<0..100> svm=<0..8000> mic=<0..255> seq=<u8> observer=<hex12> observer_rssi_dbm=<-127..-1> path=<local|tavrn>
```

Legacy reports and observed zero RSSI retain the unchanged `mind_event_v1`
grammar without `observer_rssi_dbm`.

`mind_application_wire_pack_*()` validates before publishing its output;
`mind_application_wire_unpack_*()` validates before decoding. Callers copy to
or from generic TAVRN DATA only after their normal transport ownership and
route checks; TAVRN route origin and transport `data_seq` are not MIND report
identity fields. The report packer derives `urgent` from the schema event type;
validation and unpacking reject a report whose generic DATA urgent flag does
not match that derivation. Root records are always nonurgent.
