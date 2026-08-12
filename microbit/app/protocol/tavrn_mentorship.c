#include "tavrn_mentorship.h"

#include "tavrn_maintenance.h"

#include <string.h>

static int time_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static int adva_equal(const tavrn_adva_t *left, const tavrn_adva_t *right)
{
    return left != NULL && right != NULL &&
        memcmp(left->bytes, right->bytes, TAVRN_ADVA_LEN) == 0;
}

static int adva_bytes_equal(const tavrn_adva_t *left,
                            const uint8_t right[TAVRN_ADVA_LEN])
{
    return left != NULL && right != NULL &&
        memcmp(left->bytes, right, TAVRN_ADVA_LEN) == 0;
}

static tavrn_adva_t adva_from_bytes(const uint8_t bytes[TAVRN_ADVA_LEN])
{
    tavrn_adva_t identity;

    memset(&identity, 0, sizeof(identity));
    if (bytes != NULL) {
        memcpy(identity.bytes, bytes, TAVRN_ADVA_LEN);
    }
    return identity;
}

static uint16_t pdu_u16(const tavrn_validated_control_t *control, uint8_t offset)
{
    return (uint16_t)control->pdu[offset] |
        ((uint16_t)control->pdu[(uint8_t)(offset + 1u)] << 8);
}

static void pdu_put_u16(tavrn_validated_control_t *control, uint8_t offset,
                        uint16_t value)
{
    control->pdu[offset] = (uint8_t)value;
    control->pdu[(uint8_t)(offset + 1u)] = (uint8_t)(value >> 8);
}

static int config_is_valid(const tavrn_mentorship_config_t *config)
{
    return config != NULL && config->offer_window_ms != 0u &&
        config->offer_window_ms < 0x80000000u && config->page_timeout_ms != 0u &&
        config->page_timeout_ms < 0x80000000u && config->self_bootstrap_ms != 0u &&
        config->self_bootstrap_ms < 0x80000000u &&
        config->offer_suppression_ms != 0u &&
        config->offer_suppression_ms < 0x80000000u &&
        config->join_dedupe_ms != 0u &&
        config->join_dedupe_ms < 0x80000000u &&
        config->sync_dedupe_ms != 0u &&
        config->sync_dedupe_ms < 0x80000000u &&
        config->rssi_weak_magnitude_db > config->rssi_strong_magnitude_db &&
        config->rssi_weak_delay_ms != 0u && config->rssi_strong_delay_ms != 0u &&
        config->rssi_weak_delay_ms < 0x80000000u &&
        config->rssi_strong_delay_ms < 0x80000000u &&
        config->jitter_min_ms <= config->jitter_max_ms &&
        config->jitter_max_ms < 0x80000000u && config->page_attempts != 0u;
}

static int tombstone_is_purged(const tavrn_gtt_entry_t *entry, uint32_t now_ms)
{
    return entry->occupied != 0u && entry->departed != 0u &&
        time_due(now_ms, entry->departed_deadline_ms);
}

static int record_is_better_offer(const tavrn_mentorship_offer_t *candidate,
                                  const tavrn_mentorship_offer_t *current)
{
    int identity_order;

    if (candidate->snapshot_count != current->snapshot_count) {
        return candidate->snapshot_count > current->snapshot_count;
    }
    if (candidate->rssi_magnitude_db != current->rssi_magnitude_db) {
        return candidate->rssi_magnitude_db < current->rssi_magnitude_db;
    }
    identity_order = memcmp(candidate->mentor.bytes, current->mentor.bytes,
                            TAVRN_ADVA_LEN);
    return identity_order < 0;
}

static int offer_equal(const tavrn_mentorship_offer_t *left,
                       const tavrn_mentorship_offer_t *right)
{
    return left != NULL && right != NULL &&
        adva_equal(&left->mentor, &right->mentor) &&
        adva_equal(&left->mentee, &right->mentee) &&
        left->snapshot_id == right->snapshot_id &&
        left->boot_nonce == right->boot_nonce;
}

static uint16_t local_boot_nonce(const tavrn_mentorship_t *mentorship);

static int offer_is_valid(const tavrn_mentorship_t *mentorship,
                          const tavrn_mentorship_offer_t *offer)
{
    return mentorship != NULL && offer != NULL && offer->boot_nonce != 0u &&
        offer->snapshot_count <= TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY &&
        offer->boot_nonce == local_boot_nonce(mentorship) &&
        adva_equal(&offer->mentee, &mentorship->gtt->config.local_identity);
}

static void begin_control(tavrn_validated_control_t *control,
                          tavrn_wire_type_t type, uint8_t length)
{
    memset(control, 0, sizeof(*control));
    control->type = type;
    control->pdu_len = length;
    control->pdu[0] = 0x54u;
    control->pdu[1] = 0x52u;
    control->pdu[2] = 0x02u;
    /* The public by-value builder has no router/network argument.  It emits
     * the frozen wire-vector network; a live sender stamps its configured
     * network before link admission. */
    control->pdu[3] = 0x2au;
    control->pdu[4] = (uint8_t)type;
}

static int gtt_observe_record_in(tavrn_gtt_t *gtt,
                                 const tavrn_mentorship_record_t *record,
                                 uint32_t now_ms)
{
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_observe_status_t status;

    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = record->identity;
    evidence.serial = record->serial;
    evidence.serial_present = record->serial_present;
    evidence.hop_count = record->hop_count;
    evidence.kind = record->departed != 0u ? TAVRN_GTT_EVIDENCE_DEPARTED :
                                             TAVRN_GTT_EVIDENCE_LIVENESS;
    status = tavrn_gtt_observe(gtt, &evidence, now_ms);
    return status == TAVRN_GTT_OBSERVE_ADDED ||
        status == TAVRN_GTT_OBSERVE_REFRESHED ||
        status == TAVRN_GTT_OBSERVE_DEPARTED ||
        status == TAVRN_GTT_OBSERVE_SELF_IGNORED ||
        status == TAVRN_GTT_OBSERVE_STALE;
}

static int gtt_observe_record(tavrn_mentorship_t *mentorship,
                              const tavrn_mentorship_record_t *record,
                              uint32_t now_ms)
{
    return mentorship != NULL &&
        gtt_observe_record_in(mentorship->gtt, record, now_ms);
}

static tavrn_esc_context_status_t resolve_sid8(const tavrn_mentorship_t *mentorship,
                                               uint8_t sid8, uint32_t now_ms,
                                               tavrn_esc_context_match_t *match_out)
{
    if (mentorship == NULL || mentorship->gtt == NULL) {
        return TAVRN_ESC_CONTEXT_INVALID;
    }
    return tavrn_esc_resolve_sid8(mentorship->gtt, sid8, now_ms, match_out);
}

static uint16_t local_boot_nonce(const tavrn_mentorship_t *mentorship)
{
    return mentorship->router->incarnation.snapshot.boot_nonce;
}

static void stamp_live_network(tavrn_mentorship_t *mentorship,
                               tavrn_validated_control_t *control)
{
    control->pdu[3] = mentorship->router->link->config.network_id;
}

static void clear_receiving_staging(tavrn_mentorship_t *mentorship)
{
    memset(mentorship->receiving_records, 0,
           sizeof(mentorship->receiving_records));
    mentorship->receiving_page_bitmap = 0u;
}

static void purge_completed_sync_dedupe(tavrn_mentorship_t *mentorship,
                                        uint32_t now_ms)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_MENTORSHIP_SYNC_DEDUPE_CAPACITY; index++) {
        tavrn_mentorship_sync_dedupe_t *completed = &mentorship->completed_sync[index];

        if (completed->valid != 0u && time_due(now_ms, completed->expires_at_ms)) {
            memset(completed, 0, sizeof(*completed));
        }
    }
}

static int completed_sync_page_seen(tavrn_mentorship_t *mentorship,
                                    const tavrn_adva_t *mentor,
                                    uint8_t page_index, uint32_t now_ms)
{
    uint8_t index;

    purge_completed_sync_dedupe(mentorship, now_ms);
    for (index = 0u; index < TAVRN_MENTORSHIP_SYNC_DEDUPE_CAPACITY; index++) {
        const tavrn_mentorship_sync_dedupe_t *completed =
            &mentorship->completed_sync[index];

        if (completed->valid != 0u && adva_equal(&completed->mentor, mentor) &&
            adva_equal(&completed->mentee, &mentorship->gtt->config.local_identity) &&
            completed->snapshot_id == mentorship->active_page_snapshot_id &&
            (completed->page_bitmap & ((uint16_t)1u << page_index)) != 0u) {
            return 1;
        }
    }
    return 0;
}

static tavrn_mentorship_sync_dedupe_t *completed_sync_admission_slot(
    tavrn_mentorship_t *mentorship, uint32_t now_ms)
{
    uint8_t index;

    purge_completed_sync_dedupe(mentorship, now_ms);
    for (index = 0u; index < TAVRN_MENTORSHIP_SYNC_DEDUPE_CAPACITY; index++) {
        if (mentorship->completed_sync[index].valid == 0u) {
            return &mentorship->completed_sync[index];
        }
    }
    return NULL;
}

static void remember_completed_sync(tavrn_mentorship_sync_dedupe_t *completed,
                                    tavrn_mentorship_t *mentorship,
                                    uint16_t page_bitmap, uint32_t now_ms)
{
    memset(completed, 0, sizeof(*completed));
    completed->mentor = mentorship->state.selected_mentor;
    completed->mentee = mentorship->gtt->config.local_identity;
    completed->snapshot_id = mentorship->active_page_snapshot_id;
    completed->page_bitmap = page_bitmap;
    completed->expires_at_ms = now_ms + mentorship->config.sync_dedupe_ms;
    completed->valid = 1u;
}

static void clear_receiving_session(tavrn_mentorship_t *mentorship)
{
    memset(mentorship->offers, 0, sizeof(mentorship->offers));
    mentorship->state.selected_mentor_present = 0u;
    mentorship->state.active_offer_count = 0u;
    mentorship->state.sync_pages_committed = 0u;
    mentorship->state.sync_next_index = 0u;
    mentorship->state.pull_attempts = 0u;
    mentorship->page_deadline_ms = 0u;
    mentorship->page_deadline_valid = 0u;
    mentorship->offer_window_deadline_ms = 0u;
    clear_receiving_staging(mentorship);
    mentorship->page_session_valid = 0u;
    mentorship->sync_complete_authorized = 0u;
}

/* A mentor has exactly one fixed-capacity serving slot.  Clear every field
 * owned by that slot together so a later N=1 cannot inherit an OFFER/DATA or
 * snapshot from the previous mentee.  Suppression is overhearing state, not
 * serving-session state, and intentionally survives this reset. */
