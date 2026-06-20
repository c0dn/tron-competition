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

## Build

Build the default firmware:

```bash
./build.sh
```

Or build a specific target:

```bash
./build.sh test_firmware
```

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
