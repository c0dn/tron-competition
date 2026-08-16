#include "ble_mesh_scheduler.h"
#include "ble_mesh_tx_queue.h"
#include "test_radio_scheduler_support.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * This staged contract uses the production routed scheduler, queue, radio
 * driver, and host register shim. It deliberately does not duplicate
 * selection or rolling-accounting logic in the test fixture.
 *
 * Required next API:
 *   BLE_MESH_SCHED_BUDGET_API == 1
 *   typedef struct ble_mesh_sched_budget_snapshot {
 *       uint8_t live_total_bu;
 *       uint8_t live_general_bu;
 *       uint8_t live_critical_bu;
 *       uint8_t custody_hold;
 *   } ble_mesh_sched_budget_snapshot_t;
 *   int ble_mesh_scheduler_get_budget_snapshot(
 *       const ble_mesh_scheduler_t *sched, uint32_t now_ms,
 *       ble_mesh_sched_budget_snapshot_t *snapshot_out);
 *
 * The snapshot is read-only: it exposes aggregate live usage and the hold bit,
 * never a budget-ring slot or mutable accounting state. Passing now_ms makes
 * exact 999/1000 ms and UINT32_MAX-wrap observations deterministic.
 */
#if !defined(BLE_MESH_SCHED_BUDGET_API)
#error "BUDGET_RED_GATE: BLE_MESH_SCHED_BUDGET_API=1 is required"
#elif BLE_MESH_SCHED_BUDGET_API != 1
#error "BUDGET_RED_GATE: BLE_MESH_SCHED_BUDGET_API must equal 1"
#endif

#if defined(BLE_MESH_SCHED_BUDGET_API) && BLE_MESH_SCHED_BUDGET_API == 1

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

typedef struct budget_probe {
    ble_mesh_scheduler_t *scheduler;
} budget_probe_t;

typedef struct tx_start_probe {
    ble_mesh_scheduler_t *scheduler;
    uint32_t now_ms;
    uint8_t total_bu;
    uint8_t general_bu;
    uint8_t critical_bu;
    uint8_t custody_hold;
    int snapshot_ok;
    unsigned int calls;
} tx_start_probe_t;

static const uint8_t local_adva[6] = {
    0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu,
};

static void record_first_physical_start(void *context)
{
    tx_start_probe_t *probe = context;
    ble_mesh_sched_budget_snapshot_t snapshot;

    if (probe == NULL || probe->scheduler == NULL) {
        return;
    }
    if (probe->calls == 0u) {
        memset(&snapshot, 0, sizeof(snapshot));
        probe->snapshot_ok = ble_mesh_scheduler_get_budget_snapshot(
            probe->scheduler, probe->now_ms, &snapshot) == 1;
        probe->total_bu = snapshot.live_total_bu;
        probe->general_bu = snapshot.live_general_bu;
        probe->critical_bu = snapshot.live_critical_bu;
        probe->custody_hold = snapshot.custody_hold;
    }
    probe->calls++;
}

static int prepare_scheduler(ble_mesh_scheduler_t *scheduler,
                             budget_probe_t *probe, uint32_t now_ms)
{
    if (scheduler == NULL || probe == NULL) {
        return 0;
    }
    test_radio_script_reset();
    if (ble_radio_try_init(2u) != BLE_RADIO_OP_OK ||
        ble_radio_try_idle(2u) != BLE_RADIO_OP_OK) {
        return 0;
    }
    memset(probe, 0, sizeof(*probe));
    probe->scheduler = scheduler;
    ble_mesh_scheduler_init(scheduler, now_ms, local_adva);
    return scheduler->routed_started == 1u;
}

static ble_mesh_tx_item_t make_item(uint8_t marker,
                                    ble_mesh_tx_priority_t priority,
                                    ble_mesh_tx_budget_class_t budget_class,
                                    ble_mesh_tx_sweep_count_t sweep_count,
                                    ble_mesh_tx_token_t token,
                                    uint32_t not_before_ms,
                                    uint32_t expiry_ms)
{
    ble_mesh_tx_item_t item;

    memset(&item, 0, sizeof(item));
    item.adv_len = 3u;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = priority;
    item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
    item.not_before_ms = not_before_ms;
    item.expiry_ms = expiry_ms;
    item.sweep_count = sweep_count;
    item.budget_class = budget_class;
    item.token = token;
    item.adv_data[0] = marker;
    item.adv_data[1] = 0x01u;
    item.adv_data[2] = 0x06u;
    return item;
}

