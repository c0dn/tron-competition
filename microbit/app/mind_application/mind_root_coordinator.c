#include "mind_root_coordinator.h"

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

static int record_equal(const mind_application_wire_record_t *left,
                        const mind_application_wire_record_t *right)
{
    return left != NULL && right != NULL && left->app_kind == right->app_kind &&
        left->app_source == right->app_source && left->urgent == right->urgent &&
        left->app_len == right->app_len &&
        memcmp(left->app_bytes, right->app_bytes,
               MIND_APPLICATION_WIRE_APP_BYTES_MAX) == 0;
}

static int submission_equal(const mind_root_submission_t *left,
                            const mind_root_submission_t *right)
{
    return left != NULL && right != NULL && left->kind == right->kind &&
        left->slot == right->slot && left->target_slot == right->target_slot &&
        adva_equal(&left->target, &right->target) &&
        record_equal(&left->record, &right->record);
}

static int submission_is_valid(const mind_root_coordinator_t *coordinator,
                               const mind_root_submission_t *submission)
{
    if (coordinator == NULL || submission == NULL) {
        return 0;
    }
    if (submission->kind == MIND_ROOT_SUBMISSION_ACK) {
        return submission->slot < MIND_APP_ROOT_ACK_CAPACITY &&
            submission->target_slot == 0u;
    }
    if (submission->kind == MIND_ROOT_SUBMISSION_STATE) {
        return submission->slot < MIND_APP_CAMPAIGN_CAPACITY &&
            submission->target_slot == 0u;
    }
    if (submission->kind == MIND_ROOT_SUBMISSION_EVENT) {
        return submission->slot < MIND_APP_EVENT_CAPACITY &&
            submission->target_slot < MIND_APP_ROOT_CAPACITY;
    }
    return 0;
}

static int submission_owner_is_valid(const mind_root_coordinator_t *coordinator,
                                     const mind_root_submission_t *submission)
{
    const mind_event_item_t *item;

    if (!submission_is_valid(coordinator, submission)) {
        return 0;
    }
    if (submission->kind != MIND_ROOT_SUBMISSION_EVENT) {
        return 1;
    }
    item = &coordinator->events.events[submission->slot];
    return item->occupied != 0u && submission->target_slot < item->target_count &&
        adva_equal(&item->targets[submission->target_slot].canonical_adva,
                   &submission->target) && record_equal(&item->report, &submission->record);
}

static int token_equal(const mind_root_coordinator_token_t *left,
                       const mind_root_coordinator_token_t *right)
{
    return left != NULL && right != NULL && left->valid != 0u &&
        right->valid != 0u && left->sequence == right->sequence &&
        submission_equal(&left->submission, &right->submission);
}

static int data_equal(const tron_application_data_t *left,
                      const tron_application_data_t *right)
{
    return left != NULL && right != NULL &&
        left->final_destination.width == right->final_destination.width &&
        left->final_destination.value == right->final_destination.value &&
        left->app_kind == right->app_kind && left->app_source == right->app_source &&
        left->urgent == right->urgent && left->app_len == right->app_len &&
        memcmp(left->app_bytes, right->app_bytes, sizeof(left->app_bytes)) == 0;
}

static int operations_valid(const mind_root_coordinator_operations_t *operations)
{
    return operations != NULL && operations->snapshot != NULL &&
        operations->sid8_ready != NULL && operations->resolve_outgoing != NULL &&
        operations->resolve_incoming != NULL && operations->command_peek != NULL &&
        operations->command_consume != NULL && operations->gtt_claim != NULL &&
        operations->gtt_commit != NULL && operations->gtt_cancel != NULL &&
        operations->final_peek != NULL &&
        operations->final_consume != NULL && operations->final_pin_observer != NULL &&
        operations->ingress_take != NULL &&
        operations->final_publish_local != NULL &&
        operations->log_reserve != NULL && operations->log_commit != NULL &&
        operations->log_cancel != NULL && operations->ui_publish != NULL &&
        operations->generic_submit != NULL;
}

