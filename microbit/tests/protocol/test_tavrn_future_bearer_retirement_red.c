#include "ble_mesh_tx_queue.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * This staged queue-only contract keeps typed retirement separate from the
 * already-green metadata/EDF future-bearer gate.
 *
 * Required next API:
 *   BLE_MESH_TX_QUEUE_TYPED_RETIREMENT_API == 1
 *   ble_mesh_tx_terminal_reason_t:
 *       TX_DONE, TX_FAILED, EXPIRED, CANCELED, EVICTED, PEER_RESET,
 *       IDENTITY_RESET (global reset is deliberately a later slice)
 *   ble_mesh_tx_terminal_t:
 *       reason, owner_kind, owner_domain, token, requested_channel_mask,
 *       completed_channel_mask, fault
 *   ble_mesh_tx_queue_set_terminal_handler(queue, handler, context)
 *   ble_mesh_tx_queue_retire(queue, index, reason, tx_result)
 *   ble_mesh_tx_queue_cancel(queue, token)
 *
 * retire() derives owner identity from the live item.  Its tx_result is
 * required only for TX_DONE/TX_FAILED and must describe that item's requested
 * channels; non-physical retirements pass NULL.  This keeps the existing
 * enqueue result token authoritative and avoids a second token field in any
 * input record.
 */
#if !defined(BLE_MESH_TX_QUEUE_TYPED_RETIREMENT_API)
#error "TYPED_RETIREMENT_RED_GATE: BLE_MESH_TX_QUEUE_TYPED_RETIREMENT_API=1 is required"
#elif BLE_MESH_TX_QUEUE_TYPED_RETIREMENT_API != 1
#error "TYPED_RETIREMENT_RED_GATE: BLE_MESH_TX_QUEUE_TYPED_RETIREMENT_API must equal 1"
#endif

#if defined(BLE_MESH_TX_QUEUE_TYPED_RETIREMENT_API) && \
    BLE_MESH_TX_QUEUE_TYPED_RETIREMENT_API == 1

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

typedef struct terminal_probe {
    ble_mesh_tx_queue_t *queue;
    ble_mesh_tx_token_t token;
    uint8_t index;
    uint8_t marker;
    ble_mesh_tx_terminal_reason_t reason;
    ble_mesh_tx_owner_kind_t owner_kind;
    ble_mesh_tx_owner_domain_t owner_domain;
    uint8_t requested_channel_mask;
    uint8_t completed_channel_mask;
    ble_radio_op_result_t fault;
    unsigned int calls;
    unsigned int live_calls;
    int reject;
} terminal_probe_t;

static ble_mesh_tx_item_t make_tracked_item(
    uint8_t marker, ble_mesh_tx_priority_t priority,
    ble_mesh_tx_owner_kind_t owner_kind,
    ble_mesh_tx_owner_domain_t owner_domain, ble_mesh_tx_token_t token)
{
    ble_mesh_tx_item_t item;

    memset(&item, 0, sizeof(item));
    item.adv_len = 1u;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = priority;
    item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
    item.not_before_ms = 0u;
    item.expiry_ms = 10000u;
    item.sweep_count = BLE_MESH_TX_SWEEP_COUNT_ONE;
    item.budget_class = BLE_MESH_TX_BUDGET_GENERAL;
    item.owner_kind = owner_kind;
    item.owner_domain = owner_domain;
    item.token = token;
    item.adv_data[0] = marker;
    return item;
}

static ble_mesh_tx_item_t make_anonymous_item(uint8_t marker,
                                               ble_mesh_tx_priority_t priority)
{
    return make_tracked_item(marker, priority, BLE_MESH_TX_OWNER_NONE,
                             BLE_MESH_TX_OWNER_DOMAIN_NONE,
                             BLE_MESH_TX_TOKEN_NONE);
}

