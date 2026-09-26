#ifndef ROUTED_CYCLE_H
#define ROUTED_CYCLE_H

/*
 * Routed production-cycle contract.
 *
 * This common header deliberately includes scheduler/link/router/AODV public
 * types only.  AODV_ONLY can use every type here without a FULL_TAVRN or GTT
 * include.  FULL-only GTT conversion is isolated in routed_full_telemetry.h.
 */

#include <stdint.h>

#include "aodv_core.h"
#include "ble_mesh_scheduler.h"
#include "tavrn_router.h"

#define ROUTED_CYCLE_TRACE_CAPACITY       9u
#define ROUTED_CYCLE_DIAGNOSTIC_CAPACITY  8u
#define ROUTED_CYCLE_RETRY_LOG_CAPACITY   1u
#define ROUTED_CYCLE_RREQ_TELEMETRY_CAPACITY 8u
#define ROUTED_CYCLE_GTT_SNAPSHOT_CAPACITY 16u
#define ROUTED_CYCLE_HALF_RANGE 0x80000000u

typedef char routed_cycle_trace_capacity_guard[
    (ROUTED_CYCLE_TRACE_CAPACITY == 9u) ? 1 : -1];
typedef char routed_cycle_diagnostic_capacity_guard[
    (ROUTED_CYCLE_DIAGNOSTIC_CAPACITY == 8u) ? 1 : -1];
typedef char routed_cycle_retry_log_capacity_guard[
    (ROUTED_CYCLE_RETRY_LOG_CAPACITY == 1u) ? 1 : -1];
typedef char routed_cycle_rreq_telemetry_capacity_guard[
    (ROUTED_CYCLE_RREQ_TELEMETRY_CAPACITY == 8u) ? 1 : -1];
typedef char routed_cycle_gtt_snapshot_capacity_guard[
    (ROUTED_CYCLE_GTT_SNAPSHOT_CAPACITY == 16u) ? 1 : -1];

typedef enum routed_cycle_value_state {
    ROUTED_CYCLE_VALUE_NOT_APPLICABLE = 0,
    ROUTED_CYCLE_VALUE_KNOWN,
    ROUTED_CYCLE_VALUE_UNKNOWN,
} routed_cycle_value_state_t;

typedef enum routed_cycle_boolean {
    ROUTED_CYCLE_BOOLEAN_NOT_APPLICABLE = 0,
    ROUTED_CYCLE_BOOLEAN_FALSE,
    ROUTED_CYCLE_BOOLEAN_TRUE,
    ROUTED_CYCLE_BOOLEAN_UNKNOWN,
} routed_cycle_boolean_t;

/* Scheduler poll and scheduler-event handling are intentionally distinct. */
typedef enum routed_cycle_phase {
    ROUTED_CYCLE_PHASE_NOT_APPLICABLE = 0,
    ROUTED_CYCLE_PHASE_STARTUP_RX_RETURN,
    ROUTED_CYCLE_PHASE_PRE_POLL_BOUND,
    ROUTED_CYCLE_PHASE_SCHEDULER_POLL,
    ROUTED_CYCLE_PHASE_SCHEDULER_EVENT,
    ROUTED_CYCLE_PHASE_ROUTER_TICK,
    ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT,
    ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH,
    ROUTED_CYCLE_PHASE_LINK_SERVICE,
    ROUTED_CYCLE_PHASE_HEALTHY_YIELD,
    ROUTED_CYCLE_PHASE_FAULT_IDLE,
    ROUTED_CYCLE_PHASE_UNKNOWN,
} routed_cycle_phase_t;

typedef enum routed_cycle_budget_source {
    ROUTED_CYCLE_BUDGET_SOURCE_NOT_APPLICABLE = 0,
    ROUTED_CYCLE_BUDGET_SOURCE_SCHEDULER_RETURN_GAP,
    ROUTED_CYCLE_BUDGET_SOURCE_AMBIGUOUS_HALF_RANGE,
} routed_cycle_budget_source_t;

