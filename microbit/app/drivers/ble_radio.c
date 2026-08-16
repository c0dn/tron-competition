/*
 * ble_radio.c - BLE advertising over the raw nRF RADIO (see ble_radio.h)
 */

#include "ble_radio.h"

#ifdef BLE_RADIO_HOST_TEST
extern UW ble_radio_host_in_w(UW addr);
extern void ble_radio_host_out_w(UW addr, UW value);
#define in_w ble_radio_host_in_w
#define out_w ble_radio_host_out_w
#endif

/* --- nRF52833 RADIO peripheral (base 0x40001000) --- */
#define RADIO_BASE              0x40001000UL
#define R(off)                  (RADIO_BASE + (off))

#define RADIO_TASKS_TXEN        R(0x000)
#define RADIO_TASKS_RXEN        R(0x004)
#define RADIO_TASKS_DISABLE     R(0x010)
#define RADIO_EVENTS_READY      R(0x100)
#define RADIO_EVENTS_END        R(0x10C)
#define RADIO_EVENTS_DISABLED   R(0x110)
#define RADIO_SHORTS            R(0x200)
#define RADIO_CRCSTATUS         R(0x400)
#define RADIO_PACKETPTR         R(0x504)
#define RADIO_FREQUENCY         R(0x508)
#define RADIO_TXPOWER           R(0x50C)
#define RADIO_MODE              R(0x510)
#define RADIO_PCNF0             R(0x514)
#define RADIO_PCNF1             R(0x518)
#define RADIO_BASE0             R(0x51C)
#define RADIO_PREFIX0           R(0x524)
#define RADIO_TXADDRESS         R(0x52C)
#define RADIO_RXADDRESSES       R(0x530)
#define RADIO_CRCCNF            R(0x534)
#define RADIO_CRCPOLY           R(0x538)
#define RADIO_CRCINIT           R(0x53C)
#define RADIO_RSSISAMPLE        R(0x548)
#define RADIO_STATE             R(0x550)
#define RADIO_DATAWHITEIV       R(0x554)

#define SHORT_READY_START       (1UL << 0)
#define SHORT_END_DISABLE       (1UL << 1)
#define SHORT_ADDRESS_RSSISTART (1UL << 4)
#define SHORT_END_START         (1UL << 5)

#define FICR_DEVICEADDR0        0x100000A4UL
#define FICR_DEVICEADDR1        0x100000A8UL

#define BLE_ACCESS_ADDR         0x8E89BED6UL
#define BLE_CRC_POLY            0x0000065BUL
#define BLE_CRC_INIT            0x00555555UL

#define PDU_TYPE_ADV_NONCONN_IND 0x02u
#define PDU_TXADD_RANDOM         (1u << 6)

#define BLE_RADIO_COMPAT_STATE_TIMEOUT_MS 2u

static const UB adv_freq[3] = { 2u, 26u, 80u };
static const UB adv_white[3] = { 37u, 38u, 39u };

/* [S0=header][LENGTH][AdvA(6)][AdvData...] */
static UB pkt[2 + 6 + BLE_ADV_MAX_DATA] __attribute__((aligned(4)));
static UB rx_pkt[BLE_RX_MAX] __attribute__((aligned(4)));

static UINT rx_active;
static UINT rx_channel = 37u;

static void copy_bytes(void *dst, const void *src, UINT length)
{
    UB *out = (UB *)dst;
    const UB *in = (const UB *)src;

    while (length > 0u) {
        *out++ = *in++;
        length--;
    }
}

static UB radio_channel(UINT channel)
{
    return (channel == 37u || channel == 39u) ? (UB)channel : 38u;
}

static int radio_channel_is_valid(UINT channel)
{
    return channel == 37u || channel == 38u || channel == 39u;
}

static UB radio_channel_frequency(UB channel)
{
    return channel == 37u ? 2u : channel == 39u ? 80u : 26u;
}

#ifdef BLE_RADIO_HOST_TEST
static int radio_wait_nonzero(UW address, UINT timeout_ms)
{
    UINT polls = timeout_ms;

    do {
        if (in_w(address) != 0u) {
            return 1;
        }
        polls--;
    } while (polls > 0u);
    return 0;
}

static int radio_wait_disabled(UINT timeout_ms)
{
    UINT polls = timeout_ms;

    do {
        if (in_w(RADIO_STATE) == 0u) {
            return 1;
        }
        polls--;
    } while (polls > 0u);
    return 0;
}
#else
static int radio_time_reached(const SYSTIM *start, UINT timeout_ms)
{
    SYSTIM now;

    if (tk_get_otm(&now) != E_OK) {
        return 1;
    }
    return (uint32_t)(now.lo - start->lo) >= (uint32_t)timeout_ms;
}

