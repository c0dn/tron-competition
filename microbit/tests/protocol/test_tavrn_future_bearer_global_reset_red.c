#include "ble_mesh_scheduler.h"
#include "ble_mesh_tx_queue.h"
#include "test_radio_scheduler_support.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * This staged scheduler contract deliberately uses the production scheduler,
 * queue, RADIO driver, and host register shim.  It must not be satisfied by a
 * parallel scheduler fixture.
 *
 * Required next API:
 *   BLE_MESH_SCHED_GLOBAL_RESET_API == 1
 *   typedef struct ble_mesh_sched_global_reset {
 *       ble_mesh_sched_fault_t fault;
 *       ble_mesh_tx_token_t selected_token;
 *       uint8_t selected_requested_channel_mask;
 *       uint8_t selected_completed_channel_mask;
 *       uint8_t selected_tx_valid;
 *   } ble_mesh_sched_global_reset_t;
 *   typedef int (*ble_mesh_sched_global_reset_handler_t)(
 *       void *context, const ble_mesh_sched_global_reset_t *reset);
 *   int ble_mesh_scheduler_set_global_reset_handler(
 *       ble_mesh_scheduler_t *sched,
 *       ble_mesh_sched_global_reset_handler_t handler,
 *       void *context);
 *
 * A missing handler rejects tracked admission.  This is the observable
 * fail-closed rule that prevents a later fault from raw-clearing an owner the
 * scheduler cannot synchronously reset.
 */
#if !defined(BLE_MESH_SCHED_GLOBAL_RESET_API)
#error "GLOBAL_RESET_RED_GATE: BLE_MESH_SCHED_GLOBAL_RESET_API=1 is required"
#elif BLE_MESH_SCHED_GLOBAL_RESET_API != 1
#error "GLOBAL_RESET_RED_GATE: BLE_MESH_SCHED_GLOBAL_RESET_API must equal 1"
#endif

#if defined(BLE_MESH_SCHED_GLOBAL_RESET_API) && \
    BLE_MESH_SCHED_GLOBAL_RESET_API == 1

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

typedef enum reset_order {
    RESET_ORDER_TERMINAL = 1,
    RESET_ORDER_GLOBAL,
} reset_order_t;

typedef struct reset_probe {
    ble_mesh_scheduler_t *scheduler;
    ble_mesh_tx_token_t selected_token;
    uint8_t selected_marker;
    ble_mesh_tx_terminal_reason_t terminal_reason;
    uint8_t terminal_requested_mask;
    uint8_t terminal_completed_mask;
    ble_radio_op_result_t terminal_fault;
    ble_mesh_sched_fault_t reset_fault;
    uint8_t reset_selected_valid;
    uint8_t reset_requested_mask;
    uint8_t reset_completed_mask;
    ble_mesh_tx_token_t live_tokens[BLE_MESH_TX_QUEUE_CAPACITY];
    uint8_t live_markers[BLE_MESH_TX_QUEUE_CAPACITY];
    uint8_t live_count;
    reset_order_t order[2];
    uint8_t order_count;
    unsigned int terminal_calls;
    unsigned int terminal_live_calls;
    unsigned int global_calls;
} reset_probe_t;

static const uint8_t local_adva[6] = {
    0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu,
};

static ble_mesh_tx_item_t make_item(
    uint8_t marker, ble_mesh_tx_priority_t priority,
    ble_mesh_tx_service_class_t service_class,
    ble_mesh_tx_owner_kind_t owner_kind,
    ble_mesh_tx_owner_domain_t owner_domain, ble_mesh_tx_token_t token,
    uint32_t not_before_ms)
{
    ble_mesh_tx_item_t item;

    memset(&item, 0, sizeof(item));
    item.adv_len = 3u;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = priority;
    item.service_class = service_class;
    item.not_before_ms = not_before_ms;
    item.expiry_ms = 10000u;
    item.sweep_count = service_class == BLE_MESH_TX_SERVICE_CUSTODY_DATA ?
        BLE_MESH_TX_SWEEP_COUNT_TWO : BLE_MESH_TX_SWEEP_COUNT_ONE;
    item.budget_class = priority == BLE_MESH_TX_PRIORITY_HACK ?
        BLE_MESH_TX_BUDGET_CRITICAL : BLE_MESH_TX_BUDGET_GENERAL;
    item.owner_kind = owner_kind;
    item.owner_domain = owner_domain;
    item.token = token;
    item.adv_data[0] = marker;
    item.adv_data[1] = 0x01u;
    item.adv_data[2] = 0x06u;
    return item;
}

