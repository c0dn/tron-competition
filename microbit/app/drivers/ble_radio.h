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

#include <stdint.h>

#ifdef BLE_RADIO_HOST_TEST
#include <stddef.h>
typedef uint8_t UB;
typedef int INT;
typedef unsigned int UINT;
typedef uintptr_t UW;
#else
#include <tk/tkernel.h>
#endif

#define BLE_ADV_MAX_DATA    31      /* max AD-structure bytes in a legacy ADV */

#define BLE_RADIO_ADV_CH37  (1u << 0)
#define BLE_RADIO_ADV_CH38  (1u << 1)
#define BLE_RADIO_ADV_CH39  (1u << 2)
#define BLE_RADIO_ADV_CH_ALL (BLE_RADIO_ADV_CH37 | BLE_RADIO_ADV_CH38 | BLE_RADIO_ADV_CH39)

typedef enum ble_radio_op_result {
    BLE_RADIO_OP_OK = 0,
    BLE_RADIO_OP_NO_EVENT,
    BLE_RADIO_OP_CRC_DROP,
    BLE_RADIO_OP_INVALID_ARGUMENT,
    BLE_RADIO_OP_STATE_TIMEOUT,
} ble_radio_op_result_t;

typedef struct ble_radio_tx_result {
    uint8_t requested_channel_mask;
    uint8_t completed_channel_mask;
    ble_radio_op_result_t fault;
} ble_radio_tx_result_t;

/* Bounded mesh-only operations. Legacy declarations below remain unchanged. */
ble_radio_op_result_t ble_radio_try_init(UINT state_timeout_ms);
ble_radio_op_result_t ble_radio_try_idle(UINT state_timeout_ms);
ble_radio_op_result_t ble_radio_try_listen_once(UINT channel,
                                                UINT state_timeout_ms);
ble_radio_op_result_t ble_radio_try_poll_snapshot(
    UB *buf, UINT *len, UINT *rssi_magnitude_db, UINT state_timeout_ms);
ble_radio_tx_result_t ble_radio_try_advertise_channels(
    const UB *adv, UINT adv_len, const UB *addr6, UINT channel_mask,
    UINT state_timeout_ms);
ble_radio_op_result_t ble_radio_read_default_adva(UB out_adva[6]);

/* Configure the RADIO for BLE 1M advertising. The kernel already starts the
   HFXO crystal the radio needs. Call once at boot. */
void ble_radio_init(void);

/* Broadcast one ADV_NONCONN_IND beacon carrying 'adv', 'adv_len' bytes of
   advertising data (AD structures), transmitting once on each of channels
   37, 38 and 39. 'addr6' is the 6-byte advertiser address (little-endian);
   pass NULL to use the chip's FICR-derived random static address. */
void ble_radio_advertise(const UB *adv, UINT adv_len, const UB *addr6);

/* Broadcast one ADV_NONCONN_IND on the selected primary advertising channels.
   channel_mask uses BLE_RADIO_ADV_CH37/38/39 bits; a zero mask defaults to all
   three channels. Existing callers can keep using ble_radio_advertise(). */
void ble_radio_advertise_channels(const UB *adv, UINT adv_len, const UB *addr6,
                                  UINT channel_mask);

/* Explicitly put the RADIO into the disabled/idle state. Mesh scheduling uses
   this before interleaved TX windows so RX and TX are never treated as
   simultaneous on the single nRF52833 radio. */
void ble_radio_idle(void);

/* --- Observer (passive scan) side --- */

/* Largest received PDU: 2 (header+length) + 6 (AdvA) + 31 (AdvData). */
#define BLE_RX_MAX          39

/* Put the radio into continuous receive on one primary advertising channel
    (37, 38, or 39). The beacon transmits on all three each event, so camping
    on one channel still catches it. Call once before polling. */
void ble_radio_listen(UINT channel);

/* Put the radio into one-shot receive on one primary advertising channel.
   The radio disables itself at packet END instead of auto-restarting, so the
   single rx_pkt buffer cannot be overwritten before ble_radio_poll_snapshot()
   copies it. Scheduler-owned RX should use this API. */
void ble_radio_listen_once(UINT channel);

/* Non-blocking poll for a received advertising packet. On a CRC-valid
   packet returns 1 with the raw PDU copied to 'buf' as
   [S0][LENGTH][AdvA(6)][AdvData...], *len = total bytes (>= 8),
   *rssi_dbm = positive (actual power = -(*rssi_dbm) dBm). 'buf' must hold
   at least BLE_RX_MAX bytes. Returns 0 if nothing valid is available yet. */
int ble_radio_poll(UB *buf, UINT *len, UINT *rssi_dbm);

/* Non-blocking snapshot poll. This is snapshot-safe when paired with
   ble_radio_listen_once(): a completed one-shot RX has already stopped before
   this function copies rx_pkt. On a CRC-valid packet returns 1 and leaves the
   radio idle. Returns 0 if no packet completion is pending. Returns -1 if a
   completed packet was consumed but was not usable (for example CRC failure or
   invalid arguments), also leaving the radio idle. Call ble_radio_listen_once()
   or ble_radio_listen() to restore RX after non-zero returns. Do not rely on
   this as an overwrite-safe snapshot after continuous ble_radio_listen(), which
   intentionally uses END->START auto-restart for legacy observer polling. */
int ble_radio_poll_snapshot(UB *buf, UINT *len, UINT *rssi_dbm);

#endif /* BLE_RADIO_H */
