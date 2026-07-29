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

/* Channel-mask bits for ble_radio_tx(): bit 0 = ch37, 1 = ch38, 2 = ch39. */
#define BLE_CHAN_37         0x01
#define BLE_CHAN_38         0x02
#define BLE_CHAN_39         0x04
#define BLE_CHAN_MASK_ALL   0x07

/* Configure the RADIO for BLE 1M advertising. The kernel already starts the
   HFXO crystal the radio needs. Call once at boot. */
void ble_radio_init(void);

/* Broadcast one ADV_NONCONN_IND beacon carrying 'adv', 'adv_len' bytes of
   advertising data (AD structures), transmitting once on each of channels
   37, 38 and 39. 'addr6' is the 6-byte advertiser address (little-endian);
   pass NULL to use the chip's FICR-derived random static address. */
void ble_radio_advertise(const UB *adv, UINT adv_len, const UB *addr6);

/* As ble_radio_advertise(), but transmits only on the channels selected by
   'chan_mask'. Relaying on one channel costs a third of the airtime of three,
   which is worth having once several nodes rebroadcast - but only if every node
   camps on that same channel, since ble_radio_listen() covers one at a time.
   Safe when transmitting and receiving are always symmetric. */
void ble_radio_tx(const UB *adv, UINT adv_len, const UB *addr6, UB chan_mask);

/* Set transmit power in dBm. The nRF52833 accepts +8..+2, 0, -4, -8, -12, -16,
   -20, -30 and -40; other values are undefined. Turning a node down is the
   practical way to force a relay topology on a bench, where every node would
   otherwise hear every other one directly. */
void ble_radio_set_txpower(INT dbm);

/* --- Observer (passive scan) side --- */

/* Largest received PDU: 2 (header+length) + 6 (AdvA) + 31 (AdvData). */
#define BLE_RX_MAX          39

/* A received packet plus the metadata a relay needs: RSSI for link quality,
   arrival time for duplicate-cache aging, channel for diagnostics. */
typedef struct {
    UB   pdu[BLE_RX_MAX];   /* [S0][LENGTH][AdvA(6)][AdvData...]        */
    UINT len;               /* total bytes in pdu (>= 8)                */
    UINT rssi_dbm;          /* positive; actual power = -rssi_dbm dBm   */
    UW   t_ms;              /* kernel millisecond stamp at poll         */
    UB   ch;                /* advertising channel it was heard on      */
} ble_rx_t;

/* Put the radio into continuous receive on one primary advertising channel
   (37, 38, or 39). The beacon transmits on all three each event, so camping
   on one channel still catches it. Call once before polling.
   The channel is remembered: any later transmit restores this receive state
   when it finishes, so a relay does not go deaf after its first rebroadcast. */
void ble_radio_listen(UINT channel);

/* As ble_radio_poll(), with the extra metadata described by ble_rx_t. */
int ble_radio_poll_ex(ble_rx_t *out);

/* Receive accounting.
 *
 * The receiver restarts into a single buffer, so a packet arriving before the
 * next poll overwrites the previous one and no software counter can see it
 * happen. 'hw_end' is incremented by the radio itself through PPI and therefore
 * counts every packet; 'dropped' is what that reveals was lost.
 *
 * A flood mesh that is silently discarding most of its traffic behaves exactly
 * like one that is working until this number is looked at, so treat any
 * delivery figure quoted without it as unverified. */
typedef struct {
    UW hw_end;      /* END events counted in hardware - the true arrival count */
    UW observed;    /* ENDs software actually serviced                         */
    UW crc_err;     /* of those, packets that failed CRC                       */
    UW dropped;     /* hw_end - observed: overwritten before anyone looked     */
} ble_radio_stats_t;

void ble_radio_stats(ble_radio_stats_t *out);

/* Non-blocking poll for a received advertising packet. On a CRC-valid
   packet returns 1 with the raw PDU copied to 'buf' as
   [S0][LENGTH][AdvA(6)][AdvData...], *len = total bytes (>= 8),
   *rssi_dbm = positive (actual power = -(*rssi_dbm) dBm). 'buf' must hold
   at least BLE_RX_MAX bytes. Returns 0 if nothing valid is available yet. */
int ble_radio_poll(UB *buf, UINT *len, UINT *rssi_dbm);

#endif /* BLE_RADIO_H */
