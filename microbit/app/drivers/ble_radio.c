/*
 * ble_radio.c - BLE advertising over the raw nRF RADIO (see ble_radio.h)
 *
 * The nRF RADIO can produce BLE-format packets directly when configured
 * for the BLE access address, CRC-24, per-channel data whitening, and
 * GFSK 1 Mbit. Each advertising event transmits the same PDU on the three
 * advertising channels in turn.
 */

#include "ble_radio.h"

/* --- nRF52833 RADIO peripheral (base 0x40001000) --- */
#define RADIO_BASE          0x40001000UL
#define R(off)              (RADIO_BASE + (off))

#define RADIO_TASKS_TXEN    R(0x000)
#define RADIO_TASKS_RXEN    R(0x004)
#define RADIO_TASKS_DISABLE R(0x010)
#define RADIO_EVENTS_READY  R(0x100)
#define RADIO_EVENTS_END    R(0x10C)
#define RADIO_EVENTS_DISABLED R(0x110)
#define RADIO_SHORTS        R(0x200)
#define RADIO_CRCSTATUS     R(0x400)
#define RADIO_PACKETPTR     R(0x504)
#define RADIO_FREQUENCY     R(0x508)
#define RADIO_TXPOWER       R(0x50C)
#define RADIO_MODE          R(0x510)
#define RADIO_PCNF0         R(0x514)
#define RADIO_PCNF1         R(0x518)
#define RADIO_BASE0         R(0x51C)
#define RADIO_PREFIX0       R(0x524)
#define RADIO_TXADDRESS     R(0x52C)
#define RADIO_RXADDRESSES   R(0x530)
#define RADIO_CRCCNF        R(0x534)
#define RADIO_CRCPOLY       R(0x538)
#define RADIO_CRCINIT       R(0x53C)
#define RADIO_RSSISAMPLE    R(0x548)
#define RADIO_STATE         R(0x550)
#define RADIO_DATAWHITEIV   R(0x554)

#define SHORT_READY_START   (1UL << 0)
#define SHORT_END_DISABLE   (1UL << 1)
#define SHORT_ADDRESS_RSSISTART (1UL << 4)
#define SHORT_END_START     (1UL << 5)

#define FICR_DEVICEADDR0    0x100000A4UL    /* device address (random) */
#define FICR_DEVICEADDR1    0x100000A8UL

/* BLE advertising physical-channel parameters */
#define BLE_ACCESS_ADDR     0x8E89BED6UL
#define BLE_CRC_POLY        0x0000065BUL     /* x^24+x^10+x^9+x^6+x^4+x^3+x+1 */
#define BLE_CRC_INIT        0x00555555UL

/* ADV_NONCONN_IND, TxAdd=1 (random address) -> PDU header type byte */
#define PDU_TYPE_ADV_NONCONN_IND  0x02
#define PDU_TXADD_RANDOM          (1U << 6)

/* The 3 advertising channels: RF frequency (MHz above 2400) and the
   whitening init value (channel index | 0x40). */
static const UB adv_freq[3]  = { 2, 26, 80 };   /* 2402, 2426, 2480 MHz */
static const UB adv_white[3] = { 37, 38, 39 };

/* On-air packet: [S0=header][LENGTH][payload...]. Word-aligned for
   PACKETPTR. payload = AdvA(6) + AdvData. */
static UB pkt[2 + 6 + BLE_ADV_MAX_DATA] __attribute__((aligned(4)));

static void radio_disable(void)
{
    if (in_w(RADIO_STATE) != 0) {       /* 0 == Disabled */
        out_w(RADIO_EVENTS_DISABLED, 0);
        out_w(RADIO_TASKS_DISABLE, 1);
        while (in_w(RADIO_EVENTS_DISABLED) == 0) {}
    }
}

void ble_radio_init(void)
{
    radio_disable();

    out_w(RADIO_MODE, 3);               /* Ble_1Mbit */
    out_w(RADIO_TXPOWER, 0);            /* 0 dBm */

    /* BLE access address split across PREFIX0 (MSB) + BASE0 (lower 3 bytes) */
    out_w(RADIO_BASE0, (BLE_ACCESS_ADDR << 8) & 0xFFFFFF00UL);
    out_w(RADIO_PREFIX0, (BLE_ACCESS_ADDR >> 24) & 0xFF);
    out_w(RADIO_TXADDRESS, 0);
    out_w(RADIO_RXADDRESSES, 1);

    /* PCNF0: S0=1 byte, LENGTH=8 bits, S1=0 (BLE legacy on 1M) */
    out_w(RADIO_PCNF0, (8UL << 0) | (1UL << 8));
    /* PCNF1: MAXLEN=37, BALEN=3 (4-byte address), little-endian, whitening on */
    out_w(RADIO_PCNF1, 37UL | (3UL << 16) | (1UL << 25));

    /* 24-bit CRC computed over the PDU (skip the address) */
    out_w(RADIO_CRCCNF, 3UL | (1UL << 8));
    out_w(RADIO_CRCPOLY, BLE_CRC_POLY);
    out_w(RADIO_CRCINIT, BLE_CRC_INIT);

    out_w(RADIO_SHORTS, SHORT_READY_START | SHORT_END_DISABLE);
}

