# TRON micro:bit v2 researcher pass

Source: OpenCode daemon-backed researcher-first run (`TRON microbit-v2 researcher pass`)
Status: persisted by Hermes from read-only researcher output
Platform focus: micro:bit v2 / nRF52833

## Executive summary

- micro:bit v2’s nRF52833 silicon is capable enough for BLE mesh-class experiments: 64 MHz Cortex-M4F, 512 KB flash, 128 KB RAM, BLE 1M/2M/Long Range-capable radio, Bluetooth mesh marketed by Nordic.
- Stock micro:bit v2 BLE is constrained by the Nordic **S113 SoftDevice**, which Nordic describes as **peripheral-only + broadcaster**. Bluetooth Mesh requires **Broadcaster + Observer** roles to advertise and scan, so stock micro:bit BLE is not a good standard BLE mesh base.
- Simultaneous advertise + scan/listen is plausible only as **time-sliced interleaving** on one radio. It is not true simultaneous TX/RX.
- The safest unattended flashing lane is normal target flashing through the `MICROBIT` drive / CMSIS-DAP. Avoid unattended `MAINTENANCE` mode interface-firmware updates.
- Main hard-brick risks: wrong interface firmware for board revision, probing/flashing the interface MCU instead of target MCU, enabling Nordic access/erase protection, overvoltage/back-powering through 3V/GPIO, and inductive loads on pins.

## Board/spec constraints

- Target MCU: **Nordic nRF52833**, **64 MHz Arm Cortex-M4F**, **512 KB flash**, **128 KB RAM**. User code, runtime, and BLE stack are one target flash image.
- Stock board radio exposure differs from silicon:
  - Silicon supports BLE, Bluetooth mesh, Thread/Zigbee/802.15.4, proprietary 2.4 GHz, +8 dBm TX per Nordic product page.
  - micro:bit runtime/docs expose BLE Tx **-40 to +4 dBm** and micro:bit radio Tx power levels **0..7 = -30 to +4 dBm**.
- Board power constraints:
  - Treat assembled micro:bit v2 as **max 3.6 V** board, even though nRF52833 silicon has wider supply modes.
  - V2 regulator can source **300 mA**; edge connector budget about **190 mA**.
  - 3V ring is raw target supply (`V_TGT`); external powering needs proper regulation and reverse/overvoltage protection.
- GPIO safety:
  - nRF52 GPIO is not 5V-tolerant; tolerable input is around **VDD + 0.3 V**, with absolute cases around **3.9 V**.
  - Avoid direct motors/speakers/inductive loads on GPIO because back-EMF can exceed pin limits.
  - Shared pins matter: LED matrix pins, buttons, I2C pins P19/P20, NFC-capable P8/P9, and accessibility pin P12 can affect board behavior if reused.

## BLE mesh findings

- Bluetooth Mesh requires an underlying BLE stack supporting **GAP Broadcaster and Observer** roles: advertise and scan for advertising packets.
- micro:bit v2 uses **Nordic S113 SoftDevice**. Nordic describes S113 as a **memory-optimized peripheral-only BLE stack** supporting up to 4 peripheral connections concurrently with a broadcaster. That does not satisfy stock BLE mesh observer/scanning needs.
- micro:bit Bluetooth docs say DAL/CODAL supports GAP Peripheral by default and Central requires changing runtime/SoftDevice. This aligns with treating stock S113 as insufficient for BLE mesh.
- BLE mesh lane planning should use:
  - Bluetooth Mesh advertising bearer / GATT bearer constraints.
  - Advertising-channel flood/relay behavior.
  - Small-message, managed-flood assumptions, not streaming assumptions.
- SoftDevice-free BLE mesh is plausible with another stack such as Zephyr/NCS, but should be treated as a custom firmware lane, not a stock micro:bit lane.

## RF / non-BLE radio findings

- micro:bit “radio” is **not BLE**. It is a proprietary 2.4 GHz micro:bit-to-micro:bit protocol built over Nordic proprietary/ShockBurst-style radio behavior.
- Stock RF radio constraints:
  - No built-in mesh.
  - No encryption.
  - Group code filtering, typically group `0..255`.
  - Runtime-dependent payload limits:
    - micro:bit hardware docs: standard **32-byte** payload, larger if reconfigured.
    - MicroPython v2: message length up to **251 bytes**, default queue **3**, channels **0..83**, 1 MHz steps from 2400 MHz, 1M/2M rates.
    - MakeCode `sendBuffer`: max **19 bytes**.
  - BLE and micro:bit radio are mutually exclusive in common DAL/MakeCode paths.
- RF mesh lane planning should therefore assume an **application-layer flood/relay protocol** with explicit duplicate suppression, queue overflow handling, collision/loss tolerance, and no BLE compatibility.

