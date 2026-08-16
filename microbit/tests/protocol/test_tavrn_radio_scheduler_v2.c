#include "ble_mesh_scheduler.h"
#include "ble_mesh_tx_queue.h"
#include "test_radio_scheduler_support.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;
static unsigned int emitted;

#define CHECK(requirement, expression) \
    do { \
        unsigned int bit__ = (requirement)[0] == 'B' ? 0u : 1u; \
        if (!(expression)) { \
            if ((emitted & (1u << bit__)) == 0u) { \
                printf("FAIL %s: %s:%d: assertion failed: %s\n", \
                       (requirement), __FILE__, __LINE__, #expression); \
                emitted |= 1u << bit__; \
            } \
            failures++; \
        } \
    } while (0)

static ble_mesh_tx_item_t make_item(ble_mesh_tx_priority_t priority,
                                    ble_mesh_tx_service_class_t service_class,
                                    ble_mesh_tx_token_t token,
                                    uint32_t not_before_ms, uint8_t marker)
{
    ble_mesh_tx_item_t item;

    memset(&item, 0, sizeof(item));
    item.adv_len = 3u;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = priority;
    item.service_class = service_class;
    item.token = token;
    item.not_before_ms = not_before_ms;
    item.expiry_ms = 10000u;
    item.sweep_count = service_class == BLE_MESH_TX_SERVICE_CUSTODY_DATA ?
        BLE_MESH_TX_SWEEP_COUNT_TWO : BLE_MESH_TX_SWEEP_COUNT_ONE;
    item.budget_class = priority == BLE_MESH_TX_PRIORITY_HACK ?
        BLE_MESH_TX_BUDGET_CRITICAL : BLE_MESH_TX_BUDGET_GENERAL;
    item.adv_data[0] = marker;
    return item;
}

static void test_typed_radio_precise_channel_faults(void)
{
    static const uint8_t local_adva[6] = {
        0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu,
    };
    static const uint8_t adv[] = { 0x02u, 0x01u, 0x06u };
    ble_radio_tx_result_t result;
    const uint8_t *trace;
    size_t trace_len;

    test_radio_script_reset();
    CHECK("BEARER-03", ble_radio_try_init(2u) == BLE_RADIO_OP_OK);
    CHECK("BEARER-03", ble_radio_try_idle(2u) == BLE_RADIO_OP_OK);
    CHECK("BEARER-03", ble_radio_try_listen_once(37u, 2u) == BLE_RADIO_OP_OK);

    test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, 0u, BLE_RADIO_OP_STATE_TIMEOUT,
                         TEST_RADIO_FAILURE_CH37);
    result = ble_radio_try_advertise_channels(adv, sizeof(adv), local_adva,
                                               BLE_RADIO_ADV_CH_ALL, 2u);
    trace = test_radio_channel_trace(&trace_len);
    CHECK("BEARER-03", result.requested_channel_mask == BLE_RADIO_ADV_CH_ALL &&
                         result.completed_channel_mask == 0u &&
                         result.fault == BLE_RADIO_OP_STATE_TIMEOUT);
    CHECK("BEARER-03", trace_len == 1u && trace[0] == 37u);

    test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, BLE_RADIO_ADV_CH37,
                         BLE_RADIO_OP_STATE_TIMEOUT, TEST_RADIO_FAILURE_CH38);
    result = ble_radio_try_advertise_channels(adv, sizeof(adv), local_adva,
                                               BLE_RADIO_ADV_CH_ALL, 2u);
    trace = test_radio_channel_trace(&trace_len);
    CHECK("BEARER-03", result.completed_channel_mask == BLE_RADIO_ADV_CH37 &&
                         result.fault == BLE_RADIO_OP_STATE_TIMEOUT);
    CHECK("BEARER-03", trace_len == 2u && trace[0] == 37u && trace[1] == 38u);

    test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, BLE_RADIO_ADV_CH37 | BLE_RADIO_ADV_CH38,
                         BLE_RADIO_OP_STATE_TIMEOUT, TEST_RADIO_FAILURE_CH39);
    result = ble_radio_try_advertise_channels(adv, sizeof(adv), local_adva,
                                               BLE_RADIO_ADV_CH_ALL, 2u);
    trace = test_radio_channel_trace(&trace_len);
    CHECK("BEARER-03", result.completed_channel_mask ==
                         (BLE_RADIO_ADV_CH37 | BLE_RADIO_ADV_CH38));
    CHECK("BEARER-03", trace_len == 3u && trace[2] == 39u);

    test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, BLE_RADIO_ADV_CH_ALL,
                          BLE_RADIO_OP_OK, TEST_RADIO_FAILURE_NONE);
    result = ble_radio_try_advertise_channels(adv, sizeof(adv), local_adva,
                                               BLE_RADIO_ADV_CH_ALL, 2u);
    trace = test_radio_channel_trace(&trace_len);
    CHECK("BEARER-03", result.completed_channel_mask == BLE_RADIO_ADV_CH_ALL &&
                          result.fault == BLE_RADIO_OP_OK && trace_len == 3u);

    test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, 0u, BLE_RADIO_OP_STATE_TIMEOUT,
                         TEST_RADIO_FAILURE_PRE_DISABLE);
    result = ble_radio_try_advertise_channels(adv, sizeof(adv), local_adva,
                                               BLE_RADIO_ADV_CH_ALL, 2u);
    trace = test_radio_channel_trace(&trace_len);
    CHECK("BEARER-03", result.completed_channel_mask == 0u && trace_len == 0u);
}

