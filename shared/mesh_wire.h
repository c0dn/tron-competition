/*
 * MIND — Flood Mesh Transport Header (mesh wire v1)
 * TRON Competition 2026 — Multimodal Incident Detection
 *
 * SIDE: micro:bit relay firmware (encoder) and ESP32-C3 receivers (decoder).
 *
 * This is a SEPARATE contract from schema.h on purpose. schema.h describes what
 * a wearable observed; this describes how that observation travelled. The two
 * version independently: a relay must be able to reject a transport it does not
 * understand while still decoding a payload it does, and a payload change must
 * not force every relay to be reflashed.
 *
 * ON-AIR LAYOUT
 * -------------
 * The transport header is appended INSIDE the existing Manufacturer Specific
 * Data structure, after the schema payload:
 *
 *   02 01 06                          Flags AD                         3 B
 *   0f ff ffff <7B payload> <5B hdr>  MSD AD                          16 B
 *              ^company    ^schema.h  ^this file                 total 19 B
 *
 * Legacy advertising allows 31 bytes of AdvData, so this leaves 12 B spare.
 *
 * BACKWARD COMPATIBILITY
 * ----------------------
 * A decoder that predates the mesh ignores these bytes with no change, because
 * both decoders in this project length-check manufacturer data with a MINIMUM
 * (>= 2 + MIND_PAYLOAD_SIZE) and then copy exactly sizeof(mind_adv_payload_t).
 * That is what lets a relayed message reach an unmodified ESP32-C3 as an
 * ordinary wearable advert. Preserve that property: any future field goes on
 * the END, and the payload is never rewritten in flight.
 *
 * IDENTITY AND RSSI — READ THIS
 * -----------------------------
 * A relayed advert carries the ORIGINATOR's advertising address, so AdvA[4] is
 * still the originating device. The RSSI, however, belongs to the LAST HOP.
 * Anything deriving distance or position from RSSI must check hops == 0 first,
 * or it will place the originator wherever the relay happens to be.
 */

#ifndef MIND_MESH_WIRE_H
#define MIND_MESH_WIRE_H

#include <stdint.h>

#include "schema.h"     /* MIND_PACKED, MIND_PAYLOAD_SIZE */

/* Bumped only when the byte layout below changes. Independent of
   MIND_SCHEMA_VERSION. */
#define MESH_WIRE_VERSION       1

/* version(1) + orig_id(1) + relay_id(1) + ttl(1) + hops(1) */
#define MESH_WIRE_HDR_SIZE      5

typedef struct MIND_PACKED {
    uint8_t version;    /* [0] MESH_WIRE_VERSION                              */
    uint8_t orig_id;    /* [1] originating node; restates AdvA[4] (see below) */
    uint8_t relay_id;   /* [2] node that transmitted THIS copy                */
    uint8_t ttl;        /* [3] remaining hops; 0 means do not forward         */
    uint8_t hops;       /* [4] hops already taken; 0 means heard directly     */
} mesh_wire_hdr_t;

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(mesh_wire_hdr_t) == MESH_WIRE_HDR_SIZE,
               "mesh_wire_hdr_t must be exactly 5 bytes");
#else
typedef char mesh_wire_size_check[(sizeof(mesh_wire_hdr_t) == MESH_WIRE_HDR_SIZE) ? 1 : -1];
#endif

/*
 * orig_id duplicates AdvA[4] deliberately. It is an integrity check: the two
 * disagreeing means a relay corrupted either the address or the header, and
 * forwarding that would propagate corruption under a valid-looking identity.
 * Receivers should drop such a packet rather than trust either copy.
 */

/* Total manufacturer-data length of a mesh-carrying advert: company id (2) +
 * schema payload + this header.
 *
 * Decoders should test manufacturer length against this EXACTLY to detect a
 * mesh advert. A >= test is not sufficient: a plain 9-byte non-mesh advert
 * would otherwise be read as a 2-byte payload followed by five bytes of
 * "header" made out of the sender's accelerometer reading. */
#define MESH_WIRE_MSD_LEN       (2 + MIND_PAYLOAD_SIZE + MESH_WIRE_HDR_SIZE)

/* Byte offset of the transport header within the manufacturer data. */
#define MESH_WIRE_HDR_OFFSET    (2 + MIND_PAYLOAD_SIZE)

#endif /* MIND_MESH_WIRE_H */
