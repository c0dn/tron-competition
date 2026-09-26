#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef TEST_BLE_RADIO_DRIVER
#include "ble_radio.h"
#else
#include "ble_mesh_scheduler.h"
#endif

#ifdef TEST_BLE_RADIO_DRIVER

#define HOST_RADIO_BASE          0x40001000UL
#define HOST_R(off)              (HOST_RADIO_BASE + (off))
#define HOST_RADIO_TASKS_TXEN    HOST_R(0x000)
#define HOST_RADIO_TASKS_RXEN    HOST_R(0x004)
#define HOST_RADIO_TASKS_DISABLE HOST_R(0x010)
#define HOST_RADIO_EVENTS_END    HOST_R(0x10C)
#define HOST_RADIO_EVENTS_DISABLED HOST_R(0x110)
#define HOST_RADIO_SHORTS        HOST_R(0x200)
#define HOST_RADIO_CRCSTATUS     HOST_R(0x400)
#define HOST_RADIO_PACKETPTR     HOST_R(0x504)
#define HOST_RADIO_RSSISAMPLE    HOST_R(0x548)
#define HOST_RADIO_STATE         HOST_R(0x550)
#define HOST_FICR_DEVICEADDR0    0x100000A4UL
#define HOST_FICR_DEVICEADDR1    0x100000A8UL

#define HOST_SHORT_READY_START   (1UL << 0)
#define HOST_SHORT_END_DISABLE   (1UL << 1)
#define HOST_SHORT_ADDRESS_RSSISTART (1UL << 4)
#define HOST_SHORT_END_START     (1UL << 5)
#define HOST_TX_SHORTS           (HOST_SHORT_READY_START | HOST_SHORT_END_DISABLE)
#define HOST_RX_ONCE_SHORTS      (HOST_SHORT_READY_START | HOST_SHORT_ADDRESS_RSSISTART | HOST_SHORT_END_DISABLE)
#define HOST_RX_CONT_SHORTS      (HOST_SHORT_READY_START | HOST_SHORT_ADDRESS_RSSISTART | HOST_SHORT_END_START)

static unsigned int g_failures;
static UW g_radio_state;
static UW g_radio_shorts;
static UW g_radio_events_end;
static UW g_radio_events_disabled;
static UW g_radio_crcstatus;
static UW g_radio_rssisample;
static UW g_radio_packetptr;
static unsigned int g_txen_calls;
static unsigned int g_txen_missing_tx_shorts;
static unsigned int g_rxen_calls;
static unsigned int g_disable_loses_event;

#define ASSERT_TRUE(expr, msg) \
    do { \
        if (!(expr)) { \
            printf("FAIL: %s\n", (msg)); \
            g_failures++; \
        } \
    } while (0)

#define ASSERT_EQ_U(expected, actual, msg) \
    do { \
        unsigned long e__ = (unsigned long)(expected); \
        unsigned long a__ = (unsigned long)(actual); \
        if (e__ != a__) { \
            printf("FAIL: %s expected=%lu actual=%lu\n", (msg), e__, a__); \
            g_failures++; \
        } \
    } while (0)

UW ble_radio_host_in_w(UW addr)
{
    switch (addr) {
    case HOST_RADIO_STATE:
        return g_radio_state;
    case HOST_RADIO_SHORTS:
        return g_radio_shorts;
    case HOST_RADIO_EVENTS_END:
        return g_radio_events_end;
    case HOST_RADIO_EVENTS_DISABLED:
        return g_radio_events_disabled;
    case HOST_RADIO_CRCSTATUS:
        return g_radio_crcstatus;
    case HOST_RADIO_RSSISAMPLE:
        return g_radio_rssisample;
    case HOST_RADIO_PACKETPTR:
        return g_radio_packetptr;
    case HOST_FICR_DEVICEADDR0:
        return 0x11223344UL;
    case HOST_FICR_DEVICEADDR1:
        return 0x00005566UL;
    default:
        return 0;
    }
}

void ble_radio_host_out_w(UW addr, UW value)
{
    switch (addr) {
    case HOST_RADIO_TASKS_TXEN:
        g_txen_calls++;
        if (g_radio_shorts != HOST_TX_SHORTS) {
            g_txen_missing_tx_shorts++;
        }
        g_radio_state = 0;
        g_radio_events_disabled = 1;
        break;
    case HOST_RADIO_TASKS_RXEN:
        g_rxen_calls++;
        g_radio_state = 3;
        break;
    case HOST_RADIO_TASKS_DISABLE:
        g_radio_state = 0;
        if (g_disable_loses_event == 0u) {
            g_radio_events_disabled = 1;
        }
        break;
    case HOST_RADIO_SHORTS:
        g_radio_shorts = value;
        break;
    case HOST_RADIO_EVENTS_END:
        g_radio_events_end = value;
        break;
    case HOST_RADIO_EVENTS_DISABLED:
        g_radio_events_disabled = value;
        break;
    case HOST_RADIO_PACKETPTR:
        g_radio_packetptr = value;
        break;
    default:
        break;
    }
}

static void reset_host_radio(void)
{
    g_radio_state = 0;
    g_radio_shorts = 0;
    g_radio_events_end = 0;
    g_radio_events_disabled = 0;
    g_radio_crcstatus = 0;
    g_radio_rssisample = 0;
    g_radio_packetptr = 0;
    g_txen_calls = 0;
    g_txen_missing_tx_shorts = 0;
    g_rxen_calls = 0;
    g_disable_loses_event = 0;
}

static void test_driver_tx_restores_shortcuts_after_idle(void)
{
    static const UB addr[6] = { 1u, 2u, 3u, 4u, 5u, 6u };
    UB adv[3] = { 0x02u, 0x01u, 0x06u };

    reset_host_radio();
    ble_radio_init();
    ble_radio_idle();
    ASSERT_EQ_U(0u, g_radio_shorts, "idle clears radio shortcuts in test setup");

    ble_radio_advertise_channels(adv, sizeof(adv), addr, BLE_RADIO_ADV_CH37);

    ASSERT_EQ_U(1u, g_txen_calls, "single-channel advertise starts one TX");
    ASSERT_EQ_U(0u, g_txen_missing_tx_shorts,
                "TXEN always sees READY_START|END_DISABLE shortcuts restored");
    ASSERT_EQ_U(HOST_TX_SHORTS, g_radio_shorts, "TX leaves TX shortcuts configured");
}

static void test_driver_tx_restores_shortcuts_after_rx(void)
{
    static const UB addr[6] = { 6u, 5u, 4u, 3u, 2u, 1u };
    UB adv[1] = { 0x00u };

    reset_host_radio();
    ble_radio_init();
    ble_radio_listen(38u);
    ASSERT_EQ_U(HOST_RX_CONT_SHORTS, g_radio_shorts,
                "continuous listen configures RX auto-restart shortcuts");

    ble_radio_advertise_channels(adv, sizeof(adv), addr, BLE_RADIO_ADV_CH38);

    ASSERT_EQ_U(1u, g_txen_calls, "single-channel advertise after RX starts one TX");
    ASSERT_EQ_U(0u, g_txen_missing_tx_shorts,
                "TXEN does not inherit RX shortcuts after listen");
    ASSERT_EQ_U(HOST_TX_SHORTS, g_radio_shorts, "TX overwrites RX shortcuts");
}