static int queue_has_token(const ble_mesh_tx_queue_t *queue,
                           ble_mesh_tx_token_t token)
{
    uint8_t index;

    if (queue == NULL || token == BLE_MESH_TX_TOKEN_NONE) {
        return 0;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (queue->entries[index].occupied != 0u &&
            queue->entries[index].item.token == token) {
            return 1;
        }
    }
    return 0;
}

static int queue_has_marker(const ble_mesh_tx_queue_t *queue, uint8_t marker)
{
    uint8_t index;

    if (queue == NULL) {
        return 0;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (queue->entries[index].occupied != 0u &&
            queue->entries[index].item.adv_data[0] == marker) {
            return 1;
        }
    }
    return 0;
}

static void record_order(reset_probe_t *probe, reset_order_t order)
{
    if (probe == NULL || probe->order_count >=
                             (uint8_t)(sizeof(probe->order) / sizeof(probe->order[0]))) {
        CHECK("LINK-07", 0);
        return;
    }
    probe->order[probe->order_count++] = order;
}

static int record_selected_terminal(void *context,
                                    const ble_mesh_tx_terminal_t *terminal)
{
    reset_probe_t *probe = context;

    if (probe == NULL || probe->scheduler == NULL || terminal == NULL) {
        CHECK("LINK-07", 0);
        return 0;
    }
    probe->terminal_calls++;
    record_order(probe, RESET_ORDER_TERMINAL);
    if (queue_has_token(&probe->scheduler->routed_tx_queue,
                        probe->selected_token) &&
        queue_has_marker(&probe->scheduler->routed_tx_queue,
                         probe->selected_marker)) {
        probe->terminal_live_calls++;
    }
    CHECK("LINK-07", queue_has_token(&probe->scheduler->routed_tx_queue,
                                       probe->selected_token) &&
                     queue_has_marker(&probe->scheduler->routed_tx_queue,
                                      probe->selected_marker));
    CHECK("LINK-07", terminal->reason == probe->terminal_reason &&
                     terminal->token == probe->selected_token);
    CHECK("BEARER-06", terminal->requested_channel_mask ==
                         probe->terminal_requested_mask &&
                         terminal->completed_channel_mask ==
                         probe->terminal_completed_mask &&
                         terminal->fault == probe->terminal_fault);
    return 1;
}

static int record_global_reset(void *context,
                               const ble_mesh_sched_global_reset_t *reset)
{
    reset_probe_t *probe = context;
    uint8_t index;

    if (probe == NULL || probe->scheduler == NULL || reset == NULL) {
        CHECK("LINK-07", 0);
        return 0;
    }
    probe->global_calls++;
    record_order(probe, RESET_ORDER_GLOBAL);
    CHECK("LINK-07", reset->fault == probe->reset_fault &&
                     reset->selected_tx_valid == probe->reset_selected_valid);
    CHECK("BEARER-06", reset->selected_token == probe->selected_token &&
                         reset->selected_requested_channel_mask ==
                         probe->reset_requested_mask &&
                         reset->selected_completed_channel_mask ==
                         probe->reset_completed_mask);
    if (probe->reset_selected_valid != 0u) {
        CHECK("LINK-07", probe->terminal_calls == 1u &&
                         probe->terminal_live_calls == 1u &&
                         !queue_has_token(&probe->scheduler->routed_tx_queue,
                                          probe->selected_token) &&
                         !queue_has_marker(&probe->scheduler->routed_tx_queue,
                                           probe->selected_marker));
    } else {
        CHECK("LINK-07", probe->terminal_calls == 0u &&
                         probe->selected_token == BLE_MESH_TX_TOKEN_NONE &&
                         probe->reset_requested_mask == 0u &&
                         probe->reset_completed_mask == 0u);
    }
    CHECK("LINK-07", ble_mesh_tx_queue_count(&probe->scheduler->routed_tx_queue) ==
                     probe->live_count);
    for (index = 0u; index < probe->live_count; index++) {
        if (probe->live_tokens[index] != BLE_MESH_TX_TOKEN_NONE) {
            CHECK("LINK-07", queue_has_token(&probe->scheduler->routed_tx_queue,
                                               probe->live_tokens[index]));
        }
        CHECK("BEARER-06", queue_has_marker(&probe->scheduler->routed_tx_queue,
                                              probe->live_markers[index]));
    }
    return 1;
}

