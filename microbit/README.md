# microbit-tron

micro:bit v2 firmware workspace for the TRON competition.

This repo builds one or more firmware targets against `μT-Kernel 3.0`, which is pulled in as a git submodule under `libs/mtkernel_3`.

## Repo purpose

- keep competition firmware code separate from the kernel source
- support multiple firmware targets under `app/`
- keep build and flash commands simple

## Layout

```text
.
├── app/
│   └── test-firmware/
│       ├── CMakeLists.txt
│       ├── config/
│       │   └── config.h
│       └── src/
│           └── app_main.c
├── libs/
│   └── mtkernel_3/    # git submodule
├── cmake/
├── build.sh
├── flash.sh
├── serial.sh
└── clean.sh
```

## Clone

```bash
git clone https://github.com/c0dn/microbit-tron.git
cd microbit-tron
git submodule update --init --recursive
```

## Tools

Install the host tools with `uv`:

```bash
uv tool install pyocd
uv tool install grabserial
```

You also need:

- `arm-none-eabi-gcc`
- `cmake`
- `ninja`

Generic firmware builds require a compatible ARM `arm-none-eabi-gcc`; they do
not require one exact workstation package revision.

### D2 resource-contract toolchain (Arch Linux)

The D2 current-resource gate is intentionally stricter than a generic build.
It binds declared stack leaves to the compiler and target archives that produced
the evidence. The supported workstation contract is:

- `arm-none-eabi-gcc 16.2.0-1.1` (`arm-none-eabi-gcc (Arch Repository) 16.2.0`)
- `arm-none-eabi-newlib 4.6.0.20260123-1`
- compiler `/usr/bin/arm-none-eabi-gcc`, SHA-256
  `04d818b91fff08550e414c23704cc2344ae8568a896951e5772c722210c99395`
- Cortex-M4 hard-float libgcc
  `/usr/lib/gcc/arm-none-eabi/16.2.0/thumb/v7e-m+fp/hard/libgcc.a`, SHA-256
  `3885a8d6661a68ce1da5b6586a54b5beb796df13e6dfaa40c1708f9392620d87`
- hard-float newlib libg
  `/usr/lib/gcc/arm-none-eabi/16.2.0/../../../../arm-none-eabi/lib/thumb/v7e-m+fp/hard/libg.a`,
  SHA-256 `a09944e71b329a81e34948a47108f71fa185c3a225823284b1dd1976da852659`

The D2 `required-*-stack-edges.json` evidence is compiler/archive-bound, not a
permissive compatibility check. On a compiler, libgcc, or newlib upgrade,
regenerate and review the D2 resource evidence (including frame effects) before
updating the frozen paths and hashes; do not weaken provenance checks to accept
the new toolchain automatically.

## Build

Build the default firmware:

```bash
./build.sh
```

Or build a specific target:

```bash
./build.sh test_firmware
```

The build and flash entrypoints support the legacy targets `test_firmware`,
`ble_observer`, `ble_beacon`, `ble_mesh_node`, `wearable_app`, and
`wearable_test_injector`. They also select the required profile for
`ble_link_v2_testbed` and `tavrn_routed_node`.

Output goes to:

```text
build/firmware/<target>/
```

For example:

```text
build/firmware/test_firmware/test_firmware.elf
```

## Flash

```bash
./flash.sh test_firmware
```

That runs:

- build
- `pyocd erase --mass`
- `pyocd load ...`
- `pyocd reset`

## Serial

```bash
./serial.sh /dev/ttyACM0
```

Or flash and then immediately monitor:

```bash
./flash.sh test_firmware --device /dev/ttyACM0 --monitor
```

## Add another firmware

Create a new folder under `app/`, for example:

```text
app/my-firmware/
├── CMakeLists.txt
├── config/
└── src/
```

Use `app/test-firmware/` as the starting template.

## Notes

- target board: **micro:bit v2 / nRF52833**
- each firmware can use its own `config/config.h`
- the upstream make-based build still exists inside `libs/mtkernel_3/build_make/`
