#include "tavrn_repair.h"

#include <string.h>

#define TAVRN_REPAIR_HALF_RANGE 0x80000000u

static int time_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static int logical_id_valid(const tavrn_logical_id_t *id)
{
    if (id == NULL) {
        return 0;
    }
    if (id->width == TAVRN_IDENTITY_SID16) {
        return id->value != 0u && id->value != 0xffffu;
    }
    return id->width == TAVRN_IDENTITY_SID8 &&
        (id->value & 0xff00u) == 0u && id->value != 0u && id->value != 0xffu;
}

static int logical_id_equal(const tavrn_logical_id_t *left,
                            const tavrn_logical_id_t *right)
{
    return left != NULL && right != NULL && left->width == right->width &&
        left->value == right->value;
}

static int bytes_equal(const uint8_t *left, const uint8_t *right, uint8_t length)
{
    uint8_t index;

    if (left == NULL || right == NULL) {
        return 0;
    }
    for (index = 0u; index < length; index++) {
        if (left[index] != right[index]) {
            return 0;
        }
    }
    return 1;
}

static int adva_valid(const tavrn_adva_t *adva)
{
    uint8_t index;
    uint8_t all_zero = 1u;
    uint8_t all_one = 1u;
    uint8_t random_zero = 1u;
    uint8_t random_one = 1u;

    if (adva == NULL || (adva->bytes[5] & 0xc0u) != 0xc0u) {
        return 0;
    }
    for (index = 0u; index < TAVRN_ADVA_LEN; index++) {
        if (adva->bytes[index] != 0u) {
            all_zero = 0u;
        }
        if (adva->bytes[index] != 0xffu) {
            all_one = 0u;
        }
        if (index < TAVRN_ADVA_LEN - 1u && adva->bytes[index] != 0u) {
            random_zero = 0u;
        }
        if (index < TAVRN_ADVA_LEN - 1u && adva->bytes[index] != 0xffu) {
            random_one = 0u;
        }
    }
    if ((adva->bytes[5] & 0x3fu) != 0u) {
        random_zero = 0u;
    }
    if ((adva->bytes[5] & 0x3fu) != 0x3fu) {
        random_one = 0u;
    }
    return all_zero == 0u && all_one == 0u && random_zero == 0u &&
        random_one == 0u;
}

static int direct_peer_valid(const tavrn_direct_peer_t *peer)
{
    uint16_t expected;

    if (peer == NULL || !adva_valid(&peer->adva) ||
        !logical_id_valid(&peer->logical_id)) {
        return 0;
    }
    expected = peer->logical_id.width == TAVRN_IDENTITY_SID8 ?
        peer->adva.bytes[0] : (uint16_t)peer->adva.bytes[0] |
            ((uint16_t)peer->adva.bytes[1] << 8);
    return peer->logical_id.value == expected;
}

static int direct_peer_equal(const tavrn_direct_peer_t *left,
                             const tavrn_direct_peer_t *right)
{
    return left != NULL && right != NULL &&
        logical_id_equal(&left->logical_id, &right->logical_id) &&
        bytes_equal(left->adva.bytes, right->adva.bytes, TAVRN_ADVA_LEN);
}

static int link_data_valid(const tavrn_link_data_t *data)
{
    return data != NULL && logical_id_valid(&data->origin) &&
        logical_id_valid(&data->final_destination) &&
        data->origin.width == data->final_destination.width && data->ttl <= 15u &&
        data->hops <= 15u && data->urgent <= 1u &&
        data->app_len <= TAVRN_LINK_APP_BYTES &&
        (data->ownership == TAVRN_DATA_ORIGINATED ||
         data->ownership == TAVRN_DATA_TRANSIT);
}

static int link_data_equal(const tavrn_link_data_t *left,
                           const tavrn_link_data_t *right)
{
    return left != NULL && right != NULL &&
        logical_id_equal(&left->origin, &right->origin) &&
        logical_id_equal(&left->final_destination, &right->final_destination) &&
        left->data_seq == right->data_seq && left->ttl == right->ttl &&
        left->hops == right->hops && left->app_kind == right->app_kind &&
        left->app_source == right->app_source && left->urgent == right->urgent &&
        left->app_len == right->app_len &&
        bytes_equal(left->app_bytes, right->app_bytes, TAVRN_LINK_APP_BYTES) &&
        left->ownership == right->ownership;
}

static int link_custody_tuple_valid(const tavrn_link_data_t *data,
                                    const tavrn_direct_peer_t *next_hop)
{
    return link_data_valid(data) && direct_peer_valid(next_hop) &&
        next_hop->logical_id.width == data->origin.width;
}

static int owned_data_valid(const tavrn_owned_data_event_t *owned)
{
    return owned != NULL && link_custody_tuple_valid(&owned->data, &owned->next_hop) &&
        owned->data.ownership == TAVRN_DATA_TRANSIT;
}

static int owned_data_equal(const tavrn_owned_data_event_t *left,
                            const tavrn_owned_data_event_t *right)
{
    return left != NULL && right != NULL &&
        direct_peer_equal(&left->next_hop, &right->next_hop) &&
        link_data_equal(&left->data, &right->data) &&
        left->requested_channel_mask == right->requested_channel_mask &&
        left->completed_channel_mask == right->completed_channel_mask &&
        left->attempt_count == right->attempt_count &&
        left->busy_response_count == right->busy_response_count &&
        left->first_tx_ms == right->first_tx_ms &&
        left->last_tx_ms == right->last_tx_ms &&
        left->final_deadline_ms == right->final_deadline_ms &&
        left->local_reason == right->local_reason &&
        left->mesh_fault_reason == right->mesh_fault_reason;
}