static int radio_wait_nonzero(UW address, UINT timeout_ms)
{
    SYSTIM start;

    if (tk_get_otm(&start) != E_OK) {
        return 0;
    }
    do {
        if (in_w(address) != 0u) {
            return 1;
        }
    } while (!radio_time_reached(&start, timeout_ms));
    return 0;
}

static int radio_wait_disabled(UINT timeout_ms)
{
    SYSTIM start;

    if (tk_get_otm(&start) != E_OK) {
        return 0;
    }
    do {
        if (in_w(RADIO_STATE) == 0u) {
            return 1;
        }
    } while (!radio_time_reached(&start, timeout_ms));
    return 0;
}
#endif

static ble_radio_op_result_t radio_try_disable(UINT state_timeout_ms)
{
    if (in_w(RADIO_STATE) != 0u) {
        out_w(RADIO_TASKS_DISABLE, 1u);
        if (!radio_wait_disabled(state_timeout_ms)) {
            rx_active = 0u;
            return BLE_RADIO_OP_STATE_TIMEOUT;
        }
    }
    out_w(RADIO_EVENTS_DISABLED, 0u);
    rx_active = 0u;
    return BLE_RADIO_OP_OK;
}

static void configure_radio(void)
{
    out_w(RADIO_MODE, 3u);                 /* BLE 1 Mbit */
    out_w(RADIO_TXPOWER, 0u);              /* 0 dBm */
    out_w(RADIO_BASE0, (BLE_ACCESS_ADDR << 8) & 0xFFFFFF00UL);
    out_w(RADIO_PREFIX0, (BLE_ACCESS_ADDR >> 24) & 0xFFu);
    out_w(RADIO_TXADDRESS, 0u);
    out_w(RADIO_RXADDRESSES, 1u);
    out_w(RADIO_PCNF0, (8UL << 0) | (1UL << 8));
    out_w(RADIO_PCNF1, 37UL | (3UL << 16) | (1UL << 25));
    out_w(RADIO_CRCCNF, 3UL | (1UL << 8));
    out_w(RADIO_CRCPOLY, BLE_CRC_POLY);
    out_w(RADIO_CRCINIT, BLE_CRC_INIT);
    out_w(RADIO_SHORTS, SHORT_READY_START | SHORT_END_DISABLE);
}

static int default_adva_is_valid(const UB adva[6])
{
    UINT index;
    int random_all_zero = 1;
    int random_all_one = 1;

    if ((adva[5] & 0xC0u) != 0xC0u) {
        return 0;
    }
    for (index = 0u; index < 5u; index++) {
        if (adva[index] != 0u) {
            random_all_zero = 0;
        }
        if (adva[index] != 0xFFu) {
            random_all_one = 0;
        }
    }
    if ((adva[5] & 0x3Fu) != 0u) {
        random_all_zero = 0;
    }
    if ((adva[5] & 0x3Fu) != 0x3Fu) {
        random_all_one = 0;
    }
    return !random_all_zero && !random_all_one;
}

ble_radio_op_result_t ble_radio_read_default_adva(UB out_adva[6])
{
    UB adva[6];
    UW low;
    UW high;

    if (out_adva == NULL) {
        return BLE_RADIO_OP_INVALID_ARGUMENT;
    }

    low = in_w(FICR_DEVICEADDR0);
    high = in_w(FICR_DEVICEADDR1);
    adva[0] = (UB)low;
    adva[1] = (UB)(low >> 8);
    adva[2] = (UB)(low >> 16);
    adva[3] = (UB)(low >> 24);
    adva[4] = (UB)high;
    adva[5] = (UB)(high >> 8) | 0xC0u;
    if (!default_adva_is_valid(adva)) {
        return BLE_RADIO_OP_INVALID_ARGUMENT;
    }
    copy_bytes(out_adva, adva, 6u);
    return BLE_RADIO_OP_OK;
}

ble_radio_op_result_t ble_radio_try_init(UINT state_timeout_ms)
{
    ble_radio_op_result_t result;

    if (state_timeout_ms == 0u) {
        return BLE_RADIO_OP_INVALID_ARGUMENT;
    }
    result = radio_try_disable(state_timeout_ms);
    if (result != BLE_RADIO_OP_OK) {
        return result;
    }
    rx_channel = 37u;
    configure_radio();
    return BLE_RADIO_OP_OK;
}

ble_radio_op_result_t ble_radio_try_idle(UINT state_timeout_ms)
{
    ble_radio_op_result_t result;

    if (state_timeout_ms == 0u) {
        return BLE_RADIO_OP_INVALID_ARGUMENT;
    }
    result = radio_try_disable(state_timeout_ms);
    if (result != BLE_RADIO_OP_OK) {
        return result;
    }
    out_w(RADIO_SHORTS, 0u);
    return BLE_RADIO_OP_OK;
}

