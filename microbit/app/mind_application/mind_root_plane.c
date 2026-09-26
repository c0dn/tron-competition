#include "mind_root_plane.h"

#include <limits.h>
#include <string.h>

static void increment_saturating(uint32_t *value)
{
    if (value != NULL && *value != UINT32_MAX) {
        (*value)++;
    }
}

static int adva_equal(const tavrn_adva_t *left, const tavrn_adva_t *right)
{
    return left != NULL && right != NULL &&
        memcmp(left->bytes, right->bytes, TAVRN_ADVA_LEN) == 0;
}

static int generation_is_newer(uint16_t candidate, uint16_t current)
{
    uint16_t distance = (uint16_t)(candidate - current);

    return distance != 0u && distance < 0x8000u;
}

static int root_state_equal(const mind_application_root_state_t *left,
                            const mind_application_root_state_t *right)
{
    return left != NULL && right != NULL && left->active == right->active &&
        left->nonce == right->nonce && left->generation == right->generation;
}

static int time_due(uint32_t now_ms, uint32_t due_at_ms)
{
    return (int32_t)(now_ms - due_at_ms) >= 0;
}

static uint32_t initial_due(const tavrn_adva_t *identity, uint16_t generation,
                            uint32_t now_ms)
{
    uint32_t hash = generation;
    uint8_t index;

    for (index = 0u; index < TAVRN_ADVA_LEN; index++) {
        hash = hash * 33u + identity->bytes[index];
    }
    return now_ms + MIND_APP_ROOT_INITIAL_DUE_MIN_MS +
        hash % (MIND_APP_ROOT_INITIAL_DUE_MAX_MS -
                MIND_APP_ROOT_INITIAL_DUE_MIN_MS + 1u);
}

static mind_root_entry_t *find_root(mind_root_plane_t *plane,
                                    const tavrn_adva_t *identity)
{
    uint8_t index;

    if (plane == NULL || identity == NULL) {
        return NULL;
    }
    for (index = 0u; index < MIND_APP_ROOT_CAPACITY; index++) {
        if (plane->roots[index].occupied != 0u &&
            adva_equal(&plane->roots[index].canonical_adva, identity)) {
            return &plane->roots[index];
        }
    }
    return NULL;
}

static mind_root_entry_t *find_empty_root(mind_root_plane_t *plane)
{
    uint8_t index;

    for (index = 0u; index < MIND_APP_ROOT_CAPACITY; index++) {
        if (plane->roots[index].occupied == 0u) {
            return &plane->roots[index];
        }
    }
    return NULL;
}

static mind_root_campaign_target_t *find_campaign(
    mind_root_plane_t *plane, const tavrn_adva_t *identity)
{
    uint8_t index;

    for (index = 0u; index < MIND_APP_CAMPAIGN_CAPACITY; index++) {
        if (plane->campaigns[index].occupied != 0u &&
            adva_equal(&plane->campaigns[index].canonical_adva, identity)) {
            return &plane->campaigns[index];
        }
    }
    return NULL;
}

static void reset_campaigns(mind_root_plane_t *plane, uint32_t now_ms)
{
    uint8_t index;
    mind_application_root_state_t local;

    if (plane == NULL || plane->local_index >= MIND_APP_ROOT_CAPACITY) {
        return;
    }
    local = plane->roots[plane->local_index].current;
    for (index = 0u; index < MIND_APP_CAMPAIGN_CAPACITY; index++) {
        mind_root_campaign_target_t *target = &plane->campaigns[index];

        if (target->occupied == 0u) {
            continue;
        }
        target->due_at_ms = initial_due(&target->canonical_adva,
                                        local.generation, now_ms);
        target->last_submit_ms = 0u;
        target->completed = 0u;
        target->rejected = 0u;
        target->resolver_blocked = 0u;
        target->resolver_failed_closed = 0u;
    }
}

