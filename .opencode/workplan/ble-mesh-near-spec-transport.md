# BLE mesh-like near-spec transport plan

## Status

Ready for implementation handoff after planning review. Planning only: no code changes, no hardware actions, no flash commands.

## Ground truth

- Platform: micro:bit v2 / nRF52833.
- Repo: `/home/lucas/Workspace/tron-worktrees/ble-mesh`, firmware under `microbit/`.
- Existing code has raw-radio BLE legacy advertising/scanning primitives in `microbit/app/drivers/ble_radio.{c,h}`.
- `ble_radio_advertise()` sends `ADV_NONCONN_IND` on primary advertising channels 37/38/39.
- `ble_radio_listen()` and `ble_radio_poll()` provide passive scan on one primary advertising channel at a time.
- Existing apps `ble_beacon` and `ble_observer` are separate; no full Bluetooth Mesh stack exists.
- One radio only: there is no true simultaneous TX/RX. Detector behavior must be passive listening with controlled interleaved TX windows.
- BLE lane does not need full Bluetooth Mesh compliance, but should preserve Bluetooth-Mesh-like shape where practical and be explicit about non-compliance.
- Payloads are dummy for now.
- Targeted local schema/spec search found no concrete protocol file; this plan freezes the approved conceptual schema.
- Build baseline for `test_firmware`, `ble_beacon`, and `ble_observer` was validated outside this session. Do not claim flashing.
- External grounding also used from `../admin-canonical/research_microbit_v2.md`, `../admin-canonical/research_takeaways_for_planning.md`, and `../admin-canonical/FLASH_GATING_POLICY.md`.
- In-workspace research/spec files include `BLE_MESH_LANE_BRIEF.md` and `research_ble_mesh_execution_pass.md`.
- `research_ble_mesh_execution_pass.md` is retained as execution-pass context, but any earlier build-baseline/submodule wording in that file does not supersede this durable workplan's current execution assumptions.

## Non-compliance statement

This slice implements an **experimental BLE advertising mesh-like transport**. It is **not Bluetooth Mesh compliant** because it omits provisioning, mesh network/application security, IV index, replay protection, SAR, GATT/PB-GATT, and standard mesh models. It must not emit official Bluetooth Mesh `Mesh Message` AD type `0x2A` in this slice.

## Phase 0: API, packet/spec, and build/test freeze

This phase must complete before parallel code-writer tasks.

### Packet AD structure

Use BLE legacy non-connectable advertising with a lab-only Manufacturer Specific Data AD structure (`type=0xFF`, company ID `0xFFFF`) carrying a TRON near-spec mesh-like inner PDU.

AdvData layout, max 31 bytes:

1. Flags AD structure: `[0x02, 0x01, 0x06]`.
2. Manufacturer Specific Data AD structure:
   - Length byte: `0x0F + payload_len`; max `0x1B` when `payload_len == 12`.
   - AD type: `0xFF`.
   - Company ID LE: `0xFFFF` lab/test placeholder.
   - Magic: ASCII `T`, `M`.
   - Version: `0x01`.
   - Message type/opcode: `uint8`; first slice uses `0x01` dummy/status and reserves others.
   - Network/group ID: `uint8`.
   - TTL: `uint8`, accept `0..3`, with `TRON_MESH_TTL_MAX = 3`.
   - Source ID: `uint16` little-endian.
   - Sequence: 24-bit little-endian, RAM-only per boot.
   - Payload length: `uint8`, `0..12`.
   - Dummy payload: `0..12` bytes.

Total AdvData length is `19 + payload_len`; max is exactly `31` bytes. There is no slack. Packet encoding must fail before `ble_radio_advertise()` would truncate.

No extra checksum/MIC in this slice. BLE CRC only handles on-air corruption; this is not secure/authenticated Bluetooth Mesh traffic.

Decode rejects invalid AD length, missing flags/manufacturer AD, wrong company ID, wrong magic, unsupported version, payload length overflow or mismatch, or TTL greater than max.