static void tx_on_channel(INT ch)
{
    out_w(RADIO_FREQUENCY, adv_freq[ch]);
    out_w(RADIO_DATAWHITEIV, adv_white[ch]);
    out_w(RADIO_PACKETPTR, (UW)pkt);

    out_w(RADIO_EVENTS_READY, 0);
    out_w(RADIO_EVENTS_END, 0);
    out_w(RADIO_EVENTS_DISABLED, 0);
    out_w(RADIO_TASKS_TXEN, 1);
    while (in_w(RADIO_EVENTS_DISABLED) == 0) {}   /* shorts: ready->start->end->disable */
}

void ble_radio_advertise(const UB *adv, UINT adv_len, const UB *addr6)
{
    UINT i;
    INT ch;

    if (adv_len > BLE_ADV_MAX_DATA) {
        adv_len = BLE_ADV_MAX_DATA;
    }

    pkt[0] = PDU_TYPE_ADV_NONCONN_IND | PDU_TXADD_RANDOM;   /* S0: header */
    pkt[1] = (UB)(6 + adv_len);                             /* LENGTH: AdvA + data */

    if (addr6 != NULL) {
        for (i = 0; i < 6; i++) {
            pkt[2 + i] = addr6[i];
        }
    } else {
        /* random static address from FICR (top 2 bits = 11) */
        UW a0 = in_w(FICR_DEVICEADDR0);
        UW a1 = in_w(FICR_DEVICEADDR1);
        pkt[2] = (UB)(a0);
        pkt[3] = (UB)(a0 >> 8);
        pkt[4] = (UB)(a0 >> 16);
        pkt[5] = (UB)(a0 >> 24);
        pkt[6] = (UB)(a1);
        pkt[7] = (UB)(a1 >> 8) | 0xC0;     /* MSB top bits = 11 (random static) */
    }

    for (i = 0; i < adv_len; i++) {
        pkt[8 + i] = adv[i];
    }

    for (ch = 0; ch < 3; ch++) {
        tx_on_channel(ch);
    }
}

/* --- Observer (passive scan) side --- */

static UB rx_pkt[BLE_RX_MAX] __attribute__((aligned(4)));

void ble_radio_listen(UINT channel)
{
    UB freq = (channel == 37) ? 2 : (channel == 39) ? 80 : 26;

    radio_disable();
    out_w(RADIO_FREQUENCY, freq);
    out_w(RADIO_DATAWHITEIV, channel);
    out_w(RADIO_PACKETPTR, (UW)rx_pkt);
    /* ready->start, sample RSSI on address match, auto-restart after each
       packet to stay in continuous receive */
    out_w(RADIO_SHORTS,
          SHORT_READY_START | SHORT_ADDRESS_RSSISTART | SHORT_END_START);

    out_w(RADIO_EVENTS_END, 0);
    out_w(RADIO_TASKS_RXEN, 1);
}

int ble_radio_poll(UB *buf, UINT *len, UINT *rssi_dbm)
{
    UINT n, i;

    if (in_w(RADIO_EVENTS_END) == 0) {
        return 0;                        /* nothing received yet */
    }
    out_w(RADIO_EVENTS_END, 0);

    /* RSSISAMPLE is positive; actual power = -RSSISAMPLE dBm */
    *rssi_dbm = in_w(RADIO_RSSISAMPLE) & 0x7F;

    if ((in_w(RADIO_CRCSTATUS) & 1) == 0) {
        return 0;                        /* CRC error - drop */
    }

    n = (UINT)rx_pkt[1] + 2;             /* S0 + LENGTH + payload(LENGTH) */
    if (n > BLE_RX_MAX) {
        n = BLE_RX_MAX;
    }
    for (i = 0; i < n; i++) {
        buf[i] = rx_pkt[i];
    }
    *len = n;
    return 1;
}
