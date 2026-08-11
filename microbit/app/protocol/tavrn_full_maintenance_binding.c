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
    result->targeted_owner_status = TAVRN_TARGETED_FRESHNESS_INVALID;
    result->verification_owner_status = TAVRN_RREQ_VERIFICATION_INVALID;
    result->metadata_owner_status = TAVRN_TC_METADATA_INVALID;
    result->tc_owner_status = TAVRN_TC_METADATA_INVALID;
    result->post_tick.sweep_status = TAVRN_MAINTENANCE_EXPIRY_SWEEP_INVALID;
    result->post_tick.maintenance_status = TAVRN_MAINTENANCE_INVALID;
}

static int metadata_frame_for_control(const tavrn_validated_control_t *control,
                                      tavrn_metadata_frame_kind_t *kind_out,
                                      uint8_t *unreachable_count_out)
{
    if (control == NULL || kind_out == NULL || unreachable_count_out == NULL ||
        (control->pdu[5] & 0x80u) == 0u) {
        return 0;
    }
    *unreachable_count_out = 0u;
    if (control->type == TAVRN_WIRE_E_RREQ) {
        if ((control->pdu[5] & 0x08u) != 0u) return 0;
        *kind_out = TAVRN_METADATA_FRAME_RREQ8;
        return 1;
    }
    if (control->type == TAVRN_WIRE_E_RREP) {
        if ((control->pdu[5] & 0x08u) != 0u) return 0;
        *kind_out = TAVRN_METADATA_FRAME_RREP8;
        return 1;
    }
    if (control->type == TAVRN_WIRE_E_RERR) {
        if ((control->pdu[5] & 0x10u) != 0u) return 0;
        *kind_out = TAVRN_METADATA_FRAME_RERR8;
        *unreachable_count_out = control->pdu[10];
        return 1;
    }
    return 0;
}

static int controls_equal(const tavrn_validated_control_t *left,
                          const tavrn_validated_control_t *right)
{
    return left != NULL && right != NULL && left->type == right->type &&
        left->pdu_len == right->pdu_len &&
        memcmp(left->pdu, right->pdu, left->pdu_len) == 0;
}

static tavrn_router_control_augmentation_status_t full_metadata_prepare(
    void *context, const tavrn_validated_control_t *base,
    tavrn_validated_control_t *augmented_out, uint32_t now_ms)
{
    tavrn_maintenance_t *maintenance = context;
    tavrn_metadata_frame_kind_t kind;
    tavrn_metadata_selection_t selection;
    tavrn_metadata_attached_control_t attached;
    uint8_t unreachable_count;
    tavrn_tc_metadata_status_t status;

    if (maintenance == NULL || base == NULL || augmented_out == NULL) {
        return TAVRN_ROUTER_CONTROL_AUGMENTATION_INVALID;
    }
    if (maintenance->metadata_pending.valid != 0u) {
        if (!controls_equal(base, &maintenance->metadata_pending.base)) {
            /* A retained metadata transaction owns only its exact base.  Other
             * AODV controls remain admissible without metadata. */
            *augmented_out = *base;
            return TAVRN_ROUTER_CONTROL_AUGMENTATION_OK;
        }
        if (maintenance->metadata_pending.admission_started != 0u) {
            return TAVRN_ROUTER_CONTROL_AUGMENTATION_BUSY;
        }
        *augmented_out = maintenance->metadata_pending.attached.control;
        return TAVRN_ROUTER_CONTROL_AUGMENTATION_OK;
    }
    *augmented_out = *base;
    if (!metadata_frame_for_control(base, &kind, &unreachable_count)) {
        return TAVRN_ROUTER_CONTROL_AUGMENTATION_OK;
    }
    memset(&selection, 0, sizeof(selection));
    status = tavrn_maintenance_metadata_select(&maintenance->tc_metadata, kind,
                                               unreachable_count, now_ms, &selection);
    if (status != TAVRN_TC_METADATA_OK) {
        return status == TAVRN_TC_METADATA_BUSY ?
            TAVRN_ROUTER_CONTROL_AUGMENTATION_BUSY :
            TAVRN_ROUTER_CONTROL_AUGMENTATION_INVALID;
    }
    if (selection.count == 0u) {
        return TAVRN_ROUTER_CONTROL_AUGMENTATION_OK;
    }
    memset(&attached, 0, sizeof(attached));
    status = tavrn_maintenance_metadata_attach_control(base, &selection, &attached);
    if (status != TAVRN_TC_METADATA_OK) {
        return TAVRN_ROUTER_CONTROL_AUGMENTATION_INVALID;
    }
    maintenance->metadata_pending.base = *base;
    maintenance->metadata_pending.attached = attached;
    maintenance->metadata_pending.valid = 1u;
    *augmented_out = maintenance->metadata_pending.attached.control;
    return TAVRN_ROUTER_CONTROL_AUGMENTATION_OK;
}

