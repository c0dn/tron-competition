#include "tavrn_full_repair_binding.h"

#include <string.h>

static int time_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static int direct_peer_equal(const tavrn_direct_peer_t *left,
                             const tavrn_direct_peer_t *right)
{
    return left != NULL && right != NULL &&
        left->logical_id.width == right->logical_id.width &&
        left->logical_id.value == right->logical_id.value &&
        memcmp(left->adva.bytes, right->adva.bytes, TAVRN_ADVA_LEN) == 0;
}

static tavrn_maintenance_high_token_purpose_t maintenance_purpose(
    tavrn_repair_rreq_purpose_t purpose)
{
    return purpose == TAVRN_REPAIR_RREQ_SMART_TTL ?
        TAVRN_FULL_REPAIR_HIGH_PURPOSE_SMART :
        TAVRN_FULL_REPAIR_HIGH_PURPOSE_FULL;
}

static int repair_purpose_valid(tavrn_repair_rreq_purpose_t purpose)
{
    return purpose == TAVRN_REPAIR_RREQ_SMART_TTL ||
        purpose == TAVRN_REPAIR_RREQ_FULL_SCOPE;
}

static tavrn_repair_high_token_status_t repair_high_status(
    tavrn_maintenance_high_token_status_t status)
{
    if (status == TAVRN_MAINTENANCE_HIGH_TOKEN_OK) {
        return TAVRN_REPAIR_HIGH_TOKEN_OK;
    }
    return status == TAVRN_MAINTENANCE_HIGH_TOKEN_BUSY ?
        TAVRN_REPAIR_HIGH_TOKEN_BUSY : TAVRN_REPAIR_HIGH_TOKEN_INVALID;
}

static tavrn_repair_failed_hop_verification_status_t repair_schedule_status(
    tavrn_maintenance_failed_hop_verification_status_t status)
{
    if (status == TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED ||
        status == TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_COALESCED ||
        status == TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_RETIRED_NOT_FOUND ||
        status == TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_RETIRED_DEPARTED) {
        return TAVRN_REPAIR_FAILED_HOP_VERIFICATION_OK;
    }
    return status == TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_BUSY ?
        TAVRN_REPAIR_FAILED_HOP_VERIFICATION_BUSY :
        TAVRN_REPAIR_FAILED_HOP_VERIFICATION_INVALID;
}

static tavrn_full_repair_binding_status_t binding_status(
    tavrn_repair_status_t status)
{
    if (status == TAVRN_REPAIR_OK) {
        return TAVRN_FULL_REPAIR_BINDING_OK;
    }
    if (status == TAVRN_REPAIR_BUSY) {
        return TAVRN_FULL_REPAIR_BINDING_BUSY;
    }
    if (status == TAVRN_REPAIR_IGNORED || status == TAVRN_REPAIR_DECLINED) {
        return TAVRN_FULL_REPAIR_BINDING_IGNORED;
    }
    return TAVRN_FULL_REPAIR_BINDING_INVALID;
}

static tavrn_maintenance_high_token_status_t repair_completion_status(
    tavrn_repair_status_t status)
{
    if (status == TAVRN_REPAIR_OK) {
        return TAVRN_MAINTENANCE_HIGH_TOKEN_OK;
    }
    return status == TAVRN_REPAIR_BUSY ? TAVRN_MAINTENANCE_HIGH_TOKEN_BUSY :
                                        TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
}

static tavrn_maintenance_high_token_status_t repair_scheduler_completion(
    void *context, const tavrn_maintenance_high_token_completion_t *completion)
{
    tavrn_full_repair_binding_t *binding = context;
    tavrn_repair_active_rreq_snapshot_t active;
    tavrn_repair_completion_t repair_completion;
    tavrn_repair_status_t status;

    if (binding == NULL || binding->initialized == 0u || completion == NULL ||
        completion->owner != TAVRN_FULL_REPAIR_HIGH_TOKEN_OWNER ||
        (completion->purpose != TAVRN_FULL_REPAIR_HIGH_PURPOSE_SMART &&
         completion->purpose != TAVRN_FULL_REPAIR_HIGH_PURPOSE_FULL) ||
        tavrn_repair_active_rreq_snapshot(binding->repair, &active) !=
            TAVRN_REPAIR_OK ||
        active.token != completion->token ||
        maintenance_purpose(active.purpose) != completion->purpose) {
        return TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
    }
    memset(&repair_completion, 0, sizeof(repair_completion));
    repair_completion.purpose = active.purpose;
    repair_completion.token_owner = active.token_owner;
    repair_completion.rreq_attempt = active.attempt;
    repair_completion.token = active.token;
    repair_completion.completed_channel_mask = completion->completed_channel_mask;
    repair_completion.kind = completion->kind ==
            TAVRN_MAINTENANCE_HIGH_TOKEN_COMPLETION_TX_DONE ?
        TAVRN_REPAIR_COMPLETION_RREQ_TX_DONE :
        TAVRN_REPAIR_COMPLETION_RREQ_TX_FAILED;
    status = tavrn_repair_complete(binding->repair, &repair_completion,
                                   completion->completed_at_ms, NULL);
    return repair_completion_status(status);
}