static int prepare_scheduler(ble_mesh_scheduler_t *scheduler)
{
    test_radio_script_reset();
    if (ble_radio_try_init(2u) != BLE_RADIO_OP_OK ||
        ble_radio_try_idle(2u) != BLE_RADIO_OP_OK) {
        return 0;
    }
    ble_mesh_scheduler_init(scheduler, 0u, local_adva);
    return 1;
}

static int configure_global_reset_probe(ble_mesh_scheduler_t *scheduler,
                                        reset_probe_t *probe)
{
    if (scheduler == NULL || probe == NULL) {
        return 0;
    }
    memset(probe, 0, sizeof(*probe));
    probe->scheduler = scheduler;
    if (!ble_mesh_scheduler_set_global_reset_handler(
            scheduler, record_global_reset, probe) ||
        !ble_mesh_tx_queue_set_terminal_handler(&scheduler->routed_tx_queue,
                                                record_selected_terminal,
                                                probe)) {
        return 0;
    }
    return 1;
}

static void expect_live(reset_probe_t *probe, ble_mesh_tx_token_t token,
                        uint8_t marker)
{
    if (probe == NULL || probe->live_count >= BLE_MESH_TX_QUEUE_CAPACITY) {
        CHECK("LINK-07", 0);
        return;
    }
    probe->live_tokens[probe->live_count] = token;
    probe->live_markers[probe->live_count] = marker;
    probe->live_count++;
}

static void assert_fault_idle_and_no_replay(ble_mesh_scheduler_t *scheduler,
                                            reset_probe_t *probe,
                                            uint32_t now_ms)
{
    ble_mesh_sched_event_t event;
    ble_mesh_tx_item_t late_item = make_item(
        0xe1u, BLE_MESH_TX_PRIORITY_CONTROL,
        BLE_MESH_TX_SERVICE_BEST_EFFORT, BLE_MESH_TX_OWNER_AODV,
        BLE_MESH_TX_OWNER_DOMAIN_LOW, 0x7e01u, now_ms);

    CHECK("LINK-07", ble_mesh_tx_queue_count(&scheduler->routed_tx_queue) == 0u &&
                     scheduler->routed_tx_queue.custody_bypass_count == 0u &&
                     scheduler->custody_bypass_count == 0u);
    CHECK("LINK-07", ble_mesh_scheduler_enqueue_ex(scheduler, &late_item).status ==
                     BLE_MESH_SCHED_ENQUEUE_INVALID);
    CHECK("LINK-07", ble_mesh_scheduler_poll(scheduler, now_ms, &event) == 0 &&
                     event.type == BLE_MESH_SCHED_EVENT_NONE &&
                     probe->global_calls == 1u && probe->terminal_calls <= 1u);
}

