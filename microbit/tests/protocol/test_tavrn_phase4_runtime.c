#ifdef TAVRN_GTT_H
#error "AODV_ONLY cycle contract must not pre-include tavrn_gtt.h"
#endif
#ifdef TAVRN_FULL_H
#error "AODV_ONLY cycle contract must not pre-include tavrn_full.h"
#endif

#include "routed_cycle.h"

#ifdef TAVRN_GTT_H
#error "routed_cycle.h must not include tavrn_gtt.h"
#endif
#ifdef TAVRN_FULL_H
#error "routed_cycle.h must not include tavrn_full.h"
#endif

#include "routed_full_telemetry.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;
static const char *reported[10];
static unsigned int reported_count;

static int first_for(const char *requirement)
{
    unsigned int index;

    for (index = 0u; index < reported_count; index++) {
        if (strcmp(reported[index], requirement) == 0) {
            return 0;
        }
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

static tavrn_adva_t random_static_adva(uint8_t seed)
{
    tavrn_adva_t identity;

    identity.bytes[0] = seed;
    identity.bytes[1] = (uint8_t)(seed + 1u);
    identity.bytes[2] = (uint8_t)(seed + 2u);
    identity.bytes[3] = (uint8_t)(seed + 3u);
    identity.bytes[4] = (uint8_t)(seed + 4u);
    identity.bytes[5] = (uint8_t)(0xc0u | (seed & 0x3fu));
    return identity;
}

static int adva_equal(const tavrn_adva_t *left, const tavrn_adva_t *right)
{
    return left != NULL && right != NULL &&
        memcmp(left->bytes, right->bytes, TAVRN_ADVA_LEN) == 0;
}

static tavrn_logical_id_t sid16(uint16_t value)
{
    tavrn_logical_id_t id;

    id.width = TAVRN_IDENTITY_SID16;
    id.value = value;
    return id;
}

enum cycle_call {
    CYCLE_CALL_START_RX = 1,
    CYCLE_CALL_POLL,
    CYCLE_CALL_EVENT,
    CYCLE_CALL_TICK,
    CYCLE_CALL_PREPARE,
    CYCLE_CALL_SUBMIT,
    CYCLE_CALL_DISPATCH,
    CYCLE_CALL_SERVICE,
    CYCLE_CALL_YIELD,
    CYCLE_CALL_FAULT_IDLE,
};

typedef struct cycle_fixture {
    enum cycle_call calls[32];
    uint8_t call_count;
    uint8_t poll_count;
    uint32_t offset_ms;
    uint32_t poll_offset_step_ms;
    uint32_t task_now_ms;
    uint32_t task_now_step_ms;
    routed_cycle_start_rx_result_t start_rx;
    routed_cycle_scheduler_poll_result_t poll;
    tavrn_router_phase_trace_t event;
    tavrn_router_phase_trace_t tick;
    routed_cycle_application_request_t application;
    tavrn_router_phase_trace_t submit;
    tavrn_router_phase_trace_t dispatch;
    tavrn_router_phase_trace_t service;
    routed_cycle_yield_result_t yield;
    routed_cycle_fault_idle_result_t fault_idle;
    uint32_t requested_yield_ms;
    uint32_t due_sweep_at_ms;
    uint32_t due_sweep_next_due_ms;
    uint8_t due_sweep_enabled;
    uint8_t due_sweep_passes;
    uint8_t scheduler_return_requested;
    uint8_t scheduler_return_request_takes;
    routed_cycle_trace_t sink_traces[ROUTED_CYCLE_TRACE_CAPACITY * 3u];
    uint8_t sink_count;
} cycle_fixture_t;

static void record_call(cycle_fixture_t *fixture, enum cycle_call call)
{
    if (fixture != NULL && fixture->call_count < sizeof(fixture->calls) /
                                            sizeof(fixture->calls[0])) {
        fixture->calls[fixture->call_count++] = call;
    }
}

static tavrn_router_phase_trace_t make_router_trace(
    tavrn_router_trace_phase_t phase, uint32_t completed_at_ms)
{
    tavrn_router_phase_trace_t trace;

    memset(&trace, 0, sizeof(trace));
    trace.phase = phase;
    trace.completed_at_ms = completed_at_ms;
    return trace;
}

static tavrn_router_phase_trace_t offset_trace(const tavrn_router_phase_trace_t *trace,
                                                uint32_t offset_ms)
{
    tavrn_router_phase_trace_t result = *trace;

    result.completed_at_ms += offset_ms;
    return result;
}

static routed_cycle_start_rx_result_t fixture_start_rx(void *context)
{
    cycle_fixture_t *fixture = context;

    record_call(fixture, CYCLE_CALL_START_RX);
    return fixture->start_rx;
}

static routed_cycle_scheduler_poll_result_t fixture_poll(
    void *context, uint32_t poll_started_at_ms)
{
    cycle_fixture_t *fixture = context;
    routed_cycle_scheduler_poll_result_t result = fixture->poll;

    (void)poll_started_at_ms;
    fixture->offset_ms = (uint32_t)fixture->poll_count * fixture->poll_offset_step_ms;
    fixture->poll_count++;
    result.returned_at_ms += fixture->offset_ms;
    record_call(fixture, CYCLE_CALL_POLL);
    return result;
}

static tavrn_router_phase_trace_t fixture_event(
    void *context, const ble_mesh_sched_event_t *event, uint32_t now_ms)
{
    cycle_fixture_t *fixture = context;

    (void)event;
    (void)now_ms;
    record_call(fixture, CYCLE_CALL_EVENT);
    return offset_trace(&fixture->event, fixture->offset_ms);
}

static tavrn_router_phase_trace_t fixture_tick(void *context, uint32_t now_ms)
{
    cycle_fixture_t *fixture = context;

    if (fixture->due_sweep_enabled != 0u &&
        now_ms == fixture->due_sweep_at_ms && fixture->due_sweep_passes == 0u) {
        fixture->due_sweep_passes = 1u;
        fixture->due_sweep_next_due_ms = now_ms + 10u;
        fixture->scheduler_return_requested = 1u;
    }
    record_call(fixture, CYCLE_CALL_TICK);
    return offset_trace(&fixture->tick, fixture->offset_ms);
}

static uint8_t fixture_take_scheduler_return_request(void *context)
{
    cycle_fixture_t *fixture = context;
    uint8_t requested = fixture->scheduler_return_requested;

    fixture->scheduler_return_requested = 0u;
    fixture->scheduler_return_request_takes++;
    return requested;
}

static routed_cycle_application_request_t fixture_prepare(void *context,
                                                           uint32_t now_ms)
{
    cycle_fixture_t *fixture = context;
    routed_cycle_application_request_t result = fixture->application;

    (void)now_ms;
    result.prepared_at_ms += fixture->offset_ms;
    record_call(fixture, CYCLE_CALL_PREPARE);
    return result;
}

static tavrn_router_phase_trace_t fixture_submit(
    void *context, const tron_application_data_t *data, uint32_t now_ms)
{
    cycle_fixture_t *fixture = context;

    (void)data;
    (void)now_ms;
    record_call(fixture, CYCLE_CALL_SUBMIT);
    return offset_trace(&fixture->submit, fixture->offset_ms);
}

static tavrn_router_phase_trace_t fixture_dispatch(void *context, uint32_t now_ms)
{
    cycle_fixture_t *fixture = context;

    (void)now_ms;
    record_call(fixture, CYCLE_CALL_DISPATCH);
    return offset_trace(&fixture->dispatch, fixture->offset_ms);
}

static tavrn_router_phase_trace_t fixture_service(void *context, uint32_t now_ms)
{
    cycle_fixture_t *fixture = context;

    (void)now_ms;
    record_call(fixture, CYCLE_CALL_SERVICE);
    return offset_trace(&fixture->service, fixture->offset_ms);
}

static routed_cycle_yield_result_t fixture_yield(
    void *context, uint32_t requested_slack_ms, uint32_t now_ms)
{
    cycle_fixture_t *fixture = context;
    routed_cycle_yield_result_t result = fixture->yield;

    (void)now_ms;
    fixture->requested_yield_ms = requested_slack_ms;
    result.completed_at_ms += fixture->offset_ms;
    record_call(fixture, CYCLE_CALL_YIELD);
    return result;
}

static routed_cycle_fault_idle_result_t fixture_fault_idle(void *context,
                                                            uint32_t now_ms)
{
    cycle_fixture_t *fixture = context;
    routed_cycle_fault_idle_result_t result = fixture->fault_idle;

    result.completed_at_ms += fixture->offset_ms;
    if (result.completed_at_ms - now_ms >= ROUTED_CYCLE_HALF_RANGE) {
        result.completed_at_ms = now_ms;
    }
    record_call(fixture, CYCLE_CALL_FAULT_IDLE);
    return result;
}

static uint32_t fixture_now_ms(void *context)
{
    cycle_fixture_t *fixture = context;
    uint32_t result = fixture->task_now_ms;

    fixture->task_now_ms += fixture->task_now_step_ms;
    return result;
}

static void fixture_trace_sink(void *context, const routed_cycle_trace_t *trace)
{
    cycle_fixture_t *fixture = context;

    if (fixture != NULL && trace != NULL &&
        fixture->sink_count < sizeof(fixture->sink_traces) /
                              sizeof(fixture->sink_traces[0])) {
        fixture->sink_traces[fixture->sink_count++] = *trace;
    }
}

static routed_cycle_operations_t fixture_operations(cycle_fixture_t *fixture)
{
    routed_cycle_operations_t operations;

    memset(&operations, 0, sizeof(operations));
    operations.context = fixture;
    operations.start_rx = fixture_start_rx;
    operations.scheduler_poll = fixture_poll;
    operations.router_scheduler_event = fixture_event;
    operations.router_tick = fixture_tick;
    operations.application_prepare = fixture_prepare;
    operations.router_submit = fixture_submit;
    operations.router_dispatch = fixture_dispatch;
    operations.router_link_service = fixture_service;
    operations.healthy_yield = fixture_yield;
    operations.fault_idle = fixture_fault_idle;
    operations.now_ms = fixture_now_ms;
    operations.trace_sink = fixture_trace_sink;
    return operations;
}

static void setup_healthy_fixture(cycle_fixture_t *fixture, uint8_t with_event,
                                  uint8_t submit_enabled)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->start_rx.status = ROUTED_CYCLE_START_RX_OK;
    fixture->start_rx.returned_at_ms = 95u;
    fixture->poll_offset_step_ms = 10u;
    fixture->poll.status = ROUTED_CYCLE_SCHEDULER_POLL_OK;
    fixture->poll.returned_at_ms = 101u;
    fixture->poll.event.type = with_event != 0u ? BLE_MESH_SCHED_EVENT_RX_ADV :
                                                 BLE_MESH_SCHED_EVENT_NONE;
    fixture->poll.event.fault = BLE_MESH_SCHED_FAULT_NONE;
    fixture->event = make_router_trace(TAVRN_ROUTER_TRACE_SCHEDULER_EVENT, 102u);
    fixture->event.detail.scheduler_event.status = TAVRN_ROUTER_EVENT_IGNORED;
    fixture->tick = make_router_trace(TAVRN_ROUTER_TRACE_TICK, 103u);
    fixture->tick.detail.tick.status = AODV_STATUS_OK;
    fixture->application.status = submit_enabled != 0u ?
        ROUTED_CYCLE_APPLICATION_READY : ROUTED_CYCLE_APPLICATION_DISABLED;
    fixture->application.prepared_at_ms = 104u;
    fixture->application.data.final_destination = sid16(0x4bdcu);
    fixture->application.data.app_kind = 0x7fu;
    fixture->submit = make_router_trace(TAVRN_ROUTER_TRACE_APPLICATION_SUBMIT, 105u);
    fixture->submit.detail.submit.status = AODV_STATUS_QUEUED;
    fixture->dispatch = make_router_trace(TAVRN_ROUTER_TRACE_DISPATCH, 106u);
    fixture->dispatch.detail.dispatch.status = TAVRN_ROUTER_EVENT_IGNORED;
    fixture->dispatch.detail.dispatch.action_present = TAVRN_ROUTER_TRACE_NOT_PRESENT;
    fixture->dispatch.detail.dispatch.rreq_enqueue_present =
        TAVRN_ROUTER_TRACE_NOT_PRESENT;
    fixture->dispatch.detail.dispatch.link_send_present =
        TAVRN_ROUTER_TRACE_NOT_PRESENT;
    fixture->dispatch.detail.dispatch.link_event_present =
        TAVRN_ROUTER_TRACE_NOT_PRESENT;
    fixture->service = make_router_trace(TAVRN_ROUTER_TRACE_LINK_SERVICE, 107u);
    fixture->service.detail.link_service.status = TAVRN_ROUTER_EVENT_IGNORED;
    fixture->service.detail.link_service.link_step_status = TAVRN_LINK_STEP_NO_EVENT;
    fixture->service.detail.link_service.link_event_present =
        TAVRN_ROUTER_TRACE_NOT_PRESENT;
    fixture->yield.status = ROUTED_CYCLE_YIELD_OK;
    fixture->yield.completed_at_ms = 110u;
    fixture->fault_idle.status = ROUTED_CYCLE_FAULT_IDLE_OK;
    fixture->fault_idle.completed_at_ms = 113u;
}

static int calls_match(const cycle_fixture_t *fixture,
                       const enum cycle_call *expected, uint8_t count)
{
    uint8_t index;

    if (fixture == NULL || fixture->call_count != count) {
        return 0;
    }
    for (index = 0u; index < count; index++) {
        if (fixture->calls[index] != expected[index]) {
            return 0;
        }
    }
    return 1;
}

static const routed_cycle_trace_t *trace_for_phase(
    const routed_cycle_run_result_t *result, routed_cycle_phase_t phase)
{
    uint8_t index;

    if (result == NULL) {
        return NULL;
    }
    for (index = 0u; index < result->trace_count; index++) {
        if (result->traces[index].phase == phase) {
            return &result->traces[index];
        }
    }
    return NULL;
}

static int phase_elapsed_is(const routed_cycle_run_result_t *result,
                            routed_cycle_phase_t phase, uint32_t elapsed_ms)
{
    const routed_cycle_trace_t *trace = trace_for_phase(result, phase);

    return trace != NULL && trace->timing.phase_elapsed_ms == elapsed_ms;
}

static int first_over_budget_is(const routed_cycle_run_result_t *result,
                                routed_cycle_phase_t phase)
{
    uint8_t index;

    if (result == NULL) {
        return 0;
    }
    for (index = 0u; index < result->trace_count; index++) {
        if (result->traces[index].timing.over_budget == ROUTED_CYCLE_BOOLEAN_TRUE) {
            return result->traces[index].phase == phase &&
                result->traces[index].timing.over_budget_phase == phase;
        }
    }
    return 0;
}

static void test_maint_03_task_binding_and_yield(void)
{
    static const enum cycle_call expected[] = {
        CYCLE_CALL_START_RX,
        CYCLE_CALL_POLL, CYCLE_CALL_TICK, CYCLE_CALL_PREPARE,
        CYCLE_CALL_DISPATCH, CYCLE_CALL_SERVICE, CYCLE_CALL_YIELD,
        CYCLE_CALL_POLL, CYCLE_CALL_TICK, CYCLE_CALL_PREPARE,
        CYCLE_CALL_DISPATCH, CYCLE_CALL_SERVICE, CYCLE_CALL_YIELD,
        CYCLE_CALL_POLL, CYCLE_CALL_TICK, CYCLE_CALL_PREPARE,
        CYCLE_CALL_DISPATCH, CYCLE_CALL_SERVICE, CYCLE_CALL_YIELD,
    };
    cycle_fixture_t fixture;
    routed_cycle_operations_t operations;
    routed_cycle_t cycle;
    routed_cycle_run_result_t result;
    routed_cycle_run_result_t task_result;

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.task_now_ms = 100u;
    fixture.task_now_step_ms = 10u;
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&task_result, 0xa5, sizeof(task_result));
    CHECK("MAINT-03", routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
                            routed_cycle_run_task(&cycle, &operations, 3u,
                                                  &task_result) ==
                                ROUTED_CYCLE_TASK_OK &&
                            calls_match(&fixture, expected,
                                        sizeof(expected) / sizeof(expected[0])) &&
                             fixture.poll_count == 3u && fixture.requested_yield_ms == 3u &&
                             fixture.sink_count == 21u &&
                              fixture.sink_traces[0].phase ==
                                  ROUTED_CYCLE_PHASE_PRE_POLL_BOUND &&
                              fixture.sink_traces[fixture.sink_count - 1u].phase ==
                                  ROUTED_CYCLE_PHASE_HEALTHY_YIELD &&
                              task_result.status == ROUTED_CYCLE_RUN_OK &&
                              task_result.trace_count == 7u);

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.service.completed_at_ms = 110u;
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    CHECK("MAINT-03", routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
                           routed_cycle_seed_startup_rx_return(&cycle, &operations) ==
                               ROUTED_CYCLE_RESULT_OK &&
                           routed_cycle_run_once(&cycle, &operations, 100u, &result) ==
                               ROUTED_CYCLE_RUN_OK &&
                           trace_for_phase(&result,
                                           ROUTED_CYCLE_PHASE_HEALTHY_YIELD) == NULL &&
                            fixture.requested_yield_ms == 0u);
}