static void clear_serving_session(tavrn_mentorship_t *mentorship)
{
    if (mentorship == NULL) {
        return;
    }
    memset(&mentorship->serving_snapshot, 0, sizeof(mentorship->serving_snapshot));
    memset(&mentorship->serving_mentee, 0, sizeof(mentorship->serving_mentee));
    memset(&mentorship->pending_offer, 0, sizeof(mentorship->pending_offer));
    mentorship->serving_deadline_ms = 0u;
    mentorship->serving_boot_nonce = 0u;
    mentorship->pending_offer_due_ms = 0u;
    mentorship->serving_session_valid = 0u;
    mentorship->pending_offer_valid = 0u;
    if (mentorship->pending_control.valid != 0u &&
        (mentorship->pending_control.purpose == TAVRN_MENTORSHIP_PENDING_OFFER ||
         mentorship->pending_control.purpose == TAVRN_MENTORSHIP_PENDING_DATA)) {
        memset(&mentorship->pending_control, 0, sizeof(mentorship->pending_control));
    }
}

static tavrn_mentorship_status_t enter_identity_conflict(
    tavrn_mentorship_t *mentorship, uint32_t now_ms)
{
    tavrn_direct_peer_t local_peer;

    if (mentorship == NULL || mentorship->router == NULL || mentorship->gtt == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    memset(&local_peer, 0, sizeof(local_peer));
    local_peer.adva = mentorship->gtt->config.local_identity;
    local_peer.logical_id.width = TAVRN_IDENTITY_SID16;
    local_peer.logical_id.value = (uint16_t)local_peer.adva.bytes[0] |
        ((uint16_t)local_peer.adva.bytes[1] << 8);
    if (tavrn_router_reconfigure_identity(mentorship->router, &local_peer, now_ms) !=
        TAVRN_ROUTER_INCARNATION_OK) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    clear_receiving_session(mentorship);
    clear_serving_session(mentorship);
    memset(&mentorship->pending_control, 0, sizeof(mentorship->pending_control));
    memset(mentorship->join_obligations, 0, sizeof(mentorship->join_obligations));
    mentorship->state.state = TAVRN_MENTORSHIP_IDENTITY_CONFLICT;
    mentorship->state.active_width = TAVRN_IDENTITY_SID16;
    mentorship->state.ordinary_traffic_gated = 1u;
    mentorship->state.full_bootstrap_admission_enabled = 1u;
    mentorship->started_at_ms = now_ms;
    mentorship->identity_conflict_pending = 0u;
    mentorship->join_reannounce_deadline_ms = 0u;
    mentorship->join_reannounce_valid = 0u;
    mentorship->counters.transition_cleared++;
    return TAVRN_MENTORSHIP_COLLISION;
}

static int sid8_identity_conflict(void *context, const tavrn_logical_id_t *logical_id,
                                  const tavrn_adva_t *direct_adva_or_null)
{
    tavrn_mentorship_t *mentorship = context;
    tavrn_esc_context_match_t match;
    tavrn_esc_context_status_t status;
    uint32_t now_ms;

    if (mentorship == NULL || logical_id == NULL) {
        return 1;
    }
    if (logical_id->width != TAVRN_IDENTITY_SID8) {
        return 0;
    }
    now_ms = mentorship->last_now_valid != 0u ? mentorship->last_now_ms : 0u;
    status = resolve_sid8(mentorship, (uint8_t)logical_id->value, now_ms, &match);
    if (status == TAVRN_ESC_CONTEXT_UNIQUE) {
        if (direct_adva_or_null != NULL &&
            !adva_equal(&match.identity, direct_adva_or_null)) {
            mentorship->counters.sid8_collision_drop++;
            mentorship->identity_conflict_pending = 1u;
            return 1;
        }
        return 0;
    }
    if (status == TAVRN_ESC_CONTEXT_RESERVED) {
        mentorship->counters.sid8_reserved_drop++;
    } else if (status == TAVRN_ESC_CONTEXT_COLLIDING) {
        mentorship->counters.sid8_collision_drop++;
        mentorship->identity_conflict_pending = 1u;
    } else {
        mentorship->counters.sid8_unknown_drop++;
    }
    return 1;
}

static tavrn_mentorship_status_t build_join_for_local(
    tavrn_mentorship_t *mentorship, uint16_t sequence,
    tavrn_validated_control_t *control_out)
{
    const tavrn_adva_t *local;

    if (mentorship == NULL || mentorship->gtt == NULL || control_out == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    local = &mentorship->gtt->config.local_identity;
    begin_control(control_out, TAVRN_WIRE_TC_UPDATE, 24u);
    control_out->pdu[3] = mentorship->router->link->config.network_id;
    control_out->pdu[5] = 0u;
    control_out->pdu[6] = 0xf0u;
    memcpy(&control_out->pdu[7], local->bytes, TAVRN_ADVA_LEN);
    pdu_put_u16(control_out, 13u, sequence);
    memcpy(&control_out->pdu[15], local->bytes, TAVRN_ADVA_LEN);
    control_out->pdu[21] = 0u;
    pdu_put_u16(control_out, 22u, 0u);
    return TAVRN_MENTORSHIP_OK;
}

static tavrn_mentorship_status_t retain_control(
    tavrn_mentorship_t *mentorship, const tavrn_validated_control_t *control,
    uint8_t controlled_flood, tavrn_mentorship_pending_purpose_t purpose,
    const tavrn_adva_t *join_origin, uint16_t join_sequence)
{
    if (mentorship == NULL || control == NULL || controlled_flood > 1u ||
        (purpose != TAVRN_MENTORSHIP_PENDING_OFFER &&
         purpose != TAVRN_MENTORSHIP_PENDING_PULL &&
         purpose != TAVRN_MENTORSHIP_PENDING_DATA)) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    if (mentorship->pending_control.valid != 0u) {
        return TAVRN_MENTORSHIP_BUSY;
    }
    memset(&mentorship->pending_control, 0, sizeof(mentorship->pending_control));
    mentorship->pending_control.control = *control;
    stamp_live_network(mentorship, &mentorship->pending_control.control);
    mentorship->pending_control.controlled_flood = controlled_flood;
    mentorship->pending_control.purpose = (uint8_t)purpose;
    mentorship->pending_control.valid = 1u;
    if (join_origin != NULL) {
        mentorship->pending_control.join_origin = *join_origin;
        mentorship->pending_control.join_sequence = join_sequence;
    }
    return TAVRN_MENTORSHIP_OK;
}

static void remember_join(tavrn_mentorship_t *mentorship, const tavrn_adva_t *origin,
                          uint16_t sequence, uint32_t now_ms);

static int pending_join_exists(const tavrn_mentorship_t *mentorship,
                               const tavrn_adva_t *origin, uint16_t sequence)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_MENTORSHIP_JOIN_OBLIGATION_CAPACITY; index++) {
        const tavrn_mentorship_pending_control_t *pending =
            &mentorship->join_obligations[index];

        if (pending->valid != 0u && pending->join_sequence == sequence &&
            adva_equal(&pending->join_origin, origin)) {
            return 1;
        }
    }
    return 0;
}

static tavrn_mentorship_status_t retain_join_obligation(
    tavrn_mentorship_t *mentorship, const tavrn_validated_control_t *control,
    tavrn_mentorship_pending_purpose_t purpose, const tavrn_adva_t *origin,
    uint16_t sequence)
{
    uint8_t index;
    tavrn_mentorship_pending_control_t *pending = NULL;

    if (mentorship == NULL || control == NULL || origin == NULL ||
        (purpose != TAVRN_MENTORSHIP_PENDING_JOIN_ORIGIN &&
         purpose != TAVRN_MENTORSHIP_PENDING_JOIN_RELAY)) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    if (pending_join_exists(mentorship, origin, sequence)) {
        return TAVRN_MENTORSHIP_OK;
    }
    for (index = 0u; index < TAVRN_MENTORSHIP_JOIN_OBLIGATION_CAPACITY; index++) {
        if (mentorship->join_obligations[index].valid == 0u) {
            pending = &mentorship->join_obligations[index];
            break;
        }
    }
    if (pending == NULL) {
        mentorship->counters.join_obligation_overflow++;
        return TAVRN_MENTORSHIP_BUSY;
    }
    memset(pending, 0, sizeof(*pending));
    pending->control = *control;
    stamp_live_network(mentorship, &pending->control);
    pending->controlled_flood = 1u;
    pending->purpose = (uint8_t)purpose;
    pending->join_origin = *origin;
    pending->join_sequence = sequence;
    pending->valid = 1u;
    return TAVRN_MENTORSHIP_OK;
}

/* A queued relay has not yet changed live membership.  It must nevertheless
 * reserve its compressed suffix against lower-index obligations, because this
 * flush loop commits those first. */
static tavrn_mentorship_status_t revalidate_relay_join(
    tavrn_mentorship_t *mentorship,
    const tavrn_mentorship_pending_control_t *pending, uint8_t pending_index,
    uint32_t now_ms)
{
    tavrn_adva_t subject;
    tavrn_esc_context_match_t match;
    tavrn_esc_context_status_t context_status;
    uint8_t index;

    if (mentorship == NULL || pending == NULL ||
        pending->purpose != TAVRN_MENTORSHIP_PENDING_JOIN_RELAY ||
        pending->control.type != TAVRN_WIRE_TC_UPDATE ||
        pending->control.pdu_len != 24u ||
        pending_index >= TAVRN_MENTORSHIP_JOIN_OBLIGATION_CAPACITY) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    subject = adva_from_bytes(&pending->control.pdu[15]);
    context_status = resolve_sid8(mentorship, subject.bytes[0], now_ms, &match);
    if (context_status == TAVRN_ESC_CONTEXT_RESERVED ||
        context_status == TAVRN_ESC_CONTEXT_COLLIDING ||
        (context_status == TAVRN_ESC_CONTEXT_UNIQUE &&
         !adva_equal(&match.identity, &subject))) {
        return enter_identity_conflict(mentorship, now_ms);
    }
    if (context_status != TAVRN_ESC_CONTEXT_UNKNOWN &&
        context_status != TAVRN_ESC_CONTEXT_UNIQUE) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    for (index = 0u; index < pending_index; index++) {
        const tavrn_mentorship_pending_control_t *earlier =
            &mentorship->join_obligations[index];
        tavrn_adva_t earlier_subject;

        if (earlier->valid == 0u ||
            (earlier->purpose != TAVRN_MENTORSHIP_PENDING_JOIN_ORIGIN &&
             earlier->purpose != TAVRN_MENTORSHIP_PENDING_JOIN_RELAY)) {
            continue;
        }
        earlier_subject = adva_from_bytes(&earlier->control.pdu[15]);
        if (earlier_subject.bytes[0] == subject.bytes[0] &&
            !adva_equal(&earlier_subject, &subject)) {
            return enter_identity_conflict(mentorship, now_ms);
        }
    }
    return TAVRN_MENTORSHIP_OK;
}

