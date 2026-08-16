#include "mind_application_ingress.h"
#include "routed_cycle.h"
#include "tron_mesh_packet.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int checks;
static unsigned int failures;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void make_mind_event(ble_mesh_sched_event_t *event, uint8_t network_id,
                            uint8_t wearable_id, uint32_t packet_id24)
{
    uint8_t *adv;

    memset(event, 0, sizeof(*event));
    event->type = BLE_MESH_SCHED_EVENT_RX_ADV;
    event->channel = BLE_MESH_SCHED_CH38;
    event->rssi_magnitude_db = 41u;
    event->adv_addr[4] = wearable_id;
    event->adv_addr[5] = 0xc0u;
    event->adv_len = MIND_APPLICATION_INGRESS_FRAME_BYTES;
    adv = event->adv_data;
    adv[0] = TRON_MESH_FLAGS_AD_LEN;
    adv[1] = TRON_MESH_FLAGS_AD_TYPE;
    adv[2] = TRON_MESH_FLAGS_VALUE;
    adv[3] = (uint8_t)(TRON_MESH_MANUF_BASE_LEN + MIND_PAYLOAD_SIZE);
    adv[4] = TRON_MESH_MANUF_AD_TYPE;
    adv[5] = 0xffu;
    adv[6] = 0xffu;
    adv[7] = (uint8_t)TRON_MESH_MAGIC0;
    adv[8] = (uint8_t)TRON_MESH_MAGIC1;
    adv[9] = TRON_MESH_VERSION;
    adv[10] = TRON_MESH_MSG_TYPE_MIND_EVENT;
    adv[11] = network_id;
    adv[12] = 0u;
    adv[13] = wearable_id;
    adv[14] = 0x01u;
    adv[15] = (uint8_t)(packet_id24 & 0xffu);
    adv[16] = (uint8_t)((packet_id24 >> 8) & 0xffu);
    adv[17] = (uint8_t)((packet_id24 >> 16) & 0xffu);
    adv[18] = MIND_PAYLOAD_SIZE;
    adv[19] = MIND_SCHEMA_VERSION;
    adv[20] = MIND_EVT_FALL_AND_SHOUT;
    adv[21] = 73u;
    adv[22] = 0x1fu;
    adv[23] = 0x1au;
    adv[24] = 0x7du;
    adv[25] = (uint8_t)(packet_id24 & 0xffu);
}

static mind_application_ingress_result_t receive(
    mind_application_ingress_t *ingress, uint8_t network_id,
    uint8_t wearable_id, uint32_t packet_id24, uint32_t now_ms)
{
    ble_mesh_sched_event_t event;

    make_mind_event(&event, network_id, wearable_id, packet_id24);
    return mind_application_ingress_receive(ingress, &event, now_ms);
}

static void take_one(mind_application_ingress_t *ingress)
{
    mind_application_ingress_event_t event;

    check(mind_application_ingress_take(ingress, &event) ==
              MIND_APPLICATION_INGRESS_TAKE_OK,
          "admitted report can be removed from the copied FIFO");
}

static void test_accepted_duplicate_and_identity(void)
{
    mind_application_ingress_t ingress;
    mind_application_ingress_event_t event;
    mind_application_ingress_result_t result;

    mind_application_ingress_init(&ingress, 7u);
    result = receive(&ingress, 7u, 0x2au, 0xabcdefu, 100u);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED &&
              result.validation == MIND_APPLICATION_INGRESS_VALID,
          "valid direct MIND report is accepted and consumed");
    result = receive(&ingress, 7u, 0x2au, 0xabcdefu, 101u);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_DUPLICATE &&
              ingress.queue_count == 1u && ingress.counters.duplicate == 1u,
          "duplicate reserves neither another seen slot nor queue item");
    check(mind_application_ingress_take(&ingress, &event) ==
              MIND_APPLICATION_INGRESS_TAKE_OK &&
              event.network_id == 7u && event.wearable_src16 == 0x012au &&
               event.packet_id24 == 0xabcdefu &&
               event.report.app_kind == MIND_REPORT &&
               event.report.app_source == 0x2au &&
               event.report.urgent == 1u &&
               event.report.app_len == MIND_APPLICATION_REPORT_BYTES &&
               event.report.app_bytes[0] == 0xefu &&
               event.report.app_bytes[1] == 0xcdu &&
               event.report.app_bytes[2] == 0xabu &&
               event.report.app_bytes[9] == 0xefu && event.observed_at_ms == 100u &&
               event.channel == BLE_MESH_SCHED_CH38 && event.rssi_magnitude_db == 41u &&
               mind_application_wire_validate(&event.report) == MIND_APPLICATION_WIRE_OK,
          "queue item preserves the validated urgent MIND_REPORT and RF evidence");

    result = receive(&ingress, 7u, 0x2bu, 0xabcdefu, 102u);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED &&
              ingress.queue_count == 1u,
          "same packet ID from a different wearable source is distinct");
}