static void test_maint_03_invalid_task_recovery_policy(void)
{
    cycle_fixture_t fixture;
    routed_cycle_operations_t operations;
    routed_cycle_t cycle;
    routed_cycle_run_result_t result;
    routed_cycle_run_result_t result_before;

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.tick.phase = TAVRN_ROUTER_TRACE_DISPATCH;
    fixture.task_now_ms = 100u;
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    CHECK("MAINT-03", routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
                           routed_cycle_run_task(&cycle, &operations, 1u, &result) ==
                               ROUTED_CYCLE_TASK_INVALID &&
                           cycle.fault_latched == 0u &&
                           result.status == ROUTED_CYCLE_RUN_INVALID &&
                           result.trace_count == 2u);

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.tick.phase = TAVRN_ROUTER_TRACE_DISPATCH;
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    if (routed_cycle_init(&cycle, 10u) != ROUTED_CYCLE_RESULT_OK ||
        routed_cycle_seed_startup_rx_return(&cycle, &operations) !=
            ROUTED_CYCLE_RESULT_OK) {
        CHECK("MAINT-03", 0);
        return;
    }
    fixture.call_count = 0u;
    CHECK("MAINT-03", routed_cycle_run_once(&cycle, &operations, 100u, &result) ==
                           ROUTED_CYCLE_RUN_INVALID &&
                           result.status == ROUTED_CYCLE_RUN_INVALID &&
                           result.trace_count == 2u && fixture.call_count == 2u);
    result_before = result;
    CHECK("MAINT-03", routed_cycle_task_latch_run_invalid(&cycle, 0u) ==
                           ROUTED_CYCLE_RESULT_OK &&
                           cycle.fault_latched != 0u &&
                           memcmp(&result, &result_before, sizeof(result)) == 0);
    fixture.call_count = 0u;
    memset(&result, 0xa5, sizeof(result));
    CHECK("MAINT-03", routed_cycle_run_once(&cycle, &operations, 101u, &result) ==
                           ROUTED_CYCLE_RUN_FAULT_IDLE && fixture.call_count == 1u &&
                           fixture.calls[0] == CYCLE_CALL_FAULT_IDLE &&
                           result.trace_count == 1u &&
                           result.traces[0].phase == ROUTED_CYCLE_PHASE_FAULT_IDLE);
}

