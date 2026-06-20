/*
 * ble_radio.h - BLE advertising over the raw nRF52833 RADIO (no SoftDevice).
 *
 * Configures the RADIO to emit standards-compliant BLE advertising PDUs on
 * the three primary advertising channels (37/38/39). A phone running a BLE
 * scanner (e.g. nRF Connect) can then discover the micro:bit as a beacon.
 *
 * This is BROADCAST ONLY - no connections, no GATT. Those need a link-layer
 * state machine (or the SoftDevice), which this does not implement.
 */

#ifndef BLE_RADIO_H
#define BLE_RADIO_H

#include <tk/tkernel.h>

#define BLE_ADV_MAX_DATA    31      /* max AD-structure bytes in a legacy ADV */

/* Configure the RADIO for BLE 1M advertising. The kernel already starts the
   HFXO crystal the radio needs. Call once at boot. */
void ble_radio_init(void);

/* Broadcast one ADV_NONCONN_IND beacon carrying 'adv', 'adv_len' bytes of
   advertising data (AD structures), transmitting once on each of channels
   37, 38 and 39. 'addr6' is the 6-byte advertiser address (little-endian);
   pass NULL to use the chip's FICR-derived random static address. */
void ble_radio_advertise(const UB *adv, UINT adv_len, const UB *addr6);

/* --- Observer (passive scan) side --- */

/* Largest received PDU: 2 (header+length) + 6 (AdvA) + 31 (AdvData). */
#define BLE_RX_MAX          39

/* Put the radio into continuous receive on one primary advertising channel
   (37, 38, or 39). The beacon transmits on all three each event, so camping
   on one channel still catches it. Call once before polling. */
void ble_radio_listen(UINT channel);

/* Non-blocking poll for a received advertising packet. On a CRC-valid
   packet returns 1 with the raw PDU copied to 'buf' as
   [S0][LENGTH][AdvA(6)][AdvData...], *len = total bytes (>= 8),
   *rssi_dbm = positive (actual power = -(*rssi_dbm) dBm). 'buf' must hold
   at least BLE_RX_MAX bytes. Returns 0 if nothing valid is available yet. */
int ble_radio_poll(UB *buf, UINT *len, UINT *rssi_dbm);

#endif /* BLE_RADIO_H */
