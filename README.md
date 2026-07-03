# tron-competition

Monorepo for **MIND** — Multimodal Incident Detection, TRON Competition 2026.

A micro:bit wearable detects falls/shouts (zero-ML) and BLE-broadcasts a 7-byte
incident beacon. ESP32-C3 nodes form a WiFi mesh, observe the beacons, and the
mesh root republishes over MQTT to a live web dashboard.

```
micro:bit  --BLE-->  ESP32-C3 mesh (leaf → root)  --MQTT-->  broker  --WebSocket-->  dashboard
```

## Layout

```text
.
├── microbit/       # micro:bit v2 (nRF52833) wearable + BLE tools (uT-Kernel 3.0)
│   └── libs/mtkernel_3/   # uT-Kernel 3.0 (git submodule)
├── esp-sidecar/    # ESP32-C3 firmware: BLE observer + ESP-MESH-LITE + MQTT (ESP-IDF)
├── shared/         # wire contracts: schema.h (BLE) + uplink_schema.h (mesh/MQTT)
├── infra/          # Mosquitto broker (docker-compose): MQTT 1883 + WebSocket 9001
└── dashboard/      # live web monitor (MQTT.js over WebSocket, binary decode)
```

## The pipeline end to end

1. **Wearable** (`microbit/app/wearable_app`) → BLE beacon per `shared/schema.h`.
2. **ESP32-C3 nodes** (`esp-sidecar`) observe, de-dup, and forward over the mesh;
   the root publishes binary records (`shared/uplink_schema.h`) to MQTT.
3. **Broker** (`infra`) — one command: `cd infra && docker compose up`.
4. **Dashboard** (`dashboard`) — `cd dashboard && python3 -m http.server 8080`,
   then open <http://localhost:8080> and point it at `ws://<broker-host>:9001`.

Per-component detail: [`esp-sidecar/README.md`](esp-sidecar/README.md),
[`dashboard/README.md`](dashboard/README.md).

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