static void test_bearer_03_seed_and_time_boundaries(void)
{
    cycle_fixture_t fixture;
    routed_cycle_operations_t operations;
    routed_cycle_t cycle;
    routed_cycle_run_result_t result;

    setup_healthy_fixture(&fixture, 0u, 0u);
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    CHECK("BEARER-03", routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
                           routed_cycle_seed_startup_rx_return(&cycle, &operations) ==
                               ROUTED_CYCLE_RESULT_OK && cycle.scheduler_return_seeded != 0u &&
                           cycle.last_scheduler_return_ms == 95u &&
                           cycle.first_over_budget_phase ==
                               ROUTED_CYCLE_PHASE_NOT_APPLICABLE);

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.poll.returned_at_ms = 106u;
    fixture.tick.completed_at_ms = 107u;
    fixture.application.prepared_at_ms = 108u;
    fixture.dispatch.completed_at_ms = 109u;
    fixture.service.completed_at_ms = 110u;
    fixture.yield.completed_at_ms = 115u;
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    CHECK("BEARER-03", routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
                           routed_cycle_seed_startup_rx_return(&cycle, &operations) ==
                               ROUTED_CYCLE_RESULT_OK &&
                           routed_cycle_run_once(&cycle, &operations, 105u, &result) ==
                               ROUTED_CYCLE_RUN_OK &&
                           result.traces[0].timing.elapsed_since_scheduler_return_ms ==
                               10u && result.traces[0].timing.over_budget ==
                                   ROUTED_CYCLE_BOOLEAN_FALSE);

    setup_healthy_fixture(&fixture, 0u, 0u);
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    CHECK("BEARER-03", routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
                           routed_cycle_seed_startup_rx_return(&cycle, &operations) ==
                               ROUTED_CYCLE_RESULT_OK &&
                           routed_cycle_run_once(&cycle, &operations, 106u, &result) ==
                               ROUTED_CYCLE_RUN_FAULT_IDLE &&
                           cycle.first_over_budget_phase ==
                               ROUTED_CYCLE_PHASE_PRE_POLL_BOUND &&
                           fixture.call_count == 2u &&
                           fixture.calls[1] == CYCLE_CALL_FAULT_IDLE);

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.start_rx.returned_at_ms = UINT32_MAX - 4u;
    fixture.poll.returned_at_ms = 6u;
    fixture.tick.completed_at_ms = 7u;
    fixture.application.prepared_at_ms = 8u;
    fixture.dispatch.completed_at_ms = 9u;
    fixture.service.completed_at_ms = 10u;
    fixture.yield.completed_at_ms = 15u;
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    CHECK("BEARER-03", routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
                           routed_cycle_seed_startup_rx_return(&cycle, &operations) ==
                               ROUTED_CYCLE_RESULT_OK &&
                            routed_cycle_run_once(&cycle, &operations, 5u, &result) ==
                                ROUTED_CYCLE_RUN_OK &&
                            result.traces[0].timing.elapsed_since_scheduler_return_ms ==
                                10u &&
                            phase_elapsed_is(&result,
                                             ROUTED_CYCLE_PHASE_SCHEDULER_POLL, 1u));

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.start_rx.returned_at_ms = 0u;
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    CHECK("BEARER-03", routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
                           routed_cycle_seed_startup_rx_return(&cycle, &operations) ==
                               ROUTED_CYCLE_RESULT_OK &&
                           routed_cycle_run_once(&cycle, &operations,
                                                  ROUTED_CYCLE_HALF_RANGE, &result) ==
                                ROUTED_CYCLE_RUN_INVALID && fixture.call_count == 1u);
}

static void test_bearer_03_callback_completion_ordering(void)
{
    cycle_fixture_t fixture;
    routed_cycle_operations_t operations;
    routed_cycle_t cycle;
    routed_cycle_run_result_t result;

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.tick.completed_at_ms = fixture.poll.returned_at_ms;
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    CHECK("BEARER-03", routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
                            routed_cycle_seed_startup_rx_return(&cycle, &operations) ==
                                ROUTED_CYCLE_RESULT_OK &&
                            routed_cycle_run_once(&cycle, &operations, 100u, &result) ==
                                ROUTED_CYCLE_RUN_OK &&
                            phase_elapsed_is(&result, ROUTED_CYCLE_PHASE_ROUTER_TICK,
                                             0u));

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.start_rx.returned_at_ms = UINT32_MAX - 3u;
    fixture.poll.returned_at_ms = UINT32_MAX - 1u;
    fixture.tick.completed_at_ms = 0u;
    fixture.application.prepared_at_ms = 1u;
    fixture.dispatch.completed_at_ms = 2u;
    fixture.service.completed_at_ms = 3u;
    fixture.yield.completed_at_ms = 5u;
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    CHECK("BEARER-03", routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
                            routed_cycle_seed_startup_rx_return(&cycle, &operations) ==
                                ROUTED_CYCLE_RESULT_OK &&
                            routed_cycle_run_once(&cycle, &operations,
                                                  UINT32_MAX - 2u, &result) ==
                                ROUTED_CYCLE_RUN_OK &&
                            phase_elapsed_is(&result, ROUTED_CYCLE_PHASE_ROUTER_TICK,
                                             2u));

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.tick.completed_at_ms = fixture.poll.returned_at_ms +
        ROUTED_CYCLE_HALF_RANGE;
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    if (routed_cycle_init(&cycle, 10u) != ROUTED_CYCLE_RESULT_OK ||
        routed_cycle_seed_startup_rx_return(&cycle, &operations) !=
            ROUTED_CYCLE_RESULT_OK) {
        CHECK("BEARER-03", 0);
        return;
    }
    fixture.call_count = 0u;
    CHECK("BEARER-03", routed_cycle_run_once(&cycle, &operations, 100u, &result) ==
                            ROUTED_CYCLE_RUN_INVALID && fixture.call_count == 2u &&
                            result.trace_count == 2u);

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.tick.completed_at_ms = fixture.poll.returned_at_ms - 1u;
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    if (routed_cycle_init(&cycle, 10u) != ROUTED_CYCLE_RESULT_OK ||
        routed_cycle_seed_startup_rx_return(&cycle, &operations) !=
            ROUTED_CYCLE_RESULT_OK) {
        CHECK("BEARER-03", 0);
        return;
    }
    fixture.call_count = 0u;
    CHECK("BEARER-03", routed_cycle_run_once(&cycle, &operations, 100u, &result) ==
                            ROUTED_CYCLE_RUN_INVALID && fixture.call_count == 2u &&
                            result.trace_count == 2u);
}