static void make_command_record(const mind_root_coordinator_t *coordinator,
                                uint32_t now_ms,
                                const mind_command_attempt_t *attempt,
                                mind_log_record_t *record_out)
{
    if (coordinator == NULL || attempt == NULL || record_out == NULL) {
        return;
    }
    memset(record_out, 0, sizeof(*record_out));
    record_out->kind = MIND_LOG_COMMAND;
    record_out->now_ms = now_ms;
    record_out->local_adva = coordinator->plane.local_adva;
    record_out->detail.command.command = attempt->command;
    record_out->detail.command.status = attempt->status;
}

static void make_root_record(const mind_root_coordinator_t *coordinator,
                              uint32_t now_ms, mind_log_record_t *record_out)
{
    if (coordinator == NULL || record_out == NULL) {
        return;
    }
    memset(record_out, 0, sizeof(*record_out));
    record_out->kind = MIND_LOG_ROOT;
    record_out->now_ms = now_ms;
    record_out->local_adva = coordinator->plane.local_adva;
    mind_root_plane_status(&coordinator->plane, &record_out->detail.root.state);
    record_out->detail.root.node_number = coordinator->node_number;
    record_out->detail.root.local_active =
        mind_root_plane_local_active(&coordinator->plane);
    record_out->detail.root.rootless_drop = coordinator->events.counters.rootless_drop;
}

static void process_commands(mind_root_coordinator_t *coordinator,
                             uint32_t now_ms)
{
    mind_command_attempt_t attempt;

    while (coordinator->operations.command_peek(coordinator->operations.context,
                                                &attempt)) {
        mind_log_reservation_t reservation;
        mind_log_record_t records[2];
        uint8_t needs_root = attempt.command == MIND_COMMAND_ON ||
            attempt.command == MIND_COMMAND_OFF || attempt.command == MIND_COMMAND_STATUS;
        uint8_t gtt_claimed = 0u;

        /* A busy logger leaves the copied command at its mailbox head.  No
         * local role/UI transition occurs until all required records reserve. */
        if (!coordinator->operations.log_reserve(coordinator->operations.context,
                                                 needs_root != 0u ? 2u : 1u,
                                                 &reservation)) {
            return;
        }
        if (attempt.status == MIND_COMMAND_ACCEPTED) {
            if (attempt.command == MIND_COMMAND_GTT) {
                if (coordinator->operations.gtt_claim(coordinator->operations.context)) {
                    gtt_claimed = 1u;
                } else {
                    attempt.status = MIND_COMMAND_BUSY;
                }
            } else if ((attempt.command == MIND_COMMAND_ON &&
                 mind_root_plane_local_active(&coordinator->plane) != 0u) ||
                (attempt.command == MIND_COMMAND_OFF &&
                 mind_root_plane_local_active(&coordinator->plane) == 0u)) {
                attempt.status = MIND_COMMAND_DUPLICATE;
            } else if (attempt.command != MIND_COMMAND_ON &&
                       attempt.command != MIND_COMMAND_OFF &&
                       attempt.command != MIND_COMMAND_STATUS) {
                attempt.status = MIND_COMMAND_REJECTED;
            }
        }
        make_command_record(coordinator, now_ms, &attempt, &records[0]);
        if (attempt.command == MIND_COMMAND_ON) {
            (void)mind_root_plane_set_local(&coordinator->plane, 1u, now_ms);
        } else if (attempt.command == MIND_COMMAND_OFF) {
            if (mind_root_plane_set_local(&coordinator->plane, 0u, now_ms) ==
                MIND_ROOT_APPLY_ACCEPTED) {
                mind_event_forwarder_clear_target(&coordinator->events,
                                                  &coordinator->plane.local_adva);
            }
        }
        if (needs_root != 0u) {
            make_root_record(coordinator, now_ms, &records[1]);
        }
        if (!coordinator->operations.log_commit(coordinator->operations.context,
                                                 &reservation, records)) {
            /* The fixed log queue guarantees a reserved tail commits.  Retain
             * the command if a binding reports structural failure. */
            coordinator->operations.log_cancel(coordinator->operations.context,
                                                &reservation);
            if (gtt_claimed != 0u) {
                coordinator->operations.gtt_cancel(coordinator->operations.context);
            }
            return;
        }
        if (gtt_claimed != 0u) {
            coordinator->operations.gtt_commit(coordinator->operations.context);
        }
        (void)coordinator->operations.command_consume(coordinator->operations.context);
    }
}

