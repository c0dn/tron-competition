/*
 * rf_radio.c - proprietary RF mesh radio over the raw nRF52833 RADIO.
 *
 * Packet RAM layout uses an 8-bit hardware LENGTH byte followed by the raw RF
 * mesh packet bytes. The LENGTH byte is not exposed through the public API.
 */

#include "rf_radio.h"

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

#define RADIO_STATE_DISABLED 0UL
#define RADIO_MODE_NRF_1MBIT 0UL

#define RADIO_DISABLE_TIMEOUT 100000UL
#define RADIO_TX_TIMEOUT      200000UL

/* Project-owned logical address: prefix/tag bytes plus the configured network
   id. Split across BASE0/PREFIX0 the same way ble_radio handles BLE addresses. */
#define RF_RADIO_ADDRESS_TAG 0xA55A0000UL

/* 16-bit CRC-CCITT as commonly used by Nordic proprietary-radio examples. */
#define RF_RADIO_CRC_POLY    0x00011021UL
#define RF_RADIO_CRC_INIT    0x0000FFFFUL

static rf_radio_config_t rf_cfg = {
    (UH)RF_RADIO_DEFAULT_NETWORK_ID,
    RF_RADIO_DEFAULT_CHANNEL,
    RF_RADIO_DEFAULT_TXPOWER,
};

static UB tx_pkt[1 + RF_RADIO_MAX_PACKET] __attribute__((aligned(4)));
static UB rx_pkt[1 + RF_RADIO_MAX_PACKET] __attribute__((aligned(4)));

static UINT cfg_channel(void)
{
    if (rf_cfg.channel > RF_RADIO_CHANNEL_MAX) {
        return RF_RADIO_CHANNEL_MAX;
    }
    return rf_cfg.channel;
}

static int radio_disable(void)
{
    UW guard = RADIO_DISABLE_TIMEOUT;

    if (in_w(RADIO_STATE) == RADIO_STATE_DISABLED) {
        return 1;
    }

    out_w(RADIO_EVENTS_DISABLED, 0);
    out_w(RADIO_TASKS_DISABLE, 1);
    while (in_w(RADIO_EVENTS_DISABLED) == 0 && guard-- != 0) {}

    return (in_w(RADIO_EVENTS_DISABLED) != 0) ? 1 : 0;
}

static void configure_address(void)
{
    UW addr = RF_RADIO_ADDRESS_TAG | (UW)rf_cfg.network_id;

    out_w(RADIO_BASE0, (addr << 8) & 0xFFFFFF00UL);
    out_w(RADIO_PREFIX0, (addr >> 24) & 0xFF);
    out_w(RADIO_TXADDRESS, 0);
    out_w(RADIO_RXADDRESSES, 1);
}

static void configure_common(void)
{
    out_w(RADIO_MODE, RADIO_MODE_NRF_1MBIT);
    out_w(RADIO_TXPOWER, ((UW)rf_cfg.tx_power) & 0xFFUL);

    configure_address();

    /* PCNF0: S0=0, LENGTH=8 bits, S1=0. */
    out_w(RADIO_PCNF0, 8UL << 0);
    /* PCNF1: MAXLEN=32, STATLEN=0, BALEN=3 (4-byte address), little-endian,
       whitening enabled. */
    out_w(RADIO_PCNF1, RF_RADIO_MAX_PACKET | (3UL << 16) | (1UL << 25));

    /* 16-bit CRC over the LENGTH/payload bytes, skipping the address. */
    out_w(RADIO_CRCCNF, 2UL | (1UL << 8));
    out_w(RADIO_CRCPOLY, RF_RADIO_CRC_POLY);
    out_w(RADIO_CRCINIT, RF_RADIO_CRC_INIT);
}

static void configure_channel(void)
{
    UINT ch = cfg_channel();

    out_w(RADIO_FREQUENCY, ch);
    out_w(RADIO_DATAWHITEIV, ch & 0x3F);
}