static void test_exact_equality_and_wrap_expiry(void)
{
    mind_application_ingress_t ingress;
    mind_application_ingress_result_t result;
    uint32_t committed_at = 0xffffff00u;
    uint32_t deadline = committed_at + MIND_APPLICATION_INGRESS_RETENTION_MS;

    mind_application_ingress_init(&ingress, 7u);
    check(receive(&ingress, 7u, 1u, 0x11u, 100u).outcome ==
              MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED &&
              receive(&ingress, 7u, 1u, 0x11u, 2099u).outcome ==
              MIND_APPLICATION_INGRESS_CONSUMED_DUPLICATE &&
              receive(&ingress, 7u, 1u, 0x11u, 2100u).outcome ==
              MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED,
          "2000 ms equality is expired rather than duplicate");

    mind_application_ingress_init(&ingress, 7u);
    check(receive(&ingress, 7u, 2u, 0x22u, committed_at).outcome ==
              MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED,
          "wrap fixture commits a seen key");
    result = receive(&ingress, 7u, 2u, 0x22u, deadline - 1u);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_DUPLICATE,
          "wrap-safe time just before deadline remains live");
    result = receive(&ingress, 7u, 2u, 0x22u, deadline);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED &&
              ingress.counters.expired_purged == 1u,
          "wrap-safe deadline equality expires the committed key");
}

static void test_malformed_and_foreign_are_consumed_without_poison(void)
{
    mind_application_ingress_t ingress;
    ble_mesh_sched_event_t event;
    mind_application_ingress_result_t result;

    mind_application_ingress_init(&ingress, 7u);
    make_mind_event(&event, 7u, 3u, 0x33u);
    event.adv_data[9] = 2u;
    result = mind_application_ingress_receive(&ingress, &event, 10u);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_INVALID &&
              result.validation == MIND_APPLICATION_INGRESS_INVALID_VERSION &&
              ingress.queue_count == 0u,
          "recognized bad-version MIND frame is consumed without publication");
    make_mind_event(&event, 7u, 3u, 0x33u);
    check(mind_application_ingress_receive(&ingress, &event, 11u).outcome ==
              MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED,
          "malformed frame leaves its key unseen for a later spray copy");
    take_one(&ingress);

    make_mind_event(&event, 8u, 4u, 0x44u);
    result = mind_application_ingress_receive(&ingress, &event, 12u);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_INVALID &&
              result.validation == MIND_APPLICATION_INGRESS_INVALID_NETWORK,
          "foreign-network MIND frame is consumed locally");
    make_mind_event(&event, 7u, 4u, 0x44u);
    check(mind_application_ingress_receive(&ingress, &event, 13u).outcome ==
              MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED,
          "foreign frame does not poison the local key");
    take_one(&ingress);

    make_mind_event(&event, 7u, 5u, 0x55u);
    event.adv_len = 25u;
    result = mind_application_ingress_receive(&ingress, &event, 14u);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_INVALID &&
              result.validation == MIND_APPLICATION_INGRESS_INVALID_FRAME_LENGTH,
          "recognized truncated MIND wrapper is consumed");
    make_mind_event(&event, 7u, 5u, 0x55u);
    event.adv_data[12] = 1u;
    result = mind_application_ingress_receive(&ingress, &event, 15u);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_INVALID &&
              result.validation == MIND_APPLICATION_INGRESS_INVALID_TTL,
          "nonzero TTL is rejected before cache mutation");
    make_mind_event(&event, 7u, 5u, 0x55u);
    event.adv_addr[4] = 6u;
    result = mind_application_ingress_receive(&ingress, &event, 16u);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_INVALID &&
              result.validation == MIND_APPLICATION_INGRESS_INVALID_ADVA,
          "outer AdvA must match MIND_ADVA(source ID)");
    make_mind_event(&event, 7u, 5u, 0x55u);
    event.adv_data[25] = 0u;
    result = mind_application_ingress_receive(&ingress, &event, 17u);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_INVALID &&
              result.validation == MIND_APPLICATION_INGRESS_INVALID_SCHEMA,
          "schema sequence must equal packet ID low byte");
}