static void test_driver_listen_once_snapshot(void)
{
    UB *rx;
    UB buf[BLE_RX_MAX];
    UINT len = 0u;
    UINT rssi = 0u;

    reset_host_radio();
    ble_radio_init();
    ble_radio_listen_once(37u);

    ASSERT_EQ_U(1u, g_rxen_calls, "listen_once starts RX once");
    ASSERT_EQ_U(HOST_RX_ONCE_SHORTS, g_radio_shorts,
                "listen_once uses END_DISABLE instead of END_START");
    ASSERT_TRUE((g_radio_shorts & HOST_SHORT_END_START) == 0u,
                "listen_once does not configure RX auto-restart");

    rx = (UB *)g_radio_packetptr;
    rx[0] = 0x42u;
    rx[1] = 7u;
    rx[2] = 0xA0u;
    rx[3] = 0xA1u;
    rx[4] = 0xA2u;
    rx[5] = 0xA3u;
    rx[6] = 0xA4u;
    rx[7] = 0xA5u;
    rx[8] = 0x99u;

    g_radio_state = 0;
    g_radio_events_end = 1u;
    g_radio_crcstatus = 1u;
    g_radio_rssisample = 47u;

    ASSERT_EQ_U(1u, ble_radio_poll_snapshot(buf, &len, &rssi),
                "snapshot copies completed one-shot RX packet");
    ASSERT_EQ_U(9u, len, "snapshot length comes from PDU length byte");
    ASSERT_EQ_U(47u, rssi, "snapshot copies RSSI sample");
    ASSERT_EQ_U(0x99u, buf[8], "snapshot copies AdvData byte");
    ASSERT_EQ_U(0u, g_radio_events_end, "snapshot consumes END event");
}

static void test_driver_disable_tolerates_lost_disabled_event(void)
{
    UB *rx;
    UB buf[BLE_RX_MAX];
    UINT len = 0u;
    UINT rssi = 0u;

    reset_host_radio();
    ble_radio_init();
    ble_radio_listen_once(37u);

    rx = (UB *)g_radio_packetptr;
    rx[0] = 0x42u;
    rx[1] = 6u;
    rx[2] = 0xA0u;
    rx[3] = 0xA1u;
    rx[4] = 0xA2u;
    rx[5] = 0xA3u;
    rx[6] = 0xA4u;
    rx[7] = 0xA5u;

    g_radio_state = 3u;
    g_radio_events_end = 1u;
    g_radio_crcstatus = 1u;
    g_radio_rssisample = 48u;
    g_disable_loses_event = 1u;

    ASSERT_EQ_U(1u, ble_radio_poll_snapshot(buf, &len, &rssi),
                "snapshot tolerates disable race with lost DISABLED event");
    ASSERT_EQ_U(0u, g_radio_state, "radio is disabled after race-safe snapshot");
    ASSERT_EQ_U(0u, g_radio_events_disabled, "stale/lost DISABLED event is not required");
    ASSERT_EQ_U(8u, len, "snapshot still copies the completed packet");
}

int main(void)
{
    test_driver_tx_restores_shortcuts_after_idle();
    test_driver_tx_restores_shortcuts_after_rx();
    test_driver_listen_once_snapshot();
    test_driver_disable_tolerates_lost_disabled_event();

    if (g_failures != 0u) {
        printf("ble_radio driver host tests failed: %u\n", g_failures);
        return 1;
    }

    printf("ble_radio driver host tests passed\n");
    return 0;
}

#else

#define OP_LOG_MAX 256u

static unsigned int g_failures;
static char g_ops[OP_LOG_MAX];
static unsigned int g_op_count;

static unsigned int g_listen_calls;
static unsigned int g_listen_once_calls;
static unsigned int g_continuous_listen_calls;
static unsigned int g_idle_calls;
static unsigned int g_advertise_calls;
static unsigned int g_snapshot_calls;
static unsigned int g_poll_calls;
static unsigned int g_try_listen_once_calls;
static unsigned int g_try_advertise_calls;
static unsigned int g_try_advertise_sweeps_calls;
static unsigned int g_try_snapshot_calls;
static UINT g_last_listen_channel;
static UINT g_last_adv_mask;
static UINT g_last_adv_len;
static UB g_last_adv_data[BLE_ADV_MAX_DATA];
static UB g_last_adv_addr[6];
static int g_last_adv_addr_present;
static int g_radio_listening;
static int g_radio_listening_once;

static int g_rx_available;
static int g_rx_result;
static UB g_rx_pdu[BLE_RX_MAX];
static UINT g_rx_pdu_len;
static UINT g_rx_rssi;
static int g_snapshot_saw_listening;
static int g_snapshot_saw_one_shot;
static int g_snapshot_disabled_before_copy;
static ble_radio_op_result_t g_listen_result;
static ble_radio_op_result_t g_snapshot_result;
static ble_radio_op_result_t g_tx_fault;
static uint8_t g_tx_completed_channel_mask;

static const UB g_local_adva[6] = {
    0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu,
};

#define ASSERT_TRUE(expr, msg) \
    do { \
        if (!(expr)) { \
            printf("FAIL: %s\n", (msg)); \
            g_failures++; \
        } \
    } while (0)

#define ASSERT_EQ_U(expected, actual, msg) \
    do { \
        unsigned long e__ = (unsigned long)(expected); \
        unsigned long a__ = (unsigned long)(actual); \
        if (e__ != a__) { \
            printf("FAIL: %s expected=%lu actual=%lu\n", (msg), e__, a__); \
            g_failures++; \
        } \
    } while (0)

static void record_op(char op)
{
    if (g_op_count + 1u < OP_LOG_MAX) {
        g_ops[g_op_count++] = op;
        g_ops[g_op_count] = '\0';
    }
}

static void clear_ops(void)
{
    g_op_count = 0u;
    g_ops[0] = '\0';
    g_snapshot_saw_listening = 0;
    g_snapshot_saw_one_shot = 0;
    g_snapshot_disabled_before_copy = 0;
}

static int op_index(char op, int start)
{
    unsigned int i;
    if (start < 0) {
        start = 0;
    }
    for (i = (unsigned int)start; i < g_op_count; i++) {
        if (g_ops[i] == op) {
            return (int)i;
        }
    }
    return -1;
}

