# tron-competition

Monorepo for the TRON competition project.

## Layout

```text
.
├── microbit/       # micro:bit v2 (nRF52833) firmware workspace (uT-Kernel 3.0)
│   └── libs/mtkernel_3/   # uT-Kernel 3.0 (git submodule)
└── esp-sidecar/    # ESP32-C3 sidecar (ESP-IDF/FreeRTOS bring-up)
```

The `microbit/` workspace is based on the public template
[`c0dn/microbit-tron`](https://github.com/c0dn/microbit-tron); this private
repo carries the competition-specific firmware and tooling.

## Clone (with submodule)

```bash
git clone --recurse-submodules <this-repo-url>
```

If you already cloned without `--recurse-submodules`:

```bash
git submodule update --init --recursive
```

## Host tools

```bash
uv tool install pyocd grabserial
```

Also required: `arm-none-eabi-gcc`, `cmake`, `ninja`.

## Build & flash (micro:bit)

```bash
cd microbit
./build.sh ble_observer            # or: ble_beacon, test_firmware
./flash.sh ble_observer --monitor  # flash + serial monitor @ 115200
```

`flash.sh` auto-detects the serial port. With multiple boards connected it
prompts once and can select by probe UID:

```bash
./flash.sh ble_observer --uid <probe-uid> --monitor
./flash.sh ble_beacon --device /dev/ttyACM1 --monitor
```

See [`microbit/README.md`](microbit/README.md) for full firmware details.