static int attempt_equal(const aodv_rreq_attempt_t *left,
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

static int action_attempt_valid(const tavrn_repair_t *repair,
                                const aodv_action_t *action, uint8_t scope)
{
    const aodv_rreq_attempt_t *attempt;

    if (repair == NULL || action == NULL || action->type != AODV_ACTION_SEND_RREQ ||
        action->detail.control.rreq_attempt_present != AODV_RREQ_ATTEMPT_PRESENT) {
        return 0;
    }
    attempt = &action->detail.control.rreq_attempt;
    return attempt->discovery_correlation != 0u && logical_id_valid(&attempt->origin) &&
        logical_id_equal(&attempt->destination, &repair->destination) &&
        attempt->request_id != 0u && attempt->initial_scope == scope &&
        attempt->current_scope == scope && attempt->ring_ordinal == 0u;
}

static int operations_valid(const tavrn_repair_dependency_ops_t *operations)
{
    return operations != NULL && operations->begin_deferred_rerr != NULL &&
        operations->finish_deferred_rerr != NULL &&
        operations->create_single_rreq != NULL &&
        operations->reserve_high_token != NULL &&
        operations->release_high_token != NULL &&
        operations->schedule_held_failed_hop_verification != NULL;
}

static int config_valid(const tavrn_repair_config_t *config)
{
    uint64_t derived_timeout;

    if (config == NULL || (config->enabled != 0u && config->enabled != 1u) ||
        config->net_diameter == 0u || config->net_diameter > 15u ||
        config->data_capacity != TAVRN_REPAIR_BUFFER_CAPACITY ||
        config->path_discovery_ms == 0u ||
        config->path_discovery_ms >= TAVRN_REPAIR_HALF_RANGE ||
        config->repair_timeout_ms == 0u ||
        config->repair_timeout_ms >= TAVRN_REPAIR_HALF_RANGE ||
        config->repair_cooldown_ms == 0u ||
        config->repair_cooldown_ms >= TAVRN_REPAIR_HALF_RANGE ||
        !operations_valid(config->operations)) {
        return 0;
    }
    derived_timeout = (uint64_t)config->path_discovery_ms * 2u + 500u;
    return derived_timeout < TAVRN_REPAIR_HALF_RANGE &&
        config->repair_timeout_ms == (uint32_t)derived_timeout;
}

static int repair_ready(const tavrn_repair_t *repair)
{
    return repair != NULL && repair->initialized != 0u &&
        config_valid(&repair->config);
}

static int repair_active(const tavrn_repair_t *repair)
{
    return repair != NULL && (repair->state == TAVRN_REPAIR_STATE_WAIT_SMART_TTL_RREP ||
        repair->state == TAVRN_REPAIR_STATE_WAIT_FULL_SCOPE_RREP);
}

static int repair_observes_link_reservations(const tavrn_repair_t *repair)
{
    return repair_active(repair) ||
        (repair != NULL && repair->state == TAVRN_REPAIR_STATE_FLUSHING);
}

static uint8_t buffered_count(const tavrn_repair_t *repair)
{
    uint8_t index;
    uint8_t count = 0u;

    for (index = 0u; index < TAVRN_REPAIR_BUFFER_CAPACITY; index++) {
        if (repair->slots[index].kind == TAVRN_REPAIR_SLOT_BUFFERED) {
            count++;
        }
    }
    return count;
}

static uint8_t link_reserved_count(const tavrn_repair_t *repair)
{
    uint8_t index;
    uint8_t count = 0u;

    for (index = 0u; index < TAVRN_REPAIR_BUFFER_CAPACITY; index++) {
        if (repair->slots[index].kind == TAVRN_REPAIR_SLOT_LINK_RESERVED) {
            count++;
        }
    }
    return count;
}

static uint8_t actual_or_reserved_count(const tavrn_repair_t *repair)
{
    uint8_t index;
    uint8_t count = 0u;

    for (index = 0u; index < TAVRN_REPAIR_BUFFER_CAPACITY; index++) {
        if (repair->slots[index].kind != TAVRN_REPAIR_SLOT_EMPTY) {
            count++;
        }
    }
    return count;
}

static void clear_action(tavrn_repair_action_t *action_out)
{
    if (action_out != NULL) {
        memset(action_out, 0, sizeof(*action_out));
        action_out->type = TAVRN_REPAIR_ACTION_NONE;
        action_out->purpose = TAVRN_REPAIR_RREQ_NONE;
    }
}

static void clear_terminal(tavrn_repair_terminal_t *terminal_out)
{
    if (terminal_out != NULL) {
        memset(terminal_out, 0, sizeof(*terminal_out));
        terminal_out->kind = TAVRN_REPAIR_TERMINAL_NONE;
    }
}

static void normal_failure_result(tavrn_repair_setup_result_t *result_out)
{
    memset(result_out, 0, sizeof(*result_out));
    result_out->state = TAVRN_REPAIR_SETUP_NORMAL_FAILURE;
    result_out->disposition = TAVRN_REPAIR_RETRY_DECLINED;
    result_out->failure_mode = AODV_LINK_FAILURE_IMMEDIATE_RERR;
}

static void clear_active(tavrn_repair_t *repair)
{
    memset(&repair->active_aodv_action, 0, sizeof(repair->active_aodv_action));
    memset(&repair->active_attempt, 0, sizeof(repair->active_attempt));
    repair->active_token = 0u;
    repair->active_purpose = TAVRN_REPAIR_RREQ_NONE;
    repair->active_created = 0u;
    repair->active_enqueued = 0u;
    repair->active_tx_done = 0u;
    repair->active_tx_failed = 0u;
    repair->active_token_held = 0u;
    repair->active_release_only = 0u;
    repair->stage_response_deadline_ms = 0u;
}

static tavrn_repair_status_t high_status(tavrn_repair_high_token_status_t status)
{
    if (status == TAVRN_REPAIR_HIGH_TOKEN_OK) {
        return TAVRN_REPAIR_OK;
    }
    return status == TAVRN_REPAIR_HIGH_TOKEN_BUSY ?
        TAVRN_REPAIR_BUSY : TAVRN_REPAIR_INVALID;
}

static tavrn_repair_status_t release_active_token(tavrn_repair_t *repair)
{
    tavrn_repair_high_token_status_t status;

    if (repair->active_token_held == 0u) {
        return TAVRN_REPAIR_OK;
    }
    status = repair->config.operations->release_high_token(
        repair->config.operations->context, TAVRN_REPAIR_HIGH_TOKEN_OWNER_REPAIR,
        repair->active_purpose, repair->active_token);
    if (status == TAVRN_REPAIR_HIGH_TOKEN_OK) {
        repair->active_token_held = 0u;
        return TAVRN_REPAIR_OK;
    }
    return high_status(status);
}

static void clear_nonbuffer_slots(tavrn_repair_t *repair)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_REPAIR_BUFFER_CAPACITY; index++) {
        if (repair->slots[index].kind != TAVRN_REPAIR_SLOT_BUFFERED) {
            memset(&repair->slots[index], 0, sizeof(repair->slots[index]));
        }
    }
}

static void clear_candidate_slots(tavrn_repair_t *repair)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_REPAIR_BUFFER_CAPACITY; index++) {
        if (repair->slots[index].kind == TAVRN_REPAIR_SLOT_CANDIDATE_RESERVED) {
            memset(&repair->slots[index], 0, sizeof(repair->slots[index]));
        }
    }
}

static void reset_episode(tavrn_repair_t *repair)
{
    memset(repair->slots, 0, sizeof(repair->slots));
    memset(&repair->failed_next_hop, 0, sizeof(repair->failed_next_hop));
    memset(&repair->destination, 0, sizeof(repair->destination));
    clear_active(repair);
    repair->deadline_ms = 0u;
    repair->cooldown_deadline_ms = 0u;
    repair->smart_ttl_request_id = 0u;
    repair->full_scope_request_id = 0u;
    repair->smart_ttl_scope = 0u;
    repair->smart_ttl_emissions = 0u;
    repair->full_scope_emissions = 0u;
    repair->flush_count = 0u;
    repair->drop_count = 0u;
    repair->deferred_rerr_active = 0u;
    repair->failed_hop_verification_state = TAVRN_REPAIR_FAILED_HOP_VERIFICATION_NONE;
    repair->failure_pending = 0u;
    repair->state = TAVRN_REPAIR_STATE_IDLE;
}

