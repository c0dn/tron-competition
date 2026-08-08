/* RED fallback for future production symbols only.  It deliberately does not
 * model GTT, AODV, link, router, FULL, or maintenance behavior. */
#include "tavrn_phase5_expiry_demand_contract.h"

#include <string.h>

#if defined(TAVRN_PHASE5_EXPIRY_RED_MODE)
#define tavrn_gtt_observe_with_provenance phase5_red_gtt_observe
#define tavrn_gtt_expiry_snapshot phase5_red_gtt_snapshot
#define tavrn_gtt_enumerate_known phase5_red_gtt_enumerate_known
#define tavrn_gtt_application_request phase5_red_gtt_request
#define tavrn_gtt_application_cancel phase5_red_gtt_cancel
#define tavrn_gtt_apply_application_command phase5_red_gtt_apply_application_command
#define tavrn_gtt_sync_merge phase5_red_gtt_sync_merge
#define aodv_core_subject_demand_snapshot phase5_red_aodv_demand
#define tavrn_link_v2_subject_demand_snapshot phase5_red_link_demand
#define tavrn_router_subject_demand_snapshot phase5_red_router_demand
#define tavrn_full_resolve_unique_sid8 phase5_red_full_resolve_sid8
#define tavrn_maintenance_demand_snapshot phase5_red_maintenance_demand
#define tavrn_maintenance_checked_local_departure phase5_red_maintenance_checked_departure
#define tavrn_maintenance_sweep_expiry phase5_red_maintenance_sweep
#define tavrn_maintenance_owner_pre_tick phase5_red_maintenance_owner_pre_tick
#define tavrn_maintenance_owner_post_tick phase5_red_maintenance_owner_post_tick
#define tavrn_full_application_mailbox_init phase5_red_full_application_mailbox_init
#define tavrn_full_application_mailbox_submit phase5_red_full_application_mailbox_submit
#define tavrn_full_application_mailbox_owner_take phase5_red_full_application_mailbox_owner_take
#define tavrn_full_application_mailbox_owner_publish phase5_red_full_application_mailbox_owner_publish
#define tavrn_full_application_mailbox_consumer_take phase5_red_full_application_mailbox_consumer_take
#define tavrn_full_maintenance_binding_tick phase5_red_full_maintenance_binding_tick
#endif

#if defined(TAVRN_PHASE5_EXPIRY_RED_MODE) || !defined(TAVRN_GTT_EXPIRY_API)

tavrn_gtt_expiry_observe_status_t tavrn_gtt_observe_with_provenance(
    tavrn_gtt_t *gtt, const tavrn_gtt_evidence_t *evidence,
    tavrn_gtt_provenance_t provenance, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out)
{ (void)gtt; (void)evidence; (void)provenance; (void)now_ms; (void)snapshot_out;
  return TAVRN_GTT_EXPIRY_OBSERVE_UNAVAILABLE; }

tavrn_gtt_expiry_query_status_t tavrn_gtt_expiry_snapshot(
    const tavrn_gtt_t *gtt, const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out)
{ (void)gtt; (void)identity; (void)now_ms; (void)snapshot_out;
  return TAVRN_GTT_EXPIRY_QUERY_UNAVAILABLE; }

tavrn_gtt_expiry_query_status_t tavrn_gtt_enumerate_known(
    const tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshots_out, uint8_t capacity, uint8_t *count_out)
{ (void)gtt; (void)lane; (void)now_ms; (void)snapshots_out; (void)capacity; (void)count_out;
  return TAVRN_GTT_EXPIRY_QUERY_UNAVAILABLE; }

#define APPLICATION_STUB(name) \
    tavrn_gtt_application_request_status_t name( \
        tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane, \
        const tavrn_adva_t *identity, uint32_t now_ms, \
        tavrn_gtt_expiry_snapshot_t *snapshot_out) \
    { (void)gtt; (void)lane; (void)identity; (void)now_ms; (void)snapshot_out; \
      return TAVRN_GTT_APPLICATION_UNAVAILABLE; }
APPLICATION_STUB(tavrn_gtt_application_request)
APPLICATION_STUB(tavrn_gtt_application_cancel)
#undef APPLICATION_STUB

