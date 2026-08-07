# TRON hardware-testing handover

The complete cross-lane handover is maintained on branch `admin/canonical`:

```bash
git fetch origin
git show origin/admin/canonical:HARDWARE_TESTING_HANDOVER.md
```

For this `exp/ble-mesh` branch, follow the handover's **Safety gate**, **BLE hardware test**, **Evidence to save**, and **Stop conditions** sections.

Current verification before handoff:

```text
16 TRON mesh packet host tests passed
BLE radio driver host tests passed
BLE mesh scheduler host tests passed
ble_mesh_node firmware target built successfully
```

No board was connected or flashed, and no RF/runtime claim has been proven. This transport is custom and lab-only; it is **not Bluetooth Mesh compliant**.
