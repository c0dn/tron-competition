# BLE mesh lane brief

Lane: BLE mesh
Worktree: /home/lucas/Workspace/tron-worktrees/ble-mesh
Status: continuing OpenCode main-agent session to be started now

Context grounding required before planning/execution:
1. Read `../admin-canonical/research_microbit_v2.md`
2. Read `../admin-canonical/research_takeaways_for_planning.md`
3. Read `../admin-canonical/FLASH_GATING_POLICY.md`
4. Inspect the actual TRON codebase under this worktree, especially `microbit/`

Interpretation constraints:
- Stock micro:bit v2 BLE / S113 is not a sufficient default BLE mesh assumption.
- Treat BLE mesh as a custom-firmware / alternate-stack lane unless proven otherwise.
- Detector behavior target: passive listening role plus controlled tx timing where feasible; no claim of true simultaneous tx/rx on one radio.
- Interleaving/scheduling is acceptable and realistic.
- Prefer external specifications and reference implementations where appropriate; clone/reference specs and align the implementation to radio reality.

This pass is NOT the old researcher-only pass.
This is execution-level research to ground a continuing implementation/planning session.

Required first-pass output:
1. Codebase reality summary
2. Feasible BLE-mesh architecture options for this repo
3. Constraints that block naive implementation
4. What external specs / references should be mirrored or followed
5. Proposed execution plan skeleton for later refinement

Operational rule:
- If an agent stalls or does not produce output, nudge with `continue` until it completes.