static void test_partial_selected_terminal_precedes_one_global_reset(void)
{
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    reset_probe_t probe;
    ble_mesh_tx_item_t selected = make_item(
        0xa1u, BLE_MESH_TX_PRIORITY_HACK, BLE_MESH_TX_SERVICE_BEST_EFFORT,
        BLE_MESH_TX_OWNER_LINK, BLE_MESH_TX_OWNER_DOMAIN_LOW, 0x4101u, 0u);
    ble_mesh_tx_item_t low = make_item(
        0xa2u, BLE_MESH_TX_PRIORITY_CONTROL, BLE_MESH_TX_SERVICE_BEST_EFFORT,
        BLE_MESH_TX_OWNER_AODV, BLE_MESH_TX_OWNER_DOMAIN_LOW, 0x4102u, 0u);
    ble_mesh_tx_item_t high = make_item(
        0xa3u, BLE_MESH_TX_PRIORITY_DATA, BLE_MESH_TX_SERVICE_BEST_EFFORT,
        BLE_MESH_TX_OWNER_REPAIR, BLE_MESH_TX_OWNER_DOMAIN_HIGH, 0x8103u, 0u);
    ble_mesh_tx_item_t anonymous = make_item(
        0xa4u, BLE_MESH_TX_PRIORITY_RELAY, BLE_MESH_TX_SERVICE_BEST_EFFORT,
        BLE_MESH_TX_OWNER_NONE, BLE_MESH_TX_OWNER_DOMAIN_NONE,
        BLE_MESH_TX_TOKEN_NONE, 0u);

    if (!prepare_scheduler(&scheduler) ||
        !configure_global_reset_probe(&scheduler, &probe)) {
        CHECK("LINK-07", 0);
        return;
    }
    probe.selected_token = selected.token;
    probe.selected_marker = selected.adv_data[0];
    probe.terminal_reason = BLE_MESH_TX_TERMINAL_TX_DONE;
    probe.terminal_requested_mask = BLE_RADIO_ADV_CH_ALL;
    probe.terminal_completed_mask = BLE_RADIO_ADV_CH37;
    probe.terminal_fault = BLE_RADIO_OP_STATE_TIMEOUT;
    probe.reset_fault = BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT;
    probe.reset_selected_valid = 1u;
    probe.reset_requested_mask = BLE_RADIO_ADV_CH_ALL;
    probe.reset_completed_mask = BLE_RADIO_ADV_CH37;
    expect_live(&probe, low.token, low.adv_data[0]);
    expect_live(&probe, high.token, high.adv_data[0]);
    expect_live(&probe, BLE_MESH_TX_TOKEN_NONE, anonymous.adv_data[0]);

    CHECK("LINK-07", ble_mesh_scheduler_enqueue_ex(&scheduler, &selected).status ==
                     BLE_MESH_SCHED_ENQUEUE_OK &&
                     ble_mesh_scheduler_enqueue_ex(&scheduler, &low).status ==
                     BLE_MESH_SCHED_ENQUEUE_OK &&
                     ble_mesh_scheduler_enqueue_ex(&scheduler, &high).status ==
                     BLE_MESH_SCHED_ENQUEUE_OK &&
                     ble_mesh_scheduler_enqueue_ex(&scheduler, &anonymous).status ==
                     BLE_MESH_SCHED_ENQUEUE_OK);
    scheduler.routed_tx_queue.custody_bypass_count = 2u;
    scheduler.custody_bypass_count = 2u;
    test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, BLE_RADIO_ADV_CH37,
                         BLE_RADIO_OP_STATE_TIMEOUT, TEST_RADIO_FAILURE_CH38);
    CHECK("LINK-07", ble_mesh_scheduler_poll(&scheduler, 1u, &event) == 1 &&
                     event.type == BLE_MESH_SCHED_EVENT_TX_DONE &&
                     event.tx_token == selected.token &&
                     event.tx_requested_channel_mask == BLE_RADIO_ADV_CH_ALL &&
                     event.tx_completed_channel_mask == BLE_RADIO_ADV_CH37 &&
                     event.fault == BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT);
    CHECK("LINK-07", probe.terminal_calls == 1u &&
                     probe.global_calls == 1u && probe.order_count == 2u &&
                     probe.order[0] == RESET_ORDER_TERMINAL &&
                     probe.order[1] == RESET_ORDER_GLOBAL);
    CHECK("LINK-07", ble_mesh_scheduler_poll(&scheduler, 2u, &event) == 1 &&
                     event.type == BLE_MESH_SCHED_EVENT_RADIO_FAULT &&
                     event.tx_token == selected.token &&
                     event.tx_requested_channel_mask == BLE_RADIO_ADV_CH_ALL &&
                     event.tx_completed_channel_mask == BLE_RADIO_ADV_CH37 &&
                     event.fault == BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT &&
                     probe.global_calls == 1u);
    assert_fault_idle_and_no_replay(&scheduler, &probe, 3u);
}

