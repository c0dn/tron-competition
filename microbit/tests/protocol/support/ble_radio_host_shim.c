/* Host RADIO register model for future production-mode typed-radio tests. */
#include "ble_radio.h"
#include "test_radio_scheduler_support.h"

#include <string.h>

#define HOST_RADIO_BASE            0x40001000UL
#define HOST_R(off)                (HOST_RADIO_BASE + (off))
#define HOST_TASKS_TXEN            HOST_R(0x000)
#define HOST_TASKS_RXEN            HOST_R(0x004)
#define HOST_TASKS_DISABLE         HOST_R(0x010)
#define HOST_EVENTS_END            HOST_R(0x10C)
#define HOST_EVENTS_DISABLED       HOST_R(0x110)
#define HOST_CRCSTATUS             HOST_R(0x400)
#define HOST_PACKETPTR             HOST_R(0x504)
#define HOST_RSSISAMPLE            HOST_R(0x548)
#define HOST_STATE                 HOST_R(0x550)
#define HOST_DATAWHITEIV           HOST_R(0x554)
#define HOST_FICR_DEVICEADDR0      0x100000A4UL
#define HOST_FICR_DEVICEADDR1      0x100000A8UL

#define HOST_REGISTER_WORDS        (0x600u / 4u)

static UW registers[HOST_REGISTER_WORDS];
static uint8_t scripted_raw[BLE_RX_MAX];
static size_t scripted_raw_len;
#define TEST_RADIO_CHANNEL_TRACE_CAPACITY 6u

static uint8_t channel_trace[TEST_RADIO_CHANNEL_TRACE_CAPACITY];
static size_t channel_trace_len;
static uint8_t channel_attempts[3];
static uint8_t script_requested_mask;
static uint8_t script_completed_mask;
static ble_radio_op_result_t script_fault;
static test_radio_failure_location_t script_failure_location;
static uint8_t script_failure_sweep;
static test_radio_tx_start_handler_t tx_start_handler;
static void *tx_start_context;
static unsigned int disable_count;

static UW *register_slot(UW address)
{
    UW offset;

    if (address < HOST_RADIO_BASE || address >= HOST_RADIO_BASE + 0x600u) {
        return NULL;
    }
    offset = address - HOST_RADIO_BASE;
    if ((offset % 4u) != 0u) {
        return NULL;
    }
    return &registers[offset / 4u];
}

static test_radio_failure_location_t channel_failure_location(uint8_t channel)
{
    if (channel == 37u) {
        return TEST_RADIO_FAILURE_CH37;
    }
    if (channel == 38u) {
        return TEST_RADIO_FAILURE_CH38;
    }
    return TEST_RADIO_FAILURE_CH39;
}

static uint8_t channel_trace_index(uint8_t channel)
{
    return channel == 37u ? 0u : channel == 38u ? 1u : 2u;
}

static void copy_scripted_rx(UW packet_pointer)
{
    UB *packet = (UB *)packet_pointer;

    if (packet != NULL && scripted_raw_len > 0u) {
        memcpy(packet, scripted_raw, scripted_raw_len);
    }
}

UW ble_radio_host_in_w(UW address)
{
    UW *slot = register_slot(address);

    if (address == HOST_FICR_DEVICEADDR0) {
        return 0x11223344UL;
    }
    if (address == HOST_FICR_DEVICEADDR1) {
        return 0x00005566UL;
    }
    return slot == NULL ? 0u : *slot;
}

