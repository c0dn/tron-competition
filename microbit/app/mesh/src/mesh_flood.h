/*
 * mesh_flood.h - controlled flood mesh for MIND wearable beacons.
 *
 * WHAT PROBLEM THIS SOLVES
 * ------------------------
 * A wearable advertises at 0 dBm and the ESP32-C3 hears it out to roughly
 * 20-30 m indoors. Past that the incident is simply lost - there is no retry,
 * because ADV_NONCONN_IND is unacknowledged broadcast. This makes every other
 * wearable in between a potential repeater, so coverage becomes a function of
 * how many people are wearing one rather than how far the C3 can hear.
 *
 * The scheme is a controlled flood: every node rebroadcasts each message it has
 * not seen before, at most TTL hops from the origin, after a random backoff.
 * There is no routing, no neighbour table and no link state - all of which
 * would need bidirectional links this radio does not have.
 *
 * TRANSPARENCY TO THE ESP32-C3
 * ----------------------------
 * A relayed message reaches the C3 as a normal schema-v1 advert, under the
 * ORIGINATOR's advertising address, carrying the originator's 7-byte payload
 * byte for byte. The mesh appends 5 bytes after that payload inside the same
 * Manufacturer Specific Data structure:
 *
 *   Flags AD    02 01 06
 *   MSD AD      0f ff | ff ff | <7-byte mind_adv_payload_t> | <5-byte mesh hdr>
 *                             company              payload            transport
 *
 * An unmodified C3 decoder ignores the extra bytes. Both decoders in this
 * project length-check the manufacturer data with >= (a minimum), not ==, and
 * then copy exactly sizeof(mind_adv_payload_t):
 *
 *   esp-sidecar/tools/ble_sniffer/main/main.c:142   mfg_data_len < 2 + MIND_PAYLOAD_SIZE
 *
 * so the trailing header is invisible to them. That is the whole transparency
 * mechanism, and it is the reason the payload must be forwarded verbatim: the
 * moment a relay rewrites a payload field, the C3 is being told something the
 * originator did not say.
 *
 * WHAT THIS COSTS - READ BEFORE TRUSTING RSSI
 * -------------------------------------------
 * A relayed advert arrives under the originator's address but with the RSSI of
 * the LAST HOP. Anything inferring distance or position from RSSI will place
 * the originator at the relay's location. The hop count is exposed on air
 * precisely so a receiver can tell the two cases apart, but nothing in this
 * project consumes it yet: treat RSSI-based localisation as invalid while the
 * mesh is enabled.
 *
 * THREADING
 * ---------
 * Two tasks, but the app only creates one of them.
 *
 * mesh_task() is a complete task body and expects to be the lowest-priority
 * task in the app - it spins rather than sleeps, for the reason documented at
 * MESH_POLL_SPIN. It only ever receives, decides, and queues.
 *
 * mesh_init() creates the second task itself. It drains the relay queue and
 * serves the random backoff, so that waiting happens somewhere other than the
 * receive loop. Doing the backoff inline was measured costing 75% of incoming
 * traffic - see MESH_RELAY_QUEUE - because a relay in a 50 ms delay is deaf,
 * and messages arrive in bursts precisely when a relay is busiest.
 *
 * mesh_originate() may be called from any other task. The radio itself is
 * serialised inside ble_radio.c.
 */

#ifndef MESH_FLOOD_H
#define MESH_FLOOD_H

#include <tk/tkernel.h>

#include "ble_radio.h"
#include "mesh_config.h"
#include "mesh_wire.h"      /* shared/ - the on-air transport contract */

/* The wire layout, version and length rules live in shared/mesh_wire.h so the
   ESP32-C3 decoder is held against the same definition rather than a copy of
   it. These are local spellings of those, nothing more. */
#define MESH_VERSION        MESH_WIRE_VERSION
#define MESH_HDR_SIZE       MESH_WIRE_HDR_SIZE