static int find_token_index(const ble_mesh_tx_queue_t *queue,
                            ble_mesh_tx_token_t token, uint8_t *index_out)
{
    uint8_t index;

    if (queue == NULL || token == BLE_MESH_TX_TOKEN_NONE || index_out == NULL) {
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

static void configure_probe(terminal_probe_t *probe, ble_mesh_tx_queue_t *queue,
                             const ble_mesh_tx_item_t *item,
                             ble_mesh_tx_terminal_reason_t reason,
                            uint8_t completed_channel_mask,
                            ble_radio_op_result_t fault)
{
    memset(probe, 0, sizeof(*probe));
    probe->queue = queue;
    probe->token = item->token;
    probe->marker = item->adv_data[0];
    probe->reason = reason;
    probe->owner_kind = item->owner_kind;
    probe->owner_domain = item->owner_domain;
    probe->requested_channel_mask = item->channel_mask;
    probe->completed_channel_mask = completed_channel_mask;
    probe->fault = fault;
    CHECK("LINK-07", find_token_index(queue, item->token, &probe->index) == 1);
}

static void make_physical_tx_result(ble_radio_tx_result_t *result,
                                    const ble_mesh_tx_item_t *item,
                                    uint8_t completed_channel_mask,
                                    ble_radio_op_result_t fault)
{
    uint8_t sweep_index;

    memset(result, 0, sizeof(*result));
    result->requested_channel_mask = item->channel_mask;
    result->completed_channel_mask = completed_channel_mask;
    result->requested_sweep_count = (uint8_t)item->sweep_count;
    result->attempted_sweep_count = completed_channel_mask == item->channel_mask &&
        fault == BLE_RADIO_OP_OK ? result->requested_sweep_count :
        BLE_MESH_TX_SWEEP_COUNT_ONE;
    for (sweep_index = 0u; sweep_index < result->attempted_sweep_count;
         sweep_index++) {
        result->completed_channel_masks[sweep_index] = completed_channel_mask;
    }
    result->fault = fault;
}

static int terminal_is_live(const terminal_probe_t *probe)
{
    const ble_mesh_tx_queue_entry_t *entry;

    if (probe == NULL || probe->queue == NULL ||
        probe->index >= BLE_MESH_TX_QUEUE_CAPACITY) {
        return 0;
    }
    entry = &probe->queue->entries[probe->index];
    return entry->occupied != 0u && entry->item.token == probe->token &&
        entry->item.owner_kind == probe->owner_kind &&
        entry->item.owner_domain == probe->owner_domain &&
        entry->item.channel_mask == probe->requested_channel_mask &&
        entry->item.adv_data[0] == probe->marker;
}

static int inspect_terminal(void *context,
                            const ble_mesh_tx_terminal_t *terminal)
{
    terminal_probe_t *probe = context;

    if (probe == NULL || terminal == NULL) {
        CHECK("LINK-07", 0);
        return 0;
    }

    probe->calls++;
    if (terminal_is_live(probe)) {
        probe->live_calls++;
    }
    CHECK("LINK-07", terminal_is_live(probe));
    CHECK("LINK-07", terminal->reason == probe->reason);
    CHECK("LINK-07", terminal->owner_kind == probe->owner_kind &&
                     terminal->owner_domain == probe->owner_domain &&
                     terminal->token == probe->token);
    CHECK("BEARER-06", terminal->requested_channel_mask ==
                         probe->requested_channel_mask &&
                         terminal->completed_channel_mask ==
                         probe->completed_channel_mask &&
                         terminal->fault == probe->fault);
    return probe->reject == 0;
}

static void test_physical_terminal_validation_and_order(void)
{
    ble_mesh_tx_queue_t queue;
    ble_mesh_tx_item_t item = make_tracked_item(
        0x11u, BLE_MESH_TX_PRIORITY_CONTROL, BLE_MESH_TX_OWNER_REPAIR,
        BLE_MESH_TX_OWNER_DOMAIN_HIGH, 0x8101u);
    ble_radio_tx_result_t tx_result;
    terminal_probe_t probe;
    uint8_t index;

    ble_mesh_tx_queue_init(&queue);
    memset(&probe, 0, sizeof(probe));
    CHECK("LINK-07", ble_mesh_tx_queue_set_terminal_handler(
                         &queue, inspect_terminal, &probe) == 1);
    CHECK("LINK-07", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                     BLE_MESH_TX_ENQUEUE_OK);
    CHECK("LINK-07", find_token_index(&queue, item.token, &index) == 1);

    make_physical_tx_result(&tx_result, &item, 0u, BLE_RADIO_OP_STATE_TIMEOUT);
    CHECK("LINK-07", ble_mesh_tx_queue_retire(
                         &queue, index, BLE_MESH_TX_TERMINAL_TX_DONE,
                         &tx_result) == BLE_MESH_TX_RETIRE_INVALID &&
                     ble_mesh_tx_queue_count(&queue) == 1u &&
                     probe.calls == 0u);

    tx_result.completed_channel_mask = BLE_RADIO_ADV_CH37;
    tx_result.requested_channel_mask = BLE_RADIO_ADV_CH38;
    CHECK("LINK-07", ble_mesh_tx_queue_retire(
                         &queue, index, BLE_MESH_TX_TERMINAL_TX_DONE,
                         &tx_result) == BLE_MESH_TX_RETIRE_INVALID &&
                     ble_mesh_tx_queue_count(&queue) == 1u &&
                     probe.calls == 0u);

    tx_result.requested_channel_mask = item.channel_mask;
    tx_result.completed_channel_masks[0] = BLE_RADIO_ADV_CH37;
    configure_probe(&probe, &queue, &item, BLE_MESH_TX_TERMINAL_TX_DONE,
                    BLE_RADIO_ADV_CH37, BLE_RADIO_OP_STATE_TIMEOUT);
    CHECK("LINK-07", ble_mesh_tx_queue_retire(
                         &queue, index, BLE_MESH_TX_TERMINAL_TX_DONE,
                         &tx_result) == BLE_MESH_TX_RETIRE_OK &&
                     probe.calls == 1u && probe.live_calls == 1u &&
                     ble_mesh_tx_queue_count(&queue) == 0u);

    ble_mesh_tx_queue_init(&queue);
    memset(&probe, 0, sizeof(probe));
    CHECK("LINK-07", ble_mesh_tx_queue_set_terminal_handler(
                         &queue, inspect_terminal, &probe) == 1 &&
                     ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                         BLE_MESH_TX_ENQUEUE_OK &&
                     find_token_index(&queue, item.token, &index) == 1);
    make_physical_tx_result(&tx_result, &item, BLE_RADIO_ADV_CH37,
                            BLE_RADIO_OP_STATE_TIMEOUT);
    CHECK("LINK-07", ble_mesh_tx_queue_retire(
                         &queue, index, BLE_MESH_TX_TERMINAL_TX_FAILED,
                         &tx_result) == BLE_MESH_TX_RETIRE_INVALID &&
                     ble_mesh_tx_queue_count(&queue) == 1u &&
                     probe.calls == 0u);
    make_physical_tx_result(&tx_result, &item, 0u, BLE_RADIO_OP_STATE_TIMEOUT);
    configure_probe(&probe, &queue, &item, BLE_MESH_TX_TERMINAL_TX_FAILED,
                    0u, BLE_RADIO_OP_STATE_TIMEOUT);
    CHECK("LINK-07", ble_mesh_tx_queue_retire(
                         &queue, index, BLE_MESH_TX_TERMINAL_TX_FAILED,
                         &tx_result) == BLE_MESH_TX_RETIRE_OK &&
                     probe.calls == 1u && probe.live_calls == 1u &&
                     ble_mesh_tx_queue_count(&queue) == 0u);
}

static void test_nonphysical_terminals_carry_no_completion_evidence(void)
{
    static const ble_mesh_tx_terminal_reason_t reasons[] = {
        BLE_MESH_TX_TERMINAL_EXPIRED,
        BLE_MESH_TX_TERMINAL_PEER_RESET,
        BLE_MESH_TX_TERMINAL_IDENTITY_RESET,
    };
    ble_mesh_tx_queue_t queue;
    ble_mesh_tx_item_t item;
    ble_radio_tx_result_t fake_tx;
    terminal_probe_t probe;
    uint8_t index;
    uint8_t reason_index;

    for (reason_index = 0u; reason_index <
             (uint8_t)(sizeof(reasons) / sizeof(reasons[0])); reason_index++) {
        item = make_tracked_item((uint8_t)(0x20u + reason_index),
                                 BLE_MESH_TX_PRIORITY_CONTROL,
                                 BLE_MESH_TX_OWNER_MAINTENANCE,
                                 BLE_MESH_TX_OWNER_DOMAIN_HIGH,
                                 (ble_mesh_tx_token_t)(0x8201u + reason_index));
        ble_mesh_tx_queue_init(&queue);
        memset(&probe, 0, sizeof(probe));
        CHECK("LINK-07", ble_mesh_tx_queue_set_terminal_handler(
                             &queue, inspect_terminal, &probe) == 1 &&
                         ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                             BLE_MESH_TX_ENQUEUE_OK &&
                         find_token_index(&queue, item.token, &index) == 1);

        memset(&fake_tx, 0, sizeof(fake_tx));
        fake_tx.requested_channel_mask = item.channel_mask;
        fake_tx.completed_channel_mask = BLE_RADIO_ADV_CH37;
        fake_tx.fault = BLE_RADIO_OP_STATE_TIMEOUT;
        CHECK("LINK-07", ble_mesh_tx_queue_retire(&queue, index,
                                                    reasons[reason_index],
                                                    &fake_tx) ==
                             BLE_MESH_TX_RETIRE_INVALID &&
                         ble_mesh_tx_queue_count(&queue) == 1u &&
                         probe.calls == 0u);

        configure_probe(&probe, &queue, &item, reasons[reason_index], 0u,
                        BLE_RADIO_OP_OK);
        CHECK("LINK-07", ble_mesh_tx_queue_retire(&queue, index,
                                                    reasons[reason_index], NULL) ==
                             BLE_MESH_TX_RETIRE_OK &&
                         probe.calls == 1u && probe.live_calls == 1u &&
                         ble_mesh_tx_queue_count(&queue) == 0u);
    }
}

static void test_anonymous_removals_do_not_invoke_owner_handler(void)
{
    ble_mesh_tx_queue_t queue;
    ble_mesh_tx_item_t item;
    ble_radio_tx_result_t tx_result;
    terminal_probe_t probe;
    uint8_t index;

    ble_mesh_tx_queue_init(&queue);
    memset(&probe, 0, sizeof(probe));
    CHECK("BEARER-06", ble_mesh_tx_queue_set_terminal_handler(
                          &queue, inspect_terminal, &probe) == 1);

    item = make_anonymous_item(0x31u, BLE_MESH_TX_PRIORITY_CONTROL);
    CHECK("BEARER-06", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                          BLE_MESH_TX_ENQUEUE_OK);
    CHECK("BEARER-06", find_token_index(&queue, item.token, &index) == 0);
    index = 0u;
    while (index < BLE_MESH_TX_QUEUE_CAPACITY &&
           queue.entries[index].occupied == 0u) {
        index++;
    }
    make_physical_tx_result(&tx_result, &item, BLE_RADIO_ADV_CH37,
                            BLE_RADIO_OP_STATE_TIMEOUT);
    CHECK("BEARER-06", index < BLE_MESH_TX_QUEUE_CAPACITY &&
                          ble_mesh_tx_queue_retire(
                              &queue, index, BLE_MESH_TX_TERMINAL_TX_DONE,
                              &tx_result) == BLE_MESH_TX_RETIRE_OK &&
                          probe.calls == 0u &&
                          ble_mesh_tx_queue_count(&queue) == 0u);

    item = make_anonymous_item(0x32u, BLE_MESH_TX_PRIORITY_RELAY);
    CHECK("BEARER-06", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                          BLE_MESH_TX_ENQUEUE_OK);
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (queue.entries[index].occupied != 0u) {
            break;
        }
    }
    CHECK("BEARER-06", index < BLE_MESH_TX_QUEUE_CAPACITY &&
                          ble_mesh_tx_queue_retire(
                              &queue, index, BLE_MESH_TX_TERMINAL_EXPIRED,
                              NULL) == BLE_MESH_TX_RETIRE_OK &&
                          probe.calls == 0u &&
                          ble_mesh_tx_queue_count(&queue) == 0u);

    item = make_anonymous_item(0x33u, BLE_MESH_TX_PRIORITY_RELAY);
    CHECK("BEARER-06", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                          BLE_MESH_TX_ENQUEUE_OK);
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (queue.entries[index].occupied != 0u) {
            break;
        }
    }
    CHECK("BEARER-06", index < BLE_MESH_TX_QUEUE_CAPACITY &&
                          ble_mesh_tx_queue_remove(&queue, index, NULL) == 1 &&
                          probe.calls == 0u &&
                          ble_mesh_tx_queue_count(&queue) == 0u);
}