typedef enum routed_cycle_result {
    ROUTED_CYCLE_RESULT_OK = 0,
    ROUTED_CYCLE_RESULT_EMPTY,
    ROUTED_CYCLE_RESULT_DROPPED,
    ROUTED_CYCLE_RESULT_INVALID,
} routed_cycle_result_t;

typedef struct routed_cycle_timing {
    uint32_t cycle_started_ms;
    uint32_t phase_started_ms;
    uint32_t phase_completed_ms;
    uint32_t phase_elapsed_ms;
    uint32_t scheduler_return_ms;
    uint32_t elapsed_since_scheduler_return_ms;
    uint32_t poll_bound_ms;
    routed_cycle_phase_t phase;
    routed_cycle_phase_t over_budget_phase;
    routed_cycle_budget_source_t over_budget_source;
    routed_cycle_value_state_t scheduler_return_state;
    routed_cycle_value_state_t poll_bound_state;
    routed_cycle_boolean_t over_budget;
} routed_cycle_timing_t;

typedef enum routed_cycle_start_rx_status {
    ROUTED_CYCLE_START_RX_OK = 0,
    ROUTED_CYCLE_START_RX_INVALID,
} routed_cycle_start_rx_status_t;

typedef struct routed_cycle_start_rx_result {
    uint32_t returned_at_ms;
    routed_cycle_start_rx_status_t status;
} routed_cycle_start_rx_result_t;

typedef enum routed_cycle_pre_poll_status {
    ROUTED_CYCLE_PRE_POLL_HEALTHY = 0,
    ROUTED_CYCLE_PRE_POLL_OVER_BUDGET,
    ROUTED_CYCLE_PRE_POLL_UNSEEDED,
    ROUTED_CYCLE_PRE_POLL_AMBIGUOUS_HALF_RANGE,
} routed_cycle_pre_poll_status_t;

typedef enum routed_cycle_scheduler_poll_status {
    ROUTED_CYCLE_SCHEDULER_POLL_OK = 0,
    ROUTED_CYCLE_SCHEDULER_POLL_INVALID,
} routed_cycle_scheduler_poll_status_t;

typedef struct routed_cycle_scheduler_poll_result {
    uint32_t returned_at_ms;
    ble_mesh_sched_event_t event;
    routed_cycle_scheduler_poll_status_t status;
} routed_cycle_scheduler_poll_result_t;

typedef enum routed_cycle_application_request_status {
    ROUTED_CYCLE_APPLICATION_DISABLED = 0,
    ROUTED_CYCLE_APPLICATION_READY,
    ROUTED_CYCLE_APPLICATION_INVALID,
} routed_cycle_application_request_status_t;

typedef struct routed_cycle_application_request {
    uint32_t prepared_at_ms;
    tron_application_data_t data;
    routed_cycle_application_request_status_t status;
} routed_cycle_application_request_t;

typedef enum routed_cycle_yield_status {
    ROUTED_CYCLE_YIELD_OK = 0,
    ROUTED_CYCLE_YIELD_INVALID,
} routed_cycle_yield_status_t;

typedef struct routed_cycle_yield_result {
    uint32_t completed_at_ms;
    routed_cycle_yield_status_t status;
} routed_cycle_yield_result_t;

typedef enum routed_cycle_fault_idle_status {
    ROUTED_CYCLE_FAULT_IDLE_OK = 0,
    ROUTED_CYCLE_FAULT_IDLE_INVALID,
} routed_cycle_fault_idle_status_t;

typedef struct routed_cycle_fault_idle_result {
    uint32_t completed_at_ms;
    routed_cycle_fault_idle_status_t status;
} routed_cycle_fault_idle_result_t;