static int record_from_data(const tavrn_link_data_t *data,
                            mind_application_wire_record_t *record_out)
{
    if (data == NULL || record_out == NULL ||
        data->app_len > MIND_APPLICATION_WIRE_APP_BYTES_MAX) {
        return 0;
    }
    memset(record_out, 0, sizeof(*record_out));
    record_out->app_kind = data->app_kind;
    record_out->app_source = data->app_source;
    record_out->urgent = data->urgent;
    record_out->app_len = data->app_len;
    memcpy(record_out->app_bytes, data->app_bytes, data->app_len);
    return mind_application_wire_validate(record_out) == MIND_APPLICATION_WIRE_OK;
}

static void make_event_record(const mind_root_coordinator_t *coordinator,
                               uint32_t delivered_at_ms,
                              const mind_application_wire_record_t *record,
                              const mind_application_report_t *report,
                              const tavrn_adva_t *observer, uint8_t path_local,
                              mind_log_record_t *record_out)
{
    if (coordinator == NULL || record == NULL || report == NULL || observer == NULL ||
        record_out == NULL) {
        return;
    }
    memset(record_out, 0, sizeof(*record_out));
    record_out->kind = MIND_LOG_EVENT;
    record_out->now_ms = delivered_at_ms;
    record_out->local_adva = coordinator->plane.local_adva;
    record_out->detail.event.report = *report;
    record_out->detail.event.observer = *observer;
    record_out->detail.event.wearable = record->app_source;
    record_out->detail.event.path_local = path_local;
}

