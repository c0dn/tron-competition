#include "mind_event_forwarder.h"

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

static void advance_cursor(mind_event_forwarder_t *forwarder,
                            uint8_t event_slot, uint8_t target_slot)
{
    mind_event_item_t *item;

    if (forwarder == NULL || event_slot >= MIND_APP_EVENT_CAPACITY) {
        return;
    }
    forwarder->event_cursor = (uint8_t)((event_slot + 1u) %
                                         MIND_APP_EVENT_CAPACITY);
    item = &forwarder->events[event_slot];
    if (item->occupied != 0u && item->target_count != 0u &&
        target_slot < item->target_count) {
        item->target_cursor = (uint8_t)((target_slot + 1u) % item->target_count);
    }
}

static int item_is_finished(const mind_event_item_t *item)
{
    uint8_t index;

    if (item == NULL || item->occupied == 0u) {
        return 0;
    }
    for (index = 0u; index < item->target_count; index++) {
        if (item->targets[index].state == MIND_EVENT_TARGET_PENDING ||
            item->targets[index].resolver_failed_closed != 0u) {
            return 0;
        }
    }
    return 1;
}

static void retire_if_finished(mind_event_forwarder_t *forwarder,
                               uint8_t event_slot)
{
    if (forwarder == NULL || event_slot >= MIND_APP_EVENT_CAPACITY ||
        !item_is_finished(&forwarder->events[event_slot])) {
        return;
    }
    memset(&forwarder->events[event_slot], 0, sizeof(forwarder->events[event_slot]));
    increment_saturating(&forwarder->counters.completed_events);
}

static mind_event_item_t *find_empty_event(mind_event_forwarder_t *forwarder,
                                           uint8_t *slot_out)
{
    uint8_t index;

    if (forwarder == NULL) {
        return NULL;
    }
    for (index = 0u; index < MIND_APP_EVENT_CAPACITY; index++) {
        if (forwarder->events[index].occupied == 0u) {
            if (slot_out != NULL) {
                *slot_out = index;
            }
            return &forwarder->events[index];
        }
    }
    return NULL;
}

static mind_event_target_t *submission_target(
    mind_event_forwarder_t *forwarder, const mind_root_submission_t *submission,
    mind_event_item_t **item_out)
{
    mind_event_item_t *item;
    mind_event_target_t *target;

    if (item_out != NULL) {
        *item_out = NULL;
    }
    if (forwarder == NULL || submission == NULL ||
        submission->kind != MIND_ROOT_SUBMISSION_EVENT ||
        submission->slot >= MIND_APP_EVENT_CAPACITY) {
        return NULL;
    }
    item = &forwarder->events[submission->slot];
    if (item->occupied == 0u || submission->target_slot >= item->target_count) {
        return NULL;
    }
    target = &item->targets[submission->target_slot];
    if (!adva_equal(&target->canonical_adva, &submission->target) ||
        target->state != MIND_EVENT_TARGET_PENDING) {
        return NULL;
    }
    if (item_out != NULL) {
        *item_out = item;
    }
    return target;
}

void mind_event_forwarder_init(mind_event_forwarder_t *forwarder,
                               const tavrn_adva_t *local_adva)
{
    if (forwarder != NULL && local_adva != NULL) {
        memset(forwarder, 0, sizeof(*forwarder));
        forwarder->local_adva = *local_adva;
    }
}

int mind_event_forwarder_consume_ingress(
    mind_event_forwarder_t *forwarder, void *context,
    mind_event_ingress_take_fn ingress_take, const mind_root_plane_t *plane,
    uint32_t now_ms)
{
    tavrn_adva_t roots[MIND_APP_ROOT_CAPACITY];
    mind_application_ingress_event_t ingress_event;
    mind_event_item_t *item;
    uint8_t event_slot;
    uint8_t root_count;
    uint8_t index;

    if (forwarder == NULL || ingress_take == NULL || plane == NULL) {
        return 0;
    }
    root_count = mind_root_plane_snapshot_active_roots(plane, roots);
    if (root_count == 0u) {
        if (ingress_take(context, &ingress_event) !=
            MIND_APPLICATION_INGRESS_TAKE_OK) {
            return 0;
        }
        increment_saturating(&forwarder->counters.rootless_drop);
        return 1;
    }
    item = find_empty_event(forwarder, &event_slot);
    if (item == NULL) {
        increment_saturating(&forwarder->counters.event_capacity_busy);
        return 0;
    }
    if (ingress_take(context, &ingress_event) !=
        MIND_APPLICATION_INGRESS_TAKE_OK) {
        return 0;
    }
    memset(item, 0, sizeof(*item));
    item->report = ingress_event.report;
    item->observed_at_ms = now_ms;
    item->target_count = root_count;
    item->occupied = 1u;
    for (index = 0u; index < root_count; index++) {
        item->targets[index].canonical_adva = roots[index];
        item->targets[index].state = MIND_EVENT_TARGET_PENDING;
    }
    (void)event_slot;
    return 1;
}

void mind_event_forwarder_service_local(mind_event_forwarder_t *forwarder,
                                        void *context,
                                        mind_event_local_publish_fn publish_local,
                                        uint32_t now_ms)
{
    uint8_t event_index;

    if (forwarder == NULL || publish_local == NULL) {
        return;
    }
    for (event_index = 0u; event_index < MIND_APP_EVENT_CAPACITY; event_index++) {
        mind_event_item_t *item = &forwarder->events[event_index];
        uint8_t target_index;

        if (item->occupied == 0u) {
            continue;
        }
        for (target_index = 0u; target_index < item->target_count; target_index++) {
            mind_event_target_t *target = &item->targets[target_index];
            tavrn_router_delivery_status_t status;

            if (target->state != MIND_EVENT_TARGET_PENDING ||
                !adva_equal(&target->canonical_adva, &forwarder->local_adva)) {
                continue;
            }
            status = publish_local(context, &item->report, &forwarder->local_adva,
                                   now_ms);
            if (status == TAVRN_ROUTER_DELIVERY_BUSY) {
                increment_saturating(&forwarder->counters.local_busy);
                continue;
            }
            if (status == TAVRN_ROUTER_DELIVERY_OK) {
                target->state = MIND_EVENT_TARGET_COMPLETE;
                increment_saturating(&forwarder->counters.local_published);
            }
        }
        retire_if_finished(forwarder, event_index);
    }
}