## Flashing safety findings

- Normal target flashing is relatively safe because the micro:bit has a separate interface MCU running DAPLink. Crashing target firmware should not prevent USB reflashing.
- Drive modes matter:
  - `MICROBIT` mode flashes the **target nRF52833**.
  - `MAINTENANCE` mode flashes the **interface MCU**. This is higher risk and should not be used unattended unless absolutely necessary.
- Board revision matters for interface firmware:
  - V2.00 uses KL27 interface firmware.
  - V2.20/2.21/V2.2x use nRF52820/nRF52833-family interface firmware.
  - Wrong image risks damaging recoverability.
- Useful unattended-flashing checks:
  - Read `DETAILS.TXT` for interface/build info.
  - Watch for `FAIL.TXT` and `ASSERT.TXT`.
  - Wait for remount/eject behavior after flashing.
- Probe/SWD hazards:
  - Target nRF52833 SWD: **TP11/TP12**.
  - Interface MCU debug/reset: **TP3/TP4/TP2**.
  - Probing the wrong pads can compromise DAPLink/interface recovery.
- Nordic protection hazards:
  - AP-Protect can block debugger access and require `ERASEALL`, wiping flash/UICR.
  - Do not enable access/erase protection in unattended experiments unless recovery has been proven.
  - Avoid mass-erase/recover commands unless wipe is intended.

## Open questions / unknowns

- Exact nRF52833 revision/build-code behavior for improved APPROTECT should be confirmed against the specific boards before unattended SWD automation.
- Payload/channel limits must be pinned to the actual runtime: MakeCode, MicroPython, CODAL/DAL C++, Zephyr, or NCS.
- BLE mesh on micro:bit v2 with Zephyr/NCS needs empirical validation for flash/RAM headroom and scan/advertise duty-cycle loss.
- V2.2x low-level interface recovery is less clearly documented than V2.00 KL27 full-image recovery; normal `MAINTENANCE` update is documented.

## Annotated references list

- micro:bit hardware overview — MCU, memory, radio summaries, interface chip, current budget:
  https://tech.microbit.org/hardware/
- micro:bit Bluetooth docs — S113 SoftDevice, default GAP role, BLE vs radio distinction, MakeCode Bluetooth/radio exclusivity:
  https://tech.microbit.org/bluetooth/
- Nordic S113 SoftDevice — peripheral-only + broadcaster constraint:
  https://www.nordicsemi.com/Products/Development-software/S113
- Nordic nRF52833 product page — silicon BLE mesh / Long Range / 802.15.4 / flash/RAM capability baseline:
  https://www.nordicsemi.com/Products/nRF52833
- Bluetooth Mesh FAQ — requires Broadcaster + Observer roles; small-message mesh assumptions:
  https://www.bluetooth.com/learn-about-bluetooth/feature-enhancements/mesh/mesh-faq/
- Bluetooth Mesh Protocol spec — advertising and GATT bearers:
  https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/MshPRT_v1.1/out/en/index-en.html
- Nordic DevZone scan/advertise scheduling answer — one radio; scan and advertise collide and are scheduled:
  https://devzone.nordicsemi.com/f/nordic-q-a/61588/scanning-multiple-advertisement-varying-payloads
- Zephyr scan + advertise sample — SoftDevice-free interleaving evidence:
  https://docs.zephyrproject.org/latest/samples/bluetooth/scan_adv/README.html
- Zephyr Bluetooth mesh sample — BLE mesh firmware lane reference:
  https://docs.zephyrproject.org/latest/samples/bluetooth/mesh/README.html
- micro:bit DAL radio docs — no BLE coexistence, no mesh, no encryption:
  https://lancaster-university.github.io/microbit-docs/ubit/radio/
- MicroPython v2 radio docs — queue, payload, channel, power, data-rate constraints:
  https://microbit-micropython.readthedocs.io/en/v2-docs/radio.html
- MakeCode radio sendBuffer — 19-byte MakeCode payload limit:
  https://makecode.microbit.org/reference/radio/send-buffer
- micro:bit power supply docs — voltage/current/back-powering constraints:
  https://tech.microbit.org/hardware/powersupply/
- micro:bit edge connector docs — GPIO tolerances and shared-pin hazards:
  https://tech.microbit.org/hardware/edgeconnector/
- micro:bit schematic/test-point docs — target vs interface SWD pads:
  https://tech.microbit.org/hardware/schematic/
- micro:bit DAPLink/interface docs — `MICROBIT` vs `MAINTENANCE`, status files, recovery cautions:
  https://tech.microbit.org/software/daplink-interface/
- micro:bit firmware guide — board-revision-specific interface firmware update process:
  https://microbit.org/get-started/user-guide/firmware/
