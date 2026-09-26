#include "routed_cycle.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    CAPTURED_SCHEDULER_RETURN_MS = 1574u,
    CAPTURED_REQUESTED_YIELD_MS = 1u,
    CAPTURED_POLL_BOUND_MS = 2u,
    CAPTURED_HEALTHY_WAIT_COMPLETION_GUARD_MS = 2u,
    UNDERDECLARED_HEALTHY_WAIT_COMPLETION_GUARD_MS = 0u,
    /* The 2 ms is the observed timer-phase/alignment overshoot for this
     * capture, not a universal relative-delay minimum. */
    CAPTURED_ALIGNMENT_OVERSHOOT_MS = 2u,
    CAPTURED_ALIGNED_COMPLETION_MS = CAPTURED_SCHEDULER_RETURN_MS +
        CAPTURED_REQUESTED_YIELD_MS + CAPTURED_ALIGNMENT_OVERSHOOT_MS,
    CAPTURED_FAULT_IDLE_COMPLETION_MS = 1579u,
    WIDE_GUARD_POLL_BOUND_MS = 10u,
};

enum scheduler_call {
    SCHEDULER_CALL_START_RX = 1,
    SCHEDULER_CALL_POLL,
    SCHEDULER_CALL_EVENT,
    SCHEDULER_CALL_TICK,
    SCHEDULER_CALL_PREPARE,
    SCHEDULER_CALL_SUBMIT,
    SCHEDULER_CALL_DISPATCH,
    SCHEDULER_CALL_SERVICE,
    SCHEDULER_CALL_RELATIVE_DELAY,
    SCHEDULER_CALL_FAULT_IDLE,
};

typedef struct scheduler_fixture {
    enum scheduler_call calls[10];
    uint8_t call_count;
    uint8_t start_rx_calls;
    uint8_t poll_calls;
    uint8_t scheduler_event_calls;
    uint8_t tick_calls;
    uint8_t prepare_calls;
    uint8_t submit_calls;
    uint8_t dispatch_calls;
    uint8_t service_calls;
    uint8_t relative_delay_calls;
    uint8_t fault_idle_calls;
    uint8_t poll_returns_started_at_ms;
    uint8_t advance_task_now_after_wait;
    uint8_t logger_runs_during_wait;
    uint8_t logger_sentinel_dequeues;
    uint8_t scheduler_gap_exceeded;
    uint8_t coalesced_wait_model;
    uint8_t release_bit_set;
    uint8_t preset_release_before_every_wait;
    uint8_t immediate_stale_waits;
    uint8_t blocking_waits;
    uint8_t coalesced_wait_timed_out;
    uint8_t logger_dispatches;
    uint8_t logger_mid_format_during_wait;
    uint8_t logger_runs_on_boundary_arm;
    uint8_t logger_tx_bytes;
    uint8_t logger_progress_wake_armed;
    uint8_t logger_progress_wake_signals;
    uint8_t logger_request_pending;
    uint8_t logger_request_posts;
    uint8_t logger_request_post_fails;
    uint8_t first_wait_was_immediate;
    uint8_t stale_wait_dispatched_logger;
    uint8_t wake_immediately_after_dequeue;
    uint8_t dequeue_wake_injections;
    uint32_t scheduler_return_ms;
    uint32_t relative_delay_overshoot_ms;
    uint32_t fault_idle_completion_ms;
    uint32_t poll_started_at_ms;
    uint32_t tick_at_ms;
    uint32_t prepare_at_ms;
    uint32_t dispatch_at_ms;
    uint32_t service_at_ms;
    uint32_t requested_yield_ms;
    uint32_t relative_delay_started_at_ms;
    uint32_t modeled_completion_ms;
    uint32_t fault_idle_started_at_ms;
    uint32_t healthy_wait_completion_guard_ms;
    uint32_t task_now_ms;
    uint32_t maximum_scheduler_gap_ms;
    uint32_t sentinel_marker;
    uint32_t logger_dispatch_epoch;
    uint32_t logger_progress_epoch;
    routed_cycle_diagnostic_queue_t *diagnostic_queue;
} scheduler_fixture_t;

static void liveness_logger_run(scheduler_fixture_t *fixture);
static void liveness_logger_tx_byte(scheduler_fixture_t *fixture);
static void liveness_publish_dispatch_progress(scheduler_fixture_t *fixture);

static int setup_error(const char *scenario, const char *detail)
{
    fprintf(stderr, "scheduler-overrun setup error (%s): %s\n", scenario,
            detail);
    return 0;
}