static void test_scheduler_fault_latch_and_fresh_rx_admission(void)
{
    static const uint8_t local_adva[6] = {
        0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu,
    };
    static const uint8_t raw_valid[] = {
        0x42u, 0x09u, 0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u,
        0x02u, 0x01u, 0x06u,
    };
    uint8_t raw_bad_s0[sizeof(raw_valid)];
    uint8_t copied_adva[6];
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    ble_mesh_tx_item_t item = make_item(BLE_MESH_TX_PRIORITY_DATA,
                                        BLE_MESH_TX_SERVICE_BEST_EFFORT,
                                        BLE_MESH_TX_TOKEN_NONE, 0u, 0xa5u);
    ble_mesh_sched_enqueue_result_t enqueue_result;

    ble_mesh_scheduler_init(&scheduler, 0u, local_adva);
    CHECK("BEARER-03", ble_mesh_scheduler_copy_local_adva(&scheduler, copied_adva) == 1);
    CHECK("BEARER-03", memcmp(copied_adva, local_adva, sizeof(copied_adva)) == 0);
    enqueue_result = ble_mesh_scheduler_enqueue_ex(&scheduler, &item);
    CHECK("BEARER-03", enqueue_result.status == BLE_MESH_SCHED_ENQUEUE_OK);

    test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, BLE_RADIO_ADV_CH37,
                         BLE_RADIO_OP_STATE_TIMEOUT, TEST_RADIO_FAILURE_CH38);
    CHECK("BEARER-03", ble_mesh_scheduler_poll(&scheduler, 1u, &event) == 1);
    CHECK("BEARER-03", event.type == BLE_MESH_SCHED_EVENT_TX_DONE &&
                         event.tx_requested_channel_mask == BLE_RADIO_ADV_CH_ALL &&
                         event.tx_completed_channel_mask == BLE_RADIO_ADV_CH37 &&
                         event.fault == BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT);
    CHECK("BEARER-03", ble_mesh_scheduler_poll(&scheduler, 2u, &event) == 1 &&
                         event.type == BLE_MESH_SCHED_EVENT_RADIO_FAULT);
    enqueue_result = ble_mesh_scheduler_enqueue_ex(&scheduler, &item);
    CHECK("BEARER-03", enqueue_result.status == BLE_MESH_SCHED_ENQUEUE_INVALID);
    CHECK("BEARER-03", ble_mesh_scheduler_poll(&scheduler, 3u, &event) == 0);

    /* Faulted schedulers are fail-closed; a fresh instance owns RX validation. */
    ble_mesh_scheduler_init(&scheduler, 4u, local_adva);
    test_radio_script_rx(raw_valid, sizeof(raw_valid), 47u, BLE_RADIO_OP_OK);
    CHECK("BEARER-03", ble_mesh_scheduler_poll(&scheduler, 5u, &event) == 1);
    CHECK("BEARER-03", event.type == BLE_MESH_SCHED_EVENT_RX_ADV &&
                         event.rssi_magnitude_db == 47u &&
                         memcmp(event.adv_addr, &raw_valid[2], sizeof(event.adv_addr)) == 0 &&
                         event.adv_len == 3u && event.adv_data[2] == 0x06u);
    memcpy(raw_bad_s0, raw_valid, sizeof(raw_bad_s0));
    raw_bad_s0[0] = 0x02u;
    test_radio_script_rx(raw_bad_s0, sizeof(raw_bad_s0), 48u, BLE_RADIO_OP_OK);
    CHECK("BEARER-03", ble_mesh_scheduler_poll(&scheduler, 6u, &event) == 0 &&
                         event.type == BLE_MESH_SCHED_EVENT_NONE);
}