static void enter_cooldown(tavrn_repair_t *repair, uint32_t now_ms)
{
    repair->state = TAVRN_REPAIR_STATE_COOLDOWN;
    repair->cooldown_deadline_ms = now_ms + repair->config.repair_cooldown_ms;
}

static void fill_terminal(const tavrn_repair_t *repair,
                          tavrn_repair_terminal_kind_t kind,
                          aodv_deferred_rerr_decision_t decision,
                          tavrn_repair_terminal_t *terminal_out)
{
    if (terminal_out == NULL) {
        return;
    }
    clear_terminal(terminal_out);
    terminal_out->kind = kind;
    terminal_out->deferred_rerr_decision = decision;
    terminal_out->failed_hop_verification_state = repair->failed_hop_verification_state;
    terminal_out->failed_next_hop = repair->failed_next_hop;
    terminal_out->destination = repair->destination;
    terminal_out->buffered_count = buffered_count(repair);
}

static tavrn_repair_status_t start_failure(tavrn_repair_t *repair,
                                           uint32_t now_ms)
{
    aodv_failure_status_t finish_status;
    tavrn_repair_status_t status;

    status = release_active_token(repair);
    if (status != TAVRN_REPAIR_OK) {
        return status;
    }
    clear_active(repair);
    if (repair->deferred_rerr_active != 0u) {
        finish_status = repair->config.operations->finish_deferred_rerr(
            repair->config.operations->context, &repair->destination,
            AODV_DEFERRED_RERR_REPAIR_FAILED, now_ms);
        if (finish_status == AODV_FAILURE_BUSY) {
            return TAVRN_REPAIR_BUSY;
        }
        if (finish_status != AODV_FAILURE_OK) {
            return TAVRN_REPAIR_INVALID;
        }
        repair->deferred_rerr_active = 0u;
    }
    clear_nonbuffer_slots(repair);
    repair->failure_pending = 0u;
    repair->state = TAVRN_REPAIR_STATE_DROPPING;
    return TAVRN_REPAIR_OK;
}

static tavrn_repair_status_t schedule_failed_hop_verification(
    tavrn_repair_t *repair, uint32_t now_ms)
{
    tavrn_repair_failed_hop_verification_status_t status;

    if (repair->failed_hop_verification_state !=
        TAVRN_REPAIR_FAILED_HOP_VERIFICATION_HELD) {
        enter_cooldown(repair, now_ms);
        return TAVRN_REPAIR_OK;
    }
    status = repair->config.operations->schedule_held_failed_hop_verification(
        repair->config.operations->context, &repair->failed_next_hop, now_ms);
    if (status == TAVRN_REPAIR_FAILED_HOP_VERIFICATION_OK) {
        repair->failed_hop_verification_state =
            TAVRN_REPAIR_FAILED_HOP_VERIFICATION_SCHEDULED;
        enter_cooldown(repair, now_ms);
        return TAVRN_REPAIR_OK;
    }
    return status == TAVRN_REPAIR_FAILED_HOP_VERIFICATION_BUSY ? TAVRN_REPAIR_BUSY :
        TAVRN_REPAIR_INVALID;
}

static uint16_t next_completion_token(tavrn_repair_t *repair)
{
    repair->next_completion_token++;
    if (repair->next_completion_token == 0u) {
        repair->next_completion_token++;
    }
    return repair->next_completion_token;
}

static int reservation_token_in_use(const tavrn_repair_t *repair, uint16_t token)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_REPAIR_BUFFER_CAPACITY; index++) {
        if (repair->slots[index].kind == TAVRN_REPAIR_SLOT_CANDIDATE_RESERVED &&
            repair->slots[index].reservation_token == token) {
            return 1;
        }
    }
    return 0;
}

static uint16_t next_reservation_token(tavrn_repair_t *repair)
{
    uint32_t attempts;

    for (attempts = 0u; attempts < UINT16_MAX; attempts++) {
        repair->next_reservation_token++;
        if (repair->next_reservation_token == TAVRN_REPAIR_RESERVATION_NONE) {
            repair->next_reservation_token++;
        }
        if (!reservation_token_in_use(repair, repair->next_reservation_token)) {
            return repair->next_reservation_token;
        }
    }
    return TAVRN_REPAIR_RESERVATION_NONE;
}

static tavrn_repair_status_t create_rreq(tavrn_repair_t *repair,
                                         tavrn_repair_rreq_purpose_t purpose,
                                         uint8_t scope, uint32_t now_ms,
                                         tavrn_repair_action_t *action_out)
{
    tavrn_repair_high_token_status_t reserve_status;
    aodv_single_rreq_status_t create_status;
    tavrn_repair_status_t release_status;
    aodv_action_t action;
    uint16_t token = 0u;

    reserve_status = repair->config.operations->reserve_high_token(
        repair->config.operations->context, TAVRN_REPAIR_HIGH_TOKEN_OWNER_REPAIR,
        purpose, &token);
    if (reserve_status != TAVRN_REPAIR_HIGH_TOKEN_OK) {
        if (reserve_status == TAVRN_REPAIR_HIGH_TOKEN_INVALID) {
            repair->failure_pending = 1u;
        }
        return high_status(reserve_status);
    }
    if (token < 0x8000u) {
        (void)repair->config.operations->release_high_token(
            repair->config.operations->context, TAVRN_REPAIR_HIGH_TOKEN_OWNER_REPAIR,
            purpose, token);
        repair->failure_pending = 1u;
        return TAVRN_REPAIR_INVALID;
    }
    memset(&action, 0, sizeof(action));
    create_status = repair->config.operations->create_single_rreq(
        repair->config.operations->context, &repair->destination, scope, now_ms,
        &action);
    if (create_status != AODV_SINGLE_RREQ_OK ||
        !action_attempt_valid(repair, &action, scope)) {
        repair->active_token = token;
        repair->active_purpose = purpose;
        repair->active_token_held = 1u;
        repair->active_release_only = 1u;
        release_status = release_active_token(repair);
        if (release_status == TAVRN_REPAIR_OK) {
            clear_active(repair);
        }
        if (create_status == AODV_SINGLE_RREQ_RATE_DEFERRED ||
            create_status == AODV_SINGLE_RREQ_BUSY) {
            return release_status == TAVRN_REPAIR_OK ? TAVRN_REPAIR_BUSY : release_status;
        }
        repair->failure_pending = 1u;
        return release_status == TAVRN_REPAIR_OK ? TAVRN_REPAIR_INVALID : release_status;
    }
    repair->active_aodv_action = action;
    repair->active_attempt = action.detail.control.rreq_attempt;
    repair->active_token = token;
    repair->active_purpose = purpose;
    repair->active_created = 1u;
    repair->active_token_held = 1u;
    if (purpose == TAVRN_REPAIR_RREQ_SMART_TTL) {
        repair->smart_ttl_emissions++;
        repair->smart_ttl_request_id = repair->active_attempt.request_id;
    } else {
        repair->full_scope_emissions++;
        repair->full_scope_request_id = repair->active_attempt.request_id;
    }
    action_out->type = TAVRN_REPAIR_ACTION_RREQ_CREATED;
    action_out->purpose = purpose;
    action_out->token_owner = TAVRN_REPAIR_HIGH_TOKEN_OWNER_REPAIR;
    action_out->aodv_action = action;
    action_out->rreq_attempt = repair->active_attempt;
    action_out->token = token;
    action_out->rreq_created = 1u;
    return TAVRN_REPAIR_OK;
}

