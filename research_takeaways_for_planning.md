# TRON research takeaways for planning

## Planning-impact conclusions

1. **Stock BLE mesh on micro:bit v2 is a bad default assumption**
   - Stock micro:bit BLE uses Nordic S113, which is peripheral-only + broadcaster.
   - Bluetooth Mesh expects broadcaster + observer behavior.
   - Therefore the BLE mesh lane should be treated as a **custom firmware / alternate stack lane**, likely involving Zephyr/NCS-style ideas, not a trivial extension of the current stock micro:bit BLE setup.

2. **Advertise + listen is possible only by scheduling/interleaving**
   - One radio, no true simultaneous tx/rx.
   - The no-backbone success criterion should be interpreted as successful operational interleaving under duty-cycle constraints, not literal concurrent full-duplex behavior.

3. **RF mesh is architecturally simpler on stock micro:bit semantics**
   - The built-in micro:bit radio is proprietary 2.4 GHz and not BLE.
   - No built-in mesh, no encryption, runtime-specific payload constraints.
   - But it is the more natural lane for micro:bit-first experimentation because it avoids immediate SoftDevice / BLE-role conflicts.

4. **Unattended flashing is conditionally plausible, but only through the safe lane**
   - Prefer target flashing via `MICROBIT`/CMSIS-DAP.
   - Avoid unattended `MAINTENANCE` interface firmware updates.
   - Avoid mass erase / AP-Protect / recover flows unless explicitly intended.
   - Verify board revision and target/probe identity before any automated flash.

5. **Safety-critical board constraints must shape the prompts**
   - 3.6 V board limit.
   - No 5 V-tolerant GPIO assumptions.
   - Avoid inductive loads/back-powering.
   - Shared pins can create unintended interactions.

## Prompting implications for next OpenCode planning step

- BLE mesh lane prompt should explicitly recognize stock-S113 limitations and ask for a plan that does not hand-wave observer/scanning support.
- RF mesh lane prompt should focus on application-layer flood/relay design, dedupe, queueing, and stress testing over the built-in micro:bit radio.
- No-backbone tx/rx+beacon lane prompt should explicitly define success as **scheduled interleaving** unless the planner produces evidence for anything stronger.
- Every planning/execution prompt must emphasize:
  - if an agent stalls or does not produce output, nudge with `continue` until it completes
  - supervise/verify plan-checker rather than assuming it is absent
  - code-checker gate before any flash due to hard-brick/hardware-safety concerns