static void reset_mock_radio(void)
{
    memset(g_ops, 0, sizeof(g_ops));
    g_op_count = 0u;
    g_listen_calls = 0u;
    g_listen_once_calls = 0u;
    g_continuous_listen_calls = 0u;
    g_idle_calls = 0u;
    g_advertise_calls = 0u;
    g_snapshot_calls = 0u;
    g_poll_calls = 0u;
    g_try_listen_once_calls = 0u;
    g_try_advertise_calls = 0u;
    g_try_advertise_sweeps_calls = 0u;
    g_try_snapshot_calls = 0u;
    g_last_listen_channel = 0u;
    g_last_adv_mask = 0u;
    g_last_adv_len = 0u;
    memset(g_last_adv_data, 0, sizeof(g_last_adv_data));
    memset(g_last_adv_addr, 0, sizeof(g_last_adv_addr));
    g_last_adv_addr_present = 0;
    g_radio_listening = 0;
    g_radio_listening_once = 0;
    g_rx_available = 0;
    g_rx_result = 1;
    memset(g_rx_pdu, 0, sizeof(g_rx_pdu));
    g_rx_pdu_len = 0u;
    g_rx_rssi = 0u;
    g_snapshot_saw_listening = 0;
    g_snapshot_saw_one_shot = 0;
    g_snapshot_disabled_before_copy = 0;
    g_listen_result = BLE_RADIO_OP_OK;
    g_snapshot_result = BLE_RADIO_OP_OK;
    g_tx_fault = BLE_RADIO_OP_OK;
    g_tx_completed_channel_mask = BLE_RADIO_ADV_CH_ALL;
}

static void make_adv(UB *adv, UINT adv_len, UB seed)
{
    UINT i;
    for (i = 0u; i < adv_len; i++) {
        adv[i] = (UB)(seed + i);
    }
}

static void load_rx_adv(const UB *adv, UINT adv_len, UINT rssi)
{
    UINT i;

    memset(g_rx_pdu, 0, sizeof(g_rx_pdu));
    g_rx_pdu[0] = 0x42u;
    g_rx_pdu[1] = (UB)(6u + adv_len);
    for (i = 0u; i < 6u; i++) {
        g_rx_pdu[2u + i] = (UB)(0xA0u + i);
    }
    if (adv_len > 0u) {
        memcpy(&g_rx_pdu[8], adv, adv_len);
    }
    g_rx_pdu_len = 8u + adv_len;
    g_rx_rssi = rssi;
    g_rx_available = 1;
    g_rx_result = 1;
}

void ble_radio_init(void)
{
}

void ble_radio_advertise(const UB *adv, UINT adv_len, const UB *addr6)
{
    ble_radio_advertise_channels(adv, adv_len, addr6, BLE_RADIO_ADV_CH_ALL);
}

void ble_radio_advertise_channels(const UB *adv, UINT adv_len, const UB *addr6,
                                  UINT channel_mask)
{
    (void)addr6;
    record_op('A');
    g_advertise_calls++;
    g_radio_listening = 0;
    g_radio_listening_once = 0;
    g_last_adv_mask = channel_mask;
    g_last_adv_len = adv_len;
    memset(g_last_adv_data, 0, sizeof(g_last_adv_data));
    if (adv != NULL && adv_len > 0u) {
        if (adv_len > BLE_ADV_MAX_DATA) {
            adv_len = BLE_ADV_MAX_DATA;
        }
        memcpy(g_last_adv_data, adv, adv_len);
    }
}

void ble_radio_idle(void)
{
    record_op('I');
    g_idle_calls++;
    g_radio_listening = 0;
    g_radio_listening_once = 0;
}

void ble_radio_listen(UINT channel)
{
    record_op('C');
    g_continuous_listen_calls++;
    g_last_listen_channel = channel;
    g_radio_listening = 1;
    g_radio_listening_once = 0;
}

void ble_radio_listen_once(UINT channel)
{
    record_op('L');
    g_listen_calls++;
    g_listen_once_calls++;
    g_last_listen_channel = channel;
    g_radio_listening = 1;
    g_radio_listening_once = 1;
}

int ble_radio_poll(UB *buf, UINT *len, UINT *rssi_dbm)
{
    (void)buf;
    (void)len;
    (void)rssi_dbm;
    g_poll_calls++;
    return 0;
}

int ble_radio_poll_snapshot(UB *buf, UINT *len, UINT *rssi_dbm)
{
    record_op('S');
    g_snapshot_calls++;
    if (g_radio_listening) {
        g_snapshot_saw_listening = 1;
    }
    if (g_radio_listening_once) {
        g_snapshot_saw_one_shot = 1;
    }
    if (!g_rx_available) {
        return 0;
    }

    g_rx_available = 0;
    g_radio_listening = 0;
    g_radio_listening_once = 0;
    g_snapshot_disabled_before_copy = 1;

    if (g_rx_result <= 0 || buf == NULL || len == NULL || rssi_dbm == NULL) {
        return -1;
    }

    memcpy(buf, g_rx_pdu, g_rx_pdu_len);
    *len = g_rx_pdu_len;
    *rssi_dbm = g_rx_rssi;
    return 1;
}

ble_radio_op_result_t ble_radio_try_listen_once(UINT channel, UINT state_timeout_ms)
{
    if (state_timeout_ms == 0u) {
        return BLE_RADIO_OP_INVALID_ARGUMENT;
    }
    record_op('L');
    g_listen_calls++;
    g_listen_once_calls++;
    g_try_listen_once_calls++;
    g_last_listen_channel = channel;
    if (g_listen_result != BLE_RADIO_OP_OK) {
        return g_listen_result;
    }
    g_radio_listening = 1;
    g_radio_listening_once = 1;
    return BLE_RADIO_OP_OK;
}

static ble_radio_tx_result_t mock_try_advertise_sweeps(
    const UB *adv, UINT adv_len, const UB *addr6, UINT channel_mask,
    uint8_t sweep_count, UINT state_timeout_ms)
{
    ble_radio_tx_result_t result = { 0 };
    uint8_t sweep_index;

    result.fault = BLE_RADIO_OP_INVALID_ARGUMENT;
    if (state_timeout_ms == 0u || addr6 == NULL ||
        (adv == NULL && adv_len != 0u) || adv_len > BLE_ADV_MAX_DATA ||
        channel_mask == 0u || (channel_mask & ~BLE_RADIO_ADV_CH_ALL) != 0u ||
        sweep_count == 0u || sweep_count > BLE_RADIO_MAX_ADV_SWEEPS) {
        return result;
    }

    record_op('A');
    g_advertise_calls++;
    g_radio_listening = 0;
    g_radio_listening_once = 0;
    g_last_adv_mask = channel_mask;
    g_last_adv_len = adv_len;
    memcpy(g_last_adv_addr, addr6, sizeof(g_last_adv_addr));
    g_last_adv_addr_present = 1;
    memset(g_last_adv_data, 0, sizeof(g_last_adv_data));
    if (adv != NULL && adv_len > 0u) {
        memcpy(g_last_adv_data, adv, adv_len);
    }
    result.requested_channel_mask = (uint8_t)channel_mask;
    result.requested_sweep_count = sweep_count;
    result.attempted_sweep_count = g_tx_fault == BLE_RADIO_OP_OK ?
        sweep_count : BLE_MESH_TX_SWEEP_COUNT_ONE;
    result.completed_channel_mask =
        (uint8_t)(g_tx_completed_channel_mask & channel_mask);
    for (sweep_index = 0u; sweep_index < result.attempted_sweep_count;
         sweep_index++) {
        result.completed_channel_masks[sweep_index] =
            result.completed_channel_mask;
    }
    result.fault = g_tx_fault;
    return result;
}