static ble_mesh_tx_item_t make_custody_item(uint8_t marker,
                                            ble_mesh_tx_token_t token,
                                            uint32_t not_before_ms,
                                            uint32_t expiry_ms)
{
    ble_mesh_tx_item_t item = make_item(
        marker, BLE_MESH_TX_PRIORITY_DATA, BLE_MESH_TX_BUDGET_GENERAL,
        BLE_MESH_TX_SWEEP_COUNT_TWO, token, not_before_ms, expiry_ms);

    item.service_class = BLE_MESH_TX_SERVICE_CUSTODY_DATA;
    return item;
}

static int enqueue_item(ble_mesh_scheduler_t *scheduler,
                        const ble_mesh_tx_item_t *item)
{
    return ble_mesh_scheduler_enqueue_ex(scheduler, item).status ==
        BLE_MESH_SCHED_ENQUEUE_OK;
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

static int snapshot_matches(const ble_mesh_scheduler_t *scheduler,
                            uint32_t now_ms, uint8_t total_bu,
                            uint8_t general_bu, uint8_t critical_bu,
                            uint8_t custody_hold)
{
    ble_mesh_sched_budget_snapshot_t snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    return ble_mesh_scheduler_get_budget_snapshot(scheduler, now_ms, &snapshot) == 1 &&
        snapshot.live_total_bu == total_bu &&
        snapshot.live_general_bu == general_bu &&
        snapshot.live_critical_bu == critical_bu &&
        snapshot.custody_hold == custody_hold;
}

static int poll_success(ble_mesh_scheduler_t *scheduler, uint32_t now_ms,
                        ble_mesh_tx_token_t token)
{
    ble_mesh_sched_event_t event;

    test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, BLE_RADIO_ADV_CH_ALL,
                         BLE_RADIO_OP_OK, TEST_RADIO_FAILURE_NONE);
    return ble_mesh_scheduler_poll(scheduler, now_ms, &event) == 1 &&
        event.type == BLE_MESH_SCHED_EVENT_TX_DONE &&
        event.tx_token == token &&
        event.tx_requested_channel_mask == BLE_RADIO_ADV_CH_ALL &&
        event.tx_completed_channel_mask == BLE_RADIO_ADV_CH_ALL &&
        event.fault == BLE_MESH_SCHED_FAULT_NONE;
}

static int poll_no_tx(ble_mesh_scheduler_t *scheduler, uint32_t now_ms)
{
    ble_mesh_sched_event_t event;

    return ble_mesh_scheduler_poll(scheduler, now_ms, &event) == 0 &&
        event.type == BLE_MESH_SCHED_EVENT_NONE && scheduler->rx_started != 0u;
}

static void fill_general(ble_mesh_scheduler_t *scheduler, uint32_t now_ms,
                         uint8_t count, uint8_t marker_base)
{
    uint8_t index;

    for (index = 0u; index < count; index++) {
        ble_mesh_tx_item_t item = make_item(
            (uint8_t)(marker_base + index), BLE_MESH_TX_PRIORITY_CONTROL,
            BLE_MESH_TX_BUDGET_GENERAL, BLE_MESH_TX_SWEEP_COUNT_ONE,
            BLE_MESH_TX_TOKEN_NONE, now_ms, now_ms + 5000u);

        CHECK("BEARER-05", enqueue_item(scheduler, &item));
        CHECK("BEARER-05", poll_success(scheduler, now_ms,
                                         BLE_MESH_TX_TOKEN_NONE));
    }
}

static void mark_custody_promoted(ble_mesh_scheduler_t *scheduler)
{
    uint8_t bypass_max = tron_timer_config.scheduler_custody_bypass_max;

    scheduler->routed_tx_queue.custody_bypass_count = bypass_max;
    scheduler->custody_bypass_count = bypass_max;
}