static aodv_failure_status_t repair_begin_deferred_rerr(
    void *context, const tavrn_direct_peer_t *failed_next_hop,
    const tavrn_logical_id_t *destination, uint32_t now_ms)
{
    tavrn_full_repair_binding_t *binding = context;

    if (binding == NULL || binding->aodv == NULL) {
        return AODV_FAILURE_INVALID;
    }
    return aodv_core_report_link_failure(
        binding->aodv, failed_next_hop, destination,
        AODV_LINK_FAILURE_DEFER_REPAIR_DESTINATION_RERR, now_ms);
}

static aodv_failure_status_t repair_finish_deferred_rerr(
    void *context, const tavrn_logical_id_t *destination,
    aodv_deferred_rerr_decision_t decision, uint32_t now_ms)
{
    tavrn_full_repair_binding_t *binding = context;

    return binding == NULL || binding->aodv == NULL ? AODV_FAILURE_INVALID :
        aodv_core_finish_deferred_rerr(binding->aodv, destination, decision, now_ms);
}

static aodv_single_rreq_status_t repair_create_single_rreq(
    void *context, const tavrn_logical_id_t *destination, uint8_t scope,
    uint32_t now_ms, aodv_action_t *action_out)
{
    tavrn_full_repair_binding_t *binding = context;

    return binding == NULL || binding->aodv == NULL ? AODV_SINGLE_RREQ_INVALID :
        aodv_core_create_single_rreq(binding->aodv, destination, scope, now_ms,
                                     action_out);
}

static tavrn_repair_high_token_status_t repair_reserve_high_token(
    void *context, tavrn_repair_high_token_owner_t owner,
    tavrn_repair_rreq_purpose_t purpose, uint16_t *token_out)
{
    tavrn_full_repair_binding_t *binding = context;

    if (binding == NULL || binding->maintenance == NULL || binding->link == NULL ||
        owner != TAVRN_REPAIR_HIGH_TOKEN_OWNER_REPAIR || !repair_purpose_valid(purpose)) {
        return TAVRN_REPAIR_HIGH_TOKEN_INVALID;
    }
    return repair_high_status(tavrn_maintenance_high_token_reserve(
        binding->maintenance, binding->link, TAVRN_FULL_REPAIR_HIGH_TOKEN_OWNER,
        maintenance_purpose(purpose), repair_scheduler_completion, binding, token_out));
}

static tavrn_repair_high_token_status_t repair_release_high_token(
    void *context, tavrn_repair_high_token_owner_t owner,
    tavrn_repair_rreq_purpose_t purpose, uint16_t token)
{
    tavrn_full_repair_binding_t *binding = context;

    if (binding == NULL || binding->maintenance == NULL || binding->link == NULL ||
        owner != TAVRN_REPAIR_HIGH_TOKEN_OWNER_REPAIR || !repair_purpose_valid(purpose)) {
        return TAVRN_REPAIR_HIGH_TOKEN_INVALID;
    }
    return repair_high_status(tavrn_maintenance_high_token_release(
        binding->maintenance, binding->link, TAVRN_FULL_REPAIR_HIGH_TOKEN_OWNER,
        maintenance_purpose(purpose), token));
}

