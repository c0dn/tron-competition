# dashboard

Live web monitor for the MIND system. Subscribes to the broker over
MQTT/WebSocket, decodes the binary uplink records, and renders wearable status,
ESP32-C3 node/mesh health, and an incident feed.

No build step, no framework, no database — plain HTML/JS. `MQTT.js` is vendored
under `vendor/` so it works offline (competition demo).

## Files

| File | Role |
|---|---|
| `index.html` | Layout + styling; loads `vendor/mqtt.min.js` then `app.js`. |
| `binaryReader.js` | JS mirror of `../shared/uplink_schema.h` (`DataView`, little-endian). Byte offsets must stay in lock-step with the header. |
| `app.js` | MQTT connect/subscribe, state, and rendering. |
| `vendor/mqtt.min.js` | Vendored MQTT.js v5 (browser bundle). |

## Run

Serve the folder over HTTP (module scripts don't load from `file://`):

```bash
cd dashboard
python3 -m http.server 8080
```

Open <http://localhost:8080>. In the header, set the broker WebSocket URL
(defaults to `ws://<page-host>:9001`) and click **Connect**. The broker
(`../infra`) must have its WebSocket listener up on 9001.

## Topics consumed

- `mind/ingest/event` — binary `mind_uplink_event_t` (wearable observations).
- `mind/ingest/status` — binary `mind_uplink_status_t` (node/mesh health).
- `mind/node/<id>/lwt` — retained `online`/`offline` presence (root only).

Node online/offline is derived from status-record freshness (all nodes) and the
retained LWT (the root). Incident severity ≥2 (falls / distress) drives the
color coding and the incident feed.
