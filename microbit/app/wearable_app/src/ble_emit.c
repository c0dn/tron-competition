/*
 * ble_emit.c - BLE advertising output (see ble_emit.h).
 *
 * The wearable is a mesh originator. Its incidents go out as TRON mesh
 * packets (app/protocol/tron_mesh_packet.h) carrying the schema-v1 payload
 * from shared/schema.h verbatim as msg_type MIND_EVENT, rather than as a
 * bare Manufacturer-Specific-Data beacon aimed at a single observer.
 *
 * WHY THE MESH FRAME RATHER THAN A PLAIN BEACON
 * ---------------------------------------------
 * A bare beacon only reaches whoever is in radio range at the instant it is
 * sent. Wrapping the same 7 bytes in the mesh frame gets relay, TTL and
 * duplicate suppression for free from machinery that already exists, so an
 * incident can cross the room through an intermediate node. The payload is
 * unchanged, so nothing about the detection contract moves.
 *
 * Both formats use company id 0xFFFF and are told apart by the 'T','M' magic
 * the mesh encoder writes, so they can coexist on air during a migration.
 *
 * Sizing: TRON_MESH_ADV_BASE_LEN (19) + MIND_PAYLOAD_SIZE (7) = 26 bytes,
 * inside the 31-byte legacy advertising cap with 5 to spare.
 *
 * Identity now rides the mesh src field. The fixed AdvA is kept because it
 * costs no payload and leaves address-based filtering working.
 */

#include "ble_emit.h"
#include "ble_radio.h"
#include "app_config.h"
#include "schema.h"
#include "tron_mesh_packet.h"

/* Fixed random-static advertising address for this unit (LSB..MSB). */
static const UB adva[6] = MIND_ADVA(DEVICE_ID);

/* Local byte copy rather than <string.h>. Newlib's string.h drags in stddef's
 * size_t (unsigned int), which collides with the kernel's SZ (long int) in
 * tk/syslib.h, so any translation unit that includes tk/tkernel.h cannot also
 * include string.h. ble_mesh_scheduler.c carries the same helper for the same
 * reason. */
static void copy_bytes(void *dst, const void *src, UINT len)
{
    UB *d = (UB *)dst;
    const UB *s = (const UB *)src;

    while (len > 0u) {
        *d++ = *s++;
        len--;
    }
}

UINT ble_emit_pack(const incident_state_t *st, UB *buf)
{
    tron_mesh_packet_t pkt;
    mind_adv_payload_t p;
    size_t adv_len = 0u;
    UW svm = st->accel_svm;

    if (svm > 8000u) svm = 8000u;   /* contract range: 0..8000 mg @ +/-8g */

    p.schema_version = MIND_SCHEMA_VERSION;
    p.event_type     = st->event_type;
    p.confidence     = st->confidence;
    p.accel_svm      = (uint16_t)svm;
    p.mic_level      = st->mic_level;
    p.seq            = st->seq;

    pkt.net_id = MIND_MESH_NET_ID;
    /* Originate at full TTL so relays will forward it. */
    pkt.ttl = TRON_MESH_TTL_MAX;
    pkt.src = MIND_MESH_SRC;
    /* Same id on every copy of one message: relays suppress the duplicates
     * instead of re-flooding each one. */
    pkt.seq24 = (uint32_t)(st->event_id & TRON_MESH_SEQ24_MAX);
    pkt.msg_type = TRON_MESH_MSG_TYPE_MIND_EVENT;
    pkt.payload_len = (uint8_t)MIND_PAYLOAD_SIZE;
    copy_bytes(pkt.payload, &p, (UINT)MIND_PAYLOAD_SIZE);

    if (tron_mesh_packet_encode(&pkt, buf, BLE_ADV_MAX_DATA, &adv_len)
        != TRON_MESH_PACKET_OK) {
        return 0;
    }

    return (UINT)adv_len;
}

void ble_emit_init(void)
{
    ble_radio_init();
}

void ble_emit_advertise(const incident_state_t *st)
{
    UB buf[BLE_ADV_MAX_DATA];
    UINT len = ble_emit_pack(st, buf);

    if (len == 0u) {
        return;                 /* encoder rejected it; nothing sane to send */
    }
    ble_radio_advertise(buf, len, adva);
}
