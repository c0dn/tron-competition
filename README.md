# TRON competition

Competition proof of concept: a wearable broadcasts incidents, micro:bit v2
backbone nodes forward them over BLE/TAVRN, and one serial-connected **Gateway**
feeds a localhost dashboard. The dashboard deduplicates packets, shows the
Gateway GTT roster on a persistent floorplan, and draws ballpark locations from
each positioned observer's received RSSI. One observer is enough for a marker;
additional observers contribute to a weighted centroid. This is **not**
distance-based trilateration or calibrated indoor positioning.

## Get started

You need micro:bit v2 boards (at least one backbone node and one wearable), a
USB connection to each board while flashing, and a Linux PC for the Gateway.
Install `arm-none-eabi-gcc`, `cmake`, `ninja`, `pyocd`, Python 3, `uv`, and Bun.
`grabserial` is optional for serial monitoring. The kernel is a Git submodule:

```bash
git clone --recurse-submodules https://github.com/c0dn/tron-competition
cd tron-competition
```

If you already cloned without the submodule, run
`git submodule update --init --recursive` before building.

### Flash the backbone nodes

From `microbit/`, repeat the following for each backbone board. Identify its
**full probe UID** with `pyocd list` and assign each active node a distinct
application number from 1 through 6. The Gateway is not a separate firmware
image: choose the board you will later connect to the dashboard PC. These
development builds use each board's own FICR radio identity; do not reuse a
board's probe UID for another board.

```bash
cd microbit
pyocd list
NODE_UID='paste-the-full-probe-UID-for-this-board'
NODE_NUMBER=1
./build-tavrn-ble.sh --target tavrn_routed_node \
  --feature FULL_TAVRN --repair ON --wearable-ingress ON \
  --timer BALANCED --app-node-number "$NODE_NUMBER" \
  --out "artifacts/node-$NODE_NUMBER"

# Copy the exact path printed as "Published ELF" by the build above.
NODE_ELF='/absolute/path/to/the-published-node.elf'
pyocd load "$NODE_ELF" --uid "$NODE_UID" --target nrf52833
pyocd reset --uid "$NODE_UID" --target nrf52833
```

**Confirm the UID before `pyocd load`: it programs that physical board.**
`artifacts/` is ignored by Git; use a different output directory for each node.
The checked-in [six-board UID/AdvA inventory](microbit/hardware-results/2026-08-11-tavrn-six-board-inventory.tsv)
describes the project's original boards, not yours. Do **not** use
`./flash.sh tavrn_routed_node` here: that shortcut rebuilds the `AODV_ONLY`
profile instead of the FULL_TAVRN wearable-ingress image above.

### Flash the wearable

Use a **different** probe UID for the wearable. This command rebuilds the
production wearable, mass-erases that board, loads its ELF, and resets it:

```bash
WEARABLE_UID='paste-the-full-probe-UID-for-the-wearable'
./flash.sh wearable_app --uid "$WEARABLE_UID"
```

For a synthetic fall-and-shout demo instead of waiting for a real incident,
flash `wearable_test_injector` on the wearable with the same `--uid` option.
That replaces the production wearable firmware on that board. Never substitute
a backbone UID for the wearable UID. See [microbit/README.md](microbit/README.md)
for the other firmware targets and build details.

### Start the dashboard

Connect **only the chosen Gateway** to the PC over serial. From the repository
root (`cd ..` once if you are still in `microbit/`), install the bridge image
dependency and frozen dashboard packages:

```bash
uv venv .venv
uv pip install --python .venv/bin/python -r host/requirements.txt
(
  cd dashboard
  bun install --frozen-lockfile
  bun run build
)
GATEWAY_SERIAL='/dev/serial/by-id/paste-your-gateway-link'
.venv/bin/python host/bridge.py --serial "$GATEWAY_SERIAL" --assets dashboard/dist
```

Open **http://127.0.0.1:8787/** on that PC. Turn the Gateway on in the UI,
refresh its GTT, upload a floorplan, and position the listed nodes. A received
wearable incident appears once even if several nodes report it; after the
two-second collection window, positioned observers with RSSI can produce a
ballpark marker. No coordinates are shown without a positioned observer.

The bridge requires exactly one `--serial` path and binds only to loopback.
Other Host values and cross-origin browser mutations are rejected. Floorplan
data is stored in
`${XDG_DATA_HOME:-$HOME/.local/share}/tron-dashboard/dashboard.sqlite3`;
use `--state-dir /absolute/persistent/path` to choose another local directory.
The state directory has mode `0700`, and the SQLite file has mode `0600`.
