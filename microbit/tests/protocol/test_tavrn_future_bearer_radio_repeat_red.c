#include "ble_mesh_scheduler.h"
#include "ble_mesh_tx_queue.h"
#include "test_radio_scheduler_support.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * This staged BEARER-05/06 contract uses the production radio, scheduler,
 * queue, and host register shim. It intentionally contains no alternate
 * radio or scheduler model.
 *
 * Required repeated-advertising API:
 *   BLE_RADIO_REPEATED_ADV_API == 1
 *   BLE_RADIO_MAX_ADV_SWEEPS == 2
 *   ble_radio_tx_result_t adds requested_sweep_count, attempted_sweep_count,
 *   and completed_channel_masks[BLE_RADIO_MAX_ADV_SWEEPS], while retaining
 *   requested_channel_mask, completed_channel_mask, and fault.
 *   ble_radio_try_advertise_sweeps(adv, adv_len, addr6, channel_mask,
 *                                  uint8_t sweep_count, timeout_ms)
 *
 * Scheduler TX events carry equivalent requested/attempted/mask evidence.
 * Their requested sweep count is the selected item's bounded BU cost;
 * aggregate completion is the OR of the per-sweep masks.
 */
#if !defined(BLE_RADIO_REPEATED_ADV_API)
#error "RADIO_REPEAT_RED_GATE: BLE_RADIO_REPEATED_ADV_API=1 is required"
#elif BLE_RADIO_REPEATED_ADV_API != 1
#error "RADIO_REPEAT_RED_GATE: BLE_RADIO_REPEATED_ADV_API must equal 1"
#endif

#if defined(BLE_RADIO_REPEATED_ADV_API) && BLE_RADIO_REPEATED_ADV_API == 1

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

static const uint8_t local_adva[6] = {
    0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu,
};
static const uint8_t adv_data[] = { 0x02u, 0x01u, 0x06u };

static uint8_t masks_or(const uint8_t masks[BLE_RADIO_MAX_ADV_SWEEPS])
{
    uint8_t index;
    uint8_t result = 0u;

    for (index = 0u; index < BLE_RADIO_MAX_ADV_SWEEPS; index++) {
        result |= masks[index];
    }
    return result;
}

static int tx_result_evidence_is_valid(const ble_radio_tx_result_t *result,
                                       uint8_t requested_sweeps,
                                       uint8_t attempted_sweeps,
                                       uint8_t requested_mask)
{
    uint8_t index;

    if (result == NULL || requested_sweeps == 0u ||
        requested_sweeps > BLE_RADIO_MAX_ADV_SWEEPS ||
        result->requested_channel_mask != requested_mask ||
        result->requested_sweep_count != requested_sweeps ||
        result->attempted_sweep_count != attempted_sweeps ||
        result->attempted_sweep_count > result->requested_sweep_count ||
        result->attempted_sweep_count > BLE_RADIO_MAX_ADV_SWEEPS) {
        return 0;
    }
    for (index = 0u; index < BLE_RADIO_MAX_ADV_SWEEPS; index++) {
        if ((result->completed_channel_masks[index] &
             (uint8_t)~requested_mask) != 0u ||
            (index >= result->attempted_sweep_count &&
             result->completed_channel_masks[index] != 0u)) {
            return 0;
        }
    }
    return result->completed_channel_mask ==
        masks_or(result->completed_channel_masks);
}

static int event_evidence_is_valid(const ble_mesh_sched_event_t *event,
                                   uint8_t requested_sweeps,
                                   uint8_t attempted_sweeps,
                                   uint8_t requested_mask)
{
    uint8_t index;

    if (event == NULL || requested_sweeps == 0u ||
        requested_sweeps > BLE_RADIO_MAX_ADV_SWEEPS ||
        event->tx_requested_channel_mask != requested_mask ||
        event->tx_requested_sweep_count != requested_sweeps ||
        event->tx_attempted_sweep_count != attempted_sweeps ||
        event->tx_attempted_sweep_count > event->tx_requested_sweep_count ||
        event->tx_attempted_sweep_count > BLE_RADIO_MAX_ADV_SWEEPS) {
        return 0;
    }
    for (index = 0u; index < BLE_RADIO_MAX_ADV_SWEEPS; index++) {
        if ((event->tx_completed_channel_masks[index] &
             (uint8_t)~requested_mask) != 0u ||
            (index >= event->tx_attempted_sweep_count &&
             event->tx_completed_channel_masks[index] != 0u)) {
            return 0;
        }
    }
    return event->tx_completed_channel_mask ==
        masks_or(event->tx_completed_channel_masks);
}