static ble_radio_op_result_t radio_try_listen(UINT channel,
                                               UINT state_timeout_ms,
                                               UW shorts)
{
    ble_radio_op_result_t result;
#ifdef BLE_RADIO_HOST_TEST
    UINT snapshot_pending = in_w(RADIO_EVENTS_END) != 0u;
#endif

    result = radio_try_disable(state_timeout_ms);
    if (result != BLE_RADIO_OP_OK) {
        return result;
    }
    rx_channel = channel;
    out_w(RADIO_FREQUENCY, radio_channel_frequency((UB)channel));
    out_w(RADIO_DATAWHITEIV, channel);
    out_w(RADIO_PACKETPTR, (UW)rx_pkt);
    out_w(RADIO_SHORTS, shorts);
#ifndef BLE_RADIO_HOST_TEST
    out_w(RADIO_EVENTS_END, 0u);
#else
    /* The host register shim injects a completed raw PDU before the scheduler
     * programs its first one-shot receive. Preserve that already-completed
     * snapshot while still clearing ordinary stale END state. */
    if (snapshot_pending == 0u) {
        out_w(RADIO_EVENTS_END, 0u);
    }
#endif
    out_w(RADIO_EVENTS_DISABLED, 0u);
    out_w(RADIO_TASKS_RXEN, 1u);
    if (!radio_wait_nonzero(RADIO_STATE, state_timeout_ms)) {
        (void)radio_try_disable(state_timeout_ms);
        return BLE_RADIO_OP_STATE_TIMEOUT;
    }
    rx_active = 1u;
    return BLE_RADIO_OP_OK;
}

ble_radio_op_result_t ble_radio_try_listen_once(UINT channel,
                                                 UINT state_timeout_ms)
{
    if (state_timeout_ms == 0u || !radio_channel_is_valid(channel)) {
        return BLE_RADIO_OP_INVALID_ARGUMENT;
    }
    return radio_try_listen(channel, state_timeout_ms,
                            SHORT_READY_START |
                            SHORT_ADDRESS_RSSISTART |
                            SHORT_END_DISABLE);
}

static void prepare_adv_packet(const UB *adv, UINT adv_len, const UB addr6[6])
{
    pkt[0] = PDU_TYPE_ADV_NONCONN_IND | PDU_TXADD_RANDOM;
    pkt[1] = (UB)(6u + adv_len);
    copy_bytes(&pkt[2], addr6, 6u);
    if (adv_len > 0u) {
        copy_bytes(&pkt[8], adv, adv_len);
    }
}

static int tx_on_channel(INT channel, UINT state_timeout_ms)
{
    out_w(RADIO_FREQUENCY, adv_freq[channel]);
    out_w(RADIO_DATAWHITEIV, adv_white[channel]);
    out_w(RADIO_PACKETPTR, (UW)pkt);
    out_w(RADIO_SHORTS, SHORT_READY_START | SHORT_END_DISABLE);
    out_w(RADIO_EVENTS_READY, 0u);
    out_w(RADIO_EVENTS_END, 0u);
    out_w(RADIO_EVENTS_DISABLED, 0u);
    out_w(RADIO_TASKS_TXEN, 1u);
    return radio_wait_nonzero(RADIO_EVENTS_DISABLED, state_timeout_ms);
}

ble_radio_tx_result_t ble_radio_try_advertise_channels(
    const UB *adv, UINT adv_len, const UB *addr6, UINT channel_mask,
    UINT state_timeout_ms)
{
    ble_radio_tx_result_t result;
    ble_radio_op_result_t idle_result;
    INT channel;

    result.requested_channel_mask = 0u;
    result.completed_channel_mask = 0u;
    result.fault = BLE_RADIO_OP_INVALID_ARGUMENT;
    if (state_timeout_ms == 0u || adv_len > BLE_ADV_MAX_DATA ||
        (adv == NULL && adv_len != 0u) || addr6 == NULL ||
        channel_mask == 0u ||
        (channel_mask & ~BLE_RADIO_ADV_CH_ALL) != 0u) {
        return result;
    }

    result.requested_channel_mask = (uint8_t)channel_mask;
    idle_result = radio_try_disable(state_timeout_ms);
    if (idle_result != BLE_RADIO_OP_OK) {
        result.fault = idle_result;
        return result;
    }

    prepare_adv_packet(adv, adv_len, addr6);
    for (channel = 0; channel < 3; channel++) {
        uint8_t channel_bit = (uint8_t)(1u << channel);

        if ((channel_mask & channel_bit) == 0u) {
            continue;
        }
        if (!tx_on_channel(channel, state_timeout_ms)) {
            (void)radio_try_disable(state_timeout_ms);
            result.fault = BLE_RADIO_OP_STATE_TIMEOUT;
            return result;
        }
        result.completed_channel_mask |= channel_bit;
    }
    result.fault = BLE_RADIO_OP_OK;
    return result;
}

