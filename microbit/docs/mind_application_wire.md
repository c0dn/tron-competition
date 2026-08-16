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

For reports, `packet_id24` is an exact application identity in `0..0xffffff`;
the pack API rejects wider values rather than silently masking them. Zero is
legal. The schema payload is preserved byte-for-byte, but validation requires
schema version 1, an event type known to schema v1, confidence `0..100`,
acceleration SVM `0..8000` little-endian, heartbeat confidence/mic level zero,
and `payload.seq == packet_id24 & 0xff`.

`mind_application_wire_pack_*()` validates before publishing its output;
`mind_application_wire_unpack_*()` validates before decoding. Callers copy to
or from generic TAVRN DATA only after their normal transport ownership and
route checks; TAVRN route origin and transport `data_seq` are not MIND report
identity fields. The report packer derives `urgent` from the schema event type;
validation and unpacking reject a report whose generic DATA urgent flag does
not match that derivation. Root records are always nonurgent.
