#include "aodv_core.h"
#include "ble_mesh_scheduler.h"
#include "routed_cycle.h"
#include "tavrn_link_v2.h"
#include "tavrn_router.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_NETWORK_ID 0x2au

static unsigned int failures;
static const char *reported[4];
static unsigned int reported_count;

static int first_for(const char *requirement)
{
    unsigned int index;

    for (index = 0u; index < reported_count; index++) {
        if (strcmp(reported[index], requirement) == 0) return 0;
    }
    reported[reported_count++] = requirement;
    return 1;
}

#define CHECK(requirement, expression) \
    do { \
        if (!(expression)) { \
            if (first_for(requirement)) { \
                printf("FAIL %s: %s:%d: assertion failed: %s\n", \
                       (requirement), __FILE__, __LINE__, #expression); \
            } \
            failures++; \
        } \
    } while (0)

static const uint8_t local_adva[TAVRN_ADVA_LEN] = {
    0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu,
};

typedef struct lifecycle_recorder {
    aodv_rreq_lifecycle_record_t records[32];
    uint8_t count;
} lifecycle_recorder_t;

typedef struct integration_fixture {
    ble_mesh_scheduler_t scheduler;
    tavrn_link_v2_t link;
    aodv_core_t core;
    tavrn_router_t router;
    lifecycle_recorder_t recorder;
} integration_fixture_t;

static tavrn_direct_peer_t make_peer(uint8_t seed)
{
    tavrn_direct_peer_t peer;

    memset(&peer, 0, sizeof(peer));
    peer.adva.bytes[0] = seed;
    peer.adva.bytes[1] = (uint8_t)(seed + 1u);
    peer.adva.bytes[2] = (uint8_t)(seed + 2u);
    peer.adva.bytes[3] = (uint8_t)(seed + 3u);
    peer.adva.bytes[4] = (uint8_t)(seed + 4u);
    peer.adva.bytes[5] = (uint8_t)(0xc0u | (seed & 0x3fu));
    peer.logical_id.width = TAVRN_IDENTITY_SID16;
    peer.logical_id.value = (uint16_t)peer.adva.bytes[0] |
        ((uint16_t)peer.adva.bytes[1] << 8);
    return peer;
}

static tron_application_data_t make_application(uint16_t destination)
{
    tron_application_data_t data;

    memset(&data, 0, sizeof(data));
    data.final_destination.width = TAVRN_IDENTITY_SID16;
    data.final_destination.value = destination;
    data.app_kind = 0x7fu;
    return data;
}

static aodv_core_config_t make_core_config(uint16_t initial_request_id)
{
    aodv_core_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_peer(local_adva[0]);
    config.local_peer.adva.bytes[0] = local_adva[0];
    config.local_peer.adva.bytes[1] = local_adva[1];
    config.local_peer.adva.bytes[2] = local_adva[2];
    config.local_peer.adva.bytes[3] = local_adva[3];
    config.local_peer.adva.bytes[4] = local_adva[4];
    config.local_peer.adva.bytes[5] = local_adva[5];
    config.local_peer.logical_id.value = (uint16_t)local_adva[0] |
        ((uint16_t)local_adva[1] << 8);
    config.network_id = TEST_NETWORK_ID;
    config.net_diameter = 15u;
    config.rreq_retries = 2u;
    config.rreq_rate_per_second = 10u;
    config.rerr_rate_per_second = 10u;
    config.request_rrep_ack = 1u;
    config.initial_origin_sequence = 1u;
    config.initial_request_id = initial_request_id;
    config.initial_rerr_sequence = 1u;
    config.node_traversal_ms = 10u;
    config.path_discovery_ms = 600u;
    config.rreq_seen_ms = 10000u;
    config.active_route_ms = 36000u;
    config.pending_data_ms = 5000u;
    config.blacklist_ms = 600u;
    config.rrep_dedupe_ms = 10000u;
    config.rerr_dedupe_ms = 10000u;
    config.rrep_ack_wait_ms = 250u;
    return config;
}

static tavrn_link_config_t make_link_config(void)
{
    tavrn_link_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_core_config(1u).local_peer;
    config.network_id = TEST_NETWORK_ID;
    config.hack_max_attempts = 3u;
    config.busy_max_responses = 3u;
    config.hack_response_ms = 250u;
    config.hack_turnaround_ms = 8u;
    config.retry_backoff_ms = 0u;
    config.busy_backoff_ms = 500u;
    config.data_forward_deadline_ms = 5000u;
    config.candidate_resolve_ms = 10u;
    config.data_dedupe_ms = 10000u;
    config.flood_dedupe_ms = 10000u;
    config.flood_jitter_min_ms = 20u;
    config.flood_jitter_max_ms = 120u;
    return config;
}

static void record_lifecycle(void *context,
                             const aodv_rreq_lifecycle_record_t *record)
{
    lifecycle_recorder_t *recorder = context;

    if (recorder != NULL && record != NULL && recorder->count < 32u) {
        recorder->records[recorder->count++] = *record;
    }
}

static int setup_with_hooks(
    integration_fixture_t *fixture, uint16_t initial_request_id,
    const tavrn_router_augmentation_hooks_t *augmentation_or_null)
{
    aodv_core_config_t core_config = make_core_config(initial_request_id);
    tavrn_link_config_t link_config = make_link_config();
    aodv_rreq_telemetry_config_t telemetry;

    memset(fixture, 0, sizeof(*fixture));
    ble_mesh_scheduler_init(&fixture->scheduler, 0u, local_adva);
    memset(&telemetry, 0, sizeof(telemetry));
    telemetry.context = &fixture->recorder;
    telemetry.on_lifecycle = record_lifecycle;
    return tavrn_link_v2_init(&fixture->link, &fixture->scheduler, &link_config, 0u) ==
               TAVRN_LINK_INIT_OK &&
        aodv_core_init(&fixture->core, &core_config, 0u) == AODV_INIT_OK &&
        tavrn_router_init(&fixture->router, &fixture->link, &fixture->core,
                          augmentation_or_null) == TAVRN_ROUTER_INIT_OK &&
        aodv_core_set_rreq_telemetry(&fixture->core, &telemetry) ==
            AODV_RREQ_TELEMETRY_OK;
}

static int setup(integration_fixture_t *fixture, uint16_t initial_request_id)
{
    return setup_with_hooks(fixture, initial_request_id, NULL);
}

static int poll_rreq(aodv_core_t *core, uint8_t scope, aodv_action_t *action_out)
{
    aodv_action_t action;

    memset(&action, 0, sizeof(action));
    if (aodv_core_poll_action(core, &action) != AODV_ACTION_POLL_OK ||
        action.type != AODV_ACTION_SEND_RREQ ||
        (action.detail.control.control.pdu[6] >> 4) != scope) {
        return 0;
    }
    if (action_out != NULL) *action_out = action;
    return 1;
}

static int lifecycle_matches(const aodv_rreq_lifecycle_record_t *record,
                             aodv_rreq_scope_source_t scope_source,
                             uint8_t initial_scope, uint8_t current_scope,
                             uint8_t ring_ordinal, aodv_rreq_truth_t ordinary,
                             aodv_rreq_truth_t scoped,
                             aodv_rreq_truth_t hint_used,
                             aodv_rreq_truth_t fallback,
                             aodv_rreq_truth_t full_diameter)
{
    return record != NULL && record->stage == AODV_RREQ_STAGE_ACTION_CREATED &&
        record->attempt.discovery_correlation != 0u &&
        record->attempt.initial_scope == initial_scope &&
        record->attempt.current_scope == current_scope &&
        record->attempt.ring_ordinal == ring_ordinal &&
        record->scope_source == scope_source &&
        record->action_created_at_state == AODV_RREQ_VALUE_KNOWN &&
        record->link_enqueue_at_state == AODV_RREQ_VALUE_NOT_APPLICABLE &&
        record->action_outcome == AODV_RREQ_ACTION_CREATED &&
        record->enqueue_outcome == AODV_RREQ_ENQUEUE_NOT_APPLICABLE &&
        record->ordinary == ordinary && record->scoped == scoped &&
        record->hint_used == hint_used && record->fallback == fallback &&
        record->full_diameter == full_diameter;
}

static tavrn_router_scope_hint_t give_scope_four(void *context,
                                                  const tavrn_logical_id_t *destination,
                                                  uint8_t full_scope,
                                                  uint32_t now_ms)
{
    tavrn_router_scope_hint_t hint;

    (void)context;
    (void)destination;
    (void)full_scope;
    (void)now_ms;
    hint.has_hint = 1u;
    hint.initial_scope = 4u;
    return hint;
}

static int enqueue_control_fillers(ble_mesh_scheduler_t *scheduler)
{
    ble_mesh_tx_item_t item;
    uint8_t index;

    memset(&item, 0, sizeof(item));
    item.adv_len = 1u;
    item.adv_data[0] = 0x5au;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = BLE_MESH_TX_PRIORITY_CONTROL;
    item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
    item.expiry_ms = 0x7fffffffu;
    item.sweep_count = BLE_MESH_TX_SWEEP_COUNT_ONE;
    item.budget_class = BLE_MESH_TX_BUDGET_GENERAL;
    item.token = BLE_MESH_TX_TOKEN_NONE;
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (ble_mesh_scheduler_enqueue_ex(scheduler, &item).status !=
            BLE_MESH_SCHED_ENQUEUE_OK) return 0;
    }
    return 1;
}