static tavrn_repair_failed_hop_verification_status_t
repair_schedule_held_failed_hop_verification(
    void *context, const tavrn_direct_peer_t *failed_next_hop, uint32_t now_ms)
{
    tavrn_full_repair_binding_t *binding = context;

    if (binding == NULL || binding->maintenance == NULL || failed_next_hop == NULL) {
        return TAVRN_REPAIR_FAILED_HOP_VERIFICATION_INVALID;
    }
    return repair_schedule_status(tavrn_maintenance_schedule_failed_hop_verification(
        binding->maintenance, &failed_next_hop->adva, now_ms));
}

static tavrn_router_candidate_reservation_status_t repair_candidate_reserve(
    void *context, const tavrn_rx_data_candidate_t *candidate,
    tavrn_router_candidate_reservation_token_t *token_out, uint32_t now_ms)
{
    tavrn_full_repair_binding_t *binding = context;
    tavrn_repair_candidate_result_t result;
    tavrn_repair_status_t status;

    if (token_out != NULL) {
        *token_out = TAVRN_ROUTER_CANDIDATE_RESERVATION_TOKEN_NONE;
    }
    if (binding == NULL || candidate == NULL || token_out == NULL) {
        return TAVRN_ROUTER_CANDIDATE_INVALID;
    }
    memset(&result, 0, sizeof(result));
    status = tavrn_repair_reserve_candidate(binding->repair, &candidate->data, now_ms,
                                            &result);
    if (status != TAVRN_REPAIR_OK) {
        return TAVRN_ROUTER_CANDIDATE_INVALID;
    }
    if (result.status == TAVRN_REPAIR_CANDIDATE_RESERVED) {
        *token_out = result.reservation.token;
        return TAVRN_ROUTER_CANDIDATE_RESERVED;
    }
    if (result.status == TAVRN_REPAIR_CANDIDATE_BUSY) {
        return TAVRN_ROUTER_CANDIDATE_BUSY;
    }
    return result.status == TAVRN_REPAIR_CANDIDATE_NOT_APPLICABLE ?
        TAVRN_ROUTER_CANDIDATE_NOT_APPLICABLE : TAVRN_ROUTER_CANDIDATE_INVALID;
}

static tavrn_router_candidate_completion_status_t repair_candidate_finish(
    void *context, tavrn_router_candidate_reservation_token_t token,
    const tavrn_rx_data_candidate_t *candidate, uint32_t now_ms, uint8_t commit)
{
    tavrn_full_repair_binding_t *binding = context;
    tavrn_repair_reservation_t reservation;
    tavrn_repair_status_t status;

    if (binding == NULL || candidate == NULL || token ==
        TAVRN_ROUTER_CANDIDATE_RESERVATION_TOKEN_NONE) {
        return TAVRN_ROUTER_CANDIDATE_COMPLETION_INVALID;
    }
    memset(&reservation, 0, sizeof(reservation));
    reservation.token = token;
    reservation.destination = candidate->data.final_destination;
    reservation.valid = 1u;
    status = commit != 0u ? tavrn_repair_commit_candidate(
        binding->repair, &reservation, &candidate->data, now_ms) :
        tavrn_repair_rollback_candidate(binding->repair, &reservation, now_ms);
    return status == TAVRN_REPAIR_OK ? TAVRN_ROUTER_CANDIDATE_COMPLETION_OK :
                                       TAVRN_ROUTER_CANDIDATE_COMPLETION_INVALID;
}

static tavrn_router_candidate_completion_status_t repair_candidate_commit(
    void *context, tavrn_router_candidate_reservation_token_t token,
    const tavrn_rx_data_candidate_t *candidate, uint32_t now_ms)
{
    return repair_candidate_finish(context, token, candidate, now_ms, 1u);
}

static tavrn_router_candidate_completion_status_t repair_candidate_rollback(
    void *context, tavrn_router_candidate_reservation_token_t token,
    const tavrn_rx_data_candidate_t *candidate, uint32_t now_ms)
{
    return repair_candidate_finish(context, token, candidate, now_ms, 0u);
}

/* Local repair owns only transit custody.  Every retry-exhausted DATA event,
 * including originated DATA and transit work that repair declines, still
 * retains the immediate-hop verification obligation through maintenance. */
