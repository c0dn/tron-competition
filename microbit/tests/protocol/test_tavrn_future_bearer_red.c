#include "ble_mesh_tx_queue.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * This is an intentionally staged production-queue contract test.  The
 * version gate keeps the current queue build from producing a scatter of
 * undeclared-field diagnostics.  Once the public queue API below is declared,
 * this same source runs against ble_mesh_tx_queue.c and exposes behavior.
 *
 * Required future API:
 *   BLE_MESH_TX_QUEUE_FUTURE_BEARER_API == 1
 *   item metadata: expiry_ms, sweep_count, budget_class; the existing token
 *                  remains the sole tracked/anonymous discriminator
 *   expiry scan: ble_mesh_tx_queue_collect_due_expiry(queue, now,
 *                                                      anonymous_limit,
 *                                                      result)
 *
 * The scan may purge bounded anonymous work, but it identifies at most one
 * token-bearing item for the scheduler's existing event path.
 */
#if !defined(BLE_MESH_TX_QUEUE_FUTURE_BEARER_API)
#error "FUTURE_BEARER_RED_GATE: BLE_MESH_TX_QUEUE_FUTURE_BEARER_API=1 is required"
#elif BLE_MESH_TX_QUEUE_FUTURE_BEARER_API != 1
#error "FUTURE_BEARER_RED_GATE: BLE_MESH_TX_QUEUE_FUTURE_BEARER_API must equal 1"
#endif

#if defined(BLE_MESH_TX_QUEUE_FUTURE_BEARER_API) && \
    BLE_MESH_TX_QUEUE_FUTURE_BEARER_API == 1

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

static int remove_tracked_expiry(ble_mesh_tx_queue_t *queue, uint8_t index)
{
    return ble_mesh_tx_queue_retire(queue, index, BLE_MESH_TX_TERMINAL_EXPIRED,
                                    NULL) == BLE_MESH_TX_RETIRE_OK;
}

static ble_mesh_tx_item_t make_item(uint8_t marker,
                                    ble_mesh_tx_priority_t priority,
                                    uint32_t expiry_ms,
                                    ble_mesh_tx_token_t token)
{
    ble_mesh_tx_item_t item;

    memset(&item, 0, sizeof(item));
    item.adv_len = 1u;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = priority;
    item.not_before_ms = 0u;
    item.expiry_ms = expiry_ms;
    item.sweep_count = BLE_MESH_TX_SWEEP_COUNT_ONE;
    item.budget_class = BLE_MESH_TX_BUDGET_GENERAL;
    item.token = token;
    item.adv_data[0] = marker;
    return item;
}

static void test_enqueue_requires_complete_routed_metadata(void)
{
    ble_mesh_tx_queue_t queue;
    ble_mesh_tx_enqueue_result_t result;
    ble_mesh_tx_item_t item;

    ble_mesh_tx_queue_init(&queue);

    memset(&item, 0, sizeof(item));
    item.adv_len = 1u;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.adv_data[0] = 0x10u;
    result = ble_mesh_tx_queue_enqueue(&queue, &item);
    CHECK("BEARER-06", result.status == BLE_MESH_TX_ENQUEUE_INVALID);

    item = make_item(0x11u, BLE_MESH_TX_PRIORITY_CONTROL, 0u,
                     BLE_MESH_TX_TOKEN_NONE);
    item.sweep_count = 3u;
    result = ble_mesh_tx_queue_enqueue(&queue, &item);
    CHECK("BEARER-05", result.status == BLE_MESH_TX_ENQUEUE_INVALID);

    item = make_item(0x12u, BLE_MESH_TX_PRIORITY_CONTROL, 0u,
                     BLE_MESH_TX_TOKEN_NONE);
    item.budget_class = (ble_mesh_tx_budget_class_t)2u;
    result = ble_mesh_tx_queue_enqueue(&queue, &item);
    CHECK("BEARER-05", result.status == BLE_MESH_TX_ENQUEUE_INVALID);

    item = make_item(0x13u, BLE_MESH_TX_PRIORITY_CONTROL, 0u,
                     BLE_MESH_TX_TOKEN_NONE);
    item.service_class = BLE_MESH_TX_SERVICE_CUSTODY_DATA;
    item.sweep_count = BLE_MESH_TX_SWEEP_COUNT_TWO;
    result = ble_mesh_tx_queue_enqueue(&queue, &item);
    CHECK("LINK-07", result.status == BLE_MESH_TX_ENQUEUE_INVALID);

    item = make_item(0x14u, BLE_MESH_TX_PRIORITY_CONTROL, 0u,
                     BLE_MESH_TX_TOKEN_NONE);
    result = ble_mesh_tx_queue_enqueue(&queue, &item);
    CHECK("BEARER-06", result.status == BLE_MESH_TX_ENQUEUE_OK);

    item = make_item(0x15u, BLE_MESH_TX_PRIORITY_CONTROL, 1u, 0x1234u);
    item.sweep_count = BLE_MESH_TX_SWEEP_COUNT_TWO;
    item.budget_class = BLE_MESH_TX_BUDGET_CRITICAL;
    result = ble_mesh_tx_queue_enqueue(&queue, &item);
    CHECK("LINK-07", result.status == BLE_MESH_TX_ENQUEUE_OK &&
                     result.accepted_token == 0x1234u);
}