static void test_cache_and_queue_full_leave_keys_unseen(void)
{
    mind_application_ingress_t ingress;
    mind_application_ingress_result_t result;
    uint8_t wearable_id;

    mind_application_ingress_init(&ingress, 7u);
    for (wearable_id = 1u;
         wearable_id <= MIND_APPLICATION_INGRESS_SEEN_CAPACITY; wearable_id++) {
        check(receive(&ingress, 7u, wearable_id, wearable_id, 100u).outcome ==
                  MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED,
              "each free seen-cache slot admits one report");
        take_one(&ingress);
    }
    result = receive(&ingress, 7u, 17u, 17u, 101u);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_CACHE_FULL &&
              ingress.queue_count == 0u && ingress.counters.cache_full == 1u,
          "live full seen cache rejects without queue publication");
    result = receive(&ingress, 7u, 17u, 17u, 2100u);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED,
          "cache-full rejection leaves the key unseen for retry after expiry");

    mind_application_ingress_init(&ingress, 7u);
    for (wearable_id = 1u;
         wearable_id <= MIND_APPLICATION_INGRESS_QUEUE_CAPACITY; wearable_id++) {
        check(receive(&ingress, 7u, wearable_id, wearable_id, 100u).outcome ==
                  MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED,
              "each free queue slot admits one copied report");
    }
    result = receive(&ingress, 7u, 9u, 9u, 101u);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_QUEUE_FULL &&
              ingress.queue_count == MIND_APPLICATION_INGRESS_QUEUE_CAPACITY &&
              ingress.counters.queue_full == 1u,
          "full event FIFO rejects after seen reservation but before commit");
    take_one(&ingress);
    result = receive(&ingress, 7u, 9u, 9u, 102u);
    check(result.outcome == MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED,
          "queue-full rejection leaves its key unseen for a later spray copy");
}

static void test_queue_order_copy_and_counter_saturation(void)
{
    mind_application_ingress_t ingress;
    mind_application_ingress_event_t first;
    mind_application_ingress_event_t second;
    ble_mesh_sched_event_t event;

    mind_application_ingress_init(&ingress, 7u);
    make_mind_event(&event, 7u, 8u, 0x080808u);
    check(mind_application_ingress_receive(&ingress, &event, 50u).outcome ==
              MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED,
          "first FIFO copy is accepted");
    memset(event.adv_data, 0xa5, sizeof(event.adv_data));
    make_mind_event(&event, 7u, 9u, 0x090909u);
    check(mind_application_ingress_receive(&ingress, &event, 51u).outcome ==
              MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED,
          "second FIFO copy is accepted");
    check(mind_application_ingress_take(&ingress, &first) ==
              MIND_APPLICATION_INGRESS_TAKE_OK &&
              mind_application_ingress_take(&ingress, &second) ==
              MIND_APPLICATION_INGRESS_TAKE_OK && first.report.app_source == 8u &&
              first.packet_id24 == 0x080808u && first.report.app_bytes[3] ==
              MIND_SCHEMA_VERSION && second.report.app_source == 9u &&
              second.packet_id24 == 0x090909u,
          "FIFO order and copied ownership survive later RX-buffer mutation");

    ingress.counters.accepted = UINT32_MAX - 1u;
    check(receive(&ingress, 7u, 10u, 10u, 60u).outcome ==
              MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED &&
              receive(&ingress, 7u, 11u, 11u, 61u).outcome ==
              MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED &&
              ingress.counters.accepted == UINT32_MAX,
          "ingress counters saturate during churn instead of wrapping");
}