### Packet module contract

Create protocol logic outside the hardware driver, e.g. `microbit/app/protocol/tron_mesh_packet.{h,c}`.

The header should define:

- constants for company ID, magic, version, max payload, max TTL, max AdvData length;
- packet struct fields: `net_id`, `ttl`, `src`, `seq24`, `msg_type`, `payload_len`, `payload[12]`;
- encode/decode helpers operating on BLE AdvData bytes only;
- result enum for OK, length error, missing AD, missing flags, company mismatch, magic/version mismatch, payload overflow/mismatch, TTL invalid.

Keep this module pure and host-testable: no FICR reads, no sequence counter ownership, no RADIO access.

### Node identity and sequence ownership

- Default source ID derives from nRF FICR device address low bits in node/platform code, not in `tron_mesh_packet`.
- Optional compile-time `TRON_NODE_ID` override may be provided.
- Sequence is a 24-bit RAM counter initialized at boot and owned by node/platform code.
- No NVM writes in this slice.
- Docs/logs must warn that replay resistance is not Bluetooth Mesh-compliant.

### Scheduler module contract

Create a polling-first scheduler wrapper, e.g. `microbit/app/protocol/ble_mesh_scheduler.{h,c}` or `microbit/app/drivers/ble_radio_scheduler.{h,c}` if hardware ownership requires driver placement.

It must be the single owner of the raw radio calls during mesh-node operation:

`RX_LISTEN(channel) -> RX_DISABLE/IDLE -> TX_ADV_WINDOW(channel_mask) -> RX_RESTORE(channel)`

Defaults:

- hop across 37/38/39;
- dwell 50 ms unless overridden;
- one queued packet transmits once on all three primary advertising channels;
- TX queue capacity 4;
- relay packets are dropped first on full queue;
- expose counters.

If current `ble_radio` lacks a safe public idle/stop hook, add a minimal explicit `ble_radio_idle()` / `ble_radio_stop()` hook rather than hiding RX disable behavior in unrelated code.

The scheduler/driver contract must include safe RX snapshotting before decode, such as disable-before-copy, double buffering, or another reviewed driver API, so continuous RX auto-restart cannot overwrite the packet being processed.

Preserve existing `ble_beacon` and `ble_observer` behavior/source compatibility unless a deliberate, reviewed API adjustment is required.

### Build and test integration contract

- Protocol sources live in `microbit/app/protocol/`.
- Firmware apps include protocol/scheduler sources directly in each target CMake unless Phase 0 deliberately introduces a small shared object/static library.
- Packet slice owns the host-native packet test harness and any related host CMake/scripts.
- Integration slice owns `ble_mesh_node` firmware CMake.
- Host packet tests must be executable without flashing. Exact planned command from `microbit/`:

```bash
./tests/protocol/run_tron_mesh_packet_tests.sh
```

- If the scheduler slice adds a host/simulation harness, exact planned command from `microbit/`:

```bash
./tests/protocol/run_ble_mesh_scheduler_tests.sh
```

A typical implementation can place host test files under `microbit/tests/protocol/` and keep them separate from the arm-none-eabi firmware CMake.

## Phase 1: Packet-layer implementation slice

Parallel-safe after Phase 0.

**Code-writer A scope:** `tron_mesh_packet` module, packet docs, and host-native packet tests only. No radio state access and no hardware register access.

Required host tests:

- encode/decode round trip;
- zero-length payload;
- max payload length 12;
- exact 31-byte packet;
- payload overflow reject;
- payload length mismatch reject;
- invalid AD length reject;
- missing Flags AD reject;
- missing Manufacturer Specific Data AD reject;
- wrong magic reject;
- wrong version reject;
- wrong company reject;
- TTL > 3 reject;
- 24-bit sequence little-endian;
- multiple AD structures.

Run code-checker on the packet slice before integration.

## Phase 2: Scheduler/radio ownership slice

