# RF decision questionnaire

Lane: RF mesh
Worktree: /home/lucas/Workspace/tron-worktrees/rf-mesh

Updated user guidance:
- RF mesh can be based on **AODV-style/root-aware routing**.
- The application layer can discover topology via flood/broadcast.
- A sane design is sufficient; protocol is not final.
- Payloads should be dummy payloads for now.
- The schema is now approved by the user and may be treated as authoritative if found locally or upstream.
- If a schema/spec file is found, use it as the current protocol authority, while still noting any implementation gaps against hardware reality.

Before RF implementation planning should proceed past protocol design, the user should answer/refine:

1. Compatibility target
- project-owned proprietary RF packet format
- compatible with MakeCode/MicroPython-style micro:bit radio semantics

2. First payload type
- use dummy payloads for now
- likely heartbeat/discovery style until protocol stabilizes

3. Initial test topology
- number of boards
- sender / relay / receiver roles
- success criteria for range / latency / loss

4. Radio parameters
- channel/frequency
- network/group ID
- 1M vs 2Mbit
- TX power
- fixed vs configurable payload size

5. Routing / flood policy
- AODV-like route awareness vs pure flooding baseline
- TTL
- relay once vs probabilistic relay
- retry count
- random backoff window
- root election or fixed root

6. Security expectation
- no encryption/authentication for prototype
- lightweight authentication only
- defer security to later lane

7. Flash behavior
- should `flash.sh` be patched to remove/guard `erase --mass` before any hardware phase?

8. Scope boundary
- is ESP sidecar fully out of scope for RF mesh right now?

Operational rule for future OpenCode sessions:
- If an agent stalls or does not produce output, nudge with `continue` until it completes.
