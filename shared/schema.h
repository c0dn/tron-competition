/*
 * MIND — Wearable Direct-Ingress Payload Contract (schema v1)
 * TRON Competition 2026 — Multimodal Incident Detection
 *
 * The micro:bit wearable carries this seven-byte payload in a TM/01
 * MIND_EVENT advertisement. TAVRN backbone firmware consumes that envelope
 * as direct Layer-7 application ingress; it is not legacy-relayed.
 *
 * Status: schema v1, drafted from plans/02-message-schema/PLAN.md.
 *         FREEZE with the team before writing encode/decode against it.
 *
 * Endianness: the packed payload uses little-endian multi-byte fields. Keep
 *             that ordering when hand-packing on any target.
 */

#ifndef MIND_SCHEMA_H
#define MIND_SCHEMA_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Advertising framing                                                 */
/* ------------------------------------------------------------------ */
/*
 * The complete advertisement is the TM/01 manufacturer envelope defined by
 * tron_mesh_packet.h. This header defines only its schema-v1 seven-byte
 * payload; it does not define the complete AD structure or its byte budget.
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
/* Payload — 7 bytes, carried verbatim inside TM/01 MIND_EVENT          */
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
    uint8_t  seq;            /* [6]  compatibility low byte of packet_id24    */
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
/* RF advertiser address                                                   */
/* ------------------------------------------------------------------ */
/*
 * Logical event identity is the stable pair (wearable source, packet_id24)
 * in the TM/01 envelope. wearable source is 0x0100 | DEVICE_ID; packet_id24
 * is stable across every copy of one event spray, and payload seq is its low
 * compatibility byte. The AdvA below remains a fixed random-static RF address
 * for radio-level filtering.
 *
 *   AdvA = { 0x00, 0x00, 0x00, 0x00, DEVICE_ID, 0xC0 }   // LSB .. MSB
 *                                                 ^^^^  0xC0 = random-static
 *
 * DEVICE_ID is a per-unit uint8 set at flash time. Pass MIND_ADVA(id) to
 * ble_radio_advertise() rather than NULL.
 */
#ifndef MIND_DEVICE_ID
#  define MIND_DEVICE_ID 1   /* override per unit at build time */
#endif

#define MIND_ADVA(id) { 0x00, 0x00, 0x00, 0x00, (uint8_t)(id), 0xC0 }

#endif /* MIND_SCHEMA_H */