static void test_bearer_03_partial_tx_fault_trace(void)
{
    static const enum cycle_call tx_calls[] = {
        CYCLE_CALL_POLL, CYCLE_CALL_EVENT, CYCLE_CALL_TICK,
        CYCLE_CALL_PREPARE, CYCLE_CALL_DISPATCH, CYCLE_CALL_SERVICE,
        CYCLE_CALL_YIELD,
    };
    static const enum cycle_call fault_calls[] = {
        CYCLE_CALL_POLL, CYCLE_CALL_EVENT, CYCLE_CALL_FAULT_IDLE,
    };
    cycle_fixture_t fixture;
    routed_cycle_operations_t operations;
    routed_cycle_t cycle;
    routed_cycle_run_result_t result;
    const routed_cycle_trace_t *poll_trace;

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.poll.event.type = BLE_MESH_SCHED_EVENT_TX_DONE;
    fixture.poll.event.fault = BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT;
    fixture.poll.event.tx_requested_channel_mask = BLE_MESH_SCHED_CH_ALL;
    fixture.poll.event.tx_completed_channel_mask = BLE_MESH_SCHED_CH37;
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    if (routed_cycle_init(&cycle, 10u) != ROUTED_CYCLE_RESULT_OK ||
        routed_cycle_seed_startup_rx_return(&cycle, &operations) !=
            ROUTED_CYCLE_RESULT_OK) {
        CHECK("BEARER-03", 0);
        return;
    }
    fixture.call_count = 0u;
    CHECK("BEARER-03", routed_cycle_run_once(&cycle, &operations, 100u, &result) ==
                            ROUTED_CYCLE_RUN_OK && cycle.fault_latched == 0u &&
                            calls_match(&fixture, tx_calls,
                                        sizeof(tx_calls) / sizeof(tx_calls[0])));
    poll_trace = trace_for_phase(&result, ROUTED_CYCLE_PHASE_SCHEDULER_POLL);
    CHECK("BEARER-03", poll_trace != NULL &&
                            poll_trace->detail.scheduler_poll.event.type ==
                                BLE_MESH_SCHED_EVENT_TX_DONE &&
                            poll_trace->detail.scheduler_poll.event.fault ==
                                BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT &&
                            poll_trace->detail.scheduler_poll.event
                                .tx_requested_channel_mask == BLE_MESH_SCHED_CH_ALL &&
                            poll_trace->detail.scheduler_poll.event
                                .tx_completed_channel_mask == BLE_MESH_SCHED_CH37 &&
                            trace_for_phase(&result,
                                            ROUTED_CYCLE_PHASE_SCHEDULER_EVENT) != NULL);

    fixture.poll.event.type = BLE_MESH_SCHED_EVENT_RADIO_FAULT;
    fixture.poll.event.fault = BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT;
    fixture.call_count = 0u;
    memset(&result, 0xa5, sizeof(result));
    CHECK("BEARER-03", routed_cycle_run_once(&cycle, &operations, 110u, &result) ==
                            ROUTED_CYCLE_RUN_FAULT_IDLE && cycle.fault_latched != 0u &&
                            calls_match(&fixture, fault_calls,
                                        sizeof(fault_calls) / sizeof(fault_calls[0])) &&
                            trace_for_phase(&result,
                                            ROUTED_CYCLE_PHASE_SCHEDULER_EVENT) != NULL &&
                            result.traces[result.trace_count - 1u].phase ==
                                ROUTED_CYCLE_PHASE_FAULT_IDLE);
}

static void test_bearer_03_due_sweep_scheduler_return(void)
{
    static const enum cycle_call returned_after_tick[] = {
        CYCLE_CALL_POLL,
        CYCLE_CALL_TICK,
    };
    static const enum cycle_call resumed_cycle[] = {
        CYCLE_CALL_POLL,
        CYCLE_CALL_TICK,
        CYCLE_CALL_PREPARE,
        CYCLE_CALL_DISPATCH,
        CYCLE_CALL_SERVICE,
        CYCLE_CALL_YIELD,
    };
    cycle_fixture_t fixture;
    routed_cycle_operations_t operations;
    routed_cycle_t cycle;
    routed_cycle_run_result_t result;
    const routed_cycle_trace_t *tick_trace;
    const routed_cycle_trace_t *next_pre_poll_trace;
    uint32_t recorded_gap = 0u;
    uint8_t index;

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.start_rx.returned_at_ms = 100u;
    fixture.due_sweep_enabled = 1u;
    fixture.due_sweep_at_ms = fixture.poll.returned_at_ms;
    operations = fixture_operations(&fixture);
    operations.take_scheduler_return_after_tick =
        fixture_take_scheduler_return_request;
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    if (routed_cycle_init(&cycle, 10u) != ROUTED_CYCLE_RESULT_OK ||
        routed_cycle_seed_startup_rx_return(&cycle, &operations) !=
            ROUTED_CYCLE_RESULT_OK) {
        CHECK("BEARER-03", 0);
        return;
    }

    fixture.call_count = 0u;
    CHECK("BEARER-03", routed_cycle_run_once(&cycle, &operations, 100u, &result) ==
                             ROUTED_CYCLE_RUN_OK &&
                            calls_match(&fixture, returned_after_tick,
                                        sizeof(returned_after_tick) /
                                            sizeof(returned_after_tick[0])) &&
                            result.trace_count == 3u &&
                            trace_for_phase(&result,
                                            ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT) == NULL &&
                            trace_for_phase(&result,
                                            ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH) == NULL &&
                            trace_for_phase(&result,
                                            ROUTED_CYCLE_PHASE_LINK_SERVICE) == NULL &&
                            trace_for_phase(&result,
                                            ROUTED_CYCLE_PHASE_HEALTHY_YIELD) == NULL &&
                            fixture.due_sweep_passes == 1u &&
                            fixture.due_sweep_next_due_ms == 111u &&
                            fixture.scheduler_return_requested == 0u &&
                            fixture.scheduler_return_request_takes == 1u &&
                            cycle.last_scheduler_return_ms == 101u &&
                            cycle.last_gap_phase == ROUTED_CYCLE_PHASE_ROUTER_TICK);
    tick_trace = trace_for_phase(&result, ROUTED_CYCLE_PHASE_ROUTER_TICK);
    for (index = 0u; index < fixture.sink_count; index++) {
        uint32_t sample_gap =
            fixture.sink_traces[index].timing.elapsed_since_scheduler_return_ms;

        if (sample_gap > recorded_gap) {
            recorded_gap = sample_gap;
        }
    }
    CHECK("BEARER-03", tick_trace != NULL &&
                            tick_trace->timing.elapsed_since_scheduler_return_ms == 2u &&
                            recorded_gap == tick_trace->timing.elapsed_since_scheduler_return_ms);

    fixture.call_count = 0u;
    memset(&result, 0xa5, sizeof(result));
    CHECK("BEARER-03", routed_cycle_run_once(&cycle, &operations, 110u, &result) ==
                             ROUTED_CYCLE_RUN_OK &&
                            calls_match(&fixture, resumed_cycle,
                                        sizeof(resumed_cycle) / sizeof(resumed_cycle[0])) &&
                            fixture.due_sweep_passes == 1u &&
                            fixture.scheduler_return_request_takes == 2u &&
                            trace_for_phase(&result,
                                            ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT) != NULL &&
                            trace_for_phase(&result,
                                            ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH) != NULL &&
                            trace_for_phase(&result,
                                            ROUTED_CYCLE_PHASE_LINK_SERVICE) != NULL &&
                            trace_for_phase(&result,
                                            ROUTED_CYCLE_PHASE_HEALTHY_YIELD) != NULL);
    next_pre_poll_trace = trace_for_phase(&result, ROUTED_CYCLE_PHASE_PRE_POLL_BOUND);
    CHECK("BEARER-03", next_pre_poll_trace != NULL &&
                            next_pre_poll_trace->timing.scheduler_return_ms == 101u &&
                            next_pre_poll_trace->timing.elapsed_since_scheduler_return_ms ==
                                9u);
}

static void test_bearer_03_same_time_due_sweep_and_terminal_request(void)
{
    static const enum cycle_call same_time_return[] = {
        CYCLE_CALL_POLL,
        CYCLE_CALL_TICK,
    };
    static const enum cycle_call terminal_calls[] = {
        CYCLE_CALL_POLL,
        CYCLE_CALL_TICK,
        CYCLE_CALL_FAULT_IDLE,
    };
    cycle_fixture_t fixture;
    routed_cycle_operations_t operations;
    routed_cycle_t cycle;
    routed_cycle_run_result_t result;

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.tick.completed_at_ms = fixture.poll.returned_at_ms;
    fixture.poll_offset_step_ms = 0u;
    fixture.due_sweep_enabled = 1u;
    fixture.due_sweep_at_ms = fixture.poll.returned_at_ms;
    operations = fixture_operations(&fixture);
    operations.take_scheduler_return_after_tick =
        fixture_take_scheduler_return_request;
    memset(&cycle, 0, sizeof(cycle));
    if (routed_cycle_init(&cycle, 10u) != ROUTED_CYCLE_RESULT_OK ||
        routed_cycle_seed_startup_rx_return(&cycle, &operations) !=
            ROUTED_CYCLE_RESULT_OK) {
        CHECK("BEARER-03", 0);
        return;
    }
    fixture.call_count = 0u;
    CHECK("BEARER-03", routed_cycle_run_once(&cycle, &operations, 100u, &result) ==
                             ROUTED_CYCLE_RUN_OK &&
                            calls_match(&fixture, same_time_return,
                                        sizeof(same_time_return) /
                                            sizeof(same_time_return[0])) &&
                            fixture.due_sweep_passes == 1u &&
                            fixture.scheduler_return_request_takes == 1u);

    fixture.call_count = 0u;
    CHECK("BEARER-03", routed_cycle_run_once(&cycle, &operations, 101u, &result) ==
                             ROUTED_CYCLE_RUN_OK &&
                            fixture.due_sweep_passes == 1u &&
                            fixture.scheduler_return_request_takes == 2u &&
                            fixture.scheduler_return_requested == 0u &&
                            trace_for_phase(&result,
                                            ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT) != NULL &&
                            trace_for_phase(&result,
                                            ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH) != NULL &&
                            trace_for_phase(&result,
                                            ROUTED_CYCLE_PHASE_LINK_SERVICE) != NULL);

    setup_healthy_fixture(&fixture, 0u, 0u);
    fixture.scheduler_return_requested = 1u;
    fixture.tick.terminal_fault_present = TAVRN_ROUTER_TRACE_PRESENT;
    fixture.tick.terminal_fault = TAVRN_ROUTER_FAULT_DELIVERY_ACTION_MISMATCH;
    operations = fixture_operations(&fixture);
    operations.take_scheduler_return_after_tick =
        fixture_take_scheduler_return_request;
    memset(&cycle, 0, sizeof(cycle));
    if (routed_cycle_init(&cycle, 10u) != ROUTED_CYCLE_RESULT_OK ||
        routed_cycle_seed_startup_rx_return(&cycle, &operations) !=
            ROUTED_CYCLE_RESULT_OK) {
        CHECK("BEARER-03", 0);
        return;
    }
    fixture.call_count = 0u;
    CHECK("BEARER-03", routed_cycle_run_once(&cycle, &operations, 100u, &result) ==
                             ROUTED_CYCLE_RUN_FAULT_IDLE &&
                            calls_match(&fixture, terminal_calls,
                                        sizeof(terminal_calls) / sizeof(terminal_calls[0])) &&
                            fixture.scheduler_return_request_takes == 0u &&
                            fixture.scheduler_return_requested != 0u &&
                            cycle.fault_latched != 0u &&
                            trace_for_phase(&result,
                                            ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT) == NULL);
}

