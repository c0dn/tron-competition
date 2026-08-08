#include "tavrn_full_maintenance_binding.h"

#include <string.h>

static void invalid_result(tavrn_full_maintenance_binding_result_t *result)
{
    memset(result, 0, sizeof(*result));
    result->status = TAVRN_FULL_MAINTENANCE_BINDING_INVALID;
    result->router_status = AODV_STATUS_INVALID;
    result->mentorship_status = TAVRN_MENTORSHIP_INVALID;
    result->broadcast_snapshot_status = TAVRN_ROUTER_LOCAL_BROADCAST_INVALID;
    result->broadcast_observation_status = TAVRN_MAINTENANCE_INVALID;
    result->activation_status = TAVRN_MAINTENANCE_INVALID;
    result->post_tick.sweep_status = TAVRN_MAINTENANCE_EXPIRY_SWEEP_INVALID;
    result->post_tick.maintenance_status = TAVRN_MAINTENANCE_INVALID;
}

tavrn_full_maintenance_binding_status_t tavrn_full_maintenance_binding_tick(
    tavrn_full_maintenance_binding_input_t input, uint32_t now_ms,
    tavrn_full_maintenance_binding_result_t *result_out)
{
    tavrn_maintenance_owner_pre_tick_input_t pre_input;

    if (result_out == NULL) {
        return TAVRN_FULL_MAINTENANCE_BINDING_INVALID;
    }
    if (input.router == NULL || input.mentorship == NULL || input.maintenance == NULL ||
        input.application_present > 1u || input.maintenance->router != input.router) {
        invalid_result(result_out);
        return result_out->status;
    }
    memset(result_out, 0, sizeof(*result_out));
    memset(&pre_input, 0, sizeof(pre_input));
    pre_input.application_present = input.application_present;
    pre_input.application_command = input.application_command;
    result_out->pre_tick = tavrn_maintenance_owner_pre_tick(input.maintenance, pre_input,
                                                             now_ms);
    result_out->application_present = input.application_present;
    result_out->application_result = result_out->pre_tick.application_result;
    result_out->router_status = tavrn_router_tick_ex(input.router, now_ms,
                                                      &result_out->router_trace);
    result_out->mentorship_status = tavrn_mentorship_tick(input.mentorship, now_ms);
    result_out->broadcast_snapshot_status = tavrn_router_local_broadcast_snapshot(
        input.router, &result_out->broadcast);
    if (result_out->broadcast_snapshot_status == TAVRN_ROUTER_LOCAL_BROADCAST_OK &&
        result_out->broadcast.generation != 0u &&
        result_out->broadcast.generation != input.maintenance->local_broadcast_generation_seen) {
        result_out->broadcast_observation_status = tavrn_maintenance_observe_local_broadcast(
            input.maintenance, result_out->broadcast.accepted_at_ms);
        if (result_out->broadcast_observation_status == TAVRN_MAINTENANCE_GATED) {
            result_out->broadcast_observation_status = TAVRN_MAINTENANCE_OK;
        }
        if (result_out->broadcast_observation_status == TAVRN_MAINTENANCE_OK) {
            input.maintenance->local_broadcast_generation_seen = result_out->broadcast.generation;
        }
    } else if (result_out->broadcast_snapshot_status == TAVRN_ROUTER_LOCAL_BROADCAST_OK) {
        result_out->broadcast_observation_status = TAVRN_MAINTENANCE_OK;
    } else {
        result_out->broadcast_observation_status = TAVRN_MAINTENANCE_INVALID;
    }
    result_out->activation_status = tavrn_maintenance_activate(input.maintenance, now_ms);
    result_out->post_tick = tavrn_maintenance_owner_post_tick(input.maintenance, now_ms);
    result_out->status = (result_out->router_status == AODV_STATUS_INVALID ||
                      result_out->mentorship_status == TAVRN_MENTORSHIP_INVALID ||
                      result_out->broadcast_snapshot_status !=
                          TAVRN_ROUTER_LOCAL_BROADCAST_OK ||
                      result_out->broadcast_observation_status == TAVRN_MAINTENANCE_INVALID ||
                      result_out->activation_status == TAVRN_MAINTENANCE_INVALID ||
                      result_out->post_tick.sweep_status !=
                          TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK ||
                      result_out->post_tick.maintenance_status == TAVRN_MAINTENANCE_INVALID) ?
        TAVRN_FULL_MAINTENANCE_BINDING_INVALID : TAVRN_FULL_MAINTENANCE_BINDING_OK;
    return result_out->status;
}