static tavrn_repair_status_t next_data_action(tavrn_repair_t *repair,
                                              uint32_t now_ms,
                                              tavrn_repair_action_t *action_out)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_REPAIR_BUFFER_CAPACITY; index++) {
        tavrn_repair_slot_t *slot = &repair->slots[index];

        if (slot->kind != TAVRN_REPAIR_SLOT_BUFFERED ||
            slot->action_in_flight != 0u) {
            continue;
        }
        slot->action_in_flight = 1u;
        slot->completion_token = next_completion_token(repair);
        action_out->type = repair->state == TAVRN_REPAIR_STATE_FLUSHING ?
            TAVRN_REPAIR_ACTION_FLUSH_DATA : TAVRN_REPAIR_ACTION_DROP_DATA;
        action_out->owned = slot->owned;
        action_out->token = slot->completion_token;
        action_out->buffer_index = index;
        return TAVRN_REPAIR_OK;
    }
    if (repair->state == TAVRN_REPAIR_STATE_FLUSHING) {
        if (link_reserved_count(repair) == 0u) {
            enter_cooldown(repair, now_ms);
        }
        return TAVRN_REPAIR_OK;
    }
    return schedule_failed_hop_verification(repair, now_ms);
}

tavrn_repair_status_t tavrn_repair_init(
    tavrn_repair_t *repair, const tavrn_repair_config_t *config)
{
    if (repair == NULL || !config_valid(config)) {
        return TAVRN_REPAIR_INVALID;
    }
    memset(repair, 0, sizeof(*repair));
    repair->config = *config;
    repair->initialized = 1u;
    repair->state = TAVRN_REPAIR_STATE_IDLE;
    repair->failed_hop_verification_state = TAVRN_REPAIR_FAILED_HOP_VERIFICATION_NONE;
    return TAVRN_REPAIR_OK;
}

tavrn_repair_status_t tavrn_repair_retry_exhausted(
    tavrn_repair_t *repair, const tavrn_repair_retry_input_t *input,
    uint32_t now_ms, tavrn_repair_setup_result_t *result_out)
{
    tavrn_repair_slot_t staged[TAVRN_REPAIR_BUFFER_CAPACITY];
    aodv_failure_status_t begin_status;
    uint8_t index;

    if (result_out == NULL || !repair_ready(repair) || input == NULL) {
        return TAVRN_REPAIR_INVALID;
    }
    normal_failure_result(result_out);
    if (repair->config.enabled == 0u ||
        input->trigger != TAVRN_LINK_EVENT_RETRY_EXHAUSTED ||
        input->owned.data.ownership != TAVRN_DATA_TRANSIT) {
        return TAVRN_REPAIR_OK;
    }
    if (!owned_data_valid(&input->owned) ||
        input->same_destination_link_custody.count > TAVRN_REPAIR_BUFFER_CAPACITY ||
        input->smart_ttl_scope > repair->config.net_diameter) {
        result_out->state = TAVRN_REPAIR_SETUP_INVALID;
        result_out->disposition = TAVRN_REPAIR_RETRY_INVALID;
        return TAVRN_REPAIR_INVALID;
    }
    if (repair->state == TAVRN_REPAIR_STATE_COOLDOWN) {
        if (!time_due(now_ms, repair->cooldown_deadline_ms)) {
            return TAVRN_REPAIR_OK;
        }
        reset_episode(repair);
    }
    if (repair->state != TAVRN_REPAIR_STATE_IDLE ||
        input->same_destination_link_custody.count >= TAVRN_REPAIR_BUFFER_CAPACITY) {
        return TAVRN_REPAIR_OK;
    }
    memset(staged, 0, sizeof(staged));
    staged[0].kind = TAVRN_REPAIR_SLOT_BUFFERED;
    staged[0].owned = input->owned;
    for (index = 0u; index < input->same_destination_link_custody.count; index++) {
        const tavrn_repair_link_custody_record_t *record =
            &input->same_destination_link_custody.records[index];
        const tavrn_link_data_t *data = &record->data;

        if (!link_custody_tuple_valid(data, &record->next_hop) ||
            data->ownership != TAVRN_DATA_TRANSIT ||
            !logical_id_equal(&data->final_destination,
                               &input->owned.data.final_destination) ||
            link_data_equal(data, &input->owned.data)) {
            result_out->state = TAVRN_REPAIR_SETUP_INVALID;
            result_out->disposition = TAVRN_REPAIR_RETRY_INVALID;
            return TAVRN_REPAIR_INVALID;
        }
        {
            uint8_t prior;

            for (prior = 0u; prior < index; prior++) {
                if (link_data_equal(data,
                    &input->same_destination_link_custody.records[prior].data)) {
                    result_out->state = TAVRN_REPAIR_SETUP_INVALID;
                    result_out->disposition = TAVRN_REPAIR_RETRY_INVALID;
                    return TAVRN_REPAIR_INVALID;
                }
            }
        }
        staged[(uint8_t)(index + 1u)].kind = TAVRN_REPAIR_SLOT_LINK_RESERVED;
        staged[(uint8_t)(index + 1u)].owned.next_hop = record->next_hop;
        staged[(uint8_t)(index + 1u)].owned.data = *data;
    }
    begin_status = repair->config.operations->begin_deferred_rerr(
        repair->config.operations->context, &input->owned.next_hop,
        &input->owned.data.final_destination, now_ms);
    result_out->deferred_rerr_status = begin_status;
    if (begin_status != AODV_FAILURE_OK) {
        return TAVRN_REPAIR_OK;
    }
    repair->slots[0] = staged[0];
    for (index = 0u; index < input->same_destination_link_custody.count; index++) {
        repair->slots[(uint8_t)(index + 1u)] = staged[(uint8_t)(index + 1u)];
    }
    repair->failed_next_hop = input->owned.next_hop;
    repair->destination = input->owned.data.final_destination;
    repair->deadline_ms = now_ms + repair->config.repair_timeout_ms;
    repair->smart_ttl_scope = input->smart_ttl_scope;
    repair->state = input->smart_ttl_scope != 0u &&
        input->smart_ttl_scope < repair->config.net_diameter ?
        TAVRN_REPAIR_STATE_WAIT_SMART_TTL_RREP :
        TAVRN_REPAIR_STATE_WAIT_FULL_SCOPE_RREP;
    repair->deferred_rerr_active = 1u;
    repair->failed_hop_verification_state = TAVRN_REPAIR_FAILED_HOP_VERIFICATION_HELD;
    result_out->state = TAVRN_REPAIR_SETUP_ATOMIC_OWNED;
    result_out->disposition = TAVRN_REPAIR_RETRY_OWNED;
    result_out->failure_mode = AODV_LINK_FAILURE_DEFER_REPAIR_DESTINATION_RERR;
    result_out->data_copied = 1u;
    result_out->deferred_rerr_started = 1u;
    result_out->held_failed_hop_verification_created = 1u;
    result_out->buffered_count = buffered_count(repair);
    result_out->reserved_link_slots = link_reserved_count(repair);
    return TAVRN_REPAIR_OK;
}

