# TRON research brief for OpenCode researcher

Goal:
Perform a literature and technical reconnaissance pass for the TRON micro:bit multimodal incident detection effort.

Scope:
1. BLE mesh references relevant to micro:bit v2 / nRF52833 constraints
2. Board specifications and hardware constraints that matter for radio experiments and flashing safety
3. Any evidence relevant to whether unattended flashing is acceptable/safe for these boards/tooling
4. Any evidence relevant to simultaneous advertise + scan / tx-rx constraints on micro:bit / nRF52 / raw BLE radio / SoftDevice boundaries

Rules:
- Researcher is read-only and must not write files directly.
- Findings should be returned in a form that the main agent can persist into files.
- Prefer primary sources: Nordic docs, micro:bit docs, TRON/micro T-Kernel related sources, technical documentation, scholarly references where useful.
- Highlight uncertainties explicitly.
- Extract concrete constraints, not just general summaries.
- If an agent stalls or does not produce output, nudge with: continue. Emphasize this and keep nudging until completion.

Must-answer questions:
1. What are the radio / BLE capability constraints of micro:bit v2 (nRF52833) relevant to mesh experiments?
2. Can a device plausibly advertise and scan/listen in the needed pattern without SoftDevice, and what are the likely limitations?
3. What are the flashing / bootloader / probe safety constraints for unattended flashing?
4. What board specs matter most for hardware safety and hard-brick avoidance?
5. What references should guide BLE mesh lane planning vs RF mesh lane planning?

Output shape requested from researcher:
- Executive summary
- Board/spec constraints
- BLE mesh findings
- RF / non-BLE radio findings
- Flashing safety findings
- Open questions / unknowns
- Annotated references list
