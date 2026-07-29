# Flash gating policy

Hermes is the global flash gatekeeper for TRON experiments.

Mandatory rules before any flash:
1. The relevant OpenCode session must explicitly run a code-checker review focused on:
   - hardware safety
   - hard-brick risk
   - wrong-target / wrong-probe risk
   - flash-script assumptions
2. Hermes must review the proposed flash command and target.
3. Hermes must ensure no other OpenCode or Hermes session is flashing at the same time.
4. Prefer probe-UID-targeted flashing over ambiguous device-path-only flashing when multiple boards may exist.
5. If safety assumptions are unclear, do not flash.

Current status:
- Devices unplugged
- Research/planning only

Operational reminder:
- If an agent stalls or does not produce output, nudge with: continue