static int trace_matches(const uint8_t *expected, size_t expected_length)
{
    const uint8_t *trace;
    size_t trace_length;

    trace = test_radio_channel_trace(&trace_length);
    return trace != NULL && trace_length == expected_length &&
        memcmp(trace, expected, expected_length) == 0;
}

static int prepare_direct_radio(void)
{
    test_radio_script_reset();
    return ble_radio_try_init(2u) == BLE_RADIO_OP_OK &&
        ble_radio_try_idle(2u) == BLE_RADIO_OP_OK;
}

static void test_compatibility_wrapper_is_one_sweep(void)
{
    static const uint8_t expected_trace[] = { 37u, 38u, 39u };
    ble_radio_tx_result_t result;
    unsigned int disables_before;

    if (!prepare_direct_radio()) {
        CHECK("BEARER-05", 0);
        return;
    }
    test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, BLE_RADIO_ADV_CH_ALL,
                         BLE_RADIO_OP_OK, TEST_RADIO_FAILURE_NONE);
    CHECK("BEARER-05", ble_radio_try_listen_once(37u, 2u) == BLE_RADIO_OP_OK);
    disables_before = test_radio_disable_count();
    result = ble_radio_try_advertise_channels(
        adv_data, sizeof(adv_data), local_adva, BLE_RADIO_ADV_CH_ALL, 2u);
    CHECK("BEARER-05", result.fault == BLE_RADIO_OP_OK &&
                       tx_result_evidence_is_valid(
                           &result, 1u, 1u, BLE_RADIO_ADV_CH_ALL) &&
                       result.completed_channel_masks[0] == BLE_RADIO_ADV_CH_ALL &&
                       result.completed_channel_masks[1] == 0u &&
                       trace_matches(expected_trace, sizeof(expected_trace)) &&
                       test_radio_disable_count() == disables_before + 1u);
}

static void test_two_sweeps_are_ordered_atomic_and_bounded(void)
{
    static const uint8_t expected_trace[] = { 37u, 38u, 39u, 37u, 38u, 39u };
    ble_radio_tx_result_t result;
    unsigned int disables_before;

    if (!prepare_direct_radio()) {
        CHECK("BEARER-05", 0);
        return;
    }
    test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, BLE_RADIO_ADV_CH_ALL,
                         BLE_RADIO_OP_OK, TEST_RADIO_FAILURE_NONE);
    CHECK("BEARER-05", ble_radio_try_listen_once(37u, 2u) == BLE_RADIO_OP_OK);
    disables_before = test_radio_disable_count();
    result = ble_radio_try_advertise_sweeps(
        adv_data, sizeof(adv_data), local_adva, BLE_RADIO_ADV_CH_ALL, 2u, 2u);
    CHECK("BEARER-05", result.fault == BLE_RADIO_OP_OK &&
                       tx_result_evidence_is_valid(
                           &result, 2u, 2u, BLE_RADIO_ADV_CH_ALL) &&
                       result.completed_channel_masks[0] == BLE_RADIO_ADV_CH_ALL &&
                       result.completed_channel_masks[1] == BLE_RADIO_ADV_CH_ALL &&
                       trace_matches(expected_trace, sizeof(expected_trace)) &&
                       test_radio_disable_count() == disables_before + 1u);
}