static void process_final_data(mind_root_coordinator_t *coordinator,
                               uint32_t now_ms)
{
    mind_root_inbox_entry_t entry;

    (void)now_ms;

    while (coordinator->operations.final_peek(coordinator->operations.context, &entry)) {
        mind_application_wire_record_t record;
        tavrn_adva_t origin;
        mind_root_resolver_status_t resolver;

        /* Semantic validation precedes sender resolution and every root/ACK
         * mutation.  Malformed generic DATA is consumed as Layer-7 rejected. */
        if (!record_from_data(&entry.data, &record)) {
            increment_saturating(&coordinator->counters.final_rejected);
            (void)coordinator->operations.final_consume(coordinator->operations.context);
            continue;
        }
        if (entry.path == MIND_ROOT_INBOX_PATH_LOCAL) {
            origin = entry.observer;
        } else {
            if (entry.observer_pinned != 0u) {
                origin = entry.observer;
            } else if (entry.data.origin.width != TAVRN_IDENTITY_SID8) {
                increment_saturating(&coordinator->counters.final_resolver_rejected);
                return;
            } else {
                resolver = coordinator->operations.resolve_incoming(
                    coordinator->operations.context, (uint8_t)entry.data.origin.value,
                    entry.delivered_at_ms, &origin);
                if (resolver != MIND_ROOT_RESOLVER_UNIQUE) {
                    mind_root_plane_note_incoming_resolver(&coordinator->plane, resolver);
                    increment_saturating(&coordinator->counters.final_resolver_rejected);
                    return;
                }
                if (!coordinator->operations.final_pin_observer(
                        coordinator->operations.context, &origin)) {
                    increment_saturating(&coordinator->counters.final_resolver_rejected);
                    return;
                }
            }
        }
        if (record.app_kind == MIND_REPORT ||
            record.app_kind == MIND_REPORT_OBSERVED) {
            mind_application_report_t report;
            mind_log_reservation_t reservation;
            mind_log_record_t event_record;

            if (mind_application_wire_unpack_report(&record, &report) !=
                MIND_APPLICATION_WIRE_OK) {
                increment_saturating(&coordinator->counters.final_rejected);
                (void)coordinator->operations.final_consume(
                    coordinator->operations.context);
                continue;
            }
            /* Do not release accepted DATA until the exact UART record owns a
             * logger slot.  This preserves truth under logger backpressure. */
            if (!coordinator->operations.log_reserve(coordinator->operations.context, 1u,
                                                     &reservation)) {
                increment_saturating(&coordinator->counters.final_logger_busy);
                return;
            }
            make_event_record(coordinator, entry.delivered_at_ms, &record, &report,
                              &origin,
                              entry.path == MIND_ROOT_INBOX_PATH_LOCAL,
                              &event_record);
            if (!coordinator->operations.log_commit(coordinator->operations.context,
                                                    &reservation, &event_record)) {
                coordinator->operations.log_cancel(coordinator->operations.context,
                                                   &reservation);
                increment_saturating(&coordinator->counters.final_logger_busy);
                return;
            }
        } else if (record.app_kind == ROOT_STATE) {
            mind_application_root_state_t state;

            if (mind_application_wire_unpack_root_state(&record, &state) ==
                MIND_APPLICATION_WIRE_OK) {
                if (mind_root_plane_receive_root_state(&coordinator->plane, &origin,
                                                        &state) == MIND_ROOT_APPLY_ACCEPTED &&
                    state.active == 0u) {
                    mind_event_forwarder_clear_target(&coordinator->events, &origin);
                }
            }
        } else if (record.app_kind == ROOT_ACK) {
            mind_application_root_ack_t ack;

            if (mind_application_wire_unpack_root_ack(&record, &ack) ==
                MIND_APPLICATION_WIRE_OK) {
                mind_root_plane_receive_root_ack(&coordinator->plane, &origin, &ack);
            }
        }
        (void)coordinator->operations.final_consume(coordinator->operations.context);
    }
}

static void service_topology(mind_root_coordinator_t *coordinator,
                             uint32_t now_ms)
{
    routed_cycle_gtt_snapshot_t snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    if (!coordinator->operations.snapshot(coordinator->operations.context, now_ms,
                                           &snapshot) ||
        !mind_root_plane_apply_topology(&coordinator->plane, &snapshot)) {
        mind_root_plane_note_snapshot_failure(&coordinator->plane);
    } else {
        uint8_t index;

        mind_event_forwarder_note_topology_observed(&coordinator->events);
        for (index = 0u; index < snapshot.entry_count; index++) {
            if (snapshot.entries[index].departed == ROUTED_CYCLE_BOOLEAN_TRUE) {
                mind_event_forwarder_clear_target(
                    &coordinator->events, &snapshot.entries[index].canonical_adva);
            }
        }
    }
    mind_root_plane_set_sid8_ready(&coordinator->plane,
                                   coordinator->operations.sid8_ready(
                                       coordinator->operations.context),
                                   now_ms);
}

static mind_root_coordinator_submit_status_t completed_status(
    mind_root_coordinator_submit_status_t status)
{
    return status == MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED ||
        status == MIND_ROOT_COORDINATOR_SUBMIT_QUEUED ?
        MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED :
        MIND_ROOT_COORDINATOR_SUBMIT_BUSY;
}

