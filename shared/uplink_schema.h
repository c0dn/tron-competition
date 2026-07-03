/*
 * MIND — Mesh Uplink Contract (uplink schema v1)
 * TRON Competition 2026 — Multimodal Incident Detection
 *
 * SIDE: Tier B — ESP32-C3 observer/mesh --binary--> ESP32-C3 root --MQTT--> dashboard.
 * This is the SINGLE source of truth for the binary records that travel the
 * last three hops of the pipeline:
 *
 *   1. leaf node -> root node   (esp_mesh_lite_send_raw_msg_to_root, raw bytes)
 *   2. root node -> MQTT broker (binary passthrough, topics under mind/ingest/)
 *   3. broker    -> web dashboard (decoded by a DataView mirror of these structs)
 *
 * The same packed struct is reused at every hop, so the root never re-encodes
 * and the wire footprint stays minimal. Both ESP32-C3 (this) and the dashboard
 * decoder derive their byte layout from this header.
 *
 * Endianness: ESP32-C3 is little-endian; keep every multi-byte field LE. The
 * dashboard reads them back with DataView getters passing littleEndian = true.
 *
 * Tier A (micro:bit --BLE--> ESP32-C3) is defined separately in schema.h; this
 * header includes it so the event_type enum stays shared and consistent.
 */

#ifndef MIND_UPLINK_SCHEMA_H
#define MIND_UPLINK_SCHEMA_H

#include <stdint.h>
#include "schema.h"   /* enum mind_event_type, MIND_* — the Tier A contract */

#if defined(__GNUC__)
#  define MIND_UPLINK_PACKED __attribute__((packed))
#else
#  define MIND_UPLINK_PACKED
#endif

/* ------------------------------------------------------------------ */
/* Uplink schema version                                               */
/* ------------------------------------------------------------------ */
#define MIND_UPLINK_VERSION     1

/* ------------------------------------------------------------------ */
/* Record type discriminator                                           */
/* ------------------------------------------------------------------ */
/*
 * Every record begins with the same 3-byte prefix
 *   { uint8 uplink_version, uint8 rec_type, uint8 node_id }
 * so a decoder can peek rec_type at byte offset [1] before choosing a layout.
 */
enum mind_rec_type {
    MIND_REC_EVENT  = 1,  /* a wearable observation (mind_uplink_event_t)  */
    MIND_REC_STATUS = 2,  /* an ESP32-C3 node health report (mind_uplink_status_t) */
};

/* ------------------------------------------------------------------ */
/* Event record — a decoded wearable beacon, tagged by the node that   */
/* observed it. 13 bytes.                                              */
/* ------------------------------------------------------------------ */
typedef struct MIND_UPLINK_PACKED {
    uint8_t  uplink_version; /* [0]  = MIND_UPLINK_VERSION (1)                 */
    uint8_t  rec_type;       /* [1]  = MIND_REC_EVENT (1)                      */
    uint8_t  node_id;        /* [2]  observing ESP32-C3 node id                */
    uint8_t  device_id;      /* [3]  micro:bit DEVICE_ID (from AdvA[4])        */
    int8_t   rssi;           /* [4]  receive strength, dBm (negative)          */
    uint8_t  event_type;     /* [5]  enum mind_event_type (from schema.h)      */
    uint8_t  confidence;     /* [6]  0..100                                    */
    uint16_t accel_svm;      /* [7-8] peak sum-vector-magnitude, milli-g, LE   */
    uint8_t  mic_level;      /* [9]  scaled loudness 0..255                    */
    uint8_t  seq;            /* [10] wearable payload seq (dedup key)          */
    uint16_t age_ms;         /* [11-12] ms since the node observed it (LE),    */
                             /*         clamped; lets the dashboard show       */
                             /*         freshness without any clock sync       */
} mind_uplink_event_t;

/* ------------------------------------------------------------------ */
/* Status record — ESP32-C3 node health / mesh position. 15 bytes.     */
/* ------------------------------------------------------------------ */
typedef struct MIND_UPLINK_PACKED {
    uint8_t  uplink_version; /* [0]  = MIND_UPLINK_VERSION (1)                 */
    uint8_t  rec_type;       /* [1]  = MIND_REC_STATUS (2)                     */
    uint8_t  node_id;        /* [2]  this ESP32-C3 node id                     */
    uint8_t  is_root;        /* [3]  1 if this node is the mesh root, else 0   */
    uint8_t  mesh_level;     /* [4]  mesh level; root = 1                      */
    int8_t   parent_rssi;    /* [5]  RSSI to parent, dBm (0 on root)           */
    uint32_t uptime_s;       /* [6-9]  seconds since boot, LE                  */
    uint32_t free_heap;      /* [10-13] free heap bytes, LE                    */
    uint8_t  child_count;    /* [14] number of direct mesh children            */
} mind_uplink_status_t;

/* ------------------------------------------------------------------ */
/* Compile-time size guards — the wire layout must not drift silently. */
/* ------------------------------------------------------------------ */
#define MIND_UPLINK_EVENT_SIZE   13
#define MIND_UPLINK_STATUS_SIZE  15

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(mind_uplink_event_t) == MIND_UPLINK_EVENT_SIZE,
               "mind_uplink_event_t must be exactly 13 bytes");
_Static_assert(sizeof(mind_uplink_status_t) == MIND_UPLINK_STATUS_SIZE,
               "mind_uplink_status_t must be exactly 15 bytes");
#else
typedef char mind_uplink_event_size_check[(sizeof(mind_uplink_event_t) == MIND_UPLINK_EVENT_SIZE) ? 1 : -1];
typedef char mind_uplink_status_size_check[(sizeof(mind_uplink_status_t) == MIND_UPLINK_STATUS_SIZE) ? 1 : -1];
#endif

/* Largest record — size a shared scratch/DMA buffer with this. */
#define MIND_UPLINK_MAX_SIZE  MIND_UPLINK_STATUS_SIZE

#endif /* MIND_UPLINK_SCHEMA_H */