ble_radio_tx_result_t ble_radio_try_advertise_sweeps(
    const UB *adv, UINT adv_len, const UB *addr6, UINT channel_mask,
    uint8_t sweep_count, UINT state_timeout_ms)
{
    g_try_advertise_sweeps_calls++;
    return mock_try_advertise_sweeps(adv, adv_len, addr6, channel_mask,
                                     sweep_count, state_timeout_ms);
}

ble_radio_tx_result_t ble_radio_try_advertise_channels(
    const UB *adv, UINT adv_len, const UB *addr6, UINT channel_mask,
    UINT state_timeout_ms)
{
    g_try_advertise_calls++;
    return mock_try_advertise_sweeps(adv, adv_len, addr6, channel_mask, 1u,
                                     state_timeout_ms);
}

ble_radio_op_result_t ble_radio_try_poll_snapshot(
    UB *buf, UINT *len, UINT *rssi_dbm, UINT state_timeout_ms)
{
    if (buf == NULL || len == NULL || rssi_dbm == NULL || state_timeout_ms == 0u) {
        return BLE_RADIO_OP_INVALID_ARGUMENT;
    }

    record_op('S');
    g_snapshot_calls++;
    g_try_snapshot_calls++;
    if (g_radio_listening) {
        g_snapshot_saw_listening = 1;
    }
    if (g_radio_listening_once) {
        g_snapshot_saw_one_shot = 1;
    }
    if (!g_rx_available) {
        return BLE_RADIO_OP_NO_EVENT;
    }

    g_rx_available = 0;
    g_radio_listening = 0;
    g_radio_listening_once = 0;
    g_snapshot_disabled_before_copy = 1;
    if (g_snapshot_result != BLE_RADIO_OP_OK) {
        return g_snapshot_result;
    }
    if (g_rx_result <= 0) {
        return BLE_RADIO_OP_CRC_DROP;
    }

    memcpy(buf, g_rx_pdu, g_rx_pdu_len);
    *len = g_rx_pdu_len;
    *rssi_dbm = g_rx_rssi;
    return BLE_RADIO_OP_OK;
}

static void init_legacy(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    ble_mesh_scheduler_init_legacy(sched, now_ms, g_local_adva);
}

static void test_rx_default_state(void)
{
    ble_mesh_scheduler_t sched;
    ble_mesh_sched_event_t event;

    reset_mock_radio();
    init_legacy(&sched, 0u);

    ASSERT_EQ_U(0u, sched.rx_started, "init does not claim RX before first poll");
    ASSERT_EQ_U(0u, ble_mesh_scheduler_poll(&sched, 0u, &event), "default poll has no event");
    ASSERT_EQ_U(1u, sched.rx_started, "poll starts passive RX");
    ASSERT_EQ_U(1u, g_listen_calls, "default poll listens once");
    ASSERT_EQ_U(1u, g_listen_once_calls, "default RX uses one-shot listen API");
    ASSERT_EQ_U(1u, g_try_listen_once_calls, "default RX uses the typed one-shot seam");
    ASSERT_EQ_U(0u, g_continuous_listen_calls, "scheduler does not use continuous listen API");
    ASSERT_EQ_U(37u, g_last_listen_channel, "default RX channel is 37");
    ASSERT_EQ_U(BLE_MESH_SCHED_EVENT_NONE, event.type, "default event is none");
    ASSERT_EQ_U(0u, g_advertise_calls, "default poll does not advertise");
    ASSERT_EQ_U(0u, g_poll_calls, "scheduler uses snapshot API, not unsafe poll");
    ASSERT_EQ_U(1u, g_try_snapshot_calls, "default poll uses the typed snapshot seam");
}

static void test_enqueue_tx_disable_and_restore(void)
{
    ble_mesh_scheduler_t sched;
    ble_mesh_sched_event_t event;
    UB adv[5];
    int adv_pos;
    int restore_pos;

    reset_mock_radio();
    make_adv(adv, sizeof(adv), 0x10u);
    init_legacy(&sched, 0u);

    (void)ble_mesh_scheduler_poll(&sched, 0u, &event);
    (void)ble_mesh_scheduler_poll(&sched, 50u, &event);
    ASSERT_EQ_U(38u, g_last_listen_channel, "test setup hopped to channel 38");

    ASSERT_EQ_U(1u, ble_mesh_scheduler_enqueue(&sched, adv, (uint8_t)sizeof(adv),
                                               BLE_MESH_SCHED_CH_ALL,
                                               BLE_MESH_SCHED_TX_OWN, 0u),
                "enqueue own TX succeeds");
    clear_ops();
    (void)ble_mesh_scheduler_poll(&sched, 51u, &event);

    adv_pos = op_index('A', 0);
    restore_pos = op_index('L', adv_pos + 1);

    ASSERT_TRUE(adv_pos >= 0, "TX poll calls the typed advertise operation");
    ASSERT_TRUE(restore_pos > adv_pos, "RX is restored after TX");
    ASSERT_EQ_U(0u, g_idle_calls, "scheduler does not call compatibility idle before typed TX");
    ASSERT_EQ_U(1u, g_try_advertise_calls, "one typed advertising event is sent");
    ASSERT_EQ_U(1u, g_advertise_calls, "one advertising event sent");
    ASSERT_EQ_U(0u, g_continuous_listen_calls, "TX restore keeps using scheduler one-shot RX");
    ASSERT_EQ_U(BLE_MESH_SCHED_CH_ALL, g_last_adv_mask, "TX uses all primary channels by default");
    ASSERT_EQ_U(sizeof(adv), g_last_adv_len, "TX length preserved");
    ASSERT_EQ_U(adv[0], g_last_adv_data[0], "TX payload copied");
    ASSERT_EQ_U(38u, g_last_listen_channel, "TX restores previous RX channel");
    ASSERT_EQ_U(0u, sched.legacy_tx_count, "TX queue drained after one send");
    ASSERT_EQ_U(1u, sched.counters.tx_ok, "tx_ok counter increments");
}

static void test_tx_restore_hops_when_dwell_expired(void)
{
    ble_mesh_scheduler_t sched;
    ble_mesh_sched_event_t event;
    UB adv[1] = { 0x44u };

    reset_mock_radio();
    init_legacy(&sched, 0u);

    (void)ble_mesh_scheduler_poll(&sched, 0u, &event);
    ASSERT_EQ_U(37u, g_last_listen_channel, "TX expiry test starts on channel 37");
    ASSERT_EQ_U(50u, sched.hop_at_ms, "TX expiry test initial dwell deadline");

    ASSERT_EQ_U(1u, ble_mesh_scheduler_enqueue(&sched, adv, sizeof(adv),
                                               BLE_MESH_SCHED_CH37,
                                               BLE_MESH_SCHED_TX_OWN, 0u),
                "enqueue TX for expired-dwell restore test");

    (void)ble_mesh_scheduler_poll(&sched, 50u, &event);

    ASSERT_EQ_U(1u, g_advertise_calls, "TX at dwell expiry still transmits");
    ASSERT_EQ_U(38u, g_last_listen_channel, "TX restore hops instead of extending expired dwell");
    ASSERT_EQ_U(100u, sched.hop_at_ms, "TX restore hop sets next dwell deadline");
    ASSERT_EQ_U(1u, sched.counters.tx_ok, "expired-dwell TX increments tx_ok");
}