static void test_queue_capacity_eviction_and_link_service_order(void)
{
    ble_mesh_tx_queue_t capacity_queue;
    ble_mesh_tx_queue_t eviction_queue;
    ble_mesh_tx_queue_t wrap_queue;
    ble_mesh_tx_queue_t service_queue;
    ble_mesh_tx_enqueue_result_t result;
    ble_mesh_tx_selection_t selection;
    ble_mesh_tx_item_t data = make_item(BLE_MESH_TX_PRIORITY_DATA,
                                        BLE_MESH_TX_SERVICE_BEST_EFFORT,
                                        BLE_MESH_TX_TOKEN_NONE, 0u, 1u);
    ble_mesh_tx_item_t tracked_lower = make_item(BLE_MESH_TX_PRIORITY_DATA,
                                                  BLE_MESH_TX_SERVICE_CUSTODY_DATA,
                                                  0x31u, 0u, 2u);
    ble_mesh_tx_item_t untracked_lower = make_item(BLE_MESH_TX_PRIORITY_RELAY,
                                                    BLE_MESH_TX_SERVICE_BEST_EFFORT,
                                                    BLE_MESH_TX_TOKEN_NONE, 0u, 5u);
    ble_mesh_tx_item_t hack = make_item(BLE_MESH_TX_PRIORITY_HACK,
                                         BLE_MESH_TX_SERVICE_BEST_EFFORT,
                                         BLE_MESH_TX_TOKEN_NONE, 0u, 3u);
    ble_mesh_tx_item_t second_hack = hack;
    ble_mesh_tx_item_t third_hack = hack;
    ble_mesh_tx_item_t fourth_hack = hack;
    ble_mesh_tx_item_t custody = make_item(BLE_MESH_TX_PRIORITY_DATA,
                                           BLE_MESH_TX_SERVICE_CUSTODY_DATA,
                                           0x41u, 0u, 4u);
    uint8_t i;

    ble_mesh_tx_queue_init(&capacity_queue);
    for (i = 0u; i < BLE_MESH_TX_QUEUE_CAPACITY; i++) {
        result = ble_mesh_tx_queue_enqueue(&capacity_queue, &data);
        CHECK("BEARER-03", result.status == BLE_MESH_TX_ENQUEUE_OK);
    }
    result = ble_mesh_tx_queue_enqueue(&capacity_queue, &data);
    CHECK("BEARER-03", result.status == BLE_MESH_TX_ENQUEUE_FULL);

    ble_mesh_tx_queue_init(&eviction_queue);
    result = ble_mesh_tx_queue_enqueue(&eviction_queue, &tracked_lower);
    CHECK("BEARER-03", result.status == BLE_MESH_TX_ENQUEUE_OK);
    for (i = 1u; i < BLE_MESH_TX_QUEUE_CAPACITY; i++) {
        result = ble_mesh_tx_queue_enqueue(&eviction_queue, &untracked_lower);
        CHECK("BEARER-03", result.status == BLE_MESH_TX_ENQUEUE_OK);
    }
    result = ble_mesh_tx_queue_enqueue(&eviction_queue, &hack);
    CHECK("BEARER-03", result.status == BLE_MESH_TX_ENQUEUE_OK &&
                         result.evicted_token == 0x31u &&
                         ble_mesh_tx_queue_count(&eviction_queue) == BLE_MESH_TX_QUEUE_CAPACITY);

    ble_mesh_tx_queue_init(&wrap_queue);
    wrap_queue.next_ordinal = UINT32_MAX;
    result = ble_mesh_tx_queue_enqueue(&wrap_queue, &data);
    CHECK("BEARER-03", result.status == BLE_MESH_TX_ENQUEUE_OK &&
                         wrap_queue.next_ordinal == 0u);

    ble_mesh_tx_queue_init(&service_queue);
    custody.not_before_ms = 0u;
    hack.not_before_ms = 0u;
    CHECK("LINK-04", ble_mesh_tx_queue_enqueue(&service_queue, &custody).status ==
                       BLE_MESH_TX_ENQUEUE_OK);
    CHECK("LINK-04", ble_mesh_tx_queue_enqueue(&service_queue, &hack).status ==
                        BLE_MESH_TX_ENQUEUE_OK);
    CHECK("LINK-04", ble_mesh_tx_queue_enqueue(&service_queue, &second_hack).status ==
                        BLE_MESH_TX_ENQUEUE_OK);
    CHECK("LINK-04", ble_mesh_tx_queue_enqueue(&service_queue, &third_hack).status ==
                        BLE_MESH_TX_ENQUEUE_OK);
    CHECK("LINK-04", ble_mesh_tx_queue_select_due(&service_queue, 0u, custody.token,
                                                     &selection) == BLE_MESH_TX_SELECT_OK &&
                        selection.item.priority == BLE_MESH_TX_PRIORITY_HACK);
    CHECK("LINK-04", ble_mesh_tx_queue_complete(&service_queue, selection.index, 0u,
                                                   custody.token) == 1);
    CHECK("LINK-04", service_queue.custody_bypass_count == 0u);
    CHECK("LINK-04", ble_mesh_tx_queue_select_due(&service_queue, 1u, custody.token,
                                                     &selection) == BLE_MESH_TX_SELECT_OK &&
                        selection.item.priority == BLE_MESH_TX_PRIORITY_HACK);
    CHECK("LINK-04", ble_mesh_tx_queue_complete(&service_queue, selection.index,
                                                  BLE_RADIO_ADV_CH37, custody.token) == 1);
    CHECK("LINK-04", ble_mesh_tx_queue_select_due(&service_queue, 2u, custody.token,
                                                     &selection) == BLE_MESH_TX_SELECT_OK &&
                        selection.item.priority == BLE_MESH_TX_PRIORITY_HACK);
    CHECK("LINK-04", ble_mesh_tx_queue_complete(&service_queue, selection.index,
                                                   BLE_RADIO_ADV_CH38, custody.token) == 1);

    CHECK("LINK-04", service_queue.custody_bypass_count == 2u);
    CHECK("LINK-04", ble_mesh_tx_queue_enqueue(&service_queue, &fourth_hack).status ==
                        BLE_MESH_TX_ENQUEUE_OK);
    CHECK("LINK-04", ble_mesh_tx_queue_select_due(&service_queue, 3u, custody.token,
                                                     &selection) == BLE_MESH_TX_SELECT_OK &&
                        selection.item.token == custody.token);
}