static tavrn_router_control_augmentation_status_t full_metadata_admitted(
    void *context, const tavrn_validated_control_t *base,
    const tavrn_validated_control_t *sent,
    const tavrn_direct_peer_t *next_hop_or_null, uint8_t controlled_flood,
    tavrn_link_send_status_t link_status, ble_mesh_tx_token_t scheduler_token,
    uint32_t now_ms)
{
    tavrn_maintenance_t *maintenance = context;

    (void)now_ms;
    if (maintenance == NULL || base == NULL || sent == NULL ||
        maintenance->metadata_pending.valid == 0u ||
        !controls_equal(base, &maintenance->metadata_pending.base) ||
        !controls_equal(sent, &maintenance->metadata_pending.attached.control)) {
        return TAVRN_ROUTER_CONTROL_AUGMENTATION_OK;
    }
    if (controlled_flood > 1u ||
        (controlled_flood == 0u && next_hop_or_null == NULL)) {
        return TAVRN_ROUTER_CONTROL_AUGMENTATION_INVALID;
    }
    maintenance->metadata_pending.admission_started = 1u;
    maintenance->metadata_pending.controlled_flood = controlled_flood;
    if (next_hop_or_null != NULL) {
        maintenance->metadata_pending.next_hop = *next_hop_or_null;
    } else {
        memset(&maintenance->metadata_pending.next_hop, 0,
               sizeof(maintenance->metadata_pending.next_hop));
    }
    maintenance->metadata_last_base_type = sent->type;
    maintenance->metadata_last_emitted_count =
        maintenance->metadata_pending.attached.emitted.count;
    maintenance->metadata_last_pdu_len = sent->pdu_len;
    if (link_status == TAVRN_LINK_SEND_OK) {
        if (scheduler_token == BLE_MESH_TX_TOKEN_NONE) {
            maintenance->metadata_completion_failure_count++;
            maintenance->metadata_pending.terminal_failure = 1u;
            return TAVRN_ROUTER_CONTROL_AUGMENTATION_INVALID;
        }
        maintenance->metadata_pending.scheduler_token = scheduler_token;
        maintenance->metadata_pending.retry_pending = 0u;
        return TAVRN_ROUTER_CONTROL_AUGMENTATION_OK;
    }
    if (link_status == TAVRN_LINK_SEND_BUSY || link_status == TAVRN_LINK_SEND_NO_SLOT ||
        link_status == TAVRN_LINK_SEND_LOCAL_NOT_ATTEMPTED) {
        maintenance->metadata_pending.scheduler_token = BLE_MESH_TX_TOKEN_NONE;
        maintenance->metadata_pending.retry_pending = 1u;
        return TAVRN_ROUTER_CONTROL_AUGMENTATION_OK;
    }
    maintenance->metadata_completion_failure_count++;
    maintenance->metadata_pending.terminal_failure = 1u;
    return TAVRN_ROUTER_CONTROL_AUGMENTATION_INVALID;
}