static void test_priority_banded_edf(void)
{
    ble_mesh_scheduler_t scheduler;
    budget_probe_t probe;
    ble_mesh_tx_item_t control_late = make_item(
        0x11u, BLE_MESH_TX_PRIORITY_CONTROL, BLE_MESH_TX_BUDGET_GENERAL,
        BLE_MESH_TX_SWEEP_COUNT_ONE, 0x1101u, 1u, 500u);
    ble_mesh_tx_item_t data_early = make_item(
        0x12u, BLE_MESH_TX_PRIORITY_DATA, BLE_MESH_TX_BUDGET_GENERAL,
        BLE_MESH_TX_SWEEP_COUNT_ONE, 0x1102u, 1u, 2u);
    ble_mesh_tx_item_t control_early = make_item(
        0x13u, BLE_MESH_TX_PRIORITY_CONTROL, BLE_MESH_TX_BUDGET_GENERAL,
        BLE_MESH_TX_SWEEP_COUNT_ONE, 0x1103u, 1u, 3u);

    if (!prepare_scheduler(&scheduler, &probe, 0u)) {
        CHECK("LINK-07", 0);
        return;
    }
    CHECK("BEARER-05", enqueue_item(&scheduler, &control_late));
    CHECK("BEARER-05", enqueue_item(&scheduler, &data_early));
    CHECK("BEARER-05", enqueue_item(&scheduler, &control_early));
    CHECK("BEARER-05", poll_success(&scheduler, 1u, control_early.token));
    CHECK("BEARER-05", poll_success(&scheduler, 1u, control_late.token));
    CHECK("BEARER-05", poll_success(&scheduler, 1u, data_early.token));
    CHECK("BEARER-05", snapshot_matches(&scheduler, 1u, 3u, 3u, 0u, 0u));
}

static void test_general_and_total_budget_equality(void)
{
    ble_mesh_scheduler_t scheduler;
    budget_probe_t probe;
    ble_mesh_tx_item_t blocked_general;
    ble_mesh_tx_item_t critical;
    uint8_t index;

    if (!prepare_scheduler(&scheduler, &probe, 0u)) {
        CHECK("LINK-07", 0);
        return;
    }
    fill_general(&scheduler, 1u, 32u, 0x20u);
    CHECK("BEARER-05", snapshot_matches(&scheduler, 1u, 32u, 32u, 0u, 0u));

    blocked_general = make_item(
        0x51u, BLE_MESH_TX_PRIORITY_CONTROL, BLE_MESH_TX_BUDGET_GENERAL,
        BLE_MESH_TX_SWEEP_COUNT_ONE, 0x1501u, 1u, 5001u);
    CHECK("BEARER-05", enqueue_item(&scheduler, &blocked_general));
    CHECK("BEARER-05", poll_no_tx(&scheduler, 1u));
    CHECK("BEARER-05", queue_has_token(&scheduler.routed_tx_queue,
                                         blocked_general.token) &&
                       ble_mesh_tx_queue_count(&scheduler.routed_tx_queue) == 1u);
    CHECK("BEARER-05", snapshot_matches(&scheduler, 1u, 32u, 32u, 0u, 0u));
    CHECK("LINK-07", ble_mesh_tx_queue_cancel(&scheduler.routed_tx_queue,
                                                blocked_general.token) ==
                     BLE_MESH_TX_RETIRE_OK);

    for (index = 0u; index < 8u; index++) {
        critical = make_item(
            (uint8_t)(0x60u + index), BLE_MESH_TX_PRIORITY_HACK,
            BLE_MESH_TX_BUDGET_CRITICAL, BLE_MESH_TX_SWEEP_COUNT_ONE,
            BLE_MESH_TX_TOKEN_NONE, 1u, 5001u);
        CHECK("BEARER-05", enqueue_item(&scheduler, &critical));
        CHECK("BEARER-05", poll_success(&scheduler, 1u,
                                         BLE_MESH_TX_TOKEN_NONE));
    }
    CHECK("BEARER-05", snapshot_matches(&scheduler, 1u, 40u, 32u, 8u, 0u));

    critical = make_item(
        0x70u, BLE_MESH_TX_PRIORITY_HACK, BLE_MESH_TX_BUDGET_CRITICAL,
        BLE_MESH_TX_SWEEP_COUNT_ONE, 0x1502u, 1u, 5001u);
    CHECK("BEARER-05", enqueue_item(&scheduler, &critical));
    CHECK("BEARER-05", poll_no_tx(&scheduler, 1u));
    CHECK("BEARER-05", queue_has_token(&scheduler.routed_tx_queue,
                                         critical.token) &&
                       ble_mesh_tx_queue_count(&scheduler.routed_tx_queue) == 1u);
    CHECK("BEARER-05", snapshot_matches(&scheduler, 1u, 40u, 32u, 8u, 0u));
}

