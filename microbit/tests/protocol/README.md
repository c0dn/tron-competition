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

Firmware-integrated tests are not considered executed unless hardware is flashed,
which is outside this non-hardware execution scope.