typedef enum fault_site {
    FAULT_EVENT = 0,
    FAULT_TICK,
    FAULT_PREPARE,
    FAULT_SUBMIT,
    FAULT_DISPATCH,
    FAULT_SERVICE,
    FAULT_YIELD,
} fault_site_t;

static routed_cycle_phase_t phase_for_site(fault_site_t site)
{
    if (site == FAULT_EVENT) return ROUTED_CYCLE_PHASE_SCHEDULER_EVENT;
    if (site == FAULT_TICK) return ROUTED_CYCLE_PHASE_ROUTER_TICK;
    if (site == FAULT_PREPARE || site == FAULT_SUBMIT)
        return ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT;
    if (site == FAULT_DISPATCH) return ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH;
    if (site == FAULT_SERVICE) return ROUTED_CYCLE_PHASE_LINK_SERVICE;
    return ROUTED_CYCLE_PHASE_HEALTHY_YIELD;
}

static uint8_t expected_calls_for_site(fault_site_t site, uint8_t with_event,
                                       enum cycle_call expected[10])
{
    uint8_t count = 0u;

    expected[count++] = CYCLE_CALL_POLL;
    if (with_event != 0u) expected[count++] = CYCLE_CALL_EVENT;
    if (site == FAULT_EVENT) goto done;
    expected[count++] = CYCLE_CALL_TICK;
    if (site == FAULT_TICK) goto done;
    expected[count++] = CYCLE_CALL_PREPARE;
    if (site == FAULT_PREPARE) goto done;
    expected[count++] = CYCLE_CALL_SUBMIT;
    if (site == FAULT_SUBMIT) goto done;
    expected[count++] = CYCLE_CALL_DISPATCH;
    if (site == FAULT_DISPATCH) goto done;
    expected[count++] = CYCLE_CALL_SERVICE;
    if (site == FAULT_SERVICE) goto done;
    expected[count++] = CYCLE_CALL_YIELD;
done:
    expected[count++] = CYCLE_CALL_FAULT_IDLE;
    return count;
}

static void set_terminal(tavrn_router_phase_trace_t *trace)
{
    trace->terminal_fault_present = TAVRN_ROUTER_TRACE_PRESENT;
    trace->terminal_fault = TAVRN_ROUTER_FAULT_DELIVERY_ACTION_MISMATCH;
}

static void configure_fault(cycle_fixture_t *fixture, fault_site_t site,
                             uint8_t terminal, uint8_t slow)
{
    uint32_t completed_at_ms = slow != 0u ? 112u :
        (site == FAULT_YIELD ? 107u : 106u);

    if (site == FAULT_EVENT) {
        fixture->event.completed_at_ms = completed_at_ms;
        if (terminal != 0u) set_terminal(&fixture->event);
        else if (slow == 0u) fixture->event.detail.scheduler_event.status =
            TAVRN_ROUTER_EVENT_INVALID;
    } else if (site == FAULT_TICK) {
        fixture->tick.completed_at_ms = completed_at_ms;
        if (terminal != 0u) set_terminal(&fixture->tick);
        else if (slow == 0u) fixture->tick.detail.tick.status = AODV_STATUS_INVALID;
    } else if (site == FAULT_PREPARE) {
        fixture->application.prepared_at_ms = completed_at_ms;
        fixture->application.status = ROUTED_CYCLE_APPLICATION_INVALID;
    } else if (site == FAULT_SUBMIT) {
        fixture->submit.completed_at_ms = completed_at_ms;
        if (terminal != 0u) set_terminal(&fixture->submit);
        else if (slow == 0u) fixture->submit.detail.submit.status = AODV_STATUS_INVALID;
    } else if (site == FAULT_DISPATCH) {
        fixture->dispatch.completed_at_ms = completed_at_ms;
        if (terminal != 0u) set_terminal(&fixture->dispatch);
        else if (slow == 0u) fixture->dispatch.detail.dispatch.status =
            TAVRN_ROUTER_EVENT_INVALID;
    } else if (site == FAULT_SERVICE) {
        fixture->service.completed_at_ms = completed_at_ms;
        if (terminal != 0u) set_terminal(&fixture->service);
        else if (slow == 0u) fixture->service.detail.link_service.status =
            TAVRN_ROUTER_EVENT_INVALID;
    } else {
        fixture->yield.completed_at_ms = completed_at_ms;
        if (slow == 0u) fixture->yield.status = ROUTED_CYCLE_YIELD_INVALID;
    }
}

static int terminal_is_recorded(const routed_cycle_trace_t *trace, fault_site_t site)
{
    const tavrn_router_phase_trace_t *router_trace;

    if (trace == NULL) return 0;
    router_trace = site == FAULT_SUBMIT ?
        &trace->detail.application_submit.submit : &trace->detail.router;
    return router_trace->terminal_fault_present == TAVRN_ROUTER_TRACE_PRESENT &&
        router_trace->terminal_fault == TAVRN_ROUTER_FAULT_DELIVERY_ACTION_MISMATCH;
}

static void run_fault_case(fault_site_t site, uint8_t terminal, uint8_t slow)
{
    cycle_fixture_t fixture;
    routed_cycle_operations_t operations;
    routed_cycle_t cycle;
    routed_cycle_run_result_t result;
    const routed_cycle_trace_t *fault_trace;
    enum cycle_call expected[10];
    routed_cycle_phase_t phase = phase_for_site(site);
    uint8_t with_event = site == FAULT_EVENT || site == FAULT_YIELD;

    setup_healthy_fixture(&fixture, with_event, 1u);
    configure_fault(&fixture, site, terminal, slow);
    operations = fixture_operations(&fixture);
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0xa5, sizeof(result));
    if (routed_cycle_init(&cycle, 10u) != ROUTED_CYCLE_RESULT_OK ||
        routed_cycle_seed_startup_rx_return(&cycle, &operations) !=
            ROUTED_CYCLE_RESULT_OK) {
        CHECK("BEARER-03", 0);
        return;
    }
    fixture.call_count = 0u;
    CHECK("BEARER-03", routed_cycle_run_once(&cycle, &operations, 100u, &result) ==
                                ROUTED_CYCLE_RUN_FAULT_IDLE &&
                           calls_match(&fixture, expected,
                                       expected_calls_for_site(site, with_event, expected)) &&
                           cycle.fault_latched != 0u &&
                            cycle.first_over_budget_phase ==
                                (slow != 0u ? phase :
                                 ROUTED_CYCLE_PHASE_NOT_APPLICABLE) &&
                           (slow == 0u || first_over_budget_is(&result, phase)) &&
                           (slow == 0u || result.trace_count <=
                                ROUTED_CYCLE_TRACE_CAPACITY) &&
                           (slow == 0u || trace_for_phase(&result, phase) != NULL));
    fault_trace = trace_for_phase(&result, phase);
    CHECK("BEARER-03", fault_trace != NULL &&
                           (slow == 0u ||
                            (fault_trace->timing.phase_completed_ms == 112u &&
                             fault_trace->timing.elapsed_since_scheduler_return_ms == 11u)) &&
                           (terminal == 0u || terminal_is_recorded(fault_trace, site)) &&
                           result.trace_count != 0u &&
                           result.traces[result.trace_count - 1u].phase ==
                               ROUTED_CYCLE_PHASE_FAULT_IDLE &&
                           result.traces[result.trace_count - 1u].fault_latched ==
                               ROUTED_CYCLE_BOOLEAN_TRUE);
    fixture.call_count = 0u;
    memset(&result, 0xa5, sizeof(result));
    CHECK("BEARER-03", routed_cycle_run_once(&cycle, &operations, 114u, &result) ==
                               ROUTED_CYCLE_RUN_FAULT_IDLE &&
                           fixture.call_count == 1u &&
                           fixture.calls[0] == CYCLE_CALL_FAULT_IDLE &&
                           result.trace_count == 1u &&
                           result.traces[0].phase == ROUTED_CYCLE_PHASE_FAULT_IDLE);
}

