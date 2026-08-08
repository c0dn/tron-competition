/* RED-only bridge: it deliberately supplies no ESC, snapshot, mentorship,
 * SID8, or JOIN behavior.  Its receive entry calls the real production router
 * so the test cannot pass with an isolated fake FSM. */
#include "tavrn_phase4_esc_mentor_contract.h"

#ifndef TAVRN_ESC_MENTORSHIP_API

#include <string.h>

static tavrn_mentorship_status_t unimplemented(void)
{
    return TAVRN_MENTORSHIP_UNIMPLEMENTED;
}

tavrn_mentorship_status_t tavrn_mentorship_init(
    tavrn_mentorship_t *mentorship, tavrn_router_t *router, tavrn_gtt_t *gtt,
    const tavrn_mentorship_config_t *config, uint32_t now_ms)
{
    (void)now_ms;
    if (mentorship == NULL || router == NULL || gtt == NULL || config == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    memset(mentorship, 0, sizeof(*mentorship));
    mentorship->router = router;
    mentorship->gtt = gtt;
    mentorship->config = *config;
    mentorship->state.state = TAVRN_MENTORSHIP_REJOINING;
    mentorship->state.active_width = TAVRN_IDENTITY_SID16;
    mentorship->state.ordinary_traffic_gated = 1u;
    mentorship->state.full_bootstrap_admission_enabled = 1u;
    return TAVRN_MENTORSHIP_OK;
}

tavrn_mentorship_status_t tavrn_mentorship_freeze_snapshot(
    tavrn_mentorship_t *mentorship, uint16_t snapshot_id, uint32_t now_ms,
    tavrn_mentorship_snapshot_data_t *snapshot_out)
{
    (void)mentorship;
    (void)snapshot_id;
    (void)now_ms;
    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    return unimplemented();
}

tavrn_esc_context_status_t tavrn_mentorship_resolve_sid8(
    const tavrn_mentorship_t *mentorship, uint8_t sid8, uint32_t now_ms,
    tavrn_esc_context_match_t *match_out)
{
    (void)mentorship;
    (void)sid8;
    (void)now_ms;
    if (match_out != NULL) {
        memset(match_out, 0, sizeof(*match_out));
    }
    return TAVRN_ESC_CONTEXT_INVALID;
}

tavrn_mentorship_status_t tavrn_mentorship_offer_delay_ms(
    const tavrn_mentorship_config_t *config, uint8_t rssi_magnitude_db,
    uint32_t jitter_ms, uint32_t *delay_out)
{
    (void)config;
    (void)rssi_magnitude_db;
    (void)jitter_ms;
    if (delay_out != NULL) {
        *delay_out = 0u;
    }
    return unimplemented();
}

tavrn_mentorship_offer_status_t tavrn_mentorship_collect_offer(
    tavrn_mentorship_t *mentorship, const tavrn_mentorship_offer_t *offer,
    uint32_t now_ms)
{
    (void)mentorship;
    (void)offer;
    (void)now_ms;
    return TAVRN_MENTORSHIP_OFFER_INVALID;
}

tavrn_mentorship_offer_status_t tavrn_mentorship_schedule_offer(
    tavrn_mentorship_t *mentorship, const tavrn_mentorship_offer_t *offer,
    uint32_t due_ms)
{
    (void)mentorship;
    (void)offer;
    (void)due_ms;
    return TAVRN_MENTORSHIP_OFFER_INVALID;
}

tavrn_mentorship_offer_status_t tavrn_mentorship_overhear_offer(
    tavrn_mentorship_t *mentorship, const tavrn_mentorship_offer_t *offer,
    uint32_t now_ms)
{
    (void)mentorship;
    (void)offer;
    (void)now_ms;
    return TAVRN_MENTORSHIP_OFFER_INVALID;
}

tavrn_mentorship_status_t tavrn_mentorship_select_offer(
    tavrn_mentorship_t *mentorship, uint32_t now_ms,
    tavrn_mentorship_offer_t *selected_out)
{
    (void)mentorship;
    (void)now_ms;
    if (selected_out != NULL) {
        memset(selected_out, 0, sizeof(*selected_out));
    }
    return unimplemented();
}

tavrn_mentorship_status_t tavrn_mentorship_build_sync_data(
    const tavrn_mentorship_snapshot_data_t *snapshot,
    const tavrn_adva_t *mentor, const tavrn_adva_t *mentee, uint8_t page_index,
    tavrn_validated_control_t *control_out)
{
    (void)snapshot;
    (void)mentor;
    (void)mentee;
    (void)page_index;
    if (control_out != NULL) {
        memset(control_out, 0, sizeof(*control_out));
    }
    return unimplemented();
}

tavrn_mentorship_status_t tavrn_mentorship_build_sync_offer(
    const tavrn_mentorship_snapshot_data_t *snapshot,
    const tavrn_adva_t *mentor, const tavrn_adva_t *mentee,
    uint16_t mentee_boot_nonce, tavrn_validated_control_t *control_out)
{
    (void)snapshot;
    (void)mentor;
    (void)mentee;
    (void)mentee_boot_nonce;
    if (control_out != NULL) {
        memset(control_out, 0, sizeof(*control_out));
    }
    return unimplemented();
}

tavrn_mentorship_status_t tavrn_mentorship_build_sync_pull(
    const tavrn_adva_t *mentee, const tavrn_adva_t *mentor,
    uint16_t snapshot_id, uint8_t page_index,
    tavrn_validated_control_t *control_out)
{
    (void)mentee;
    (void)mentor;
    (void)snapshot_id;
    (void)page_index;
    if (control_out != NULL) {
        memset(control_out, 0, sizeof(*control_out));
    }
    return unimplemented();
}

tavrn_mentorship_status_t tavrn_mentorship_ingest_sync_data(
    tavrn_mentorship_t *mentorship, const tavrn_adva_t *mentor,
    const tavrn_validated_control_t *control, uint32_t now_ms)
{
    (void)mentorship;
    (void)mentor;
    (void)control;
    (void)now_ms;
    return unimplemented();
}

tavrn_mentorship_status_t tavrn_mentorship_handle_scheduler_event(
    tavrn_mentorship_t *mentorship, const ble_mesh_sched_event_t *event,
    uint32_t now_ms, tavrn_mentorship_event_trace_t *trace_out)
{
    tavrn_router_phase_trace_t router_trace;
    tavrn_router_event_status_t router_status;

    if (trace_out != NULL) {
        memset(trace_out, 0, sizeof(*trace_out));
    }
    if (mentorship == NULL || mentorship->router == NULL || event == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    memset(&router_trace, 0, sizeof(router_trace));
    router_status = tavrn_router_handle_scheduler_event_ex(
        mentorship->router, event, now_ms, &router_trace);
    if (trace_out != NULL) {
        trace_out->router_result = router_status;
        trace_out->wire_decode_result =
            router_trace.detail.scheduler_event.wire_decode_result;
        trace_out->wire_type = router_trace.detail.scheduler_event.decoded_frame_type;
        trace_out->link_event_type = router_trace.detail.scheduler_event.link_event_type;
        trace_out->reached_wire_link_router =
            router_trace.detail.scheduler_event.wire_decode_present !=
                    TAVRN_ROUTER_TRACE_NOT_PRESENT &&
                router_trace.detail.scheduler_event.link_step_present !=
                    TAVRN_ROUTER_TRACE_NOT_PRESENT &&
                router_trace.detail.scheduler_event.link_event_present !=
                    TAVRN_ROUTER_TRACE_NOT_PRESENT;
    }
    return router_status == TAVRN_ROUTER_EVENT_INVALID ? TAVRN_MENTORSHIP_INVALID :
                                                        unimplemented();
}

tavrn_mentorship_status_t tavrn_mentorship_tick(
    tavrn_mentorship_t *mentorship, uint32_t now_ms)
{
    (void)mentorship;
    (void)now_ms;
    return unimplemented();
}

tavrn_mentorship_status_t tavrn_mentorship_activate_sid8(
    tavrn_mentorship_t *mentorship, uint32_t now_ms)
{
    (void)mentorship;
    (void)now_ms;
    return unimplemented();
}

tavrn_mentorship_status_t tavrn_mentorship_recover_sid16(
    tavrn_mentorship_t *mentorship, uint32_t now_ms)
{
    (void)mentorship;
    (void)now_ms;
    return unimplemented();
}

aodv_status_t tavrn_mentorship_submit_application(
    tavrn_mentorship_t *mentorship, const tron_application_data_t *data,
    uint32_t now_ms)
{
    if (mentorship == NULL || mentorship->router == NULL) {
        return AODV_STATUS_INVALID;
    }
    return tavrn_router_submit_application(mentorship->router, data, now_ms);
}

tavrn_mentorship_status_t tavrn_mentorship_dispatch(
    tavrn_mentorship_t *mentorship, uint32_t now_ms,
    tavrn_router_phase_trace_t *trace_out)
{
    if (mentorship == NULL || mentorship->router == NULL || trace_out == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    (void)tavrn_router_dispatch_trace_ex(mentorship->router, now_ms, trace_out);
    return unimplemented();
}

tavrn_mentorship_status_t tavrn_mentorship_build_join(
    tavrn_mentorship_t *mentorship, uint16_t tc_sequence,
    tavrn_validated_control_t *control_out)
{
    (void)mentorship;
    (void)tc_sequence;
    if (control_out != NULL) {
        memset(control_out, 0, sizeof(*control_out));
    }
    return unimplemented();
}

tavrn_mentorship_status_t tavrn_mentorship_state_snapshot(
    const tavrn_mentorship_t *mentorship,
    tavrn_mentorship_state_snapshot_t *snapshot_out)
{
    if (mentorship == NULL || snapshot_out == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    *snapshot_out = mentorship->state;
    return TAVRN_MENTORSHIP_OK;
}

const tavrn_mentorship_counters_t *tavrn_mentorship_counters(
    const tavrn_mentorship_t *mentorship)
{
    return mentorship == NULL ? NULL : &mentorship->counters;
}

#endif /* TAVRN_ESC_MENTORSHIP_API */