static tavrn_mentorship_status_t flush_pending_control(
    tavrn_mentorship_t *mentorship, uint32_t now_ms)
{
    tavrn_link_event_t outcome;
    tavrn_mentorship_pending_control_t pending;
    tavrn_link_send_status_t send_status;

    if (mentorship == NULL || mentorship->router == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    if (mentorship->pending_control.valid == 0u) {
        return TAVRN_MENTORSHIP_OK;
    }
    pending = mentorship->pending_control;
    stamp_live_network(mentorship, &pending.control);
    memset(&outcome, 0, sizeof(outcome));
    send_status = tavrn_link_v2_send_control(mentorship->router->link,
                                              &pending.control, NULL,
                                              pending.controlled_flood, now_ms,
                                              &outcome);
    if (send_status == TAVRN_LINK_SEND_BUSY) {
        return TAVRN_MENTORSHIP_BUSY;
    }
    if (send_status != TAVRN_LINK_SEND_OK) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    if (pending.controlled_flood != 0u) {
        tavrn_router_note_local_broadcast(mentorship->router, now_ms);
    }
    memset(&mentorship->pending_control, 0, sizeof(mentorship->pending_control));
    if (outcome.type != TAVRN_LINK_EVENT_NONE &&
        tavrn_router_handle_link_event(mentorship->router, &outcome, now_ms) ==
            TAVRN_ROUTER_EVENT_INVALID) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    switch ((tavrn_mentorship_pending_purpose_t)pending.purpose) {
    case TAVRN_MENTORSHIP_PENDING_OFFER:
        mentorship->pending_offer_valid = 0u;
        break;
    case TAVRN_MENTORSHIP_PENDING_PULL:
        mentorship->state.pull_attempts++;
        mentorship->page_deadline_ms = now_ms + mentorship->config.page_timeout_ms;
        mentorship->page_deadline_valid = 1u;
        break;
    default:
        break;
    }
    return TAVRN_MENTORSHIP_OK;
}

static tavrn_mentorship_status_t flush_pending_join_obligation(
    tavrn_mentorship_t *mentorship, uint32_t now_ms)
{
    uint8_t index;

    if (mentorship == NULL || mentorship->router == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    for (index = 0u; index < TAVRN_MENTORSHIP_JOIN_OBLIGATION_CAPACITY; index++) {
        tavrn_mentorship_pending_control_t *pending =
            &mentorship->join_obligations[index];
        tavrn_link_event_t outcome;
        tavrn_link_send_status_t send_status;
        tavrn_mentorship_record_t record;
        tavrn_adva_t join_origin;
        tavrn_esc_context_match_t candidate_match;
        tavrn_esc_context_status_t candidate_context;
        tavrn_mentorship_status_t revalidation;
        uint16_t join_sequence;

        if (pending->valid == 0u) {
            continue;
        }
        if (pending->purpose != TAVRN_MENTORSHIP_PENDING_JOIN_ORIGIN &&
            pending->purpose != TAVRN_MENTORSHIP_PENDING_JOIN_RELAY) {
            return TAVRN_MENTORSHIP_INVALID;
        }
        memset(&record, 0, sizeof(record));
        if (pending->purpose == TAVRN_MENTORSHIP_PENDING_JOIN_RELAY) {
            revalidation = revalidate_relay_join(mentorship, pending, index, now_ms);
            if (revalidation != TAVRN_MENTORSHIP_OK) {
                return revalidation;
            }
            record.identity = adva_from_bytes(&pending->control.pdu[15]);
            record.ttl_bucket = 1u;
            record.hop_count = 1u;
            record.departed = pending->control.pdu[21] == 1u;
            candidate_context = tavrn_esc_resolve_sid8(
                mentorship->gtt, record.identity.bytes[0], now_ms, &candidate_match);
            if (record.departed == 0u &&
                candidate_context != TAVRN_ESC_CONTEXT_UNKNOWN &&
                (candidate_context != TAVRN_ESC_CONTEXT_UNIQUE ||
                 !adva_equal(&candidate_match.identity, &record.identity))) {
                return enter_identity_conflict(mentorship, now_ms);
            }
        }
        stamp_live_network(mentorship, &pending->control);
        memset(&outcome, 0, sizeof(outcome));
        send_status = tavrn_link_v2_send_control(mentorship->router->link,
                                                  &pending->control, NULL,
                                                  pending->controlled_flood,
                                                  now_ms, &outcome);
        if (send_status == TAVRN_LINK_SEND_BUSY) {
            return TAVRN_MENTORSHIP_BUSY;
        }
        if (send_status != TAVRN_LINK_SEND_OK) {
            return TAVRN_MENTORSHIP_INVALID;
        }
        if (pending->controlled_flood != 0u) {
            tavrn_router_note_local_broadcast(mentorship->router, now_ms);
        }
        if (pending->purpose == TAVRN_MENTORSHIP_PENDING_JOIN_ORIGIN) {
            int sequence_commit_ok = 1;

            if (mentorship->tc_metadata != NULL) {
                tavrn_tc_sequence_ticket_t ticket;

                memset(&ticket, 0, sizeof(ticket));
                ticket.sequence = pending->join_sequence;
                ticket.event = TAVRN_TC_EVENT_JOIN;
                ticket.valid = 1u;
                if (tavrn_maintenance_tc_sequence_commit(
                        mentorship->tc_metadata, &ticket,
                        TAVRN_TC_ADMISSION_ADMITTED, now_ms) != TAVRN_TC_METADATA_OK) {
                    sequence_commit_ok = 0;
                }
            }
            memset(pending, 0, sizeof(*pending));
            if (outcome.type != TAVRN_LINK_EVENT_NONE &&
                tavrn_router_handle_link_event(mentorship->router, &outcome, now_ms) ==
                    TAVRN_ROUTER_EVENT_INVALID) {
                return TAVRN_MENTORSHIP_INVALID;
            }
            if (sequence_commit_ok == 0) {
                return TAVRN_MENTORSHIP_INVALID;
            }
            mentorship->state.join_originated = 1u;
            mentorship->counters.join_originated++;
            /* Queue admission is the existing JOIN transaction commit point.
             * Rebase from that point, rather than the stale due time, so a
             * delayed/busy JOIN can never produce catch-up reannouncements. */
            if (mentorship->state.state == TAVRN_MENTORSHIP_SID8_ACTIVE &&
                mentorship->tc_metadata != NULL) {
                mentorship->join_reannounce_deadline_ms =
                    now_ms + TAVRN_MENTORSHIP_JOIN_REANNOUNCE_MS(
                        &mentorship->config);
                mentorship->join_reannounce_valid = 1u;
            }
        } else if (pending->purpose == TAVRN_MENTORSHIP_PENDING_JOIN_RELAY) {
            join_origin = pending->join_origin;
            join_sequence = pending->join_sequence;
            memset(pending, 0, sizeof(*pending));
            if (outcome.type != TAVRN_LINK_EVENT_NONE &&
                tavrn_router_handle_link_event(mentorship->router, &outcome, now_ms) ==
                    TAVRN_ROUTER_EVENT_INVALID) {
                return TAVRN_MENTORSHIP_INVALID;
            }
            if (!gtt_observe_record(mentorship, &record, now_ms)) {
                return TAVRN_MENTORSHIP_BUSY;
            }
            remember_join(mentorship, &join_origin, join_sequence, now_ms);
            mentorship->counters.join_received++;
            mentorship->counters.join_relayed++;
        }
        memset(pending, 0, sizeof(*pending));
        return TAVRN_MENTORSHIP_OK;
    }
    return TAVRN_MENTORSHIP_OK;
}

static void originate_join(tavrn_mentorship_t *mentorship)
{
    tavrn_validated_control_t join;
    uint16_t sequence = 1u;

    if (mentorship == NULL || mentorship->router == NULL || mentorship->gtt == NULL) {
        return;
    }
    if (mentorship->tc_metadata != NULL) {
        tavrn_tc_sequence_ticket_t ticket;
        tavrn_tc_metadata_status_t status = tavrn_maintenance_tc_sequence_prepare(
            mentorship->tc_metadata, TAVRN_TC_EVENT_JOIN, &ticket);

        if ((status != TAVRN_TC_METADATA_PREPARED &&
              status != TAVRN_TC_METADATA_RETAINED) || ticket.valid == 0u) {
            return;
        }
        sequence = ticket.sequence;
    }
    if (build_join_for_local(mentorship, sequence, &join) != TAVRN_MENTORSHIP_OK) {
        return;
    }
    (void)retain_join_obligation(mentorship, &join,
                                  TAVRN_MENTORSHIP_PENDING_JOIN_ORIGIN,
                                  &mentorship->gtt->config.local_identity,
                                  sequence);
}

tavrn_mentorship_status_t tavrn_mentorship_init(
    tavrn_mentorship_t *mentorship, tavrn_router_t *router, tavrn_gtt_t *gtt,
    const tavrn_mentorship_config_t *config, uint32_t now_ms)
{
    if (mentorship == NULL || router == NULL || gtt == NULL || gtt->storage == NULL ||
        !config_is_valid(config)) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    memset(mentorship, 0, sizeof(*mentorship));
    mentorship->router = router;
    mentorship->gtt = gtt;
    mentorship->config = *config;
    mentorship->started_at_ms = now_ms;
    mentorship->state.state = TAVRN_MENTORSHIP_REJOINING;
    mentorship->state.active_width = TAVRN_IDENTITY_SID16;
    mentorship->state.ordinary_traffic_gated = 1u;
    mentorship->state.full_bootstrap_admission_enabled = 1u;
    tavrn_link_v2_set_identity_admission(router->link, sid8_identity_conflict,
                                         mentorship);
    return TAVRN_MENTORSHIP_OK;
}

tavrn_mentorship_status_t tavrn_mentorship_bind_tc_metadata(
    tavrn_mentorship_t *mentorship, struct tavrn_tc_metadata_state *state)
{
    if (mentorship == NULL || state == NULL || mentorship->tc_metadata != NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    mentorship->tc_metadata = state;
    return TAVRN_MENTORSHIP_OK;
}

tavrn_mentorship_status_t tavrn_mentorship_freeze_snapshot(
    tavrn_mentorship_t *mentorship, uint16_t snapshot_id, uint32_t now_ms,
    tavrn_mentorship_snapshot_data_t *snapshot_out)
{
    tavrn_mentorship_snapshot_data_t snapshot;
    uint8_t index;

    if (mentorship == NULL || mentorship->gtt == NULL ||
        mentorship->gtt->storage == NULL || snapshot_out == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.snapshot_id = snapshot_id;
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &mentorship->gtt->storage->entries[index];
        tavrn_mentorship_record_t *record;
        uint32_t remaining_hard_ms;
        uint32_t bucket;
        int is_local;

        if (entry->occupied == 0u || tombstone_is_purged(entry, now_ms)) {
            continue;
        }
        is_local = adva_equal(&entry->identity, &mentorship->gtt->config.local_identity);
        if (snapshot.count >= TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY) {
            return TAVRN_MENTORSHIP_BUSY;
        }
        record = &snapshot.records[snapshot.count++];
        record->identity = entry->identity;
        record->serial = entry->serial;
        record->serial_present = entry->serial_present;
        record->departed = entry->departed;
        if (entry->departed != 0u) {
            record->ttl_bucket = 0u;
        } else {
            /* Local identity is live by construction.  Its retained GTT
             * deadline is not remote evidence, so freeze a fresh hard TTL. */
            if (is_local == 0 && time_due(now_ms, entry->hard_deadline_ms)) {
                bucket = 1u;
            } else {
                remaining_hard_ms = is_local != 0 ?
                    mentorship->gtt->config.hard_expiry_ms :
                    entry->hard_deadline_ms - now_ms;
                bucket = (remaining_hard_ms + 19999u) / 20000u;
                if (bucket == 0u) {
                    bucket = 1u;
                } else if (bucket > 15u) {
                    bucket = 15u;
                }
            }
            record->ttl_bucket = (uint8_t)bucket;
        }
        record->hop_count = entry->hop_count;
    }
    for (index = 1u; index < snapshot.count; index++) {
        tavrn_mentorship_record_t current = snapshot.records[index];
        uint8_t insertion = index;

        while (insertion > 0u &&
               memcmp(current.identity.bytes,
                      snapshot.records[(uint8_t)(insertion - 1u)].identity.bytes,
                      TAVRN_ADVA_LEN) < 0) {
            snapshot.records[insertion] = snapshot.records[(uint8_t)(insertion - 1u)];
            insertion--;
        }
        snapshot.records[insertion] = current;
    }
    mentorship->frozen_snapshot = snapshot;
    mentorship->state.frozen_snapshot_count = snapshot.count;
    *snapshot_out = snapshot;
    return TAVRN_MENTORSHIP_OK;
}

tavrn_esc_context_status_t tavrn_mentorship_resolve_sid8(
    const tavrn_mentorship_t *mentorship, uint8_t sid8, uint32_t now_ms,
    tavrn_esc_context_match_t *match_out)
{
    return resolve_sid8(mentorship, sid8, now_ms, match_out);
}

tavrn_mentorship_status_t tavrn_mentorship_offer_delay_ms(
    const tavrn_mentorship_config_t *config, uint8_t rssi_magnitude_db,
    uint32_t jitter_ms, uint32_t *delay_out)
{
    uint32_t range;
    uint32_t position;
    uint32_t delay;

    if (delay_out != NULL) {
        *delay_out = 0u;
    }
    if (!config_is_valid(config) || delay_out == NULL ||
        jitter_ms < config->jitter_min_ms || jitter_ms > config->jitter_max_ms) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    if (rssi_magnitude_db >= config->rssi_weak_magnitude_db) {
        delay = config->rssi_weak_delay_ms;
    } else if (rssi_magnitude_db <= config->rssi_strong_magnitude_db) {
        delay = config->rssi_strong_delay_ms;
    } else {
        range = (uint32_t)config->rssi_weak_magnitude_db -
            config->rssi_strong_magnitude_db;
        position = (uint32_t)rssi_magnitude_db - config->rssi_strong_magnitude_db;
        delay = config->rssi_strong_delay_ms +
            ((config->rssi_weak_delay_ms - config->rssi_strong_delay_ms) *
             position) / range;
    }
    *delay_out = delay + jitter_ms;
    return TAVRN_MENTORSHIP_OK;
}

tavrn_mentorship_offer_status_t tavrn_mentorship_collect_offer(
    tavrn_mentorship_t *mentorship, const tavrn_mentorship_offer_t *offer,
    uint32_t now_ms)
{
    uint8_t index;
    int empty = -1;
    int worst = -1;

    if (!offer_is_valid(mentorship, offer)) {
        return TAVRN_MENTORSHIP_OFFER_INVALID;
    }
    for (index = 0u; index < TAVRN_MENTORSHIP_OFFER_CAPACITY; index++) {
        if (mentorship->offers[index].boot_nonce == 0u) {
            if (empty < 0) {
                empty = (int)index;
            }
            continue;
        }
        if (offer_equal(&mentorship->offers[index], offer)) {
            return TAVRN_MENTORSHIP_OFFER_DUPLICATE;
        }
    }
    if (empty < 0) {
        mentorship->counters.offer_capacity_full++;
        for (index = 0u; index < TAVRN_MENTORSHIP_OFFER_CAPACITY; index++) {
            if (worst < 0 || record_is_better_offer(
                                 &mentorship->offers[(uint8_t)worst],
                                 &mentorship->offers[index])) {
                worst = (int)index;
            }
        }
        if (worst >= 0 && record_is_better_offer(
                              offer, &mentorship->offers[(uint8_t)worst])) {
            mentorship->offers[(uint8_t)worst] = *offer;
            mentorship->counters.offer_collected++;
            return TAVRN_MENTORSHIP_OFFER_ACCEPTED;
        }
        mentorship->counters.offer_not_retained++;
        return TAVRN_MENTORSHIP_OFFER_STORAGE_FULL;
    }
    mentorship->offers[(uint8_t)empty] = *offer;
    mentorship->state.active_offer_count++;
    mentorship->counters.offer_collected++;
    if (mentorship->state.state == TAVRN_MENTORSHIP_REJOINING) {
        mentorship->state.state = TAVRN_MENTORSHIP_COLLECTING_OFFERS;
        mentorship->offer_window_deadline_ms = now_ms + mentorship->config.offer_window_ms;
    }
    return TAVRN_MENTORSHIP_OFFER_ACCEPTED;
}

tavrn_mentorship_offer_status_t tavrn_mentorship_schedule_offer(
    tavrn_mentorship_t *mentorship, const tavrn_mentorship_offer_t *offer,
    uint32_t due_ms)
{
    if (mentorship == NULL || offer == NULL || offer->boot_nonce == 0u ||
        offer->snapshot_count > TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY ||
        (!offer_is_valid(mentorship, offer) &&
         !adva_equal(&offer->mentor, &mentorship->gtt->config.local_identity))) {
        return TAVRN_MENTORSHIP_OFFER_INVALID;
    }
    if (mentorship->offer_suppressed_until_ms != 0u &&
        !time_due(due_ms, mentorship->offer_suppressed_until_ms)) {
        return TAVRN_MENTORSHIP_OFFER_SUPPRESSED;
    }
    mentorship->pending_offer = *offer;
    mentorship->pending_offer_valid = 1u;
    mentorship->pending_offer_due_ms = due_ms;
    mentorship->counters.offer_scheduled++;
    return TAVRN_MENTORSHIP_OFFER_ACCEPTED;
}

tavrn_mentorship_offer_status_t tavrn_mentorship_overhear_offer(
    tavrn_mentorship_t *mentorship, const tavrn_mentorship_offer_t *offer,
    uint32_t now_ms)
{
    (void)now_ms;
    if (mentorship == NULL || offer == NULL || offer->boot_nonce == 0u ||
        offer->snapshot_count > TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY) {
        return TAVRN_MENTORSHIP_OFFER_INVALID;
    }
    if (mentorship->pending_offer_valid == 0u ||
        !adva_equal(&mentorship->pending_offer.mentee, &offer->mentee)) {
        return TAVRN_MENTORSHIP_OFFER_ACCEPTED;
    }
    memset(&mentorship->pending_offer, 0, sizeof(mentorship->pending_offer));
    mentorship->pending_offer_valid = 0u;
    mentorship->offer_suppressed_until_ms = mentorship->pending_offer_due_ms +
        mentorship->config.offer_suppression_ms;
    mentorship->pending_offer_due_ms = 0u;
    mentorship->counters.offer_suppressed++;
    return TAVRN_MENTORSHIP_OFFER_SUPPRESSED;
}

tavrn_mentorship_status_t tavrn_mentorship_select_offer(
    tavrn_mentorship_t *mentorship, uint32_t now_ms,
    tavrn_mentorship_offer_t *selected_out)
{
    int best = -1;
    uint8_t index;

    if (selected_out != NULL) {
        memset(selected_out, 0, sizeof(*selected_out));
    }
    (void)now_ms;
    if (mentorship == NULL || selected_out == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    for (index = 0u; index < TAVRN_MENTORSHIP_OFFER_CAPACITY; index++) {
        if (mentorship->offers[index].boot_nonce == 0u) {
            continue;
        }
        if (best < 0 || record_is_better_offer(&mentorship->offers[index],
                                                &mentorship->offers[(uint8_t)best])) {
            best = (int)index;
        }
    }
    if (best < 0) {
        return TAVRN_MENTORSHIP_UNRESOLVED;
    }
    *selected_out = mentorship->offers[(uint8_t)best];
    mentorship->counters.offer_selected++;
    mentorship->state.state = TAVRN_MENTORSHIP_SYNCING;
    mentorship->state.selected_mentor = selected_out->mentor;
    mentorship->state.selected_snapshot_id = selected_out->snapshot_id;
    mentorship->state.selected_mentor_present = 1u;
    mentorship->state.frozen_snapshot_count = selected_out->snapshot_count;
    mentorship->state.sync_next_index = 0u;
    mentorship->state.sync_pages_committed = 0u;
    mentorship->state.pull_attempts = 0u;
    mentorship->page_deadline_ms = 0u;
    mentorship->page_deadline_valid = 0u;
    clear_receiving_staging(mentorship);
    mentorship->page_session_valid = 1u;
    mentorship->active_page_snapshot_id = selected_out->snapshot_id;
    return TAVRN_MENTORSHIP_OK;
}

tavrn_mentorship_status_t tavrn_mentorship_build_sync_offer(
    const tavrn_mentorship_snapshot_data_t *snapshot,
    const tavrn_adva_t *mentor, const tavrn_adva_t *mentee,
    uint16_t mentee_boot_nonce, tavrn_validated_control_t *control_out)
{
    if (snapshot == NULL || mentor == NULL || mentee == NULL || control_out == NULL ||
        snapshot->count > TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY ||
        mentee_boot_nonce == 0u) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    begin_control(control_out, TAVRN_WIRE_SYNC_OFFER, 24u);
    control_out->pdu[6] = 0x10u;
    memcpy(&control_out->pdu[7], mentor->bytes, TAVRN_ADVA_LEN);
    memcpy(&control_out->pdu[13], mentee->bytes, TAVRN_ADVA_LEN);
    control_out->pdu[19] = snapshot->count;
    pdu_put_u16(control_out, 20u, snapshot->snapshot_id);
    pdu_put_u16(control_out, 22u, mentee_boot_nonce);
    return TAVRN_MENTORSHIP_OK;
}

tavrn_mentorship_status_t tavrn_mentorship_build_sync_pull(
    const tavrn_adva_t *mentee, const tavrn_adva_t *mentor,
    uint16_t snapshot_id, uint8_t page_index,
    tavrn_validated_control_t *control_out)
{
    if (mentee == NULL || mentor == NULL || control_out == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    begin_control(control_out, TAVRN_WIRE_SYNC_PULL, 22u);
    memcpy(&control_out->pdu[6], mentee->bytes, TAVRN_ADVA_LEN);
    memcpy(&control_out->pdu[12], mentor->bytes, TAVRN_ADVA_LEN);
    pdu_put_u16(control_out, 18u, snapshot_id);
    control_out->pdu[20] = page_index;
    control_out->pdu[21] = 1u;
    return TAVRN_MENTORSHIP_OK;
}

tavrn_mentorship_status_t tavrn_mentorship_build_sync_data(
    const tavrn_mentorship_snapshot_data_t *snapshot,
    const tavrn_adva_t *mentor, const tavrn_adva_t *mentee, uint8_t page_index,
    tavrn_validated_control_t *control_out)
{
    tavrn_mentorship_record_t record;
    uint8_t last;

    if (snapshot == NULL || mentor == NULL || mentee == NULL || control_out == NULL ||
        snapshot->count > TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY ||
        (snapshot->count != 0u && page_index >= snapshot->count) ||
        (snapshot->count == 0u && page_index != 0u)) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    last = snapshot->count == 0u || page_index == (uint8_t)(snapshot->count - 1u);
    if (snapshot->count == 0u) {
        begin_control(control_out, TAVRN_WIRE_SYNC_DATA, 15u);
        control_out->pdu[5] = 0x40u;
    } else {
        record = snapshot->records[page_index];
        begin_control(control_out, TAVRN_WIRE_SYNC_DATA, 24u);
        control_out->pdu[5] = (uint8_t)(0x80u | (last != 0u ? 0x40u : 0u));
        memcpy(&control_out->pdu[15], record.identity.bytes, TAVRN_ADVA_LEN);
        pdu_put_u16(control_out, 21u, record.serial);
        control_out->pdu[23] = (uint8_t)((record.departed != 0u ? 0u :
                                          record.ttl_bucket & 0x0fu) << 4) |
            (record.hop_count & 0x0fu);
    }
    memcpy(&control_out->pdu[6], mentee->bytes, TAVRN_ADVA_LEN);
    pdu_put_u16(control_out, 12u, snapshot->snapshot_id);
    control_out->pdu[14] = page_index;
    return TAVRN_MENTORSHIP_OK;
}

static uint16_t expected_receiving_page_bitmap(uint8_t record_count)
{
    if (record_count >= TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY) {
        return UINT16_MAX;
    }
    return (uint16_t)(((uint16_t)1u << record_count) - 1u);
}

static tavrn_mentorship_status_t commit_received_snapshot(
    tavrn_mentorship_t *mentorship, uint16_t received_page_bitmap,
    uint32_t now_ms)
{
    tavrn_gtt_sync_record_t records[TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY];
    tavrn_gtt_sync_merge_status_t merge_status;
    uint8_t index;
    uint8_t record_count;

    if (mentorship == NULL || mentorship->gtt == NULL ||
        mentorship->gtt->storage == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    record_count = mentorship->state.frozen_snapshot_count;
    if ((record_count == 0u && received_page_bitmap != 1u) ||
        (record_count != 0u &&
         received_page_bitmap != expected_receiving_page_bitmap(record_count))) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    for (index = 0u; index < record_count; index++) {
        const tavrn_mentorship_record_t *record = &mentorship->receiving_records[index];
        uint8_t prior;

        memset(&records[index], 0, sizeof(records[index]));
        records[index].identity = record->identity;
        records[index].serial = record->serial;
        records[index].serial_present = record->serial_present;
        records[index].hop_count = record->hop_count;
        records[index].departed = record->departed;
        records[index].remaining_lifetime_ms =
            (uint32_t)record->ttl_bucket * 20000u;
        if (record->departed != 0u) {
            continue;
        }
        for (prior = 0u; prior < index; prior++) {
            const tavrn_mentorship_record_t *previous =
                &mentorship->receiving_records[prior];

            if (previous->departed == 0u &&
                previous->identity.bytes[0] == record->identity.bytes[0] &&
                !adva_equal(&previous->identity, &record->identity)) {
                return enter_identity_conflict(mentorship, now_ms);
            }
        }
        if (!adva_equal(&record->identity, &mentorship->gtt->config.local_identity)) {
            tavrn_esc_context_match_t existing;
            tavrn_esc_context_status_t context_status = tavrn_esc_resolve_sid8(
                mentorship->gtt, record->identity.bytes[0], now_ms, &existing);

            if (context_status == TAVRN_ESC_CONTEXT_COLLIDING ||
                (context_status == TAVRN_ESC_CONTEXT_UNIQUE &&
                 !adva_equal(&existing.identity, &record->identity))) {
                return enter_identity_conflict(mentorship, now_ms);
            }
        }
    }
    merge_status = record_count == 0u ? TAVRN_GTT_SYNC_MERGE_UNCHANGED :
        tavrn_gtt_sync_merge(mentorship->gtt, records, record_count, now_ms);
    if (merge_status != TAVRN_GTT_SYNC_MERGE_COMMITTED &&
        merge_status != TAVRN_GTT_SYNC_MERGE_UNCHANGED) {
        return TAVRN_MENTORSHIP_BUSY;
    }
    return TAVRN_MENTORSHIP_OK;
}

tavrn_mentorship_status_t tavrn_mentorship_ingest_sync_data(
    tavrn_mentorship_t *mentorship, const tavrn_adva_t *mentor,
    const tavrn_validated_control_t *control, uint32_t now_ms)
{
    uint8_t index;
    uint8_t present;
    tavrn_mentorship_record_t record;
    tavrn_esc_context_match_t existing;
    tavrn_esc_context_status_t context_status;
    tavrn_mentorship_status_t commit_status;
    tavrn_mentorship_sync_dedupe_t *completed_slot = NULL;
    uint16_t received_page_bitmap;

    if (mentorship == NULL || mentor == NULL || control == NULL ||
        control->type != TAVRN_WIRE_SYNC_DATA ||
        (control->pdu_len != 15u && control->pdu_len != 24u)) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    if (!adva_bytes_equal(&mentorship->gtt->config.local_identity,
                          &control->pdu[6]) ||
        mentorship->state.state != TAVRN_MENTORSHIP_SYNCING ||
        mentorship->page_session_valid == 0u) {
        return TAVRN_MENTORSHIP_OK;
    }
    if (!adva_equal(&mentorship->state.selected_mentor, mentor) ||
        mentorship->active_page_snapshot_id != pdu_u16(control, 12u)) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    if (mentorship->page_deadline_valid == 0u) {
        return TAVRN_MENTORSHIP_SYNC_PAGE_DUPLICATE;
    }
    index = control->pdu[14];
    present = (control->pdu[5] & 0x80u) != 0u;
    if ((present == 0u && (control->pdu_len != 15u || control->pdu[5] != 0x40u ||
                           index != 0u)) ||
        (present != 0u && control->pdu_len != 24u) ||
        index >= TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY ||
        (index >= mentorship->state.frozen_snapshot_count &&
         mentorship->state.frozen_snapshot_count != 0u)) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    if ((mentorship->state.frozen_snapshot_count == 0u &&
         (present != 0u || index != 0u || control->pdu[5] != 0x40u)) ||
        (mentorship->state.frozen_snapshot_count != 0u &&
         (present == 0u || ((control->pdu[5] & 0x40u) != 0u) !=
               (index == (uint8_t)(mentorship->state.frozen_snapshot_count - 1u))))) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    /* Scheduler ingress precedes mentorship_tick().  A page at the deadline
     * belongs to the timeout/retry path and cannot clear that deadline. */
    if (mentorship->page_deadline_valid != 0u &&
        time_due(now_ms, mentorship->page_deadline_ms)) {
        return TAVRN_MENTORSHIP_SYNC_PAGE_DUPLICATE;
    }
    if (completed_sync_page_seen(mentorship, mentor, index, now_ms)) {
        mentorship->counters.sync_page_duplicate++;
        return TAVRN_MENTORSHIP_SYNC_PAGE_DUPLICATE;
    }
    if ((mentorship->receiving_page_bitmap & ((uint16_t)1u << index)) != 0u) {
        mentorship->counters.sync_page_duplicate++;
        return TAVRN_MENTORSHIP_SYNC_PAGE_DUPLICATE;
    }
    if (index != mentorship->state.sync_next_index) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    if ((control->pdu[5] & 0x40u) != 0u) {
        completed_slot = completed_sync_admission_slot(mentorship, now_ms);
        if (completed_slot == NULL) {
            mentorship->counters.sync_dedupe_capacity_full++;
            return TAVRN_MENTORSHIP_BUSY;
        }
    }
    if (present != 0u) {
        memset(&record, 0, sizeof(record));
        record.identity = adva_from_bytes(&control->pdu[15]);
        record.serial = pdu_u16(control, 21u);
        record.serial_present = 1u;
        record.departed = (control->pdu[23] >> 4) == 0u;
        record.ttl_bucket = (uint8_t)(control->pdu[23] >> 4);
        record.hop_count = (uint8_t)(control->pdu[23] & 0x0fu);
        if (record.hop_count < TAVRN_GTT_HOP_MAX) {
            record.hop_count++;
        }
        context_status = resolve_sid8(mentorship, record.identity.bytes[0], now_ms,
                                       &existing);
        if (record.departed == 0u && context_status == TAVRN_ESC_CONTEXT_UNIQUE &&
            !adva_equal(&existing.identity, &record.identity)) {
            return enter_identity_conflict(mentorship, now_ms);
        }
        if (record.departed == 0u && context_status == TAVRN_ESC_CONTEXT_COLLIDING) {
            return enter_identity_conflict(mentorship, now_ms);
        }
        mentorship->receiving_records[index] = record;
    }
    received_page_bitmap = mentorship->receiving_page_bitmap |
        ((uint16_t)1u << index);
    if ((control->pdu[5] & 0x40u) != 0u) {
        commit_status = commit_received_snapshot(mentorship, received_page_bitmap,
                                                 now_ms);
        if (commit_status != TAVRN_MENTORSHIP_OK) {
            return commit_status;
        }
    }
    mentorship->receiving_page_bitmap = received_page_bitmap;
    mentorship->state.sync_pages_committed++;
    mentorship->state.sync_next_index = (uint8_t)(index + 1u);
    mentorship->state.pull_attempts = 0u;
    mentorship->page_deadline_ms = 0u;
    mentorship->page_deadline_valid = 0u;
    if ((control->pdu[5] & 0x40u) != 0u) {
        remember_completed_sync(completed_slot, mentorship, received_page_bitmap,
                                now_ms);
        mentorship->sync_complete_authorized = 1u;
        mentorship->counters.sync_page_merged +=
            mentorship->state.frozen_snapshot_count;
    }
    return TAVRN_MENTORSHIP_OK;
}

static tavrn_mentorship_status_t process_hello(tavrn_mentorship_t *mentorship,
                                                 const tavrn_rx_control_event_t *event,
                                                 uint32_t now_ms)
{
    tavrn_mentorship_record_t record;
    tavrn_mentorship_offer_t offer;
    tavrn_mentorship_snapshot_data_t snapshot;
    tavrn_esc_context_match_t existing;
    tavrn_esc_context_status_t context_status;
    uint16_t boot_nonce;
    uint16_t snapshot_id;
    uint32_t jitter;
    uint32_t delay;
    uint8_t index;

    if (event->control.type != TAVRN_WIRE_HELLO || event->control.pdu_len != 19u ||
        event->control.pdu[5] != 0x40u ||
        !adva_bytes_equal(&event->transmitter.adva, &event->control.pdu[11])) {
        return TAVRN_MENTORSHIP_OK;
    }
    boot_nonce = pdu_u16(&event->control, 17u);
    if (mentorship->state.state == TAVRN_MENTORSHIP_SID8_ACTIVE &&
        mentorship->serving_session_valid != 0u) {
        if (time_due(now_ms, mentorship->serving_deadline_ms)) {
            clear_serving_session(mentorship);
        } else if (adva_equal(&mentorship->serving_mentee,
                              &event->transmitter.adva)) {
            if (mentorship->serving_boot_nonce == boot_nonce) {
                /* Exact N=1 retransmission is a pure serving-session no-op. */
                return TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED;
            }
            /* The same physical mentee has a new incarnation.  Discard its
             * entire old serving slot before taking a replacement snapshot. */
            clear_serving_session(mentorship);
        } else {
            context_status = resolve_sid8(mentorship,
                                          event->transmitter.adva.bytes[0],
                                          now_ms, &existing);
            if (context_status == TAVRN_ESC_CONTEXT_RESERVED) {
                mentorship->counters.sid8_reserved_drop++;
                return enter_identity_conflict(mentorship, now_ms);
            }
            if (context_status == TAVRN_ESC_CONTEXT_COLLIDING ||
                (context_status == TAVRN_ESC_CONTEXT_UNIQUE &&
                 !adva_equal(&existing.identity, &event->transmitter.adva))) {
                mentorship->counters.sid8_collision_drop++;
                return enter_identity_conflict(mentorship, now_ms);
            }
            /* One mentor never interleaves state for independent mentees. */
            return TAVRN_MENTORSHIP_BUSY;
        }
    }
    memset(&record, 0, sizeof(record));
    record.identity = event->transmitter.adva;
    /* N=1's boot nonce distinguishes an incarnation; it is not a membership
     * freshness serial and must never advance GTT serial state. */
    record.serial_present = 0u;
    record.ttl_bucket = 1u;
    record.hop_count = 1u;
    /* A late full-identity N=1 is admissible in SID8 only if it refreshes the
     * exact retained identity.  Resolve before GTT observation so a newcomer
     * can never manufacture the collision it is supposed to report. */
    if (mentorship->state.state == TAVRN_MENTORSHIP_SID8_ACTIVE) {
        context_status = resolve_sid8(mentorship, record.identity.bytes[0], now_ms,
                                      &existing);
        if (context_status == TAVRN_ESC_CONTEXT_RESERVED) {
            mentorship->counters.sid8_reserved_drop++;
            return enter_identity_conflict(mentorship, now_ms);
        }
        if (context_status == TAVRN_ESC_CONTEXT_COLLIDING) {
            mentorship->counters.sid8_collision_drop++;
            return enter_identity_conflict(mentorship, now_ms);
        }
        if (context_status == TAVRN_ESC_CONTEXT_UNIQUE &&
            !adva_equal(&existing.identity, &record.identity)) {
            mentorship->counters.sid8_collision_drop++;
            return enter_identity_conflict(mentorship, now_ms);
        }
    }
    if (!gtt_observe_record(mentorship, &record, now_ms)) {
        return TAVRN_MENTORSHIP_BUSY;
    }
    if (mentorship->state.active_width == TAVRN_IDENTITY_SID8) {
        mentorship->counters.bootstrap_admitted_while_sid8++;
    }
    if (mentorship->state.state != TAVRN_MENTORSHIP_SID8_ACTIVE) {
        return TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED;
    }
    if (mentorship->pending_control.valid != 0u || mentorship->pending_offer_valid != 0u) {
        return TAVRN_MENTORSHIP_BUSY;
    }
    snapshot_id = (uint16_t)(boot_nonce ^
        ((uint16_t)mentorship->gtt->config.local_identity.bytes[0] |
         ((uint16_t)mentorship->gtt->config.local_identity.bytes[1] << 8)));
    if (snapshot_id == 0u) {
        snapshot_id = 1u;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    if (tavrn_mentorship_freeze_snapshot(mentorship, snapshot_id, now_ms, &snapshot) !=
        TAVRN_MENTORSHIP_OK) {
        return TAVRN_MENTORSHIP_BUSY;
    }
    memset(&offer, 0, sizeof(offer));
    offer.mentor = mentorship->gtt->config.local_identity;
    offer.mentee = event->transmitter.adva;
    offer.snapshot_id = snapshot.snapshot_id;
    offer.boot_nonce = boot_nonce;
    offer.snapshot_count = snapshot.count;
    offer.rssi_magnitude_db = event->rssi_magnitude_db;
    jitter = mentorship->config.jitter_min_ms;
    if (mentorship->config.jitter_max_ms > mentorship->config.jitter_min_ms) {
        uint32_t hash = boot_nonce;

        for (index = 0u; index < TAVRN_ADVA_LEN; index++) {
            hash += (uint32_t)offer.mentor.bytes[index] +
                ((uint32_t)offer.mentee.bytes[index] << 1u);
        }
        jitter += hash % (mentorship->config.jitter_max_ms -
                          mentorship->config.jitter_min_ms + 1u);
    }
    if (tavrn_mentorship_offer_delay_ms(&mentorship->config,
                                        event->rssi_magnitude_db, jitter,
                                        &delay) != TAVRN_MENTORSHIP_OK) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    if (tavrn_mentorship_schedule_offer(mentorship, &offer, now_ms + delay) ==
        TAVRN_MENTORSHIP_OFFER_INVALID) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    mentorship->serving_snapshot = snapshot;
    mentorship->serving_mentee = event->transmitter.adva;
    mentorship->serving_boot_nonce = boot_nonce;
    mentorship->serving_session_valid = 1u;
    mentorship->serving_deadline_ms = now_ms +
        mentorship->config.page_timeout_ms * mentorship->config.page_attempts +
        mentorship->config.offer_window_ms;
    return TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED;
}

static tavrn_mentorship_status_t process_pull(tavrn_mentorship_t *mentorship,
                                               const tavrn_rx_control_event_t *event,
                                               uint32_t now_ms)
{
    tavrn_adva_t mentee;
    tavrn_adva_t mentor;
    tavrn_validated_control_t data;
    uint16_t snapshot_id;
    uint8_t page_index;

    if (event->control.type != TAVRN_WIRE_SYNC_PULL || event->control.pdu_len != 22u) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    mentee = adva_from_bytes(&event->control.pdu[6]);
    mentor = adva_from_bytes(&event->control.pdu[12]);
    snapshot_id = pdu_u16(&event->control, 18u);
    page_index = event->control.pdu[20];
    if (!adva_equal(&mentor, &mentorship->gtt->config.local_identity) ||
        mentorship->serving_session_valid == 0u ||
        !adva_equal(&mentee, &mentorship->serving_mentee)) {
        return TAVRN_MENTORSHIP_OK;
    }
    if (time_due(now_ms, mentorship->serving_deadline_ms)) {
        clear_serving_session(mentorship);
        return TAVRN_MENTORSHIP_OK;
    }
    if (!adva_equal(&mentee, &event->transmitter.adva) ||
        snapshot_id != mentorship->serving_snapshot.snapshot_id ||
        (mentorship->serving_snapshot.count == 0u && page_index != 0u) ||
        (mentorship->serving_snapshot.count != 0u &&
         page_index >= mentorship->serving_snapshot.count)) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    if (tavrn_mentorship_build_sync_data(&mentorship->serving_snapshot, &mentor,
                                         &mentee, page_index, &data) !=
        TAVRN_MENTORSHIP_OK) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    mentorship->serving_deadline_ms = now_ms +
        mentorship->config.page_timeout_ms * mentorship->config.page_attempts +
        mentorship->config.offer_window_ms;
    return retain_control(mentorship, &data, 0u, TAVRN_MENTORSHIP_PENDING_DATA,
                          NULL, 0u);
}

static void purge_expired_joins(tavrn_mentorship_t *mentorship, uint32_t now_ms)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_MENTORSHIP_JOIN_DEDUPE_CAPACITY; index++) {
        tavrn_mentorship_join_key_t *key = &mentorship->joins[index];

        if (key->occupied != 0u && time_due(now_ms, key->expires_at_ms)) {
            memset(key, 0, sizeof(*key));
            mentorship->counters.join_dedupe_expired++;
        }
    }
}

static int join_order_before(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) < 0;
}

static uint32_t next_join_admission_order(tavrn_mentorship_t *mentorship)
{
    mentorship->next_join_admission_order++;
    if (mentorship->next_join_admission_order == 0u) {
        mentorship->next_join_admission_order++;
    }
    return mentorship->next_join_admission_order;
}

static int oldest_live_join_slot(const tavrn_mentorship_t *mentorship)
{
    int oldest = -1;
    uint8_t index;

    for (index = 0u; index < TAVRN_MENTORSHIP_JOIN_DEDUPE_CAPACITY; index++) {
        if (mentorship->joins[index].occupied == 0u) {
            continue;
        }
        if (oldest < 0 || join_order_before(
                              mentorship->joins[index].admission_order,
                              mentorship->joins[(uint8_t)oldest].admission_order)) {
            oldest = (int)index;
        }
    }
    return oldest;
}

static int join_seen(tavrn_mentorship_t *mentorship, const tavrn_adva_t *origin,
                     uint16_t sequence, uint32_t now_ms)
{
    uint8_t index;

    purge_expired_joins(mentorship, now_ms);
    for (index = 0u; index < TAVRN_MENTORSHIP_JOIN_DEDUPE_CAPACITY; index++) {
        if (mentorship->joins[index].occupied != 0u &&
            mentorship->joins[index].sequence == sequence &&
            adva_equal(&mentorship->joins[index].origin, origin)) {
            return 1;
        }
    }
    return 0;
}

static void remember_join(tavrn_mentorship_t *mentorship, const tavrn_adva_t *origin,
                          uint16_t sequence, uint32_t now_ms)
{
    uint8_t index;
    int slot = -1;

    purge_expired_joins(mentorship, now_ms);
    for (index = 0u; index < TAVRN_MENTORSHIP_JOIN_DEDUPE_CAPACITY; index++) {
        if (mentorship->joins[index].occupied == 0u) {
            slot = (int)index;
            break;
        }
    }
    if (slot < 0) {
        slot = oldest_live_join_slot(mentorship);
        if (slot < 0) {
            return;
        }
        mentorship->counters.join_dedupe_replaced++;
    }
    memset(&mentorship->joins[(uint8_t)slot], 0,
           sizeof(mentorship->joins[(uint8_t)slot]));
    mentorship->joins[(uint8_t)slot].occupied = 1u;
    mentorship->joins[(uint8_t)slot].origin = *origin;
    mentorship->joins[(uint8_t)slot].sequence = sequence;
    mentorship->joins[(uint8_t)slot].expires_at_ms =
        now_ms + mentorship->config.join_dedupe_ms;
    mentorship->joins[(uint8_t)slot].admission_order =
        next_join_admission_order(mentorship);
}

static tavrn_mentorship_status_t process_join(tavrn_mentorship_t *mentorship,
                                               const tavrn_rx_control_event_t *event,
                                               uint32_t now_ms)
{
    tavrn_adva_t origin;
    tavrn_adva_t subject;
    tavrn_mentorship_record_t record;
    tavrn_esc_context_match_t match;
    tavrn_esc_context_status_t context_status;
    tavrn_validated_control_t relay;
    uint16_t sequence;

    if (event->control.type != TAVRN_WIRE_TC_UPDATE || event->control.pdu_len != 24u) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    origin = adva_from_bytes(&event->control.pdu[7]);
    subject = adva_from_bytes(&event->control.pdu[15]);
    sequence = pdu_u16(&event->control, 13u);
    if (join_seen(mentorship, &origin, sequence, now_ms)) {
        mentorship->counters.join_duplicate++;
        return TAVRN_MENTORSHIP_OK;
    }
    if (pending_join_exists(mentorship, &origin, sequence)) {
        return TAVRN_MENTORSHIP_OK;
    }
    context_status = resolve_sid8(mentorship, subject.bytes[0], now_ms, &match);
    if (context_status == TAVRN_ESC_CONTEXT_UNIQUE &&
        !adva_equal(&match.identity, &subject)) {
        return enter_identity_conflict(mentorship, now_ms);
    }
    if (context_status == TAVRN_ESC_CONTEXT_COLLIDING ||
        context_status == TAVRN_ESC_CONTEXT_RESERVED) {
        return enter_identity_conflict(mentorship, now_ms);
    }
    relay = event->control;
    if ((relay.pdu[6] >> 4) != 0u && (relay.pdu[6] & 0x0fu) < 15u) {
        relay.pdu[6] = (uint8_t)(((relay.pdu[6] >> 4) - 1u) << 4) |
            (uint8_t)((relay.pdu[6] & 0x0fu) + 1u);
        return retain_join_obligation(mentorship, &relay,
                                      TAVRN_MENTORSHIP_PENDING_JOIN_RELAY,
                                      &origin, sequence);
    }
    memset(&record, 0, sizeof(record));
    record.identity = subject;
    record.ttl_bucket = 1u;
    record.hop_count = 1u;
    record.departed = event->control.pdu[21] == 1u;
    if (!gtt_observe_record(mentorship, &record, now_ms)) {
        return TAVRN_MENTORSHIP_BUSY;
    }
    remember_join(mentorship, &origin, sequence, now_ms);
    mentorship->counters.join_received++;
    return TAVRN_MENTORSHIP_OK;
}

static int is_full_identity_control_type(tavrn_wire_type_t type,
                                         const tavrn_validated_control_t *control)
{
    (void)control;
    return type == TAVRN_WIRE_HELLO || type == TAVRN_WIRE_SYNC_OFFER ||
        type == TAVRN_WIRE_SYNC_PULL || type == TAVRN_WIRE_SYNC_DATA ||
        type == TAVRN_WIRE_TC_UPDATE;
}

tavrn_mentorship_status_t tavrn_mentorship_handle_scheduler_event(
    tavrn_mentorship_t *mentorship, const ble_mesh_sched_event_t *event,
    uint32_t now_ms, tavrn_mentorship_event_trace_t *trace_out)
{
    tavrn_router_phase_trace_t router_trace;
    tavrn_router_event_status_t router_status;
    const tavrn_rx_control_event_t *control_event;
    tavrn_mentorship_status_t result = TAVRN_MENTORSHIP_OK;
    tavrn_mentorship_offer_t offer;

    if (trace_out != NULL) {
        memset(trace_out, 0, sizeof(*trace_out));
    }
    if (mentorship == NULL || mentorship->router == NULL || event == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    mentorship->last_now_ms = now_ms;
    mentorship->last_now_valid = 1u;
    purge_completed_sync_dedupe(mentorship, now_ms);
    /* Ordinary compressed routing is never allowed to mutate the common core
     * while a FULL node is collecting/repairing context. */
    if (event->type == BLE_MESH_SCHED_EVENT_RX_ADV && event->adv_len >= 12u &&
        mentorship->state.ordinary_traffic_gated != 0u &&
        !is_full_identity_control_type((tavrn_wire_type_t)event->adv_data[11], NULL)) {
        mentorship->counters.gated_data_aodv++;
        if (trace_out != NULL) {
            trace_out->router_result = TAVRN_ROUTER_EVENT_REJOINING;
        }
        return TAVRN_MENTORSHIP_GATED;
    }
    memset(&router_trace, 0, sizeof(router_trace));
    router_status = tavrn_router_handle_scheduler_event_ex(mentorship->router, event,
                                                             now_ms, &router_trace);
    if (trace_out != NULL) {
        trace_out->router_result = router_status;
        trace_out->wire_decode_result =
            router_trace.detail.scheduler_event.wire_decode_result;
        trace_out->wire_type = router_trace.detail.scheduler_event.decoded_frame_type;
        trace_out->link_event_type = router_trace.detail.scheduler_event.link_event_type;
        trace_out->reached_wire_link_router =
            router_trace.detail.scheduler_event.wire_decode_present ==
                    TAVRN_ROUTER_TRACE_PRESENT &&
            router_trace.detail.scheduler_event.link_step_present ==
                    TAVRN_ROUTER_TRACE_PRESENT &&
            router_trace.detail.scheduler_event.link_event_present ==
                    TAVRN_ROUTER_TRACE_PRESENT;
        if (router_trace.detail.scheduler_event.rx_control_present ==
            TAVRN_ROUTER_TRACE_PRESENT) {
            trace_out->rx_control = router_trace.detail.scheduler_event.rx_control;
            trace_out->rx_control_present = 1u;
        }
    }
    if (router_status == TAVRN_ROUTER_EVENT_INVALID) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    if (mentorship->identity_conflict_pending != 0u) {
        return enter_identity_conflict(mentorship, now_ms);
    }
    if (router_trace.detail.scheduler_event.rx_control_present !=
        TAVRN_ROUTER_TRACE_PRESENT) {
        return TAVRN_MENTORSHIP_OK;
    }
    control_event = &router_trace.detail.scheduler_event.rx_control;
    switch (control_event->control.type) {
    case TAVRN_WIRE_HELLO:
        result = process_hello(mentorship, control_event, now_ms);
        break;
    case TAVRN_WIRE_SYNC_OFFER:
        if (!adva_bytes_equal(&mentorship->gtt->config.local_identity,
                              &control_event->control.pdu[13])) {
            /* Advertisements are broadcast.  A complete offer for another
             * mentee is observable only for the existing local dampener. */
            if (mentorship->state.state == TAVRN_MENTORSHIP_SID8_ACTIVE) {
                memset(&offer, 0, sizeof(offer));
                offer.mentor = adva_from_bytes(&control_event->control.pdu[7]);
                offer.mentee = adva_from_bytes(&control_event->control.pdu[13]);
                offer.snapshot_count = control_event->control.pdu[19];
                offer.snapshot_id = pdu_u16(&control_event->control, 20u);
                offer.boot_nonce = pdu_u16(&control_event->control, 22u);
                (void)tavrn_mentorship_overhear_offer(mentorship, &offer, now_ms);
            }
            break;
        }
        {
            memset(&offer, 0, sizeof(offer));
            offer.mentor = adva_from_bytes(&control_event->control.pdu[7]);
            offer.mentee = adva_from_bytes(&control_event->control.pdu[13]);
            offer.snapshot_count = control_event->control.pdu[19];
            offer.snapshot_id = pdu_u16(&control_event->control, 20u);
            offer.boot_nonce = pdu_u16(&control_event->control, 22u);
            offer.rssi_magnitude_db = control_event->rssi_magnitude_db;
            if (mentorship->state.state == TAVRN_MENTORSHIP_REJOINING ||
                mentorship->state.state == TAVRN_MENTORSHIP_COLLECTING_OFFERS) {
                if (tavrn_mentorship_collect_offer(mentorship, &offer, now_ms) ==
                    TAVRN_MENTORSHIP_OFFER_INVALID) {
                    result = TAVRN_MENTORSHIP_INVALID;
                }
            }
        }
        break;
    case TAVRN_WIRE_SYNC_PULL:
        result = process_pull(mentorship, control_event, now_ms);
        break;
    case TAVRN_WIRE_SYNC_DATA:
        if (!adva_bytes_equal(&mentorship->gtt->config.local_identity,
                              &control_event->control.pdu[6]) ||
            mentorship->state.state != TAVRN_MENTORSHIP_SYNCING ||
            mentorship->page_session_valid == 0u) {
            /* A valid page may be heard after another mentee's exchange or
             * after this node has already left its session; neither is a
             * terminal local protocol error. */
            break;
        }
        result = tavrn_mentorship_ingest_sync_data(
            mentorship, &control_event->transmitter.adva, &control_event->control,
            now_ms);
        if (result == TAVRN_MENTORSHIP_OK) {
            if (mentorship->sync_complete_authorized != 0u) {
                /* Exact expected final page was committed; no public bypass. */
                if (tavrn_mentorship_activate_sid8(mentorship, now_ms) !=
                    TAVRN_MENTORSHIP_OK) {
                    result = TAVRN_MENTORSHIP_COLLISION;
                }
            } else {
                tavrn_validated_control_t pull;

                if (tavrn_mentorship_build_sync_pull(
                        &mentorship->gtt->config.local_identity,
                        &mentorship->state.selected_mentor,
                        mentorship->active_page_snapshot_id,
                        mentorship->state.sync_next_index, &pull) !=
                    TAVRN_MENTORSHIP_OK) {
                    result = TAVRN_MENTORSHIP_INVALID;
                } else {
                    result = retain_control(mentorship, &pull, 0u,
                                            TAVRN_MENTORSHIP_PENDING_PULL,
                                            NULL, 0u);
                }
            }
        }
        break;
    case TAVRN_WIRE_TC_UPDATE:
        /* The installed FULL maintenance binding owns generic TC validation,
         * GTT apply, dedupe, and relay.  Mentorship keeps the legacy bootstrap
         * JOIN path only when that binding is absent. */
        if (mentorship->tc_metadata == NULL) {
            result = process_join(mentorship, control_event, now_ms);
        }
        break;
    default:
        break;
    }
    return result;
}

tavrn_mentorship_status_t tavrn_mentorship_activate_sid8(
    tavrn_mentorship_t *mentorship, uint32_t now_ms)
{
    tavrn_direct_peer_t local_peer;
    uint8_t index;
    uint8_t self_bootstrap;
    uint8_t sync_complete;

    if (mentorship == NULL || mentorship->router == NULL || mentorship->gtt == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    self_bootstrap = (mentorship->state.state == TAVRN_MENTORSHIP_REJOINING ||
                      mentorship->state.state == TAVRN_MENTORSHIP_COLLECTING_OFFERS) &&
        mentorship->state.active_offer_count == 0u &&
        time_due(now_ms, mentorship->started_at_ms + mentorship->config.self_bootstrap_ms);
    sync_complete = mentorship->state.state == TAVRN_MENTORSHIP_SYNCING &&
        mentorship->sync_complete_authorized != 0u;
    if (self_bootstrap == 0u && sync_complete == 0u) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &mentorship->gtt->storage->entries[index];
        tavrn_esc_context_match_t match;

        if (entry->occupied == 0u || entry->departed != 0u ||
            tombstone_is_purged(entry, now_ms)) {
            continue;
        }
        if (resolve_sid8(mentorship, entry->identity.bytes[0], now_ms, &match) !=
            TAVRN_ESC_CONTEXT_UNIQUE) {
            return enter_identity_conflict(mentorship, now_ms);
        }
    }
    memset(&local_peer, 0, sizeof(local_peer));
    local_peer.adva = mentorship->gtt->config.local_identity;
    local_peer.logical_id.width = TAVRN_IDENTITY_SID8;
    local_peer.logical_id.value = local_peer.adva.bytes[0];
    if (tavrn_router_reconfigure_identity(mentorship->router, &local_peer, now_ms) !=
            TAVRN_ROUTER_INCARNATION_OK ||
        tavrn_router_complete_full_bootstrap(mentorship->router, now_ms) !=
            TAVRN_ROUTER_INCARNATION_OK) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    clear_receiving_session(mentorship);
    mentorship->state.state = TAVRN_MENTORSHIP_SID8_ACTIVE;
    mentorship->state.active_width = TAVRN_IDENTITY_SID8;
    mentorship->state.ordinary_traffic_gated = 0u;
    mentorship->state.full_bootstrap_admission_enabled = 1u;
    mentorship->counters.transition_cleared++;
    originate_join(mentorship);
    return TAVRN_MENTORSHIP_OK;
}

tavrn_mentorship_status_t tavrn_mentorship_recover_sid16(
    tavrn_mentorship_t *mentorship, uint32_t now_ms)
{
    tavrn_direct_peer_t local_peer;

    if (mentorship == NULL || mentorship->router == NULL || mentorship->gtt == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    memset(&local_peer, 0, sizeof(local_peer));
    local_peer.adva = mentorship->gtt->config.local_identity;
    local_peer.logical_id.width = TAVRN_IDENTITY_SID16;
    local_peer.logical_id.value = (uint16_t)local_peer.adva.bytes[0] |
        ((uint16_t)local_peer.adva.bytes[1] << 8);
    if (tavrn_router_reconfigure_identity(mentorship->router, &local_peer, now_ms) !=
        TAVRN_ROUTER_INCARNATION_OK) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    mentorship->state.state = TAVRN_MENTORSHIP_REJOINING;
    mentorship->state.active_width = TAVRN_IDENTITY_SID16;
    mentorship->state.ordinary_traffic_gated = 1u;
    mentorship->started_at_ms = now_ms;
    mentorship->join_reannounce_deadline_ms = 0u;
    mentorship->join_reannounce_valid = 0u;
    clear_receiving_session(mentorship);
    clear_serving_session(mentorship);
    memset(&mentorship->pending_control, 0, sizeof(mentorship->pending_control));
    memset(mentorship->join_obligations, 0, sizeof(mentorship->join_obligations));
    mentorship->counters.transition_cleared++;
    return TAVRN_MENTORSHIP_OK;
}

tavrn_mentorship_status_t tavrn_mentorship_tick(
    tavrn_mentorship_t *mentorship, uint32_t now_ms)
{
    tavrn_validated_control_t control;
    tavrn_mentorship_offer_t selected;

    if (mentorship == NULL || mentorship->router == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    mentorship->last_now_ms = now_ms;
    mentorship->last_now_valid = 1u;
    purge_completed_sync_dedupe(mentorship, now_ms);
    if (mentorship->serving_session_valid != 0u &&
        time_due(now_ms, mentorship->serving_deadline_ms)) {
        clear_serving_session(mentorship);
    }
    if (mentorship->pending_control.valid != 0u) {
        return flush_pending_control(mentorship, now_ms);
    }
    {
        uint8_t index;

        for (index = 0u; index < TAVRN_MENTORSHIP_JOIN_OBLIGATION_CAPACITY;
             index++) {
            if (mentorship->join_obligations[index].valid != 0u) {
                return flush_pending_join_obligation(mentorship, now_ms);
            }
        }
    }
    if (mentorship->pending_offer_valid != 0u &&
        time_due(now_ms, mentorship->pending_offer_due_ms) &&
        mentorship->pending_control.valid == 0u && mentorship->serving_session_valid != 0u) {
        if (tavrn_mentorship_build_sync_offer(
                &mentorship->serving_snapshot, &mentorship->gtt->config.local_identity,
                &mentorship->serving_mentee, mentorship->serving_boot_nonce,
                &control) != TAVRN_MENTORSHIP_OK) {
            return TAVRN_MENTORSHIP_INVALID;
        }
        return retain_control(mentorship, &control, 0u,
                              TAVRN_MENTORSHIP_PENDING_OFFER, NULL, 0u);
    }
    if (mentorship->state.state == TAVRN_MENTORSHIP_COLLECTING_OFFERS &&
        time_due(now_ms, mentorship->offer_window_deadline_ms) &&
        mentorship->pending_control.valid == 0u) {
        memset(&selected, 0, sizeof(selected));
        if (tavrn_mentorship_select_offer(mentorship, now_ms, &selected) !=
            TAVRN_MENTORSHIP_OK ||
            tavrn_mentorship_build_sync_pull(
                &mentorship->gtt->config.local_identity, &selected.mentor,
                selected.snapshot_id, 0u, &control) != TAVRN_MENTORSHIP_OK) {
            return TAVRN_MENTORSHIP_INVALID;
        }
        return retain_control(mentorship, &control, 0u,
                              TAVRN_MENTORSHIP_PENDING_PULL, NULL, 0u);
    }
    if (mentorship->state.state == TAVRN_MENTORSHIP_SYNCING &&
        mentorship->page_deadline_valid != 0u &&
        time_due(now_ms, mentorship->page_deadline_ms) &&
        mentorship->pending_control.valid == 0u) {
        if (mentorship->state.pull_attempts >= mentorship->config.page_attempts) {
            mentorship->state.state = TAVRN_MENTORSHIP_REJOINING;
            mentorship->state.restart_count++;
            mentorship->started_at_ms = now_ms;
            clear_receiving_session(mentorship);
            return TAVRN_MENTORSHIP_RESTARTED;
        }
        if (tavrn_mentorship_build_sync_pull(
                &mentorship->gtt->config.local_identity,
                &mentorship->state.selected_mentor,
                mentorship->active_page_snapshot_id,
                mentorship->state.sync_next_index, &control) != TAVRN_MENTORSHIP_OK) {
            return TAVRN_MENTORSHIP_INVALID;
        }
        return retain_control(mentorship, &control, 0u,
                              TAVRN_MENTORSHIP_PENDING_PULL, NULL, 0u);
    }
    if ((mentorship->state.state == TAVRN_MENTORSHIP_REJOINING ||
         mentorship->state.state == TAVRN_MENTORSHIP_COLLECTING_OFFERS) &&
        mentorship->state.active_offer_count == 0u &&
        time_due(now_ms, mentorship->started_at_ms + mentorship->config.self_bootstrap_ms)) {
        tavrn_mentorship_status_t activation =
            tavrn_mentorship_activate_sid8(mentorship, now_ms);

        if (activation != TAVRN_MENTORSHIP_OK) {
            return activation;
        }
        return TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED;
    }
    /* Full maintenance owns the one shared TC sequence stream.  An unbound
     * legacy bootstrap fixture retains its one initial JOIN only; it cannot
     * safely mint fresh periodic TC UUIDs.  A pending JOIN is flushed above,
     * before this branch, so it always retains exact bytes/sequence across
     * busy backpressure.  The existing UUID retention is longer than TC
     * subject suppression and well below remote soft expiry in every profile. */
    if (mentorship->state.state == TAVRN_MENTORSHIP_SID8_ACTIVE &&
        mentorship->tc_metadata != NULL &&
        mentorship->join_reannounce_valid != 0u &&
        time_due(now_ms, mentorship->join_reannounce_deadline_ms)) {
        originate_join(mentorship);
    }
    return TAVRN_MENTORSHIP_OK;
}

aodv_status_t tavrn_mentorship_submit_application(
    tavrn_mentorship_t *mentorship, const tron_application_data_t *data,
    uint32_t now_ms)
{
    if (mentorship == NULL || mentorship->router == NULL || data == NULL) {
        return AODV_STATUS_INVALID;
    }
    if (mentorship->state.ordinary_traffic_gated != 0u) {
        mentorship->counters.gated_data_aodv++;
        return AODV_STATUS_REJOINING;
    }
    return tavrn_router_submit_application(mentorship->router, data, now_ms);
}

tavrn_mentorship_status_t tavrn_mentorship_dispatch(
    tavrn_mentorship_t *mentorship, uint32_t now_ms,
    tavrn_router_phase_trace_t *trace_out)
{
    tavrn_router_event_status_t status;

    if (mentorship == NULL || mentorship->router == NULL || trace_out == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    if (mentorship->state.ordinary_traffic_gated != 0u) {
        memset(trace_out, 0, sizeof(*trace_out));
        trace_out->phase = TAVRN_ROUTER_TRACE_DISPATCH;
        trace_out->completed_at_ms = now_ms;
        trace_out->detail.dispatch.status = TAVRN_ROUTER_EVENT_REJOINING;
        trace_out->detail.dispatch.dispatch_event.type =
            TAVRN_ROUTER_DISPATCH_EVENT_NONE;
        return TAVRN_MENTORSHIP_OK;
    }
    status = tavrn_router_dispatch_trace_ex(mentorship->router, now_ms, trace_out);
    return status == TAVRN_ROUTER_EVENT_INVALID ? TAVRN_MENTORSHIP_INVALID :
                                                  TAVRN_MENTORSHIP_OK;
}

tavrn_mentorship_status_t tavrn_mentorship_build_join(
    tavrn_mentorship_t *mentorship, uint16_t tc_sequence,
    tavrn_validated_control_t *control_out)
{
    return build_join_for_local(mentorship, tc_sequence, control_out);
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