void mind_root_plane_observe_target(mind_root_plane_t *plane,
                                    const tavrn_adva_t *identity,
                                    uint32_t now_ms)
{
    uint8_t index;
    mind_root_campaign_target_t *target;
    mind_application_root_state_t local;

    if (plane == NULL || identity == NULL ||
        adva_equal(identity, &plane->local_adva)) {
        return;
    }
    target = find_campaign(plane, identity);
    if (target != NULL) {
        if (target->resolver_blocked != 0u) {
            target->resolver_blocked = 0u;
            target->due_at_ms = now_ms;
        }
        return;
    }
    for (index = 0u; index < MIND_APP_CAMPAIGN_CAPACITY; index++) {
        if (plane->campaigns[index].occupied == 0u) {
            target = &plane->campaigns[index];
            memset(target, 0, sizeof(*target));
            target->canonical_adva = *identity;
            target->occupied = 1u;
            local = plane->roots[plane->local_index].current;
            target->due_at_ms = initial_due(identity, local.generation, now_ms);
            return;
        }
    }
    increment_saturating(&plane->counters.campaign_deferred);
}

static void clear_departed(mind_root_plane_t *plane, const tavrn_adva_t *identity)
{
    uint8_t index;
    mind_root_entry_t *root;
    mind_root_campaign_target_t *target;

    if (plane == NULL || identity == NULL || adva_equal(identity, &plane->local_adva)) {
        return;
    }
    root = find_root(plane, identity);
    if (root != NULL && root->local == 0u) {
        memset(root, 0, sizeof(*root));
        increment_saturating(&plane->counters.departed_cleared);
    }
    target = find_campaign(plane, identity);
    if (target != NULL) {
        memset(target, 0, sizeof(*target));
    }
    for (index = 0u; index < MIND_APP_ROOT_ACK_CAPACITY; index++) {
        if (plane->acknowledgements[index].occupied != 0u &&
            adva_equal(&plane->acknowledgements[index].canonical_adva, identity)) {
            memset(&plane->acknowledgements[index], 0,
                   sizeof(plane->acknowledgements[index]));
        }
    }
}

static void queue_ack(mind_root_plane_t *plane, const tavrn_adva_t *identity,
                      const mind_application_root_state_t *state,
                      mind_root_apply_status_t result)
{
    uint8_t index;

    if (plane == NULL || identity == NULL || state == NULL) {
        return;
    }
    for (index = 0u; index < MIND_APP_ROOT_ACK_CAPACITY; index++) {
        mind_root_ack_obligation_t *ack = &plane->acknowledgements[index];

        if (ack->occupied == 0u) {
            memset(ack, 0, sizeof(*ack));
            ack->canonical_adva = *identity;
            ack->ack.state = *state;
            ack->ack.status = (mind_application_root_ack_status_t)result;
            ack->occupied = 1u;
            return;
        }
    }
    increment_saturating(&plane->counters.ack_queue_full);
}

void mind_root_plane_init(mind_root_plane_t *plane,
                          const tavrn_adva_t *local_adva,
                          uint16_t router_boot_nonce)
{
    mind_root_entry_t *local;

    if (plane == NULL || local_adva == NULL || router_boot_nonce == 0u) {
        return;
    }
    memset(plane, 0, sizeof(*plane));
    plane->local_adva = *local_adva;
    plane->local_index = 0u;
    local = &plane->roots[plane->local_index];
    local->canonical_adva = *local_adva;
    local->current.active = 0u;
    local->current.nonce = router_boot_nonce;
    local->current.generation = 1u;
    local->occupied = 1u;
    local->local = 1u;
    mind_topology_adapter_init(&plane->topology);
}

mind_root_apply_status_t mind_root_plane_set_local(mind_root_plane_t *plane,
                                                    uint8_t active,
                                                    uint32_t now_ms)
{
    mind_root_entry_t *local;

    if (plane == NULL || active > 1u || plane->local_index >= MIND_APP_ROOT_CAPACITY) {
        return MIND_ROOT_APPLY_INVALID;
    }
    local = &plane->roots[plane->local_index];
    if (local->occupied == 0u || local->local == 0u) {
        return MIND_ROOT_APPLY_INVALID;
    }
    if (local->current.active == active) {
        reset_campaigns(plane, now_ms);
        increment_saturating(&plane->counters.duplicate);
        return MIND_ROOT_APPLY_DUPLICATE;
    }
    local->current.generation++;
    if (local->current.generation == 0u) {
        local->current.generation = 1u;
    }
    local->current.active = active;
    reset_campaigns(plane, now_ms);
    increment_saturating(&plane->counters.accepted);
    return MIND_ROOT_APPLY_ACCEPTED;
}