static void test_zero_completion_failure_precedes_one_global_reset(void)
{
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    reset_probe_t probe;
    ble_mesh_tx_item_t selected = make_item(
        0xb1u, BLE_MESH_TX_PRIORITY_HACK, BLE_MESH_TX_SERVICE_BEST_EFFORT,
        BLE_MESH_TX_OWNER_MAINTENANCE, BLE_MESH_TX_OWNER_DOMAIN_HIGH, 0x8201u, 0u);
    ble_mesh_tx_item_t remaining = make_item(
        0xb2u, BLE_MESH_TX_PRIORITY_CONTROL, BLE_MESH_TX_SERVICE_BEST_EFFORT,
        BLE_MESH_TX_OWNER_MENTORSHIP, BLE_MESH_TX_OWNER_DOMAIN_HIGH, 0x8202u, 0u);

    if (!prepare_scheduler(&scheduler) ||
        !configure_global_reset_probe(&scheduler, &probe)) {
        CHECK("LINK-07", 0);
        return;
    }
    probe.selected_token = selected.token;
    probe.selected_marker = selected.adv_data[0];
    probe.terminal_reason = BLE_MESH_TX_TERMINAL_TX_FAILED;
    probe.terminal_requested_mask = BLE_RADIO_ADV_CH_ALL;
    probe.terminal_completed_mask = 0u;
    probe.terminal_fault = BLE_RADIO_OP_STATE_TIMEOUT;
    probe.reset_fault = BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT;
    probe.reset_selected_valid = 1u;
    probe.reset_requested_mask = BLE_RADIO_ADV_CH_ALL;
    probe.reset_completed_mask = 0u;
    expect_live(&probe, remaining.token, remaining.adv_data[0]);

    CHECK("LINK-07", ble_mesh_scheduler_enqueue_ex(&scheduler, &selected).status ==
                     BLE_MESH_SCHED_ENQUEUE_OK &&
                     ble_mesh_scheduler_enqueue_ex(&scheduler, &remaining).status ==
                     BLE_MESH_SCHED_ENQUEUE_OK);
    test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, 0u, BLE_RADIO_OP_STATE_TIMEOUT,
                         TEST_RADIO_FAILURE_CH37);
    CHECK("LINK-07", ble_mesh_scheduler_poll(&scheduler, 1u, &event) == 1 &&
                     event.type == BLE_MESH_SCHED_EVENT_TX_FAILED &&
                     event.tx_token == selected.token &&
                     event.tx_requested_channel_mask == BLE_RADIO_ADV_CH_ALL &&
                     event.tx_completed_channel_mask == 0u &&
                     event.fault == BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT);
    CHECK("LINK-07", probe.terminal_calls == 1u &&
                     probe.global_calls == 1u && probe.order_count == 2u &&
                     probe.order[0] == RESET_ORDER_TERMINAL &&
                     probe.order[1] == RESET_ORDER_GLOBAL);
    CHECK("LINK-07", ble_mesh_scheduler_poll(&scheduler, 2u, &event) == 1 &&
                     event.type == BLE_MESH_SCHED_EVENT_RADIO_FAULT &&
                     event.tx_token == selected.token &&
                     event.tx_requested_channel_mask == BLE_RADIO_ADV_CH_ALL &&
                     event.tx_completed_channel_mask == 0u &&
                     event.fault == BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT &&
                     probe.global_calls == 1u);
    assert_fault_idle_and_no_replay(&scheduler, &probe, 3u);
}

