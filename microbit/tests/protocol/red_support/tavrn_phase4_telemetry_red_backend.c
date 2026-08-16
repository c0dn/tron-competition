/* RED-only FULL and generic-AODV telemetry port.  It never fabricates a
 * lifecycle callback or copied FULL snapshot. */

#include "routed_full_telemetry.h"

#include <string.h>

routed_full_telemetry_status_t routed_full_telemetry_snapshot_gtt(
    const tavrn_gtt_t *gtt, uint32_t query_at_ms,
    routed_cycle_gtt_snapshot_t *snapshot_out)
{
    (void)gtt;
    (void)query_at_ms;
    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    return ROUTED_FULL_TELEMETRY_OK;
}

aodv_status_t aodv_core_submit_application_scoped_ex(
    aodv_core_t *core, const tron_application_data_t *data,
    uint8_t initial_scope, aodv_rreq_scope_source_t scope_source,
    uint32_t now_ms)
{
    (void)scope_source;
    return aodv_core_submit_application_scoped(core, data, initial_scope, now_ms);
}

aodv_rreq_telemetry_status_t aodv_core_set_rreq_telemetry(
    aodv_core_t *core,
    const aodv_rreq_telemetry_config_t *config_or_null)
{
    (void)core;
    (void)config_or_null;
    return AODV_RREQ_TELEMETRY_OK;
}

aodv_rreq_telemetry_status_t aodv_core_report_rreq_link_enqueue(
    aodv_core_t *core, const aodv_rreq_link_enqueue_t *enqueue)
{
    (void)core;
    (void)enqueue;
    return AODV_RREQ_TELEMETRY_INVALID;
}

/* These RED-only adapters preserve the real router side effect and status
 * while deliberately withholding the new by-value phase details. */
#ifdef TAVRN_PHASE4_RREQ_INTEGRATION
tavrn_router_event_status_t tavrn_router_handle_scheduler_event_ex(
    tavrn_router_t *router, const ble_mesh_sched_event_t *event,
    uint32_t now_ms, tavrn_router_phase_trace_t *trace_out)
{
    if (trace_out != NULL) {
        memset(trace_out, 0, sizeof(*trace_out));
    }
    return tavrn_router_handle_scheduler_event(router, event, now_ms);
}

aodv_status_t tavrn_router_tick_ex(tavrn_router_t *router, uint32_t now_ms,
                                   tavrn_router_phase_trace_t *trace_out)
{
    if (trace_out != NULL) {
        memset(trace_out, 0, sizeof(*trace_out));
    }
    return tavrn_router_tick(router, now_ms);
}

aodv_status_t tavrn_router_submit_application_ex(
    tavrn_router_t *router, const tron_application_data_t *data,
    uint32_t now_ms, tavrn_router_phase_trace_t *trace_out)
{
    if (trace_out != NULL) {
        memset(trace_out, 0, sizeof(*trace_out));
    }
    return tavrn_router_submit_application(router, data, now_ms);
}

tavrn_router_event_status_t tavrn_router_dispatch_trace_ex(
    tavrn_router_t *router, uint32_t now_ms,
    tavrn_router_phase_trace_t *trace_out)
{
    tavrn_router_dispatch_event_t event;

    if (trace_out != NULL) {
        memset(trace_out, 0, sizeof(*trace_out));
    }
    return tavrn_router_dispatch_ex(router, now_ms, &event);
}

tavrn_router_event_status_t tavrn_router_service_link_ex(
    tavrn_router_t *router, uint32_t now_ms,
    tavrn_router_phase_trace_t *trace_out)
{
    if (trace_out != NULL) {
        memset(trace_out, 0, sizeof(*trace_out));
    }
    return tavrn_router_service_link(router, now_ms);
}
#endif