static void test_bearer_03_fault_and_overrun_matrices(void)
{
    fault_site_t site;

    for (site = FAULT_EVENT; site <= FAULT_YIELD; site++) {
        run_fault_case(site, 0u, 1u);
        run_fault_case(site, 0u, 0u);
        if (site != FAULT_PREPARE && site != FAULT_YIELD) {
            run_fault_case(site, 1u, 0u);
        }
    }
}

static routed_cycle_trace_t make_pre_poll_trace(uint32_t marker)
{
    routed_cycle_trace_t trace;

    memset(&trace, 0, sizeof(trace));
    trace.phase = ROUTED_CYCLE_PHASE_PRE_POLL_BOUND;
    trace.timing.phase = ROUTED_CYCLE_PHASE_PRE_POLL_BOUND;
    trace.timing.cycle_started_ms = marker;
    trace.timing.phase_started_ms = marker;
    trace.timing.phase_completed_ms = marker;
    trace.timing.scheduler_return_state = ROUTED_CYCLE_VALUE_KNOWN;
    trace.timing.poll_bound_state = ROUTED_CYCLE_VALUE_KNOWN;
    trace.timing.over_budget = ROUTED_CYCLE_BOOLEAN_FALSE;
    trace.fault_latched = ROUTED_CYCLE_BOOLEAN_FALSE;
    trace.detail.pre_poll.status = ROUTED_CYCLE_PRE_POLL_HEALTHY;
    return trace;
}

static routed_cycle_trace_t make_fault_trace(uint32_t marker)
{
    routed_cycle_trace_t trace = make_pre_poll_trace(marker);

    trace.phase = ROUTED_CYCLE_PHASE_FAULT_IDLE;
    trace.timing.phase = ROUTED_CYCLE_PHASE_FAULT_IDLE;
    trace.timing.over_budget = ROUTED_CYCLE_BOOLEAN_TRUE;
    trace.timing.over_budget_phase = ROUTED_CYCLE_PHASE_LINK_SERVICE;
    trace.fault_latched = ROUTED_CYCLE_BOOLEAN_TRUE;
    trace.detail.fault_idle.status = ROUTED_CYCLE_FAULT_IDLE_OK;
    trace.detail.fault_idle.completed_at_ms = marker;
    return trace;
}

static aodv_rreq_attempt_t make_attempt(uint16_t request_id)
{
    aodv_rreq_attempt_t attempt;

    memset(&attempt, 0, sizeof(attempt));
    attempt.discovery_correlation = request_id;
    attempt.origin = sid16(0x4218u);
    attempt.destination = sid16(0x4bdcu);
    attempt.request_id = request_id;
    attempt.initial_scope = 1u;
    attempt.current_scope = 1u;
    return attempt;
}

static aodv_rreq_lifecycle_record_t make_action_record(uint16_t request_id)
{
    aodv_rreq_lifecycle_record_t record;

    memset(&record, 0, sizeof(record));
    record.attempt = make_attempt(request_id);
    record.action_created_at_ms = request_id;
    record.stage = AODV_RREQ_STAGE_ACTION_CREATED;
    record.scope_source = AODV_RREQ_SCOPE_ORDINARY;
    record.action_created_at_state = AODV_RREQ_VALUE_KNOWN;
    record.link_enqueue_at_state = AODV_RREQ_VALUE_NOT_APPLICABLE;
    record.ordinary = AODV_RREQ_TRUTH_TRUE;
    record.scoped = AODV_RREQ_TRUTH_FALSE;
    record.hint_used = AODV_RREQ_TRUTH_FALSE;
    record.fallback = AODV_RREQ_TRUTH_FALSE;
    record.full_diameter = AODV_RREQ_TRUTH_FALSE;
    record.action_outcome = AODV_RREQ_ACTION_CREATED;
    record.enqueue_outcome = AODV_RREQ_ENQUEUE_NOT_APPLICABLE;
    return record;
}

static void expect_invalid_trace(routed_cycle_diagnostic_queue_t *queue,
                                 const routed_cycle_trace_t *trace)
{
    routed_cycle_diagnostic_queue_t before = *queue;

    CHECK("BUILD-01", routed_cycle_diagnostic_enqueue(queue, trace) ==
                           ROUTED_CYCLE_RESULT_INVALID &&
                           memcmp(queue, &before, sizeof(before)) == 0);
}

static void test_build_01_queue_policy_and_discriminants(void)
{
    routed_cycle_diagnostic_queue_t queue;
    routed_cycle_trace_t trace;
    routed_cycle_trace_t observed;
    aodv_rreq_lifecycle_record_t record;
    routed_cycle_rreq_queue_t rreq_queue;
    uint8_t index;
    static const uint32_t retained[] = { 3u, 4u, 5u, 6u, 7u, 8u, 90u, 93u };

    routed_cycle_diagnostic_queue_init(&queue);
    for (index = 1u; index <= ROUTED_CYCLE_DIAGNOSTIC_CAPACITY; index++) {
        trace = make_pre_poll_trace(index);
        (void)routed_cycle_diagnostic_enqueue(&queue, &trace);
    }
    trace = make_fault_trace(90u);
    (void)routed_cycle_diagnostic_enqueue(&queue, &trace);
    trace = make_pre_poll_trace(91u);
    (void)routed_cycle_diagnostic_enqueue(&queue, &trace);
    trace = make_pre_poll_trace(92u);
    (void)routed_cycle_diagnostic_enqueue(&queue, &trace);
    trace = make_fault_trace(93u);
    (void)routed_cycle_diagnostic_enqueue(&queue, &trace);
    trace = make_pre_poll_trace(94u);
    CHECK("BUILD-01", routed_cycle_diagnostic_enqueue(&queue, &trace) ==
                           ROUTED_CYCLE_RESULT_DROPPED && queue.dropped_count == 3u &&
                           queue.evicted_healthy_count == 2u &&
                           queue.dropped_fault_count == 0u);
    for (index = 0u; index < sizeof(retained) / sizeof(retained[0]); index++) {
        memset(&observed, 0, sizeof(observed));
        CHECK("BUILD-01", routed_cycle_diagnostic_dequeue(&queue, &observed) ==
                               ROUTED_CYCLE_RESULT_OK &&
                               observed.timing.cycle_started_ms == retained[index]);
    }

    routed_cycle_diagnostic_queue_init(&queue);
    trace = make_pre_poll_trace(1u);
    trace.phase = ROUTED_CYCLE_PHASE_SCHEDULER_POLL;
    trace.timing.phase = ROUTED_CYCLE_PHASE_SCHEDULER_POLL;
    trace.detail.scheduler_poll.status = ROUTED_CYCLE_SCHEDULER_POLL_OK;
    trace.detail.scheduler_poll.event.type = BLE_MESH_SCHED_EVENT_NONE;
    trace.detail.scheduler_poll.event.fault = BLE_MESH_SCHED_FAULT_POLL_OVERRUN;
    expect_invalid_trace(&queue, &trace);
    trace = make_pre_poll_trace(2u);
    trace.phase = ROUTED_CYCLE_PHASE_ROUTER_TICK;
    trace.timing.phase = ROUTED_CYCLE_PHASE_ROUTER_TICK;
    trace.detail.router = make_router_trace(TAVRN_ROUTER_TRACE_TICK, 2u);
    trace.detail.router.detail.tick.status = AODV_STATUS_OK;
    trace.detail.router.terminal_fault_present = TAVRN_ROUTER_TRACE_PRESENT;
    trace.detail.router.terminal_fault = TAVRN_ROUTER_FAULT_NONE;
    expect_invalid_trace(&queue, &trace);
    trace = make_pre_poll_trace(3u);
    trace.phase = ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT;
    trace.timing.phase = ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT;
    trace.detail.application_submit.request.status = ROUTED_CYCLE_APPLICATION_DISABLED;
    trace.detail.application_submit.submit_present = TAVRN_ROUTER_TRACE_PRESENT;
    trace.detail.application_submit.submit = make_router_trace(
        TAVRN_ROUTER_TRACE_APPLICATION_SUBMIT, 3u);
    trace.detail.application_submit.submit.detail.submit.status = AODV_STATUS_QUEUED;
    expect_invalid_trace(&queue, &trace);
    trace = make_pre_poll_trace(4u);
    trace.phase = ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH;
    trace.timing.phase = ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH;
    trace.detail.router = make_router_trace(TAVRN_ROUTER_TRACE_DISPATCH, 4u);
    trace.detail.router.detail.dispatch.status = TAVRN_ROUTER_EVENT_IGNORED;
    trace.detail.router.detail.dispatch.action_present = TAVRN_ROUTER_TRACE_PRESENT;
    trace.detail.router.detail.dispatch.action.type = AODV_ACTION_PENDING_DATA_FAILED;
    trace.detail.router.detail.dispatch.action.detail.failure.destination = sid16(0x4bdcu);
    trace.detail.router.detail.dispatch.rreq_enqueue_present =
        TAVRN_ROUTER_TRACE_PRESENT;
    trace.detail.router.detail.dispatch.link_send_present =
        TAVRN_ROUTER_TRACE_NOT_PRESENT;
    trace.detail.router.detail.dispatch.link_event_present =
        TAVRN_ROUTER_TRACE_NOT_PRESENT;
    expect_invalid_trace(&queue, &trace);
    trace.detail.router.detail.dispatch.rreq_enqueue_present =
        TAVRN_ROUTER_TRACE_NOT_PRESENT;
    trace.detail.router.detail.dispatch.link_send_present = TAVRN_ROUTER_TRACE_PRESENT;
    expect_invalid_trace(&queue, &trace);
    trace = make_pre_poll_trace(5u);
    trace.phase = ROUTED_CYCLE_PHASE_LINK_SERVICE;
    trace.timing.phase = ROUTED_CYCLE_PHASE_LINK_SERVICE;
    trace.detail.router = make_router_trace(TAVRN_ROUTER_TRACE_LINK_SERVICE, 5u);
    trace.detail.router.detail.link_service.status = TAVRN_ROUTER_EVENT_OK;
    trace.detail.router.detail.link_service.link_step_status = TAVRN_LINK_STEP_EVENT;
    trace.detail.router.detail.link_service.link_event_present =
        TAVRN_ROUTER_TRACE_NOT_PRESENT;
    expect_invalid_trace(&queue, &trace);
    trace = make_pre_poll_trace(6u);
    trace.phase = ROUTED_CYCLE_PHASE_HEALTHY_YIELD;
    trace.timing.phase = ROUTED_CYCLE_PHASE_HEALTHY_YIELD;
    trace.detail.healthy_yield.requested_slack_ms = 0u;
    trace.detail.healthy_yield.result.status = ROUTED_CYCLE_YIELD_OK;
    expect_invalid_trace(&queue, &trace);
    trace = make_fault_trace(7u);
    trace.detail.fault_idle.status = ROUTED_CYCLE_FAULT_IDLE_INVALID;
    expect_invalid_trace(&queue, &trace);
    trace = make_pre_poll_trace(8u);
    trace.detail.pre_poll.status = (routed_cycle_pre_poll_status_t)99;
    expect_invalid_trace(&queue, &trace);

    routed_cycle_rreq_queue_init(&rreq_queue);
    record = make_action_record(1u);
    CHECK("BUILD-01", routed_cycle_rreq_enqueue(&rreq_queue, &record) ==
                           ROUTED_CYCLE_RESULT_OK &&
                           routed_cycle_rreq_dequeue(&rreq_queue, &record) ==
                               ROUTED_CYCLE_RESULT_OK);
    record = make_action_record(2u);
    record.stage = AODV_RREQ_STAGE_ROUTER_LINK_ENQUEUE;
    record.action_created_at_state = AODV_RREQ_VALUE_NOT_APPLICABLE;
    record.link_enqueue_at_state = AODV_RREQ_VALUE_KNOWN;
    record.link_enqueue_at_ms = 1u;
    record.enqueue_outcome = AODV_RREQ_ENQUEUE_ADMITTED;
    CHECK("BUILD-01", routed_cycle_rreq_enqueue(&rreq_queue, &record) ==
                           ROUTED_CYCLE_RESULT_INVALID);
}