static void test_queue_full_drop_policy(void)
{
    ble_mesh_scheduler_t sched;
    UB adv[1] = { 0x55u };
    unsigned int i;

    reset_mock_radio();
    init_legacy(&sched, 0u);

    for (i = 0u; i < BLE_MESH_SCHED_LEGACY_TX_QUEUE_CAPACITY; i++) {
        ASSERT_EQ_U(1u, ble_mesh_scheduler_enqueue(&sched, adv, sizeof(adv),
                                                   BLE_MESH_SCHED_CH_ALL,
                                                   BLE_MESH_SCHED_TX_OWN, 0u),
                    "fill queue with own packet");
    }
    ASSERT_EQ_U(BLE_MESH_SCHED_LEGACY_TX_QUEUE_CAPACITY, sched.legacy_tx_count,
                "queue reaches capacity");
    ASSERT_EQ_U(0u, ble_mesh_scheduler_enqueue(&sched, adv, sizeof(adv),
                                               BLE_MESH_SCHED_CH_ALL,
                                               BLE_MESH_SCHED_TX_OWN, 0u),
                "full own queue drops new own packet");
    ASSERT_EQ_U(1u, sched.counters.queue_drop, "queue_drop increments for full own queue");
    ASSERT_EQ_U(0u, sched.counters.relay_drop, "no relay dropped from all-own queue");

    ASSERT_EQ_U(0u, ble_mesh_scheduler_enqueue(&sched, adv, sizeof(adv),
                                               BLE_MESH_SCHED_CH_ALL,
                                               BLE_MESH_SCHED_TX_RELAY, 0u),
                "full queue drops new relay first");
    ASSERT_EQ_U(2u, sched.counters.queue_drop, "queue_drop increments for dropped relay");
    ASSERT_EQ_U(1u, sched.counters.relay_drop, "relay_drop increments for dropped relay");
}

static void test_relay_priority_drop(void)
{
    ble_mesh_scheduler_t sched;
    ble_mesh_sched_event_t event;
    UB relay_adv[1];
    UB own_adv[1] = { 0xA5u };
    unsigned int i;
    unsigned int own_count = 0u;

    reset_mock_radio();
    init_legacy(&sched, 0u);

    for (i = 0u; i < BLE_MESH_SCHED_LEGACY_TX_QUEUE_CAPACITY; i++) {
        relay_adv[0] = (UB)(0x20u + i);
        ASSERT_EQ_U(1u, ble_mesh_scheduler_enqueue(&sched, relay_adv, sizeof(relay_adv),
                                                   BLE_MESH_SCHED_CH_ALL,
                                                   BLE_MESH_SCHED_TX_RELAY, 0u),
                    "fill queue with relay packet");
    }

    ASSERT_EQ_U(1u, ble_mesh_scheduler_enqueue(&sched, own_adv, sizeof(own_adv),
                                               BLE_MESH_SCHED_CH_ALL,
                                               BLE_MESH_SCHED_TX_OWN, 0u),
                "own packet replaces oldest relay when queue is full");
    ASSERT_EQ_U(BLE_MESH_SCHED_LEGACY_TX_QUEUE_CAPACITY, sched.legacy_tx_count,
                "queue remains at capacity");
    ASSERT_EQ_U(1u, sched.counters.queue_drop, "queue_drop records replaced relay");
    ASSERT_EQ_U(1u, sched.counters.relay_drop, "relay_drop records replaced relay");

    for (i = 0u; i < sched.legacy_tx_count; i++) {
        if (sched.legacy_tx_queue[i].kind == BLE_MESH_SCHED_TX_OWN) {
            own_count++;
        }
    }
    ASSERT_EQ_U(1u, own_count, "queue contains the own packet after relay drop");

    (void)ble_mesh_scheduler_poll(&sched, 0u, &event);
    ASSERT_EQ_U(1u, g_advertise_calls, "own packet is transmitted first");
    ASSERT_EQ_U(own_adv[0], g_last_adv_data[0], "transmitted packet is own priority packet");
}

static void test_relay_rate_limit(void)
{
    ble_mesh_scheduler_t sched;
    ble_mesh_sched_event_t event;
    UB first[1] = { 0x31u };
    UB second[1] = { 0x32u };

    reset_mock_radio();
    init_legacy(&sched, 0u);
    ASSERT_EQ_U(1u, ble_mesh_scheduler_enqueue(&sched, first, sizeof(first),
                                               BLE_MESH_SCHED_CH_ALL,
                                               BLE_MESH_SCHED_TX_RELAY, 0u),
                "enqueue first relay");
    ASSERT_EQ_U(1u, ble_mesh_scheduler_enqueue(&sched, second, sizeof(second),
                                               BLE_MESH_SCHED_CH_ALL,
                                               BLE_MESH_SCHED_TX_RELAY, 0u),
                "enqueue second relay");

    (void)ble_mesh_scheduler_poll(&sched, 0u, &event);
    ASSERT_EQ_U(1u, g_advertise_calls, "first relay transmits immediately");
    ASSERT_EQ_U(first[0], g_last_adv_data[0], "first relay payload transmitted");
    ASSERT_EQ_U(1u, sched.legacy_tx_count, "second relay remains queued");

    (void)ble_mesh_scheduler_poll(&sched, 100u, &event);
    ASSERT_EQ_U(1u, g_advertise_calls, "second relay is rate-limited at 100 ms");
    ASSERT_EQ_U(1u, sched.legacy_tx_count, "rate-limited relay remains queued");
    ASSERT_TRUE(sched.counters.relay_rate_limited >= 1u,
                "relay_rate_limited counter increments");

    (void)ble_mesh_scheduler_poll(&sched,
                                  tron_timer_config.scheduler_relay_spacing_ms,
                                  &event);
    ASSERT_EQ_U(2u, g_advertise_calls, "second relay transmits after interval");
    ASSERT_EQ_U(second[0], g_last_adv_data[0], "second relay payload transmitted");
    ASSERT_EQ_U(0u, sched.legacy_tx_count, "relay queue drained after rate interval");
}

