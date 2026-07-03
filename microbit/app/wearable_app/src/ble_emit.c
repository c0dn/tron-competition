/*
 * ble_emit.c - BLE advertising output (see ble_emit.h).
 *
 * The wire format is defined once in the shared contract (schema.h). We fill
 * the packed mind_adv_payload_t and frame it as the two AD structures the
 * contract mandates: Flags + Manufacturer Specific Data. Identity rides the
 * fixed AdvA (MIND_ADVA), so nothing identity-related is in the payload.
 *
 * NOTE on the driver: ble_radio_advertise(adv, len, addr6) takes the FULL AD
 * structures in 'adv' (not just the payload) and the address as the 3rd arg,
 * so we hand-frame Flags+MSD here rather than the shorthand in schema.h.
 */

#include "ble_emit.h"
#include "ble_radio.h"
#include "app_config.h"
#include "schema.h"

/* Fixed random-static advertising address for this unit (LSB..MSB). */
static const UB adva[6] = MIND_ADVA(DEVICE_ID);

UINT ble_emit_pack(const incident_state_t *st, UB *buf)
{
    mind_adv_payload_t p;
    const UB *pbytes = (const UB *)&p;
    UW svm = st->accel_svm;
    UINT len = 0;
    UINT i;

    if (svm > 8000u) svm = 8000u;   /* contract range: 0..8000 mg @ +/-8g */

    p.schema_version = MIND_SCHEMA_VERSION;
    p.event_type     = st->event_type;
    p.confidence     = st->confidence;
    p.accel_svm      = (uint16_t)svm;
    p.mic_level      = st->mic_level;
    p.seq            = st->seq;

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
        buf[len++] = pbytes[i];     /* packed struct == wire bytes (LE) */
    }

    return len;
}

void ble_emit_init(void)
{
    ble_radio_init();
}

void ble_emit_advertise(const incident_state_t *st)
{
    UB buf[BLE_ADV_MAX_DATA];
    UINT len = ble_emit_pack(st, buf);
    ble_radio_advertise(buf, len, adva);
}