static void test_non_mind_tx_and_fault_passthrough(void)
{
    mind_application_ingress_t ingress;
    ble_mesh_sched_event_t event;

    mind_application_ingress_init(&ingress, 7u);
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
    check(mind_application_ingress_receive(&ingress, &event, 1u).outcome ==
              MIND_APPLICATION_INGRESS_PASSTHROUGH,
          "non-MIND RX passes unchanged");
    make_mind_event(&event, 7u, 1u, 1u);
    event.type = BLE_MESH_SCHED_EVENT_TX_DONE;
    check(mind_application_ingress_receive(&ingress, &event, 2u).outcome ==
              MIND_APPLICATION_INGRESS_PASSTHROUGH,
          "TX event passes unchanged even with MIND-shaped bytes");
    event.type = BLE_MESH_SCHED_EVENT_RADIO_FAULT;
    event.fault = BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT;
    check(mind_application_ingress_receive(&ingress, &event, 3u).outcome ==
              MIND_APPLICATION_INGRESS_PASSTHROUGH && ingress.queue_count == 0u &&
              ingress.counters.passed_non_mind == 3u,
           "fault event passes unchanged and never publishes ingress state");
}

static void test_corrupt_queue_fails_closed_without_mutation(void)
{
    mind_application_ingress_t ingress;
    mind_application_ingress_t before;
    mind_application_ingress_event_t taken;
    ble_mesh_sched_event_t event;
    mind_application_ingress_result_t result;

    mind_application_ingress_init(&ingress, 7u);
    ingress.queue_tail = 1u;
    before = ingress;
    make_mind_event(&event, 7u, 13u, 0x131313u);
    result = mind_application_ingress_receive(&ingress, &event, 20u);
    check(result.outcome == MIND_APPLICATION_INGRESS_INVALID_STATE &&
              result.validation == MIND_APPLICATION_INGRESS_INVALID_ARGUMENT &&
              memcmp(&ingress, &before, sizeof(ingress)) == 0,
          "recognized MIND with an inconsistent FIFO fails closed without mutation");

    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
    result = mind_application_ingress_receive(&ingress, &event, 21u);
    check(result.outcome == MIND_APPLICATION_INGRESS_PASSTHROUGH &&
              ingress.queue_head == before.queue_head &&
              ingress.queue_tail == before.queue_tail &&
              ingress.queue_count == before.queue_count &&
              memcmp(ingress.seen, before.seen, sizeof(ingress.seen)) == 0 &&
              memcmp(ingress.queue, before.queue, sizeof(ingress.queue)) == 0 &&
              ingress.counters.passed_non_mind ==
                  before.counters.passed_non_mind + 1u,
          "non-MIND RX classifies before corrupt application-state validation");

    before = ingress;
    make_mind_event(&event, 7u, 13u, 0x131313u);
    event.type = BLE_MESH_SCHED_EVENT_TX_DONE;
    result = mind_application_ingress_receive(&ingress, &event, 22u);
    check(result.outcome == MIND_APPLICATION_INGRESS_PASSTHROUGH &&
              ingress.queue_head == before.queue_head &&
              ingress.queue_tail == before.queue_tail &&
              ingress.queue_count == before.queue_count &&
              memcmp(ingress.seen, before.seen, sizeof(ingress.seen)) == 0 &&
              memcmp(ingress.queue, before.queue, sizeof(ingress.queue)) == 0 &&
              ingress.counters.passed_non_mind ==
                  before.counters.passed_non_mind + 1u,
          "MIND-shaped TX classifies before corrupt application-state validation");

    before = ingress;
    event.type = BLE_MESH_SCHED_EVENT_RADIO_FAULT;
    event.fault = BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT;
    result = mind_application_ingress_receive(&ingress, &event, 23u);
    check(result.outcome == MIND_APPLICATION_INGRESS_PASSTHROUGH &&
              ingress.queue_head == before.queue_head &&
              ingress.queue_tail == before.queue_tail &&
              ingress.queue_count == before.queue_count &&
              memcmp(ingress.seen, before.seen, sizeof(ingress.seen)) == 0 &&
              memcmp(ingress.queue, before.queue, sizeof(ingress.queue)) == 0 &&
              ingress.counters.passed_non_mind ==
                  before.counters.passed_non_mind + 1u,
          "MIND-shaped fault classifies before corrupt application-state validation");

    before = ingress;
    check(mind_application_ingress_take(&ingress, &taken) ==
              MIND_APPLICATION_INGRESS_TAKE_INVALID &&
              memcmp(&ingress, &before, sizeof(ingress)) == 0,
          "corrupt FIFO dequeue fails closed without mutation");
}

