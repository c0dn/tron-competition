# Multi-lane continuation note

The initial researcher-only OpenCode pass is complete and should not be reused.
From this point onward:
- BLE mesh lane gets a continuing main-agent session beginning with execution-level research.
- RF mesh lane gets a continuing main-agent session beginning with execution-level research.
- esp-c3 remains deferred.
- Hermes supervises later checker/flash gates.

User clarification incorporated:
- Criterion 3 means passive detector listening plus controlled tx scheduling where feasible.
- True simultaneous tx/rx is not assumed; realistic BLE behavior is time-sliced/interleaved.

Operational rule for every continued OpenCode session:
- If an agent stalls or does not produce output, nudge with `continue` until it completes.