static void record_call(scheduler_fixture_t *fixture, enum scheduler_call call)
{
    if (fixture != NULL && fixture->call_count <
                               sizeof(fixture->calls) / sizeof(fixture->calls[0])) {
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

static routed_cycle_start_rx_result_t scheduler_start_rx(void *context)
{
    scheduler_fixture_t *fixture = context;
    routed_cycle_start_rx_result_t result;

    fixture->start_rx_calls++;
    record_call(fixture, SCHEDULER_CALL_START_RX);
    result.returned_at_ms = fixture->scheduler_return_ms;
    result.status = ROUTED_CYCLE_START_RX_OK;
    return result;
}

static routed_cycle_scheduler_poll_result_t scheduler_poll(
    void *context, uint32_t poll_started_at_ms)
{
    scheduler_fixture_t *fixture = context;
    routed_cycle_scheduler_poll_result_t result;

    memset(&result, 0, sizeof(result));
    fixture->poll_calls++;
    fixture->poll_started_at_ms = poll_started_at_ms;
    record_call(fixture, SCHEDULER_CALL_POLL);
    result.returned_at_ms = fixture->poll_returns_started_at_ms != 0u ?
        poll_started_at_ms : fixture->scheduler_return_ms;
    fixture->scheduler_return_ms = result.returned_at_ms;
    result.event.type = BLE_MESH_SCHED_EVENT_NONE;
    result.event.fault = BLE_MESH_SCHED_FAULT_NONE;
    result.status = ROUTED_CYCLE_SCHEDULER_POLL_OK;
    return result;
}

static tavrn_router_phase_trace_t scheduler_event(
    void *context, const ble_mesh_sched_event_t *event, uint32_t now_ms)
{
    scheduler_fixture_t *fixture = context;
    tavrn_router_phase_trace_t trace = make_router_trace(
        TAVRN_ROUTER_TRACE_SCHEDULER_EVENT, now_ms);

    (void)event;
    fixture->scheduler_event_calls++;
    record_call(fixture, SCHEDULER_CALL_EVENT);
    trace.detail.scheduler_event.status = TAVRN_ROUTER_EVENT_IGNORED;
    return trace;
}

static tavrn_router_phase_trace_t scheduler_tick(void *context, uint32_t now_ms)
{
    scheduler_fixture_t *fixture = context;
    tavrn_router_phase_trace_t trace = make_router_trace(TAVRN_ROUTER_TRACE_TICK,
                                                          now_ms);

    fixture->tick_calls++;
    fixture->tick_at_ms = now_ms;
    record_call(fixture, SCHEDULER_CALL_TICK);
    trace.detail.tick.status = AODV_STATUS_OK;
    return trace;
}

static routed_cycle_application_request_t scheduler_prepare(void *context,
                                                             uint32_t now_ms)
{
    scheduler_fixture_t *fixture = context;
    routed_cycle_application_request_t result;

    memset(&result, 0, sizeof(result));
    fixture->prepare_calls++;
    fixture->prepare_at_ms = now_ms;
    record_call(fixture, SCHEDULER_CALL_PREPARE);
    result.prepared_at_ms = now_ms;
    result.status = ROUTED_CYCLE_APPLICATION_DISABLED;
    return result;
}

static tavrn_router_phase_trace_t scheduler_submit(
    void *context, const tron_application_data_t *data, uint32_t now_ms)
{
    scheduler_fixture_t *fixture = context;
    tavrn_router_phase_trace_t trace = make_router_trace(
        TAVRN_ROUTER_TRACE_APPLICATION_SUBMIT, now_ms);

    (void)data;
    fixture->submit_calls++;
    record_call(fixture, SCHEDULER_CALL_SUBMIT);
    trace.detail.submit.status = AODV_STATUS_QUEUED;
    return trace;
}

static tavrn_router_phase_trace_t scheduler_dispatch(void *context,
                                                      uint32_t now_ms)
{
    scheduler_fixture_t *fixture = context;
    tavrn_router_phase_trace_t trace = make_router_trace(
        TAVRN_ROUTER_TRACE_DISPATCH, now_ms);

    fixture->dispatch_calls++;
    fixture->dispatch_at_ms = now_ms;
    record_call(fixture, SCHEDULER_CALL_DISPATCH);
    trace.detail.dispatch.status = TAVRN_ROUTER_EVENT_IGNORED;
    trace.detail.dispatch.action_present = TAVRN_ROUTER_TRACE_NOT_PRESENT;
    trace.detail.dispatch.rreq_enqueue_present = TAVRN_ROUTER_TRACE_NOT_PRESENT;
    trace.detail.dispatch.link_send_present = TAVRN_ROUTER_TRACE_NOT_PRESENT;
    trace.detail.dispatch.link_event_present = TAVRN_ROUTER_TRACE_NOT_PRESENT;
    return trace;
}

static tavrn_router_phase_trace_t scheduler_service(void *context,
                                                     uint32_t now_ms)
{
    scheduler_fixture_t *fixture = context;
    tavrn_router_phase_trace_t trace = make_router_trace(
        TAVRN_ROUTER_TRACE_LINK_SERVICE, now_ms);

    fixture->service_calls++;
    fixture->service_at_ms = now_ms;
    record_call(fixture, SCHEDULER_CALL_SERVICE);
    trace.detail.link_service.status = TAVRN_ROUTER_EVENT_IGNORED;
    trace.detail.link_service.link_step_status = TAVRN_LINK_STEP_NO_EVENT;
    trace.detail.link_service.link_event_present = TAVRN_ROUTER_TRACE_NOT_PRESENT;
    return trace;
}

static routed_cycle_yield_result_t scheduler_coalesced_wait(
    scheduler_fixture_t *fixture, uint32_t requested_slack_ms, uint32_t now_ms)
{
    routed_cycle_yield_result_t result;
    uint32_t dispatch_epoch = fixture->logger_dispatch_epoch;
    uint32_t elapsed_ms = 0u;

    fixture->logger_progress_wake_armed = 1u;
    fixture->logger_request_posts++;
    if (fixture->logger_request_post_fails != 0u) {
        fixture->logger_progress_wake_armed = 0u;
        result.status = ROUTED_CYCLE_YIELD_INVALID;
        result.completed_at_ms = now_ms;
        return result;
    }
    fixture->logger_request_pending = 1u;
    for (;;) {
        if (fixture->logger_progress_wake_armed != 0u &&
            fixture->logger_request_pending != 0u &&
            fixture->logger_runs_on_boundary_arm != 0u &&
            elapsed_ms == CAPTURED_POLL_BOUND_MS) {
            liveness_logger_run(fixture);
        }
        if (fixture->preset_release_before_every_wait != 0u) {
            if (fixture->immediate_stale_waits != 0u) {
                elapsed_ms++;
            }
            fixture->release_bit_set = 1u;
        }
        if (fixture->release_bit_set != 0u) {
            if (fixture->immediate_stale_waits == 0u &&
                fixture->blocking_waits == 0u) {
                fixture->first_wait_was_immediate = 1u;
            }
            fixture->release_bit_set = 0u;
            fixture->immediate_stale_waits++;
        } else {
            fixture->blocking_waits++;
            if (fixture->logger_runs_during_wait != 0u &&
                fixture->logger_request_pending != 0u) {
                liveness_logger_run(fixture);
            }
            if (fixture->logger_mid_format_during_wait != 0u) {
                liveness_logger_tx_byte(fixture);
            }
            elapsed_ms += requested_slack_ms;
        }
        if (fixture->logger_dispatch_epoch != dispatch_epoch) {
            fixture->logger_progress_wake_armed = 0u;
            result.status = ROUTED_CYCLE_YIELD_OK;
            break;
        }
        if (elapsed_ms > CAPTURED_POLL_BOUND_MS) {
            fixture->logger_progress_wake_armed = 0u;
            fixture->coalesced_wait_timed_out = 1u;
            result.status = ROUTED_CYCLE_YIELD_INVALID;
            break;
        }
        if (elapsed_ms == CAPTURED_POLL_BOUND_MS) {
            fixture->logger_progress_wake_armed = 1u;
        }
    }
    result.completed_at_ms = now_ms + elapsed_ms +
        fixture->relative_delay_overshoot_ms;
    return result;
}

static routed_cycle_yield_result_t scheduler_relative_delay(
    void *context, uint32_t requested_slack_ms, uint32_t now_ms)
{
    scheduler_fixture_t *fixture = context;
    routed_cycle_yield_result_t result;

    fixture->relative_delay_calls++;
    fixture->requested_yield_ms = requested_slack_ms;
    fixture->relative_delay_started_at_ms = now_ms;
    record_call(fixture, SCHEDULER_CALL_RELATIVE_DELAY);
    if (fixture->coalesced_wait_model != 0u) {
        result = scheduler_coalesced_wait(fixture, requested_slack_ms, now_ms);
    } else {
        result.completed_at_ms = now_ms + requested_slack_ms +
            fixture->relative_delay_overshoot_ms;
        result.status = ROUTED_CYCLE_YIELD_OK;
        if (fixture->logger_runs_during_wait != 0u) {
            liveness_logger_run(fixture);
        }
    }
    fixture->modeled_completion_ms = result.completed_at_ms;
    if (fixture->advance_task_now_after_wait != 0u) {
        fixture->task_now_ms = result.completed_at_ms;
    }
    return result;
}

static routed_cycle_fault_idle_result_t scheduler_fault_idle(void *context,
                                                              uint32_t now_ms)
{
    scheduler_fixture_t *fixture = context;
    routed_cycle_fault_idle_result_t result;

    fixture->fault_idle_calls++;
    fixture->fault_idle_started_at_ms = now_ms;
    record_call(fixture, SCHEDULER_CALL_FAULT_IDLE);
    result.completed_at_ms = fixture->fault_idle_completion_ms;
    result.status = ROUTED_CYCLE_FAULT_IDLE_OK;
    return result;
}

static void setup_fixture(scheduler_fixture_t *fixture,
                          uint32_t scheduler_return_ms,
                          uint32_t relative_delay_overshoot_ms,
                          uint32_t fault_idle_completion_ms)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->scheduler_return_ms = scheduler_return_ms;
    fixture->relative_delay_overshoot_ms = relative_delay_overshoot_ms;
    fixture->fault_idle_completion_ms = fault_idle_completion_ms;
}

static routed_cycle_operations_t fixture_operations(scheduler_fixture_t *fixture)
{
    routed_cycle_operations_t operations;

    memset(&operations, 0, sizeof(operations));
    operations.context = fixture;
    operations.start_rx = scheduler_start_rx;
    operations.scheduler_poll = scheduler_poll;
    operations.router_scheduler_event = scheduler_event;
    operations.router_tick = scheduler_tick;
    operations.application_prepare = scheduler_prepare;
    operations.router_submit = scheduler_submit;
    operations.router_dispatch = scheduler_dispatch;
    operations.router_link_service = scheduler_service;
    operations.healthy_yield = scheduler_relative_delay;
    operations.fault_idle = scheduler_fault_idle;
    operations.healthy_wait_completion_guard_ms =
        fixture->healthy_wait_completion_guard_ms;
    return operations;
}

static const routed_cycle_trace_t *trace_for_phase(
    const routed_cycle_run_result_t *result, routed_cycle_phase_t phase,
    uint8_t *count_out)
{
    const routed_cycle_trace_t *trace = NULL;
    uint8_t count = 0u;
    uint8_t index;

    if (count_out != NULL) {
        *count_out = 0u;
    }
    if (result == NULL) {
        return NULL;
    }
    for (index = 0u; index < result->trace_count; index++) {
        if (result->traces[index].phase == phase) {
            trace = &result->traces[index];
            count++;
        }
    }
    if (count_out != NULL) {
        *count_out = count;
    }
    return trace;
}

static int calls_match(const scheduler_fixture_t *fixture,
                       uint8_t yield_count, uint8_t fault_idle_count)
{
    enum scheduler_call expected[8];
    uint8_t expected_count = 0u;
    uint8_t index;

    expected[expected_count++] = SCHEDULER_CALL_START_RX;
    expected[expected_count++] = SCHEDULER_CALL_POLL;
    expected[expected_count++] = SCHEDULER_CALL_TICK;
    expected[expected_count++] = SCHEDULER_CALL_PREPARE;
    expected[expected_count++] = SCHEDULER_CALL_DISPATCH;
    expected[expected_count++] = SCHEDULER_CALL_SERVICE;
    if (yield_count != 0u) {
        expected[expected_count++] = SCHEDULER_CALL_RELATIVE_DELAY;
    }
    if (fault_idle_count != 0u) {
        expected[expected_count++] = SCHEDULER_CALL_FAULT_IDLE;
    }
    if (fixture == NULL || fixture->call_count != expected_count) {
        return 0;
    }
    for (index = 0u; index < expected_count; index++) {
        if (fixture->calls[index] != expected[index]) {
            return 0;
        }
    }
    return 1;
}

static int phase_completed_at(const routed_cycle_run_result_t *result,
                              routed_cycle_phase_t phase, uint32_t completed_at_ms)
{
    const routed_cycle_trace_t *trace;
    uint8_t count;

    trace = trace_for_phase(result, phase, &count);
    return count == 1u && trace != NULL &&
        trace->timing.phase_completed_ms == completed_at_ms;
}

static int run_scenario(const char *scenario, scheduler_fixture_t *fixture,
                        uint32_t poll_bound_ms, routed_cycle_t *cycle,
                        routed_cycle_run_result_t *result,
                        routed_cycle_run_status_t *run_status)
{
    routed_cycle_operations_t operations = fixture_operations(fixture);

    memset(cycle, 0, sizeof(*cycle));
    memset(result, 0, sizeof(*result));
    if (routed_cycle_init(cycle, poll_bound_ms) != ROUTED_CYCLE_RESULT_OK) {
        return setup_error(scenario, "routed_cycle_init failed");
    }
    if (routed_cycle_seed_startup_rx_return(cycle, &operations) !=
        ROUTED_CYCLE_RESULT_OK) {
        return setup_error(scenario, "startup scheduler return was rejected");
    }
    *run_status = routed_cycle_run_once(cycle, &operations,
                                        fixture->scheduler_return_ms, result);
    if (*run_status != ROUTED_CYCLE_RUN_OK &&
        *run_status != ROUTED_CYCLE_RUN_FAULT_IDLE) {
        return setup_error(scenario, "run_once returned a structural failure");
    }
    return 1;
}

static int common_invariants(const char *scenario,
                             const scheduler_fixture_t *fixture,
                             const routed_cycle_run_result_t *result,
                             routed_cycle_run_status_t run_status)
{
    const routed_cycle_trace_t *poll;
    const routed_cycle_trace_t *fault_idle;
    uint8_t pre_poll_count;
    uint8_t poll_count;
    uint8_t scheduler_event_count;
    uint8_t tick_count;
    uint8_t application_count;
    uint8_t dispatch_count;
    uint8_t service_count;
    uint8_t healthy_yield_count;
    uint8_t fault_idle_count;

    (void)trace_for_phase(result, ROUTED_CYCLE_PHASE_PRE_POLL_BOUND,
                          &pre_poll_count);
    poll = trace_for_phase(result, ROUTED_CYCLE_PHASE_SCHEDULER_POLL,
                           &poll_count);
    (void)trace_for_phase(result, ROUTED_CYCLE_PHASE_SCHEDULER_EVENT,
                          &scheduler_event_count);
    (void)trace_for_phase(result, ROUTED_CYCLE_PHASE_ROUTER_TICK, &tick_count);
    (void)trace_for_phase(result, ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT,
                          &application_count);
    (void)trace_for_phase(result, ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH,
                          &dispatch_count);
    (void)trace_for_phase(result, ROUTED_CYCLE_PHASE_LINK_SERVICE, &service_count);
    (void)trace_for_phase(result, ROUTED_CYCLE_PHASE_HEALTHY_YIELD,
                          &healthy_yield_count);
    fault_idle = trace_for_phase(result, ROUTED_CYCLE_PHASE_FAULT_IDLE,
                                 &fault_idle_count);

    if (result->status != run_status) {
        return setup_error(scenario, "result status disagrees with run_once");
    }
    if (fixture->start_rx_calls != 1u || fixture->poll_calls != 1u ||
        fixture->scheduler_event_calls != 0u || fixture->tick_calls != 1u ||
        fixture->prepare_calls != 1u || fixture->submit_calls != 0u ||
        fixture->dispatch_calls != 1u || fixture->service_calls != 1u) {
        return setup_error(scenario, "passive relay callback count changed");
    }
    if (fixture->poll_started_at_ms != fixture->scheduler_return_ms ||
        fixture->tick_at_ms != fixture->scheduler_return_ms ||
        fixture->prepare_at_ms != fixture->scheduler_return_ms ||
        fixture->dispatch_at_ms != fixture->scheduler_return_ms ||
        fixture->service_at_ms != fixture->scheduler_return_ms) {
        return setup_error(scenario, "passive relay timestamps changed");
    }
    if (pre_poll_count != 1u || poll_count != 1u ||
        scheduler_event_count != 0u || tick_count != 1u ||
        application_count != 1u || dispatch_count != 1u || service_count != 1u ||
        healthy_yield_count > 1u || fault_idle_count > 1u) {
        return setup_error(scenario, "phase trace cardinality changed");
    }
    if (healthy_yield_count != fixture->relative_delay_calls ||
        fault_idle_count != fixture->fault_idle_calls ||
        result->trace_count != (uint8_t)(6u + healthy_yield_count +
                                         fault_idle_count) ||
        result->trace_count != fixture->call_count ||
        !calls_match(fixture, healthy_yield_count, fault_idle_count)) {
        return setup_error(scenario, "trace and callback order/count mismatch");
    }
    if (poll == NULL || poll->detail.scheduler_poll.returned_at_ms !=
                             fixture->scheduler_return_ms ||
        poll->detail.scheduler_poll.event.type != BLE_MESH_SCHED_EVENT_NONE ||
        poll->detail.scheduler_poll.event.fault != BLE_MESH_SCHED_FAULT_NONE ||
        !phase_completed_at(result, ROUTED_CYCLE_PHASE_PRE_POLL_BOUND,
                            fixture->scheduler_return_ms) ||
        !phase_completed_at(result, ROUTED_CYCLE_PHASE_SCHEDULER_POLL,
                            fixture->scheduler_return_ms) ||
        !phase_completed_at(result, ROUTED_CYCLE_PHASE_ROUTER_TICK,
                            fixture->scheduler_return_ms) ||
        !phase_completed_at(result, ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT,
                            fixture->scheduler_return_ms) ||
        !phase_completed_at(result, ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH,
                            fixture->scheduler_return_ms) ||
        !phase_completed_at(result, ROUTED_CYCLE_PHASE_LINK_SERVICE,
                            fixture->scheduler_return_ms)) {
        return setup_error(scenario, "scheduler return or zero-work trace changed");
    }
    if (fault_idle_count != 0u &&
        (fault_idle == NULL ||
         fault_idle->detail.fault_idle.completed_at_ms !=
             fixture->fault_idle_completion_ms ||
         fault_idle->timing.phase_started_ms != fixture->fault_idle_started_at_ms)) {
        return setup_error(scenario, "fault-idle callback completion changed");
    }
    return 1;
}

static int validate_captured_admission(
    const scheduler_fixture_t *fixture, const routed_cycle_t *cycle,
    const routed_cycle_run_result_t *result, routed_cycle_run_status_t run_status,
    const routed_cycle_trace_t **healthy_yield_out)
{
    const routed_cycle_trace_t *healthy_yield;
    const routed_cycle_trace_t *fault_idle;
    uint8_t healthy_yield_count;
    uint8_t fault_idle_count;
    uint32_t scheduler_return_ms;
    uint32_t requested_ms;
    uint32_t modeled_completion_ms;
    uint32_t gap_ms;
    uint32_t poll_bound_ms;

    healthy_yield = trace_for_phase(result, ROUTED_CYCLE_PHASE_HEALTHY_YIELD,
                                    &healthy_yield_count);
    fault_idle = trace_for_phase(result, ROUTED_CYCLE_PHASE_FAULT_IDLE,
                                 &fault_idle_count);
    if (healthy_yield == NULL || healthy_yield_count != 1u ||
        fault_idle != NULL || fault_idle_count != 0u ||
        run_status != ROUTED_CYCLE_RUN_OK ||
        cycle->first_over_budget_phase != ROUTED_CYCLE_PHASE_HEALTHY_YIELD ||
        cycle->fault_latched != 0u) {
        return setup_error("capture", "admitted delay lost observe-only attribution");
    }

    scheduler_return_ms = healthy_yield->timing.scheduler_return_ms;
    requested_ms = healthy_yield->detail.healthy_yield.requested_slack_ms;
    modeled_completion_ms =
        healthy_yield->detail.healthy_yield.result.completed_at_ms;
    gap_ms = modeled_completion_ms - scheduler_return_ms;
    poll_bound_ms = healthy_yield->timing.poll_bound_ms;
    if (healthy_yield->timing.over_budget != ROUTED_CYCLE_BOOLEAN_TRUE ||
        healthy_yield->timing.over_budget_phase !=
            ROUTED_CYCLE_PHASE_HEALTHY_YIELD ||
        healthy_yield->timing.over_budget_source !=
            ROUTED_CYCLE_BUDGET_SOURCE_SCHEDULER_RETURN_GAP ||
        scheduler_return_ms != CAPTURED_SCHEDULER_RETURN_MS ||
        requested_ms != CAPTURED_REQUESTED_YIELD_MS ||
        modeled_completion_ms != CAPTURED_ALIGNED_COMPLETION_MS ||
        gap_ms != CAPTURED_ALIGNED_COMPLETION_MS -
            CAPTURED_SCHEDULER_RETURN_MS ||
        poll_bound_ms != CAPTURED_POLL_BOUND_MS ||
        fixture->requested_yield_ms != requested_ms ||
        fixture->modeled_completion_ms != modeled_completion_ms ||
        fixture->relative_delay_started_at_ms != scheduler_return_ms ||
        healthy_yield->fault_latched != ROUTED_CYCLE_BOOLEAN_FALSE) {
        return setup_error("capture", "captured admission values changed");
    }
    *healthy_yield_out = healthy_yield;
    return 1;
}

static int validate_clean_rejection(const routed_cycle_t *cycle,
                                    const routed_cycle_run_result_t *result,
                                    routed_cycle_run_status_t run_status)
{
    uint8_t healthy_yield_count;
    uint8_t fault_idle_count;

    (void)trace_for_phase(result, ROUTED_CYCLE_PHASE_HEALTHY_YIELD,
                          &healthy_yield_count);
    (void)trace_for_phase(result, ROUTED_CYCLE_PHASE_FAULT_IDLE,
                          &fault_idle_count);
    if (run_status != ROUTED_CYCLE_RUN_OK || result->status != ROUTED_CYCLE_RUN_OK ||
        cycle->fault_latched != 0u ||
        cycle->first_over_budget_phase != ROUTED_CYCLE_PHASE_NOT_APPLICABLE ||
        healthy_yield_count != 0u || fault_idle_count != 0u) {
        return setup_error("capture", "unsafe delay rejection was not clean");
    }
    return 1;
}

static uint32_t liveness_now_ms(void *context)
{
    scheduler_fixture_t *fixture = context;

    return fixture->task_now_ms;
}

static void liveness_trace_sink(void *context, const routed_cycle_trace_t *trace)
{
    scheduler_fixture_t *fixture = context;

    if (fixture == NULL || trace == NULL) {
        return;
    }
    if (trace->timing.elapsed_since_scheduler_return_ms >
        fixture->maximum_scheduler_gap_ms) {
        fixture->maximum_scheduler_gap_ms =
            trace->timing.elapsed_since_scheduler_return_ms;
    }
    if (trace->timing.elapsed_since_scheduler_return_ms > CAPTURED_POLL_BOUND_MS) {
        fixture->scheduler_gap_exceeded = 1u;
    }
}

static routed_cycle_trace_t make_liveness_sentinel(uint32_t marker)
{
    routed_cycle_trace_t trace;

    memset(&trace, 0, sizeof(trace));
    trace.phase = ROUTED_CYCLE_PHASE_PRE_POLL_BOUND;
    trace.timing.cycle_started_ms = marker;
    trace.timing.phase_started_ms = marker;
    trace.timing.phase_completed_ms = marker;
    trace.timing.scheduler_return_ms = marker;
    trace.timing.poll_bound_ms = CAPTURED_POLL_BOUND_MS;
    trace.timing.phase = ROUTED_CYCLE_PHASE_PRE_POLL_BOUND;
    trace.timing.over_budget_phase = ROUTED_CYCLE_PHASE_NOT_APPLICABLE;
    trace.timing.over_budget_source = ROUTED_CYCLE_BUDGET_SOURCE_NOT_APPLICABLE;
    trace.timing.scheduler_return_state = ROUTED_CYCLE_VALUE_KNOWN;
    trace.timing.poll_bound_state = ROUTED_CYCLE_VALUE_KNOWN;
    trace.timing.over_budget = ROUTED_CYCLE_BOOLEAN_FALSE;
    trace.fault_latched = ROUTED_CYCLE_BOOLEAN_FALSE;
    trace.detail.pre_poll.status = ROUTED_CYCLE_PRE_POLL_HEALTHY;
    return trace;
}

static void liveness_publish_dispatch_progress(scheduler_fixture_t *fixture)
{
    if (fixture == NULL) {
        return;
    }
    fixture->logger_dispatch_epoch++;
    if (fixture->logger_progress_wake_armed != 0u) {
        fixture->logger_progress_wake_armed = 0u;
        fixture->release_bit_set = 1u;
        fixture->logger_progress_wake_signals++;
    }
}

static void liveness_logger_run(scheduler_fixture_t *fixture)
{
    routed_cycle_trace_t trace;

    if (fixture == NULL) {
        return;
    }
    fixture->logger_dispatches++;
    fixture->logger_request_pending = 0u;
    liveness_publish_dispatch_progress(fixture);
    if (fixture->diagnostic_queue == NULL) {
        return;
    }
    if (routed_cycle_diagnostic_dequeue(fixture->diagnostic_queue, &trace) ==
        ROUTED_CYCLE_RESULT_OK) {
        fixture->logger_progress_epoch++;
        if (fixture->wake_immediately_after_dequeue != 0u) {
            fixture->release_bit_set = 1u;
            fixture->dequeue_wake_injections++;
        }
        if (trace.timing.cycle_started_ms == fixture->sentinel_marker) {
            fixture->logger_sentinel_dequeues++;
        }
    }
}

static void liveness_logger_tx_byte(scheduler_fixture_t *fixture)
{
    if (fixture == NULL) {
        return;
    }
    fixture->logger_tx_bytes++;
    liveness_publish_dispatch_progress(fixture);
}

static int liveness_predicate(const scheduler_fixture_t *fixture,
                              const routed_cycle_t *cycle,
                              routed_cycle_task_status_t task_status,
                              uint32_t iterations)
{
    return fixture != NULL && cycle != NULL &&
        task_status == ROUTED_CYCLE_TASK_OK && fixture->poll_calls == iterations &&
        fixture->relative_delay_calls != 0u &&
        fixture->logger_sentinel_dequeues != 0u &&
        fixture->logger_dispatches != 0u && fixture->immediate_stale_waits != 0u &&
        fixture->logger_dispatch_epoch != 0u &&
        fixture->first_wait_was_immediate != 0u &&
        fixture->stale_wait_dispatched_logger == 0u && fixture->blocking_waits != 0u &&
        fixture->dequeue_wake_injections == 1u &&
        fixture->logger_request_posts == iterations &&
        fixture->logger_request_pending == 0u &&
        fixture->coalesced_wait_timed_out == 0u &&
        fixture->fault_idle_calls == 0u && fixture->scheduler_gap_exceeded == 0u &&
        fixture->maximum_scheduler_gap_ms <= CAPTURED_POLL_BOUND_MS &&
        cycle->fault_latched == 0u;
}

static int run_logger_liveness(void)
{
    enum { LOGGER_LIVENESS_ITERATIONS = 4u };
    scheduler_fixture_t fixture;
    scheduler_fixture_t no_wait_fixture;
    routed_cycle_t cycle;
    routed_cycle_t no_wait_cycle;
    routed_cycle_operations_t operations;
    routed_cycle_operations_t no_wait_operations;
    routed_cycle_run_result_t result;
    routed_cycle_run_result_t no_wait_result;
    routed_cycle_diagnostic_queue_t queue;
    routed_cycle_diagnostic_queue_t no_wait_queue;
    routed_cycle_trace_t sentinel;
    routed_cycle_task_status_t task_status;
    routed_cycle_task_status_t no_wait_task_status;

    setup_fixture(&fixture, CAPTURED_SCHEDULER_RETURN_MS, 0u,
                  CAPTURED_SCHEDULER_RETURN_MS + CAPTURED_POLL_BOUND_MS);
    fixture.poll_returns_started_at_ms = 1u;
    fixture.advance_task_now_after_wait = 1u;
    fixture.logger_runs_during_wait = 1u;
    fixture.coalesced_wait_model = 1u;
    fixture.release_bit_set = 1u;
    fixture.wake_immediately_after_dequeue = 1u;
    fixture.task_now_ms = CAPTURED_SCHEDULER_RETURN_MS;
    fixture.sentinel_marker = 0x4c495645u;
    routed_cycle_diagnostic_queue_init(&queue);
    sentinel = make_liveness_sentinel(fixture.sentinel_marker);
    if (routed_cycle_diagnostic_enqueue(&queue, &sentinel) != ROUTED_CYCLE_RESULT_OK) {
        return setup_error("LOGGER-LIVENESS-01", "sentinel enqueue failed");
    }
    fixture.diagnostic_queue = &queue;
    operations = fixture_operations(&fixture);
    operations.now_ms = liveness_now_ms;
    operations.trace_sink = liveness_trace_sink;
    memset(&cycle, 0, sizeof(cycle));
    memset(&result, 0, sizeof(result));
    if (routed_cycle_init(&cycle, CAPTURED_POLL_BOUND_MS) !=
        ROUTED_CYCLE_RESULT_OK) {
        return setup_error("LOGGER-LIVENESS-01", "cycle initialization failed");
    }
    task_status = routed_cycle_run_task(&cycle, &operations,
                                        LOGGER_LIVENESS_ITERATIONS, &result);
    if (!liveness_predicate(&fixture, &cycle, task_status,
                            LOGGER_LIVENESS_ITERATIONS) || queue.count != 0u) {
        return setup_error("LOGGER-LIVENESS-01",
                           "periodic mesh wait did not run the logger cleanly");
    }

    setup_fixture(&no_wait_fixture, CAPTURED_SCHEDULER_RETURN_MS, 0u,
                  CAPTURED_SCHEDULER_RETURN_MS + CAPTURED_POLL_BOUND_MS + 1u);
    no_wait_fixture.poll_returns_started_at_ms = 1u;
    no_wait_fixture.advance_task_now_after_wait = 1u;
    no_wait_fixture.logger_runs_during_wait = 1u;
    no_wait_fixture.coalesced_wait_model = 1u;
    no_wait_fixture.preset_release_before_every_wait = 1u;
    no_wait_fixture.task_now_ms = CAPTURED_SCHEDULER_RETURN_MS;
    no_wait_fixture.sentinel_marker = 0x4c495645u;
    routed_cycle_diagnostic_queue_init(&no_wait_queue);
    sentinel = make_liveness_sentinel(no_wait_fixture.sentinel_marker);
    if (routed_cycle_diagnostic_enqueue(&no_wait_queue, &sentinel) !=
        ROUTED_CYCLE_RESULT_OK) {
        return setup_error("LOGGER-LIVENESS-01", "negative sentinel enqueue failed");
    }
    no_wait_fixture.diagnostic_queue = &no_wait_queue;
    no_wait_operations = fixture_operations(&no_wait_fixture);
    no_wait_operations.now_ms = liveness_now_ms;
    no_wait_operations.trace_sink = liveness_trace_sink;
    memset(&no_wait_cycle, 0, sizeof(no_wait_cycle));
    memset(&no_wait_result, 0, sizeof(no_wait_result));
    if (routed_cycle_init(&no_wait_cycle, CAPTURED_POLL_BOUND_MS) !=
        ROUTED_CYCLE_RESULT_OK) {
        return setup_error("LOGGER-LIVENESS-01", "negative cycle initialization failed");
    }
    no_wait_task_status = routed_cycle_run_task(&no_wait_cycle, &no_wait_operations,
                                                LOGGER_LIVENESS_ITERATIONS,
                                                &no_wait_result);
    if (no_wait_task_status != ROUTED_CYCLE_TASK_FAULT_IDLE ||
        no_wait_fixture.poll_calls != 1u || no_wait_fixture.relative_delay_calls != 1u ||
        no_wait_fixture.immediate_stale_waits < 2u ||
        no_wait_fixture.blocking_waits != 0u || no_wait_fixture.logger_dispatches != 0u ||
        no_wait_fixture.logger_sentinel_dequeues != 0u ||
        no_wait_fixture.coalesced_wait_timed_out == 0u ||
        no_wait_fixture.logger_progress_wake_armed != 0u ||
        no_wait_fixture.logger_progress_wake_signals != 0u ||
        no_wait_fixture.logger_request_posts != 1u ||
        no_wait_fixture.modeled_completion_ms -
            no_wait_fixture.relative_delay_started_at_ms !=
                CAPTURED_POLL_BOUND_MS + 1u ||
        no_wait_queue.count != 1u ||
        liveness_predicate(&no_wait_fixture, &no_wait_cycle, no_wait_task_status,
                           LOGGER_LIVENESS_ITERATIONS)) {
        return setup_error("LOGGER-LIVENESS-01",
                           "no-wait control unexpectedly satisfied liveness");
    }
    return 1;
}

static int run_logger_dispatch_modes(void)
{
    enum { LOGGER_DISPATCH_ITERATIONS = 2u };
    scheduler_fixture_t idle_fixture;
    scheduler_fixture_t tx_fixture;
    scheduler_fixture_t diagnostic_stall_fixture;
    scheduler_fixture_t boundary_fixture;
    routed_cycle_t idle_cycle;
    routed_cycle_t tx_cycle;
    routed_cycle_t diagnostic_stall_cycle;
    routed_cycle_t boundary_cycle;
    routed_cycle_run_result_t idle_result;
    routed_cycle_run_result_t tx_result;
    routed_cycle_run_result_t diagnostic_stall_result;
    routed_cycle_run_result_t boundary_result;
    routed_cycle_operations_t operations;
    routed_cycle_diagnostic_queue_t diagnostic_queue;
    routed_cycle_trace_t sentinel;
    routed_cycle_task_status_t status;

    setup_fixture(&idle_fixture, CAPTURED_SCHEDULER_RETURN_MS, 0u,
                  CAPTURED_SCHEDULER_RETURN_MS + CAPTURED_POLL_BOUND_MS);
    idle_fixture.poll_returns_started_at_ms = 1u;
    idle_fixture.advance_task_now_after_wait = 1u;
    idle_fixture.logger_runs_during_wait = 1u;
    idle_fixture.coalesced_wait_model = 1u;
    idle_fixture.release_bit_set = 1u;
    idle_fixture.task_now_ms = CAPTURED_SCHEDULER_RETURN_MS;
    operations = fixture_operations(&idle_fixture);
    operations.now_ms = liveness_now_ms;
    operations.trace_sink = liveness_trace_sink;
    memset(&idle_cycle, 0, sizeof(idle_cycle));
    memset(&idle_result, 0, sizeof(idle_result));
    if (routed_cycle_init(&idle_cycle, CAPTURED_POLL_BOUND_MS) !=
            ROUTED_CYCLE_RESULT_OK) {
        return setup_error("LOGGER-DISPATCH-01", "idle cycle initialization failed");
    }
    status = routed_cycle_run_task(&idle_cycle, &operations,
                                   LOGGER_DISPATCH_ITERATIONS, &idle_result);
    if (status != ROUTED_CYCLE_TASK_OK || idle_fixture.immediate_stale_waits == 0u ||
        idle_fixture.blocking_waits == 0u || idle_fixture.logger_dispatches == 0u ||
        idle_fixture.logger_dispatch_epoch == 0u ||
        idle_fixture.coalesced_wait_timed_out != 0u ||
        idle_fixture.scheduler_gap_exceeded != 0u || idle_cycle.fault_latched != 0u) {
        return setup_error("LOGGER-DISPATCH-01",
                           "idle logger did not acknowledge a stale release safely");
    }

    setup_fixture(&tx_fixture, CAPTURED_SCHEDULER_RETURN_MS, 0u,
                  CAPTURED_SCHEDULER_RETURN_MS + CAPTURED_POLL_BOUND_MS);
    tx_fixture.poll_returns_started_at_ms = 1u;
    tx_fixture.advance_task_now_after_wait = 1u;
    tx_fixture.logger_mid_format_during_wait = 1u;
    tx_fixture.coalesced_wait_model = 1u;
    tx_fixture.release_bit_set = 1u;
    tx_fixture.task_now_ms = CAPTURED_SCHEDULER_RETURN_MS;
    operations = fixture_operations(&tx_fixture);
    operations.now_ms = liveness_now_ms;
    operations.trace_sink = liveness_trace_sink;
    memset(&tx_cycle, 0, sizeof(tx_cycle));
    memset(&tx_result, 0, sizeof(tx_result));
    if (routed_cycle_init(&tx_cycle, CAPTURED_POLL_BOUND_MS) !=
            ROUTED_CYCLE_RESULT_OK) {
        return setup_error("LOGGER-DISPATCH-02", "TX cycle initialization failed");
    }
    status = routed_cycle_run_task(&tx_cycle, &operations,
                                   LOGGER_DISPATCH_ITERATIONS, &tx_result);
    if (status != ROUTED_CYCLE_TASK_OK || tx_fixture.logger_tx_bytes == 0u ||
        tx_fixture.logger_dispatches != 0u || tx_fixture.logger_dispatch_epoch == 0u ||
        tx_fixture.coalesced_wait_timed_out != 0u ||
        tx_fixture.scheduler_gap_exceeded != 0u || tx_cycle.fault_latched != 0u) {
        return setup_error("LOGGER-DISPATCH-02",
                           "mid-format TX progress did not acknowledge safely");
    }

    setup_fixture(&diagnostic_stall_fixture, CAPTURED_SCHEDULER_RETURN_MS, 0u,
                  CAPTURED_SCHEDULER_RETURN_MS + CAPTURED_POLL_BOUND_MS);
    diagnostic_stall_fixture.poll_returns_started_at_ms = 1u;
    diagnostic_stall_fixture.advance_task_now_after_wait = 1u;
    diagnostic_stall_fixture.logger_mid_format_during_wait = 1u;
    diagnostic_stall_fixture.coalesced_wait_model = 1u;
    diagnostic_stall_fixture.release_bit_set = 1u;
    diagnostic_stall_fixture.task_now_ms = CAPTURED_SCHEDULER_RETURN_MS;
    diagnostic_stall_fixture.sentinel_marker = 0x44494147u;
    routed_cycle_diagnostic_queue_init(&diagnostic_queue);
    sentinel = make_liveness_sentinel(diagnostic_stall_fixture.sentinel_marker);
    if (routed_cycle_diagnostic_enqueue(&diagnostic_queue, &sentinel) !=
            ROUTED_CYCLE_RESULT_OK) {
        return setup_error("LOGGER-DISPATCH-03", "diagnostic sentinel enqueue failed");
    }
    diagnostic_stall_fixture.diagnostic_queue = &diagnostic_queue;
    operations = fixture_operations(&diagnostic_stall_fixture);
    operations.now_ms = liveness_now_ms;
    operations.trace_sink = liveness_trace_sink;
    memset(&diagnostic_stall_cycle, 0, sizeof(diagnostic_stall_cycle));
    memset(&diagnostic_stall_result, 0, sizeof(diagnostic_stall_result));
    if (routed_cycle_init(&diagnostic_stall_cycle, CAPTURED_POLL_BOUND_MS) !=
            ROUTED_CYCLE_RESULT_OK) {
        return setup_error("LOGGER-DISPATCH-03",
                           "diagnostic stall cycle initialization failed");
    }
    status = routed_cycle_run_task(&diagnostic_stall_cycle, &operations,
                                   LOGGER_DISPATCH_ITERATIONS,
                                   &diagnostic_stall_result);
    if (status != ROUTED_CYCLE_TASK_OK ||
        diagnostic_stall_fixture.logger_tx_bytes == 0u ||
        diagnostic_stall_fixture.logger_dispatch_epoch == 0u ||
        diagnostic_stall_fixture.logger_progress_epoch != 0u ||
        diagnostic_stall_fixture.coalesced_wait_timed_out != 0u ||
        diagnostic_stall_fixture.scheduler_gap_exceeded != 0u ||
        diagnostic_stall_cycle.fault_latched != 0u || diagnostic_queue.count != 1u) {
        return setup_error("LOGGER-DISPATCH-03",
                           "pending diagnostic rejected active TX progress");
    }

    setup_fixture(&boundary_fixture, CAPTURED_SCHEDULER_RETURN_MS, 0u,
                  CAPTURED_SCHEDULER_RETURN_MS + CAPTURED_POLL_BOUND_MS);
    boundary_fixture.poll_returns_started_at_ms = 1u;
    boundary_fixture.advance_task_now_after_wait = 1u;
    boundary_fixture.logger_runs_on_boundary_arm = 1u;
    boundary_fixture.coalesced_wait_model = 1u;
    boundary_fixture.release_bit_set = 1u;
    boundary_fixture.task_now_ms = CAPTURED_SCHEDULER_RETURN_MS;
    operations = fixture_operations(&boundary_fixture);
    operations.now_ms = liveness_now_ms;
    operations.trace_sink = liveness_trace_sink;
    memset(&boundary_cycle, 0, sizeof(boundary_cycle));
    memset(&boundary_result, 0, sizeof(boundary_result));
    if (routed_cycle_init(&boundary_cycle, CAPTURED_POLL_BOUND_MS) !=
            ROUTED_CYCLE_RESULT_OK) {
        return setup_error("LOGGER-DISPATCH-04",
                           "boundary cycle initialization failed");
    }
    status = routed_cycle_run_task(&boundary_cycle, &operations, 1u,
                                   &boundary_result);
    if (status != ROUTED_CYCLE_TASK_OK ||
        boundary_fixture.logger_dispatches != 1u ||
        boundary_fixture.logger_progress_wake_signals != 1u ||
        boundary_fixture.logger_progress_wake_armed != 0u ||
        boundary_fixture.logger_request_pending != 0u ||
        boundary_fixture.logger_request_posts != 1u ||
        boundary_fixture.coalesced_wait_timed_out != 0u ||
        boundary_fixture.modeled_completion_ms -
            boundary_fixture.relative_delay_started_at_ms !=
                CAPTURED_POLL_BOUND_MS ||
        boundary_fixture.scheduler_gap_exceeded != 0u ||
        boundary_cycle.fault_latched != 0u) {
        return setup_error("LOGGER-DISPATCH-04",
                           "exact-boundary logger wake was not one-shot");
    }
    return 1;
}

static int run_wide_guard(void)
{
    scheduler_fixture_t fixture;
    routed_cycle_t cycle;
    routed_cycle_run_result_t result;
    routed_cycle_run_status_t run_status;
    const routed_cycle_trace_t *healthy_yield;
    uint8_t healthy_yield_count;
    uint8_t fault_idle_count;

    setup_fixture(&fixture, CAPTURED_SCHEDULER_RETURN_MS, 0u,
                  CAPTURED_SCHEDULER_RETURN_MS + WIDE_GUARD_POLL_BOUND_MS);
    if (!run_scenario("wide guard", &fixture, WIDE_GUARD_POLL_BOUND_MS,
                      &cycle, &result, &run_status) ||
        !common_invariants("wide guard", &fixture, &result, run_status)) {
        return 0;
    }
    healthy_yield = trace_for_phase(&result, ROUTED_CYCLE_PHASE_HEALTHY_YIELD,
                                    &healthy_yield_count);
    (void)trace_for_phase(&result, ROUTED_CYCLE_PHASE_FAULT_IDLE,
                          &fault_idle_count);
    if (run_status != ROUTED_CYCLE_RUN_OK ||
        cycle.first_over_budget_phase != ROUTED_CYCLE_PHASE_NOT_APPLICABLE ||
        cycle.fault_latched != 0u || healthy_yield == NULL ||
        healthy_yield_count != 1u || fault_idle_count != 0u ||
        fixture.relative_delay_calls != 1u || fixture.requested_yield_ms == 0u ||
        healthy_yield->detail.healthy_yield.requested_slack_ms !=
            fixture.requested_yield_ms ||
        healthy_yield->detail.healthy_yield.result.completed_at_ms !=
            fixture.modeled_completion_ms ||
        fixture.modeled_completion_ms != fixture.relative_delay_started_at_ms +
            fixture.requested_yield_ms) {
        return setup_error("wide guard", "safe voluntary yield was not admitted");
    }
    return 1;
}

int main(void)
{
    scheduler_fixture_t fixture;
    scheduler_fixture_t underdeclared_fixture;
    routed_cycle_t cycle;
    routed_cycle_t underdeclared_cycle;
    routed_cycle_run_result_t result;
    routed_cycle_run_result_t underdeclared_result;
    routed_cycle_run_status_t run_status;
    routed_cycle_run_status_t underdeclared_run_status;
    const routed_cycle_trace_t *healthy_yield = NULL;

    setup_fixture(&fixture, CAPTURED_SCHEDULER_RETURN_MS,
                   CAPTURED_ALIGNMENT_OVERSHOOT_MS,
                   CAPTURED_FAULT_IDLE_COMPLETION_MS);
    fixture.healthy_wait_completion_guard_ms =
        CAPTURED_HEALTHY_WAIT_COMPLETION_GUARD_MS;
    if (!run_scenario("capture", &fixture, CAPTURED_POLL_BOUND_MS,
                       &cycle, &result, &run_status) ||
        !common_invariants("capture", &fixture, &result, run_status) ||
        !validate_clean_rejection(&cycle, &result, run_status) ||
        fixture.relative_delay_calls != 0u) {
        return 2;
    }

    setup_fixture(&underdeclared_fixture, CAPTURED_SCHEDULER_RETURN_MS,
                  CAPTURED_ALIGNMENT_OVERSHOOT_MS,
                  CAPTURED_FAULT_IDLE_COMPLETION_MS);
    underdeclared_fixture.healthy_wait_completion_guard_ms =
        UNDERDECLARED_HEALTHY_WAIT_COMPLETION_GUARD_MS;
    if (!run_scenario("underdeclared guard", &underdeclared_fixture,
                      CAPTURED_POLL_BOUND_MS, &underdeclared_cycle,
                      &underdeclared_result, &underdeclared_run_status) ||
        !common_invariants("underdeclared guard", &underdeclared_fixture,
                           &underdeclared_result, underdeclared_run_status) ||
        !validate_captured_admission(&underdeclared_fixture, &underdeclared_cycle,
                                     &underdeclared_result,
                                     underdeclared_run_status, &healthy_yield)) {
        return 2;
    }

    if (!run_wide_guard() || !run_logger_liveness() ||
        !run_logger_dispatch_modes()) {
        return 2;
    }
    return 0;
}