static int prepare_pending_request(mind_root_coordinator_t *coordinator,
                                   const mind_root_submission_t *submission,
                                   uint32_t now_ms,
                                   mind_root_coordinator_request_t *request_out)
{
    tavrn_logical_id_t sid8;
    mind_root_resolver_status_t resolver;
    int resolved;

    if (coordinator == NULL || submission == NULL || request_out == NULL ||
        !submission_owner_is_valid(coordinator, submission)) {
        return 0;
    }
    memset(&sid8, 0, sizeof(sid8));
    resolver = coordinator->operations.resolve_outgoing(
        coordinator->operations.context, &submission->target, &sid8);
    if (resolver == MIND_ROOT_RESOLVER_UNIQUE && sid8.width != TAVRN_IDENTITY_SID8) {
        resolver = MIND_ROOT_RESOLVER_INVALID;
    }
    if (submission->kind == MIND_ROOT_SUBMISSION_EVENT) {
        resolved = mind_event_forwarder_submission_resolved(&coordinator->events,
                                                            submission, resolver);
    } else {
        resolved = mind_root_plane_submission_resolved(&coordinator->plane, submission,
                                                       resolver, now_ms);
    }
    if (!resolved) {
        return 0;
    }
    request_out->data.final_destination = sid8;
    request_out->data.app_kind = submission->record.app_kind;
    request_out->data.app_source = submission->record.app_source;
    request_out->data.urgent = submission->record.urgent;
    request_out->data.app_len = submission->record.app_len;
    memcpy(request_out->data.app_bytes, submission->record.app_bytes,
           sizeof(request_out->data.app_bytes));
    coordinator->next_token_sequence++;
    if (coordinator->next_token_sequence == 0u) {
        coordinator->next_token_sequence++;
    }
    request_out->token.submission = *submission;
    request_out->token.sequence = coordinator->next_token_sequence;
    request_out->token.valid = 1u;
    coordinator->pending = request_out->token;
    coordinator->pending_data = request_out->data;
    return 1;
}

static int prepare_root_owner(mind_root_coordinator_t *coordinator,
                               uint32_t now_ms, uint8_t ack_only,
                               mind_root_coordinator_request_t *request_out)
{
    mind_root_submission_t submission;
    uint8_t attempt;
    uint8_t attempt_limit = ack_only != 0u ? MIND_APP_ROOT_ACK_CAPACITY :
        MIND_APP_CAMPAIGN_CAPACITY;

    /* A non-UNIQUE resolver result marks this exact owner unavailable.  Keep
     * selecting within its independent table so one stale topology identity
     * cannot starve another owner already eligible in this arbiter phase. */
    for (attempt = 0u; attempt < attempt_limit; attempt++) {
        int available = ack_only != 0u ?
            mind_root_plane_prepare_ack_submission(&coordinator->plane, now_ms,
                                                    &submission) :
            mind_root_plane_prepare_state_submission(&coordinator->plane, now_ms,
                                                      &submission);

        if (available == 0) {
            return 0;
        }
        if (prepare_pending_request(coordinator, &submission, now_ms, request_out)) {
            return 1;
        }
    }
    return 0;
}

static int prepare_event_owner(mind_root_coordinator_t *coordinator,
                               uint32_t now_ms, uint8_t urgent,
                               mind_root_coordinator_request_t *request_out)
{
    mind_root_submission_t submission;

    return mind_event_forwarder_prepare_submission(&coordinator->events, urgent,
                                                   &submission) &&
        prepare_pending_request(coordinator, &submission, now_ms, request_out);
}

int mind_root_coordinator_init(mind_root_coordinator_t *coordinator,
                               const tavrn_adva_t *local_adva,
                               uint16_t router_boot_nonce,
                               uint8_t node_number,
                               const mind_root_coordinator_operations_t *operations)
{
    if (coordinator == NULL || local_adva == NULL || router_boot_nonce == 0u ||
        node_number < 1u || node_number > 6u || !operations_valid(operations)) {
        return 0;
    }
    memset(coordinator, 0, sizeof(*coordinator));
    mind_root_plane_init(&coordinator->plane, local_adva, router_boot_nonce);
    mind_event_forwarder_init(&coordinator->events, local_adva);
    coordinator->operations = *operations;
    coordinator->node_number = node_number;
    coordinator->initialized = 1u;
    return 1;
}