static tavrn_router_data_terminal_disposition_t
repair_schedule_failed_hop_verification(tavrn_full_repair_binding_t *binding,
                                        const tavrn_link_event_t *event,
                                        uint32_t now_ms)
{
    tavrn_maintenance_failed_hop_verification_status_t status;

    if (binding == NULL || binding->maintenance == NULL || event == NULL) {
        return TAVRN_ROUTER_DATA_TERMINAL_INVALID;
    }
    status = tavrn_maintenance_schedule_failed_hop_verification(
        binding->maintenance, &event->detail.owned_data.next_hop.adva, now_ms);
    if (status == TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED ||
        status == TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_COALESCED ||
        status == TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_RETIRED_NOT_FOUND ||
        status == TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_RETIRED_DEPARTED) {
        return TAVRN_ROUTER_DATA_TERMINAL_DECLINED;
    }
    return status == TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_BUSY ?
        TAVRN_ROUTER_DATA_TERMINAL_BUSY : TAVRN_ROUTER_DATA_TERMINAL_INVALID;
}

static tavrn_router_data_terminal_disposition_t repair_terminal(
    void *context, const tavrn_link_event_t *event, uint32_t now_ms)
{
    tavrn_full_repair_binding_t *binding = context;
    tavrn_repair_link_custody_terminal_input_t input;
    tavrn_repair_link_custody_result_t observed;
    tavrn_repair_status_t status;

    if (binding == NULL || binding->initialized == 0u || event == NULL) {
        return TAVRN_ROUTER_DATA_TERMINAL_INVALID;
    }
    memset(&input, 0, sizeof(input));
    memset(&observed, 0, sizeof(observed));
    if (event->type == TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED) {
        input.next_hop = event->detail.transferred_data.next_hop;
        input.data = event->detail.transferred_data.data;
        status = tavrn_repair_observe_link_custody(
            binding->repair, &input, TAVRN_REPAIR_LINK_CUSTODY_TRANSFERRED,
            now_ms, &observed);
        if (status == TAVRN_REPAIR_INVALID) {
            return TAVRN_ROUTER_DATA_TERMINAL_INVALID;
        }
        return status == TAVRN_REPAIR_OK && observed.released_reservation != 0u ?
            TAVRN_ROUTER_DATA_TERMINAL_OBSERVED :
            TAVRN_ROUTER_DATA_TERMINAL_DECLINED;
    }
    if (event->type != TAVRN_LINK_EVENT_CUSTODY_REJECTED &&
        event->type != TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED &&
        event->type != TAVRN_LINK_EVENT_CUSTODY_BUSY_EXPIRED &&
        event->type != TAVRN_LINK_EVENT_RETRY_EXHAUSTED &&
        event->type != TAVRN_LINK_EVENT_RADIO_FAULT_TERMINAL &&
        event->type != TAVRN_LINK_EVENT_SERVICE_FAULT_TERMINAL) {
        return TAVRN_ROUTER_DATA_TERMINAL_DECLINED;
    }
    if (event->detail.owned_data.data.ownership != TAVRN_DATA_TRANSIT) {
        if (event->type == TAVRN_LINK_EVENT_RETRY_EXHAUSTED) {
            return repair_schedule_failed_hop_verification(binding, event, now_ms);
        }
        return TAVRN_ROUTER_DATA_TERMINAL_DECLINED;
    }
    input.next_hop = event->detail.owned_data.next_hop;
    input.data = event->detail.owned_data.data;
    if (event->type != TAVRN_LINK_EVENT_RETRY_EXHAUSTED) {
        status = tavrn_repair_observe_link_custody(
            binding->repair, &input, TAVRN_REPAIR_LINK_CUSTODY_REJECTED,
            now_ms, &observed);
        if (status == TAVRN_REPAIR_INVALID) {
            return TAVRN_ROUTER_DATA_TERMINAL_INVALID;
        }
        return status == TAVRN_REPAIR_OK && observed.released_reservation != 0u ?
            TAVRN_ROUTER_DATA_TERMINAL_OBSERVED :
            TAVRN_ROUTER_DATA_TERMINAL_DECLINED;
    }
    input.owned = event->detail.owned_data;
    input.owned_present = 1u;
    status = tavrn_repair_observe_link_custody(
        binding->repair, &input, TAVRN_REPAIR_LINK_CUSTODY_RETRY_EXHAUSTED,
        now_ms, &observed);
    if (status == TAVRN_REPAIR_OK && observed.converted_to_buffer != 0u) {
        return TAVRN_ROUTER_DATA_TERMINAL_OWNED;
    }
    if (status == TAVRN_REPAIR_INVALID) {
        return TAVRN_ROUTER_DATA_TERMINAL_INVALID;
    }
    {
        tavrn_link_custody_data_snapshot_t link_snapshot;
        tavrn_repair_retry_input_t retry;
        tavrn_repair_setup_result_t setup;
        tavrn_router_scope_hint_t hint;
        uint8_t index;

        memset(&link_snapshot, 0, sizeof(link_snapshot));
        memset(&retry, 0, sizeof(retry));
        memset(&setup, 0, sizeof(setup));
        memset(&hint, 0, sizeof(hint));
        if (tavrn_link_v2_custody_snapshot_for_destination(
                binding->link, &input.data.final_destination, &link_snapshot) !=
            TAVRN_LINK_CUSTODY_SNAPSHOT_OK ||
            tavrn_router_initial_scope(binding->router, &input.data.final_destination,
                                       binding->repair->config.net_diameter, now_ms,
                                       &hint) != TAVRN_ROUTER_SCOPE_OK) {
            status = TAVRN_REPAIR_DECLINED;
        } else {
            retry.trigger = TAVRN_LINK_EVENT_RETRY_EXHAUSTED;
            retry.owned = event->detail.owned_data;
            retry.smart_ttl_scope = hint.has_hint != 0u ? hint.initial_scope :
                                                         binding->repair->config.net_diameter;
            for (index = 0u; index < link_snapshot.count; index++) {
                if (link_snapshot.records[index].data.ownership != TAVRN_DATA_TRANSIT) {
                    continue;
                }
                if (retry.same_destination_link_custody.count >=
                    TAVRN_REPAIR_BUFFER_CAPACITY) {
                    status = TAVRN_REPAIR_DECLINED;
                    break;
                }
                retry.same_destination_link_custody.records[
                    retry.same_destination_link_custody.count].next_hop =
                    link_snapshot.records[index].next_hop;
                retry.same_destination_link_custody.records[
                    retry.same_destination_link_custody.count].data =
                    link_snapshot.records[index].data;
                retry.same_destination_link_custody.count++;
            }
            if (status != TAVRN_REPAIR_DECLINED) {
                status = tavrn_repair_retry_exhausted(binding->repair, &retry, now_ms,
                                                      &setup);
            }
        }
        if (status == TAVRN_REPAIR_OK &&
            setup.disposition == TAVRN_REPAIR_RETRY_OWNED) {
            return TAVRN_ROUTER_DATA_TERMINAL_OWNED;
        }
        if (status == TAVRN_REPAIR_INVALID) {
            return TAVRN_ROUTER_DATA_TERMINAL_INVALID;
        }
        return repair_schedule_failed_hop_verification(binding, event, now_ms);
    }
    return TAVRN_ROUTER_DATA_TERMINAL_DECLINED;
}

