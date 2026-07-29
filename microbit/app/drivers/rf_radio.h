/*
 * rf_radio.h - proprietary RF mesh radio over the raw nRF52833 RADIO.
 *
 * This driver is intentionally separate from ble_radio. It uses Nordic
 * proprietary 1 Mbit mode with one logical address and raw RF mesh packets as
 * the RADIO payload. The kernel is expected to have started the HFXO crystal,
 * matching the existing BLE radio driver assumption.
 */

#ifndef RF_RADIO_H
#define RF_RADIO_H

#include <tk/tkernel.h>

#define RF_RADIO_MAX_PACKET           32U
#define RF_RADIO_CHANNEL_MAX          83U

#ifndef RF_RADIO_DEFAULT_NETWORK_ID
#define RF_RADIO_DEFAULT_NETWORK_ID   0x5452U
#endif

#ifndef RF_RADIO_DEFAULT_CHANNEL
#define RF_RADIO_DEFAULT_CHANNEL      7U
#endif

#ifndef RF_RADIO_DEFAULT_TXPOWER
#define RF_RADIO_DEFAULT_TXPOWER      0x00
#endif

/* Common nRF52833 RADIO TXPOWER register field values. */
#define RF_RADIO_TXPOWER_POS8DBM      0x08
#define RF_RADIO_TXPOWER_POS4DBM      0x04
#define RF_RADIO_TXPOWER_0DBM         0x00
#define RF_RADIO_TXPOWER_NEG4DBM      0xFC
#define RF_RADIO_TXPOWER_NEG8DBM      0xF8
#define RF_RADIO_TXPOWER_NEG12DBM     0xF4
#define RF_RADIO_TXPOWER_NEG16DBM     0xF0
#define RF_RADIO_TXPOWER_NEG20DBM     0xEC
#define RF_RADIO_TXPOWER_NEG40DBM     0xD8

typedef struct {
    UH   network_id;      /* used to derive the RADIO base/prefix address */
    UINT channel;         /* 0..83 => 2400 MHz + channel */
    INT  tx_power;        /* raw TXPOWER field value; 0x00 is 0 dBm */
} rf_radio_config_t;

/* Configure the RADIO for proprietary 1 Mbit RF mesh packets. Pass NULL for
   defaults. This does not start RX; call rf_radio_listen() after init. */
void rf_radio_init(const rf_radio_config_t *cfg);

/* Arm the RADIO for receive until one packet ends. rf_radio_poll() restarts RX
   after handling a completed packet, so the app should poll frequently. The app
   must call this again after each rf_radio_send(); the driver does not assume
   simultaneous TX/RX. */
void rf_radio_listen(void);

/* Non-blocking poll for one CRC-valid RF mesh packet. On success returns 1,
   copies up to RF_RADIO_MAX_PACKET payload bytes into buf, sets *len to the raw
   mesh packet length, and optionally sets *rssi_dbm to the positive RSSI
   magnitude (actual power ~= -rssi_dbm dBm). Returns 0 if no valid packet is
   available. */
int rf_radio_poll(UB *buf, UINT *len, UINT *rssi_dbm);

/* Synchronously send one raw RF mesh packet and return after RADIO is disabled.
   Returns 1 when the bounded TX completed, or 0 for invalid input/timeout. */
int rf_radio_send(const UB *buf, UINT len);

#endif /* RF_RADIO_H */