static tavrn_gtt_config_t make_gtt_config(tavrn_adva_t local)
{
    tavrn_gtt_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_identity = local;
    config.soft_expiry_ms = 10u;
    config.hard_expiry_ms = 20u;
    config.departed_retention_ms = 40u;
    return config;
}

static tavrn_gtt_evidence_t make_evidence(tavrn_adva_t identity, uint16_t serial,
                                           uint8_t hop, tavrn_gtt_evidence_kind_t kind)
{
    tavrn_gtt_evidence_t evidence;

    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = identity;
    evidence.serial = serial;
    evidence.serial_present = 1u;
    evidence.hop_count = hop;
    evidence.kind = kind;
    return evidence;
}

static int adapter_preserved(const tavrn_gtt_t *gtt, const tavrn_gtt_t *before,
                             const tavrn_gtt_storage_t *storage,
                             const tavrn_gtt_storage_t *storage_before,
                             const tavrn_gtt_counters_t *counters_before)
{
    const tavrn_gtt_counters_t *counters = tavrn_gtt_counters(gtt);

    return counters != NULL && memcmp(gtt, before, sizeof(*gtt)) == 0 &&
        memcmp(storage, storage_before, sizeof(*storage)) == 0 &&
        memcmp(counters, counters_before, sizeof(*counters)) == 0;
}

static int entry_matches(const routed_cycle_gtt_snapshot_entry_t *entry,
                         const tavrn_adva_t *identity, uint32_t evidence_at,
                         uint16_t serial, uint8_t hop,
                         routed_cycle_value_state_t serial_state,
                         routed_cycle_value_state_t hop_state,
                         routed_cycle_gtt_freshness_t freshness,
                         routed_cycle_boolean_t departed)
{
    return adva_equal(&entry->canonical_adva, identity) &&
        entry->last_evidence_ms == evidence_at &&
        entry->soft_deadline_ms == evidence_at + 10u &&
        entry->hard_deadline_ms == evidence_at + 20u &&
        entry->departed_deadline_ms ==
            (departed == ROUTED_CYCLE_BOOLEAN_TRUE ? evidence_at + 40u : 0u) &&
        entry->serial == serial && entry->hop_count == hop &&
        entry->serial_state == serial_state && entry->hop_state == hop_state &&
        entry->freshness == freshness &&
        entry->departed == departed;
}

static void test_gtt_02_gtt_05_snapshot_and_immutability(void)
{
    tavrn_gtt_t gtt;
    tavrn_gtt_t before;
    tavrn_gtt_storage_t storage;
    tavrn_gtt_storage_t storage_before;
    tavrn_gtt_counters_t counters_before;
    tavrn_gtt_config_t config = make_gtt_config(random_static_adva(0x50u));
    tavrn_gtt_evidence_t soft = make_evidence(random_static_adva(0x10u), 2u, 3u,
                                               TAVRN_GTT_EVIDENCE_LIVENESS);
    tavrn_gtt_evidence_t departed = make_evidence(random_static_adva(0x20u), 4u, 5u,
                                                   TAVRN_GTT_EVIDENCE_DEPARTED);
    tavrn_gtt_evidence_t active = make_evidence(random_static_adva(0x30u), 1u, 2u,
                                                 TAVRN_GTT_EVIDENCE_LIVENESS);
    tavrn_gtt_evidence_t hard = make_evidence(random_static_adva(0x40u), 3u, 4u,
                                               TAVRN_GTT_EVIDENCE_LIVENESS);
    tavrn_gtt_evidence_t expired = make_evidence(random_static_adva(0x60u), 5u, 6u,
                                                  TAVRN_GTT_EVIDENCE_DEPARTED);
    routed_cycle_gtt_snapshot_t snapshot;
    uint32_t base = UINT32_MAX - 40u;
    uint32_t query_at = base + 10u;

    memset(&gtt, 0, sizeof(gtt));
    memset(&storage, 0, sizeof(storage));
    CHECK("GTT-02", tavrn_gtt_init(&gtt, &storage, &config, base) == TAVRN_GTT_INIT_OK &&
                         tavrn_gtt_observe(&gtt, &active, base + 9u) ==
                             TAVRN_GTT_OBSERVE_ADDED &&
                         tavrn_gtt_observe(&gtt, &soft, base) == TAVRN_GTT_OBSERVE_ADDED &&
                         tavrn_gtt_observe(&gtt, &hard, base - 10u) ==
                             TAVRN_GTT_OBSERVE_ADDED &&
                         tavrn_gtt_observe(&gtt, &departed, base + 1u) ==
                             TAVRN_GTT_OBSERVE_DEPARTED &&
                         tavrn_gtt_observe(&gtt, &expired, base - 31u) ==
                             TAVRN_GTT_OBSERVE_DEPARTED);
    before = gtt;
    storage_before = storage;
    counters_before = *tavrn_gtt_counters(&gtt);
    memset(&snapshot, 0xa5, sizeof(snapshot));
    CHECK("GTT-05", routed_full_telemetry_snapshot_gtt(&gtt, query_at, &snapshot) ==
                         ROUTED_FULL_TELEMETRY_OK &&
                         adapter_preserved(&gtt, &before, &storage, &storage_before,
                                           &counters_before) &&
                         snapshot.query_at_ms == query_at &&
                         adva_equal(&snapshot.local_canonical_adva,
                                    &config.local_identity) &&
                         snapshot.entry_count == 5u && snapshot.nondeparted_count == 4u &&
                         entry_matches(&snapshot.entries[0], &soft.identity, base, 2u, 3u,
                                       ROUTED_CYCLE_VALUE_KNOWN,
                                       ROUTED_CYCLE_VALUE_KNOWN,
                                       ROUTED_CYCLE_GTT_FRESHNESS_SOFT_STALE,
                                       ROUTED_CYCLE_BOOLEAN_FALSE) &&
                         entry_matches(&snapshot.entries[1], &departed.identity, base + 1u,
                                       4u, 5u, ROUTED_CYCLE_VALUE_KNOWN,
                                       ROUTED_CYCLE_VALUE_KNOWN,
                                       ROUTED_CYCLE_GTT_FRESHNESS_DEPARTED,
                                       ROUTED_CYCLE_BOOLEAN_TRUE) &&
                         entry_matches(&snapshot.entries[2], &active.identity, base + 9u,
                                       1u, 2u, ROUTED_CYCLE_VALUE_KNOWN,
                                       ROUTED_CYCLE_VALUE_KNOWN,
                                       ROUTED_CYCLE_GTT_FRESHNESS_ACTIVE,
                                       ROUTED_CYCLE_BOOLEAN_FALSE) &&
                         entry_matches(&snapshot.entries[3], &hard.identity, base - 10u,
                                       3u, 4u, ROUTED_CYCLE_VALUE_KNOWN,
                                       ROUTED_CYCLE_VALUE_KNOWN,
                                       ROUTED_CYCLE_GTT_FRESHNESS_HARD_EXPIRED,
                                       ROUTED_CYCLE_BOOLEAN_FALSE) &&
                         entry_matches(&snapshot.entries[4], &config.local_identity, base,
                                       0u, 0u, ROUTED_CYCLE_VALUE_UNKNOWN,
                                       ROUTED_CYCLE_VALUE_KNOWN,
                                       ROUTED_CYCLE_GTT_FRESHNESS_ACTIVE,
                                       ROUTED_CYCLE_BOOLEAN_FALSE));
    CHECK("GTT-02", snapshot.entry_count == 5u && snapshot.nondeparted_count == 4u &&
                         snapshot.entries[1].departed == ROUTED_CYCLE_BOOLEAN_TRUE &&
                         adva_equal(&snapshot.entries[4].canonical_adva,
                                    &config.local_identity));
    before = gtt;
    storage_before = storage;
    counters_before = *tavrn_gtt_counters(&gtt);
    CHECK("GTT-05", routed_full_telemetry_snapshot_gtt(&gtt, query_at, NULL) ==
                         ROUTED_FULL_TELEMETRY_INVALID &&
                         adapter_preserved(&gtt, &before, &storage, &storage_before,
                                           &counters_before));
}