static void test_priority_banded_edf_and_fifo_ties(void)
{
    ble_mesh_tx_queue_t queue;
    ble_mesh_tx_selection_t selection;
    ble_mesh_tx_item_t item;

    ble_mesh_tx_queue_init(&queue);
    item = make_item(0x21u, BLE_MESH_TX_PRIORITY_DATA, 100u,
                     BLE_MESH_TX_TOKEN_NONE);
    CHECK("BEARER-05", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                       BLE_MESH_TX_ENQUEUE_OK);
    item = make_item(0x22u, BLE_MESH_TX_PRIORITY_CONTROL, 300u,
                     BLE_MESH_TX_TOKEN_NONE);
    CHECK("BEARER-05", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                       BLE_MESH_TX_ENQUEUE_OK);
    CHECK("BEARER-05", ble_mesh_tx_queue_select_due(&queue, 1u,
                                                      BLE_MESH_TX_TOKEN_NONE,
                                                      &selection) ==
                       BLE_MESH_TX_SELECT_OK &&
                       selection.item.adv_data[0] == 0x22u);

    ble_mesh_tx_queue_init(&queue);
    item = make_item(0x23u, BLE_MESH_TX_PRIORITY_CONTROL, 900u,
                     BLE_MESH_TX_TOKEN_NONE);
    CHECK("BEARER-05", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                       BLE_MESH_TX_ENQUEUE_OK);
    item = make_item(0x24u, BLE_MESH_TX_PRIORITY_CONTROL, 100u,
                     BLE_MESH_TX_TOKEN_NONE);
    CHECK("BEARER-05", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                       BLE_MESH_TX_ENQUEUE_OK);
    CHECK("BEARER-05", ble_mesh_tx_queue_select_due(&queue, 1u,
                                                      BLE_MESH_TX_TOKEN_NONE,
                                                      &selection) ==
                       BLE_MESH_TX_SELECT_OK &&
                       selection.item.adv_data[0] == 0x24u);

    ble_mesh_tx_queue_init(&queue);
    item = make_item(0x25u, BLE_MESH_TX_PRIORITY_CONTROL, 200u,
                     BLE_MESH_TX_TOKEN_NONE);
    CHECK("BEARER-05", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                       BLE_MESH_TX_ENQUEUE_OK);
    item = make_item(0x26u, BLE_MESH_TX_PRIORITY_CONTROL, 200u,
                     BLE_MESH_TX_TOKEN_NONE);
    CHECK("BEARER-05", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                       BLE_MESH_TX_ENQUEUE_OK);
    CHECK("BEARER-05", ble_mesh_tx_queue_select_due(&queue, 1u,
                                                      BLE_MESH_TX_TOKEN_NONE,
                                                      &selection) ==
                        BLE_MESH_TX_SELECT_OK &&
                        selection.item.adv_data[0] == 0x25u);
    CHECK("BEARER-05", ble_mesh_tx_queue_remove(&queue, selection.index, NULL) == 1);
    CHECK("BEARER-05", ble_mesh_tx_queue_select_due(&queue, 1u,
                                                      BLE_MESH_TX_TOKEN_NONE,
                                                      &selection) ==
                        BLE_MESH_TX_SELECT_OK &&
                        selection.item.adv_data[0] == 0x26u);

    ble_mesh_tx_queue_init(&queue);
    item = make_item(0x27u, BLE_MESH_TX_PRIORITY_CONTROL,
                     UINT32_C(0xfffffffc), BLE_MESH_TX_TOKEN_NONE);
    item.not_before_ms = UINT32_C(0xfffffff8);
    CHECK("BEARER-05", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                       BLE_MESH_TX_ENQUEUE_OK);
    item = make_item(0x28u, BLE_MESH_TX_PRIORITY_CONTROL, 0x00000004u,
                     BLE_MESH_TX_TOKEN_NONE);
    item.not_before_ms = UINT32_C(0xfffffff8);
    CHECK("BEARER-05", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                       BLE_MESH_TX_ENQUEUE_OK);
    CHECK("BEARER-05", ble_mesh_tx_queue_select_due(&queue, UINT32_C(0xfffffff8),
                                                      BLE_MESH_TX_TOKEN_NONE,
                                                      &selection) ==
                       BLE_MESH_TX_SELECT_OK &&
                       selection.item.adv_data[0] == 0x27u);
}