static routed_cycle_start_rx_result_t cycle_start_rx(void *context)
{
    routed_cycle_start_rx_result_t result;

    (void)context;
    result.returned_at_ms = 0u;
    result.status = ROUTED_CYCLE_START_RX_OK;
    return result;
}

static routed_cycle_scheduler_poll_result_t cycle_scheduler_poll(
    void *context, uint32_t poll_started_at_ms)
{
    routed_cycle_scheduler_poll_result_t result;

    (void)context;
    memset(&result, 0, sizeof(result));
    result.returned_at_ms = poll_started_at_ms;
    result.status = ROUTED_CYCLE_SCHEDULER_POLL_OK;
    result.event.fault = BLE_MESH_SCHED_FAULT_NONE;
    return result;
}

static tavrn_router_phase_trace_t cycle_router_event(
    void *context, const ble_mesh_sched_event_t *event, uint32_t now_ms)
{
    integration_fixture_t *fixture = context;
    tavrn_router_phase_trace_t trace;

    memset(&trace, 0, sizeof(trace));
    (void)tavrn_router_handle_scheduler_event_ex(&fixture->router, event, now_ms,
                                                 &trace);
    return trace;
}

static tavrn_router_phase_trace_t cycle_router_tick(void *context, uint32_t now_ms)
{
    integration_fixture_t *fixture = context;
    tavrn_router_phase_trace_t trace;

    memset(&trace, 0, sizeof(trace));
    (void)tavrn_router_tick_ex(&fixture->router, now_ms, &trace);
    return trace;
}

