/* Test-only RED radio/scheduler/queue port. Never link this file into firmware. */
#include "ble_mesh_scheduler.h"
#include "test_radio_scheduler_support.h"

#include <string.h>

static ble_radio_tx_result_t scripted_tx;
static test_radio_failure_location_t scripted_failure_location;
static uint8_t scripted_failure_sweep;
static uint8_t channel_trace[BLE_RADIO_MAX_ADV_SWEEPS * 3u];
static size_t channel_trace_len;

void test_radio_script_reset(void)
{
    memset(&scripted_tx, 0, sizeof(scripted_tx));
    memset(channel_trace, 0, sizeof(channel_trace));
    channel_trace_len = 0u;
    scripted_failure_location = TEST_RADIO_FAILURE_NONE;
    scripted_failure_sweep = 1u;
}

void test_radio_script_tx(uint8_t requested_channel_mask,
                          uint8_t completed_channel_mask,
                          ble_radio_op_result_t fault,
                          test_radio_failure_location_t failure_location)
{
    scripted_tx.requested_channel_mask = requested_channel_mask;
    scripted_tx.completed_channel_mask = completed_channel_mask;
    scripted_tx.fault = fault;
    scripted_failure_location = failure_location;
    scripted_failure_sweep = 1u;
}

void test_radio_script_tx_repeat_failure(
    uint8_t failure_sweep, test_radio_failure_location_t failure_location)
{
    if (failure_sweep == 0u || failure_location < TEST_RADIO_FAILURE_CH37 ||
        failure_location > TEST_RADIO_FAILURE_CH39) {
        return;
    }
    scripted_failure_sweep = failure_sweep;
    scripted_failure_location = failure_location;
}

void test_radio_script_rx(const uint8_t *raw_pdu, size_t raw_pdu_len,
                          uint8_t rssi_magnitude_db,
                          ble_radio_op_result_t result)
{
    (void)raw_pdu;
    (void)raw_pdu_len;
    (void)rssi_magnitude_db;
    (void)result;
}

const uint8_t *test_radio_channel_trace(size_t *length_out)
{
    if (length_out != NULL) {
        *length_out = channel_trace_len;
    }
    return channel_trace;
}

ble_radio_op_result_t ble_radio_try_init(UINT state_timeout_ms)
{
    return state_timeout_ms == 0u ? BLE_RADIO_OP_INVALID_ARGUMENT : BLE_RADIO_OP_OK;
}

ble_radio_op_result_t ble_radio_try_idle(UINT state_timeout_ms)
{
    return state_timeout_ms == 0u ? BLE_RADIO_OP_INVALID_ARGUMENT : BLE_RADIO_OP_OK;
}

ble_radio_op_result_t ble_radio_try_listen_once(UINT channel, UINT state_timeout_ms)
{
    if (state_timeout_ms == 0u || (channel != 37u && channel != 38u && channel != 39u)) {
        return BLE_RADIO_OP_INVALID_ARGUMENT;
    }
    return BLE_RADIO_OP_OK;
}

ble_radio_op_result_t ble_radio_try_poll_snapshot(
    UB *buf, UINT *len, UINT *rssi_magnitude_db, UINT state_timeout_ms)
{
    (void)buf;
    (void)len;
    (void)rssi_magnitude_db;
    return state_timeout_ms == 0u ? BLE_RADIO_OP_INVALID_ARGUMENT : BLE_RADIO_OP_NO_EVENT;
}

ble_radio_tx_result_t ble_radio_try_advertise_sweeps(
    const UB *adv, UINT adv_len, const UB *addr6, UINT channel_mask,
    uint8_t sweep_count, UINT state_timeout_ms)
{
    ble_radio_tx_result_t result;
    uint8_t sweep;
    uint8_t channel;

    memset(&result, 0, sizeof(result));
    if (state_timeout_ms == 0u || (channel_mask & ~BLE_RADIO_ADV_CH_ALL) != 0u ||
        (channel_mask & BLE_RADIO_ADV_CH_ALL) == 0u ||
        (adv == NULL && adv_len != 0u) || adv_len > BLE_ADV_MAX_DATA ||
        addr6 == NULL || sweep_count == 0u ||
        sweep_count > BLE_RADIO_MAX_ADV_SWEEPS) {
        result.fault = BLE_RADIO_OP_INVALID_ARGUMENT;
        return result;
    }
    result.requested_channel_mask = channel_mask;
    result.requested_sweep_count = sweep_count;
    result.fault = scripted_tx.fault;
    channel_trace_len = 0u;
    if (scripted_failure_location == TEST_RADIO_FAILURE_PRE_DISABLE) {
        return result;
    }
    for (sweep = 0u; sweep < sweep_count; sweep++) {
        result.attempted_sweep_count++;
        for (channel = 0u; channel < 3u; channel++) {
            uint8_t channel_bit = (uint8_t)(1u << channel);
            uint8_t channel_number = channel == 0u ? 37u :
                channel == 1u ? 38u : 39u;
            test_radio_failure_location_t failure_location = channel == 0u ?
                TEST_RADIO_FAILURE_CH37 : channel == 1u ?
                TEST_RADIO_FAILURE_CH38 : TEST_RADIO_FAILURE_CH39;

            if ((channel_mask & channel_bit) == 0u) {
                continue;
            }
            if (channel_trace_len < sizeof(channel_trace)) {
                channel_trace[channel_trace_len++] = channel_number;
            }
            if (scripted_failure_location == failure_location &&
                scripted_failure_sweep == (uint8_t)(sweep + 1u)) {
                return result;
            }
            if ((scripted_tx.completed_channel_mask & channel_bit) != 0u ||
                scripted_tx.fault == BLE_RADIO_OP_OK) {
                result.completed_channel_masks[sweep] |= channel_bit;
                result.completed_channel_mask |= channel_bit;
            }
        }
    }
    return result;
}