static void test_invalid_sweep_count_has_no_physical_start(void)
{
    static const uint8_t no_trace[] = { 0u };
    const uint8_t invalid_counts[] = {
        0u, (uint8_t)(BLE_RADIO_MAX_ADV_SWEEPS + 1u),
    };
    uint8_t index;

    for (index = 0u; index < sizeof(invalid_counts) / sizeof(invalid_counts[0]);
         index++) {
        ble_radio_tx_result_t result;
        unsigned int disables_before;

        if (!prepare_direct_radio()) {
            CHECK("BEARER-05", 0);
            continue;
        }
        test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, BLE_RADIO_ADV_CH_ALL,
                             BLE_RADIO_OP_OK, TEST_RADIO_FAILURE_NONE);
        CHECK("BEARER-05", ble_radio_try_listen_once(37u, 2u) == BLE_RADIO_OP_OK);
        disables_before = test_radio_disable_count();
        result = ble_radio_try_advertise_sweeps(
            adv_data, sizeof(adv_data), local_adva, BLE_RADIO_ADV_CH_ALL,
            invalid_counts[index], 2u);
        CHECK("BEARER-05", result.fault == BLE_RADIO_OP_INVALID_ARGUMENT &&
                           result.requested_channel_mask == 0u &&
                           result.completed_channel_mask == 0u &&
                           result.requested_sweep_count == 0u &&
                           result.attempted_sweep_count == 0u &&
                           result.completed_channel_masks[0] == 0u &&
                           result.completed_channel_masks[1] == 0u &&
                           trace_matches(no_trace, 0u) &&
                           test_radio_disable_count() == disables_before);
    }
}

static void test_second_sweep_fault_preserves_exact_radio_evidence(void)
{
    static const uint8_t expected_trace[] = { 37u, 38u, 39u, 37u, 38u };
    ble_radio_tx_result_t result;
    unsigned int disables_before;

    if (!prepare_direct_radio()) {
        CHECK("BEARER-05", 0);
        return;
    }
    test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, BLE_RADIO_ADV_CH_ALL,
                         BLE_RADIO_OP_STATE_TIMEOUT, TEST_RADIO_FAILURE_NONE);
    test_radio_script_tx_repeat_failure(2u, TEST_RADIO_FAILURE_CH38);
    CHECK("BEARER-05", ble_radio_try_listen_once(37u, 2u) == BLE_RADIO_OP_OK);
    disables_before = test_radio_disable_count();
    result = ble_radio_try_advertise_sweeps(
        adv_data, sizeof(adv_data), local_adva, BLE_RADIO_ADV_CH_ALL, 2u, 2u);
    CHECK("BEARER-05", result.fault == BLE_RADIO_OP_STATE_TIMEOUT &&
                       tx_result_evidence_is_valid(
                           &result, 2u, 2u, BLE_RADIO_ADV_CH_ALL) &&
                       result.completed_channel_masks[0] == BLE_RADIO_ADV_CH_ALL &&
                       result.completed_channel_masks[1] == BLE_RADIO_ADV_CH37 &&
                       result.completed_channel_mask == BLE_RADIO_ADV_CH_ALL &&
                       trace_matches(expected_trace, sizeof(expected_trace)) &&
                       test_radio_disable_count() == disables_before + 2u);
}

static int find_item_index(const ble_mesh_tx_queue_t *queue,
                           ble_mesh_tx_token_t token, uint8_t *index_out)
{
    uint8_t index;

    if (queue == NULL || index_out == NULL || token == BLE_MESH_TX_TOKEN_NONE) {
        return 0;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (queue->entries[index].occupied != 0u &&
            queue->entries[index].item.token == token) {
            *index_out = index;
            return 1;
        }
    }
    return 0;
}

static ble_mesh_tx_item_t make_repeated_item(uint8_t marker,
                                              ble_mesh_tx_token_t token)
{
    ble_mesh_tx_item_t item;

    memset(&item, 0, sizeof(item));
    item.adv_len = sizeof(adv_data);
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = BLE_MESH_TX_PRIORITY_CONTROL;
    item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
    item.not_before_ms = 0u;
    item.expiry_ms = 10000u;
    item.sweep_count = BLE_MESH_TX_SWEEP_COUNT_TWO;
    item.budget_class = BLE_MESH_TX_BUDGET_GENERAL;
    item.token = token;
    memcpy(item.adv_data, adv_data, sizeof(adv_data));
    item.adv_data[0] = marker;
    return item;
}

static ble_radio_tx_result_t make_partial_repeat_result(void)
{
    ble_radio_tx_result_t result;

    memset(&result, 0, sizeof(result));
    result.requested_channel_mask = BLE_RADIO_ADV_CH_ALL;
    result.completed_channel_mask = BLE_RADIO_ADV_CH_ALL;
    result.requested_sweep_count = BLE_RADIO_MAX_ADV_SWEEPS;
    result.attempted_sweep_count = BLE_RADIO_MAX_ADV_SWEEPS;
    result.completed_channel_masks[0] = BLE_RADIO_ADV_CH_ALL;
    result.completed_channel_masks[1] = BLE_RADIO_ADV_CH37;
    result.fault = BLE_RADIO_OP_STATE_TIMEOUT;
    return result;
}

