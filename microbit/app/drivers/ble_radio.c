/*
 * ble_radio.c - BLE advertising over the raw nRF RADIO (see ble_radio.h)
 *
 * The nRF RADIO can produce BLE-format packets directly when configured
 * for the BLE access address, CRC-24, per-channel data whitening, and
 * GFSK 1 Mbit. Each advertising event transmits the same PDU on the three
 * advertising channels in turn.
 *
 * TX and RX use incompatible SHORTS wiring, so every mode switch sets its own
 * and passes through DISABLED on the way. Getting this wrong is not a
 * performance detail: transmitting with the receiver's SHORTS in place wedges
 * the busy-wait below forever. See the SHORTS note on radio_tx_one(), and
 * app/mesh_echo for the regression test that catches it.
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

/* --- Hardware receive counter: PPI ch0 routes RADIO EVENTS_END to TIMER3 ---
 *
 * SHORT_END_START restarts the receiver into one buffer, so a packet arriving
 * before the next poll replaces its predecessor, and EVENTS_END - a single flag
 * - cannot record that it happened. Software cannot count what it never sees,
 * and a software ring would not help either: the overwrite is done by the
 * radio's own DMA before any copy could run.
 *
 * A timer in counter mode, incremented directly by the END event over PPI, sees
 * every packet whether or not software got there. The difference between it and
 * the poll count is the real loss. Without that figure, a mesh quietly dropping
 * most of its traffic is indistinguishable from one that is working.
 *
 * TIMER3 is physical timer 4 in micro T-Kernel/SM terms, and USE_PTMR is 1 in
 * every app config here, so it is reachable through StartPhysicalTimer(). It is
 * driven directly anyway because the ptimer API has no counter mode - it always
 * programs MODE=0 (timer) off the 16 MHz clock. Physical timer 4 is therefore
 * RESERVED: do not call StartPhysicalTimer(4, ...) anywhere in this tree.
 * hw_timer.c takes physical timers 2 and 3 and documents the same split.
 */
#define PPI_BASE            0x4001F000UL
#define PPI_CHENSET         (PPI_BASE + 0x504)
#define PPI_CHENCLR         (PPI_BASE + 0x508)
#define PPI_CH0_EEP         (PPI_BASE + 0x510)
#define PPI_CH0_TEP         (PPI_BASE + 0x514)
#define PPI_CH_RXCOUNT      (1UL << 0)

#define T3_BASE             0x4001A000UL    /* TIMER3 == physical timer 4 */
#define T3_TASKS_START      (T3_BASE + 0x000)
#define T3_TASKS_COUNT      (T3_BASE + 0x008)
#define T3_TASKS_CLEAR      (T3_BASE + 0x00C)
#define T3_TASKS_CAPTURE0   (T3_BASE + 0x040)
#define T3_MODE             (T3_BASE + 0x504)
#define T3_BITMODE          (T3_BASE + 0x508)
#define T3_CC0              (T3_BASE + 0x540)
#define T3_MODE_COUNTER     1UL
#define T3_BITMODE_32       3UL

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

/* Receive landing buffer. SHORT_END_START restarts RX into this same buffer, so
   a packet arriving before the next poll overwrites its predecessor silently.
   EVENTS_END is a single flag and cannot record that it happened. Counting the
   loss needs hardware help (PPI routing EVENTS_END to a counter) rather than a
   software ring, which would not stop the overwrite - deferred with the
   hardware-timer work rather than faked here. */
static UB rx_pkt[BLE_RX_MAX] __attribute__((aligned(4)));

/* Radio ownership. Advertising and relaying run on different tasks, so every
   entry point that touches the peripheral serialises here. TA_INHERIT keeps a
   low-priority relay from blocking the detector's own advert; the critical
   sections are short (a 3-channel burst is ~1.2 ms). */
static ID   radio_mtx = 0;

/* Receive state, so a transmit can put the receiver back exactly as it was.
   Without this a node goes deaf the first time it transmits. */
static UINT rx_ch = 37;
static BOOL rx_active = FALSE;

/* Software-side receive tallies, compared against the TIMER2 hardware count. */
static UW rx_observed = 0;
static UW rx_crc_err = 0;

static void radio_lock(void)
{
    if (radio_mtx > 0) {
        tk_loc_mtx(radio_mtx, TMO_FEVR);
    }
}

static void radio_unlock(void)
{
    if (radio_mtx > 0) {
        tk_unl_mtx(radio_mtx);
    }
}

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
    T_CMTX cmtx = { .exinf = NULL, .mtxatr = TA_INHERIT, .ceilpri = 0 };

    if (radio_mtx <= 0) {
        radio_mtx = tk_cre_mtx(&cmtx);  /* <=0: single-task caller, run unlocked */
    }

    radio_disable();
    rx_active = FALSE;
    rx_observed = 0;
    rx_crc_err = 0;

    /* TIMER3 counts RADIO END events fed to it over PPI (see the note above). */
    out_w(T3_MODE, T3_MODE_COUNTER);
    out_w(T3_BITMODE, T3_BITMODE_32);
    out_w(T3_TASKS_CLEAR, 1);
    out_w(T3_TASKS_START, 1);
    out_w(PPI_CH0_EEP, RADIO_EVENTS_END);
    out_w(PPI_CH0_TEP, T3_TASKS_COUNT);
    out_w(PPI_CHENSET, PPI_CH_RXCOUNT);

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