static tavrn_router_control_augmentation_status_t full_metadata_completed(
    void *context, const ble_mesh_sched_event_t *event, uint32_t now_ms)
{
    tavrn_maintenance_t *maintenance = context;
    tavrn_metadata_pending_transaction_t *pending;
    tavrn_tc_metadata_status_t status;

    if (maintenance == NULL || event == NULL ||
        (event->type != BLE_MESH_SCHED_EVENT_TX_DONE &&
         event->type != BLE_MESH_SCHED_EVENT_TX_FAILED)) {
        return TAVRN_ROUTER_CONTROL_AUGMENTATION_INVALID;
    }
    pending = &maintenance->metadata_pending;
    if (pending->valid == 0u || pending->scheduler_token != event->tx_token) {
        return TAVRN_ROUTER_CONTROL_AUGMENTATION_OK;
    }
    if (event->type == BLE_MESH_SCHED_EVENT_TX_DONE &&
        event->tx_completed_channel_mask != 0u) {
        status = tavrn_maintenance_metadata_commit(
            &maintenance->tc_metadata, &pending->attached.emitted,
            TAVRN_TC_ADMISSION_ADMITTED, now_ms);
        if (status != TAVRN_TC_METADATA_OK) {
            maintenance->metadata_completion_failure_count++;
            pending->terminal_failure = 1u;
            return TAVRN_ROUTER_CONTROL_AUGMENTATION_INVALID;
        }
        maintenance->metadata_completion_commit_count++;
        memset(pending, 0, sizeof(*pending));
        return TAVRN_ROUTER_CONTROL_AUGMENTATION_OK;
    }
    /* Zero-mask TX_DONE and every TX_FAILED have not proved a physical
     * emission.  Preserve the exact PDU, selected entries and route context. */
    pending->scheduler_token = BLE_MESH_TX_TOKEN_NONE;
    pending->retry_pending = 1u;
    maintenance->metadata_completion_retry_count++;
    return TAVRN_ROUTER_CONTROL_AUGMENTATION_OK;
}

static void full_control_received(void *context,
                                  const tavrn_rx_control_event_t *control_event,
                                  uint32_t now_ms)
{
    tavrn_maintenance_t *maintenance = context;
    tavrn_tc_metadata_action_t relay;
    const tavrn_validated_control_t *control;

    if (maintenance == NULL || control_event == NULL) return;
    control = &control_event->control;
    if (control->type == TAVRN_WIRE_TC_UPDATE) {
        memset(&relay, 0, sizeof(relay));
        (void)tavrn_maintenance_tc_receive(&maintenance->tc_metadata, control->pdu,
                                           control->pdu_len,
                                           &control_event->transmitter.adva, now_ms,
                                           &relay);
        return;
    }
    if ((control->type == TAVRN_WIRE_E_RREQ || control->type == TAVRN_WIRE_E_RREP) &&
        (control->pdu[5] & 0x08u) != 0u) {
        if (maintenance->metadata_pending.valid != 0u) {
            maintenance->metadata_frozen_rx_count++;
            return;
        }
        (void)tavrn_maintenance_metadata_receive_control(
            &maintenance->tc_metadata, control_event, now_ms);
    } else if (control->type == TAVRN_WIRE_E_RERR &&
               (control->pdu[5] & 0x10u) != 0u) {
        if (maintenance->metadata_pending.valid != 0u) {
            maintenance->metadata_frozen_rx_count++;
            return;
        }
        (void)tavrn_maintenance_metadata_receive_control(
            &maintenance->tc_metadata, control_event, now_ms);
    }
}

static tavrn_router_data_terminal_disposition_t
tavrn_full_maintenance_tc_on_retry_exhausted(
    void *context, const tavrn_link_event_t *event, uint32_t now_ms)
{
    tavrn_maintenance_t *maintenance = context;

    if (maintenance == NULL || event == NULL ||
        event->type != TAVRN_LINK_EVENT_RETRY_EXHAUSTED ||
        event->detail.owned_data.data.ownership != TAVRN_DATA_TRANSIT) {
        return TAVRN_ROUTER_DATA_TERMINAL_DECLINED;
    }
    (void)tavrn_maintenance_tc_on_retry_exhausted(
        maintenance, &event->detail.owned_data.next_hop.adva, NULL, now_ms);
    return TAVRN_ROUTER_DATA_TERMINAL_DECLINED;
}

