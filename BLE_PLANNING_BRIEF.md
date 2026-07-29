# BLE planning brief for next OpenCode pass

Lane: BLE mesh
Worktree: /home/lucas/Workspace/tron-worktrees/ble-mesh

Current conclusion:
Proceed on the assumption that the near-term implementation target is a **near-spec BLE advertising mesh-like transport**, referenced against the official Bluetooth Mesh spec where reasonably possible, without requiring full Bluetooth Mesh compliance.

Grounding documents to read first:
1. `BLE_MESH_LANE_BRIEF.md`
2. `../admin-canonical/research_microbit_v2.md`
3. `../admin-canonical/research_takeaways_for_planning.md`
4. `research_ble_mesh_execution_pass.md`
5. `../admin-canonical/FLASH_GATING_POLICY.md`

Updated design guidance from user:
- BLE mesh does not need to be a full mesh implementation.
- It should follow the official Bluetooth Mesh spec as far as reasonably possible.
- It may drop unnecessary or impossible-to-support features.
- Keep as much spec-shape as practical while remaining honest about non-compliance.
- Protocol/payload details are not final yet, so use dummy payloads for now.
- The schema is now approved by the user and may be treated as authoritative if found locally or upstream.
- If a schema/spec file is found, use it as the current protocol authority, while still noting any implementation gaps against hardware reality.

Mandatory planning posture:
- Use workflow planning explicitly.
- Keep tasks small and scoped.
- Respect dependencies before parallel code-writer execution.
- Supervise/verify that `plan-checker` ran and the plan became solid.
- If an agent stalls or does not produce output, nudge with `continue` until it completes.

Planning target:
Produce an execution-ready plan for implementing a combined detector/mesh node on top of the current raw BLE advertising/scanning code.

The plan should include:
1. build-baseline restoration steps
2. packet-layer design tasks
3. RX/TX scheduler/refactor tasks
4. new target/app creation tasks (`ble_mesh_node` or equivalent)
5. parallel code-writer subtasks where safe
6. code-checker gates before any hardware-related step
7. explicit no-flash-until-Hermes gate