/* A trace contains exactly one producer-owned payload selected by phase. */
typedef struct routed_cycle_trace {
    routed_cycle_timing_t timing;
    routed_cycle_phase_t phase;
    /* True for every trace emitted at or after the first terminal producer result,
     * including FAULT_IDLE.  Queue retention uses this copied discriminant. */
    routed_cycle_boolean_t fault_latched;
    union {
        struct {
            routed_cycle_pre_poll_status_t status;
        } pre_poll;
        routed_cycle_scheduler_poll_result_t scheduler_poll;
        tavrn_router_phase_trace_t router;
        struct {
            routed_cycle_application_request_t request;
            tavrn_router_phase_trace_t submit;
            tavrn_router_trace_presence_t submit_present;
        } application_submit;
        struct {
            uint32_t requested_slack_ms;
            routed_cycle_yield_result_t result;
        } healthy_yield;
        routed_cycle_fault_idle_result_t fault_idle;
    } detail;
} routed_cycle_trace_t;

typedef enum routed_cycle_run_status {
    ROUTED_CYCLE_RUN_OK = 0,
    ROUTED_CYCLE_RUN_FAULT_IDLE,
    ROUTED_CYCLE_RUN_INVALID,
} routed_cycle_run_status_t;

typedef struct routed_cycle_run_result {
    routed_cycle_run_status_t status;
    uint8_t trace_count;
    routed_cycle_trace_t traces[ROUTED_CYCLE_TRACE_CAPACITY];
} routed_cycle_run_result_t;

/* The state records the post-operation return that starts the next-poll bound
 * and the last phase that consumed that interval. */
typedef struct routed_cycle {
    uint32_t poll_bound_ms;
    uint32_t last_scheduler_return_ms;
    routed_cycle_phase_t last_gap_phase;
    /* Captures the first producer that strictly crossed the active
     * return-to-next-poll bound; it remains stable through fault idle. */
    routed_cycle_phase_t first_over_budget_phase;
    uint8_t scheduler_return_seeded;
    uint8_t fault_latched;
} routed_cycle_t;

typedef routed_cycle_start_rx_result_t (*routed_cycle_start_rx_fn)(void *context);
typedef routed_cycle_scheduler_poll_result_t (*routed_cycle_scheduler_poll_fn)(
    void *context, uint32_t poll_started_at_ms);
typedef tavrn_router_phase_trace_t (*routed_cycle_router_scheduler_event_fn)(
    void *context, const ble_mesh_sched_event_t *event, uint32_t now_ms);
/* An optional application-owned ingress filter runs before router or FULL
 * mentorship handling.  CONSUMED produces an ignored scheduler trace without
 * invoking the router callback; PASSTHROUGH preserves the existing path. */
typedef enum routed_cycle_scheduler_event_filter_status {
    ROUTED_CYCLE_SCHEDULER_EVENT_PASSTHROUGH = 0,
    ROUTED_CYCLE_SCHEDULER_EVENT_CONSUMED,
    ROUTED_CYCLE_SCHEDULER_EVENT_FILTER_INVALID,
} routed_cycle_scheduler_event_filter_status_t;

/* The filter owns its synchronous work interval.  Its completion is the
 * consumed scheduler-event trace completion and the router callback start for
 * a passthrough event. */
typedef struct routed_cycle_scheduler_event_filter_result {
    routed_cycle_scheduler_event_filter_status_t status;
    uint32_t completed_at_ms;
} routed_cycle_scheduler_event_filter_result_t;

typedef routed_cycle_scheduler_event_filter_result_t
    (*routed_cycle_scheduler_event_filter_fn)(
        void *context, const ble_mesh_sched_event_t *event, uint32_t now_ms);
typedef tavrn_router_phase_trace_t (*routed_cycle_router_tick_fn)(
    void *context, uint32_t now_ms);
/* Optional one-shot take owned by the router-tick binding.  A nonzero result
 * directs this cycle to return to scheduler service immediately after a
 * successful, appended router-tick trace.  The binding consumes its request
 * when it returns nonzero; a NULL callback preserves the ordinary cycle. */