/* Largest payload the mesh will carry. Legacy advertising caps AdvData at 31:
   Flags(3) + MSD overhead(4) leaves 24 for payload + mesh header. */
#define MESH_MAX_PAYLOAD    (31 - 3 - 4 - MESH_HDR_SIZE)

typedef struct {
    UB   device_id;         /* this node's id: AdvA[4] and orig_id when we send */
    UB   role;              /* MESH_ROLE_*                                      */
    UB   ttl;               /* initial hop budget for our own messages          */
    UB   relay_repeats;     /* rebroadcasts per relayed message                 */
    UB   relay_chan_mask;   /* BLE_CHAN_* bits used when relaying               */
    UW   jitter_max_us;     /* backoff drawn uniformly from [0, this)           */
    UINT listen_ch;         /* advertising channel to camp on                   */
    PRI  tx_task_pri;       /* backoff task; must outrank the mesh_task() caller */
} mesh_cfg_t;

/* Counters for the bench. Every drop path is separate on purpose: "the mesh is
   dropping traffic" is not actionable, but knowing whether it is duplicate
   suppression (working as designed), an exhausted TTL (a range or topology
   problem) or malformed input (a framing bug) is. */
typedef struct {
    UW rx_total;        /* CRC-valid adverts seen, including strangers'    */
    UW rx_mesh;         /* of those, well-formed messages from this mesh   */
    UW originated;      /* messages we sourced                             */
    UW relayed;         /* distinct messages we rebroadcast                */
    UW tx_bursts;       /* transmit bursts, counting repeats               */
    UW dup_dropped;     /* already in the cache - the flood terminating    */
    UW ttl_dropped;     /* hop budget exhausted                            */
    UW own_dropped;     /* our own message came back to us                 */
    UW malformed;       /* mesh company id but unparseable or wrong version */
    UW queue_dropped;   /* relay backlog full - arriving faster than backoff drains */
    UW radio_dropped;   /* ble_radio_stats().dropped, mirrored for one-stop reporting */
} mesh_stats_t;

/* Populate 'cfg' from mesh_config.h defaults with the given identity. Call
   this and then override individual fields rather than filling the struct by
   hand, so a new field cannot be left uninitialised. */
void mesh_default_cfg(mesh_cfg_t *cfg, UB device_id);

/* Bring up the radio, the time base and the mesh state. Calls ble_radio_init()
   and hw_timer_init(): an app that uses the mesh must NOT also call those
   itself. Starts listening on cfg->listen_ch, and creates the internal backoff
   task described above at cfg->tx_task_pri. */
void mesh_init(const mesh_cfg_t *cfg);

/* Originate a message from this node: framed with a fresh mesh header, TTL from
   the config, hops 0, sent on all three channels under our own AdvA.
   'payload' is the opaque application payload (7 bytes for schema v1).
   With MESH_ENABLE 0 this still transmits, but without the mesh header, so a
   mesh-disabled build is indistinguishable on air from the pre-mesh firmware. */
void mesh_originate(const UB *payload, UINT payload_len);

/* One receive-and-relay step. Returns 1 if a packet was processed, 0 if the
   radio had nothing. Exposed for apps that own their main loop and want to
   interleave other work; mesh_task() is the usual entry point. */
int mesh_poll_once(void);

/* Relay loop. Never returns. Runs mesh_poll_once() at the cadence set by
   MESH_POLL_SPIN. Must be the lowest-priority task in the app. */
void mesh_task(void);

void mesh_stats(mesh_stats_t *out);

/* Frame Flags + MSD with an explicit mesh header. Exposed so a test app can
   build deliberately malformed or pre-aged packets (a TTL of 1 arriving from
   nowhere, say) that the normal path would never produce. 'buf' needs
   BLE_ADV_MAX_DATA bytes. Returns bytes written. */
UINT mesh_build_adv(UB *buf, const UB *payload, UINT payload_len,
                    UB orig_id, UB relay_id, UB ttl, UB hops);

#endif /* MESH_FLOOD_H */