static void test_hack_turnaround_due_selection(void)
{
    ble_mesh_tx_queue_t queue;
    ble_mesh_tx_selection_t selection;
    ble_mesh_tx_item_t hack = make_item(BLE_MESH_TX_PRIORITY_HACK,
                                         BLE_MESH_TX_SERVICE_BEST_EFFORT,
                                         BLE_MESH_TX_TOKEN_NONE, 8u, 0xa1u);
    ble_mesh_tx_item_t data = make_item(BLE_MESH_TX_PRIORITY_DATA,
                                         BLE_MESH_TX_SERVICE_BEST_EFFORT,
                                         BLE_MESH_TX_TOKEN_NONE, 0u, 0xa2u);

    ble_mesh_tx_queue_init(&queue);
    CHECK("LINK-02", ble_mesh_tx_queue_enqueue(&queue, &hack).status ==
                       BLE_MESH_TX_ENQUEUE_OK);
    CHECK("LINK-02", ble_mesh_tx_queue_select_due(&queue, 7u,
                                                     BLE_MESH_TX_TOKEN_NONE,
                                                     &selection) ==
                       BLE_MESH_TX_SELECT_NONE);
    CHECK("LINK-02", ble_mesh_tx_queue_select_due(&queue, 8u,
                                                     BLE_MESH_TX_TOKEN_NONE,
                                                     &selection) ==
                       BLE_MESH_TX_SELECT_OK &&
                       selection.item.priority == BLE_MESH_TX_PRIORITY_HACK);

    ble_mesh_tx_queue_init(&queue);
    CHECK("LINK-02", ble_mesh_tx_queue_enqueue(&queue, &hack).status ==
                       BLE_MESH_TX_ENQUEUE_OK);
    CHECK("LINK-02", ble_mesh_tx_queue_enqueue(&queue, &data).status ==
                       BLE_MESH_TX_ENQUEUE_OK);
    CHECK("LINK-02", ble_mesh_tx_queue_select_due(&queue, 0u,
                                                     BLE_MESH_TX_TOKEN_NONE,
                                                     &selection) ==
                       BLE_MESH_TX_SELECT_OK && selection.item.adv_data[0] == 0xa2u);

    ble_mesh_tx_queue_init(&queue);
    hack.not_before_ms = 4u;
    CHECK("LINK-02", ble_mesh_tx_queue_enqueue(&queue, &hack).status ==
                       BLE_MESH_TX_ENQUEUE_OK);
    CHECK("LINK-02", ble_mesh_tx_queue_select_due(&queue, 3u,
                                                     BLE_MESH_TX_TOKEN_NONE,
                                                     &selection) ==
                       BLE_MESH_TX_SELECT_NONE);
    CHECK("LINK-02", ble_mesh_tx_queue_select_due(&queue, 4u,
                                                     BLE_MESH_TX_TOKEN_NONE,
                                                     &selection) ==
                       BLE_MESH_TX_SELECT_OK &&
                       selection.item.priority == BLE_MESH_TX_PRIORITY_HACK);
}

int main(void)
{
    test_typed_radio_precise_channel_faults();
    test_scheduler_fault_latch_and_fresh_rx_admission();
    test_queue_capacity_eviction_and_link_service_order();
    test_hack_turnaround_due_selection();
    if (failures != 0u) {
        printf("tavrn_radio_scheduler_v2 RED tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_radio_scheduler_v2 tests passed\n");
    return 0;
}
