/* RED-only common cycle port.  It deliberately supplies no behavior. */

#include "routed_cycle.h"

#include <string.h>

/* Keep the RED backend coupled to the observation-only callback declaration;
 * its deliberately failing behavior remains unchanged. */
typedef routed_cycle_trace_sink_fn routed_cycle_trace_sink_red_stub_t;

routed_cycle_result_t routed_cycle_init(routed_cycle_t *cycle,
                                        uint32_t poll_bound_ms)
{
    (void)cycle;
    (void)poll_bound_ms;
    return ROUTED_CYCLE_RESULT_OK;
}

routed_cycle_result_t routed_cycle_seed_startup_rx_return(
    routed_cycle_t *cycle, const routed_cycle_operations_t *operations)
{
    (void)cycle;
    (void)operations;
    return ROUTED_CYCLE_RESULT_OK;
}

routed_cycle_run_status_t routed_cycle_run_once(
    routed_cycle_t *cycle, const routed_cycle_operations_t *operations,
    uint32_t poll_started_at_ms, routed_cycle_run_result_t *result_out)
{
    (void)cycle;
    (void)operations;
    (void)poll_started_at_ms;
    if (result_out != NULL) {
        memset(result_out, 0, sizeof(*result_out));
    }
    return ROUTED_CYCLE_RUN_OK;
}

routed_cycle_result_t routed_cycle_task_latch_run_invalid(
    routed_cycle_t *cycle, uint32_t iteration_limit)
{
    (void)cycle;
    (void)iteration_limit;
    return ROUTED_CYCLE_RESULT_OK;
}

routed_cycle_task_status_t routed_cycle_run_task(
    routed_cycle_t *cycle, const routed_cycle_operations_t *operations,
    uint32_t iteration_limit, routed_cycle_run_result_t *result_storage)
{
    (void)cycle;
    (void)operations;
    (void)iteration_limit;
    (void)result_storage;
    return ROUTED_CYCLE_TASK_OK;
}

void routed_cycle_trace_reset(routed_cycle_trace_t *trace)
{
    (void)trace;
}

void routed_cycle_diagnostic_queue_init(routed_cycle_diagnostic_queue_t *queue)
{
    (void)queue;
}

routed_cycle_result_t routed_cycle_diagnostic_enqueue(
    routed_cycle_diagnostic_queue_t *queue, const routed_cycle_trace_t *trace)
{
    (void)queue;
    (void)trace;
    return ROUTED_CYCLE_RESULT_DROPPED;
}

routed_cycle_result_t routed_cycle_diagnostic_dequeue(
    routed_cycle_diagnostic_queue_t *queue, routed_cycle_trace_t *trace_out)
{
    (void)queue;
    (void)trace_out;
    return ROUTED_CYCLE_RESULT_EMPTY;
}

void routed_cycle_rreq_queue_init(routed_cycle_rreq_queue_t *queue)
{
    (void)queue;
}

routed_cycle_result_t routed_cycle_rreq_enqueue(
    routed_cycle_rreq_queue_t *queue,
    const aodv_rreq_lifecycle_record_t *record)
{
    (void)queue;
    (void)record;
    return ROUTED_CYCLE_RESULT_DROPPED;
}

routed_cycle_result_t routed_cycle_rreq_dequeue(
    routed_cycle_rreq_queue_t *queue, aodv_rreq_lifecycle_record_t *record_out)
{
    (void)queue;
    (void)record_out;
    return ROUTED_CYCLE_RESULT_EMPTY;
}
