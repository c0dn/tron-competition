# dashboard

Live web monitor for the MIND system — **React + TypeScript + Vite**. Subscribes
to the broker over MQTT/WebSocket, decodes the binary uplink records, and renders
wearable status (with a live accel-SVM sparkline), ESP32-C3 node/mesh health, and
a filterable / acknowledgeable incident feed.

## Setup

```bash
cd dashboard
npm install
npm run dev        # dev server with HMR at http://localhost:5173
```

Production bundle (self-contained static files for the demo laptop):

```bash
npm run build      # tsc --noEmit typecheck, then vite build -> dist/
npm run preview    # serve the built dist/ locally
```

Open the app, click the **⚙** (top-right) to open the connection drawer, set the
broker WebSocket URL (defaults to `ws://<page-host>:9001`), and Connect. The
broker (`../infra`) must have its WebSocket listener up on 9001. The URL is
remembered in `localStorage`.

## Structure

```text
src/
  main.tsx                 app entry
  App.tsx                  wiring: store + MQTT hook + layout
  styles.css               dark theme
  lib/
    uplink.ts              typed decoder — mirror of ../../shared/uplink_schema.h
    useMqtt.ts             connect/subscribe/dispatch hook (MQTT.js over WS)
    format.ts              age / uptime / heap formatters
  state/
    store.ts               reducer: wearables, nodes, incidents (+ SVM history)
  components/
    Header.tsx  ConnectionDrawer.tsx
    WearablePanel.tsx  WearableCard.tsx  Sparkline.tsx
    NodePanel.tsx  NodeCard.tsx
    IncidentFeed.tsx
```

`src/lib/uplink.ts` is the JS-side of the binary contract — its byte offsets
must stay in lock-step with `../../shared/uplink_schema.h` (a size mismatch
throws). Verified byte-for-byte against records emitted from the C structs.

## Topics consumed

- `mind/ingest/event` — binary `mind_uplink_event_t` (wearable observations).
- `mind/ingest/status` — binary `mind_uplink_status_t` (node/mesh health).
- `mind/node/<id>/lwt` — retained `online`/`offline` presence (root only).

Node online/offline is derived from status-record freshness (all nodes) and the
retained LWT (the root). Incident severity ≥2 (falls / distress) drives the color
coding, the pulsing critical cards, and the incident feed.

## Features

- **Connection drawer** — set/change the broker URL, live connection state, reconnect.
- **Wearable cards** — event badge, confidence / SVM / mic metrics, a rolling
  accel-SVM **sparkline**, observing node + RSSI, last-seen; critical events pulse.
- **Node cards** — role (root/leaf), mesh level, children, parent RSSI, heap,
  uptime, online/offline dot.
- **Incident feed** — filter (all / critical / unacked), acknowledge individual
  incidents or all, clear acknowledged.
