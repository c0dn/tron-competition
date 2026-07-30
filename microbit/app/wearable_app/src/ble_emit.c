/*
 * ble_emit.c - BLE advertising output (see ble_emit.h).
 *
 * The wire format is defined once in the shared contract (schema.h). We fill
 * the packed mind_adv_payload_t and frame it as the two AD structures the
 * contract mandates: Flags + Manufacturer Specific Data. Identity rides the
 * fixed AdvA (MIND_ADVA), so nothing identity-related is in the payload.
 *
 * MESH
 * ----
 * With MESH_ENABLE set, the same payload is handed to mesh_originate() instead
 * of straight to the radio, which frames it identically and appends the 5-byte
 * transport header from shared/mesh_wire.h. The detection code above this file
 * is unchanged either way, and so is the ESP32-C3: the extra bytes sit past the
 * length every existing decoder checks.
 *
 * With MESH_ENABLE 0 this file behaves exactly as it did before the mesh
 * existed, down to the byte, so the mesh can be taken out of the picture
 * without reverting anything.
 */

#include "ble_emit.h"
#include "app_config.h"
#include "schema.h"

#if MESH_ENABLE
#  include "mesh_flood.h"
#else
#  include "ble_radio.h"
#endif

#if !MESH_ENABLE
/* Fixed random-static advertising address for this unit (LSB..MSB).
   With the mesh on, mesh_init() derives the same address from DEVICE_ID and
   owns transmission, so this would be a second copy of one fact. */
static const UB adva[6] = MIND_ADVA(DEVICE_ID);
#endif

/* Fill the 7-byte contract payload from an incident. Kept separate from the
   AD framing because the mesh does its own framing but needs this identical. */
static void pack_payload(const incident_state_t *st, UB *out)
{
    mind_adv_payload_t p;
    const UB *pbytes = (const UB *)&p;
    UW svm = st->accel_svm;
    UINT i;

    if (svm > 8000u) svm = 8000u;   /* contract range: 0..8000 mg @ +/-8g */

    p.schema_version = MIND_SCHEMA_VERSION;
    p.event_type     = st->event_type;
    p.confidence     = st->confidence;
    p.accel_svm      = (uint16_t)svm;
    p.mic_level      = st->mic_level;
    p.seq            = st->seq;

    for (i = 0; i < MIND_PAYLOAD_SIZE; i++) {
        out[i] = pbytes[i];         /* packed struct == wire bytes (LE) */
    }
}

UINT ble_emit_pack(const incident_state_t *st, UB *buf)
{
    UB payload[MIND_PAYLOAD_SIZE];
    UINT len = 0;
    UINT i;

    pack_payload(st, payload);

    /* AD 1 - Flags: LE General Discoverable, BR/EDR not supported. */
    buf[len++] = 0x02;
    buf[len++] = MIND_AD_TYPE_FLAGS;
    buf[len++] = 0x06;

    /* AD 2 - Manufacturer Specific Data: company(2) + payload(7). */
    buf[len++] = (UB)(1 + 2 + MIND_PAYLOAD_SIZE);   /* type + company + payload */
    buf[len++] = MIND_AD_TYPE_MSD;
    buf[len++] = (UB)(MIND_COMPANY_ID & 0xFF);
    buf[len++] = (UB)(MIND_COMPANY_ID >> 8);
    for (i = 0; i < MIND_PAYLOAD_SIZE; i++) {
        buf[len++] = payload[i];
    }

    return len;
}

void ble_emit_init(void)
{
#if MESH_ENABLE
    mesh_cfg_t cfg;

    /* mesh_init() brings up the radio and the hardware timer itself, so
       ble_radio_init() must NOT also be called here. */
    mesh_default_cfg(&cfg, DEVICE_ID);
    cfg.tx_task_pri = MESH_TX_TASK_PRI;
    mesh_init(&cfg);
#else
    ble_radio_init();
#endif
}

void ble_emit_advertise(const incident_state_t *st)
{
#if MESH_ENABLE
    UB payload[MIND_PAYLOAD_SIZE];

    pack_payload(st, payload);
    mesh_originate(payload, MIND_PAYLOAD_SIZE);
#else
    UB buf[BLE_ADV_MAX_DATA];
    UINT len = ble_emit_pack(st, buf);

    ble_radio_advertise(buf, len, adva);
#endif
}