tavrn_repair_status_t tavrn_repair_reserve_candidate(
    tavrn_repair_t *repair, const tavrn_link_data_t *data, uint32_t now_ms,
    tavrn_repair_candidate_result_t *result_out)
{
    uint8_t index;
    uint16_t token;

    (void)now_ms;
    if (result_out == NULL || !repair_ready(repair) || data == NULL) {
        return TAVRN_REPAIR_INVALID;
    }
    memset(result_out, 0, sizeof(*result_out));
    result_out->status = TAVRN_REPAIR_CANDIDATE_NOT_APPLICABLE;
    if (repair->config.enabled == 0u || !repair_active(repair) ||
        !link_data_valid(data) || data->ownership != TAVRN_DATA_TRANSIT) {
        return !link_data_valid(data) ? TAVRN_REPAIR_INVALID : TAVRN_REPAIR_OK;
    }
    if (!logical_id_equal(&data->final_destination, &repair->destination)) {
        return TAVRN_REPAIR_OK;
    }
    result_out->actual_or_reserved_count = actual_or_reserved_count(repair);
    if (result_out->actual_or_reserved_count >= TAVRN_REPAIR_BUFFER_CAPACITY) {
        result_out->status = TAVRN_REPAIR_CANDIDATE_BUSY;
        return TAVRN_REPAIR_OK;
    }
    token = next_reservation_token(repair);
    if (token == TAVRN_REPAIR_RESERVATION_NONE) {
        result_out->status = TAVRN_REPAIR_CANDIDATE_BUSY;
        return TAVRN_REPAIR_OK;
    }
    for (index = 0u; index < TAVRN_REPAIR_BUFFER_CAPACITY; index++) {
        if (repair->slots[index].kind == TAVRN_REPAIR_SLOT_EMPTY) {
            repair->slots[index].kind = TAVRN_REPAIR_SLOT_CANDIDATE_RESERVED;
            repair->slots[index].owned.data = *data;
            repair->slots[index].reservation_token = token;
            result_out->status = TAVRN_REPAIR_CANDIDATE_RESERVED;
            result_out->reservation.token = token;
            result_out->reservation.destination = repair->destination;
            result_out->reservation.valid = 1u;
            result_out->actual_or_reserved_count = actual_or_reserved_count(repair);
            return TAVRN_REPAIR_OK;
        }
    }
    result_out->status = TAVRN_REPAIR_CANDIDATE_BUSY;
    return TAVRN_REPAIR_OK;
}

tavrn_repair_status_t tavrn_repair_commit_candidate(
    tavrn_repair_t *repair, const tavrn_repair_reservation_t *reservation,
    const tavrn_link_data_t *data, uint32_t now_ms)
{
    uint8_t index;

    (void)now_ms;
    if (!repair_ready(repair) || reservation == NULL || data == NULL ||
        reservation->valid != 1u || reservation->token == TAVRN_REPAIR_RESERVATION_NONE ||
        !link_data_valid(data)) {
        return TAVRN_REPAIR_INVALID;
    }
    if (repair->config.enabled == 0u || !repair_active(repair)) {
        return TAVRN_REPAIR_IGNORED;
    }
    for (index = 0u; index < TAVRN_REPAIR_BUFFER_CAPACITY; index++) {
        tavrn_repair_slot_t *slot = &repair->slots[index];

        if (slot->kind == TAVRN_REPAIR_SLOT_CANDIDATE_RESERVED &&
            slot->reservation_token == reservation->token &&
            logical_id_equal(&reservation->destination, &repair->destination)) {
            if (!link_data_equal(&slot->owned.data, data)) {
                return TAVRN_REPAIR_IGNORED;
            }
            slot->kind = TAVRN_REPAIR_SLOT_BUFFERED;
            slot->reservation_token = TAVRN_REPAIR_RESERVATION_NONE;
            return TAVRN_REPAIR_OK;
        }
    }
    return TAVRN_REPAIR_IGNORED;
}

tavrn_repair_status_t tavrn_repair_rollback_candidate(
    tavrn_repair_t *repair, const tavrn_repair_reservation_t *reservation,
    uint32_t now_ms)
{
    uint8_t index;

    (void)now_ms;
    if (!repair_ready(repair) || reservation == NULL || reservation->valid != 1u ||
        reservation->token == TAVRN_REPAIR_RESERVATION_NONE) {
        return TAVRN_REPAIR_INVALID;
    }
    if (repair->config.enabled == 0u || !repair_active(repair)) {
        return TAVRN_REPAIR_IGNORED;
    }
    for (index = 0u; index < TAVRN_REPAIR_BUFFER_CAPACITY; index++) {
        tavrn_repair_slot_t *slot = &repair->slots[index];

        if (slot->kind == TAVRN_REPAIR_SLOT_CANDIDATE_RESERVED &&
            slot->reservation_token == reservation->token &&
            logical_id_equal(&reservation->destination, &repair->destination)) {
            memset(slot, 0, sizeof(*slot));
            return TAVRN_REPAIR_OK;
        }
    }
    return TAVRN_REPAIR_IGNORED;
}

tavrn_repair_status_t tavrn_repair_observe_link_custody(
    tavrn_repair_t *repair,
    const tavrn_repair_link_custody_terminal_input_t *input,
    tavrn_repair_link_custody_terminal_t terminal, uint32_t now_ms,
    tavrn_repair_link_custody_result_t *result_out)
{
    uint8_t index;

    if (result_out == NULL || !repair_ready(repair) || input == NULL ||
        input->owned_present > 1u ||
        !link_custody_tuple_valid(&input->data, &input->next_hop) ||
        terminal > TAVRN_REPAIR_LINK_CUSTODY_INVALID) {
        return TAVRN_REPAIR_INVALID;
    }
    if (terminal == TAVRN_REPAIR_LINK_CUSTODY_RETRY_EXHAUSTED &&
        (input->owned_present != 1u || !owned_data_valid(&input->owned) ||
          !link_data_equal(&input->data, &input->owned.data) ||
          !direct_peer_equal(&input->next_hop, &input->owned.next_hop))) {
        return TAVRN_REPAIR_INVALID;
    }
    memset(result_out, 0, sizeof(*result_out));
    if (repair->config.enabled == 0u || !repair_observes_link_reservations(repair) ||
        !logical_id_equal(&input->data.final_destination, &repair->destination)) {
        return TAVRN_REPAIR_IGNORED;
    }
    for (index = 0u; index < TAVRN_REPAIR_BUFFER_CAPACITY; index++) {
        tavrn_repair_slot_t *slot = &repair->slots[index];

        if (slot->kind != TAVRN_REPAIR_SLOT_LINK_RESERVED ||
            !link_data_equal(&slot->owned.data, &input->data) ||
            !direct_peer_equal(&slot->owned.next_hop, &input->next_hop)) {
            continue;
        }
        if (terminal == TAVRN_REPAIR_LINK_CUSTODY_TRANSFERRED ||
            terminal == TAVRN_REPAIR_LINK_CUSTODY_REJECTED) {
            memset(slot, 0, sizeof(*slot));
            result_out->released_reservation = 1u;
        } else if (terminal == TAVRN_REPAIR_LINK_CUSTODY_RETRY_EXHAUSTED) {
            slot->owned = input->owned;
            slot->kind = TAVRN_REPAIR_SLOT_BUFFERED;
            result_out->converted_to_buffer = 1u;
        } else {
            return TAVRN_REPAIR_IGNORED;
        }
        result_out->buffered_count = buffered_count(repair);
        result_out->reserved_link_slots = link_reserved_count(repair);
        if (repair->state == TAVRN_REPAIR_STATE_FLUSHING &&
            result_out->buffered_count == 0u &&
            result_out->reserved_link_slots == 0u) {
            enter_cooldown(repair, now_ms);
        }
        return TAVRN_REPAIR_OK;
    }
    return TAVRN_REPAIR_IGNORED;
}