typedef struct cycle_fixture {
    mind_application_ingress_t ingress;
    ble_mesh_sched_event_t event;
    uint32_t filter_completed_at_ms;
    uint32_t router_started_at_ms;
    uint32_t mentorship_dispatches;
} cycle_fixture_t;

static routed_cycle_start_rx_result_t cycle_start_rx(void *context)
{
    routed_cycle_start_rx_result_t result;

    (void)context;
    result.returned_at_ms = 0u;
    result.status = ROUTED_CYCLE_START_RX_OK;
    return result;
}

static routed_cycle_scheduler_poll_result_t cycle_poll(void *context,
                                                        uint32_t started_at_ms)
{
    cycle_fixture_t *fixture = context;
    routed_cycle_scheduler_poll_result_t result;

    (void)started_at_ms;
    memset(&result, 0, sizeof(result));
    result.returned_at_ms = 1u;
    result.status = ROUTED_CYCLE_SCHEDULER_POLL_OK;
    result.event = fixture->event;
    return result;
}

static routed_cycle_scheduler_event_filter_result_t cycle_filter(
    void *context, const ble_mesh_sched_event_t *event, uint32_t now_ms)
{
    cycle_fixture_t *fixture = context;
    routed_cycle_scheduler_event_filter_result_t filter_result;
    mind_application_ingress_result_t result = mind_application_ingress_receive(
        &fixture->ingress, event, now_ms);

    filter_result.completed_at_ms = fixture->filter_completed_at_ms;
    filter_result.status = result.outcome == MIND_APPLICATION_INGRESS_PASSTHROUGH ?
        ROUTED_CYCLE_SCHEDULER_EVENT_PASSTHROUGH :
        ROUTED_CYCLE_SCHEDULER_EVENT_CONSUMED;
    return filter_result;
}

static tavrn_router_phase_trace_t cycle_router_event(
    void *context, const ble_mesh_sched_event_t *event, uint32_t now_ms)
{
    cycle_fixture_t *fixture = context;
    tavrn_router_phase_trace_t trace;

    (void)event;
    fixture->router_started_at_ms = now_ms;
    fixture->mentorship_dispatches++;
    memset(&trace, 0, sizeof(trace));
    trace.phase = TAVRN_ROUTER_TRACE_SCHEDULER_EVENT;
    trace.completed_at_ms = now_ms;
    trace.detail.scheduler_event.status = TAVRN_ROUTER_EVENT_IGNORED;
    return trace;
}

static tavrn_router_phase_trace_t cycle_trace(tavrn_router_trace_phase_t phase,
                                              uint32_t now_ms)
{
    tavrn_router_phase_trace_t trace;

    memset(&trace, 0, sizeof(trace));
    trace.phase = phase;
    trace.completed_at_ms = now_ms;
    if (phase == TAVRN_ROUTER_TRACE_TICK) {
        trace.detail.tick.status = AODV_STATUS_OK;
    } else if (phase == TAVRN_ROUTER_TRACE_DISPATCH) {
        trace.detail.dispatch.status = TAVRN_ROUTER_EVENT_IGNORED;
    } else {
        trace.detail.link_service.status = TAVRN_ROUTER_EVENT_IGNORED;
    }
    return trace;
}