static routed_cycle_application_request_t cycle_application_prepare(
    void *context, uint32_t now_ms)
{
    routed_cycle_application_request_t request;

    (void)context;
    memset(&request, 0, sizeof(request));
    request.prepared_at_ms = now_ms;
    request.status = ROUTED_CYCLE_APPLICATION_DISABLED;
    return request;
}

static tavrn_router_phase_trace_t cycle_router_submit(
    void *context, const tron_application_data_t *data, uint32_t now_ms)
{
    integration_fixture_t *fixture = context;
    tavrn_router_phase_trace_t trace;

    memset(&trace, 0, sizeof(trace));
    (void)tavrn_router_submit_application_ex(&fixture->router, data, now_ms, &trace);
    return trace;
}

static tavrn_router_phase_trace_t cycle_router_dispatch(void *context,
                                                         uint32_t now_ms)
{
    integration_fixture_t *fixture = context;
    tavrn_router_phase_trace_t trace;

    memset(&trace, 0, sizeof(trace));
    (void)tavrn_router_dispatch_trace_ex(&fixture->router, now_ms, &trace);
    return trace;
}

static tavrn_router_phase_trace_t cycle_router_service(void *context,
                                                        uint32_t now_ms)
{
    integration_fixture_t *fixture = context;
    tavrn_router_phase_trace_t trace;

    memset(&trace, 0, sizeof(trace));
    (void)tavrn_router_service_link_ex(&fixture->router, now_ms, &trace);
    return trace;
}