static void test_checked_retirement_validates_repeated_evidence(void)
{
    ble_mesh_tx_queue_t queue;
    ble_mesh_tx_item_t item = make_repeated_item(0x91u, 0x6101u);
    ble_radio_tx_result_t result = make_partial_repeat_result();
    uint8_t item_index;

    ble_mesh_tx_queue_init(&queue);
    if (ble_mesh_tx_queue_enqueue(&queue, &item).status != BLE_MESH_TX_ENQUEUE_OK ||
        !find_item_index(&queue, item.token, &item_index)) {
        CHECK("LINK-07", 0);
        return;
    }

    result.requested_sweep_count = BLE_MESH_TX_SWEEP_COUNT_ONE;
    CHECK("BEARER-06", ble_mesh_tx_queue_retire(
                           &queue, item_index, BLE_MESH_TX_TERMINAL_TX_DONE,
                           &result) == BLE_MESH_TX_RETIRE_INVALID &&
                         ble_mesh_tx_queue_count(&queue) == 1u);
    result = make_partial_repeat_result();
    result.attempted_sweep_count = BLE_MESH_TX_SWEEP_COUNT_ONE;
    CHECK("BEARER-06", ble_mesh_tx_queue_retire(
                           &queue, item_index, BLE_MESH_TX_TERMINAL_TX_DONE,
                           &result) == BLE_MESH_TX_RETIRE_INVALID &&
                         ble_mesh_tx_queue_count(&queue) == 1u);
    result = make_partial_repeat_result();
    result.attempted_sweep_count = (uint8_t)(BLE_RADIO_MAX_ADV_SWEEPS + 1u);
    CHECK("BEARER-06", ble_mesh_tx_queue_retire(
                           &queue, item_index, BLE_MESH_TX_TERMINAL_TX_DONE,
                           &result) == BLE_MESH_TX_RETIRE_INVALID &&
                         ble_mesh_tx_queue_count(&queue) == 1u);
    result = make_partial_repeat_result();
    result.completed_channel_masks[1] = 0x80u;
    result.completed_channel_mask = (uint8_t)(BLE_RADIO_ADV_CH_ALL | 0x80u);
    CHECK("BEARER-06", ble_mesh_tx_queue_retire(
                           &queue, item_index, BLE_MESH_TX_TERMINAL_TX_DONE,
                           &result) == BLE_MESH_TX_RETIRE_INVALID &&
                         ble_mesh_tx_queue_count(&queue) == 1u);
    result = make_partial_repeat_result();
    result.completed_channel_mask = BLE_RADIO_ADV_CH37;
    CHECK("BEARER-06", ble_mesh_tx_queue_retire(
                           &queue, item_index, BLE_MESH_TX_TERMINAL_TX_DONE,
                           &result) == BLE_MESH_TX_RETIRE_INVALID &&
                         ble_mesh_tx_queue_count(&queue) == 1u);

    result = make_partial_repeat_result();
    CHECK("BEARER-06", ble_mesh_tx_queue_retire(
                           &queue, item_index, BLE_MESH_TX_TERMINAL_TX_DONE,
                           &result) == BLE_MESH_TX_RETIRE_OK &&
                         ble_mesh_tx_queue_count(&queue) == 0u);
}

static int prepare_scheduler(ble_mesh_scheduler_t *scheduler)
{
    test_radio_script_reset();
    if (ble_radio_try_init(2u) != BLE_RADIO_OP_OK ||
        ble_radio_try_idle(2u) != BLE_RADIO_OP_OK) {
        return 0;
    }
    ble_mesh_scheduler_init(scheduler, 0u, local_adva);
    return scheduler->routed_started == 1u;
}