static void test_handler_rejection_preserves_tracked_storage(void)
{
    ble_mesh_tx_queue_t queue;
    ble_mesh_tx_item_t item = make_tracked_item(
        0x41u, BLE_MESH_TX_PRIORITY_CONTROL, BLE_MESH_TX_OWNER_AODV,
        BLE_MESH_TX_OWNER_DOMAIN_LOW, 0x4101u);
    ble_radio_tx_result_t tx_result;
    terminal_probe_t probe;
    uint8_t index;

    ble_mesh_tx_queue_init(&queue);
    CHECK("LINK-07", ble_mesh_tx_queue_set_terminal_handler(
                         &queue, inspect_terminal, &probe) == 1 &&
                     ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                         BLE_MESH_TX_ENQUEUE_OK &&
                     find_token_index(&queue, item.token, &index) == 1);
    make_physical_tx_result(&tx_result, &item, 0u, BLE_RADIO_OP_STATE_TIMEOUT);
    configure_probe(&probe, &queue, &item, BLE_MESH_TX_TERMINAL_TX_FAILED,
                    0u, BLE_RADIO_OP_STATE_TIMEOUT);
    probe.reject = 1;
    CHECK("LINK-07", ble_mesh_tx_queue_retire(
                         &queue, index, BLE_MESH_TX_TERMINAL_TX_FAILED,
                         &tx_result) == BLE_MESH_TX_RETIRE_REJECTED &&
                     probe.calls == 1u && probe.live_calls == 1u &&
                     terminal_is_live(&probe) && ble_mesh_tx_queue_count(&queue) == 1u);
    probe.reject = 0;
    CHECK("LINK-07", ble_mesh_tx_queue_retire(
                         &queue, index, BLE_MESH_TX_TERMINAL_TX_FAILED,
                         &tx_result) == BLE_MESH_TX_RETIRE_OK &&
                     probe.calls == 2u && probe.live_calls == 2u &&
                     ble_mesh_tx_queue_count(&queue) == 0u);
}