tavrn_repair_status_t tavrn_repair_reconcile_link_custody(
    tavrn_repair_t *repair,
    const tavrn_repair_link_custody_snapshot_t *current_link_custody,
    uint32_t now_ms,
    tavrn_repair_link_custody_reconcile_result_t *result_out)
{
    uint8_t index;

    if (result_out != NULL) {
        memset(result_out, 0, sizeof(*result_out));
    }
    if (!repair_ready(repair) || current_link_custody == NULL || result_out == NULL ||
        current_link_custody->count > TAVRN_REPAIR_BUFFER_CAPACITY) {
        return TAVRN_REPAIR_INVALID;
    }
    if (repair->config.enabled == 0u) {
        return TAVRN_REPAIR_IGNORED;
    }
    for (index = 0u; index < current_link_custody->count; index++) {
        const tavrn_repair_link_custody_record_t *record =
            &current_link_custody->records[index];

        if (!link_custody_tuple_valid(&record->data, &record->next_hop) ||
            !logical_id_equal(&record->data.final_destination,
                              &repair->destination)) {
            return TAVRN_REPAIR_INVALID;
        }
    }
    if (!repair_observes_link_reservations(repair)) {
        return TAVRN_REPAIR_IGNORED;
    }
    for (index = 0u; index < TAVRN_REPAIR_BUFFER_CAPACITY; index++) {
        tavrn_repair_slot_t *slot = &repair->slots[index];
        uint8_t current_index;
        uint8_t found = 0u;

        if (slot->kind != TAVRN_REPAIR_SLOT_LINK_RESERVED) {
            continue;
        }
        for (current_index = 0u;
             current_index < current_link_custody->count; current_index++) {
            const tavrn_repair_link_custody_record_t *record =
                &current_link_custody->records[current_index];

            if (link_data_equal(&slot->owned.data, &record->data) &&
                direct_peer_equal(&slot->owned.next_hop, &record->next_hop)) {
                found = 1u;
                break;
            }
        }
        if (found == 0u) {
            memset(slot, 0, sizeof(*slot));
            result_out->released_count++;
        }
    }
    result_out->buffered_count = buffered_count(repair);
    result_out->reserved_link_slots = link_reserved_count(repair);
    if (repair->state == TAVRN_REPAIR_STATE_FLUSHING &&
        result_out->buffered_count == 0u && result_out->reserved_link_slots == 0u) {
        enter_cooldown(repair, now_ms);
    }
    return TAVRN_REPAIR_OK;
}

tavrn_repair_status_t tavrn_repair_owner_tick(
    tavrn_repair_t *repair, uint32_t now_ms, tavrn_repair_action_t *action_out)
{
    tavrn_repair_status_t status;

    if (!repair_ready(repair) || action_out == NULL) {
        return TAVRN_REPAIR_INVALID;
    }
    clear_action(action_out);
    if (repair->config.enabled == 0u) {
        return TAVRN_REPAIR_IGNORED;
    }
    if (repair->state == TAVRN_REPAIR_STATE_IDLE) {
        return TAVRN_REPAIR_OK;
    }
    if (repair->state == TAVRN_REPAIR_STATE_COOLDOWN) {
        if (time_due(now_ms, repair->cooldown_deadline_ms)) {
            reset_episode(repair);
        }
        return TAVRN_REPAIR_OK;
    }
    if (repair->state == TAVRN_REPAIR_STATE_FLUSHING ||
        repair->state == TAVRN_REPAIR_STATE_DROPPING) {
        if (repair->active_token_held != 0u) {
            status = release_active_token(repair);
            if (status != TAVRN_REPAIR_OK) {
                return status;
            }
            clear_active(repair);
        }
        return next_data_action(repair, now_ms, action_out);
    }
    if (repair->failure_pending != 0u || time_due(now_ms, repair->deadline_ms)) {
        status = start_failure(repair, now_ms);
        if (status != TAVRN_REPAIR_OK) {
            return status;
        }
        return next_data_action(repair, now_ms, action_out);
    }
    if (repair->active_release_only != 0u) {
        status = release_active_token(repair);
        if (status != TAVRN_REPAIR_OK) {
            return status;
        }
        clear_active(repair);
        if (repair->failure_pending != 0u) {
            return TAVRN_REPAIR_INVALID;
        }
    }
    if (repair->active_created != 0u) {
        if (repair->active_tx_failed != 0u) {
            status = release_active_token(repair);
            if (status != TAVRN_REPAIR_OK) {
                return status;
            }
            clear_active(repair);
            if (repair->state == TAVRN_REPAIR_STATE_WAIT_SMART_TTL_RREP) {
                repair->state = TAVRN_REPAIR_STATE_WAIT_FULL_SCOPE_RREP;
            } else {
                status = start_failure(repair, now_ms);
                return status == TAVRN_REPAIR_OK ?
                    next_data_action(repair, now_ms, action_out) : status;
            }
        } else if (repair->active_tx_done != 0u &&
                   time_due(now_ms, repair->stage_response_deadline_ms)) {
            status = release_active_token(repair);
            if (status != TAVRN_REPAIR_OK) {
                return status;
            }
            clear_active(repair);
            if (repair->state == TAVRN_REPAIR_STATE_WAIT_SMART_TTL_RREP) {
                repair->state = TAVRN_REPAIR_STATE_WAIT_FULL_SCOPE_RREP;
            } else {
                status = start_failure(repair, now_ms);
                return status == TAVRN_REPAIR_OK ?
                    next_data_action(repair, now_ms, action_out) : status;
            }
        } else {
            if (repair->active_enqueued == 0u) {
                action_out->type = TAVRN_REPAIR_ACTION_RREQ_CREATED;
                action_out->purpose = repair->active_purpose;
                action_out->token_owner = TAVRN_REPAIR_HIGH_TOKEN_OWNER_REPAIR;
                action_out->aodv_action = repair->active_aodv_action;
                action_out->rreq_attempt = repair->active_attempt;
                action_out->token = repair->active_token;
                action_out->rreq_created = 1u;
            }
            return TAVRN_REPAIR_OK;
        }
    }
    if (repair->state == TAVRN_REPAIR_STATE_WAIT_SMART_TTL_RREP) {
        status = create_rreq(repair, TAVRN_REPAIR_RREQ_SMART_TTL,
                             repair->smart_ttl_scope, now_ms, action_out);
    } else {
        status = create_rreq(repair, TAVRN_REPAIR_RREQ_FULL_SCOPE,
                             repair->config.net_diameter, now_ms, action_out);
    }
    if (status == TAVRN_REPAIR_INVALID && repair->failure_pending != 0u) {
        tavrn_repair_status_t failure_status = start_failure(repair, now_ms);

        clear_action(action_out);
        return failure_status == TAVRN_REPAIR_OK ? TAVRN_REPAIR_INVALID : failure_status;
    }
    return status;
}

