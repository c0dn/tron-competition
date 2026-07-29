# TRON hardware-testing handover

## Scope and current truth

This repository has two micro:bit v2 / nRF52833 experimental mesh lanes:

- `exp/ble-mesh`: a custom, lab-only BLE advertising mesh-like transport. It is **not Bluetooth Mesh compliant**.
- `exp/rf-mesh`: a project-owned Nordic proprietary 2.4 GHz protocol with root-aware/AODV-style routing.

The host tests and firmware builds pass as of this handover. No board was connected or flashed during the software handover, so RF behavior, range, stability, packet delivery, and coexistence remain **unverified on hardware**.

The ESP32-C3 lane remains deferred; this handover is micro:bit-first.

## Safety gate — mandatory before every flash

1. Use only **micro:bit v2** boards (nRF52833).
2. Review the exact commit, target, role configuration, probe UID, and command.
3. Run a code-checker review focused on hardware safety, hard-brick risk, wrong-target/probe risk, and flash-script assumptions.
4. Flash **one board at a time**. Never run concurrent flash jobs.
5. Target each board by its pyOCD probe UID. Do not guess from `/dev/ttyACM*` numbering.
6. Keep the boards on USB power for the first test. Do not attach external power or GPIO hardware.
7. The repository `microbit/flash.sh` performs a **mass erase** before loading. Preserve anything needed from a board before using it.
8. If the target, UID, or board revision is unclear, stop—do not flash.

At handover time, `/dev/ttyACM0` on the development host belongs to a Sonoff Zigbee dongle, not a micro:bit. Re-enumerate devices after connecting boards.

## Prerequisites

```bash
sudo apt install gcc-arm-none-eabi cmake ninja-build
uv tool install pyocd
uv tool install grabserial
```

Clone and initialise the kernel submodule:

```bash
git clone https://github.com/c0dn/tron-competition.git
cd tron-competition
git submodule update --init --recursive
```

Before connecting boards, capture a baseline; then connect one board and repeat:

```bash
pyocd list
ls -l /dev/serial/by-id/
```

Record a mapping for every board:

| Physical label | Role | pyOCD probe UID | Stable serial symlink | Commit |
| --- | --- | --- | --- | --- |
| A | | | | |
| B | | | | |
| C | | | | |

## Software-only preflight

### BLE lane

```bash
git switch exp/ble-mesh
git submodule update --init --recursive
cd microbit
./tests/protocol/run_tron_mesh_packet_tests.sh
./tests/protocol/run_ble_mesh_scheduler_tests.sh
./build.sh ble_mesh_node
```

Expected:

- 16 packet tests pass.
- BLE radio host tests pass.
- BLE scheduler tests pass.
- `build/firmware/ble_mesh_node/ble_mesh_node.elf` is produced.

### RF lane

```bash
git switch exp/rf-mesh
git submodule update --init --recursive
cd microbit
cc -std=c11 -Wall -Wextra -Werror \
  -I app/rf_mesh_core \
  tests/rf_mesh_core/test_rf_mesh_core.c \
  app/rf_mesh_core/rf_mesh_core.c \
  app/rf_mesh_core/rf_mesh_packet.c \
  -o /tmp/tron_rf_mesh_core_tests
/tmp/tron_rf_mesh_core_tests
./build.sh rf_mesh_node
```

Expected:

- `rf_mesh_core tests passed`.
- `build/firmware/rf_mesh_node/rf_mesh_node.elf` is produced for the generic compile check.
- Role images for flashing must be produced with `build-rf-role.sh`, not reused from the generic build cache.

## BLE hardware test

### Build and flash

All BLE nodes can use the same firmware; the default node ID is derived from FICR. Build once:

```bash
git switch exp/ble-mesh
cd microbit
./build.sh ble_mesh_node
```

After the mandatory review and UID verification, flash each board **serially**, replacing the placeholders:

```bash
./flash.sh ble_mesh_node --uid <BOARD_A_PROBE_UID>
# Wait for completion and disconnect/label or otherwise positively identify A.
./flash.sh ble_mesh_node --uid <BOARD_B_PROBE_UID>
```

Monitor each board using its stable serial symlink, preferably in separate terminals:

```bash
./serial.sh /dev/serial/by-id/<BOARD_A_SERIAL_LINK>
./serial.sh /dev/serial/by-id/<BOARD_B_SERIAL_LINK>
```

Expected boot/runtime text includes:

```text
experimental BLE advertising mesh-like transport; not Bluetooth Mesh compliant
mesh node src=0x.... net=0x01 ttl=3 passive RX with interleaved TX
mesh own dummy queued ...
mesh rx dummy ...
mesh counters rx_ok=... tx_ok=... decode_error=... duplicate_drop=... ttl_drop=... queue_drop=... relay_scheduled=... relay_rate_limited=...
```

### BLE acceptance sequence

1. **Two-node smoke test, 60 seconds**
   - Both nodes continue printing their own TX activity.
   - Each node's `rx_ok` and `tx_ok` increase.
   - Received packets show the other node's source ID.
   - No reset, lock-up, or runaway queue/drop behavior.
