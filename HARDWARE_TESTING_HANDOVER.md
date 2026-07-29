# TRON hardware-testing handover

The complete cross-lane handover is maintained on branch `admin/canonical`:

```bash
git fetch origin
git show origin/admin/canonical:HARDWARE_TESTING_HANDOVER.md
```

For this `exp/rf-mesh` branch, follow the handover's **Safety gate**, **RF hardware test**, **Evidence to save**, and **Stop conditions** sections.

Current verification before handoff:

```text
RF mesh core host tests passed
rf_mesh_node firmware target built successfully
RF role-build safety tests passed
```

Build role images only with `microbit/build-rf-role.sh`; it uses a clean build
directory and emits role/node-labelled artifacts plus manifests.

No board was connected or flashed, and no RF/runtime claim has been proven. V2 has no encryption/authentication and no ACK/retry; `tx_attempt` is not confirmed delivery.