ble_radio_tx_result_t ble_radio_try_advertise_channels(
    const UB *adv, UINT adv_len, const UB *addr6, UINT channel_mask,
    UINT state_timeout_ms)
{
    return ble_radio_try_advertise_sweeps(adv, adv_len, addr6, channel_mask,
                                          1u, state_timeout_ms);
}

unsigned int test_radio_disable_count(void)
{
    return 0u;
}

ble_radio_op_result_t ble_radio_read_default_adva(UB out_adva[6])
{
    if (out_adva == NULL) {
        return BLE_RADIO_OP_INVALID_ARGUMENT;
    }
    memset(out_adva, 0, 6u);
    return BLE_RADIO_OP_INVALID_ARGUMENT;
}

void ble_mesh_scheduler_init(ble_mesh_scheduler_t *sched, uint32_t now_ms,
                             const uint8_t local_adva[6])
{
    (void)now_ms;
    if (sched != NULL && local_adva != NULL) {
        memset(sched, 0, sizeof(*sched));
        memcpy(sched->local_adva, local_adva, 6u);
        sched->routed_started = 1u;
    }
}

int ble_mesh_scheduler_copy_local_adva(const ble_mesh_scheduler_t *sched,
                                       uint8_t out_adva[6])
{
    if (sched == NULL || out_adva == NULL || sched->routed_started == 0u) {
        return 0;
    }
    memcpy(out_adva, sched->local_adva, 6u);
    return 1;
}

ble_mesh_sched_enqueue_result_t ble_mesh_scheduler_enqueue_ex(
    ble_mesh_scheduler_t *sched, const ble_mesh_tx_item_t *item)
{
    ble_mesh_sched_enqueue_result_t result;

    (void)sched;
    (void)item;
    memset(&result, 0, sizeof(result));
    result.status = BLE_MESH_SCHED_ENQUEUE_INVALID;
    return result;
}

int ble_mesh_scheduler_poll(ble_mesh_scheduler_t *sched, uint32_t now_ms,
                            ble_mesh_sched_event_t *event)
{
    (void)sched;
    (void)now_ms;
    if (event != NULL) {
        memset(event, 0, sizeof(*event));
    }
    return 0;
}

void ble_mesh_tx_queue_init(ble_mesh_tx_queue_t *queue)
{
    if (queue != NULL) {
        memset(queue, 0, sizeof(*queue));
    }
}

ble_mesh_tx_enqueue_result_t ble_mesh_tx_queue_enqueue(
    ble_mesh_tx_queue_t *queue, const ble_mesh_tx_item_t *item)
{
    ble_mesh_tx_enqueue_result_t result;

    (void)queue;
    (void)item;
    memset(&result, 0, sizeof(result));
    result.status = BLE_MESH_TX_ENQUEUE_INVALID;
    return result;
}

ble_mesh_tx_select_status_t ble_mesh_tx_queue_select_due(
    ble_mesh_tx_queue_t *queue, uint32_t now_ms,
    ble_mesh_tx_token_t promoted_custody_token,
    ble_mesh_tx_selection_t *selection_out)
{
    (void)queue;
    (void)now_ms;
    (void)promoted_custody_token;
    if (selection_out != NULL) {
        memset(selection_out, 0, sizeof(*selection_out));
    }
    return BLE_MESH_TX_SELECT_INVALID;
}

int ble_mesh_tx_queue_remove(ble_mesh_tx_queue_t *queue, uint8_t index,
                             ble_mesh_tx_item_t *removed_out)
{
    (void)queue;
    (void)index;
    (void)removed_out;
    return 0;
}

int ble_mesh_tx_queue_complete(ble_mesh_tx_queue_t *queue, uint8_t index,
                               uint8_t completed_channel_mask,
                               ble_mesh_tx_token_t promoted_custody_token)
{
    (void)queue;
    (void)index;
    (void)completed_channel_mask;
    (void)promoted_custody_token;
    return 0;
}

uint8_t ble_mesh_tx_queue_count(const ble_mesh_tx_queue_t *queue)
{
    return queue == NULL ? 0u : queue->count;
}