int mind_event_forwarder_prepare_submission(
    mind_event_forwarder_t *forwarder, uint8_t urgent,
    mind_root_submission_t *submission_out)
{
    uint8_t event_offset;

    if (forwarder == NULL || submission_out == NULL || urgent > 1u) {
        return 0;
    }
    memset(submission_out, 0, sizeof(*submission_out));
    for (event_offset = 0u; event_offset < MIND_APP_EVENT_CAPACITY; event_offset++) {
        uint8_t event_index = (uint8_t)((forwarder->event_cursor + event_offset) %
                                        MIND_APP_EVENT_CAPACITY);
        mind_event_item_t *item = &forwarder->events[event_index];
        uint8_t target_offset;

        if (item->occupied == 0u || item->report.urgent != urgent) {
            continue;
        }
        for (target_offset = 0u; target_offset < item->target_count; target_offset++) {
            uint8_t target_index = (uint8_t)((item->target_cursor +
                target_offset) % item->target_count);
            mind_event_target_t *target = &item->targets[target_index];

            if (target->state != MIND_EVENT_TARGET_PENDING ||
                target->resolver_blocked != 0u ||
                target->resolver_failed_closed != 0u ||
                adva_equal(&target->canonical_adva, &forwarder->local_adva)) {
                continue;
            }
            submission_out->target = target->canonical_adva;
            submission_out->record = item->report;
            submission_out->slot = event_index;
            submission_out->target_slot = target_index;
            submission_out->kind = MIND_ROOT_SUBMISSION_EVENT;
            return 1;
        }
    }
    return 0;
}

int mind_event_forwarder_submission_resolved(
    mind_event_forwarder_t *forwarder, const mind_root_submission_t *submission,
    mind_root_resolver_status_t status)
{
    mind_event_target_t *target = submission_target(forwarder, submission, NULL);

    if (target == NULL) {
        return 0;
    }
    advance_cursor(forwarder, submission->slot, submission->target_slot);
    if (status == MIND_ROOT_RESOLVER_UNIQUE) {
        return 1;
    }
    if (status == MIND_ROOT_RESOLVER_UNKNOWN) {
        target->resolver_blocked = 1u;
        increment_saturating(&forwarder->counters.resolver_unknown);
    } else if (status == MIND_ROOT_RESOLVER_COLLIDING) {
        target->resolver_blocked = 1u;
        increment_saturating(&forwarder->counters.resolver_colliding);
    } else if (status == MIND_ROOT_RESOLVER_RESERVED) {
        target->resolver_failed_closed = 1u;
        increment_saturating(&forwarder->counters.resolver_reserved);
    } else {
        target->resolver_failed_closed = 1u;
        increment_saturating(&forwarder->counters.resolver_invalid);
    }
    return 0;
}

void mind_event_forwarder_submission_complete(
    mind_event_forwarder_t *forwarder, const mind_root_submission_t *submission,
    uint8_t accepted)
{
    mind_event_item_t *item;
    mind_event_target_t *target = submission_target(forwarder, submission, &item);

    if (target == NULL || item == NULL) {
        return;
    }
    advance_cursor(forwarder, submission->slot, submission->target_slot);
    if (accepted != 0u) {
        target->state = MIND_EVENT_TARGET_COMPLETE;
        increment_saturating(&forwarder->counters.remote_handoff);
        retire_if_finished(forwarder, submission->slot);
    } else {
        increment_saturating(&forwarder->counters.remote_busy);
    }
}

void mind_event_forwarder_note_topology_observed(mind_event_forwarder_t *forwarder)
{
    uint8_t event_index;

    if (forwarder == NULL) {
        return;
    }
    for (event_index = 0u; event_index < MIND_APP_EVENT_CAPACITY; event_index++) {
        mind_event_item_t *item = &forwarder->events[event_index];
        uint8_t target_index;

        if (item->occupied == 0u) {
            continue;
        }
        for (target_index = 0u; target_index < item->target_count; target_index++) {
            item->targets[target_index].resolver_blocked = 0u;
        }
    }
}

void mind_event_forwarder_clear_target(mind_event_forwarder_t *forwarder,
                                       const tavrn_adva_t *canonical_adva)
{
    uint8_t event_index;

    if (forwarder == NULL || canonical_adva == NULL) {
        return;
    }
    for (event_index = 0u; event_index < MIND_APP_EVENT_CAPACITY; event_index++) {
        mind_event_item_t *item = &forwarder->events[event_index];
        uint8_t target_index;

        if (item->occupied == 0u) {
            continue;
        }
        for (target_index = 0u; target_index < item->target_count; target_index++) {
            mind_event_target_t *target = &item->targets[target_index];

            if (target->state == MIND_EVENT_TARGET_PENDING &&
                adva_equal(&target->canonical_adva, canonical_adva)) {
                target->state = MIND_EVENT_TARGET_CANCELLED;
                target->resolver_blocked = 0u;
                target->resolver_failed_closed = 0u;
                increment_saturating(&forwarder->counters.cancelled_targets);
            }
        }
        retire_if_finished(forwarder, event_index);
    }
}