int mind_root_coordinator_prepare(mind_root_coordinator_t *coordinator,
                                   uint32_t now_ms,
                                   mind_root_coordinator_request_t *request_out)
{
    if (coordinator == NULL || request_out == NULL || coordinator->initialized == 0u ||
        coordinator->pending.valid != 0u) {
        return 0;
    }
    memset(request_out, 0, sizeof(*request_out));
    process_commands(coordinator, now_ms);
    process_final_data(coordinator, now_ms);
    service_topology(coordinator, now_ms);
    (void)mind_event_forwarder_consume_ingress(
        &coordinator->events, coordinator->operations.context,
        coordinator->operations.ingress_take, &coordinator->plane, now_ms);
    coordinator->operations.ui_publish(
        coordinator->operations.context,
        mind_root_plane_local_active(&coordinator->plane),
        mind_root_plane_active_count(&coordinator->plane));
    /* One generic application request may be prepared in this cycle.  Local
     * delivery is explicitly not a self-route and uses the final inbox only. */
    if (prepare_root_owner(coordinator, now_ms, 1u, request_out)) {
        return 1;
    }
    mind_event_forwarder_service_local(&coordinator->events,
                                       coordinator->operations.context,
                                       coordinator->operations.final_publish_local,
                                       now_ms);
    if (prepare_event_owner(coordinator, now_ms, 1u, request_out)) {
        return 1;
    }
    if (prepare_root_owner(coordinator, now_ms, 0u, request_out)) {
        return 1;
    }
    return prepare_event_owner(coordinator, now_ms, 0u, request_out);
}

int mind_root_coordinator_complete(mind_root_coordinator_t *coordinator,
                                   const mind_root_coordinator_token_t *token,
                                   mind_root_coordinator_submit_status_t status,
                                   uint32_t now_ms)
{
    uint8_t accepted;

    if (coordinator == NULL || coordinator->initialized == 0u ||
        !submission_is_valid(coordinator, &coordinator->pending.submission) ||
        !token_equal(&coordinator->pending, token)) {
        if (coordinator != NULL) {
            increment_saturating(&coordinator->counters.invalid_completion);
        }
        return 0;
    }
    accepted = completed_status(status) == MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED;
    if (coordinator->pending.submission.kind == MIND_ROOT_SUBMISSION_EVENT) {
        mind_event_forwarder_submission_complete(&coordinator->events,
                                                 &coordinator->pending.submission,
                                                 accepted);
    } else {
        mind_root_plane_submission_complete(&coordinator->plane,
                                             &coordinator->pending.submission,
                                             accepted, now_ms);
    }
    memset(&coordinator->pending, 0, sizeof(coordinator->pending));
    memset(&coordinator->pending_data, 0, sizeof(coordinator->pending_data));
    return 1;
}

mind_root_coordinator_submit_status_t mind_root_coordinator_submit(
    mind_root_coordinator_t *coordinator,
    const mind_root_coordinator_token_t *token,
    const tron_application_data_t *data, uint32_t now_ms)
{
    mind_root_coordinator_submit_status_t status;

    if (coordinator == NULL || token == NULL || data == NULL ||
        coordinator->initialized == 0u ||
        !submission_is_valid(coordinator, &coordinator->pending.submission) ||
        !token_equal(&coordinator->pending, token) ||
        !data_equal(&coordinator->pending_data, data)) {
        if (coordinator != NULL) {
            increment_saturating(&coordinator->counters.invalid_submit);
        }
        return MIND_ROOT_COORDINATOR_SUBMIT_INVALID;
    }
    if (coordinator->pending.submission.kind != MIND_ROOT_SUBMISSION_EVENT) {
        mind_root_plane_submission_invoked(&coordinator->plane,
                                            &coordinator->pending.submission, now_ms);
    }
    status = coordinator->operations.generic_submit(coordinator->operations.context,
                                                    data, now_ms);
    (void)mind_root_coordinator_complete(coordinator, token, status, now_ms);
    return status;
}

const mind_root_coordinator_token_t *mind_root_coordinator_pending(
    const mind_root_coordinator_t *coordinator)
{
    return coordinator != NULL && coordinator->pending.valid != 0u ?
        &coordinator->pending : NULL;
}