void rf_radio_init(const rf_radio_config_t *cfg)
{
    if (cfg != NULL) {
        rf_cfg = *cfg;
    } else {
        rf_cfg.network_id = (UH)RF_RADIO_DEFAULT_NETWORK_ID;
        rf_cfg.channel = RF_RADIO_DEFAULT_CHANNEL;
        rf_cfg.tx_power = RF_RADIO_DEFAULT_TXPOWER;
    }

    if (rf_cfg.channel > RF_RADIO_CHANNEL_MAX) {
        rf_cfg.channel = RF_RADIO_CHANNEL_MAX;
    }

    if (radio_disable()) {
        configure_common();
        configure_channel();
        out_w(RADIO_SHORTS, 0);
    }
}

void rf_radio_listen(void)
{
    if (!radio_disable()) {
        return;
    }

    configure_common();
    configure_channel();
    rx_pkt[0] = 0;
    out_w(RADIO_PACKETPTR, (UW)rx_pkt);

    /* ready->start, sample RSSI on address match, and disable at packet end.
       With one RX buffer this keeps the CRC-validated packet stable until
       rf_radio_poll() copies it; poll restarts RX after handling the packet. */
    out_w(RADIO_SHORTS,
          SHORT_READY_START | SHORT_ADDRESS_RSSISTART | SHORT_END_DISABLE);

    out_w(RADIO_EVENTS_READY, 0);
    out_w(RADIO_EVENTS_END, 0);
    out_w(RADIO_EVENTS_DISABLED, 0);
    out_w(RADIO_TASKS_RXEN, 1);
}

int rf_radio_poll(UB *buf, UINT *len, UINT *rssi_dbm)
{
    UINT n, i;
    UINT rssi;

    if (buf == NULL || len == NULL) {
        return 0;
    }

    if (in_w(RADIO_EVENTS_END) == 0) {
        return 0;
    }
    out_w(RADIO_EVENTS_END, 0);

    rssi = in_w(RADIO_RSSISAMPLE) & 0x7F;

    if ((in_w(RADIO_CRCSTATUS) & 1) == 0) {
        rf_radio_listen();
        return 0;
    }

    n = (UINT)rx_pkt[0];
    if (n > RF_RADIO_MAX_PACKET) {
        rf_radio_listen();
        return 0;
    }

    for (i = 0; i < n; i++) {
        buf[i] = rx_pkt[1 + i];
    }
    *len = n;
    if (rssi_dbm != NULL) {
        *rssi_dbm = rssi;
    }
    rf_radio_listen();
    return 1;
}

int rf_radio_send(const UB *buf, UINT len)
{
    UINT i;
    UW guard = RADIO_TX_TIMEOUT;

    if (buf == NULL || len > RF_RADIO_MAX_PACKET) {
        return 0;
    }

    tx_pkt[0] = (UB)len;
    for (i = 0; i < len; i++) {
        tx_pkt[1 + i] = buf[i];
    }

    if (!radio_disable()) {
        return 0;
    }

    configure_common();
    configure_channel();
    out_w(RADIO_PACKETPTR, (UW)tx_pkt);
    out_w(RADIO_SHORTS, SHORT_READY_START | SHORT_END_DISABLE);

    out_w(RADIO_EVENTS_READY, 0);
    out_w(RADIO_EVENTS_END, 0);
    out_w(RADIO_EVENTS_DISABLED, 0);
    out_w(RADIO_TASKS_TXEN, 1);

    while (in_w(RADIO_EVENTS_DISABLED) == 0 && guard-- != 0) {}
    if (in_w(RADIO_EVENTS_DISABLED) == 0) {
        (void)radio_disable();
        return 0;
    }

    out_w(RADIO_SHORTS, 0);
    out_w(RADIO_EVENTS_READY, 0);
    out_w(RADIO_EVENTS_END, 0);
    return 1;
}
