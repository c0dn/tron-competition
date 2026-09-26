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

## Dashboard bridge deployment

Install the bridge image validator, then install the frozen dashboard
dependencies and build the production assets before launch:

```bash
uv venv .venv
uv pip install --python .venv/bin/python -r host/requirements.txt
(
  cd dashboard
  bun install --frozen-lockfile
  bun run build
)
```

Run the dashboard bridge with exactly one serial-connected Gateway:

```bash
.venv/bin/python host/bridge.py --serial /dev/serial/by-id/<gateway> --assets dashboard/dist
```

The bridge rejects zero or multiple `--serial` arguments before opening its
localhost HTTP server. Floorplan metadata and image bytes are stored together
in `${XDG_DATA_HOME:-$HOME/.local/share}/tron-dashboard/dashboard.sqlite3`.
An empty or relative `XDG_DATA_HOME` is ignored in favor of the fallback.
Use an explicit absolute directory when needed:

```bash
.venv/bin/python host/bridge.py --serial /dev/ttyACM0 --state-dir /var/lib/tron-dashboard
```

The state directory uses mode `0700`; `dashboard.sqlite3` uses mode `0600`.
Keep this directory on persistent local storage.

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