static void test_two_sweep_charge_survives_physical_outcomes(void)
{
    typedef struct outcome_case {
        uint8_t completed_mask;
        ble_radio_op_result_t radio_fault;
        test_radio_failure_location_t failure_location;
        ble_mesh_sched_event_type_t expected_event;
        ble_mesh_sched_fault_t expected_fault;
    } outcome_case_t;
    static const outcome_case_t outcomes[] = {
        { BLE_RADIO_ADV_CH_ALL, BLE_RADIO_OP_OK, TEST_RADIO_FAILURE_NONE,
          BLE_MESH_SCHED_EVENT_TX_DONE, BLE_MESH_SCHED_FAULT_NONE },
        { BLE_RADIO_ADV_CH37, BLE_RADIO_OP_STATE_TIMEOUT,
          TEST_RADIO_FAILURE_CH38, BLE_MESH_SCHED_EVENT_TX_DONE,
          BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT },
        { 0u, BLE_RADIO_OP_STATE_TIMEOUT, TEST_RADIO_FAILURE_CH37,
          BLE_MESH_SCHED_EVENT_TX_FAILED, BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT },
        { BLE_RADIO_ADV_CH_ALL, BLE_RADIO_OP_OK, TEST_RADIO_FAILURE_RESTORE,
          BLE_MESH_SCHED_EVENT_TX_DONE, BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT },
    };
    uint8_t index;

    for (index = 0u; index < (uint8_t)(sizeof(outcomes) / sizeof(outcomes[0]));
         index++) {
        ble_mesh_scheduler_t scheduler;
        budget_probe_t probe;
        ble_mesh_tx_item_t item = make_item(
            (uint8_t)(0x80u + index), BLE_MESH_TX_PRIORITY_CONTROL,
            BLE_MESH_TX_BUDGET_GENERAL, BLE_MESH_TX_SWEEP_COUNT_TWO,
            (ble_mesh_tx_token_t)(0x1601u + index), 1u, 5001u);
        ble_mesh_sched_event_t event;
        tx_start_probe_t tx_start;

        if (!prepare_scheduler(&scheduler, &probe, 0u)) {
            CHECK("LINK-07", 0);
            continue;
        }
        CHECK("BEARER-05", enqueue_item(&scheduler, &item));
        memset(&tx_start, 0, sizeof(tx_start));
        tx_start.scheduler = &scheduler;
        tx_start.now_ms = 1u;
        test_radio_set_tx_start_handler(record_first_physical_start, &tx_start);
        test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, outcomes[index].completed_mask,
                             outcomes[index].radio_fault,
                             outcomes[index].failure_location);
        CHECK("BEARER-05", ble_mesh_scheduler_poll(&scheduler, 1u, &event) == 1 &&
                           event.type == outcomes[index].expected_event &&
                           event.tx_token == item.token &&
                           event.fault == outcomes[index].expected_fault);
        CHECK("BEARER-05", tx_start.calls != 0u && tx_start.snapshot_ok &&
                           tx_start.total_bu == 2u && tx_start.general_bu == 2u &&
                           tx_start.critical_bu == 0u &&
                           tx_start.custody_hold == 0u);
        test_radio_set_tx_start_handler(NULL, NULL);
        CHECK("BEARER-05", snapshot_matches(&scheduler, 1u, 2u, 2u, 0u, 0u));
    }
}

static void test_rolling_window_boundary(uint32_t charged_at_ms,
                                         uint8_t marker)
{
    ble_mesh_scheduler_t scheduler;
    budget_probe_t probe;
    uint32_t expires_at_ms = charged_at_ms + 1000u;
    ble_mesh_tx_item_t first = make_item(
        marker, BLE_MESH_TX_PRIORITY_CONTROL, BLE_MESH_TX_BUDGET_GENERAL,
        BLE_MESH_TX_SWEEP_COUNT_ONE, BLE_MESH_TX_TOKEN_NONE, charged_at_ms,
        charged_at_ms + 5000u);
    ble_mesh_tx_item_t second = make_item(
        (uint8_t)(marker + 1u), BLE_MESH_TX_PRIORITY_CONTROL,
        BLE_MESH_TX_BUDGET_GENERAL, BLE_MESH_TX_SWEEP_COUNT_ONE,
        BLE_MESH_TX_TOKEN_NONE, expires_at_ms, expires_at_ms + 5000u);

    if (!prepare_scheduler(&scheduler, &probe, charged_at_ms)) {
        CHECK("LINK-07", 0);
        return;
    }
    CHECK("BEARER-05", enqueue_item(&scheduler, &first));
    CHECK("BEARER-05", poll_success(&scheduler, charged_at_ms,
                                     BLE_MESH_TX_TOKEN_NONE));
    CHECK("BEARER-05", snapshot_matches(&scheduler, charged_at_ms + 999u,
                                         1u, 1u, 0u, 0u));
    CHECK("BEARER-05", snapshot_matches(&scheduler, expires_at_ms,
                                         0u, 0u, 0u, 0u));
    CHECK("BEARER-05", enqueue_item(&scheduler, &second));
    CHECK("BEARER-05", poll_success(&scheduler, expires_at_ms,
                                     BLE_MESH_TX_TOKEN_NONE));
    CHECK("BEARER-05", snapshot_matches(&scheduler, expires_at_ms,
                                         1u, 1u, 0u, 0u));
}