static void test_no_selected_tx_uses_only_one_global_reset(void)
{
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    reset_probe_t probe;
    ble_mesh_tx_item_t tracked = make_item(
        0xc1u, BLE_MESH_TX_PRIORITY_CONTROL, BLE_MESH_TX_SERVICE_BEST_EFFORT,
        BLE_MESH_TX_OWNER_AODV, BLE_MESH_TX_OWNER_DOMAIN_LOW, 0x4301u, 100u);
    ble_mesh_tx_item_t anonymous = make_item(
        0xc2u, BLE_MESH_TX_PRIORITY_RELAY, BLE_MESH_TX_SERVICE_BEST_EFFORT,
        BLE_MESH_TX_OWNER_NONE, BLE_MESH_TX_OWNER_DOMAIN_NONE,
        BLE_MESH_TX_TOKEN_NONE, 100u);

    if (!prepare_scheduler(&scheduler) ||
        !configure_global_reset_probe(&scheduler, &probe)) {
        CHECK("LINK-07", 0);
        return;
    }
    probe.selected_token = BLE_MESH_TX_TOKEN_NONE;
    probe.reset_fault = BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT;
    probe.reset_selected_valid = 0u;
    probe.reset_requested_mask = 0u;
    probe.reset_completed_mask = 0u;
    expect_live(&probe, tracked.token, tracked.adv_data[0]);
    expect_live(&probe, BLE_MESH_TX_TOKEN_NONE, anonymous.adv_data[0]);

    CHECK("LINK-07", ble_mesh_scheduler_enqueue_ex(&scheduler, &tracked).status ==
                     BLE_MESH_SCHED_ENQUEUE_OK &&
                     ble_mesh_scheduler_enqueue_ex(&scheduler, &anonymous).status ==
                     BLE_MESH_SCHED_ENQUEUE_OK);
    scheduler.routed_tx_queue.custody_bypass_count = 2u;
    scheduler.custody_bypass_count = 2u;
    test_radio_script_tx(0u, 0u, BLE_RADIO_OP_STATE_TIMEOUT,
                         TEST_RADIO_FAILURE_RESTORE);
    CHECK("LINK-07", ble_mesh_scheduler_poll(&scheduler, 1u, &event) == 1 &&
                     event.type == BLE_MESH_SCHED_EVENT_RADIO_FAULT &&
                     event.tx_token == BLE_MESH_TX_TOKEN_NONE &&
                     event.tx_requested_channel_mask == 0u &&
                     event.tx_completed_channel_mask == 0u &&
                     event.fault == BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT);
    CHECK("LINK-07", probe.terminal_calls == 0u && probe.global_calls == 1u &&
                     probe.order_count == 1u && probe.order[0] == RESET_ORDER_GLOBAL);
    assert_fault_idle_and_no_replay(&scheduler, &probe, 2u);
}

static void test_missing_global_handler_rejects_tracked_admission(void)
{
    ble_mesh_scheduler_t scheduler;
    reset_probe_t probe;
    ble_mesh_tx_item_t tracked = make_item(
        0xd1u, BLE_MESH_TX_PRIORITY_CONTROL, BLE_MESH_TX_SERVICE_BEST_EFFORT,
        BLE_MESH_TX_OWNER_AODV, BLE_MESH_TX_OWNER_DOMAIN_LOW, 0x4401u, 0u);

    if (!prepare_scheduler(&scheduler)) {
        CHECK("LINK-07", 0);
        return;
    }
    memset(&probe, 0, sizeof(probe));
    probe.scheduler = &scheduler;
    CHECK("LINK-07", ble_mesh_scheduler_set_global_reset_handler(
                         &scheduler, NULL, &probe) == 0);
    CHECK("LINK-07", ble_mesh_scheduler_enqueue_ex(&scheduler, &tracked).status ==
                     BLE_MESH_SCHED_ENQUEUE_REJECTED &&
                     ble_mesh_tx_queue_count(&scheduler.routed_tx_queue) == 0u);
    CHECK("LINK-07", ble_mesh_scheduler_set_global_reset_handler(
                         &scheduler, record_global_reset, &probe) == 1 &&
                     ble_mesh_scheduler_enqueue_ex(&scheduler, &tracked).status ==
                     BLE_MESH_SCHED_ENQUEUE_OK &&
                     queue_has_token(&scheduler.routed_tx_queue, tracked.token));
}

int main(void)
{
    test_partial_selected_terminal_precedes_one_global_reset();
    test_zero_completion_failure_precedes_one_global_reset();
    test_no_selected_tx_uses_only_one_global_reset();
    test_missing_global_handler_rejects_tracked_admission();

    if (failures != 0u) {
        printf("tavrn_future_bearer global reset RED tests failed: %u assertion(s)\n",
               failures);
        return 1;
    }
    printf("tavrn_future_bearer global reset tests passed\n");
    return 0;
}

#endif /* BLE_MESH_SCHED_GLOBAL_RESET_API */
