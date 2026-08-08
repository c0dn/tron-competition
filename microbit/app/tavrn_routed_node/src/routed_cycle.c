#include "routed_cycle.h"

#include <string.h>

static int cycle_phase_is_traceable(routed_cycle_phase_t phase)
{
    return phase >= ROUTED_CYCLE_PHASE_PRE_POLL_BOUND &&
        phase <= ROUTED_CYCLE_PHASE_FAULT_IDLE;
}

static int cycle_phase_or_not_applicable_is_valid(routed_cycle_phase_t phase)
{
    return phase == ROUTED_CYCLE_PHASE_NOT_APPLICABLE ||
        cycle_phase_is_traceable(phase) ||
        phase == ROUTED_CYCLE_PHASE_STARTUP_RX_RETURN;
}

static int value_state_is_valid(routed_cycle_value_state_t state)
{
    return state >= ROUTED_CYCLE_VALUE_NOT_APPLICABLE &&
        state <= ROUTED_CYCLE_VALUE_UNKNOWN;
}

static int boolean_is_valid(routed_cycle_boolean_t value)
{
    return value >= ROUTED_CYCLE_BOOLEAN_NOT_APPLICABLE &&
        value <= ROUTED_CYCLE_BOOLEAN_UNKNOWN;
}

static int router_presence_is_valid(tavrn_router_trace_presence_t presence)
{
    return presence == TAVRN_ROUTER_TRACE_NOT_PRESENT ||
        presence == TAVRN_ROUTER_TRACE_PRESENT;
}

static int router_fault_is_valid(tavrn_router_fault_reason_t fault)
{
    return fault >= TAVRN_ROUTER_FAULT_NONE &&
        fault <= TAVRN_ROUTER_FAULT_FAILURE_REPORT_INVALID;
}

static int router_event_status_is_valid(tavrn_router_event_status_t status)
{
    return status >= TAVRN_ROUTER_EVENT_OK &&
        status <= TAVRN_ROUTER_EVENT_REJOINING;
}

static int aodv_status_is_valid(aodv_status_t status)
{
    return status >= AODV_STATUS_OK && status <= AODV_STATUS_REJOINING;
}

static int link_send_status_is_valid(tavrn_link_send_status_t status)
{
    return status >= TAVRN_LINK_SEND_OK && status <= TAVRN_LINK_SEND_MESH_FAULTED;
}

static int link_step_status_is_valid(tavrn_link_step_status_t status)
{
    return status >= TAVRN_LINK_STEP_NO_EVENT && status <= TAVRN_LINK_STEP_INVALID;
}

static int link_event_type_is_valid(tavrn_link_event_type_t type)
{
    return type >= TAVRN_LINK_EVENT_NONE &&
        type <= TAVRN_LINK_EVENT_SERVICE_FAULT_TERMINAL;
}

static int scheduler_event_type_is_valid(ble_mesh_sched_event_type_t type)
{
    return type >= BLE_MESH_SCHED_EVENT_NONE &&
        type <= BLE_MESH_SCHED_EVENT_SERVICE_FAULT;
}

static int scheduler_fault_is_valid(ble_mesh_sched_fault_t fault)
{
    return fault >= BLE_MESH_SCHED_FAULT_NONE &&
        fault <= BLE_MESH_SCHED_FAULT_INTERNAL_STATE;
}

static int scheduler_event_is_valid(const ble_mesh_sched_event_t *event)
{
    if (event == NULL || !scheduler_event_type_is_valid(event->type) ||
        !scheduler_fault_is_valid(event->fault)) {
        return 0;
    }
    if (event->type == BLE_MESH_SCHED_EVENT_NONE) {
        return event->fault == BLE_MESH_SCHED_FAULT_NONE;
    }
    if (event->type == BLE_MESH_SCHED_EVENT_RADIO_FAULT ||
        event->type == BLE_MESH_SCHED_EVENT_SERVICE_FAULT) {
        return event->fault != BLE_MESH_SCHED_FAULT_NONE;
    }
    return 1;
}

static int scheduler_poll_result_is_valid(
    const routed_cycle_scheduler_poll_result_t *result)
{
    return result != NULL &&
        (result->status == ROUTED_CYCLE_SCHEDULER_POLL_OK ||
         result->status == ROUTED_CYCLE_SCHEDULER_POLL_INVALID) &&
        scheduler_event_is_valid(&result->event);
}

static int application_status_is_valid(routed_cycle_application_request_status_t status)
{
    return status >= ROUTED_CYCLE_APPLICATION_DISABLED &&
        status <= ROUTED_CYCLE_APPLICATION_INVALID;
}

static int yield_status_is_valid(routed_cycle_yield_status_t status)
{
    return status == ROUTED_CYCLE_YIELD_OK ||
        status == ROUTED_CYCLE_YIELD_INVALID;
}

static int fault_idle_status_is_valid(routed_cycle_fault_idle_status_t status)
{
    return status == ROUTED_CYCLE_FAULT_IDLE_OK ||
        status == ROUTED_CYCLE_FAULT_IDLE_INVALID;
}

static int action_type_is_link_sendable(aodv_action_type_t type)
{
    return type == AODV_ACTION_SEND_RREQ || type == AODV_ACTION_SEND_RREP ||
        type == AODV_ACTION_FORWARD_RREP || type == AODV_ACTION_SEND_RERR ||
        type == AODV_ACTION_SEND_RREP_ACK || type == AODV_ACTION_FORWARD_DATA;
}

static int logical_id_equal(const tavrn_logical_id_t *left,
                            const tavrn_logical_id_t *right)
{
    return left != NULL && right != NULL && left->width == right->width &&
        left->value == right->value;
}

static int rreq_attempt_equal(const aodv_rreq_attempt_t *left,
                              const aodv_rreq_attempt_t *right)
{
    return left != NULL && right != NULL &&
        left->discovery_correlation == right->discovery_correlation &&
        logical_id_equal(&left->origin, &right->origin) &&
        logical_id_equal(&left->destination, &right->destination) &&
        left->request_id == right->request_id &&
        left->initial_scope == right->initial_scope &&
        left->current_scope == right->current_scope &&
        left->ring_ordinal == right->ring_ordinal;
}

