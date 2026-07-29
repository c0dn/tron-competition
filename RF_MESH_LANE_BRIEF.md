# RF mesh lane brief

Lane: RF mesh
Worktree: /home/lucas/Workspace/tron-worktrees/rf-mesh
Status: continuing OpenCode main-agent session to be started now

Context grounding required before planning/execution:
1. Read `../admin-canonical/research_microbit_v2.md`
2. Read `../admin-canonical/research_takeaways_for_planning.md`
3. Read `../admin-canonical/FLASH_GATING_POLICY.md`
4. Inspect the actual TRON codebase under this worktree, especially `microbit/`

Interpretation constraints:
- RF mesh is expected to need a deeper design pass with user comments later.
- This session should begin with execution-level research grounded in the codebase and the first research pass.
- Treat micro:bit proprietary radio semantics as the likely starting point, not BLE mesh assumptions.
- Focus on what is realistic in the current repo and what additional protocol/design work is needed.

This pass is NOT the old researcher-only pass.
This is execution-level research to ground a continuing implementation/planning session.

Required first-pass output:
1. Codebase reality summary
2. Feasible RF-mesh architecture options for this repo
3. Constraints, missing protocol pieces, and likely design tradeoffs
4. What must be decided by the user before implementation is safe to proceed
5. Proposed execution plan skeleton for later refinement

Operational rule:
- If an agent stalls or does not produce output, nudge with `continue` until it completes.