static tavrn_router_phase_trace_t cycle_tick(void *context, uint32_t now_ms)
{
    (void)context;
    return cycle_trace(TAVRN_ROUTER_TRACE_TICK, now_ms);
}

static routed_cycle_application_request_t cycle_prepare(void *context,
                                                         uint32_t now_ms)
{
    routed_cycle_application_request_t request;

    (void)context;
    memset(&request, 0, sizeof(request));
    request.prepared_at_ms = now_ms;
    request.status = ROUTED_CYCLE_APPLICATION_DISABLED;
    return request;
}

static tavrn_router_phase_trace_t cycle_submit(void *context,
                                               const tron_application_data_t *data,
                                               uint32_t now_ms)
{
    (void)context;
    (void)data;
    return cycle_trace(TAVRN_ROUTER_TRACE_APPLICATION_SUBMIT, now_ms);
}

static tavrn_router_phase_trace_t cycle_dispatch(void *context, uint32_t now_ms)
{
    (void)context;
    return cycle_trace(TAVRN_ROUTER_TRACE_DISPATCH, now_ms);
}

static tavrn_router_phase_trace_t cycle_service(void *context, uint32_t now_ms)
{
    (void)context;
    return cycle_trace(TAVRN_ROUTER_TRACE_LINK_SERVICE, now_ms);
}

static routed_cycle_yield_result_t cycle_yield(void *context,
                                                uint32_t requested_ms,
                                                uint32_t now_ms)
{
    routed_cycle_yield_result_t result;

    (void)context;
    (void)requested_ms;
    result.completed_at_ms = now_ms;
    result.status = ROUTED_CYCLE_YIELD_OK;
    return result;
}

static routed_cycle_fault_idle_result_t cycle_fault_idle(void *context,
                                                          uint32_t now_ms)
{
    routed_cycle_fault_idle_result_t result;

    (void)context;
    result.completed_at_ms = now_ms;
    result.status = ROUTED_CYCLE_FAULT_IDLE_OK;
    return result;
}

static void configure_cycle_operations(cycle_fixture_t *fixture,
                                       routed_cycle_operations_t *operations)
{
    memset(operations, 0, sizeof(*operations));
    operations->context = fixture;
    operations->start_rx = cycle_start_rx;
    operations->scheduler_poll = cycle_poll;
    operations->scheduler_event_filter = cycle_filter;
    operations->router_scheduler_event = cycle_router_event;
    operations->router_tick = cycle_tick;
    operations->application_prepare = cycle_prepare;
    operations->router_submit = cycle_submit;
    operations->router_dispatch = cycle_dispatch;
    operations->router_link_service = cycle_service;
    operations->healthy_yield = cycle_yield;
    operations->fault_idle = cycle_fault_idle;
}

static void test_delayed_consumed_event_uses_filter_completion(void)
{
    cycle_fixture_t fixture;
    routed_cycle_t cycle;
    routed_cycle_operations_t operations;
    routed_cycle_run_result_t result;

    memset(&fixture, 0, sizeof(fixture));
    mind_application_ingress_init(&fixture.ingress, 7u);
    make_mind_event(&fixture.event, 7u, 12u, 0x121212u);
    fixture.filter_completed_at_ms = 5u;
    configure_cycle_operations(&fixture, &operations);

    check(routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
              routed_cycle_seed_startup_rx_return(&cycle, &operations) ==
              ROUTED_CYCLE_RESULT_OK &&
               routed_cycle_run_once(&cycle, &operations, 0u, &result) ==
               ROUTED_CYCLE_RUN_OK && fixture.mentorship_dispatches == 0u &&
               fixture.ingress.queue_count == 1u && result.trace_count >= 3u &&
               result.traces[2].detail.router.detail.scheduler_event.status ==
               TAVRN_ROUTER_EVENT_IGNORED &&
               result.traces[2].timing.phase_started_ms == 1u &&
               result.traces[2].timing.phase_completed_ms == 5u &&
               result.traces[2].timing.phase_elapsed_ms == 4u &&
               result.traces[2].timing.over_budget == ROUTED_CYCLE_BOOLEAN_FALSE,
          "consumed MIND RX uses filter completion for elapsed time and budget");
}