static void test_gtt_02_invalid_and_capacity_boundaries(void)
{
    tavrn_gtt_t gtt;
    tavrn_gtt_t before;
    tavrn_gtt_t invalid;
    tavrn_gtt_storage_t storage;
    tavrn_gtt_storage_t storage_before;
    tavrn_gtt_counters_t counters_before;
    tavrn_gtt_config_t config = make_gtt_config(random_static_adva(0x61u));
    routed_cycle_gtt_snapshot_t snapshot;
    routed_cycle_gtt_snapshot_t snapshot_before;
    uint8_t index;

    memset(&gtt, 0, sizeof(gtt));
    memset(&storage, 0, sizeof(storage));
    CHECK("GTT-02", tavrn_gtt_init(&gtt, &storage, &config, 0u) == TAVRN_GTT_INIT_OK);
    before = gtt;
    storage_before = storage;
    counters_before = *tavrn_gtt_counters(&gtt);
    memset(&snapshot, 0xa5, sizeof(snapshot));
    snapshot_before = snapshot;
    invalid = gtt;
    invalid.storage = NULL;
    CHECK("GTT-05", routed_full_telemetry_snapshot_gtt(&invalid, 0u, &snapshot) ==
                         ROUTED_FULL_TELEMETRY_INVALID &&
                         adapter_preserved(&gtt, &before, &storage, &storage_before,
                                           &counters_before) &&
                         memcmp(&snapshot, &snapshot_before, sizeof(snapshot)) == 0);
    invalid = gtt;
    invalid.config.hard_expiry_ms = invalid.config.soft_expiry_ms;
    CHECK("GTT-05", routed_full_telemetry_snapshot_gtt(&invalid, 0u, &snapshot) ==
                         ROUTED_FULL_TELEMETRY_INVALID &&
                         memcmp(&snapshot, &snapshot_before, sizeof(snapshot)) == 0);
    CHECK("GTT-05", routed_full_telemetry_snapshot_gtt(NULL, 0u, &snapshot) ==
                         ROUTED_FULL_TELEMETRY_INVALID &&
                         memcmp(&snapshot, &snapshot_before, sizeof(snapshot)) == 0);
    before = gtt;
    storage_before = storage;
    counters_before = *tavrn_gtt_counters(&gtt);
    memset(&snapshot, 0xa5, sizeof(snapshot));
    CHECK("GTT-05", routed_full_telemetry_snapshot_gtt(&gtt, 0u, &snapshot) ==
                         ROUTED_FULL_TELEMETRY_OK &&
                         adapter_preserved(&gtt, &before, &storage, &storage_before,
                                           &counters_before) &&
                         snapshot.entry_count == 1u && snapshot.nondeparted_count == 1u &&
                         adva_equal(&snapshot.entries[0].canonical_adva,
                                    &config.local_identity));
    for (index = 1u; index < TAVRN_GTT_CAPACITY; index++) {
        tavrn_gtt_evidence_t evidence = make_evidence(random_static_adva(index), index,
                                                       1u, TAVRN_GTT_EVIDENCE_LIVENESS);
        CHECK("GTT-02", tavrn_gtt_observe(&gtt, &evidence, 1u) ==
                             TAVRN_GTT_OBSERVE_ADDED);
    }
    {
        tavrn_gtt_evidence_t seventeenth = make_evidence(random_static_adva(0x20u),
            1u, 1u, TAVRN_GTT_EVIDENCE_LIVENESS);

        CHECK("GTT-02", tavrn_gtt_observe(&gtt, &seventeenth, 2u) ==
                             TAVRN_GTT_OBSERVE_CAPACITY_REJECTED);
        before = gtt;
        storage_before = storage;
        counters_before = *tavrn_gtt_counters(&gtt);
        memset(&snapshot, 0xa5, sizeof(snapshot));
        CHECK("GTT-02",
                             routed_full_telemetry_snapshot_gtt(&gtt, 2u, &snapshot) ==
                                 ROUTED_FULL_TELEMETRY_OK &&
                             adapter_preserved(&gtt, &before, &storage,
                                               &storage_before, &counters_before) &&
                             snapshot.entry_count == TAVRN_GTT_CAPACITY &&
                             snapshot.nondeparted_count == TAVRN_GTT_CAPACITY);
    }
}

static void test_serial_04_rejoining_and_busy_cycle_statuses(void)
{
    cycle_fixture_t fixture;
    routed_cycle_operations_t operations;
    routed_cycle_t cycle;
    routed_cycle_run_result_t result;

    setup_healthy_fixture(&fixture, 1u, 0u);
    fixture.event.detail.scheduler_event.status = TAVRN_ROUTER_EVENT_REJOINING;
    fixture.dispatch.detail.dispatch.status = TAVRN_ROUTER_EVENT_BUSY;
    operations = fixture_operations(&fixture);
    CHECK("SERIAL-04", TAVRN_ROUTER_EVENT_REJOINING != TAVRN_ROUTER_EVENT_BUSY &&
                           TAVRN_ROUTER_EVENT_REJOINING !=
                               TAVRN_ROUTER_EVENT_INVALID &&
                           routed_cycle_init(&cycle, 10u) == ROUTED_CYCLE_RESULT_OK &&
                           routed_cycle_seed_startup_rx_return(&cycle, &operations) ==
                               ROUTED_CYCLE_RESULT_OK &&
                           routed_cycle_run_once(&cycle, &operations, 100u, &result) ==
                               ROUTED_CYCLE_RUN_OK &&
                           trace_for_phase(&result,
                                           ROUTED_CYCLE_PHASE_SCHEDULER_EVENT) != NULL &&
                           trace_for_phase(&result,
                                           ROUTED_CYCLE_PHASE_SCHEDULER_EVENT)
                                   ->detail.router.detail.scheduler_event.status ==
                               TAVRN_ROUTER_EVENT_REJOINING &&
                           trace_for_phase(&result,
                                           ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH) != NULL &&
                           trace_for_phase(&result,
                                           ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH)
                                   ->detail.router.detail.dispatch.status ==
                               TAVRN_ROUTER_EVENT_BUSY &&
                           cycle.fault_latched == 0u);
}

int main(void)
{
    test_maint_03_task_binding_and_yield();
    test_maint_03_invalid_task_recovery_policy();
    test_bearer_03_seed_and_time_boundaries();
    test_bearer_03_callback_completion_ordering();
    test_bearer_03_partial_tx_fault_trace();
    test_bearer_03_due_sweep_scheduler_return();
    test_bearer_03_same_time_due_sweep_and_terminal_request();
    test_bearer_03_fault_and_overrun_matrices();
    test_build_01_queue_policy_and_discriminants();
    test_gtt_02_gtt_05_snapshot_and_immutability();
    test_gtt_02_invalid_and_capacity_boundaries();
    test_serial_04_rejoining_and_busy_cycle_statuses();
    if (failures != 0u) {
        printf("tavrn_phase4_runtime RED tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_phase4_runtime tests passed\n");
    return 0;
}
