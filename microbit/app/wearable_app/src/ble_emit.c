/*
 * ble_emit.c - BLE advertising output (see ble_emit.h).
 *
 * Schema-v1 payload bytes are carried inside a TM/01 manufacturer frame.  A
 * stable 24-bit packet id distinguishes one logical wearable message from its
 * copies; TTL is zero because backbone application ingress consumes the frame
 * directly rather than sending it through the legacy flood node.
 */

#include "ble_emit.h"
#include "ble_radio.h"
#include "app_config.h"
#include "schema.h"
#include "tron_mesh_packet.h"

/* Fixed random-static advertising address for this unit (LSB..MSB). */
static const UB adva[6] = MIND_ADVA(DEVICE_ID);

UINT ble_emit_pack(const incident_state_t *st, UB *buf)
{
    tron_mesh_packet_t packet;
    mind_adv_payload_t p;
    size_t adv_len = 0u;
    UW svm = st->accel_svm;

    if (svm > 8000u) svm = 8000u;   /* contract range: 0..8000 mg @ +/-8g */

    p.schema_version = MIND_SCHEMA_VERSION;
    p.event_type     = st->event_type;
    p.confidence     = st->confidence;
    p.accel_svm      = (uint16_t)svm;
    p.mic_level      = st->mic_level;
    /* The schema compatibility byte is always derived from the on-air ID. */
    p.seq            = (UB)(st->event_id & 0xFFu);

    packet.net_id = MIND_MESH_NET_ID;
    packet.ttl = 0u;
    packet.src = MIND_MESH_SRC;
    packet.seq24 = (uint32_t)(st->event_id & TRON_MESH_SEQ24_MAX);
    packet.msg_type = TRON_MESH_MSG_TYPE_MIND_EVENT;
    packet.payload_len = (uint8_t)MIND_PAYLOAD_SIZE;
    {
        const UB *bytes = (const UB *)&p;
        UINT index;

        for (index = 0; index < MIND_PAYLOAD_SIZE; index++) {
            packet.payload[index] = bytes[index];
        }
    }

    if (tron_mesh_packet_encode(&packet, buf, BLE_ADV_MAX_DATA, &adv_len) !=
        TRON_MESH_PACKET_OK) {
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
        return;
    }
    ble_radio_advertise(buf, len, adva);
}