static void test_expiry_sweep_precedes_selection_and_bounds_removal(void)
{
    ble_mesh_tx_queue_t queue;
    ble_mesh_tx_expiry_result_t expiry;
    ble_mesh_tx_selection_t selection;
    ble_mesh_tx_item_t item;

    ble_mesh_tx_queue_init(&queue);
    item = make_item(0x31u, BLE_MESH_TX_PRIORITY_CONTROL, UINT32_C(0xfffffffd),
                     0x3101u);
    CHECK("LINK-07", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                     BLE_MESH_TX_ENQUEUE_OK);
    item = make_item(0x32u, BLE_MESH_TX_PRIORITY_HACK, UINT32_C(0xfffffffe),
                     BLE_MESH_TX_TOKEN_NONE);
    CHECK("BEARER-06", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                       BLE_MESH_TX_ENQUEUE_OK);
    item = make_item(0x33u, BLE_MESH_TX_PRIORITY_CONTROL, UINT32_C(0xffffffff),
                     0x3102u);
    CHECK("LINK-07", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                     BLE_MESH_TX_ENQUEUE_OK);
    item = make_item(0x35u, BLE_MESH_TX_PRIORITY_RELAY, 0x00000004u,
                     BLE_MESH_TX_TOKEN_NONE);
    CHECK("BEARER-06", ble_mesh_tx_queue_enqueue(&queue, &item).status ==
                       BLE_MESH_TX_ENQUEUE_OK);

    memset(&expiry, 0, sizeof(expiry));
    CHECK("LINK-07", ble_mesh_tx_queue_collect_due_expiry(
                          &queue, 0u, 1u, &expiry) ==
                      BLE_MESH_TX_EXPIRY_OK &&
                      expiry.tracked_due == 1u &&
                      expiry.anonymous_purged_count == 1u &&
                      (expiry.tracked_item.token == 0x3101u ||
                       expiry.tracked_item.token == 0x3102u));
    CHECK("BEARER-05", ble_mesh_tx_queue_select_due(&queue, 0u,
                                                      BLE_MESH_TX_TOKEN_NONE,
                                                      &selection) ==
                        BLE_MESH_TX_SELECT_OK &&
                        selection.item.adv_data[0] == 0x35u);
    CHECK("LINK-07", remove_tracked_expiry(&queue, expiry.tracked_index) == 1);

    memset(&expiry, 0, sizeof(expiry));
    CHECK("LINK-07", ble_mesh_tx_queue_collect_due_expiry(
                          &queue, 0u, 1u, &expiry) ==
                      BLE_MESH_TX_EXPIRY_OK &&
                      expiry.tracked_due == 1u &&
                      expiry.anonymous_purged_count == 0u &&
                      ble_mesh_tx_queue_count(&queue) == 2u);
    CHECK("LINK-07", remove_tracked_expiry(&queue, expiry.tracked_index) == 1 &&
                      ble_mesh_tx_queue_count(&queue) == 1u);
}

int main(void)
{
    test_enqueue_requires_complete_routed_metadata();
    test_priority_banded_edf_and_fifo_ties();
    test_expiry_sweep_precedes_selection_and_bounds_removal();

    if (failures != 0u) {
        printf("tavrn_future_bearer RED tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_future_bearer tests passed\n");
    return 0;
}

#endif /* BLE_MESH_TX_QUEUE_FUTURE_BEARER_API */