mind_application_root_state_t mind_root_plane_local_state(
    const mind_root_plane_t *plane)
{
    mind_application_root_state_t state;

    memset(&state, 0, sizeof(state));
    if (plane != NULL && plane->local_index < MIND_APP_ROOT_CAPACITY &&
        plane->roots[plane->local_index].occupied != 0u) {
        state = plane->roots[plane->local_index].current;
    }
    return state;
}

uint8_t mind_root_plane_local_active(const mind_root_plane_t *plane)
{
    return mind_root_plane_local_state(plane).active;
}

int mind_root_plane_apply_topology(mind_root_plane_t *plane,
                                   const routed_cycle_gtt_snapshot_t *snapshot)
{
    uint8_t index;

    if (plane == NULL || !mind_topology_adapter_copy_snapshot(&plane->topology, snapshot)) {
        return 0;
    }
    for (index = 0u; index < plane->topology.entry_count; index++) {
        const mind_topology_entry_t *entry = &plane->topology.entries[index];

        if (entry->departed == ROUTED_CYCLE_BOOLEAN_TRUE) {
            clear_departed(plane, &entry->canonical_adva);
        } else {
            mind_root_plane_observe_target(plane, &entry->canonical_adva,
                                            plane->topology.query_at_ms);
        }
    }
    /* An OK topology observation is the rediscovery signal for unresolved ACK
     * obligations.  It does not erase absent identities. */
    for (index = 0u; index < MIND_APP_ROOT_ACK_CAPACITY; index++) {
        if (plane->acknowledgements[index].occupied != 0u &&
            plane->acknowledgements[index].resolver_blocked != 0u) {
            plane->acknowledgements[index].resolver_blocked = 0u;
        }
    }
    return 1;
}

void mind_root_plane_note_snapshot_failure(mind_root_plane_t *plane)
{
    if (plane != NULL) {
        mind_topology_adapter_note_snapshot_failure(&plane->topology);
        increment_saturating(&plane->counters.snapshot_failures);
    }
}

void mind_root_plane_set_sid8_ready(mind_root_plane_t *plane, uint8_t ready,
                                    uint32_t now_ms)
{
    if (plane == NULL) {
        return;
    }
    if (ready != 0u && plane->sid8_ready == 0u) {
        plane->sid8_ready = 1u;
        reset_campaigns(plane, now_ms);
    } else if (ready == 0u) {
        plane->sid8_ready = 0u;
    }
}

mind_root_apply_status_t mind_root_plane_receive_root_state(
    mind_root_plane_t *plane, const tavrn_adva_t *origin,
    const mind_application_root_state_t *state)
{
    mind_root_entry_t *entry;
    mind_root_apply_status_t result;

    if (plane == NULL || origin == NULL || state == NULL || state->active > 1u ||
        state->nonce == 0u || state->generation == 0u) {
        return MIND_ROOT_APPLY_INVALID;
    }
    entry = find_root(plane, origin);
    if (entry == NULL) {
        entry = find_empty_root(plane);
        if (entry == NULL) {
            increment_saturating(&plane->counters.capacity_rejected);
            result = MIND_ROOT_APPLY_CAPACITY_REJECTED;
            queue_ack(plane, origin, state, result);
            return result;
        }
        memset(entry, 0, sizeof(*entry));
        entry->canonical_adva = *origin;
        entry->current = *state;
        entry->occupied = 1u;
        increment_saturating(&plane->counters.accepted);
        result = MIND_ROOT_APPLY_ACCEPTED;
        queue_ack(plane, origin, state, result);
        return result;
    }
    if (entry->local != 0u) {
        increment_saturating(&plane->counters.stale);
        result = MIND_ROOT_APPLY_STALE;
        queue_ack(plane, origin, state, result);
        return result;
    }
    if (state->nonce == entry->prior_nonce) {
        increment_saturating(&plane->counters.stale);
        result = MIND_ROOT_APPLY_STALE;
    } else if (state->nonce != entry->current.nonce) {
        entry->prior_nonce = entry->current.nonce;
        entry->current = *state;
        increment_saturating(&plane->counters.accepted);
        result = MIND_ROOT_APPLY_ACCEPTED;
    } else if (state->generation == entry->current.generation) {
        if (state->active == entry->current.active) {
            increment_saturating(&plane->counters.duplicate);
            result = MIND_ROOT_APPLY_DUPLICATE;
        } else {
            increment_saturating(&plane->counters.stale);
            result = MIND_ROOT_APPLY_STALE;
        }
    } else if (generation_is_newer(state->generation, entry->current.generation)) {
        entry->current = *state;
        increment_saturating(&plane->counters.accepted);
        result = MIND_ROOT_APPLY_ACCEPTED;
    } else {
        increment_saturating(&plane->counters.stale);
        result = MIND_ROOT_APPLY_STALE;
    }
    queue_ack(plane, origin, state, result);
    return result;
}