static void test_delayed_passthrough_starts_router_at_filter_completion(void)
{
    cycle_fixture_t fixture;
    routed_cycle_t cycle;
    routed_cycle_operations_t operations;
    routed_cycle_run_result_t result;

    memset(&fixture, 0, sizeof(fixture));
    mind_application_ingress_init(&fixture.ingress, 7u);
    fixture.event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
    fixture.filter_completed_at_ms = 6u;
    configure_cycle_operations(&fixture, &operations);

    check(routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
              routed_cycle_seed_startup_rx_return(&cycle, &operations) ==
                  ROUTED_CYCLE_RESULT_OK &&
              routed_cycle_run_once(&cycle, &operations, 0u, &result) ==
                  ROUTED_CYCLE_RUN_OK &&
              fixture.mentorship_dispatches == 1u &&
              fixture.router_started_at_ms == 6u && result.trace_count >= 3u &&
              result.traces[2].timing.phase_started_ms == 1u &&
              result.traces[2].timing.phase_completed_ms == 6u &&
              result.traces[2].timing.phase_elapsed_ms == 5u,
          "passthrough router phase starts at delayed filter completion");
}

static void test_delayed_filter_budget_and_invalid_time_fail_closed(void)
{
    cycle_fixture_t fixture;
    routed_cycle_t cycle;
    routed_cycle_operations_t operations;
    routed_cycle_run_result_t result;

    memset(&fixture, 0, sizeof(fixture));
    mind_application_ingress_init(&fixture.ingress, 7u);
    make_mind_event(&fixture.event, 7u, 14u, 0x141414u);
    fixture.filter_completed_at_ms = 12u;
    configure_cycle_operations(&fixture, &operations);
    check(routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
              routed_cycle_seed_startup_rx_return(&cycle, &operations) ==
                  ROUTED_CYCLE_RESULT_OK &&
              routed_cycle_run_once(&cycle, &operations, 0u, &result) ==
                  ROUTED_CYCLE_RUN_FAULT_IDLE && fixture.mentorship_dispatches == 0u &&
              result.trace_count == 4u &&
              result.traces[2].timing.phase_elapsed_ms == 11u &&
              result.traces[2].timing.over_budget == ROUTED_CYCLE_BOOLEAN_TRUE &&
              result.traces[2].timing.over_budget_phase ==
                  ROUTED_CYCLE_PHASE_SCHEDULER_EVENT,
          "delayed filter time participates in the scheduler-return budget");

    memset(&fixture, 0, sizeof(fixture));
    mind_application_ingress_init(&fixture.ingress, 7u);
    make_mind_event(&fixture.event, 7u, 15u, 0x151515u);
    fixture.filter_completed_at_ms = 0u;
    configure_cycle_operations(&fixture, &operations);
    check(routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
              routed_cycle_seed_startup_rx_return(&cycle, &operations) ==
                  ROUTED_CYCLE_RESULT_OK &&
              routed_cycle_run_once(&cycle, &operations, 0u, &result) ==
                  ROUTED_CYCLE_RUN_INVALID && fixture.mentorship_dispatches == 0u &&
              result.status == ROUTED_CYCLE_RUN_INVALID && result.trace_count == 2u,
          "filter completion before its start fails closed before router dispatch");
}

int main(void)
{
    printf("mind_application_ingress tests\n\n");
    test_accepted_duplicate_and_identity();
    test_exact_equality_and_wrap_expiry();
    test_malformed_and_foreign_are_consumed_without_poison();
    test_cache_and_queue_full_leave_keys_unseen();
    test_queue_order_copy_and_counter_saturation();
    test_non_mind_tx_and_fault_passthrough();
    test_corrupt_queue_fails_closed_without_mutation();
    test_delayed_consumed_event_uses_filter_completion();
    test_delayed_passthrough_starts_router_at_filter_completion();
    test_delayed_filter_budget_and_invalid_time_fail_closed();
    printf("\n%u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