static routed_cycle_yield_result_t cycle_yield(
    void *context, uint32_t requested_slack_ms, uint32_t now_ms)
{
    routed_cycle_yield_result_t result;

    (void)context;
    result.completed_at_ms = now_ms + requested_slack_ms;
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

static routed_cycle_operations_t cycle_operations(integration_fixture_t *fixture)
{
    routed_cycle_operations_t operations;

    memset(&operations, 0, sizeof(operations));
    operations.context = fixture;
    operations.start_rx = cycle_start_rx;
    operations.scheduler_poll = cycle_scheduler_poll;
    operations.router_scheduler_event = cycle_router_event;
    operations.router_tick = cycle_router_tick;
    operations.application_prepare = cycle_application_prepare;
    operations.router_submit = cycle_router_submit;
    operations.router_dispatch = cycle_router_dispatch;
    operations.router_link_service = cycle_router_service;
    operations.healthy_yield = cycle_yield;
    operations.fault_idle = cycle_fault_idle;
    return operations;
}

static const routed_cycle_trace_t *cycle_trace_for_phase(
    const routed_cycle_run_result_t *result, routed_cycle_phase_t phase)
{
    uint8_t index;

    if (result == NULL) return NULL;
    for (index = 0u; index < result->trace_count; index++) {
        if (result->traces[index].phase == phase) return &result->traces[index];
    }
    return NULL;
}

static void test_aodv_02_action_and_enqueue_reports(void)
{
    integration_fixture_t fixture;
    tron_application_data_t application = make_application(0x4bdcu);
    aodv_action_t action;
    aodv_rreq_link_enqueue_t enqueue;
    aodv_core_t core_before;
    uint8_t recorder_before;

    if (!setup(&fixture, 0xfffeu)) {
        CHECK("AODV-02", 0);
        return;
    }
    CHECK("AODV-02", aodv_core_submit_application(&fixture.core, &application, 0u) ==
                           AODV_STATUS_QUEUED && poll_rreq(&fixture.core, 1u, &action) &&
                           action.detail.control.rreq_attempt_present ==
                               AODV_RREQ_ATTEMPT_PRESENT &&
                           action.detail.control.rreq_attempt.discovery_correlation != 0u &&
                           action.detail.control.rreq_attempt.request_id == 0xfffeu &&
                           fixture.recorder.count == 1u &&
                           fixture.recorder.records[0].stage ==
                               AODV_RREQ_STAGE_ACTION_CREATED &&
                           fixture.recorder.records[0].attempt.request_id == 0xfffeu);
    memset(&enqueue, 0, sizeof(enqueue));
    enqueue.attempt = action.detail.control.rreq_attempt;
    enqueue.link_enqueue_at_ms = 1u;
    enqueue.outcome = AODV_RREQ_ENQUEUE_NOT_ADMITTED;
    CHECK("AODV-04", aodv_core_report_rreq_link_enqueue(&fixture.core, &enqueue) ==
                           AODV_RREQ_TELEMETRY_OK && fixture.recorder.count == 2u &&
                           fixture.recorder.records[1].stage ==
                               AODV_RREQ_STAGE_ROUTER_LINK_ENQUEUE &&
                           fixture.recorder.records[1].enqueue_outcome ==
                               AODV_RREQ_ENQUEUE_NOT_ADMITTED &&
                           fixture.recorder.records[1].attempt.request_id == 0xfffeu);
    core_before = fixture.core;
    recorder_before = fixture.recorder.count;
    CHECK("AODV-04", aodv_core_report_rreq_link_enqueue(&fixture.core, &enqueue) ==
                           AODV_RREQ_TELEMETRY_INVALID &&
                           memcmp(&fixture.core, &core_before, sizeof(fixture.core)) == 0 &&
                           fixture.recorder.count == recorder_before);
    enqueue.attempt.request_id++;
    CHECK("AODV-04", aodv_core_report_rreq_link_enqueue(&fixture.core, &enqueue) ==
                           AODV_RREQ_TELEMETRY_INVALID &&
                           memcmp(&fixture.core, &core_before, sizeof(fixture.core)) == 0 &&
                           fixture.recorder.count == recorder_before);
}

static void test_aodv_04_router_admission_retry(void)
{
    integration_fixture_t fixture;
    tron_application_data_t application = make_application(0x4bdcu);
    tavrn_router_phase_trace_t rejected_trace;
    tavrn_router_phase_trace_t admitted_trace;

    if (!setup(&fixture, 1u) || !enqueue_control_fillers(&fixture.scheduler)) {
        CHECK("AODV-04", 0);
        return;
    }
    CHECK("AODV-04", tavrn_router_submit_application(&fixture.router, &application, 0u) ==
                           AODV_STATUS_QUEUED &&
                           tavrn_router_dispatch_trace_ex(&fixture.router, 0u,
                                                          &rejected_trace) ==
                               TAVRN_ROUTER_EVENT_BUSY && fixture.recorder.count == 2u &&
                           rejected_trace.phase == TAVRN_ROUTER_TRACE_DISPATCH &&
                           rejected_trace.detail.dispatch.action_present ==
                               TAVRN_ROUTER_TRACE_PRESENT &&
                           rejected_trace.detail.dispatch.action.type == AODV_ACTION_SEND_RREQ &&
                           rejected_trace.detail.dispatch.rreq_enqueue_present ==
                               TAVRN_ROUTER_TRACE_PRESENT &&
                           fixture.recorder.records[1].stage ==
                               AODV_RREQ_STAGE_ROUTER_LINK_ENQUEUE &&
                           fixture.recorder.records[1].enqueue_outcome ==
                               AODV_RREQ_ENQUEUE_NOT_ADMITTED &&
                           rejected_trace.detail.dispatch.rreq_enqueue.outcome ==
                               AODV_RREQ_ENQUEUE_NOT_ADMITTED &&
                           rejected_trace.detail.dispatch.rreq_enqueue.attempt.request_id ==
                               fixture.recorder.records[1].attempt.request_id);
    ble_mesh_scheduler_init(&fixture.scheduler, 1u, local_adva);
    CHECK("AODV-04", tavrn_router_dispatch_trace_ex(&fixture.router, 1u,
                                                      &admitted_trace) ==
                           TAVRN_ROUTER_EVENT_OK && fixture.recorder.count == 3u &&
                           admitted_trace.phase == TAVRN_ROUTER_TRACE_DISPATCH &&
                           admitted_trace.detail.dispatch.action_present ==
                               TAVRN_ROUTER_TRACE_PRESENT &&
                           admitted_trace.detail.dispatch.rreq_enqueue_present ==
                               TAVRN_ROUTER_TRACE_PRESENT &&
                           fixture.recorder.records[2].stage ==
                               AODV_RREQ_STAGE_ROUTER_LINK_ENQUEUE &&
                           fixture.recorder.records[2].enqueue_outcome ==
                               AODV_RREQ_ENQUEUE_ADMITTED &&
                           admitted_trace.detail.dispatch.rreq_enqueue.outcome ==
                               AODV_RREQ_ENQUEUE_ADMITTED &&
                           fixture.recorder.records[1].attempt.discovery_correlation ==
                               fixture.recorder.records[2].attempt.discovery_correlation &&
                           fixture.recorder.records[1].attempt.request_id ==
                               fixture.recorder.records[2].attempt.request_id &&
                           rejected_trace.detail.dispatch.rreq_enqueue.attempt.request_id ==
                               admitted_trace.detail.dispatch.rreq_enqueue.attempt.request_id);
}

static void test_aodv_04_cycle_router_dispatch_trace(void)
{
    integration_fixture_t fixture;
    tron_application_data_t application = make_application(0x4bdcu);
    routed_cycle_operations_t operations;
    routed_cycle_t cycle;
    routed_cycle_run_result_t result;
    const routed_cycle_trace_t *dispatch_trace;
    routed_cycle_trace_t mismatched_trace;
    routed_cycle_diagnostic_queue_t queue;
    routed_cycle_diagnostic_queue_t queue_before;

    if (!setup(&fixture, 1u)) {
        CHECK("AODV-04", 0);
        return;
    }
    operations = cycle_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    if (routed_cycle_init(&cycle, 10u) != ROUTED_CYCLE_RESULT_OK ||
        tavrn_router_submit_application(&fixture.router, &application, 0u) !=
            AODV_STATUS_QUEUED ||
        routed_cycle_seed_startup_rx_return(&cycle, &operations) !=
            ROUTED_CYCLE_RESULT_OK) {
        CHECK("AODV-04", 0);
        return;
    }
    CHECK("AODV-04", routed_cycle_run_once(&cycle, &operations, 1u, &result) ==
                           ROUTED_CYCLE_RUN_OK && fixture.recorder.count == 2u);
    dispatch_trace = cycle_trace_for_phase(&result,
                                           ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH);
    CHECK("AODV-04", dispatch_trace != NULL &&
                           dispatch_trace->detail.router.phase ==
                               TAVRN_ROUTER_TRACE_DISPATCH &&
                           dispatch_trace->detail.router.detail.dispatch.action_present ==
                               TAVRN_ROUTER_TRACE_PRESENT &&
                           dispatch_trace->detail.router.detail.dispatch.action.type ==
                               AODV_ACTION_SEND_RREQ &&
                           dispatch_trace->detail.router.detail.dispatch.action.detail.control
                               .rreq_attempt_present == AODV_RREQ_ATTEMPT_PRESENT &&
                           dispatch_trace->detail.router.detail.dispatch.rreq_enqueue_present ==
                               TAVRN_ROUTER_TRACE_PRESENT &&
                           dispatch_trace->detail.router.detail.dispatch.link_send_present ==
                               TAVRN_ROUTER_TRACE_PRESENT &&
                           dispatch_trace->detail.router.detail.dispatch.link_send_status ==
                               TAVRN_LINK_SEND_OK &&
                           dispatch_trace->detail.router.detail.dispatch.rreq_enqueue.attempt
                               .discovery_correlation == fixture.recorder.records[0].attempt
                                   .discovery_correlation &&
                           dispatch_trace->detail.router.detail.dispatch.rreq_enqueue.attempt
                               .request_id == fixture.recorder.records[1].attempt.request_id &&
                           dispatch_trace->detail.router.detail.dispatch.action.detail.control
                               .rreq_attempt.request_id == dispatch_trace->detail.router.detail
                                   .dispatch.rreq_enqueue.attempt.request_id &&
                           fixture.recorder.records[1].stage ==
                               AODV_RREQ_STAGE_ROUTER_LINK_ENQUEUE &&
                           fixture.recorder.records[1].enqueue_outcome ==
                               AODV_RREQ_ENQUEUE_ADMITTED);

    if (dispatch_trace == NULL) {
        return;
    }
    routed_cycle_diagnostic_queue_init(&queue);
    CHECK("AODV-04", routed_cycle_diagnostic_enqueue(&queue, dispatch_trace) ==
                           ROUTED_CYCLE_RESULT_OK);
    mismatched_trace = *dispatch_trace;
    mismatched_trace.detail.router.detail.dispatch.rreq_enqueue.attempt.request_id++;
    queue_before = queue;
    CHECK("AODV-04", routed_cycle_diagnostic_enqueue(&queue, &mismatched_trace) ==
                           ROUTED_CYCLE_RESULT_INVALID &&
                           memcmp(&queue, &queue_before, sizeof(queue)) == 0);
}

static void test_gtt_06_rings_hints_capacity_and_rediscovery(void)
{
    integration_fixture_t fixture;
    tron_application_data_t first = make_application(0x4bdcu);
    tron_application_data_t second = make_application(0x4cdeu);
    aodv_action_t action[3];
    aodv_action_t expired;
    uint8_t index;
    uint32_t first_correlation;

    if (!setup(&fixture, 0xfffeu)) {
        CHECK("GTT-06", 0);
        return;
    }
    CHECK("GTT-06", aodv_core_submit_application_scoped_ex(
                         &fixture.core, &first, 4u, AODV_RREQ_SCOPE_ROUTER_HINT, 0u) ==
                         AODV_STATUS_QUEUED && poll_rreq(&fixture.core, 4u, &action[0]) &&
                         aodv_core_tick(&fixture.core, 600u) == AODV_STATUS_OK &&
                         poll_rreq(&fixture.core, 15u, &action[1]) &&
                         aodv_core_tick(&fixture.core, 1200u) == AODV_STATUS_OK &&
                         poll_rreq(&fixture.core, 15u, &action[2]) &&
                          action[0].detail.control.rreq_attempt.request_id == 0xfffeu &&
                          action[1].detail.control.rreq_attempt.request_id == 0xffffu &&
                          action[2].detail.control.rreq_attempt.request_id == 1u &&
                          action[0].detail.control.rreq_attempt.discovery_correlation != 0u &&
                          action[0].detail.control.rreq_attempt.discovery_correlation ==
                              action[1].detail.control.rreq_attempt.discovery_correlation &&
                          action[1].detail.control.rreq_attempt.discovery_correlation ==
                              action[2].detail.control.rreq_attempt.discovery_correlation &&
                          fixture.recorder.count == 3u &&
                          fixture.recorder.records[0].attempt.discovery_correlation ==
                              fixture.recorder.records[1].attempt.discovery_correlation &&
                          fixture.recorder.records[1].attempt.discovery_correlation ==
                              fixture.recorder.records[2].attempt.discovery_correlation &&
                          lifecycle_matches(&fixture.recorder.records[0],
                                            AODV_RREQ_SCOPE_ROUTER_HINT,
                                            4u, 4u, 0u, AODV_RREQ_TRUTH_FALSE,
                                            AODV_RREQ_TRUTH_TRUE,
                                            AODV_RREQ_TRUTH_TRUE,
                                            AODV_RREQ_TRUTH_FALSE,
                                            AODV_RREQ_TRUTH_FALSE) &&
                          lifecycle_matches(&fixture.recorder.records[1],
                                            AODV_RREQ_SCOPE_ROUTER_HINT,
                                            4u, 15u, 1u, AODV_RREQ_TRUTH_FALSE,
                                            AODV_RREQ_TRUTH_TRUE,
                                            AODV_RREQ_TRUTH_FALSE,
                                            AODV_RREQ_TRUTH_TRUE,
                                            AODV_RREQ_TRUTH_TRUE) &&
                          lifecycle_matches(&fixture.recorder.records[2],
                                            AODV_RREQ_SCOPE_ROUTER_HINT,
                                            4u, 15u, 2u, AODV_RREQ_TRUTH_FALSE,
                                            AODV_RREQ_TRUTH_TRUE,
                                            AODV_RREQ_TRUTH_FALSE,
                                            AODV_RREQ_TRUTH_FALSE,
                                            AODV_RREQ_TRUTH_TRUE));
    first_correlation = action[0].detail.control.rreq_attempt.discovery_correlation;
    for (index = 3u; index < 6u; index++) {
        (void)aodv_core_tick(&fixture.core, (uint32_t)index * 600u);
        (void)poll_rreq(&fixture.core, 15u, NULL);
    }
    (void)aodv_core_tick(&fixture.core, 5001u);
    (void)aodv_core_poll_action(&fixture.core, &expired);
    CHECK("AODV-02", aodv_core_submit_application(&fixture.core, &first, 5002u) ==
                           AODV_STATUS_QUEUED && poll_rreq(&fixture.core, 1u, &action[0]) &&
                           action[0].detail.control.rreq_attempt.discovery_correlation != 0u &&
                           action[0].detail.control.rreq_attempt.discovery_correlation !=
                               first_correlation);

    if (!setup(&fixture, 1u)) {
        CHECK("AODV-02", 0);
        return;
    }
    CHECK("AODV-02", aodv_core_submit_application(&fixture.core, &first, 0u) ==
                           AODV_STATUS_QUEUED && poll_rreq(&fixture.core, 1u, &action[0]) &&
                           aodv_core_submit_application(&fixture.core, &second, 1u) ==
                               AODV_STATUS_QUEUED && poll_rreq(&fixture.core, 1u, &action[1]) &&
                           action[0].detail.control.rreq_attempt.destination.value !=
                               action[1].detail.control.rreq_attempt.destination.value &&
                            action[0].detail.control.rreq_attempt.discovery_correlation !=
                               action[1].detail.control.rreq_attempt.discovery_correlation);

    if (!setup(&fixture, 1u)) {
        CHECK("GTT-06", 0);
        return;
    }
    {
        static const uint8_t scopes[] = { 1u, 3u, 5u, 7u, 15u };
        aodv_action_t ordinary_actions[sizeof(scopes) / sizeof(scopes[0])];
        int ordinary_ok = aodv_core_submit_application(&fixture.core, &first, 0u) ==
            AODV_STATUS_QUEUED;

        for (index = 0u; index < sizeof(scopes) / sizeof(scopes[0]); index++) {
            if (!poll_rreq(&fixture.core, scopes[index], &ordinary_actions[index])) {
                ordinary_ok = 0;
                break;
            }
            if (index + 1u < sizeof(scopes) / sizeof(scopes[0]) &&
                aodv_core_tick(&fixture.core, (uint32_t)(index + 1u) * 600u) !=
                    AODV_STATUS_OK) {
                ordinary_ok = 0;
                break;
            }
        }
        CHECK("GTT-06", ordinary_ok != 0 && fixture.recorder.count == 5u &&
                             ordinary_actions[0].detail.control.rreq_attempt
                                 .discovery_correlation != 0u &&
                             ordinary_actions[0].detail.control.rreq_attempt
                                 .discovery_correlation ==
                                 ordinary_actions[1].detail.control.rreq_attempt
                                     .discovery_correlation &&
                             ordinary_actions[1].detail.control.rreq_attempt
                                 .discovery_correlation ==
                                 ordinary_actions[2].detail.control.rreq_attempt
                                     .discovery_correlation &&
                             ordinary_actions[2].detail.control.rreq_attempt
                                 .discovery_correlation ==
                                 ordinary_actions[3].detail.control.rreq_attempt
                                     .discovery_correlation &&
                             ordinary_actions[3].detail.control.rreq_attempt
                                 .discovery_correlation ==
                                 ordinary_actions[4].detail.control.rreq_attempt
                                     .discovery_correlation &&
                             fixture.recorder.records[0].attempt.discovery_correlation ==
                                 fixture.recorder.records[4].attempt.discovery_correlation &&
                             lifecycle_matches(&fixture.recorder.records[0],
                                               AODV_RREQ_SCOPE_ORDINARY,
                                               1u, 1u, 0u, AODV_RREQ_TRUTH_TRUE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_FALSE) &&
                             lifecycle_matches(&fixture.recorder.records[1],
                                               AODV_RREQ_SCOPE_ORDINARY,
                                               1u, 3u, 1u, AODV_RREQ_TRUTH_TRUE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_FALSE) &&
                             lifecycle_matches(&fixture.recorder.records[2],
                                               AODV_RREQ_SCOPE_ORDINARY,
                                               1u, 5u, 2u, AODV_RREQ_TRUTH_TRUE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_FALSE) &&
                             lifecycle_matches(&fixture.recorder.records[3],
                                               AODV_RREQ_SCOPE_ORDINARY,
                                               1u, 7u, 3u, AODV_RREQ_TRUTH_TRUE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_FALSE) &&
                             lifecycle_matches(&fixture.recorder.records[4],
                                               AODV_RREQ_SCOPE_ORDINARY,
                                               1u, 15u, 4u, AODV_RREQ_TRUTH_TRUE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_FALSE,
                                               AODV_RREQ_TRUTH_TRUE));
    }
}

static void test_gtt_06_router_hint_provenance(void)
{
    integration_fixture_t fixture;
    tavrn_router_augmentation_hooks_t hooks;
    tron_application_data_t application = make_application(0x4bdcu);
    aodv_action_t action;

    memset(&hooks, 0, sizeof(hooks));
    hooks.initial_scope = give_scope_four;
    if (!setup_with_hooks(&fixture, 1u, &hooks)) {
        CHECK("GTT-06", 0);
        return;
    }
    CHECK("GTT-06", tavrn_router_submit_application(&fixture.router, &application, 0u) ==
                         AODV_STATUS_QUEUED && poll_rreq(&fixture.core, 4u, &action) &&
                         fixture.recorder.count == 1u &&
                         lifecycle_matches(&fixture.recorder.records[0],
                                           AODV_RREQ_SCOPE_ROUTER_HINT,
                                           4u, 4u, 0u, AODV_RREQ_TRUTH_FALSE,
                                           AODV_RREQ_TRUTH_TRUE,
                                           AODV_RREQ_TRUTH_TRUE,
                                           AODV_RREQ_TRUTH_FALSE,
                                           AODV_RREQ_TRUTH_FALSE));
}

static void test_aodv_02_action_capacity(void)
{
    integration_fixture_t fixture;
    tron_application_data_t overflow = make_application(0x4c00u);
    uint8_t index;
    int admitted = 1;

    if (!setup(&fixture, 1u)) {
        CHECK("AODV-02", 0);
        return;
    }
    for (index = 0u; index < TAVRN_AODV_ACTION_CAPACITY; index++) {
        tron_application_data_t application = make_application(
            (uint16_t)(0x4b00u + index));

        if (aodv_core_submit_application(&fixture.core, &application, index) !=
            AODV_STATUS_QUEUED) admitted = 0;
    }
    CHECK("AODV-02", admitted != 0 && fixture.recorder.count ==
                           TAVRN_AODV_ACTION_CAPACITY &&
                           aodv_core_submit_application(&fixture.core,
                                                        &overflow,
                                                        20u) == AODV_STATUS_BUSY);
}

int main(void)
{
    test_aodv_02_action_and_enqueue_reports();
    test_aodv_04_router_admission_retry();
    test_aodv_04_cycle_router_dispatch_trace();
    test_gtt_06_rings_hints_capacity_and_rediscovery();
    test_gtt_06_router_hint_provenance();
    test_aodv_02_action_capacity();
    if (failures != 0u) {
        printf("tavrn_phase4_rreq RED tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_phase4_rreq tests passed\n");
    return 0;
}