void ble_radio_host_out_w(UW address, UW value)
{
    UW *slot = register_slot(address);
    uint8_t channel;
    uint8_t channel_mask;

    if (slot != NULL) {
        *slot = value;
    }
    if (address == HOST_PACKETPTR) {
        copy_scripted_rx(value);
        return;
    }
    if (address == HOST_TASKS_DISABLE) {
        disable_count++;
        if (script_failure_location != TEST_RADIO_FAILURE_PRE_DISABLE) {
            *register_slot(HOST_STATE) = 0u;
            *register_slot(HOST_EVENTS_DISABLED) = 1u;
        }
        return;
    }
    if (address == HOST_TASKS_TXEN) {
        uint8_t trace_index;

        channel = (uint8_t)*register_slot(HOST_DATAWHITEIV);
        channel_mask = channel == 37u ? BLE_RADIO_ADV_CH37 :
            channel == 38u ? BLE_RADIO_ADV_CH38 : BLE_RADIO_ADV_CH39;
        if ((script_requested_mask & channel_mask) == 0u) {
            return;
        }
        if (tx_start_handler != NULL) {
            tx_start_handler(tx_start_context);
        }
        trace_index = channel_trace_index(channel);
        channel_attempts[trace_index]++;
        if (channel_trace_len < sizeof(channel_trace)) {
            channel_trace[channel_trace_len++] = channel;
        }
        if (!(script_failure_location == channel_failure_location(channel) &&
              channel_attempts[trace_index] == script_failure_sweep) &&
            ((script_completed_mask & channel_mask) != 0u ||
             script_fault == BLE_RADIO_OP_OK)) {
            *register_slot(HOST_STATE) = 0u;
            *register_slot(HOST_EVENTS_DISABLED) = 1u;
        }
        return;
    }
    if (address == HOST_TASKS_RXEN) {
        if (script_failure_location != TEST_RADIO_FAILURE_RESTORE) {
            *register_slot(HOST_STATE) = 3u;
        }
    }
}

void test_radio_script_reset(void)
{
    memset(registers, 0, sizeof(registers));
    memset(scripted_raw, 0, sizeof(scripted_raw));
    memset(channel_trace, 0, sizeof(channel_trace));
    memset(channel_attempts, 0, sizeof(channel_attempts));
    scripted_raw_len = 0u;
    channel_trace_len = 0u;
    script_requested_mask = BLE_RADIO_ADV_CH_ALL;
    script_completed_mask = BLE_RADIO_ADV_CH_ALL;
    script_fault = BLE_RADIO_OP_OK;
    script_failure_location = TEST_RADIO_FAILURE_NONE;
    script_failure_sweep = 1u;
    tx_start_handler = NULL;
    tx_start_context = NULL;
    disable_count = 0u;
}

void test_radio_set_tx_start_handler(test_radio_tx_start_handler_t handler,
                                    void *context)
{
    tx_start_handler = handler;
    tx_start_context = context;
}

void test_radio_script_tx(uint8_t requested_channel_mask,
                          uint8_t completed_channel_mask,
                          ble_radio_op_result_t fault,
                          test_radio_failure_location_t failure_location)
{
    script_requested_mask = requested_channel_mask;
    script_completed_mask = completed_channel_mask;
    script_fault = fault;
    script_failure_location = failure_location;
    script_failure_sweep = 1u;
    channel_trace_len = 0u;
    memset(channel_attempts, 0, sizeof(channel_attempts));
    *register_slot(HOST_EVENTS_DISABLED) = 0u;
    *register_slot(HOST_STATE) =
        failure_location == TEST_RADIO_FAILURE_PRE_DISABLE ? 3u : 0u;
}

void test_radio_script_tx_repeat_failure(
    uint8_t failure_sweep, test_radio_failure_location_t failure_location)
{
    if (failure_sweep == 0u || failure_location < TEST_RADIO_FAILURE_CH37 ||
        failure_location > TEST_RADIO_FAILURE_CH39) {
        return;
    }
    script_failure_sweep = failure_sweep;
    script_failure_location = failure_location;
}

void test_radio_script_rx(const uint8_t *raw_pdu, size_t raw_pdu_len,
                          uint8_t rssi_magnitude_db,
                          ble_radio_op_result_t result)
{
    if (raw_pdu == NULL || raw_pdu_len > BLE_RX_MAX) {
        scripted_raw_len = 0u;
        return;
    }
    memcpy(scripted_raw, raw_pdu, raw_pdu_len);
    scripted_raw_len = raw_pdu_len;
    copy_scripted_rx(*register_slot(HOST_PACKETPTR));
    *register_slot(HOST_RSSISAMPLE) = rssi_magnitude_db;
    *register_slot(HOST_CRCSTATUS) = result == BLE_RADIO_OP_OK ? 1u : 0u;
    *register_slot(HOST_EVENTS_END) = result == BLE_RADIO_OP_NO_EVENT ? 0u : 1u;
    *register_slot(HOST_STATE) = 0u;
}

const uint8_t *test_radio_channel_trace(size_t *length_out)
{
    if (length_out != NULL) {
        *length_out = channel_trace_len;
    }
    return channel_trace;
}

unsigned int test_radio_disable_count(void)
{
    return disable_count;
}