static int router_trace_terminal_is_valid(const tavrn_router_phase_trace_t *trace)
{
    if (trace == NULL || !router_presence_is_valid(trace->terminal_fault_present) ||
        !router_fault_is_valid(trace->terminal_fault)) {
        return 0;
    }
    if (trace->terminal_fault_present == TAVRN_ROUTER_TRACE_NOT_PRESENT) {
        return trace->terminal_fault == TAVRN_ROUTER_FAULT_NONE;
    }
    return trace->terminal_fault != TAVRN_ROUTER_FAULT_NONE;
}

static int router_dispatch_trace_is_valid(const tavrn_router_dispatch_trace_t *trace)
{
    const aodv_action_t *action;

    if (trace == NULL || !router_event_status_is_valid(trace->status) ||
        !router_presence_is_valid(trace->action_present) ||
        !router_presence_is_valid(trace->rreq_enqueue_present) ||
        !router_presence_is_valid(trace->link_send_present) ||
        !router_presence_is_valid(trace->link_event_present)) {
        return 0;
    }
    if (trace->action_present == TAVRN_ROUTER_TRACE_NOT_PRESENT) {
        return trace->rreq_enqueue_present == TAVRN_ROUTER_TRACE_NOT_PRESENT &&
            trace->link_send_present == TAVRN_ROUTER_TRACE_NOT_PRESENT;
    }
    action = &trace->action;
    if (action->type <= AODV_ACTION_NONE ||
        action->type > AODV_ACTION_BLACKLIST_NEIGHBOR) {
        return 0;
    }
    if (trace->rreq_enqueue_present == TAVRN_ROUTER_TRACE_PRESENT) {
        if (action->type != AODV_ACTION_SEND_RREQ ||
            action->detail.control.rreq_attempt_present != AODV_RREQ_ATTEMPT_PRESENT ||
            (trace->rreq_enqueue.outcome != AODV_RREQ_ENQUEUE_ADMITTED &&
             trace->rreq_enqueue.outcome != AODV_RREQ_ENQUEUE_NOT_ADMITTED &&
             trace->rreq_enqueue.outcome != AODV_RREQ_ENQUEUE_UNKNOWN) ||
            !rreq_attempt_equal(&action->detail.control.rreq_attempt,
                                &trace->rreq_enqueue.attempt)) {
            return 0;
        }
    }
    if (trace->link_send_present == TAVRN_ROUTER_TRACE_PRESENT &&
        (!action_type_is_link_sendable(action->type) ||
         !link_send_status_is_valid(trace->link_send_status))) {
        return 0;
    }
    return trace->link_event_present == TAVRN_ROUTER_TRACE_NOT_PRESENT ||
        link_event_type_is_valid(trace->link_event.type);
}

static int router_link_service_trace_is_valid(
    const tavrn_router_link_service_trace_t *trace)
{
    if (trace == NULL || !router_event_status_is_valid(trace->status) ||
        !link_step_status_is_valid(trace->link_step_status) ||
        !router_presence_is_valid(trace->link_event_present)) {
        return 0;
    }
    if (trace->link_step_status == TAVRN_LINK_STEP_EVENT) {
        return trace->link_event_present == TAVRN_ROUTER_TRACE_PRESENT &&
            link_event_type_is_valid(trace->link_event.type) &&
            trace->link_event.type != TAVRN_LINK_EVENT_NONE;
    }
    return trace->link_event_present == TAVRN_ROUTER_TRACE_NOT_PRESENT;
}

static int router_trace_is_valid(const tavrn_router_phase_trace_t *trace,
                                 tavrn_router_trace_phase_t expected_phase)
{
    if (trace == NULL || trace->phase != expected_phase ||
        !router_trace_terminal_is_valid(trace)) {
        return 0;
    }
    switch (expected_phase) {
    case TAVRN_ROUTER_TRACE_SCHEDULER_EVENT:
        return router_event_status_is_valid(trace->detail.scheduler_event.status);
    case TAVRN_ROUTER_TRACE_TICK:
        return aodv_status_is_valid(trace->detail.tick.status);
    case TAVRN_ROUTER_TRACE_APPLICATION_SUBMIT:
        return aodv_status_is_valid(trace->detail.submit.status);
    case TAVRN_ROUTER_TRACE_DISPATCH:
        return router_dispatch_trace_is_valid(&trace->detail.dispatch);
    case TAVRN_ROUTER_TRACE_LINK_SERVICE:
        return router_link_service_trace_is_valid(&trace->detail.link_service);
    default:
        return 0;
    }
}

static int router_trace_is_terminal(const tavrn_router_phase_trace_t *trace,
                                    tavrn_router_trace_phase_t phase)
{
    if (trace == NULL || trace->terminal_fault_present == TAVRN_ROUTER_TRACE_PRESENT) {
        return 1;
    }
    if (phase == TAVRN_ROUTER_TRACE_TICK) {
        return trace->detail.tick.status == AODV_STATUS_INVALID;
    }
    if (phase == TAVRN_ROUTER_TRACE_APPLICATION_SUBMIT) {
        return trace->detail.submit.status == AODV_STATUS_INVALID;
    }
    if (phase == TAVRN_ROUTER_TRACE_SCHEDULER_EVENT) {
        return trace->detail.scheduler_event.status == TAVRN_ROUTER_EVENT_INVALID;
    }
    if (phase == TAVRN_ROUTER_TRACE_DISPATCH) {
        return trace->detail.dispatch.status == TAVRN_ROUTER_EVENT_INVALID;
    }
    return trace->detail.link_service.status == TAVRN_ROUTER_EVENT_INVALID;
}

static int cycle_is_valid(const routed_cycle_t *cycle)
{
    return cycle != NULL && cycle->poll_bound_ms != 0u &&
        cycle->poll_bound_ms < ROUTED_CYCLE_HALF_RANGE &&
        cycle->scheduler_return_seeded <= 1u && cycle->fault_latched <= 1u &&
        cycle_phase_or_not_applicable_is_valid(cycle->last_gap_phase) &&
        cycle_phase_or_not_applicable_is_valid(cycle->first_over_budget_phase);
}

static int operations_support_normal_cycle(const routed_cycle_operations_t *operations)
{
    return operations != NULL && operations->scheduler_poll != NULL &&
        operations->router_scheduler_event != NULL && operations->router_tick != NULL &&
        operations->application_prepare != NULL && operations->router_submit != NULL &&
        operations->router_dispatch != NULL && operations->router_link_service != NULL &&
        operations->healthy_yield != NULL && operations->fault_idle != NULL;
}