static int active_tuple_matches(const tavrn_repair_t *repair,
                                tavrn_repair_rreq_purpose_t purpose,
                                tavrn_repair_high_token_owner_t owner,
                                const aodv_rreq_attempt_t *attempt,
                                uint16_t token)
{
    return repair->active_created != 0u &&
        purpose == repair->active_purpose &&
        owner == TAVRN_REPAIR_HIGH_TOKEN_OWNER_REPAIR &&
        token == repair->active_token && attempt_equal(attempt, &repair->active_attempt);
}

tavrn_repair_status_t tavrn_repair_rreq_enqueue(
    tavrn_repair_t *repair, const tavrn_repair_rreq_enqueue_t *enqueue,
    uint32_t now_ms)
{
    (void)now_ms;
    if (!repair_ready(repair) || enqueue == NULL ||
        enqueue->status > TAVRN_REPAIR_RREQ_ENQUEUE_INVALID) {
        return TAVRN_REPAIR_INVALID;
    }
    if (repair->config.enabled == 0u) {
        return TAVRN_REPAIR_IGNORED;
    }
    if (!repair_active(repair) || !active_tuple_matches(
            repair, enqueue->purpose, enqueue->token_owner,
            &enqueue->rreq_attempt, enqueue->token)) {
        return TAVRN_REPAIR_IGNORED;
    }
    if (enqueue->status == TAVRN_REPAIR_RREQ_ENQUEUE_ADMITTED) {
        if (repair->active_enqueued != 0u) {
            return TAVRN_REPAIR_IGNORED;
        }
        repair->active_enqueued = 1u;
        return TAVRN_REPAIR_OK;
    }
    if (enqueue->status == TAVRN_REPAIR_RREQ_ENQUEUE_BUSY) {
        return TAVRN_REPAIR_BUSY;
    }
    /* The exact created RREQ never entered the scheduler.  It is terminal for
     * this scope, but a Smart-TTL terminal may still advance once to full. */
    repair->active_tx_failed = 1u;
    {
        tavrn_repair_status_t release_status = release_active_token(repair);

        if (release_status != TAVRN_REPAIR_OK) {
            return release_status;
        }
    }
    return enqueue->status == TAVRN_REPAIR_RREQ_ENQUEUE_INVALID ?
        TAVRN_REPAIR_INVALID : TAVRN_REPAIR_DECLINED;
}

tavrn_repair_status_t tavrn_repair_data_action_not_admitted(
    tavrn_repair_t *repair, const tavrn_repair_action_t *action,
    uint32_t now_ms)
{
    tavrn_repair_slot_t *slot;

    (void)now_ms;
    if (!repair_ready(repair) || action == NULL || action->token == 0u ||
        action->buffer_index >= TAVRN_REPAIR_BUFFER_CAPACITY ||
        (action->type != TAVRN_REPAIR_ACTION_FLUSH_DATA &&
         action->type != TAVRN_REPAIR_ACTION_DROP_DATA)) {
        return TAVRN_REPAIR_INVALID;
    }
    if ((repair->state != TAVRN_REPAIR_STATE_FLUSHING &&
         repair->state != TAVRN_REPAIR_STATE_DROPPING) ||
        (repair->state == TAVRN_REPAIR_STATE_FLUSHING &&
         action->type != TAVRN_REPAIR_ACTION_FLUSH_DATA) ||
        (repair->state == TAVRN_REPAIR_STATE_DROPPING &&
         action->type != TAVRN_REPAIR_ACTION_DROP_DATA)) {
        return TAVRN_REPAIR_IGNORED;
    }
    slot = &repair->slots[action->buffer_index];
    if (slot->kind != TAVRN_REPAIR_SLOT_BUFFERED || slot->action_in_flight == 0u ||
        slot->completion_token != action->token ||
        !owned_data_equal(&slot->owned, &action->owned)) {
        return TAVRN_REPAIR_IGNORED;
    }
    slot->action_in_flight = 0u;
    slot->completion_token = 0u;
    return TAVRN_REPAIR_OK;
}

tavrn_repair_status_t tavrn_repair_complete(
    tavrn_repair_t *repair, const tavrn_repair_completion_t *completion,
    uint32_t now_ms, tavrn_repair_terminal_t *terminal_out)
{
    tavrn_repair_slot_t *slot;
    tavrn_repair_status_t status;

    clear_terminal(terminal_out);
    if (!repair_ready(repair) || completion == NULL ||
        completion->kind > TAVRN_REPAIR_COMPLETION_DATA_DROPPED) {
        return TAVRN_REPAIR_INVALID;
    }
    if (repair->config.enabled == 0u) {
        return TAVRN_REPAIR_IGNORED;
    }
    if (completion->kind == TAVRN_REPAIR_COMPLETION_RREQ_TX_DONE ||
        completion->kind == TAVRN_REPAIR_COMPLETION_RREQ_TX_FAILED) {
        if (!repair_active(repair) ||
            (completion->kind == TAVRN_REPAIR_COMPLETION_RREQ_TX_DONE &&
             repair->active_enqueued == 0u) ||
            !active_tuple_matches(repair, completion->purpose,
                                  completion->token_owner,
                                  &completion->rreq_attempt, completion->token) ||
            repair->active_tx_done != 0u || repair->active_tx_failed != 0u) {
            return TAVRN_REPAIR_IGNORED;
        }
        if (completion->kind == TAVRN_REPAIR_COMPLETION_RREQ_TX_DONE) {
            if (completion->completed_channel_mask == 0u) {
                return TAVRN_REPAIR_INVALID;
            }
            repair->active_tx_done = 1u;
            repair->stage_response_deadline_ms =
                now_ms + repair->config.path_discovery_ms;
            return TAVRN_REPAIR_OK;
        }
        repair->active_tx_failed = 1u;
        return release_active_token(repair);
    }
    if ((repair->state != TAVRN_REPAIR_STATE_FLUSHING &&
         repair->state != TAVRN_REPAIR_STATE_DROPPING) ||
        completion->buffer_index >= TAVRN_REPAIR_BUFFER_CAPACITY) {
        return TAVRN_REPAIR_IGNORED;
    }
    slot = &repair->slots[completion->buffer_index];
    if (slot->kind != TAVRN_REPAIR_SLOT_BUFFERED || slot->action_in_flight == 0u ||
        slot->completion_token != completion->token ||
        (repair->state == TAVRN_REPAIR_STATE_FLUSHING &&
         completion->kind != TAVRN_REPAIR_COMPLETION_DATA_FLUSHED &&
         completion->kind != TAVRN_REPAIR_COMPLETION_DATA_DROPPED) ||
        (repair->state == TAVRN_REPAIR_STATE_DROPPING &&
         completion->kind != TAVRN_REPAIR_COMPLETION_DATA_DROPPED)) {
        return TAVRN_REPAIR_IGNORED;
    }
    if (completion->kind == TAVRN_REPAIR_COMPLETION_DATA_FLUSHED) {
        repair->flush_count++;
    } else {
        repair->drop_count++;
    }
    memset(slot, 0, sizeof(*slot));
    if (buffered_count(repair) != 0u) {
        return TAVRN_REPAIR_OK;
    }
    if (repair->state == TAVRN_REPAIR_STATE_FLUSHING) {
        if (link_reserved_count(repair) == 0u) {
            enter_cooldown(repair, now_ms);
        }
        return TAVRN_REPAIR_OK;
    }
    status = schedule_failed_hop_verification(repair, now_ms);
    fill_terminal(repair, TAVRN_REPAIR_TERMINAL_FAILED,
                  AODV_DEFERRED_RERR_REPAIR_FAILED, terminal_out);
    return status;
}