static void test_tracked_retirement_requires_handler(void)
{
    ble_mesh_tx_queue_t queue;
    ble_mesh_tx_item_t item = make_tracked_item(
        0x42u, BLE_MESH_TX_PRIORITY_CONTROL, BLE_MESH_TX_OWNER_AODV,
        BLE_MESH_TX_OWNER_DOMAIN_LOW, 0x4201u);
    ble_radio_tx_result_t tx_result;
    uint8_t index;

    ble_mesh_tx_queue_init(&queue);
    CHECK("LINK-07", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                     BLE_MESH_TX_ENQUEUE_OK &&
                     find_token_index(&queue, item.token, &index) == 1);
    make_physical_tx_result(&tx_result, &item, 0u, BLE_RADIO_OP_STATE_TIMEOUT);
    CHECK("LINK-07", ble_mesh_tx_queue_retire(
                         &queue, index, BLE_MESH_TX_TERMINAL_TX_FAILED,
                         &tx_result) == BLE_MESH_TX_RETIRE_REJECTED &&
                     ble_mesh_tx_queue_count(&queue) == 1u &&
                     queue.entries[index].occupied != 0u &&
                     queue.entries[index].item.token == item.token);
}

static void test_eviction_is_terminal_and_transactional(void)
{
    ble_mesh_tx_queue_t queue;
    ble_mesh_tx_item_t evicted = make_tracked_item(
        0x51u, BLE_MESH_TX_PRIORITY_RELAY, BLE_MESH_TX_OWNER_REPAIR,
        BLE_MESH_TX_OWNER_DOMAIN_HIGH, 0x8501u);
    ble_mesh_tx_item_t anonymous = make_anonymous_item(
        0x52u, BLE_MESH_TX_PRIORITY_RELAY);
    ble_mesh_tx_item_t admitted = make_tracked_item(
        0x53u, BLE_MESH_TX_PRIORITY_HACK, BLE_MESH_TX_OWNER_LINK,
        BLE_MESH_TX_OWNER_DOMAIN_LOW, 0x5301u);
    ble_mesh_tx_enqueue_result_t result;
    terminal_probe_t probe;
    uint8_t index;
    uint8_t fill_index;

    ble_mesh_tx_queue_init(&queue);
    CHECK("LINK-07", ble_mesh_tx_queue_set_terminal_handler(
                         &queue, inspect_terminal, &probe) == 1 &&
                     ble_mesh_tx_queue_enqueue(&queue, &evicted).status ==
                         BLE_MESH_TX_ENQUEUE_OK);
    for (fill_index = 1u; fill_index < BLE_MESH_TX_QUEUE_CAPACITY; fill_index++) {
        CHECK("BEARER-06", ble_mesh_tx_queue_enqueue(&queue, &anonymous).status ==
                             BLE_MESH_TX_ENQUEUE_OK);
    }
    configure_probe(&probe, &queue, &evicted, BLE_MESH_TX_TERMINAL_EVICTED,
                    0u, BLE_RADIO_OP_OK);
    probe.reject = 1;
    result = ble_mesh_tx_queue_enqueue(&queue, &admitted);
    CHECK("LINK-07", result.status == BLE_MESH_TX_ENQUEUE_REJECTED &&
                     ble_mesh_tx_queue_count(&queue) == BLE_MESH_TX_QUEUE_CAPACITY &&
                     terminal_is_live(&probe) && probe.calls == 1u &&
                     probe.live_calls == 1u &&
                     find_token_index(&queue, admitted.token, &index) == 0);

    probe.reject = 0;
    result = ble_mesh_tx_queue_enqueue(&queue, &admitted);
    CHECK("LINK-07", result.status == BLE_MESH_TX_ENQUEUE_OK &&
                     result.accepted_token == admitted.token &&
                     result.evicted_token == evicted.token &&
                     ble_mesh_tx_queue_count(&queue) == BLE_MESH_TX_QUEUE_CAPACITY &&
                     probe.calls == 2u && probe.live_calls == 2u &&
                     find_token_index(&queue, evicted.token, &index) == 0 &&
                     find_token_index(&queue, admitted.token, &index) == 1);
}