static tavrn_repair_status_t reconcile_link_reservations(
    tavrn_full_repair_binding_t *binding, uint32_t now_ms)
{
    tavrn_repair_snapshot_t repair_snapshot;
    tavrn_link_custody_data_snapshot_t link_snapshot;
    tavrn_repair_link_custody_snapshot_t repair_link_snapshot;
    tavrn_repair_link_custody_reconcile_result_t reconciliation;
    uint8_t index;

    memset(&repair_snapshot, 0, sizeof(repair_snapshot));
    if (tavrn_repair_snapshot(binding->repair, &repair_snapshot) != TAVRN_REPAIR_OK) {
        return TAVRN_REPAIR_INVALID;
    }
    if (repair_snapshot.reserved_link_slots == 0u) {
        return TAVRN_REPAIR_OK;
    }
    memset(&link_snapshot, 0, sizeof(link_snapshot));
    memset(&repair_link_snapshot, 0, sizeof(repair_link_snapshot));
    memset(&reconciliation, 0, sizeof(reconciliation));
    if (tavrn_link_v2_custody_snapshot_for_destination(
            binding->link, &repair_snapshot.destination, &link_snapshot) !=
            TAVRN_LINK_CUSTODY_SNAPSHOT_OK ||
        link_snapshot.count > TAVRN_REPAIR_BUFFER_CAPACITY) {
        return TAVRN_REPAIR_INVALID;
    }
    for (index = 0u; index < link_snapshot.count; index++) {
        repair_link_snapshot.records[index].next_hop =
            link_snapshot.records[index].next_hop;
        repair_link_snapshot.records[index].data = link_snapshot.records[index].data;
    }
    repair_link_snapshot.count = link_snapshot.count;
    return tavrn_repair_reconcile_link_custody(binding->repair,
                                               &repair_link_snapshot, now_ms,
                                               &reconciliation);
}