tavrn_gtt_application_command_result_t tavrn_gtt_apply_application_command(
    tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane,
    tavrn_gtt_application_command_t command)
{
    tavrn_gtt_application_command_result_t result = {
        .status = TAVRN_GTT_APPLICATION_UNAVAILABLE,
        .query_status = TAVRN_GTT_EXPIRY_QUERY_UNAVAILABLE,
    };

    (void)gtt;
    (void)lane;
    (void)command;
    return result;
}

tavrn_gtt_sync_merge_status_t tavrn_gtt_sync_merge(
    tavrn_gtt_t *gtt, const tavrn_gtt_sync_record_t *records, uint8_t record_count,
    uint32_t now_ms)
{ (void)gtt; (void)records; (void)record_count; (void)now_ms;
  return TAVRN_GTT_SYNC_MERGE_UNAVAILABLE; }

#endif /* RED mode or no GTT expiry API */

#if defined(TAVRN_PHASE5_EXPIRY_RED_MODE) || !defined(TAVRN_SUBJECT_DEMAND_API)

tavrn_aodv_subject_demand_snapshot_t aodv_core_subject_demand_snapshot(
    const aodv_core_t *core, const tavrn_logical_id_t *subject,
    const tavrn_adva_t *canonical_subject, uint32_t now_ms)
{ tavrn_aodv_subject_demand_snapshot_t result; (void)core; (void)subject;
  (void)canonical_subject; (void)now_ms;
  result.reason_mask = 0u; result.valid_route_to_subject = 0u; result.snapshot_available = 0u;
  return result; }

tavrn_link_subject_demand_snapshot_t tavrn_link_v2_subject_demand_snapshot(
    const tavrn_link_v2_t *link, const tavrn_logical_id_t *subject,
    const tavrn_adva_t *canonical_subject, uint32_t now_ms)
{ tavrn_link_subject_demand_snapshot_t result; (void)link; (void)subject;
  (void)canonical_subject; (void)now_ms; result.reason_mask = 0u; result.snapshot_available = 0u;
  return result; }

tavrn_router_subject_demand_snapshot_t tavrn_router_subject_demand_snapshot(
    const tavrn_router_t *router, const tavrn_logical_id_t *subject,
    const tavrn_adva_t *canonical_subject, uint32_t now_ms)
{ tavrn_router_subject_demand_snapshot_t result; (void)router; (void)subject;
  (void)canonical_subject; (void)now_ms; result.reason_mask = 0u; result.snapshot_available = 0u;
  return result; }

#endif /* RED mode or no subject-demand API */

#if defined(TAVRN_PHASE5_EXPIRY_RED_MODE) || !defined(TAVRN_FULL_EXPIRY_API)

tavrn_esc_context_status_t tavrn_full_resolve_unique_sid8(
    const tavrn_full_t *full, const tavrn_adva_t *identity, tavrn_logical_id_t *sid8_out)
{ (void)full; (void)identity; (void)sid8_out; return TAVRN_ESC_CONTEXT_UNKNOWN; }

#endif /* RED mode or no FULL expiry API */

#if defined(TAVRN_PHASE5_EXPIRY_RED_MODE) || !defined(TAVRN_MAINTENANCE_EXPIRY_DEMAND_API)

tavrn_maintenance_demand_status_t tavrn_maintenance_demand_snapshot(
    const tavrn_maintenance_t *maintenance, const tavrn_adva_t *identity,
    uint32_t now_ms, tavrn_maintenance_demand_snapshot_t *snapshot_out)
{ (void)maintenance; (void)identity; (void)now_ms; (void)snapshot_out;
  return TAVRN_MAINTENANCE_DEMAND_UNAVAILABLE; }

tavrn_maintenance_checked_departure_status_t tavrn_maintenance_checked_local_departure(
    tavrn_maintenance_t *maintenance, const tavrn_gtt_departure_candidate_t *candidate,
    uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out)
{ (void)maintenance; (void)candidate; (void)now_ms; (void)snapshot_out;
  return TAVRN_GTT_CHECKED_UNAVAILABLE; }