/* Transmit the staged packet on one advertising channel.
 *
 * SHORTS and radio_disable() here are load-bearing, not defensive tidiness.
 * ble_radio_listen() leaves SHORTS = READY_START | ADDRESS_RSSISTART |
 * END_START and the peripheral in RX. Inheriting that state would sequence a
 * transmit TXEN -> READY -> START -> END -> START -> ... so EVENTS_DISABLED is
 * never raised and the wait below never returns; keying TASKS_TXEN out of RX is
 * invalid regardless, since the state machine has to pass through DISABLED.
 * Setting both unconditionally makes a transmit independent of whatever ran
 * before it. app/mesh_echo exists to catch a regression here.
 */
static void radio_tx_one(INT ch)
{
    radio_disable();
    out_w(RADIO_SHORTS, SHORT_READY_START | SHORT_END_DISABLE);

    out_w(RADIO_FREQUENCY, adv_freq[ch]);
    out_w(RADIO_DATAWHITEIV, adv_white[ch]);
    out_w(RADIO_PACKETPTR, (UW)pkt);

    out_w(RADIO_EVENTS_READY, 0);
    out_w(RADIO_EVENTS_END, 0);
    out_w(RADIO_EVENTS_DISABLED, 0);
    out_w(RADIO_TASKS_TXEN, 1);
    while (in_w(RADIO_EVENTS_DISABLED) == 0) {}   /* shorts: ready->start->end->disable */
}

/* Reconfigure for continuous receive. Caller must hold the radio lock. */
static void listen_locked(UINT channel)
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

    rx_ch = channel;
    rx_active = TRUE;
}

void ble_radio_set_txpower(INT dbm)
{
    /* nRF52833 accepts a fixed set: +8..+2, 0, -4, -8, -12, -16, -20, -30, -40.
       The register is two's complement dBm, so the cast covers all of them.
       Useful for shrinking a node's range on the bench to force a multi-hop
       topology onto a desk instead of a corridor. */
    radio_lock();
    out_w(RADIO_TXPOWER, (UW)(dbm & 0xFF));
    radio_unlock();
}

void ble_radio_tx(const UB *adv, UINT adv_len, const UB *addr6, UB chan_mask)
{
    UINT i;
    INT ch;

    if (adv_len > BLE_ADV_MAX_DATA) {
        adv_len = BLE_ADV_MAX_DATA;
    }

    radio_lock();

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

    /* END fires on transmit completion too, and counting those would inflate
       the receive tally. Unhook the counter for the duration of the burst. */
    out_w(PPI_CHENCLR, PPI_CH_RXCOUNT);

    for (ch = 0; ch < 3; ch++) {
        if (chan_mask & (1U << ch)) {
            radio_tx_one(ch);
        }
    }

    out_w(PPI_CHENSET, PPI_CH_RXCOUNT);

    /* Hand the radio back to the receiver if it was listening before. A relay
       transmits constantly; without this it hears exactly one packet ever. */
    if (rx_active) {
        listen_locked(rx_ch);
    }

    radio_unlock();
}

void ble_radio_advertise(const UB *adv, UINT adv_len, const UB *addr6)
{
    ble_radio_tx(adv, adv_len, addr6, BLE_CHAN_MASK_ALL);
}

/* --- Observer (passive scan) side --- */

void ble_radio_listen(UINT channel)
{
    radio_lock();
    listen_locked(channel);
    radio_unlock();
}

int ble_radio_poll_ex(ble_rx_t *out)
{
    UINT n, i;
    SYSTIM t;

    radio_lock();

    if (in_w(RADIO_EVENTS_END) == 0) {
        radio_unlock();
        return 0;                        /* nothing received yet */
    }
    out_w(RADIO_EVENTS_END, 0);
    rx_observed++;      /* one END serviced, whatever its CRC turns out to be */

    /* RSSISAMPLE is positive; actual power = -RSSISAMPLE dBm */
    out->rssi_dbm = in_w(RADIO_RSSISAMPLE) & 0x7F;

    if ((in_w(RADIO_CRCSTATUS) & 1) == 0) {
        rx_crc_err++;
        radio_unlock();
        return 0;                        /* CRC error - drop */
    }

    n = (UINT)rx_pkt[1] + 2;             /* S0 + LENGTH + payload(LENGTH) */
    if (n > BLE_RX_MAX) {
        n = BLE_RX_MAX;
    }
    for (i = 0; i < n; i++) {
        out->pdu[i] = rx_pkt[i];
    }
    out->len = n;
    out->ch = (UB)rx_ch;

    radio_unlock();

    /* Millisecond stamp for cache aging. Sub-tick relay backoff needs better
       than this and will come from a hardware timer, not the kernel clock. */
    out->t_ms = (tk_get_otm(&t) == E_OK) ? (UW)t.lo : 0;
    return 1;
}

void ble_radio_stats(ble_radio_stats_t *out)
{
    UW hw;

    radio_lock();
    out_w(T3_TASKS_CAPTURE0, 1);
    hw = in_w(T3_CC0);

    out->hw_end   = hw;
    out->observed = rx_observed;
    out->crc_err  = rx_crc_err;
    /* Unsigned, so a transient hw < observed (a TX END slipping in at the
       moment the counter was unhooked) reads as 0 rather than a huge number. */
    out->dropped  = (hw > rx_observed) ? (hw - rx_observed) : 0;
    radio_unlock();
}

int ble_radio_poll(UB *buf, UINT *len, UINT *rssi_dbm)
{
    ble_rx_t rx;
    UINT i;

    if (!ble_radio_poll_ex(&rx)) {
        return 0;
    }
    for (i = 0; i < rx.len; i++) {
        buf[i] = rx.pdu[i];
    }
    *len = rx.len;
    *rssi_dbm = rx.rssi_dbm;
    return 1;
}