tavrn_full_repair_binding_status_t tavrn_full_repair_binding_init(
    tavrn_full_repair_binding_t *binding,
    const tavrn_full_repair_binding_config_t *config)
{
    tavrn_repair_config_t repair_config;
    tavrn_router_candidate_reservation_port_t candidate;
    tavrn_router_data_terminal_hook_t terminal;

    if (binding == NULL || config == NULL || config->repair == NULL ||
        config->router == NULL || config->link == NULL || config->aodv == NULL ||
        config->maintenance == NULL || config->gtt == NULL || config->net_diameter == 0u ||
        config->path_discovery_ms == 0u || config->repair_timeout_ms == 0u ||
        config->repair_cooldown_ms == 0u || config->router->link != config->link ||
        config->router->aodv != config->aodv || config->maintenance->router != config->router ||
        config->maintenance->gtt != config->gtt) {
        return TAVRN_FULL_REPAIR_BINDING_INVALID;
    }
    memset(binding, 0, sizeof(*binding));
    binding->repair = config->repair;
    binding->router = config->router;
    binding->link = config->link;
    binding->aodv = config->aodv;
    binding->maintenance = config->maintenance;
    binding->gtt = config->gtt;
    binding->operations.context = binding;
    binding->operations.begin_deferred_rerr = repair_begin_deferred_rerr;
    binding->operations.finish_deferred_rerr = repair_finish_deferred_rerr;
    binding->operations.create_single_rreq = repair_create_single_rreq;
    binding->operations.reserve_high_token = repair_reserve_high_token;
    binding->operations.release_high_token = repair_release_high_token;
    binding->operations.schedule_held_failed_hop_verification =
        repair_schedule_held_failed_hop_verification;
    memset(&repair_config, 0, sizeof(repair_config));
    repair_config.enabled = 1u;
    repair_config.net_diameter = config->net_diameter;
    repair_config.data_capacity = TAVRN_REPAIR_BUFFER_CAPACITY;
    repair_config.path_discovery_ms = config->path_discovery_ms;
    repair_config.repair_timeout_ms = config->repair_timeout_ms;
    repair_config.repair_cooldown_ms = config->repair_cooldown_ms;
    repair_config.operations = &binding->operations;
    if (tavrn_repair_init(binding->repair, &repair_config) != TAVRN_REPAIR_OK) {
        return TAVRN_FULL_REPAIR_BINDING_INVALID;
    }
    memset(&candidate, 0, sizeof(candidate));
    candidate.context = binding;
    candidate.reserve = repair_candidate_reserve;
    candidate.commit = repair_candidate_commit;
    candidate.rollback = repair_candidate_rollback;
    memset(&terminal, 0, sizeof(terminal));
    terminal.context = binding;
    terminal.handle = repair_terminal;
    if (tavrn_router_set_candidate_reservation_port(binding->router, &candidate) !=
            TAVRN_ROUTER_EVENT_OK ||
        tavrn_router_set_data_terminal_hook(binding->router, &terminal) !=
            TAVRN_ROUTER_EVENT_OK) {
        (void)tavrn_router_set_candidate_reservation_port(binding->router, NULL);
        return TAVRN_FULL_REPAIR_BINDING_INVALID;
    }
    binding->initialized = 1u;
    return TAVRN_FULL_REPAIR_BINDING_OK;
}

