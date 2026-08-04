# Protocol host tests

Protocol behavior tests must run on the host without flashing hardware. The
packet test harness command reserved by the approved plan is:

```bash
./tests/protocol/run_tron_mesh_packet_tests.sh
```

If the scheduler slice adds a host/simulation harness, use:

```bash
./tests/protocol/run_ble_mesh_scheduler_tests.sh
```

Duplicate suppression has its own harness:

```bash
./tests/protocol/run_tron_mesh_dedupe_tests.sh
```

Addressed PING/PONG builders, validation, pending correlation, periodic timing,
timeouts, and 32-bit deadline wrap have a deterministic pure-C harness:

```bash
./tests/protocol/run_tron_mesh_pingpong_tests.sh
```

The dedupe cache originally lived inside `app/ble_mesh_node/src/main.c`, which
cannot be compiled on the host, so it shipped untested and silently degraded to
a single usable slot. It now lives in `app/protocol/` for the same reason the
packet and scheduler modules do.

Firmware-integrated tests are not considered executed unless hardware is flashed,
which is outside this non-hardware execution scope.
