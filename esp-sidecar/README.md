# esp-sidecar

ESP32-C3 mesh sidecar for the MIND system (Multimodal Incident Detection,
TRON 2026). Each unit is a **BLE observer + ESP-MESH-LITE node**: it passively
scans for micro:bit wearable beacons ([`../shared/schema.h`](../shared/schema.h)),
de-duplicates them, and forwards each observation over a self-organizing WiFi
mesh. The mesh **root** republishes every record over MQTT to the dashboard.

```
micro:bit  --BLE (schema.h)-->  C3 leaf  --mesh raw (uplink_schema.h)-->  C3 root  --MQTT-->  broker --WS--> dashboard
```

One firmware image runs on every unit; the **root/leaf role is elected at
runtime** by mesh-lite — no per-role build. See the pipeline design in
`../shared/uplink_schema.h` and the broker/dashboard under `../infra` and
`../dashboard`.

## Architecture (components)

| Component | Role |
|---|---|
| `main/` | NVS + netif + event loop, then starts the BLE observer and the uplink. |
| `components/mind_ble` | NimBLE passive observer. Decodes schema-v1 MSD adverts, filters by AdvA (`0xC0`) + company id (`0xFFFF`) + schema version, de-dups the wearable's incident burst by `(device_id, event_type, seq)`, and queues `mind_observation_t`. |
| `components/mind_mesh` | ESP-MESH-LITE bring-up (WiFi AP+STA, router config), role helpers, node→root raw send, and the root-side raw-record handler. |
| `components/mind_uplink` | Forward-or-publish: leaf sends records to root over the mesh; root runs the MQTT client and republishes all records (own + descendants'). Periodic node-status. |
| `components/mind_shared` | Interface-only: exposes `../../shared` (`schema.h`, `uplink_schema.h`) as an include path. |

## Target

ESP32-C3 SuperMini (4 MB flash), native USB Serial/JTAG over USB-C — flashing
and the serial console share the one port. Console is routed to USB Serial/JTAG
(`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`).

> **Single radio:** the C3 shares one 2.4 GHz radio between WiFi (mesh) and BLE.
> `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` is set, and the BLE scan window (30 ms)
> is kept below the scan interval (100 ms) so the mesh is not starved. Tune in
> `components/mind_ble/mind_ble.c` if beacons or MQTT drop under load.

## Requirements

- ESP-IDF **v5.5.x**. Managed dependency `espressif/mesh_lite` (pulls in
  `iot_bridge`) is fetched automatically on first build from
  `components/mind_mesh/idf_component.yml`.

Activate ESP-IDF in your shell first (`. $IDF_PATH/export.sh`).

## Configure (per unit)

```bash
idf.py set-target esp32c3        # first time only
idf.py menuconfig                # → "MIND Sidecar Configuration"
```

Set, under that menu:

- **Node ID** — a distinct `1..254` per flashed unit (stamped into every record).
- **WiFi router** SSID / password — the LAN the broker lives on.
- **Mesh SoftAP** SSID prefix / password — shared by all nodes.
- **MQTT broker URI** — `mqtt://<broker-host-ip>:1883`.

(Defaults live in `main/Kconfig.projbuild`; overrides persist in `sdkconfig`.)

## Build, flash & monitor

```bash
idf.py build
idf.py -p <PORT> flash monitor      # e.g. /dev/ttyACM0 ; exit with Ctrl-]
```

Flash ≥2 units with different Node IDs to form a mesh. If a board does not
enumerate, force download mode: hold BOOT → tap RST → release BOOT.

## What you should see

- Each node logs `mind` boot, `mind_ble` scanning, and `mind_uplink` started.
- Bring a powered micro:bit near a node → its incidents/heartbeats are decoded
  and forwarded; the node that becomes root logs `MQTT connected`.
- Subscribe from the broker host to watch records arrive:
  ```bash
  mosquitto_sub -h <broker-host> -t 'mind/ingest/#' -v          # binary
  mosquitto_sub -h <broker-host> -t 'mind/node/+/lwt' -v        # presence
  ```

## Verify-on-hardware checklist

The binary wire contract (C ↔ dashboard JS) and the broker/WebSocket path are
already validated off-hardware. On real boards, confirm in order:

1. One node decodes a wearable (serial log), incl. burst de-dup.
2. Two nodes form a mesh (root election; a leaf reports `mesh_level == 2`).
3. Root publishes to the broker (`mosquitto_sub`).
4. A wearable in range of a **leaf** still reaches the broker (aggregate-at-root).
5. Dashboard updates live; a powered-off root flips **offline** via LWT.
6. ~10-min soak: steady beacon reception + stable MQTT (coexistence).

> **API note:** the mesh-lite raw-message and SoftAP setters are coded against
> the current upstream API. If a call name/struct field differs on your pinned
> `mesh_lite` release, adjust in `components/mind_mesh/mind_mesh.c` (the one
> `NOTE(verify)` marks the SoftAP setter).