static void test_exact_rolling_window_boundaries(void)
{
    test_rolling_window_boundary(1u, 0x90u);
    test_rolling_window_boundary(UINT32_MAX - 500u, 0xa0u);
}

static void test_budget_blocked_ordinary_selection_is_not_bypassed(void)
{
    ble_mesh_scheduler_t scheduler;
    budget_probe_t probe;
    ble_mesh_tx_item_t selected_general = make_item(
        0xb1u, BLE_MESH_TX_PRIORITY_CONTROL, BLE_MESH_TX_BUDGET_GENERAL,
        BLE_MESH_TX_SWEEP_COUNT_ONE, 0x1701u, 1u, 5001u);
    ble_mesh_tx_item_t lower_critical = make_item(
        0xb2u, BLE_MESH_TX_PRIORITY_DATA, BLE_MESH_TX_BUDGET_CRITICAL,
        BLE_MESH_TX_SWEEP_COUNT_ONE, 0x1702u, 1u, 5001u);

    if (!prepare_scheduler(&scheduler, &probe, 0u)) {
        CHECK("LINK-07", 0);
        return;
    }
    fill_general(&scheduler, 1u, 32u, 0xb8u);
    CHECK("BEARER-05", enqueue_item(&scheduler, &selected_general));
    CHECK("BEARER-05", enqueue_item(&scheduler, &lower_critical));
    CHECK("BEARER-05", poll_no_tx(&scheduler, 1u));
    CHECK("BEARER-05", queue_has_token(&scheduler.routed_tx_queue,
                                         selected_general.token) &&
                       queue_has_token(&scheduler.routed_tx_queue,
                                       lower_critical.token) &&
                       ble_mesh_tx_queue_count(&scheduler.routed_tx_queue) == 2u);
    CHECK("BEARER-05", snapshot_matches(&scheduler, 1u, 32u, 32u, 0u, 0u));
}

