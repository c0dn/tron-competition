/*
 * MIND — Wearable BLE Beacon Contract (schema v1)
 * TRON Competition 2026 — Multimodal Incident Detection
 *
 * SIDE: Tier A only — micro:bit wearable  --BLE advertising-->  ESP32-C3.
 * This is the SINGLE source of truth for the on-air binary payload.
 * Both the micro:bit firmware (encoder) and the ESP32-C3 observer (decoder)
 * MUST include this header (or an exact copy) so the bytes agree.
 *
 * Status: schema v1, drafted from plans/02-message-schema/PLAN.md.
 *         FREEZE with the team before writing encode/decode against it.
 *
 * Endianness: both targets (nRF52833, ESP32-C3) are little-endian, so the
 *             packed struct below maps byte-for-byte onto the wire. Keep all
 *             multi-byte fields little-endian if you ever hand-pack instead.
 */

#ifndef MIND_SCHEMA_H
#define MIND_SCHEMA_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Advertising framing                                                 */
/* ------------------------------------------------------------------ */
/*
 * The beacon advertises EXACTLY two AD structures, nothing else:
 *   1. Flags            (AD type 0x01)
 *   2. Manufacturer Specific Data (AD type 0xFF) carrying the payload below
 *
 * REMOVED (legacy waste — do NOT include):
 *   - Complete Local Name  ("uTK-sensor")   <-- drop it
 *   - any human-readable / company-name AD structures
 *
 * On-air budget: Flags(3) + MSD[len(1)+type(1)+company(2)+payload(7)=11] = 14 B
 *                Legacy AdvData cap = 31 B  ->  17 B headroom.
 */
#define MIND_AD_TYPE_FLAGS      0x01
#define MIND_AD_TYPE_MSD        0xFF   /* Manufacturer Specific Data          */
#define MIND_COMPANY_ID         0xFFFF /* SIG test id — demo only (Plan 02 R1)*/

/* ------------------------------------------------------------------ */
/* Schema version                                                      */
/* ------------------------------------------------------------------ */
#define MIND_SCHEMA_VERSION     1

/* ------------------------------------------------------------------ */
/* Event types (canonical enum — shared with Plan 01)                  */
/* ------------------------------------------------------------------ */
enum mind_event_type {
    MIND_EVT_HEARTBEAT       = 0, /* alive/normal, slow cadence               */
    MIND_EVT_MOTION          = 1, /* notable movement, not an incident        */
    MIND_EVT_POSSIBLE_FALL   = 2, /* free-fall + impact, not yet confirmed    */
    MIND_EVT_CONFIRMED_FALL  = 3, /* impact + post-impact immobility          */
    MIND_EVT_POSSIBLE_DISTRESS = 4, /* standalone shout/scream (audio only)   */
    MIND_EVT_FALL_AND_SHOUT  = 5, /* fall + shout coincided (highest prio)    */
    /* 6..255 reserved — do not use in v1                                     */
};

/* ------------------------------------------------------------------ */
/* Payload — 7 bytes, carried inside the MSD after the company id      */
/* ------------------------------------------------------------------ */
#if defined(__GNUC__)
#  define MIND_PACKED __attribute__((packed))
#else
#  define MIND_PACKED
#endif

typedef struct MIND_PACKED {
    uint8_t  schema_version; /* [0]  = MIND_SCHEMA_VERSION (1)                */
    uint8_t  event_type;     /* [1]  enum mind_event_type                     */
    uint8_t  confidence;     /* [2]  0..100 (heartbeat = 0)                   */
    uint16_t accel_svm;      /* [3-4] peak sum-vector-magnitude, milli-g,     */
                             /*       0..8000 @ ±8g, little-endian            */
    uint8_t  mic_level;      /* [5]  scaled loudness 0..255 (shout /          */
                             /*       corroboration); 0 on plain heartbeat    */
    uint8_t  seq;            /* [6]  monotonic counter, wraps 255->0 (dedup)  */
} mind_adv_payload_t;

/* Compile-time guard: payload must be exactly 7 bytes. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(mind_adv_payload_t) == 7,
               "mind_adv_payload_t must be exactly 7 bytes");
#else
typedef char mind_payload_size_check[(sizeof(mind_adv_payload_t) == 7) ? 1 : -1];
#endif

#define MIND_PAYLOAD_SIZE 7

/* ------------------------------------------------------------------ */
/* Identity — carried in the advertising address (AdvA), 0 payload B   */
/* ------------------------------------------------------------------ */
/*
 * The wearable uses a FIXED random-static address (NOT the FICR-derived
 * random address the current firmware uses — that is a bug for identity).
 *
 *   AdvA = { 0x00, 0x00, 0x00, 0x00, DEVICE_ID, 0xC0 }   // LSB .. MSB
 *                                                 ^^^^  0xC0 = random-static
 *
 * DEVICE_ID is a per-unit uint8 set at flash time. The ESP32-C3 maps
 * AdvA -> human device label via a small static table.
 *
 * ble_radio_advertise() already takes an addr6 arg: pass MIND_ADVA(id),
 * not NULL.
 */
#ifndef MIND_DEVICE_ID
#  define MIND_DEVICE_ID 1   /* override per unit at build time */
#endif

#define MIND_ADVA(id) { 0x00, 0x00, 0x00, 0x00, (uint8_t)(id), 0xC0 }

#endif /* MIND_SCHEMA_H */
