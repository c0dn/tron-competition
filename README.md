# tron-competition

Monorepo for the TRON competition project.

## Layout

```text
.
├── microbit/       # micro:bit v2 (nRF52833) firmware workspace (uT-Kernel 3.0)
│   └── libs/mtkernel_3/   # uT-Kernel 3.0 (git submodule)
└── esp-sidecar/    # ESP32-C3 sidecar (ESP-IDF/FreeRTOS bring-up)
```

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

## Local dashboard bridge security

The host bridge binds only to `127.0.0.1`. It serves its dashboard and APIs
only when the request `Host` is `localhost:<actual-port>` or
`127.0.0.1:<actual-port>`; other Host values, including a different port, are
rejected. Mutation endpoints require one `application/json` Content-Type
(optionally `charset=utf-8`). Browser requests must provide the matching
`http` Origin and may only declare `Sec-Fetch-Site: same-origin` or `none`.
The bridge sends no CORS permissions. Local CLI clients remain supported when
they send JSON with no Origin or fetch-metadata headers.

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