static void test_custody_hold_and_reserved_critical_lane(void)
{
    ble_mesh_scheduler_t scheduler;
    budget_probe_t probe;
    ble_mesh_tx_item_t custody = make_custody_item(0xc1u, 0x1801u, 1u, 5001u);
    ble_mesh_tx_item_t general_fallback = make_item(
        0xc2u, BLE_MESH_TX_PRIORITY_HACK, BLE_MESH_TX_BUDGET_GENERAL,
        BLE_MESH_TX_SWEEP_COUNT_ONE, 0x1802u, 1u, 5001u);
    ble_mesh_tx_item_t lower_critical = make_item(
        0xc3u, BLE_MESH_TX_PRIORITY_RELAY, BLE_MESH_TX_BUDGET_CRITICAL,
        BLE_MESH_TX_SWEEP_COUNT_ONE, 0x1803u, 1u, 5001u);
    ble_mesh_tx_item_t later_critical = make_item(
        0xc4u, BLE_MESH_TX_PRIORITY_HACK, BLE_MESH_TX_BUDGET_CRITICAL,
        BLE_MESH_TX_SWEEP_COUNT_ONE, 0x1804u, 2u, 5001u);
    ble_mesh_tx_item_t critical;
    uint8_t index;

    if (!prepare_scheduler(&scheduler, &probe, 0u)) {
        CHECK("LINK-07", 0);
        return;
    }
    fill_general(&scheduler, 1u, 32u, 0xd0u);
    CHECK("LINK-04", enqueue_item(&scheduler, &custody));
    CHECK("BEARER-05", enqueue_item(&scheduler, &general_fallback));
    CHECK("BEARER-05", enqueue_item(&scheduler, &lower_critical));
    CHECK("BEARER-05", enqueue_item(&scheduler, &later_critical));
    mark_custody_promoted(&scheduler);
    CHECK("BEARER-05", scheduler.custody_bypass_count ==
                       tron_timer_config.scheduler_custody_bypass_max &&
                       scheduler.routed_tx_queue.custody_bypass_count ==
                       tron_timer_config.scheduler_custody_bypass_max);
    CHECK("BEARER-05", poll_no_tx(&scheduler, 1u));
    CHECK("BEARER-05", snapshot_matches(&scheduler, 1u, 32u, 32u, 0u, 1u));
    CHECK("LINK-07", queue_has_token(&scheduler.routed_tx_queue, custody.token) &&
                     queue_has_token(&scheduler.routed_tx_queue,
                                      general_fallback.token) &&
                     queue_has_token(&scheduler.routed_tx_queue,
                                      lower_critical.token) &&
                     queue_has_token(&scheduler.routed_tx_queue,
                                      later_critical.token));

    CHECK("LINK-07", ble_mesh_tx_queue_cancel(&scheduler.routed_tx_queue,
                                                general_fallback.token) ==
                     BLE_MESH_TX_RETIRE_OK &&
                     ble_mesh_tx_queue_cancel(&scheduler.routed_tx_queue,
                                              lower_critical.token) ==
                     BLE_MESH_TX_RETIRE_OK);
    CHECK("BEARER-05", poll_success(&scheduler, 2u, later_critical.token));
    CHECK("BEARER-05", snapshot_matches(&scheduler, 2u, 33u, 32u, 1u, 1u));

    for (index = 1u; index < 8u; index++) {
        critical = make_item(
            (uint8_t)(0xe0u + index), BLE_MESH_TX_PRIORITY_HACK,
            BLE_MESH_TX_BUDGET_CRITICAL, BLE_MESH_TX_SWEEP_COUNT_ONE,
            (ble_mesh_tx_token_t)(0x1810u + index), 2u, 5002u);
        CHECK("BEARER-05", enqueue_item(&scheduler, &critical));
        CHECK("BEARER-05", poll_success(&scheduler, 2u, critical.token));
    }
    CHECK("BEARER-05", snapshot_matches(&scheduler, 2u, 40u, 32u, 8u, 1u));

    critical = make_item(
        0xf0u, BLE_MESH_TX_PRIORITY_HACK, BLE_MESH_TX_BUDGET_CRITICAL,
        BLE_MESH_TX_SWEEP_COUNT_ONE, 0x1820u, 2u, 5002u);
    CHECK("BEARER-05", enqueue_item(&scheduler, &critical));
    CHECK("BEARER-05", poll_no_tx(&scheduler, 2u));
    CHECK("BEARER-05", queue_has_token(&scheduler.routed_tx_queue,
                                         custody.token) &&
                       queue_has_token(&scheduler.routed_tx_queue,
                                       critical.token));
    CHECK("BEARER-05", snapshot_matches(&scheduler, 2u, 40u, 32u, 8u, 1u));

    CHECK("BEARER-05", poll_success(&scheduler, 1001u, custody.token));
    CHECK("LINK-07", !queue_has_token(&scheduler.routed_tx_queue, custody.token) &&
                     queue_has_token(&scheduler.routed_tx_queue, critical.token));
    CHECK("BEARER-05", snapshot_matches(&scheduler, 1001u, 10u, 2u, 8u, 0u));
}

static void test_custody_cancel_clears_hold(void)
{
    ble_mesh_scheduler_t scheduler;
    budget_probe_t probe;
    ble_mesh_tx_item_t custody = make_custody_item(0xf1u, 0x1901u, 1u, 5001u);

    if (!prepare_scheduler(&scheduler, &probe, 0u)) {
        CHECK("LINK-07", 0);
        return;
    }
    fill_general(&scheduler, 1u, 32u, 0x30u);
    CHECK("LINK-04", enqueue_item(&scheduler, &custody));
    mark_custody_promoted(&scheduler);
    CHECK("BEARER-05", poll_no_tx(&scheduler, 1u));
    CHECK("BEARER-05", snapshot_matches(&scheduler, 1u, 32u, 32u, 0u, 1u));
    CHECK("LINK-07", ble_mesh_tx_queue_cancel(&scheduler.routed_tx_queue,
                                                custody.token) ==
                     BLE_MESH_TX_RETIRE_OK);
    CHECK("BEARER-05", poll_no_tx(&scheduler, 1u));
    CHECK("BEARER-05", snapshot_matches(&scheduler, 1u, 32u, 32u, 0u, 0u));

}