ble_radio_op_result_t ble_radio_try_poll_snapshot(
    UB *buf, UINT *len, UINT *rssi_magnitude_db, UINT state_timeout_ms)
{
    ble_radio_op_result_t idle_result;
    UINT packet_len;

    if (buf == NULL || len == NULL || rssi_magnitude_db == NULL ||
        state_timeout_ms == 0u) {
        return BLE_RADIO_OP_INVALID_ARGUMENT;
    }
    if (in_w(RADIO_EVENTS_END) == 0u) {
        return BLE_RADIO_OP_NO_EVENT;
    }

    idle_result = radio_try_disable(state_timeout_ms);
    if (idle_result != BLE_RADIO_OP_OK) {
        return idle_result;
    }
    out_w(RADIO_EVENTS_END, 0u);
    if ((in_w(RADIO_CRCSTATUS) & 1u) == 0u) {
        return BLE_RADIO_OP_CRC_DROP;
    }

    if (rx_pkt[1] > 37u) {
        return BLE_RADIO_OP_CRC_DROP;
    }
    packet_len = (UINT)rx_pkt[1] + 2u;
    if (packet_len > BLE_RX_MAX) {
        return BLE_RADIO_OP_CRC_DROP;
    }

    *rssi_magnitude_db = in_w(RADIO_RSSISAMPLE) & 0x7Fu;
    copy_bytes(buf, rx_pkt, packet_len);
    *len = packet_len;
    return BLE_RADIO_OP_OK;
}

void ble_radio_init(void)
{
    (void)ble_radio_try_init(BLE_RADIO_COMPAT_STATE_TIMEOUT_MS);
}

void ble_radio_idle(void)
{
    (void)ble_radio_try_idle(BLE_RADIO_COMPAT_STATE_TIMEOUT_MS);
}

void ble_radio_advertise_channels(const UB *adv, UINT adv_len, const UB *addr6,
                                   UINT channel_mask)
{
    UB default_adva[6];
    const UB *selected_adva = addr6;

    if (adv_len > BLE_ADV_MAX_DATA) {
        adv_len = BLE_ADV_MAX_DATA;
    }
    channel_mask &= BLE_RADIO_ADV_CH_ALL;
    if (channel_mask == 0u) {
        channel_mask = BLE_RADIO_ADV_CH_ALL;
    }
    if (selected_adva == NULL) {
        if (ble_radio_read_default_adva(default_adva) != BLE_RADIO_OP_OK) {
            return;
        }
        selected_adva = default_adva;
    }
    (void)ble_radio_try_advertise_channels(
        adv, adv_len, selected_adva, channel_mask,
        BLE_RADIO_COMPAT_STATE_TIMEOUT_MS);
}

void ble_radio_advertise(const UB *adv, UINT adv_len, const UB *addr6)
{
    ble_radio_advertise_channels(adv, adv_len, addr6, BLE_RADIO_ADV_CH_ALL);
}

void ble_radio_listen(UINT channel)
{
    (void)radio_try_listen(radio_channel(channel),
                           BLE_RADIO_COMPAT_STATE_TIMEOUT_MS,
                           SHORT_READY_START |
                           SHORT_ADDRESS_RSSISTART |
                           SHORT_END_START);
}

void ble_radio_listen_once(UINT channel)
{
    (void)ble_radio_try_listen_once(radio_channel(channel),
                                    BLE_RADIO_COMPAT_STATE_TIMEOUT_MS);
}

int ble_radio_poll_snapshot(UB *buf, UINT *len, UINT *rssi_dbm)
{
    ble_radio_op_result_t result = ble_radio_try_poll_snapshot(
        buf, len, rssi_dbm, BLE_RADIO_COMPAT_STATE_TIMEOUT_MS);

    if (result == BLE_RADIO_OP_OK) {
        return 1;
    }
    return result == BLE_RADIO_OP_NO_EVENT ? 0 : -1;
}

int ble_radio_poll(UB *buf, UINT *len, UINT *rssi_dbm)
{
    UINT channel = rx_channel;
    int was_listening = rx_active != 0u;
    int result = ble_radio_poll_snapshot(buf, len, rssi_dbm);

    if (was_listening && rx_active == 0u) {
        ble_radio_listen(channel);
    }
    return result > 0 ? 1 : 0;
}