tavrn_maintenance_expiry_sweep_status_t tavrn_maintenance_sweep_expiry(
    tavrn_maintenance_t *maintenance, uint32_t now_ms,
    tavrn_maintenance_expiry_sweep_snapshot_t *snapshot_out)
{ (void)maintenance; (void)now_ms; (void)snapshot_out;
  return TAVRN_MAINTENANCE_EXPIRY_SWEEP_UNAVAILABLE; }

tavrn_maintenance_owner_pre_tick_result_t tavrn_maintenance_owner_pre_tick(
    tavrn_maintenance_t *maintenance, tavrn_maintenance_owner_pre_tick_input_t input,
    uint32_t now_ms)
{
    tavrn_maintenance_owner_pre_tick_result_t result = {
        .observe_status = TAVRN_GTT_EXPIRY_OBSERVE_UNAVAILABLE,
    };

    result.application_result.status = TAVRN_GTT_APPLICATION_UNAVAILABLE;
    result.application_result.query_status = TAVRN_GTT_EXPIRY_QUERY_UNAVAILABLE;
    result.observe_present = 0u;
    result.application_present = 0u;
    (void)maintenance;
    (void)input;
    (void)now_ms;
    return result;
}

tavrn_maintenance_owner_post_tick_result_t tavrn_maintenance_owner_post_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    tavrn_maintenance_owner_post_tick_result_t result = {
        .sweep_status = TAVRN_MAINTENANCE_EXPIRY_SWEEP_UNAVAILABLE,
        .maintenance_status = TAVRN_MAINTENANCE_INVALID,
    };

    (void)maintenance;
    (void)now_ms;
    return result;
}

tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_init(
    tavrn_full_application_mailbox_t *mailbox)
{ (void)mailbox; return TAVRN_FULL_APPLICATION_MAILBOX_UNAVAILABLE; }
tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_submit(
    tavrn_full_application_mailbox_t *mailbox, tavrn_gtt_application_command_t command)
{ (void)mailbox; (void)command; return TAVRN_FULL_APPLICATION_MAILBOX_UNAVAILABLE; }
tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_owner_take(
    tavrn_full_application_mailbox_t *mailbox, tavrn_gtt_application_command_t *command_out)
{ (void)mailbox; (void)command_out; return TAVRN_FULL_APPLICATION_MAILBOX_UNAVAILABLE; }
tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_owner_publish(
    tavrn_full_application_mailbox_t *mailbox, tavrn_gtt_application_command_result_t result)
{ (void)mailbox; (void)result; return TAVRN_FULL_APPLICATION_MAILBOX_UNAVAILABLE; }
tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_consumer_take(
    tavrn_full_application_mailbox_t *mailbox,
    tavrn_gtt_application_command_result_t *result_out)
{ (void)mailbox; (void)result_out; return TAVRN_FULL_APPLICATION_MAILBOX_UNAVAILABLE; }

tavrn_full_maintenance_binding_status_t tavrn_full_maintenance_binding_tick(
    tavrn_full_maintenance_binding_input_t input, uint32_t now_ms,
    tavrn_full_maintenance_binding_result_t *result_out)
{
    (void)input;
    (void)now_ms;
    if (result_out == NULL) {
        return TAVRN_FULL_MAINTENANCE_BINDING_INVALID;
    }
    memset(result_out, 0, sizeof(*result_out));
    result_out->status = TAVRN_FULL_MAINTENANCE_BINDING_UNAVAILABLE;
    result_out->application_result.status = TAVRN_GTT_APPLICATION_UNAVAILABLE;
    result_out->application_result.query_status = TAVRN_GTT_EXPIRY_QUERY_UNAVAILABLE;
    result_out->router_status = AODV_STATUS_INVALID;
    result_out->mentorship_status = TAVRN_MENTORSHIP_INVALID;
    result_out->broadcast_snapshot_status = TAVRN_ROUTER_LOCAL_BROADCAST_INVALID;
    result_out->broadcast_observation_status = TAVRN_MAINTENANCE_INVALID;
    result_out->activation_status = TAVRN_MAINTENANCE_INVALID;
    result_out->post_tick.sweep_status = TAVRN_MAINTENANCE_EXPIRY_SWEEP_UNAVAILABLE;
    result_out->post_tick.maintenance_status = TAVRN_MAINTENANCE_INVALID;
    return result_out->status;
}

#endif /* RED mode or no maintenance-expiry-demand API */