static void test_bypass_advances_only_for_successful_nonpromoted_work(void)
{
    ble_mesh_scheduler_t scheduler;
    budget_probe_t probe;
    ble_mesh_tx_item_t custody = make_custody_item(0x71u, 0x1a01u, 1u, 5001u);
    ble_mesh_tx_item_t ordinary = make_item(
        0x72u, BLE_MESH_TX_PRIORITY_HACK, BLE_MESH_TX_BUDGET_GENERAL,
        BLE_MESH_TX_SWEEP_COUNT_ONE, 0x1a02u, 1u, 5001u);
    ble_mesh_sched_event_t event;

    if (!prepare_scheduler(&scheduler, &probe, 0u)) {
        CHECK("LINK-07", 0);
        return;
    }
    CHECK("LINK-04", enqueue_item(&scheduler, &custody));
    CHECK("BEARER-05", enqueue_item(&scheduler, &ordinary));
    CHECK("BEARER-05", poll_success(&scheduler, 1u, ordinary.token));
    CHECK("BEARER-05", scheduler.custody_bypass_count == 1u &&
                       scheduler.routed_tx_queue.custody_bypass_count == 1u &&
                       queue_has_token(&scheduler.routed_tx_queue, custody.token));

    if (!prepare_scheduler(&scheduler, &probe, 0u)) {
        CHECK("LINK-07", 0);
        return;
    }
    fill_general(&scheduler, 1u, 32u, 0x80u);
    CHECK("LINK-04", enqueue_item(&scheduler, &custody));
    CHECK("BEARER-05", enqueue_item(&scheduler, &ordinary));
    CHECK("BEARER-05", poll_no_tx(&scheduler, 1u));
    CHECK("BEARER-05", scheduler.custody_bypass_count == 0u &&
                       scheduler.routed_tx_queue.custody_bypass_count == 0u &&
                       queue_has_token(&scheduler.routed_tx_queue, custody.token) &&
                       queue_has_token(&scheduler.routed_tx_queue, ordinary.token));

    if (!prepare_scheduler(&scheduler, &probe, 0u)) {
        CHECK("LINK-07", 0);
        return;
    }
    CHECK("LINK-04", enqueue_item(&scheduler, &custody));
    CHECK("BEARER-05", enqueue_item(&scheduler, &ordinary));
    test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, 0u, BLE_RADIO_OP_STATE_TIMEOUT,
                         TEST_RADIO_FAILURE_CH37);
    CHECK("BEARER-05", ble_mesh_scheduler_poll(&scheduler, 1u, &event) == 1 &&
                       event.type == BLE_MESH_SCHED_EVENT_TX_FAILED &&
                       event.tx_token == ordinary.token);
    CHECK("LINK-07", scheduler.custody_bypass_count == 0u &&
                     scheduler.routed_tx_queue.custody_bypass_count == 0u &&
                     queue_has_token(&scheduler.routed_tx_queue, custody.token));
    CHECK("BEARER-05", snapshot_matches(&scheduler, 1u, 1u, 1u, 0u, 0u));
}

static void test_expiry_retirement_precedes_selection(void)
{
    ble_mesh_scheduler_t scheduler;
    budget_probe_t probe;
    ble_mesh_sched_event_t event;
    ble_mesh_tx_item_t item;
    const ble_mesh_sched_counters_t *counters;
    unsigned int disables_before;

    CHECK("LINK-07", prepare_scheduler(&scheduler, &probe, 0u));
    item = make_item(0xe0u, BLE_MESH_TX_PRIORITY_HACK,
                     BLE_MESH_TX_BUDGET_CRITICAL,
                     BLE_MESH_TX_SWEEP_COUNT_ONE, BLE_MESH_TX_TOKEN_NONE,
                     UINT32_MAX - 8u, UINT32_MAX - 2u);
    CHECK("BEARER-06", enqueue_item(&scheduler, &item));
    item = make_item(0xe1u, BLE_MESH_TX_PRIORITY_CONTROL,
                     BLE_MESH_TX_BUDGET_GENERAL,
                     BLE_MESH_TX_SWEEP_COUNT_TWO, 0x3101u,
                     UINT32_MAX - 8u, UINT32_MAX);
    CHECK("LINK-07", enqueue_item(&scheduler, &item));
    item = make_item(0xe2u, BLE_MESH_TX_PRIORITY_CONTROL,
                     BLE_MESH_TX_BUDGET_GENERAL,
                     BLE_MESH_TX_SWEEP_COUNT_ONE, 0x3102u, 0u, 0u);
    CHECK("LINK-07", enqueue_item(&scheduler, &item));
    item = make_item(0xe3u, BLE_MESH_TX_PRIORITY_RELAY,
                     BLE_MESH_TX_BUDGET_GENERAL,
                     BLE_MESH_TX_SWEEP_COUNT_ONE, BLE_MESH_TX_TOKEN_NONE,
                     0u, 100u);
    CHECK("BEARER-05", enqueue_item(&scheduler, &item));

    disables_before = test_radio_disable_count();
    memset(&event, 0xa5, sizeof(event));
    CHECK("LINK-07", ble_mesh_scheduler_poll(&scheduler, 0u, &event) == 1 &&
                     event.type == BLE_MESH_SCHED_EVENT_TX_EXPIRED &&
                     event.tx_token == 0x3101u &&
                     event.tx_requested_channel_mask == BLE_RADIO_ADV_CH_ALL &&
                     event.tx_requested_sweep_count == BLE_MESH_TX_SWEEP_COUNT_TWO &&
                     event.tx_attempted_sweep_count == 0u &&
                     event.tx_completed_channel_mask == 0u &&
                     event.tx_completed_channel_masks[0] == 0u &&
                     event.tx_completed_channel_masks[1] == 0u &&
                     event.fault == BLE_MESH_SCHED_FAULT_NONE &&
                     ble_mesh_tx_queue_count(&scheduler.routed_tx_queue) == 2u &&
                     test_radio_disable_count() == disables_before);
    counters = ble_mesh_scheduler_counters(&scheduler);
    CHECK("BEARER-06", counters != NULL &&
                       counters->tx_expired_anonymous == 1u &&
                       counters->tx_expired_tracked == 1u);

    CHECK("LINK-07", ble_mesh_scheduler_poll(&scheduler, 0u, &event) == 1 &&
                     event.type == BLE_MESH_SCHED_EVENT_TX_EXPIRED &&
                     event.tx_token == 0x3102u &&
                     event.tx_requested_sweep_count == BLE_MESH_TX_SWEEP_COUNT_ONE &&
                     event.tx_attempted_sweep_count == 0u &&
                     event.tx_completed_channel_mask == 0u &&
                     ble_mesh_tx_queue_count(&scheduler.routed_tx_queue) == 1u &&
                     test_radio_disable_count() == disables_before);
    CHECK("BEARER-06", counters->tx_expired_anonymous == 1u &&
                       counters->tx_expired_tracked == 2u);

    test_radio_script_tx(BLE_RADIO_ADV_CH_ALL, BLE_RADIO_ADV_CH_ALL,
                         BLE_RADIO_OP_OK, TEST_RADIO_FAILURE_NONE);
    CHECK("BEARER-05", ble_mesh_scheduler_poll(&scheduler, 0u, &event) == 1 &&
                        event.type == BLE_MESH_SCHED_EVENT_TX_DONE &&
                        event.tx_token == BLE_MESH_TX_TOKEN_NONE &&
                        ble_mesh_tx_queue_count(&scheduler.routed_tx_queue) == 0u);
}