static int completion_timestamp_is_ordered(uint32_t phase_started_ms,
                                           uint32_t phase_completed_ms)
{
    return phase_completed_ms - phase_started_ms < ROUTED_CYCLE_HALF_RANGE;
}

static int trace_begin(routed_cycle_trace_t *trace, const routed_cycle_t *cycle,
                       routed_cycle_phase_t phase, uint32_t cycle_started_ms,
                       uint32_t phase_started_ms, uint32_t phase_completed_ms,
                       uint32_t scheduler_return_ms)
{
    if (!completion_timestamp_is_ordered(phase_started_ms, phase_completed_ms)) {
        return 0;
    }
    routed_cycle_trace_reset(trace);
    trace->phase = phase;
    trace->fault_latched = ROUTED_CYCLE_BOOLEAN_FALSE;
    trace->timing.cycle_started_ms = cycle_started_ms;
    trace->timing.phase_started_ms = phase_started_ms;
    trace->timing.phase_completed_ms = phase_completed_ms;
    trace->timing.phase_elapsed_ms = phase_completed_ms - phase_started_ms;
    trace->timing.scheduler_return_ms = scheduler_return_ms;
    trace->timing.elapsed_since_scheduler_return_ms =
        phase_completed_ms - scheduler_return_ms;
    trace->timing.poll_bound_ms = cycle->poll_bound_ms;
    trace->timing.phase = phase;
    trace->timing.over_budget_phase = ROUTED_CYCLE_PHASE_NOT_APPLICABLE;
    trace->timing.over_budget_source = ROUTED_CYCLE_BUDGET_SOURCE_NOT_APPLICABLE;
    trace->timing.scheduler_return_state = ROUTED_CYCLE_VALUE_KNOWN;
    trace->timing.poll_bound_state = ROUTED_CYCLE_VALUE_KNOWN;
    trace->timing.over_budget = ROUTED_CYCLE_BOOLEAN_FALSE;
    return 1;
}

static int trace_mark_budget(routed_cycle_t *cycle, routed_cycle_trace_t *trace)
{
    uint32_t elapsed;

    elapsed = trace->timing.elapsed_since_scheduler_return_ms;
    if (elapsed == ROUTED_CYCLE_HALF_RANGE) {
        trace->timing.over_budget = ROUTED_CYCLE_BOOLEAN_TRUE;
        trace->timing.over_budget_source =
            ROUTED_CYCLE_BUDGET_SOURCE_AMBIGUOUS_HALF_RANGE;
    } else if (elapsed > cycle->poll_bound_ms) {
        trace->timing.over_budget = ROUTED_CYCLE_BOOLEAN_TRUE;
        trace->timing.over_budget_source =
            ROUTED_CYCLE_BUDGET_SOURCE_SCHEDULER_RETURN_GAP;
    } else {
        return 0;
    }
    if (cycle->first_over_budget_phase == ROUTED_CYCLE_PHASE_NOT_APPLICABLE) {
        cycle->first_over_budget_phase = trace->phase;
    }
    trace->timing.over_budget_phase = cycle->first_over_budget_phase;
    return 1;
}

static int trace_append(const routed_cycle_operations_t *operations,
                        routed_cycle_run_result_t *result,
                        const routed_cycle_trace_t *trace)
{
    uint8_t index;

    if (operations == NULL || result == NULL || trace == NULL ||
        result->trace_count >= ROUTED_CYCLE_TRACE_CAPACITY) {
        return 0;
    }
    index = result->trace_count;
    result->traces[index] = *trace;
    result->trace_count++;
    if (operations->trace_sink != NULL) {
        operations->trace_sink(operations->context, &result->traces[index]);
    }
    return 1;
}