static void test_channel_dwell_hop(void)
{
    ble_mesh_scheduler_t sched;
    ble_mesh_sched_event_t event;

    reset_mock_radio();
    init_legacy(&sched, 0u);

    (void)ble_mesh_scheduler_poll(&sched, 0u, &event);
    ASSERT_EQ_U(37u, g_last_listen_channel, "initial listen channel 37");
    ASSERT_EQ_U(1u, g_listen_calls, "one listen before dwell expires");

    (void)ble_mesh_scheduler_poll(&sched, 49u, &event);
    ASSERT_EQ_U(37u, g_last_listen_channel, "still on channel 37 before dwell expiry");
    ASSERT_EQ_U(1u, g_listen_calls, "no hop before 50 ms dwell");

    (void)ble_mesh_scheduler_poll(&sched, 50u, &event);
    ASSERT_EQ_U(38u, g_last_listen_channel, "hop to channel 38 at dwell expiry");

    (void)ble_mesh_scheduler_poll(&sched, 100u, &event);
    ASSERT_EQ_U(39u, g_last_listen_channel, "hop to channel 39 at next dwell expiry");

    (void)ble_mesh_scheduler_poll(&sched, 150u, &event);
    ASSERT_EQ_U(37u, g_last_listen_channel, "hop wraps back to channel 37");
    ASSERT_EQ_U(g_listen_calls, g_listen_once_calls, "all scheduler RX starts are one-shot");
    ASSERT_EQ_U(0u, g_continuous_listen_calls, "dwell hopping never uses continuous listen");
}

static void test_repeated_rx_does_not_extend_dwell(void)
{
    ble_mesh_scheduler_t sched;
    ble_mesh_sched_event_t event;
    UB adv[2];

    reset_mock_radio();
    make_adv(adv, sizeof(adv), 0x80u);
    init_legacy(&sched, 0u);

    (void)ble_mesh_scheduler_poll(&sched, 0u, &event);
    ASSERT_EQ_U(50u, sched.hop_at_ms, "initial dwell deadline is 50 ms");

    load_rx_adv(adv, sizeof(adv), 60u);
    ASSERT_EQ_U(1u, ble_mesh_scheduler_poll(&sched, 49u, &event),
                "RX just before dwell expiry is delivered");
    ASSERT_EQ_U(37u, event.channel, "pre-expiry RX is reported on channel 37");
    ASSERT_EQ_U(37u, g_last_listen_channel, "pre-expiry RX restores same channel");
    ASSERT_EQ_U(50u, sched.hop_at_ms, "pre-expiry RX restore preserves dwell deadline");

    load_rx_adv(adv, sizeof(adv), 61u);
    ASSERT_EQ_U(1u, ble_mesh_scheduler_poll(&sched, 50u, &event),
                "RX at dwell expiry is delivered before hopping");
    ASSERT_EQ_U(37u, event.channel, "expiry RX is reported on the channel that received it");
    ASSERT_EQ_U(38u, g_last_listen_channel, "expiry RX restore hops to next channel");
    ASSERT_EQ_U(100u, sched.hop_at_ms, "hop after RX sets a fresh dwell deadline");

    load_rx_adv(adv, sizeof(adv), 62u);
    ASSERT_EQ_U(1u, ble_mesh_scheduler_poll(&sched, 99u, &event),
                "RX before second dwell expiry is delivered");
    ASSERT_EQ_U(38u, event.channel, "second pre-expiry RX is on channel 38");
    ASSERT_EQ_U(38u, g_last_listen_channel, "second pre-expiry RX restores channel 38");
    ASSERT_EQ_U(100u, sched.hop_at_ms, "second RX restore still preserves dwell deadline");

    load_rx_adv(adv, sizeof(adv), 63u);
    ASSERT_EQ_U(1u, ble_mesh_scheduler_poll(&sched, 100u, &event),
                "RX at second dwell expiry is delivered");
    ASSERT_EQ_U(38u, event.channel, "second expiry RX is reported on channel 38");
    ASSERT_EQ_U(39u, g_last_listen_channel, "busy RX traffic cannot starve channel hopping");
    ASSERT_EQ_U(150u, sched.hop_at_ms, "second hop sets next dwell deadline");
    ASSERT_EQ_U(4u, sched.counters.rx_ok, "all repeated RX events were counted");
}

static void test_due_own_tx_preempts_continuous_rx(void)
{
    ble_mesh_scheduler_t sched;
    ble_mesh_sched_event_t event;
    UB rx_adv[2] = { 0x61u, 0x62u };
    UB tx_adv[1] = { 0xA5u };

    reset_mock_radio();
    init_legacy(&sched, 0u);
    (void)ble_mesh_scheduler_poll(&sched, 0u, &event);
    ASSERT_EQ_U(1u, ble_mesh_scheduler_enqueue(&sched, tx_adv, sizeof(tx_adv),
                                               BLE_MESH_SCHED_CH_ALL,
                                               BLE_MESH_SCHED_TX_OWN, 1u),
                "enqueue due own TX");
    load_rx_adv(rx_adv, sizeof(rx_adv), 60u);

    (void)ble_mesh_scheduler_poll(&sched, 1u, &event);

    ASSERT_EQ_U(1u, g_advertise_calls,
                "due own TX is serviced even when an RX snapshot is ready");
    ASSERT_EQ_U(tx_adv[0], g_last_adv_data[0], "due own payload transmitted");
    ASSERT_EQ_U(0u, sched.legacy_tx_count, "due own queue entry drained");
}

static void test_safe_rx_snapshot_ownership(void)
{
    ble_mesh_scheduler_t sched;
    ble_mesh_sched_event_t event;
    UB adv[6];

    reset_mock_radio();
    make_adv(adv, sizeof(adv), 0x70u);
    init_legacy(&sched, 0u);

    (void)ble_mesh_scheduler_poll(&sched, 0u, &event);
    load_rx_adv(adv, sizeof(adv), 61u);
    clear_ops();

    ASSERT_EQ_U(1u, ble_mesh_scheduler_poll(&sched, 1u, &event), "RX snapshot produces event");
    ASSERT_EQ_U(BLE_MESH_SCHED_EVENT_RX_ADV, event.type, "event type is RX_ADV");
    ASSERT_EQ_U(37u, event.channel, "RX event reports current channel");
    ASSERT_EQ_U(61u, event.rssi_magnitude_db,
                "RX event reports RSSI magnitude snapshot");
    ASSERT_EQ_U(sizeof(adv), event.adv_len, "RX AdvData length copied");
    ASSERT_EQ_U(adv[0], event.adv_data[0], "RX AdvData bytes copied");
    ASSERT_EQ_U(0u, g_poll_calls, "unsafe ble_radio_poll was not used");
    ASSERT_TRUE(g_snapshot_saw_listening, "snapshot was taken while RX was active");
    ASSERT_TRUE(g_snapshot_saw_one_shot, "snapshot was taken from one-shot RX");
    ASSERT_TRUE(g_snapshot_disabled_before_copy, "mock snapshot disabled RX before copying");
    ASSERT_EQ_U(1u, g_radio_listening, "scheduler restored RX after snapshot copy");
    ASSERT_TRUE(op_index('L', op_index('S', 0) + 1) > op_index('S', 0),
                "RX restore happens after snapshot");
    ASSERT_EQ_U(1u, sched.counters.rx_ok, "rx_ok counter increments for snapshot event");
}