void mind_root_plane_receive_root_ack(mind_root_plane_t *plane,
                                      const tavrn_adva_t *origin,
                                      const mind_application_root_ack_t *ack)
{
    uint8_t index;
    mind_application_root_state_t local;

    if (plane == NULL || origin == NULL || ack == NULL) {
        return;
    }
    local = mind_root_plane_local_state(plane);
    for (index = 0u; index < MIND_APP_CAMPAIGN_CAPACITY; index++) {
        mind_root_campaign_target_t *target = &plane->campaigns[index];

        if (target->occupied == 0u || !adva_equal(&target->canonical_adva, origin)) {
            continue;
        }
        if (target->completed != 0u || target->rejected != 0u) {
            increment_saturating(&plane->counters.ack_late);
            return;
        }
        if (!root_state_equal(&ack->state, &local)) {
            increment_saturating(&plane->counters.ack_unmatched);
            return;
        }
        if (ack->status == MIND_APPLICATION_ROOT_ACK_ACCEPTED ||
            ack->status == MIND_APPLICATION_ROOT_ACK_DUPLICATE) {
            target->completed = 1u;
        } else {
            target->rejected = 1u;
        }
        return;
    }
    increment_saturating(&plane->counters.ack_unmatched);
}

int mind_root_plane_prepare_ack_submission(
    mind_root_plane_t *plane, uint32_t now_ms,
    mind_root_submission_t *submission_out)
{
    uint8_t index;

    (void)now_ms;
    if (plane == NULL || submission_out == NULL || plane->sid8_ready == 0u) {
        return 0;
    }
    memset(submission_out, 0, sizeof(*submission_out));
    for (index = 0u; index < MIND_APP_ROOT_ACK_CAPACITY; index++) {
        mind_root_ack_obligation_t *ack = &plane->acknowledgements[index];

        if (ack->occupied == 0u || ack->resolver_blocked != 0u ||
            ack->resolver_failed_closed != 0u) {
            continue;
        }
        if (mind_application_wire_pack_root_ack(&submission_out->record,
                                                 &ack->ack) !=
            MIND_APPLICATION_WIRE_OK) {
            ack->resolver_failed_closed = 1u;
            continue;
        }
        submission_out->target = ack->canonical_adva;
        submission_out->slot = index;
        submission_out->kind = MIND_ROOT_SUBMISSION_ACK;
        return 1;
    }
    return 0;
}