typedef uint8_t (*routed_cycle_take_scheduler_return_after_tick_fn)(void *context);
typedef routed_cycle_application_request_t (*routed_cycle_application_prepare_fn)(
    void *context, uint32_t now_ms);
typedef tavrn_router_phase_trace_t (*routed_cycle_router_submit_fn)(
    void *context, const tron_application_data_t *data, uint32_t now_ms);
typedef tavrn_router_phase_trace_t (*routed_cycle_router_dispatch_fn)(
    void *context, uint32_t now_ms);
typedef tavrn_router_phase_trace_t (*routed_cycle_router_link_service_fn)(
    void *context, uint32_t now_ms);
typedef routed_cycle_yield_result_t (*routed_cycle_healthy_yield_fn)(
    void *context, uint32_t requested_slack_ms, uint32_t now_ms);
typedef routed_cycle_fault_idle_result_t (*routed_cycle_fault_idle_fn)(
    void *context, uint32_t now_ms);
typedef uint32_t (*routed_cycle_now_ms_fn)(void *context);
/* This synchronous observer receives the copied result trace.  It has no
 * status return and cannot change the cycle's protocol decision. */
typedef void (*routed_cycle_trace_sink_fn)(
    void *context, const routed_cycle_trace_t *trace);

typedef struct routed_cycle_operations {
    void *context;
    routed_cycle_start_rx_fn start_rx;
    routed_cycle_scheduler_poll_fn scheduler_poll;
    routed_cycle_scheduler_event_filter_fn scheduler_event_filter;
    routed_cycle_router_scheduler_event_fn router_scheduler_event;
    routed_cycle_router_tick_fn router_tick;
    routed_cycle_take_scheduler_return_after_tick_fn
        take_scheduler_return_after_tick;
    routed_cycle_application_prepare_fn application_prepare;
    routed_cycle_router_submit_fn router_submit;
    routed_cycle_router_dispatch_fn router_dispatch;
    routed_cycle_router_link_service_fn router_link_service;
    routed_cycle_healthy_yield_fn healthy_yield;
    routed_cycle_fault_idle_fn fault_idle;
    routed_cycle_now_ms_fn now_ms;
    routed_cycle_trace_sink_fn trace_sink;
    /* Reserve this much of a healthy poll interval for completion of the
     * selected wait primitive.  A zero-initialized caller keeps the existing
     * remaining-slack admission behavior. */
    uint32_t healthy_wait_completion_guard_ms;
} routed_cycle_operations_t;

typedef enum routed_cycle_task_status {
    ROUTED_CYCLE_TASK_OK = 0,
    ROUTED_CYCLE_TASK_FAULT_IDLE,
    ROUTED_CYCLE_TASK_INVALID,
} routed_cycle_task_status_t;

/* Each router callback must return the matching tavrn_router_trace_phase_t;
 * run_once rejects a mismatched discriminant, invalid scheduler event/fault
 * combination, or missing required callback before invoking later operations. */

/* Fixed copied diagnostics retain phase-discriminated producer results.  A
 * full queue drops a new healthy trace (incrementing dropped_count).  A new
 * fault_latched trace evicts the oldest retained healthy trace instead,
 * incrementing evicted_healthy_count and preserving FIFO order among every
 * retained trace; if every retained trace is fault-latched, the new trace is
 * dropped and dropped_fault_count increments.  Queue changes no protocol
 * state.  APIs reject NULL, corrupt ring indices/counts, invalid enum values,
 * and a trace whose phase/timing/union discriminants disagree without mutation. */
typedef struct routed_cycle_diagnostic_queue {
    routed_cycle_trace_t records[ROUTED_CYCLE_DIAGNOSTIC_CAPACITY];
    uint32_t dropped_count;
    uint32_t evicted_healthy_count;
    uint32_t dropped_fault_count;
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} routed_cycle_diagnostic_queue_t;

/* Healthy link-break telemetry is deliberately independent of fault diagnostics:
 * one complete retry-exhausted event is retained by value, with retain-oldest /
 * drop-newest overflow.  It owns no wait, timer, RTOS, or protocol callback. */