static void test_null_event_consumes_valid_rx_without_error_count(void)
{
    ble_mesh_scheduler_t sched;
    UB adv[2];

    reset_mock_radio();
    make_adv(adv, sizeof(adv), 0x90u);
    init_legacy(&sched, 0u);

    (void)ble_mesh_scheduler_poll(&sched, 0u, NULL);
    load_rx_adv(adv, sizeof(adv), 64u);

    ASSERT_EQ_U(1u, ble_mesh_scheduler_poll(&sched, 1u, NULL),
                "NULL event still reports a consumed valid RX snapshot");
    ASSERT_EQ_U(1u, sched.counters.rx_ok, "valid NULL-event RX increments rx_ok");
    ASSERT_EQ_U(0u, sched.counters.rx_crc_or_empty,
                "valid NULL-event RX is not counted as CRC/empty");
    ASSERT_EQ_U(1u, g_radio_listening, "RX is restored after NULL-event consumption");
}

static void test_null_event_malformed_rx_counts_error(void)
{
    ble_mesh_scheduler_t sched;

    reset_mock_radio();
    init_legacy(&sched, 0u);

    (void)ble_mesh_scheduler_poll(&sched, 0u, NULL);
    g_rx_pdu[0] = 0x42u;
    g_rx_pdu[1] = 5u; /* invalid: AdvA must be at least 6 bytes */
    g_rx_pdu_len = 7u;
    g_rx_rssi = 65u;
    g_rx_available = 1;
    g_rx_result = 1;

    ASSERT_EQ_U(0u, ble_mesh_scheduler_poll(&sched, 1u, NULL),
                "NULL event malformed RX is not reported as a valid event");
    ASSERT_EQ_U(0u, sched.counters.rx_ok, "malformed NULL-event RX does not increment rx_ok");
    ASSERT_EQ_U(1u, sched.counters.rx_crc_or_empty,
                "malformed NULL-event RX increments rx_crc_or_empty");
    ASSERT_EQ_U(1u, g_radio_listening, "RX is restored after malformed NULL-event RX");
}

static void test_legacy_scheduler_stores_and_transmits_canonical_adva(void)
{
    ble_mesh_scheduler_t sched;
    ble_mesh_sched_event_t event;
    UB canonical_adva[6] = { 0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu };
    UB expected_adva[6];
    UB copied_adva[6];
    UB adv[1] = { 0xa5u };

    reset_mock_radio();
    memcpy(expected_adva, canonical_adva, sizeof(expected_adva));
    ble_mesh_scheduler_init_legacy(&sched, 0u, canonical_adva);
    canonical_adva[0] = 0u;

    ASSERT_EQ_U(1u, ble_mesh_scheduler_copy_local_adva(&sched, copied_adva),
                "legacy scheduler accepts its canonical AdvA");
    ASSERT_TRUE(memcmp(copied_adva, expected_adva, sizeof(copied_adva)) == 0,
                "legacy scheduler retains an AdvA copy rather than the caller pointer");
    ASSERT_EQ_U(1u, ble_mesh_scheduler_enqueue(&sched, adv, sizeof(adv),
                                                BLE_MESH_SCHED_CH_ALL,
                                                BLE_MESH_SCHED_TX_OWN, 0u),
                "canonical AdvA test enqueues TX");
    (void)ble_mesh_scheduler_poll(&sched, 0u, &event);

    ASSERT_EQ_U(1u, g_last_adv_addr_present,
                "legacy typed TX receives a non-NULL AdvA");
    ASSERT_TRUE(memcmp(g_last_adv_addr, expected_adva, sizeof(g_last_adv_addr)) == 0,
                "legacy typed TX uses the stored canonical AdvA");
}

static void test_typed_snapshot_crc_drop_restores_rx(void)
{
    ble_mesh_scheduler_t sched;
    ble_mesh_sched_event_t event;

    reset_mock_radio();
    init_legacy(&sched, 0u);
    (void)ble_mesh_scheduler_poll(&sched, 0u, &event);
    g_rx_available = 1;
    g_snapshot_result = BLE_RADIO_OP_CRC_DROP;

    ASSERT_EQ_U(0u, ble_mesh_scheduler_poll(&sched, 1u, &event),
                "typed CRC drop produces no legacy RX event");
    ASSERT_EQ_U(1u, sched.counters.rx_crc_or_empty,
                "typed CRC drop increments the legacy invalid-RX counter");
    ASSERT_EQ_U(1u, g_radio_listening,
                "typed CRC drop restores one-shot RX");
    ASSERT_EQ_U(2u, g_try_snapshot_calls,
                "typed snapshot seam is used for no-event and CRC paths");
}