static void test_scheduler_repeated_events_and_terminals(void)
{
    static const uint8_t full_trace[] = { 37u, 38u, 39u, 37u, 38u, 39u };
    static const uint8_t partial_trace[] = { 37u, 38u, 39u, 37u, 38u };
    static const uint8_t failed_trace[] = { 37u };
    typedef struct scheduler_case {
        uint8_t marker;
        ble_mesh_tx_token_t token;
        uint8_t failure_sweep;
        test_radio_failure_location_t failure_location;
        uint8_t expected_attempted_sweeps;
        uint8_t first_completed_mask;
        uint8_t second_completed_mask;
        ble_mesh_sched_event_type_t event_type;
        ble_mesh_sched_fault_t fault;
        const uint8_t *trace;
        size_t trace_length;
    } scheduler_case_t;
    static const scheduler_case_t cases[] = {
        { 0xa1u, 0x6102u, 0u, TEST_RADIO_FAILURE_NONE, 2u,
          BLE_RADIO_ADV_CH_ALL,
          BLE_RADIO_ADV_CH_ALL, BLE_MESH_SCHED_EVENT_TX_DONE,
          BLE_MESH_SCHED_FAULT_NONE, full_trace, sizeof(full_trace) },
        { 0xa2u, 0x6103u, 2u, TEST_RADIO_FAILURE_CH38, 2u,
          BLE_RADIO_ADV_CH_ALL,
          BLE_RADIO_ADV_CH37, BLE_MESH_SCHED_EVENT_TX_DONE,
          BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT, partial_trace,
          sizeof(partial_trace) },
        { 0xa3u, 0x6104u, 1u, TEST_RADIO_FAILURE_CH37, 1u, 0u, 0u,
          BLE_MESH_SCHED_EVENT_TX_FAILED, BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT,
          failed_trace, sizeof(failed_trace) },
    };
    uint8_t index;

    for (index = 0u; index < sizeof(cases) / sizeof(cases[0]); index++) {
        ble_mesh_scheduler_t scheduler;
        ble_mesh_sched_event_t event;
        ble_mesh_tx_item_t item = make_repeated_item(cases[index].marker,
                                                      cases[index].token);

        if (!prepare_scheduler(&scheduler)) {
            CHECK("LINK-07", 0);
            continue;
        }
        if (cases[index].failure_sweep == 0u) {
            test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, BLE_RADIO_ADV_CH_ALL,
                                 BLE_RADIO_OP_OK, TEST_RADIO_FAILURE_NONE);
        } else if (cases[index].failure_sweep == 1u) {
            test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, 0u,
                                 BLE_RADIO_OP_STATE_TIMEOUT,
                                 TEST_RADIO_FAILURE_CH37);
        } else {
            test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, BLE_RADIO_ADV_CH_ALL,
                                 BLE_RADIO_OP_STATE_TIMEOUT,
                                 TEST_RADIO_FAILURE_NONE);
            test_radio_script_tx_repeat_failure(cases[index].failure_sweep,
                                                cases[index].failure_location);
        }
        CHECK("LINK-07", ble_mesh_scheduler_enqueue_ex(&scheduler, &item).status ==
                         BLE_MESH_SCHED_ENQUEUE_OK &&
                      ble_mesh_scheduler_poll(&scheduler, 1u, &event) == 1 &&
                      event.type == cases[index].event_type &&
                      event.tx_token == item.token &&
                      event.fault == cases[index].fault &&
                      ble_mesh_tx_queue_count(&scheduler.routed_tx_queue) == 0u);
        CHECK("BEARER-06", event_evidence_is_valid(
                               &event, item.sweep_count,
                               cases[index].expected_attempted_sweeps,
                              item.channel_mask) &&
                              event.tx_completed_channel_masks[0] ==
                                  cases[index].first_completed_mask &&
                              event.tx_completed_channel_masks[1] ==
                                  cases[index].second_completed_mask &&
                              trace_matches(cases[index].trace,
                                            cases[index].trace_length));
    }
}

int main(void)
{
    CHECK("BEARER-05", BLE_RADIO_MAX_ADV_SWEEPS == 2u);
    test_compatibility_wrapper_is_one_sweep();
    test_two_sweeps_are_ordered_atomic_and_bounded();
    test_invalid_sweep_count_has_no_physical_start();
    test_second_sweep_fault_preserves_exact_radio_evidence();
    test_checked_retirement_validates_repeated_evidence();
    test_scheduler_repeated_events_and_terminals();

    if (failures != 0u) {
        printf("tavrn_future_bearer radio repeat RED tests failed: %u assertion(s)\n",
               failures);
        return 1;
    }
    printf("tavrn_future_bearer radio repeat tests passed\n");
    return 0;
}

#endif /* BLE_RADIO_REPEATED_ADV_API */
