# ble_sniffer

Standalone receiver for MIND wearable BLE beacons, used to check that a
micro:bit is on the air and that its payload matches the wire contract.

Runs a NimBLE passive observer and prints every decoded schema-v1 advert. No
WiFi, no mesh, no MQTT — one ESP32-C3, one micro:bit, no router or broker.

## Why it is separate

The sidecar in [`../..`](../..) only makes an observation visible once it has
travelled the mesh and reached a broker, so a BLE decode fault looks identical
to a mesh or MQTT one. Flashing this instead isolates the radio link.

It also decodes independently rather than linking the sidecar's observer: a test
tool that shares code with the thing it tests cannot detect a bug in the shared
code. Both sides are written against [`shared/schema.h`](../../../shared/schema.h)
separately, so the two agreeing is real evidence.

## Build, flash, watch

Requires ESP-IDF v5.5.x (`. $IDF_PATH/export.sh`).

```bash
idf.py set-target esp32c3     # first time only
idf.py build
idf.py -p /dev/ttyACM1 flash monitor
```

Pipe through the watcher to make incidents stand out from heartbeats:

```bash
idf.py -p /dev/ttyACM1 monitor | ./watch-observer.sh
```

Pairs with `microbit/scripts/watch-wearable.sh` on the transmitting side for a
side-by-side view of an event and the beacon it produces.

## What you should see

```
I (312) sniffer: MIND BLE sniffer - schema v1, company 0xFFFF, payload 7 B
I (452) sniffer: scanning (window=144/interval=160 units, passive)
I (7204) sniffer: obs dev=1 rssi=-39 ver=1 evt=0(HEARTBEAT) conf=0 svm=979 mic=0 seq=120 prev_rx=0
```

- `dev` comes from `AdvA[4]`, the wearable's `DEVICE_ID`.
- `svm` is the accelerometer sum-vector magnitude in milli-g; roughly 1000 at
  rest is a good sanity check that the payload is being read correctly.
- `prev_rx` is how many copies of the *previous* record were heard. The wearable
  bursts an incident ~50 times, so a healthy link shows a large count; a small
  one means packets are being missed.

Adverts are filtered on `AdvA[5] == 0xC0`, company id `0xFFFF` and schema
version before being decoded. A wearable running a different schema version logs
a warning rather than being silently dropped.

Raw advert bytes are logged at debug level — set the log level to `DEBUG` to
check framing by eye:

```bash
idf.py menuconfig   # Component config -> Log -> Default level -> Debug
```

## Filtering

Only the sniffer's own output:

```bash
idf.py -p /dev/ttyACM1 monitor --print-filter="sniffer=I"
```