int mind_root_plane_prepare_state_submission(
    mind_root_plane_t *plane, uint32_t now_ms,
    mind_root_submission_t *submission_out)
{
    uint8_t index;
    mind_application_root_state_t local;

    if (plane == NULL || submission_out == NULL || plane->sid8_ready == 0u) {
        return 0;
    }
    memset(submission_out, 0, sizeof(*submission_out));
    if (plane->root_state_submission_seen != 0u &&
        (uint32_t)(now_ms - plane->last_root_state_invoked_ms) <
            MIND_APP_ROOT_SUBMIT_SPACING_MS) {
        return 0;
    }
    local = mind_root_plane_local_state(plane);
    for (index = 0u; index < MIND_APP_CAMPAIGN_CAPACITY; index++) {
        mind_root_campaign_target_t *target = &plane->campaigns[index];

        if (target->occupied == 0u || target->completed != 0u ||
            target->rejected != 0u || target->resolver_blocked != 0u ||
            target->resolver_failed_closed != 0u || !time_due(now_ms, target->due_at_ms)) {
            continue;
        }
        if (mind_application_wire_pack_root_state(&submission_out->record, &local) !=
            MIND_APPLICATION_WIRE_OK) {
            target->resolver_failed_closed = 1u;
            continue;
        }
        submission_out->target = target->canonical_adva;
        submission_out->slot = index;
        submission_out->kind = MIND_ROOT_SUBMISSION_STATE;
        return 1;
    }
    return 0;
}

int mind_root_plane_prepare_submission(mind_root_plane_t *plane,
                                        uint32_t now_ms,
                                        mind_root_submission_t *submission_out)
{
    return mind_root_plane_prepare_ack_submission(plane, now_ms, submission_out) ||
        mind_root_plane_prepare_state_submission(plane, now_ms, submission_out);
}

int mind_root_plane_submission_resolved(mind_root_plane_t *plane,
                                        const mind_root_submission_t *submission,
                                        mind_root_resolver_status_t status,
                                        uint32_t now_ms)
{
    uint8_t *blocked;
    uint8_t *failed_closed;

    (void)now_ms;
    if (plane == NULL || submission == NULL ||
        (submission->kind != MIND_ROOT_SUBMISSION_ACK &&
         submission->kind != MIND_ROOT_SUBMISSION_STATE)) {
        return 0;
    }
    if (status == MIND_ROOT_RESOLVER_UNIQUE) {
        return 1;
    }
    if (submission->kind == MIND_ROOT_SUBMISSION_ACK) {
        if (submission->slot >= MIND_APP_ROOT_ACK_CAPACITY ||
            plane->acknowledgements[submission->slot].occupied == 0u) {
            return 0;
        }
        blocked = &plane->acknowledgements[submission->slot].resolver_blocked;
        failed_closed = &plane->acknowledgements[submission->slot].resolver_failed_closed;
    } else {
        if (submission->slot >= MIND_APP_CAMPAIGN_CAPACITY ||
            plane->campaigns[submission->slot].occupied == 0u) {
            return 0;
        }
        blocked = &plane->campaigns[submission->slot].resolver_blocked;
        failed_closed = &plane->campaigns[submission->slot].resolver_failed_closed;
    }
    if (status == MIND_ROOT_RESOLVER_UNKNOWN) {
        *blocked = 1u;
        increment_saturating(&plane->counters.resolver_unknown);
    } else if (status == MIND_ROOT_RESOLVER_COLLIDING) {
        *blocked = 1u;
        increment_saturating(&plane->counters.resolver_colliding);
    } else if (status == MIND_ROOT_RESOLVER_RESERVED) {
        *failed_closed = 1u;
        increment_saturating(&plane->counters.resolver_reserved);
    } else {
        *failed_closed = 1u;
        increment_saturating(&plane->counters.resolver_invalid);
    }
    return 0;
}

void mind_root_plane_note_incoming_resolver(mind_root_plane_t *plane,
                                            mind_root_resolver_status_t status)
{
    if (plane == NULL) {
        return;
    }
    if (status == MIND_ROOT_RESOLVER_UNKNOWN) {
        increment_saturating(&plane->counters.resolver_unknown);
    } else if (status == MIND_ROOT_RESOLVER_COLLIDING) {
        increment_saturating(&plane->counters.resolver_colliding);
    } else if (status == MIND_ROOT_RESOLVER_RESERVED) {
        increment_saturating(&plane->counters.resolver_reserved);
    } else if (status == MIND_ROOT_RESOLVER_INVALID) {
        increment_saturating(&plane->counters.resolver_invalid);
    }
}