static void test_tracked_expiry_requires_event_storage(void)
{
    ble_mesh_scheduler_t scheduler;
    budget_probe_t probe;
    ble_mesh_sched_event_t event;
    ble_mesh_tx_item_t item;
    const ble_mesh_sched_counters_t *counters;

    CHECK("LINK-07", prepare_scheduler(&scheduler, &probe, 100u));
    item = make_item(0xe4u, BLE_MESH_TX_PRIORITY_CONTROL,
                     BLE_MESH_TX_BUDGET_GENERAL,
                     BLE_MESH_TX_SWEEP_COUNT_ONE, 0x3103u, 100u, 100u);
    CHECK("LINK-07", enqueue_item(&scheduler, &item));
    CHECK("LINK-07", ble_mesh_scheduler_poll(&scheduler, 100u, NULL) == 0 &&
                     ble_mesh_tx_queue_count(&scheduler.routed_tx_queue) == 1u);
    counters = ble_mesh_scheduler_counters(&scheduler);
    CHECK("LINK-07", counters != NULL && counters->tx_expired_tracked == 0u);
    CHECK("LINK-07", ble_mesh_scheduler_poll(&scheduler, 100u, &event) == 1 &&
                     event.type == BLE_MESH_SCHED_EVENT_TX_EXPIRED &&
                     event.tx_token == 0x3103u &&
                     ble_mesh_tx_queue_count(&scheduler.routed_tx_queue) == 0u &&
                     counters->tx_expired_tracked == 1u);
}

int main(void)
{
    test_priority_banded_edf();
    test_general_and_total_budget_equality();
    test_two_sweep_charge_survives_physical_outcomes();
    test_exact_rolling_window_boundaries();
    test_budget_blocked_ordinary_selection_is_not_bypassed();
    test_custody_hold_and_reserved_critical_lane();
    test_custody_cancel_clears_hold();
    test_bypass_advances_only_for_successful_nonpromoted_work();
    test_expiry_retirement_precedes_selection();
    test_tracked_expiry_requires_event_storage();

    if (failures != 0u) {
        printf("tavrn_future_bearer budget RED tests failed: %u assertion(s)\n",
               failures);
        return 1;
    }
    printf("tavrn_future_bearer budget tests passed\n");
    return 0;
}

#endif /* BLE_MESH_SCHED_BUDGET_API */