static void test_typed_tx_completion_accounting_and_fault_diagnostic(void)
{
    ble_mesh_scheduler_t sched;
    ble_mesh_sched_event_t event;
    UB adv[1] = { 0x5au };

    reset_mock_radio();
    init_legacy(&sched, 0u);
    g_tx_completed_channel_mask = BLE_RADIO_ADV_CH37;
    g_tx_fault = BLE_RADIO_OP_STATE_TIMEOUT;
    ASSERT_EQ_U(1u, ble_mesh_scheduler_enqueue(&sched, adv, sizeof(adv),
                                                BLE_MESH_SCHED_CH_ALL,
                                                BLE_MESH_SCHED_TX_OWN, 0u),
                "partial typed TX enqueues");
    ASSERT_EQ_U(1u, ble_mesh_scheduler_poll(&sched, 0u, &event),
                "partial typed TX emits its current TX event");
    ASSERT_EQ_U(BLE_MESH_SCHED_EVENT_TX_DONE, event.type,
                "partial typed TX remains TX_DONE after one completed channel");
    ASSERT_EQ_U(BLE_MESH_TX_TOKEN_NONE, event.tx_token,
                "legacy TX_DONE uses the untracked token");
    ASSERT_EQ_U(BLE_MESH_SCHED_CH_ALL, event.tx_requested_channel_mask,
                "partial typed TX event reports requested channels");
    ASSERT_EQ_U(BLE_MESH_SCHED_CH37, event.tx_completed_channel_mask,
                "partial typed TX event reports completed channels");
    ASSERT_EQ_U(BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT, event.fault,
                "partial typed TX event retains its timeout");
    ASSERT_EQ_U(1u, sched.counters.tx_ok,
                "a completed typed channel counts one physical legacy TX");
    ASSERT_EQ_U(1u, ble_mesh_scheduler_poll(&sched, 1u, &event),
                "partial typed TX reports the latched radio fault");
    ASSERT_EQ_U(BLE_MESH_SCHED_EVENT_RADIO_FAULT, event.type,
                "partial typed TX fault is radio-visible");
    ASSERT_EQ_U(BLE_MESH_TX_TOKEN_NONE, event.tx_token,
                "partial typed TX fault retains the untracked token");
    ASSERT_EQ_U(BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT, event.fault,
                "partial typed TX retains its timeout reason");
    ASSERT_EQ_U(BLE_MESH_SCHED_CH_ALL, event.tx_requested_channel_mask,
                "partial typed TX reports requested channels");
    ASSERT_EQ_U(BLE_MESH_SCHED_CH37, event.tx_completed_channel_mask,
                "partial typed TX reports completed channels");

    reset_mock_radio();
    init_legacy(&sched, 0u);
    g_tx_completed_channel_mask = 0u;
    g_tx_fault = BLE_RADIO_OP_STATE_TIMEOUT;
    ASSERT_EQ_U(1u, ble_mesh_scheduler_enqueue(&sched, adv, sizeof(adv),
                                                BLE_MESH_SCHED_CH_ALL,
                                                BLE_MESH_SCHED_TX_OWN, 0u),
                "zero-completion typed TX enqueues");
    ASSERT_EQ_U(1u, ble_mesh_scheduler_poll(&sched, 0u, &event),
                "zero-completion typed TX emits its current TX event");
    ASSERT_EQ_U(BLE_MESH_SCHED_EVENT_TX_FAILED, event.type,
                "zero-completion typed TX emits TX_FAILED");
    ASSERT_EQ_U(BLE_MESH_TX_TOKEN_NONE, event.tx_token,
                "legacy TX_FAILED uses the untracked token");
    ASSERT_EQ_U(BLE_MESH_SCHED_CH_ALL, event.tx_requested_channel_mask,
                "zero-completion typed TX event reports requested channels");
    ASSERT_EQ_U(0u, event.tx_completed_channel_mask,
                "zero-completion typed TX event reports no completed channels");
    ASSERT_EQ_U(BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT, event.fault,
                "zero-completion typed TX event retains its timeout");
    ASSERT_EQ_U(0u, sched.counters.tx_ok,
                "zero completed typed channels do not count as TX success");
    ASSERT_EQ_U(1u, ble_mesh_scheduler_poll(&sched, 1u, &event),
                "zero-completion typed TX reports the latched fault");
    ASSERT_EQ_U(BLE_MESH_SCHED_EVENT_RADIO_FAULT, event.type,
                "zero-completion typed TX fault is radio-visible");
    ASSERT_EQ_U(BLE_MESH_TX_TOKEN_NONE, event.tx_token,
                "zero-completion fault retains the untracked token");
    ASSERT_EQ_U(BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT, event.fault,
                "zero-completion fault retains its timeout reason");
    ASSERT_EQ_U(BLE_MESH_SCHED_CH_ALL, event.tx_requested_channel_mask,
                "zero-completion fault preserves requested channels");
    ASSERT_EQ_U(0u, event.tx_completed_channel_mask,
                "zero-completion typed TX diagnostic stays zero");

    reset_mock_radio();
    init_legacy(&sched, 0u);
    ASSERT_EQ_U(1u, ble_mesh_scheduler_enqueue(&sched, adv, sizeof(adv),
                                                BLE_MESH_SCHED_CH_ALL,
                                                BLE_MESH_SCHED_TX_OWN, 0u),
                "successful typed TX enqueues");
    ASSERT_EQ_U(1u, ble_mesh_scheduler_poll(&sched, 0u, &event),
                "successful typed TX emits TX_DONE");
    ASSERT_EQ_U(BLE_MESH_SCHED_EVENT_TX_DONE, event.type,
                "successful typed TX event is TX_DONE");
    ASSERT_EQ_U(BLE_MESH_TX_TOKEN_NONE, event.tx_token,
                "successful legacy TX uses the untracked token");
    ASSERT_EQ_U(BLE_MESH_SCHED_CH_ALL, event.tx_requested_channel_mask,
                "successful typed TX reports requested channels");
    ASSERT_EQ_U(BLE_MESH_SCHED_CH_ALL, event.tx_completed_channel_mask,
                "successful typed TX reports completed channels");
    ASSERT_EQ_U(BLE_MESH_SCHED_FAULT_NONE, event.fault,
                "successful typed TX reports no fault");
    ASSERT_EQ_U(1u, sched.counters.tx_ok,
                "successful typed TX increments the legacy TX counter");
    ASSERT_EQ_U(0u, ble_mesh_scheduler_poll(&sched, 1u, &event),
                "successful typed TX does not emit a following fault");
    ASSERT_EQ_U(BLE_MESH_SCHED_EVENT_NONE, event.type,
                "successful typed TX leaves the scheduler healthy");

    reset_mock_radio();
    init_legacy(&sched, 0u);
    (void)ble_mesh_scheduler_poll(&sched, 0u, &event);
    g_listen_result = BLE_RADIO_OP_STATE_TIMEOUT;
    ASSERT_EQ_U(1u, ble_mesh_scheduler_enqueue(&sched, adv, sizeof(adv),
                                                BLE_MESH_SCHED_CH_ALL,
                                                BLE_MESH_SCHED_TX_OWN, 1u),
                "restore-failure typed TX enqueues");
    ASSERT_EQ_U(1u, ble_mesh_scheduler_poll(&sched, 1u, &event),
                "restore-failure typed TX emits its current TX event");
    ASSERT_EQ_U(BLE_MESH_SCHED_EVENT_TX_DONE, event.type,
                "restore failure preserves completed TX_DONE");
    ASSERT_EQ_U(BLE_MESH_TX_TOKEN_NONE, event.tx_token,
                "restore-failure legacy TX uses the untracked token");
    ASSERT_EQ_U(BLE_MESH_SCHED_CH_ALL, event.tx_requested_channel_mask,
                "restore failure preserves TX requested channels");
    ASSERT_EQ_U(BLE_MESH_SCHED_CH_ALL, event.tx_completed_channel_mask,
                "restore failure preserves TX completed channels");
    ASSERT_EQ_U(BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT, event.fault,
                "restore failure is retained on the current TX event");
    ASSERT_EQ_U(1u, ble_mesh_scheduler_poll(&sched, 2u, &event),
                "restore failure emits its following radio fault");
    ASSERT_EQ_U(BLE_MESH_SCHED_EVENT_RADIO_FAULT, event.type,
                "restore failure is radio-visible");
    ASSERT_EQ_U(BLE_MESH_TX_TOKEN_NONE, event.tx_token,
                "restore fault retains the untracked token");
    ASSERT_EQ_U(BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT, event.fault,
                "restore fault retains its timeout reason");
    ASSERT_EQ_U(BLE_MESH_SCHED_CH_ALL, event.tx_requested_channel_mask,
                "restore fault preserves TX requested channels");
    ASSERT_EQ_U(BLE_MESH_SCHED_CH_ALL, event.tx_completed_channel_mask,
                "restore fault preserves TX completed channels");
}

int main(void)
{
    test_rx_default_state();
    test_enqueue_tx_disable_and_restore();
    test_tx_restore_hops_when_dwell_expired();
    test_queue_full_drop_policy();
    test_relay_priority_drop();
    test_relay_rate_limit();
    test_channel_dwell_hop();
    test_repeated_rx_does_not_extend_dwell();
    test_due_own_tx_preempts_continuous_rx();
    test_safe_rx_snapshot_ownership();
    test_null_event_consumes_valid_rx_without_error_count();
    test_null_event_malformed_rx_counts_error();
    test_legacy_scheduler_stores_and_transmits_canonical_adva();
    test_typed_snapshot_crc_drop_restores_rx();
    test_typed_tx_completion_accounting_and_fault_diagnostic();

    if (g_failures != 0u) {
        printf("ble_mesh_scheduler tests failed: %u\n", g_failures);
        return 1;
    }

    printf("ble_mesh_scheduler tests passed\n");
    return 0;
}

#endif /* TEST_BLE_RADIO_DRIVER */