void mind_root_plane_submission_invoked(mind_root_plane_t *plane,
                                        const mind_root_submission_t *submission,
                                        uint32_t now_ms)
{
    if (plane == NULL || submission == NULL ||
        submission->kind != MIND_ROOT_SUBMISSION_STATE ||
        submission->slot >= MIND_APP_CAMPAIGN_CAPACITY ||
        plane->campaigns[submission->slot].occupied == 0u) {
        return;
    }
    plane->last_root_state_invoked_ms = now_ms;
    plane->root_state_submission_seen = 1u;
    increment_saturating(&plane->counters.root_state_submit_invoked);
}

void mind_root_plane_submission_complete(mind_root_plane_t *plane,
                                         const mind_root_submission_t *submission,
                                         uint8_t accepted, uint32_t now_ms)
{
    if (plane == NULL || submission == NULL) {
        return;
    }
    if (submission->kind == MIND_ROOT_SUBMISSION_ACK) {
        if (submission->slot >= MIND_APP_ROOT_ACK_CAPACITY ||
            plane->acknowledgements[submission->slot].occupied == 0u) {
            return;
        }
        if (accepted != 0u) {
            memset(&plane->acknowledgements[submission->slot], 0,
                   sizeof(plane->acknowledgements[submission->slot]));
            increment_saturating(&plane->counters.submissions);
        } else {
            increment_saturating(&plane->counters.submission_busy);
        }
        return;
    }
    if (submission->kind != MIND_ROOT_SUBMISSION_STATE ||
        submission->slot >= MIND_APP_CAMPAIGN_CAPACITY ||
        plane->campaigns[submission->slot].occupied == 0u) {
        return;
    }
    if (accepted != 0u) {
        mind_root_campaign_target_t *target = &plane->campaigns[submission->slot];

        target->last_submit_ms = now_ms;
        target->due_at_ms = now_ms + MIND_APP_ROOT_ACK_RETRY_MS;
        increment_saturating(&plane->counters.submissions);
    } else {
        /* The generic submit call happened, so preserve global pacing while
         * allowing another simultaneously due target to make progress first. */
        plane->campaigns[submission->slot].due_at_ms =
            now_ms + MIND_APP_ROOT_SUBMIT_SPACING_MS + 1u;
        increment_saturating(&plane->counters.submission_busy);
    }
}

void mind_root_plane_status(const mind_root_plane_t *plane,
                            mind_root_plane_status_t *status_out)
{
    uint8_t index;

    if (status_out == NULL) {
        return;
    }
    memset(status_out, 0, sizeof(*status_out));
    if (plane == NULL) {
        return;
    }
    for (index = 0u; index < MIND_APP_ROOT_CAPACITY; index++) {
        if (plane->roots[index].occupied != 0u &&
            plane->roots[index].current.active != 0u) {
            status_out->active_roots++;
        }
    }
    for (index = 0u; index < MIND_APP_CAMPAIGN_CAPACITY; index++) {
        const mind_root_campaign_target_t *target = &plane->campaigns[index];

        if (target->occupied == 0u) {
            continue;
        }
        if (target->last_submit_ms != 0u) {
            status_out->announced++;
        }
        if (target->completed != 0u) {
            status_out->acked++;
        } else if (target->rejected != 0u) {
            status_out->rejected++;
        } else {
            status_out->pending++;
        }
    }
}

uint8_t mind_root_plane_active_count(const mind_root_plane_t *plane)
{
    mind_root_plane_status_t status;

    mind_root_plane_status(plane, &status);
    return status.active_roots;
}

uint8_t mind_root_plane_snapshot_active_roots(
    const mind_root_plane_t *plane, tavrn_adva_t roots_out[MIND_APP_ROOT_CAPACITY])
{
    uint8_t index;
    uint8_t count = 0u;

    if (plane == NULL || roots_out == NULL) {
        return 0u;
    }
    for (index = 0u; index < MIND_APP_ROOT_CAPACITY; index++) {
        const mind_root_entry_t *root = &plane->roots[index];

        if (root->occupied != 0u && root->current.active != 0u) {
            roots_out[count] = root->canonical_adva;
            count++;
        }
    }
    return count;
}