2. **Two-node stress test, 5 minutes**
   - Capture complete serial logs from both nodes.
   - Confirm counters continue increasing through the full interval.
   - Record final counters and any reset/stall timestamp.
3. **Relay test, 3 nodes**
   - Use spacing or shielding so A and C cannot communicate directly while B can hear both.
   - Confirm C receives A-originated packets and that B reports `relay_scheduled`/relay TX activity.
   - Treat this as exploratory until topology isolation is independently demonstrated.
4. **Coexistence/no-backbone test**
   - Current firmware interleaves RX and TX on one radio; it does not perform true simultaneous RX/TX.
   - It recognises the project packet format. Unrelated advertisements may contribute to `decode_error` and are not surfaced as beacon observations.
   - Therefore, do **not** mark the broader “listen for advertising beacons while mesh TX/RX” criterion complete yet. First add or enable explicit beacon-observation telemetry, rerun host/build checks, review, then test it on hardware.

## RF hardware test

### Build role-specific images

Use the checked role builder. It creates a fresh temporary build directory for
every image, rejects ambiguous role/node-ID combinations, and emits a
role/node-labelled ELF plus a configuration manifest. Keep the default network
ID `0x5452`, channel `7` (2407 MHz), and TX power `0x00` (0 dBm) for the first test.

```bash
git switch exp/rf-mesh
cd microbit

./build-rf-role.sh --role root --node-id 0x0001
./build-rf-role.sh --role node --node-id 0x1001
./build-rf-role.sh --role node --node-id 0x1002
```

Role-specific ELFs:

```text
artifacts/rf_mesh_node-root-0x0001.elf
artifacts/rf_mesh_node-node-0x1001.elf
artifacts/rf_mesh_node-node-0x1002.elf
```

### Flash role-specific images

Because `flash.sh` rebuilds from the shared default build directory, use pyOCD directly for these role-specific ELFs. Run each three-command group only after the mandatory review, and never overlap groups:

```bash
pyocd erase --mass --uid <ROOT_PROBE_UID>
pyocd load artifacts/rf_mesh_node-root-0x0001.elf --uid <ROOT_PROBE_UID>
pyocd reset --uid <ROOT_PROBE_UID>

pyocd erase --mass --uid <NODE_A_PROBE_UID>
pyocd load artifacts/rf_mesh_node-node-0x1001.elf --uid <NODE_A_PROBE_UID>
pyocd reset --uid <NODE_A_PROBE_UID>

pyocd erase --mass --uid <NODE_B_PROBE_UID>
pyocd load artifacts/rf_mesh_node-node-0x1002.elf --uid <NODE_B_PROBE_UID>
pyocd reset --uid <NODE_B_PROBE_UID>
```

Monitor each stable serial symlink. The firmware prints once per second:

```text
mesh id=0x.... root=... q=... rx=... tx_attempt=... dup=... drop=... relay_attempt=... route=... metric=... next=0x.... delivered=... appdrop=...
```

### RF acceptance sequence

1. **Root + one node smoke test, 60 seconds**
   - Root shows `root=1`; node shows `root=0`.
   - Node learns a root route: metric changes from `255`, and `next` is nonzero.
   - `rx`/`tx_attempt` increase without resets or stalls.
   - Root `delivered` increases when dummy DATA arrives.
2. **Two-node stress test, 5 minutes**
   - Capture full logs and final counters.
   - Verify the route remains live and delivery continues for the full interval.
   - Record drops/duplicates; do not equate `tx_attempt` with confirmed delivery because v2 has no ACK/retry.
3. **Three-node relay test**
   - Root A, relay B, edge C; isolate A↔C direct RF as far as practical.
   - C should learn a route whose next hop is B.
   - B's `relay_attempt` should increase.
   - Root's `delivered` should increase from C-originated DATA.
4. **Failure/recovery probes**
   - Power off relay B and observe route expiry/loss.
   - Restore B and record time to route recovery and resumed delivery.
   - Reboot C and verify it rediscovers the root without rebooting A/B.

## Evidence to save

For every run, keep:

- branch and `git rev-parse HEAD`;
- board role ↔ probe UID ↔ serial-link mapping;
- exact build configuration and flash commands;
- complete timestamped serial logs from every node;
- test start/end time and physical topology/spacing;
- final counters and observed resets/stalls;
- photos or a simple topology diagram for relay-isolation tests;
- pass/fail against each criterion, with failures preserved rather than summarised away.

Suggested log naming:

```text
hardware-results/<date>-<lane>-<test>/<role>-serial.log
hardware-results/<date>-<lane>-<test>/README.md
```

Do not commit raw logs until they have been checked for machine paths, personal identifiers, or unrelated serial output.

## Stop conditions

Stop immediately if:

- the probe UID does not match the labelled board;
- pyOCD reports an unexpected target;
- a board repeatedly resets, overheats, draws abnormal current, or disappears from USB;
- serial output is corrupt at the expected 115200 baud;
- counters freeze while the task appears alive;
- an unreviewed command would mass-erase or flash a board;
- another process/session is already flashing.

Preserve logs and the exact failing command. Do not repeatedly mass-erase/reflash as a substitute for diagnosis.