Parallel-safe with Phase 1 after Phase 0.

**Code-writer B scope:** scheduler wrapper plus minimal `ble_radio` idle/stop/snapshot hook if required. Do not edit packet module.

Required behavior:

- passive RX by default;
- app loop calls scheduler poll/tick;
- bounded TX windows disable RX, transmit, then restore RX;
- no simultaneous TX/RX claim;
- safe RX snapshot before decode;
- queue/drop policy and counters implemented;
- existing `ble_beacon`/`ble_observer` builds preserved.

Required no-hardware scheduler validation cases:

- RX default state;
- enqueue TX;
- disable RX before TX;
- restore RX channel after TX;
- queue full/drop policy;
- relay-priority drops;
- rate limiting;
- channel dwell/hop behavior;
- safe RX snapshot ownership.

Run code-checker on scheduler/radio changes before integration.

## Phase 3: Combined detector/mesh-node target

Serialized after Phase 1 and Phase 2 pass code-checker.

Create `microbit/app/ble_mesh_node/` with a CMake target that includes the protocol/scheduler modules and keeps existing targets intact.

Behavior:

- listen passively by default;
- decode TRON mesh-like packets;
- drop duplicates before local log/delivery and relay scheduling;
- log non-duplicate dummy payloads and counters;
- generate own dummy/status packet at interval >= 1000 ms;
- relay only if TTL > 0, packet is not from self, and duplicate cache permits it;
- decrement TTL by 1 for relay;
- dedupe cache at least 16 entries, keyed by `net_id + src + seq24 + msg_type`, expiry around 10 s;
- random relay backoff 20-120 ms;
- relay rate limit max 5 relay TX events/sec;
- TX queue capacity 4 drops relay packets first;
- expose counters: `rx_ok`, `tx_ok`, `decode_error`, `duplicate_drop`, `ttl_drop`, `queue_drop`, `relay_scheduled`, `relay_rate_limited`.

Counter semantics: `ttl_drop` means relay suppressed because TTL is zero; it does not mean local delivery/logging was suppressed.

Serial logs must include: `experimental BLE advertising mesh-like transport; not Bluetooth Mesh compliant`.

Run code-checker on the integrated node slice before any hardware-related plan.

## Phase 4: No-flash verification and future hardware gate

Run verification from `/home/lucas/Workspace/tron-worktrees/ble-mesh/microbit`.

Required no-flash checks after implementation:

```bash
./tests/protocol/run_tron_mesh_packet_tests.sh
# If added by the scheduler slice:
./tests/protocol/run_ble_mesh_scheduler_tests.sh
./build.sh test_firmware
./build.sh ble_beacon
./build.sh ble_observer
./build.sh ble_mesh_node
```

Firmware-integrated tests are not considered executed unless flashed; packet behavior tests must be host-native or explicitly downgraded to compile-only in a separate reviewed plan revision. No flash commands.

Future hardware/flash gate:

- no flash during planning or initial implementation;
- before any future flash, run code-checker focused on hardware safety, hard-brick risk, wrong-target/wrong-probe risk, and flash-script assumptions;
- current `microbit/flash.sh` performs `pyocd erase --mass` and must not be used casually;
- Hermes must approve exact command and target and ensure no concurrent flashing;
- prefer UID-targeted normal target flashing only;
- no `MAINTENANCE` mode;
- no ambiguous device path;
- no hardware claims unless actually performed.

## Parallel execution shape

1. Serialize Phase 0 contract/spec/build-test freeze.
2. Run Code-writer A and Code-writer B in parallel after Phase 0:
   - A: packet module + host tests/docs only.
   - B: scheduler wrapper + minimal radio idle/snapshot hook only.
3. Run code-checker after A and after B.
4. Serialize Code-writer C for `ble_mesh_node` after A+B are accepted.
5. Run final code-checker and no-flash tests/builds.
6. Stop before any hardware action unless Hermes gate is explicitly satisfied.
