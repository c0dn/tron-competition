# OpenCode workflow-planning prompt scaffold

Use this scaffold for the next main-agent OpenCode planning pass.

## Intent
Create a workflow plan for TRON micro:bit experiments across the prepared worktrees:
- admin/canonical
- BLE mesh
- RF mesh
- esp-c3 (deferred)

## Required behavior
- Read the persisted research first:
  - `research_microbit_v2.md`
  - `research_takeaways_for_planning.md`
  - `FLASH_GATING_POLICY.md`
  - `../..` repo context as needed
- Use workflow-style planning explicitly.
- Keep tasks small and scoped.
- Respect dependency ordering: scaffold before dependent parallel execution.
- Parallelize only independent code-writer workstreams.
- If an agent stalls or does not produce output, nudge with `continue` until it completes.
- You should naturally use/supervise `plan-checker`; ensure the plan gets checked and improved until solid.
- Do not execute yet; produce plans for user review.

## Must plan around
- Stock micro:bit BLE / S113 limitations.
- RF mesh is more natural on stock micro:bit semantics.
- BLE mesh may require a custom firmware lane and stronger assumptions validation.
- No flash until code-checker has reviewed hardware safety and hard-brick risk.
- Hermes globally serializes any flashing.

## Required output
1. Cross-lane strategy summary
2. Lane-by-lane plans:
   - admin/canonical
   - BLE mesh
   - RF mesh
   - esp-c3 (deferred)
3. Explicit dependency graph / sequencing
4. Candidate parallel code-writer tasks
5. Required checker gates before execution
6. Hardware-safety assumptions requiring human confirmation