tavrn_full_repair_binding_status_t tavrn_full_repair_binding_tick(
    tavrn_full_repair_binding_t *binding, uint32_t now_ms,
    tavrn_full_repair_binding_tick_result_t *result_out)
{
    tavrn_repair_rreq_enqueue_t enqueue;
    tavrn_repair_completion_t completion;
    uint16_t evicted = BLE_MESH_TX_TOKEN_NONE;

    if (result_out != NULL) {
        memset(result_out, 0, sizeof(*result_out));
        result_out->repair_status = TAVRN_REPAIR_INVALID;
        result_out->reforward_status = TAVRN_ROUTER_REFORWARD_INVALID;
        result_out->eviction.external_status = TAVRN_MAINTENANCE_HIGH_TOKEN_NOT_FOUND;
    }
    if (binding == NULL || binding->initialized == 0u || result_out == NULL) {
        return TAVRN_FULL_REPAIR_BINDING_INVALID;
    }
    result_out->repair_status = reconcile_link_reservations(binding, now_ms);
    if (result_out->repair_status != TAVRN_REPAIR_OK) {
        return TAVRN_FULL_REPAIR_BINDING_INVALID;
    }
    result_out->repair_status = tavrn_repair_owner_tick(binding->repair, now_ms,
                                                          &result_out->action);
    if (result_out->repair_status != TAVRN_REPAIR_OK) {
        return binding_status(result_out->repair_status);
    }
    if (result_out->action.type == TAVRN_REPAIR_ACTION_NONE) {
        return TAVRN_FULL_REPAIR_BINDING_OK;
    }
    result_out->action_present = 1u;
    if (result_out->action.type == TAVRN_REPAIR_ACTION_RREQ_CREATED) {
        tavrn_router_tracked_rreq_status_t router_status =
            tavrn_router_enqueue_tracked_rreq(binding->router,
                                              &result_out->action.aodv_action,
                                              result_out->action.token, now_ms, &evicted);

        if (evicted != BLE_MESH_TX_TOKEN_NONE &&
            tavrn_maintenance_high_token_evicted(binding->maintenance, binding->router,
                binding->link, evicted, now_ms, &result_out->eviction) !=
                TAVRN_MAINTENANCE_HIGH_TOKEN_OK) {
            return TAVRN_FULL_REPAIR_BINDING_INVALID;
        }
        memset(&enqueue, 0, sizeof(enqueue));
        enqueue.purpose = result_out->action.purpose;
        enqueue.token_owner = result_out->action.token_owner;
        enqueue.rreq_attempt = result_out->action.rreq_attempt;
        enqueue.token = result_out->action.token;
        enqueue.status = router_status == TAVRN_ROUTER_TRACKED_RREQ_OK ?
            TAVRN_REPAIR_RREQ_ENQUEUE_ADMITTED :
            router_status == TAVRN_ROUTER_TRACKED_RREQ_BUSY ?
                TAVRN_REPAIR_RREQ_ENQUEUE_BUSY :
                router_status == TAVRN_ROUTER_TRACKED_RREQ_LOCAL_NOT_ATTEMPTED ?
                    TAVRN_REPAIR_RREQ_ENQUEUE_LOCAL_NOT_ATTEMPTED :
                    TAVRN_REPAIR_RREQ_ENQUEUE_INVALID;
        result_out->repair_status = tavrn_repair_rreq_enqueue(binding->repair, &enqueue,
                                                               now_ms);
        return binding_status(result_out->repair_status);
    }
    if (result_out->action.type == TAVRN_REPAIR_ACTION_FLUSH_DATA) {
        result_out->reforward_status = tavrn_router_reforward_transit_data(
            binding->router, &result_out->action.owned.data, now_ms, NULL);
        if (result_out->reforward_status == TAVRN_ROUTER_REFORWARD_BUSY) {
            result_out->repair_status = tavrn_repair_data_action_not_admitted(
                binding->repair, &result_out->action, now_ms);
            return binding_status(result_out->repair_status);
        }
        if (result_out->reforward_status == TAVRN_ROUTER_REFORWARD_NOT_FOUND ||
            result_out->reforward_status == TAVRN_ROUTER_REFORWARD_INVALID) {
            if (tavrn_router_release_transit_pin(binding->router,
                                                 &result_out->action.owned.data,
                                                 now_ms) != TAVRN_ROUTER_EVENT_OK) {
                return TAVRN_FULL_REPAIR_BINDING_INVALID;
            }
            memset(&completion, 0, sizeof(completion));
            completion.kind = TAVRN_REPAIR_COMPLETION_DATA_DROPPED;
        } else if (result_out->reforward_status != TAVRN_ROUTER_REFORWARD_OK) {
            return TAVRN_FULL_REPAIR_BINDING_INVALID;
        } else {
            memset(&completion, 0, sizeof(completion));
            completion.kind = TAVRN_REPAIR_COMPLETION_DATA_FLUSHED;
        }
    } else if (result_out->action.type == TAVRN_REPAIR_ACTION_DROP_DATA) {
        if (tavrn_router_release_transit_pin(binding->router,
                                             &result_out->action.owned.data,
                                             now_ms) != TAVRN_ROUTER_EVENT_OK) {
            return TAVRN_FULL_REPAIR_BINDING_INVALID;
        }
        memset(&completion, 0, sizeof(completion));
        completion.kind = TAVRN_REPAIR_COMPLETION_DATA_DROPPED;
    } else {
        return TAVRN_FULL_REPAIR_BINDING_INVALID;
    }
    completion.token = result_out->action.token;
    completion.buffer_index = result_out->action.buffer_index;
    result_out->repair_status = tavrn_repair_complete(binding->repair, &completion,
                                                       now_ms, &result_out->terminal);
    return binding_status(result_out->repair_status);
}

