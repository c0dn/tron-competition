/*
 * ble_emit.h - BLE advertising output for the wearable.
 * Packs incident_state_t into a TM/01 MIND_EVENT frame and advertises with
 * the fixed AdvA identity.
 */

#ifndef BLE_EMIT_H
#define BLE_EMIT_H

#include "incident.h"

/* Configure the radio for advertising. Call once at boot. */
void ble_emit_init(void);

/* Build the full AD structures (Flags + MSD) for 'st' into 'buf'; returns the
 * byte length. 'buf' must hold at least BLE_ADV_MAX_DATA bytes. */
UINT ble_emit_pack(const incident_state_t *st, UB *buf);

/* Pack 'st' and broadcast it once (all three primary channels) using the
 * fixed random-static AdvA. */
void ble_emit_advertise(const incident_state_t *st);

#endif /* BLE_EMIT_H */