typedef enum routed_cycle_retry_log_status {
    ROUTED_CYCLE_RETRY_LOG_OK = 0,
    ROUTED_CYCLE_RETRY_LOG_EMPTY,
    ROUTED_CYCLE_RETRY_LOG_DROPPED,
    ROUTED_CYCLE_RETRY_LOG_INVALID,
} routed_cycle_retry_log_status_t;

typedef struct routed_cycle_retry_log_mailbox {
    tavrn_link_event_t event;
    uint32_t dropped_count;
    uint8_t pending;
} routed_cycle_retry_log_mailbox_t;

typedef struct routed_cycle_retry_log_snapshot {
    uint32_t dropped_count;
    uint8_t pending;
} routed_cycle_retry_log_snapshot_t;

/* FULL-only code supplies all semantically retained GTT entries (including
 * self, hard-expired members, and unexpired departed tombstones) through this
 * common copied representation. */
typedef enum routed_cycle_gtt_freshness {
    ROUTED_CYCLE_GTT_FRESHNESS_NOT_APPLICABLE = 0,
    ROUTED_CYCLE_GTT_FRESHNESS_ACTIVE,
    ROUTED_CYCLE_GTT_FRESHNESS_SOFT_STALE,
    ROUTED_CYCLE_GTT_FRESHNESS_HARD_EXPIRED,
    ROUTED_CYCLE_GTT_FRESHNESS_DEPARTED,
} routed_cycle_gtt_freshness_t;

typedef struct routed_cycle_gtt_snapshot_entry {
    tavrn_adva_t canonical_adva;
    uint32_t last_evidence_ms;
    uint32_t soft_deadline_ms;
    uint32_t hard_deadline_ms;
    uint32_t departed_deadline_ms;
    uint16_t serial;
    uint8_t hop_count;
    routed_cycle_value_state_t serial_state;
    routed_cycle_value_state_t hop_state;
    routed_cycle_gtt_freshness_t freshness;
    routed_cycle_boolean_t departed;
} routed_cycle_gtt_snapshot_entry_t;

typedef struct routed_cycle_gtt_snapshot {
    uint32_t query_at_ms;
    tavrn_adva_t local_canonical_adva;
    uint8_t entry_count;
    /* Every copied member that is not a departed tombstone: self, ACTIVE,
     * SOFT_STALE, and HARD_EXPIRED. */
    uint8_t nondeparted_count;
    routed_cycle_gtt_snapshot_entry_t
        entries[ROUTED_CYCLE_GTT_SNAPSHOT_CAPACITY];
} routed_cycle_gtt_snapshot_t;

/* RREQ lifecycle records originate in generic AODV and remain FIFO/drop-newest
 * at this app-facing telemetry port.  Queue APIs reject invalid stages and
 * inconsistent stage/outcome/timestamp combinations without mutation. */
typedef struct routed_cycle_rreq_queue {
    aodv_rreq_lifecycle_record_t records[ROUTED_CYCLE_RREQ_TELEMETRY_CAPACITY];
    uint32_t dropped_count;
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} routed_cycle_rreq_queue_t;

routed_cycle_result_t routed_cycle_init(routed_cycle_t *cycle,
                                         uint32_t poll_bound_ms);
/* Calls start_rx and seeds the same return-to-next-poll gate used by every
 * ordinary cycle.  It must complete before routed_cycle_run_once(). */
routed_cycle_result_t routed_cycle_seed_startup_rx_return(
    routed_cycle_t *cycle, const routed_cycle_operations_t *operations);