tavrn_router_control_intercept_status_t tavrn_full_repair_binding_receive_rrep(
    tavrn_full_repair_binding_t *binding,
    const tavrn_rx_control_event_t *control_event, uint32_t now_ms)
{
    tavrn_repair_active_rreq_snapshot_t active;
    aodv_control_input_t input;
    aodv_route_snapshot_t route;
    tavrn_repair_rrep_completion_t completion;
    aodv_status_t ingest;
    tavrn_repair_status_t status;

    if (binding == NULL || binding->initialized == 0u || control_event == NULL) {
        return TAVRN_ROUTER_CONTROL_INTERCEPT_INVALID;
    }
    if (control_event->control.type != TAVRN_WIRE_E_RREP) {
        return TAVRN_ROUTER_CONTROL_INTERCEPT_IGNORED;
    }
    if (tavrn_repair_active_rreq_snapshot(binding->repair, &active) !=
        TAVRN_REPAIR_OK) {
        return TAVRN_ROUTER_CONTROL_INTERCEPT_IGNORED;
    }
    memset(&input, 0, sizeof(input));
    input.transmitter = control_event->transmitter;
    input.control = control_event->control;
    if (aodv_core_rrep_matches_attempt(binding->aodv, &input, &active.attempt) !=
        AODV_RREP_ATTEMPT_MATCH) {
        return TAVRN_ROUTER_CONTROL_INTERCEPT_IGNORED;
    }
    if (active.tx_done == 0u || time_due(now_ms, active.total_deadline_ms) ||
        time_due(now_ms, active.stage_response_deadline_ms) ||
        direct_peer_equal(&control_event->transmitter, &active.failed_next_hop)) {
        return TAVRN_ROUTER_CONTROL_INTERCEPT_CONSUMED;
    }
    ingest = tavrn_router_ingest_rrep_for_attempt(binding->router, control_event,
                                                   &active.attempt, now_ms);
    if (ingest == AODV_STATUS_BUSY) {
        return TAVRN_ROUTER_CONTROL_INTERCEPT_BUSY;
    }
    if (ingest != AODV_STATUS_OK && ingest != AODV_STATUS_DUPLICATE) {
        return TAVRN_ROUTER_CONTROL_INTERCEPT_CONSUMED;
    }
    memset(&route, 0, sizeof(route));
    memset(&completion, 0, sizeof(completion));
    completion.purpose = active.purpose;
    completion.token_owner = active.token_owner;
    completion.rreq_attempt = active.attempt;
    completion.token = active.token;
    completion.transmitter = control_event->transmitter;
    completion.router_admitted = 1u;
    completion.route_installed = tavrn_router_route_to_subject(
        binding->router, &active.destination, now_ms, &route) == TAVRN_ROUTER_ROUTE_OK;
    if (completion.route_installed != 0u) {
        completion.installed_next_hop = route.next_hop;
    }
    status = tavrn_repair_receive_rrep(binding->repair, &completion, now_ms, NULL);
    if (status == TAVRN_REPAIR_BUSY) {
        return TAVRN_ROUTER_CONTROL_INTERCEPT_BUSY;
    }
    return status == TAVRN_REPAIR_INVALID ? TAVRN_ROUTER_CONTROL_INTERCEPT_INVALID :
                                            TAVRN_ROUTER_CONTROL_INTERCEPT_CONSUMED;
}