static routed_cycle_run_status_t emit_fault_idle(
    routed_cycle_t *cycle, const routed_cycle_operations_t *operations,
    uint32_t cycle_started_ms, uint32_t phase_started_ms,
    routed_cycle_run_result_t *result_out)
{
    routed_cycle_fault_idle_result_t idle_result;
    routed_cycle_trace_t trace;

    idle_result = operations->fault_idle(operations->context, phase_started_ms);
    if (!fault_idle_status_is_valid(idle_result.status)) {
        result_out->status = ROUTED_CYCLE_RUN_INVALID;
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (!trace_begin(&trace, cycle, ROUTED_CYCLE_PHASE_FAULT_IDLE,
                     cycle_started_ms, phase_started_ms,
                     idle_result.completed_at_ms,
                     cycle->last_scheduler_return_ms)) {
        result_out->status = ROUTED_CYCLE_RUN_INVALID;
        return ROUTED_CYCLE_RUN_INVALID;
    }
    trace.fault_latched = ROUTED_CYCLE_BOOLEAN_TRUE;
    trace.detail.fault_idle = idle_result;
    if (!trace_append(operations, result_out, &trace)) {
        result_out->status = ROUTED_CYCLE_RUN_INVALID;
        return ROUTED_CYCLE_RUN_INVALID;
    }
    result_out->status = ROUTED_CYCLE_RUN_FAULT_IDLE;
    return ROUTED_CYCLE_RUN_FAULT_IDLE;
}

routed_cycle_result_t routed_cycle_init(routed_cycle_t *cycle,
                                         uint32_t poll_bound_ms)
{
    routed_cycle_t initialized;

    if (cycle == NULL || poll_bound_ms == 0u ||
        poll_bound_ms >= ROUTED_CYCLE_HALF_RANGE) {
        return ROUTED_CYCLE_RESULT_INVALID;
    }
    memset(&initialized, 0, sizeof(initialized));
    initialized.poll_bound_ms = poll_bound_ms;
    initialized.last_gap_phase = ROUTED_CYCLE_PHASE_NOT_APPLICABLE;
    initialized.first_over_budget_phase = ROUTED_CYCLE_PHASE_NOT_APPLICABLE;
    *cycle = initialized;
    return ROUTED_CYCLE_RESULT_OK;
}

routed_cycle_result_t routed_cycle_seed_startup_rx_return(
    routed_cycle_t *cycle, const routed_cycle_operations_t *operations)
{
    routed_cycle_start_rx_result_t start_rx;

    if (!cycle_is_valid(cycle) || cycle->scheduler_return_seeded != 0u ||
        cycle->fault_latched != 0u || operations == NULL ||
        operations->start_rx == NULL) {
        return ROUTED_CYCLE_RESULT_INVALID;
    }
    start_rx = operations->start_rx(operations->context);
    if (start_rx.status != ROUTED_CYCLE_START_RX_OK) {
        return ROUTED_CYCLE_RESULT_INVALID;
    }
    cycle->last_scheduler_return_ms = start_rx.returned_at_ms;
    cycle->last_gap_phase = ROUTED_CYCLE_PHASE_STARTUP_RX_RETURN;
    cycle->first_over_budget_phase = ROUTED_CYCLE_PHASE_NOT_APPLICABLE;
    cycle->scheduler_return_seeded = 1u;
    return ROUTED_CYCLE_RESULT_OK;
}

routed_cycle_run_status_t routed_cycle_run_once(
    routed_cycle_t *cycle, const routed_cycle_operations_t *operations,
    uint32_t poll_started_at_ms, routed_cycle_run_result_t *result_out)
{
    routed_cycle_trace_t trace;
    routed_cycle_scheduler_poll_result_t poll;
    tavrn_router_phase_trace_t router_trace;
    routed_cycle_application_request_t request;
    routed_cycle_yield_result_t yield_result;
    uint32_t phase_started_at_ms;
    uint32_t elapsed;
    uint32_t voluntary_yield_ms;
    uint32_t available_yield_ms;
    uint32_t requested_yield_ms;
    int terminal;

    if (!cycle_is_valid(cycle) || operations == NULL || result_out == NULL) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (cycle->fault_latched != 0u) {
        if (operations->fault_idle == NULL) {
            return ROUTED_CYCLE_RUN_INVALID;
        }
        memset(result_out, 0, sizeof(*result_out));
        result_out->status = ROUTED_CYCLE_RUN_INVALID;
        return emit_fault_idle(cycle, operations, poll_started_at_ms,
                               poll_started_at_ms, result_out);
    }
    if (cycle->scheduler_return_seeded == 0u ||
        !operations_support_normal_cycle(operations)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }

    memset(result_out, 0, sizeof(*result_out));
    result_out->status = ROUTED_CYCLE_RUN_INVALID;
    elapsed = poll_started_at_ms - cycle->last_scheduler_return_ms;
    if (elapsed == ROUTED_CYCLE_HALF_RANGE) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (!trace_begin(&trace, cycle, ROUTED_CYCLE_PHASE_PRE_POLL_BOUND,
                     poll_started_at_ms, poll_started_at_ms, poll_started_at_ms,
                     cycle->last_scheduler_return_ms)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    trace.detail.pre_poll.status = elapsed > cycle->poll_bound_ms ?
        ROUTED_CYCLE_PRE_POLL_OVER_BUDGET : ROUTED_CYCLE_PRE_POLL_HEALTHY;
    if (elapsed > cycle->poll_bound_ms) {
        trace.timing.over_budget = ROUTED_CYCLE_BOOLEAN_TRUE;
        trace.timing.over_budget_phase = ROUTED_CYCLE_PHASE_PRE_POLL_BOUND;
        trace.timing.over_budget_source =
            ROUTED_CYCLE_BUDGET_SOURCE_SCHEDULER_RETURN_GAP;
        cycle->first_over_budget_phase = ROUTED_CYCLE_PHASE_PRE_POLL_BOUND;
        cycle->fault_latched = 1u;
        trace.fault_latched = ROUTED_CYCLE_BOOLEAN_TRUE;
    }
    cycle->last_gap_phase = ROUTED_CYCLE_PHASE_PRE_POLL_BOUND;
    if (!trace_append(operations, result_out, &trace)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (cycle->fault_latched != 0u) {
        return emit_fault_idle(cycle, operations, poll_started_at_ms,
                               poll_started_at_ms, result_out);
    }

    poll = operations->scheduler_poll(operations->context, poll_started_at_ms);
    if (!scheduler_poll_result_is_valid(&poll)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (!trace_begin(&trace, cycle, ROUTED_CYCLE_PHASE_SCHEDULER_POLL,
                     poll_started_at_ms, poll_started_at_ms, poll.returned_at_ms,
                     poll.returned_at_ms)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    cycle->last_scheduler_return_ms = poll.returned_at_ms;
    cycle->last_gap_phase = ROUTED_CYCLE_PHASE_SCHEDULER_POLL;
    trace.detail.scheduler_poll = poll;
    if (poll.status == ROUTED_CYCLE_SCHEDULER_POLL_INVALID) {
        cycle->fault_latched = 1u;
        trace.fault_latched = ROUTED_CYCLE_BOOLEAN_TRUE;
    }
    if (!trace_append(operations, result_out, &trace)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (cycle->fault_latched != 0u) {
        return emit_fault_idle(cycle, operations, poll_started_at_ms,
                               poll.returned_at_ms, result_out);
    }

    phase_started_at_ms = poll.returned_at_ms;
    if (poll.event.type != BLE_MESH_SCHED_EVENT_NONE) {
        router_trace = operations->router_scheduler_event(
            operations->context, &poll.event, phase_started_at_ms);
        if (!router_trace_is_valid(&router_trace,
                                   TAVRN_ROUTER_TRACE_SCHEDULER_EVENT)) {
            return ROUTED_CYCLE_RUN_INVALID;
        }
        if (!trace_begin(&trace, cycle, ROUTED_CYCLE_PHASE_SCHEDULER_EVENT,
                         poll_started_at_ms, phase_started_at_ms,
                         router_trace.completed_at_ms,
                         cycle->last_scheduler_return_ms)) {
            return ROUTED_CYCLE_RUN_INVALID;
        }
        trace.detail.router = router_trace;
        terminal = trace_mark_budget(cycle, &trace) ||
            router_trace_is_terminal(&router_trace,
                                     TAVRN_ROUTER_TRACE_SCHEDULER_EVENT) ||
            poll.event.type == BLE_MESH_SCHED_EVENT_RADIO_FAULT ||
            poll.event.type == BLE_MESH_SCHED_EVENT_SERVICE_FAULT;
        if (terminal != 0) {
            cycle->fault_latched = 1u;
            trace.fault_latched = ROUTED_CYCLE_BOOLEAN_TRUE;
        }
        cycle->last_gap_phase = ROUTED_CYCLE_PHASE_SCHEDULER_EVENT;
        if (!trace_append(operations, result_out, &trace)) {
            return ROUTED_CYCLE_RUN_INVALID;
        }
        if (terminal != 0) {
            return emit_fault_idle(cycle, operations, poll_started_at_ms,
                                   router_trace.completed_at_ms, result_out);
        }
        phase_started_at_ms = router_trace.completed_at_ms;
    }

    router_trace = operations->router_tick(operations->context, phase_started_at_ms);
    if (!router_trace_is_valid(&router_trace, TAVRN_ROUTER_TRACE_TICK)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (!trace_begin(&trace, cycle, ROUTED_CYCLE_PHASE_ROUTER_TICK,
                     poll_started_at_ms, phase_started_at_ms,
                     router_trace.completed_at_ms,
                     cycle->last_scheduler_return_ms)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    trace.detail.router = router_trace;
    terminal = trace_mark_budget(cycle, &trace) ||
        router_trace_is_terminal(&router_trace, TAVRN_ROUTER_TRACE_TICK);
    if (terminal != 0) {
        cycle->fault_latched = 1u;
        trace.fault_latched = ROUTED_CYCLE_BOOLEAN_TRUE;
    }
    cycle->last_gap_phase = ROUTED_CYCLE_PHASE_ROUTER_TICK;
    if (!trace_append(operations, result_out, &trace)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (terminal != 0) {
        return emit_fault_idle(cycle, operations, poll_started_at_ms,
                               router_trace.completed_at_ms, result_out);
    }
    if (operations->take_scheduler_return_after_tick != NULL &&
        operations->take_scheduler_return_after_tick(operations->context) != 0u) {
        result_out->status = ROUTED_CYCLE_RUN_OK;
        return ROUTED_CYCLE_RUN_OK;
    }

    phase_started_at_ms = router_trace.completed_at_ms;
    request = operations->application_prepare(operations->context, phase_started_at_ms);
    if (!application_status_is_valid(request.status)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (!trace_begin(&trace, cycle, ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT,
                     poll_started_at_ms, phase_started_at_ms, request.prepared_at_ms,
                     cycle->last_scheduler_return_ms)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    trace.detail.application_submit.request = request;
    trace.detail.application_submit.submit_present = TAVRN_ROUTER_TRACE_NOT_PRESENT;
    terminal = request.status == ROUTED_CYCLE_APPLICATION_INVALID;
    if (request.status == ROUTED_CYCLE_APPLICATION_READY) {
        router_trace = operations->router_submit(operations->context, &request.data,
                                                 request.prepared_at_ms);
        if (!router_trace_is_valid(&router_trace,
                                   TAVRN_ROUTER_TRACE_APPLICATION_SUBMIT)) {
            return ROUTED_CYCLE_RUN_INVALID;
        }
        if (!completion_timestamp_is_ordered(request.prepared_at_ms,
                                             router_trace.completed_at_ms) ||
            !completion_timestamp_is_ordered(trace.timing.phase_started_ms,
                                             router_trace.completed_at_ms)) {
            return ROUTED_CYCLE_RUN_INVALID;
        }
        trace.detail.application_submit.submit = router_trace;
        trace.detail.application_submit.submit_present = TAVRN_ROUTER_TRACE_PRESENT;
        trace.timing.phase_completed_ms = router_trace.completed_at_ms;
        trace.timing.phase_elapsed_ms = router_trace.completed_at_ms -
            trace.timing.phase_started_ms;
        trace.timing.elapsed_since_scheduler_return_ms =
            router_trace.completed_at_ms - trace.timing.scheduler_return_ms;
        terminal = terminal != 0 || router_trace_is_terminal(
            &router_trace, TAVRN_ROUTER_TRACE_APPLICATION_SUBMIT);
    }
    terminal = trace_mark_budget(cycle, &trace) || terminal;
    if (terminal != 0) {
        cycle->fault_latched = 1u;
        trace.fault_latched = ROUTED_CYCLE_BOOLEAN_TRUE;
    }
    cycle->last_gap_phase = ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT;
    if (!trace_append(operations, result_out, &trace)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (terminal != 0) {
        return emit_fault_idle(cycle, operations, poll_started_at_ms,
                               trace.timing.phase_completed_ms, result_out);
    }

    phase_started_at_ms = trace.timing.phase_completed_ms;
    router_trace = operations->router_dispatch(operations->context, phase_started_at_ms);
    if (!router_trace_is_valid(&router_trace, TAVRN_ROUTER_TRACE_DISPATCH)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (!trace_begin(&trace, cycle, ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH,
                     poll_started_at_ms, phase_started_at_ms,
                     router_trace.completed_at_ms,
                     cycle->last_scheduler_return_ms)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    trace.detail.router = router_trace;
    terminal = trace_mark_budget(cycle, &trace) ||
        router_trace_is_terminal(&router_trace, TAVRN_ROUTER_TRACE_DISPATCH);
    if (terminal != 0) {
        cycle->fault_latched = 1u;
        trace.fault_latched = ROUTED_CYCLE_BOOLEAN_TRUE;
    }
    cycle->last_gap_phase = ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH;
    if (!trace_append(operations, result_out, &trace)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (terminal != 0) {
        return emit_fault_idle(cycle, operations, poll_started_at_ms,
                               router_trace.completed_at_ms, result_out);
    }

    phase_started_at_ms = router_trace.completed_at_ms;
    router_trace = operations->router_link_service(operations->context,
                                                    phase_started_at_ms);
    if (!router_trace_is_valid(&router_trace, TAVRN_ROUTER_TRACE_LINK_SERVICE)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (!trace_begin(&trace, cycle, ROUTED_CYCLE_PHASE_LINK_SERVICE,
                     poll_started_at_ms, phase_started_at_ms,
                     router_trace.completed_at_ms,
                     cycle->last_scheduler_return_ms)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    trace.detail.router = router_trace;
    terminal = trace_mark_budget(cycle, &trace) ||
        router_trace_is_terminal(&router_trace, TAVRN_ROUTER_TRACE_LINK_SERVICE);
    if (terminal != 0) {
        cycle->fault_latched = 1u;
        trace.fault_latched = ROUTED_CYCLE_BOOLEAN_TRUE;
    }
    cycle->last_gap_phase = ROUTED_CYCLE_PHASE_LINK_SERVICE;
    if (!trace_append(operations, result_out, &trace)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (terminal != 0) {
        return emit_fault_idle(cycle, operations, poll_started_at_ms,
                               router_trace.completed_at_ms, result_out);
    }

    voluntary_yield_ms = cycle->poll_bound_ms > 1u ? cycle->poll_bound_ms - 1u : 0u;
    available_yield_ms = trace.timing.elapsed_since_scheduler_return_ms <
        voluntary_yield_ms ? voluntary_yield_ms -
        trace.timing.elapsed_since_scheduler_return_ms : 0u;
    if (available_yield_ms <= operations->healthy_wait_completion_guard_ms) {
        result_out->status = ROUTED_CYCLE_RUN_OK;
        return ROUTED_CYCLE_RUN_OK;
    }
    requested_yield_ms = available_yield_ms -
        operations->healthy_wait_completion_guard_ms;
    yield_result = operations->healthy_yield(operations->context, requested_yield_ms,
                                             router_trace.completed_at_ms);
    if (!yield_status_is_valid(yield_result.status)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (!trace_begin(&trace, cycle, ROUTED_CYCLE_PHASE_HEALTHY_YIELD,
                     poll_started_at_ms, router_trace.completed_at_ms,
                     yield_result.completed_at_ms,
                     cycle->last_scheduler_return_ms)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    trace.detail.healthy_yield.requested_slack_ms = requested_yield_ms;
    trace.detail.healthy_yield.result = yield_result;
    terminal = trace_mark_budget(cycle, &trace) ||
        yield_result.status != ROUTED_CYCLE_YIELD_OK;
    if (terminal != 0) {
        cycle->fault_latched = 1u;
        trace.fault_latched = ROUTED_CYCLE_BOOLEAN_TRUE;
    }
    cycle->last_gap_phase = ROUTED_CYCLE_PHASE_HEALTHY_YIELD;
    if (!trace_append(operations, result_out, &trace)) {
        return ROUTED_CYCLE_RUN_INVALID;
    }
    if (terminal != 0) {
        return emit_fault_idle(cycle, operations, poll_started_at_ms,
                               yield_result.completed_at_ms, result_out);
    }
    result_out->status = ROUTED_CYCLE_RUN_OK;
    return ROUTED_CYCLE_RUN_OK;
}

routed_cycle_result_t routed_cycle_task_latch_run_invalid(
    routed_cycle_t *cycle, uint32_t iteration_limit)
{
    if (!cycle_is_valid(cycle) || cycle->scheduler_return_seeded == 0u ||
        iteration_limit != 0u) {
        return ROUTED_CYCLE_RESULT_INVALID;
    }
    cycle->fault_latched = 1u;
    return ROUTED_CYCLE_RESULT_OK;
}

routed_cycle_task_status_t routed_cycle_run_task(
    routed_cycle_t *cycle, const routed_cycle_operations_t *operations,
    uint32_t iteration_limit, routed_cycle_run_result_t *result_storage)
{
    routed_cycle_task_status_t task_status = ROUTED_CYCLE_TASK_OK;
    uint32_t iteration = 0u;

    if (!cycle_is_valid(cycle) || operations == NULL || result_storage == NULL ||
        operations->now_ms == NULL ||
        (cycle->fault_latched == 0u && !operations_support_normal_cycle(operations)) ||
        (cycle->fault_latched != 0u && operations->fault_idle == NULL)) {
        return ROUTED_CYCLE_TASK_INVALID;
    }
    if (cycle->scheduler_return_seeded == 0u &&
        routed_cycle_seed_startup_rx_return(cycle, operations) !=
            ROUTED_CYCLE_RESULT_OK) {
        return ROUTED_CYCLE_TASK_INVALID;
    }
    for (;;) {
        routed_cycle_run_status_t status;

        if (iteration_limit != 0u && iteration >= iteration_limit) {
            return task_status;
        }
        status = routed_cycle_run_once(cycle, operations,
                                       operations->now_ms(operations->context),
                                       result_storage);
        if (status == ROUTED_CYCLE_RUN_INVALID) {
            if (routed_cycle_task_latch_run_invalid(cycle, iteration_limit) !=
                ROUTED_CYCLE_RESULT_OK) {
                return ROUTED_CYCLE_TASK_INVALID;
            }
        }
        if (status == ROUTED_CYCLE_RUN_FAULT_IDLE) {
            task_status = ROUTED_CYCLE_TASK_FAULT_IDLE;
        }
        iteration++;
    }
}

void routed_cycle_trace_reset(routed_cycle_trace_t *trace)
{
    if (trace != NULL) {
        memset(trace, 0, sizeof(*trace));
    }
}

static int timing_is_valid(const routed_cycle_timing_t *timing,
                           routed_cycle_phase_t phase)
{
    return timing != NULL && timing->phase == phase &&
        cycle_phase_or_not_applicable_is_valid(timing->over_budget_phase) &&
        (timing->over_budget_source >= ROUTED_CYCLE_BUDGET_SOURCE_NOT_APPLICABLE &&
         timing->over_budget_source <=
             ROUTED_CYCLE_BUDGET_SOURCE_AMBIGUOUS_HALF_RANGE) &&
        value_state_is_valid(timing->scheduler_return_state) &&
        value_state_is_valid(timing->poll_bound_state) &&
        boolean_is_valid(timing->over_budget);
}

static int trace_is_valid(const routed_cycle_trace_t *trace)
{
    if (trace == NULL || !cycle_phase_is_traceable(trace->phase) ||
        !timing_is_valid(&trace->timing, trace->phase) ||
        (trace->fault_latched != ROUTED_CYCLE_BOOLEAN_FALSE &&
         trace->fault_latched != ROUTED_CYCLE_BOOLEAN_TRUE)) {
        return 0;
    }
    switch (trace->phase) {
    case ROUTED_CYCLE_PHASE_PRE_POLL_BOUND:
        return trace->detail.pre_poll.status >= ROUTED_CYCLE_PRE_POLL_HEALTHY &&
            trace->detail.pre_poll.status <=
                ROUTED_CYCLE_PRE_POLL_AMBIGUOUS_HALF_RANGE;
    case ROUTED_CYCLE_PHASE_SCHEDULER_POLL:
        return scheduler_poll_result_is_valid(&trace->detail.scheduler_poll);
    case ROUTED_CYCLE_PHASE_SCHEDULER_EVENT:
        return router_trace_is_valid(&trace->detail.router,
                                     TAVRN_ROUTER_TRACE_SCHEDULER_EVENT);
    case ROUTED_CYCLE_PHASE_ROUTER_TICK:
        return router_trace_is_valid(&trace->detail.router, TAVRN_ROUTER_TRACE_TICK);
    case ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT:
        if (!application_status_is_valid(trace->detail.application_submit.request.status) ||
            !router_presence_is_valid(trace->detail.application_submit.submit_present)) {
            return 0;
        }
        if (trace->detail.application_submit.request.status ==
            ROUTED_CYCLE_APPLICATION_READY) {
            return trace->detail.application_submit.submit_present ==
                TAVRN_ROUTER_TRACE_PRESENT && router_trace_is_valid(
                    &trace->detail.application_submit.submit,
                    TAVRN_ROUTER_TRACE_APPLICATION_SUBMIT);
        }
        return trace->detail.application_submit.submit_present ==
            TAVRN_ROUTER_TRACE_NOT_PRESENT;
    case ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH:
        return router_trace_is_valid(&trace->detail.router,
                                     TAVRN_ROUTER_TRACE_DISPATCH);
    case ROUTED_CYCLE_PHASE_LINK_SERVICE:
        return router_trace_is_valid(&trace->detail.router,
                                     TAVRN_ROUTER_TRACE_LINK_SERVICE);
    case ROUTED_CYCLE_PHASE_HEALTHY_YIELD:
        return trace->detail.healthy_yield.requested_slack_ms != 0u &&
            (trace->detail.healthy_yield.result.status == ROUTED_CYCLE_YIELD_OK ||
             trace->detail.healthy_yield.result.status == ROUTED_CYCLE_YIELD_INVALID);
    case ROUTED_CYCLE_PHASE_FAULT_IDLE:
        return trace->detail.fault_idle.status == ROUTED_CYCLE_FAULT_IDLE_OK;
    default:
        return 0;
    }
}

static int diagnostic_queue_is_valid(const routed_cycle_diagnostic_queue_t *queue)
{
    uint8_t expected_tail;

    if (queue == NULL || queue->head >= ROUTED_CYCLE_DIAGNOSTIC_CAPACITY ||
        queue->tail >= ROUTED_CYCLE_DIAGNOSTIC_CAPACITY ||
        queue->count > ROUTED_CYCLE_DIAGNOSTIC_CAPACITY) {
        return 0;
    }
    expected_tail = (uint8_t)((queue->head + queue->count) %
                              ROUTED_CYCLE_DIAGNOSTIC_CAPACITY);
    return queue->tail == expected_tail;
}

void routed_cycle_diagnostic_queue_init(routed_cycle_diagnostic_queue_t *queue)
{
    if (queue != NULL) {
        memset(queue, 0, sizeof(*queue));
    }
}

routed_cycle_result_t routed_cycle_diagnostic_enqueue(
    routed_cycle_diagnostic_queue_t *queue, const routed_cycle_trace_t *trace)
{
    uint8_t remove_offset;
    uint8_t index;
    uint8_t next_index;

    if (!diagnostic_queue_is_valid(queue) || !trace_is_valid(trace)) {
        return ROUTED_CYCLE_RESULT_INVALID;
    }
    if (queue->count < ROUTED_CYCLE_DIAGNOSTIC_CAPACITY) {
        queue->records[queue->tail] = *trace;
        queue->tail = (uint8_t)((queue->tail + 1u) %
                                ROUTED_CYCLE_DIAGNOSTIC_CAPACITY);
        queue->count++;
        return ROUTED_CYCLE_RESULT_OK;
    }
    if (trace->fault_latched != ROUTED_CYCLE_BOOLEAN_TRUE) {
        queue->dropped_count++;
        return ROUTED_CYCLE_RESULT_DROPPED;
    }
    for (remove_offset = 0u; remove_offset < queue->count; remove_offset++) {
        index = (uint8_t)((queue->head + remove_offset) %
                          ROUTED_CYCLE_DIAGNOSTIC_CAPACITY);
        if (queue->records[index].fault_latched == ROUTED_CYCLE_BOOLEAN_FALSE) {
            break;
        }
    }
    if (remove_offset == queue->count) {
        queue->dropped_fault_count++;
        return ROUTED_CYCLE_RESULT_DROPPED;
    }
    for (index = remove_offset; index + 1u < queue->count; index++) {
        uint8_t destination = (uint8_t)((queue->head + index) %
                                        ROUTED_CYCLE_DIAGNOSTIC_CAPACITY);

        next_index = (uint8_t)((destination + 1u) %
                               ROUTED_CYCLE_DIAGNOSTIC_CAPACITY);
        queue->records[destination] = queue->records[next_index];
    }
    queue->tail = (uint8_t)((queue->tail + ROUTED_CYCLE_DIAGNOSTIC_CAPACITY - 1u) %
                            ROUTED_CYCLE_DIAGNOSTIC_CAPACITY);
    queue->count--;
    queue->evicted_healthy_count++;
    queue->records[queue->tail] = *trace;
    queue->tail = (uint8_t)((queue->tail + 1u) %
                            ROUTED_CYCLE_DIAGNOSTIC_CAPACITY);
    queue->count++;
    return ROUTED_CYCLE_RESULT_OK;
}

routed_cycle_result_t routed_cycle_diagnostic_dequeue(
    routed_cycle_diagnostic_queue_t *queue, routed_cycle_trace_t *trace_out)
{
    if (!diagnostic_queue_is_valid(queue) || trace_out == NULL) {
        return ROUTED_CYCLE_RESULT_INVALID;
    }
    if (queue->count == 0u) {
        return ROUTED_CYCLE_RESULT_EMPTY;
    }
    if (!trace_is_valid(&queue->records[queue->head])) {
        return ROUTED_CYCLE_RESULT_INVALID;
    }
    *trace_out = queue->records[queue->head];
    queue->head = (uint8_t)((queue->head + 1u) % ROUTED_CYCLE_DIAGNOSTIC_CAPACITY);
    queue->count--;
    return ROUTED_CYCLE_RESULT_OK;
}

static int logical_id_is_valid(const tavrn_logical_id_t *identity)
{
    if (identity == NULL) {
        return 0;
    }
    if (identity->width == TAVRN_IDENTITY_SID16) {
        return identity->value != 0u && identity->value != 0xffffu;
    }
    if (identity->width == TAVRN_IDENTITY_SID8) {
        return (identity->value & 0xff00u) == 0u && identity->value != 0u &&
            identity->value != 0x00ffu;
    }
    return 0;
}

static int rreq_value_state_is_observed(aodv_rreq_value_state_t state)
{
    return state == AODV_RREQ_VALUE_KNOWN || state == AODV_RREQ_VALUE_UNKNOWN;
}

static int rreq_record_is_valid(const aodv_rreq_lifecycle_record_t *record)
{
    if (record == NULL || record->attempt.discovery_correlation == 0u ||
        !logical_id_is_valid(&record->attempt.origin) ||
        !logical_id_is_valid(&record->attempt.destination) ||
        record->attempt.initial_scope == 0u || record->attempt.initial_scope > 15u ||
        record->attempt.current_scope == 0u || record->attempt.current_scope > 15u ||
        record->scope_source < AODV_RREQ_SCOPE_NOT_APPLICABLE ||
        record->scope_source > AODV_RREQ_SCOPE_UNKNOWN ||
        record->ordinary < AODV_RREQ_TRUTH_NOT_APPLICABLE ||
        record->ordinary > AODV_RREQ_TRUTH_UNKNOWN ||
        record->scoped < AODV_RREQ_TRUTH_NOT_APPLICABLE ||
        record->scoped > AODV_RREQ_TRUTH_UNKNOWN ||
        record->hint_used < AODV_RREQ_TRUTH_NOT_APPLICABLE ||
        record->hint_used > AODV_RREQ_TRUTH_UNKNOWN ||
        record->fallback < AODV_RREQ_TRUTH_NOT_APPLICABLE ||
        record->fallback > AODV_RREQ_TRUTH_UNKNOWN ||
        record->full_diameter < AODV_RREQ_TRUTH_NOT_APPLICABLE ||
        record->full_diameter > AODV_RREQ_TRUTH_UNKNOWN) {
        return 0;
    }
    if (record->stage == AODV_RREQ_STAGE_ACTION_CREATED) {
        return rreq_value_state_is_observed(record->action_created_at_state) &&
            record->link_enqueue_at_state == AODV_RREQ_VALUE_NOT_APPLICABLE &&
            record->link_enqueue_at_ms == 0u &&
            (record->action_outcome == AODV_RREQ_ACTION_CREATED ||
             record->action_outcome == AODV_RREQ_ACTION_UNKNOWN) &&
            record->enqueue_outcome == AODV_RREQ_ENQUEUE_NOT_APPLICABLE;
    }
    if (record->stage == AODV_RREQ_STAGE_ROUTER_LINK_ENQUEUE) {
        return rreq_value_state_is_observed(record->action_created_at_state) &&
            rreq_value_state_is_observed(record->link_enqueue_at_state) &&
            (record->action_outcome == AODV_RREQ_ACTION_CREATED ||
             record->action_outcome == AODV_RREQ_ACTION_UNKNOWN) &&
            (record->enqueue_outcome == AODV_RREQ_ENQUEUE_ADMITTED ||
             record->enqueue_outcome == AODV_RREQ_ENQUEUE_NOT_ADMITTED ||
             record->enqueue_outcome == AODV_RREQ_ENQUEUE_UNKNOWN);
    }
    return 0;
}

static int rreq_queue_is_valid(const routed_cycle_rreq_queue_t *queue)
{
    uint8_t expected_tail;

    if (queue == NULL || queue->head >= ROUTED_CYCLE_RREQ_TELEMETRY_CAPACITY ||
        queue->tail >= ROUTED_CYCLE_RREQ_TELEMETRY_CAPACITY ||
        queue->count > ROUTED_CYCLE_RREQ_TELEMETRY_CAPACITY) {
        return 0;
    }
    expected_tail = (uint8_t)((queue->head + queue->count) %
                              ROUTED_CYCLE_RREQ_TELEMETRY_CAPACITY);
    return queue->tail == expected_tail;
}

void routed_cycle_rreq_queue_init(routed_cycle_rreq_queue_t *queue)
{
    if (queue != NULL) {
        memset(queue, 0, sizeof(*queue));
    }
}

routed_cycle_result_t routed_cycle_rreq_enqueue(
    routed_cycle_rreq_queue_t *queue,
    const aodv_rreq_lifecycle_record_t *record)
{
    if (!rreq_queue_is_valid(queue) || !rreq_record_is_valid(record)) {
        return ROUTED_CYCLE_RESULT_INVALID;
    }
    if (queue->count == ROUTED_CYCLE_RREQ_TELEMETRY_CAPACITY) {
        queue->dropped_count++;
        return ROUTED_CYCLE_RESULT_DROPPED;
    }
    queue->records[queue->tail] = *record;
    queue->tail = (uint8_t)((queue->tail + 1u) %
                            ROUTED_CYCLE_RREQ_TELEMETRY_CAPACITY);
    queue->count++;
    return ROUTED_CYCLE_RESULT_OK;
}

routed_cycle_result_t routed_cycle_rreq_dequeue(
    routed_cycle_rreq_queue_t *queue, aodv_rreq_lifecycle_record_t *record_out)
{
    if (!rreq_queue_is_valid(queue) || record_out == NULL) {
        return ROUTED_CYCLE_RESULT_INVALID;
    }
    if (queue->count == 0u) {
        return ROUTED_CYCLE_RESULT_EMPTY;
    }
    if (!rreq_record_is_valid(&queue->records[queue->head])) {
        return ROUTED_CYCLE_RESULT_INVALID;
    }
    *record_out = queue->records[queue->head];
    queue->head = (uint8_t)((queue->head + 1u) %
                            ROUTED_CYCLE_RREQ_TELEMETRY_CAPACITY);
    queue->count--;
    return ROUTED_CYCLE_RESULT_OK;
}