/* Executes only this order: pre-poll gate, scheduler poll, optional scheduler
 * event handling, tick, optional immediate scheduler return, optional submit,
 * one dispatch, link service, then remaining-slack yield or fault idle.  An
 * immediate return is available only through the optional one-shot tick-owner
 * callback after the tick trace is appended and terminal status is rejected.
 * It is the sole ordered executor for a
 * routed_mesh_task ordinary iteration: the binding must call this API once per
 * iteration and must not retain a handwritten poll/tick/submit/dispatch/service
 * loop.  Equality with poll_bound is healthy; an elapsed value of exactly half
 * range is invalid/ambiguous.  Healthy yield requests
 * max((poll_bound_ms - 1) - elapsed_since_scheduler_return_ms, 0).  It admits
 * a healthy yield only when that available amount is greater than
 * healthy_wait_completion_guard_ms, and requests the remaining amount after
 * that guard; otherwise it invokes neither healthy_yield nor emits a
 * HEALTHY_YIELD trace.  Callback
 * completion timestamps must equal their phase start or advance by less than
 * half range; a backward or half-range completion is structural invalid.
 * ROUTED_CYCLE_TIMING_OVERRUN_TERMINAL defaults to 1 for strict reusable
 * tests.  A production target may define it as 0 to retain timing diagnostics
 * without converting an ordinary bound crossing into a terminal fault. */
routed_cycle_run_status_t routed_cycle_run_once(
    routed_cycle_t *cycle, const routed_cycle_operations_t *operations,
    uint32_t poll_started_at_ms, routed_cycle_run_result_t *result_out);
/* Task policy for a structural run failure after startup.  A finite host run
 * remains invalid; the firmware form latches the cycle without changing its
 * already-populated result storage so the following iteration uses fault idle. */
routed_cycle_result_t routed_cycle_task_latch_run_invalid(
    routed_cycle_t *cycle, uint32_t iteration_limit);
/* Task-level binding contract.  It seeds startup RX exactly once, then calls
 * routed_cycle_run_once() exactly once for each iteration using now_ms().
 * result_storage is required caller-owned storage and is reused for every
 * iteration, avoiding a run-result-sized mesh-task stack allocation.  A
 * nonzero iteration_limit returns after that many host-test iterations; zero
 * is the firmware infinite-operation form.  routed_mesh_task prepares its
 * cycle, operations, and result storage outside its body and is a one-call
 * adapter to this API. */
routed_cycle_task_status_t routed_cycle_run_task(
    routed_cycle_t *cycle, const routed_cycle_operations_t *operations,
    uint32_t iteration_limit, routed_cycle_run_result_t *result_storage);

void routed_cycle_trace_reset(routed_cycle_trace_t *trace);
void routed_cycle_diagnostic_queue_init(routed_cycle_diagnostic_queue_t *queue);
routed_cycle_result_t routed_cycle_diagnostic_enqueue(
    routed_cycle_diagnostic_queue_t *queue, const routed_cycle_trace_t *trace);
routed_cycle_result_t routed_cycle_diagnostic_dequeue(
    routed_cycle_diagnostic_queue_t *queue, routed_cycle_trace_t *trace_out);

void routed_cycle_retry_log_init(routed_cycle_retry_log_mailbox_t *mailbox);
routed_cycle_retry_log_status_t routed_cycle_retry_log_offer(
    routed_cycle_retry_log_mailbox_t *mailbox, const tavrn_link_event_t *event);
routed_cycle_retry_log_status_t routed_cycle_retry_log_take(
    routed_cycle_retry_log_mailbox_t *mailbox, tavrn_link_event_t *event_out);
routed_cycle_retry_log_status_t routed_cycle_retry_log_snapshot(
    const routed_cycle_retry_log_mailbox_t *mailbox,
    routed_cycle_retry_log_snapshot_t *snapshot_out);

void routed_cycle_rreq_queue_init(routed_cycle_rreq_queue_t *queue);
routed_cycle_result_t routed_cycle_rreq_enqueue(
    routed_cycle_rreq_queue_t *queue,
    const aodv_rreq_lifecycle_record_t *record);
routed_cycle_result_t routed_cycle_rreq_dequeue(
    routed_cycle_rreq_queue_t *queue, aodv_rreq_lifecycle_record_t *record_out);

#endif /* ROUTED_CYCLE_H */