tavrn_repair_status_t tavrn_repair_receive_rrep(
    tavrn_repair_t *repair, const tavrn_repair_rrep_completion_t *completion,
    uint32_t now_ms, tavrn_repair_terminal_t *terminal_out)
{
    aodv_failure_status_t finish_status;
    tavrn_repair_status_t status;

    clear_terminal(terminal_out);
    if (!repair_ready(repair) || completion == NULL) {
        return TAVRN_REPAIR_INVALID;
    }
    if (repair->config.enabled == 0u) {
        return TAVRN_REPAIR_IGNORED;
    }
    if (!repair_active(repair) || time_due(now_ms, repair->deadline_ms) ||
        (repair->active_tx_done != 0u &&
         time_due(now_ms, repair->stage_response_deadline_ms)) ||
        completion->router_admitted != 1u || completion->route_installed != 1u ||
        !direct_peer_valid(&completion->transmitter) ||
        !direct_peer_valid(&completion->installed_next_hop) ||
        !direct_peer_equal(&completion->transmitter,
                           &completion->installed_next_hop) ||
        direct_peer_equal(&completion->installed_next_hop,
                          &repair->failed_next_hop) ||
        repair->active_tx_done == 0u || !active_tuple_matches(
            repair, completion->purpose, completion->token_owner,
            &completion->rreq_attempt, completion->token)) {
        return TAVRN_REPAIR_IGNORED;
    }
    finish_status = repair->config.operations->finish_deferred_rerr(
        repair->config.operations->context, &repair->destination,
        AODV_DEFERRED_RERR_ROUTE_REPAIRED, now_ms);
    if (finish_status == AODV_FAILURE_BUSY) {
        return TAVRN_REPAIR_BUSY;
    }
    if (finish_status != AODV_FAILURE_OK) {
        return TAVRN_REPAIR_INVALID;
    }
    repair->deferred_rerr_active = 0u;
    repair->failed_hop_verification_state =
        TAVRN_REPAIR_FAILED_HOP_VERIFICATION_CANCELED;
    clear_candidate_slots(repair);
    repair->state = TAVRN_REPAIR_STATE_FLUSHING;
    status = release_active_token(repair);
    if (status == TAVRN_REPAIR_OK) {
        clear_active(repair);
    }
    if (buffered_count(repair) == 0u && link_reserved_count(repair) == 0u &&
        status == TAVRN_REPAIR_OK) {
        enter_cooldown(repair, now_ms);
    }
    fill_terminal(repair, TAVRN_REPAIR_TERMINAL_REPAIRED,
                  AODV_DEFERRED_RERR_ROUTE_REPAIRED, terminal_out);
    return status;
}

tavrn_repair_status_t tavrn_repair_snapshot(
    const tavrn_repair_t *repair, tavrn_repair_snapshot_t *snapshot_out)
{
    if (!repair_ready(repair) || snapshot_out == NULL) {
        return TAVRN_REPAIR_INVALID;
    }
    memset(snapshot_out, 0, sizeof(*snapshot_out));
    snapshot_out->state = repair->state;
    snapshot_out->failed_next_hop = repair->failed_next_hop;
    snapshot_out->destination = repair->destination;
    snapshot_out->deadline_ms = repair->deadline_ms;
    snapshot_out->cooldown_deadline_ms = repair->cooldown_deadline_ms;
    snapshot_out->smart_ttl_request_id = repair->smart_ttl_request_id;
    snapshot_out->full_scope_request_id = repair->full_scope_request_id;
    snapshot_out->active_token = repair->active_token_held != 0u ?
        repair->active_token : 0u;
    snapshot_out->buffered_count = buffered_count(repair);
    snapshot_out->reserved_link_slots = link_reserved_count(repair);
    snapshot_out->actual_or_reserved_count = actual_or_reserved_count(repair);
    snapshot_out->smart_ttl_emissions = repair->smart_ttl_emissions;
    snapshot_out->full_scope_emissions = repair->full_scope_emissions;
    snapshot_out->inner_retry_count = 0u;
    snapshot_out->flush_count = repair->flush_count;
    snapshot_out->drop_count = repair->drop_count;
    snapshot_out->deferred_rerr_active = repair->deferred_rerr_active;
    snapshot_out->failed_hop_verification_state = repair->failed_hop_verification_state;
    return TAVRN_REPAIR_OK;
}

tavrn_repair_status_t tavrn_repair_active_rreq_snapshot(
    const tavrn_repair_t *repair,
    tavrn_repair_active_rreq_snapshot_t *snapshot_out)
{
    if (!repair_ready(repair) || snapshot_out == NULL) {
        return TAVRN_REPAIR_INVALID;
    }
    memset(snapshot_out, 0, sizeof(*snapshot_out));
    snapshot_out->state = repair->state;
    snapshot_out->failed_next_hop = repair->failed_next_hop;
    snapshot_out->destination = repair->destination;
    snapshot_out->total_deadline_ms = repair->deadline_ms;
    snapshot_out->stage_response_deadline_ms = repair->stage_response_deadline_ms;
    if (!repair_active(repair) || repair->active_created == 0u) {
        return TAVRN_REPAIR_IGNORED;
    }
    snapshot_out->purpose = repair->active_purpose;
    snapshot_out->token_owner = TAVRN_REPAIR_HIGH_TOKEN_OWNER_REPAIR;
    snapshot_out->attempt = repair->active_attempt;
    snapshot_out->token = repair->active_token;
    snapshot_out->active = 1u;
    snapshot_out->tx_done = repair->active_tx_done;
    return TAVRN_REPAIR_OK;
}