static void test_cancel_is_exact_idempotent_and_raw_remove_refuses_tracked(void)
{
    ble_mesh_tx_queue_t queue;
    ble_mesh_tx_item_t item = make_tracked_item(
        0x61u, BLE_MESH_TX_PRIORITY_CONTROL, BLE_MESH_TX_OWNER_MENTORSHIP,
        BLE_MESH_TX_OWNER_DOMAIN_HIGH, 0x8601u);
    terminal_probe_t probe;
    uint8_t index;

    ble_mesh_tx_queue_init(&queue);
    CHECK("LINK-07", ble_mesh_tx_queue_set_terminal_handler(
                         &queue, inspect_terminal, &probe) == 1 &&
                     ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                         BLE_MESH_TX_ENQUEUE_OK &&
                     find_token_index(&queue, item.token, &index) == 1);
    configure_probe(&probe, &queue, &item, BLE_MESH_TX_TERMINAL_CANCELED,
                    0u, BLE_RADIO_OP_OK);
    CHECK("LINK-07", ble_mesh_tx_queue_remove(&queue, index, NULL) == 0 &&
                     terminal_is_live(&probe) && probe.calls == 0u &&
                     ble_mesh_tx_queue_count(&queue) == 1u);
    CHECK("LINK-07", ble_mesh_tx_queue_cancel(&queue, item.token) ==
                         BLE_MESH_TX_RETIRE_OK && probe.calls == 1u &&
                     probe.live_calls == 1u && ble_mesh_tx_queue_count(&queue) == 0u);
    CHECK("LINK-07", ble_mesh_tx_queue_cancel(&queue, item.token) ==
                         BLE_MESH_TX_RETIRE_OK && probe.calls == 1u &&
                     ble_mesh_tx_queue_count(&queue) == 0u);
}

int main(void)
{
    test_physical_terminal_validation_and_order();
    test_nonphysical_terminals_carry_no_completion_evidence();
    test_anonymous_removals_do_not_invoke_owner_handler();
    test_handler_rejection_preserves_tracked_storage();
    test_tracked_retirement_requires_handler();
    test_eviction_is_terminal_and_transactional();
    test_cancel_is_exact_idempotent_and_raw_remove_refuses_tracked();

    if (failures != 0u) {
        printf("tavrn_future_bearer retirement RED tests failed: %u assertion(s)\n",
               failures);
        return 1;
    }
    printf("tavrn_future_bearer retirement tests passed\n");
    return 0;
}

#endif /* BLE_MESH_TX_QUEUE_TYPED_RETIREMENT_API */