tavrn_full_maintenance_binding_status_t tavrn_full_maintenance_binding_install(
    tavrn_router_t *router, tavrn_mentorship_t *mentorship,
    tavrn_maintenance_t *maintenance)
{
    tavrn_router_control_augmentation_t augmentation;
    tavrn_router_data_terminal_hook_t terminal_hook;

    if (router == NULL || mentorship == NULL || maintenance == NULL ||
        maintenance->router != router) {
        return TAVRN_FULL_MAINTENANCE_BINDING_INVALID;
    }
    memset(&augmentation, 0, sizeof(augmentation));
    augmentation.context = maintenance;
    augmentation.prepare = full_metadata_prepare;
    augmentation.admitted = full_metadata_admitted;
    augmentation.completed = full_metadata_completed;
    augmentation.received = full_control_received;
    memset(&terminal_hook, 0, sizeof(terminal_hook));
    terminal_hook.context = maintenance;
    terminal_hook.handle = tavrn_full_maintenance_tc_on_retry_exhausted;
    if (tavrn_router_set_control_augmentation(router, &augmentation) !=
            TAVRN_ROUTER_EVENT_OK ||
        tavrn_router_set_data_terminal_hook(router, &terminal_hook) !=
            TAVRN_ROUTER_EVENT_OK ||
        tavrn_mentorship_bind_tc_metadata(mentorship, &maintenance->tc_metadata) !=
            TAVRN_MENTORSHIP_OK) {
        return TAVRN_FULL_MAINTENANCE_BINDING_INVALID;
    }
    return TAVRN_FULL_MAINTENANCE_BINDING_OK;
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
    result_out->targeted_owner_status = tavrn_maintenance_targeted_owner_tick(
        input.maintenance, input.router, input.router->link, input.maintenance->gtt,
        now_ms, &result_out->targeted_owner_action);
    result_out->verification_owner_status =
        tavrn_maintenance_verification_owner_tick(
            input.maintenance, input.router, input.router->link,
            input.maintenance->gtt, now_ms,
             &result_out->verification_owner_action);
    result_out->metadata_owner_status = tavrn_maintenance_metadata_owner_tick(
        input.maintenance, now_ms);
    result_out->tc_owner_status = tavrn_maintenance_tc_owner_tick(input.maintenance,
                                                                    now_ms);
    result_out->post_tick = tavrn_maintenance_owner_post_tick(input.maintenance, now_ms);
    {
        const tavrn_maintenance_counters_t *counters =
            tavrn_maintenance_counters(input.maintenance);

        if (counters != NULL) {
            result_out->maintenance_counters = *counters;
        }
    }
    result_out->status = (result_out->router_status == AODV_STATUS_INVALID ||
                      result_out->mentorship_status == TAVRN_MENTORSHIP_INVALID ||
                      result_out->broadcast_snapshot_status !=
                          TAVRN_ROUTER_LOCAL_BROADCAST_OK ||
                       result_out->broadcast_observation_status == TAVRN_MAINTENANCE_INVALID ||
                        result_out->activation_status == TAVRN_MAINTENANCE_INVALID ||
                        result_out->targeted_owner_status == TAVRN_TARGETED_FRESHNESS_INVALID ||
                          result_out->verification_owner_status ==
                              TAVRN_RREQ_VERIFICATION_INVALID ||
                          result_out->metadata_owner_status == TAVRN_TC_METADATA_INVALID ||
                          result_out->tc_owner_status == TAVRN_TC_METADATA_INVALID ||
                       result_out->post_tick.sweep_status !=
                          TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK ||
                      result_out->post_tick.maintenance_status == TAVRN_MAINTENANCE_INVALID) ?
        TAVRN_FULL_MAINTENANCE_BINDING_INVALID : TAVRN_FULL_MAINTENANCE_BINDING_OK;
    return result_out->status;
}
