#include "tavrn_full.h"

#include <string.h>

#include "tavrn_smart_ttl.h"

static int logical_matches_identity(const tavrn_logical_id_t *logical_id,
                                    const tavrn_adva_t *identity)
{
    uint16_t suffix;

    if (logical_id == NULL || identity == NULL) {
        return 0;
    }
    if (logical_id->width == TAVRN_IDENTITY_SID8) {
        suffix = identity->bytes[0];
        return logical_id->value != 0u && logical_id->value != 0x00ffu &&
               logical_id->value == suffix;
    }
    if (logical_id->width == TAVRN_IDENTITY_SID16) {
        suffix = (uint16_t)identity->bytes[0] |
                 ((uint16_t)identity->bytes[1] << 8);
        return logical_id->value != 0u && logical_id->value != 0xffffu &&
               logical_id->value == suffix;
    }
    return 0;
}

static int resolve_unique_member(const tavrn_full_t *full,
                                 const tavrn_logical_id_t *logical_id,
                                 uint32_t now_ms,
                                 tavrn_gtt_snapshot_t *snapshot_out)
{
    tavrn_gtt_snapshot_t candidate;
    uint8_t matches = 0u;
    uint8_t index;

    if (full == NULL || full->gtt == NULL || full->gtt->storage == NULL ||
        logical_id == NULL || snapshot_out == NULL) {
        return 0;
    }
    memset(snapshot_out, 0, sizeof(*snapshot_out));
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &full->gtt->storage->entries[index];

        if (entry->occupied == 0u ||
            !logical_matches_identity(logical_id, &entry->identity)) {
            continue;
        }
        if (tavrn_gtt_snapshot(full->gtt, &entry->identity, now_ms,
                               &candidate) != TAVRN_GTT_QUERY_FOUND) {
            continue;
        }
        matches++;
        if (matches == 1u) {
            *snapshot_out = candidate;
        }
    }
    return matches == 1u;
}

static void observe_identity(tavrn_gtt_t *gtt, const tavrn_adva_t *identity,
                              uint16_t serial, uint8_t serial_present,
                              uint8_t hop_count,
                              tavrn_gtt_provenance_t provenance,
                              uint32_t now_ms)
{
    tavrn_gtt_evidence_t evidence;

    if (gtt == NULL || identity == NULL) {
        return;
    }
    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = *identity;
    evidence.serial = serial;
    evidence.serial_present = serial_present;
    evidence.hop_count = hop_count;
    evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    (void)tavrn_gtt_observe_with_provenance(gtt, &evidence, provenance, now_ms,
                                             NULL);
}

static void tavrn_full_observe(void *context,
                               const tavrn_router_observation_t *observation,
                               uint32_t now_ms)
{
    tavrn_full_t *full = context;
    uint8_t index;

    if (full == NULL || full->gtt == NULL || observation == NULL) {
        return;
    }

    /* This direct observation precedes logical-subject resolution, allowing a
     * same-frame SID16 subject to resolve through the transmitter binding. */
    observe_identity(full->gtt, &observation->transmitter.adva, 0u, 0u, 1u,
                     TAVRN_GTT_PROVENANCE_DIRECT_OUTER_TRANSMITTER, now_ms);

    for (index = 0u; index < observation->subject_count; index++) {
        const tavrn_router_evidence_subject_t *subject =
            &observation->subjects[index];
        tavrn_adva_t identity;
        tavrn_gtt_snapshot_t snapshot;
        uint8_t serial_present;

        if (subject->hop_present == 0u || subject->hop_count == 0u ||
            subject->hop_count > TAVRN_GTT_HOP_MAX) {
            continue;
        }
        if (subject->canonical_present != 0u) {
            if (!logical_matches_identity(&subject->logical_id,
                                          &subject->canonical_identity)) {
                continue;
            }
            identity = subject->canonical_identity;
        } else {
            if (!resolve_unique_member(full, &subject->logical_id, now_ms,
                                       &snapshot)) {
                continue;
            }
            identity = snapshot.identity;
        }

        if (subject->serial_kind == TAVRN_ROUTER_EVIDENCE_SERIAL_NODE_SEQUENCE ||
            subject->serial_kind ==
                TAVRN_ROUTER_EVIDENCE_SERIAL_DESTINATION_SEQUENCE) {
            serial_present = 1u;
        } else if (subject->serial_kind == TAVRN_ROUTER_EVIDENCE_SERIAL_NONE) {
            serial_present = 0u;
        } else {
            continue;
        }
        observe_identity(full->gtt, &identity, subject->serial, serial_present,
                         subject->hop_count,
                         TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, now_ms);
    }
}

static tavrn_router_scope_hint_t tavrn_full_initial_scope(
    void *context, const tavrn_logical_id_t *destination,
    uint8_t full_scope, uint32_t now_ms)
{
    tavrn_full_t *full = context;
    tavrn_router_scope_hint_t hint;
    tavrn_gtt_snapshot_t snapshot;
    tavrn_smart_ttl_decision_t decision;

    hint.has_hint = 0u;
    hint.initial_scope = full_scope;
    if (!resolve_unique_member(full, destination, now_ms, &snapshot)) {
        return hint;
    }
    decision = tavrn_smart_ttl_decide(&snapshot, full_scope);
    if (decision.has_hint != 0u && decision.initial_scope != 0u &&
        decision.initial_scope <= full_scope) {
        hint.has_hint = 1u;
        hint.initial_scope = decision.initial_scope;
    }
    return hint;
}

tavrn_full_init_status_t tavrn_full_init(tavrn_full_t *full, tavrn_gtt_t *gtt)
{
    if (full == NULL || gtt == NULL) {
        return TAVRN_FULL_INIT_INVALID_ARGUMENT;
    }
    full->gtt = gtt;
    return TAVRN_FULL_INIT_OK;
}

tavrn_router_augmentation_hooks_t tavrn_full_router_hooks(tavrn_full_t *full)
{
    tavrn_router_augmentation_hooks_t hooks;

    memset(&hooks, 0, sizeof(hooks));
    if (full != NULL) {
        hooks.context = full;
        hooks.observe = tavrn_full_observe;
        hooks.initial_scope = tavrn_full_initial_scope;
    }
    return hooks;
}

tavrn_esc_context_status_t tavrn_full_resolve_unique_sid8(
    const tavrn_full_t *full, const tavrn_adva_t *identity,
    tavrn_logical_id_t *sid8_out)
{
    uint8_t sid8;
    uint8_t matches = 0u;
    uint8_t index;

    if (sid8_out != NULL) {
        memset(sid8_out, 0, sizeof(*sid8_out));
    }
    if (full == NULL || full->gtt == NULL || full->gtt->storage == NULL ||
        identity == NULL || sid8_out == NULL) {
        return TAVRN_ESC_CONTEXT_INVALID;
    }
    sid8 = identity->bytes[0];
    if (sid8 == 0u || sid8 == 0xffu) {
        return TAVRN_ESC_CONTEXT_RESERVED;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &full->gtt->storage->entries[index];

        if (entry->occupied == 0u || entry->departed != 0u ||
            entry->identity.bytes[0] != sid8) {
            continue;
        }
        matches++;
    }
    if (matches == 0u) {
        return TAVRN_ESC_CONTEXT_UNKNOWN;
    }
    if (matches != 1u) {
        return TAVRN_ESC_CONTEXT_COLLIDING;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &full->gtt->storage->entries[index];

        if (entry->occupied != 0u && entry->departed == 0u &&
            entry->identity.bytes[0] == sid8) {
            if (memcmp(entry->identity.bytes, identity->bytes, TAVRN_ADVA_LEN) != 0) {
                return TAVRN_ESC_CONTEXT_UNKNOWN;
            }
            sid8_out->width = TAVRN_IDENTITY_SID8;
            sid8_out->value = sid8;
            return TAVRN_ESC_CONTEXT_UNIQUE;
        }
    }
    return TAVRN_ESC_CONTEXT_UNKNOWN;
}

static int mailbox_command_is_valid(const tavrn_gtt_application_command_t *command)
{
    return command != NULL &&
        command->kind <= TAVRN_GTT_APPLICATION_COMMAND_QUERY;
}

static int mailbox_result_is_valid(const tavrn_gtt_application_command_result_t *result)
{
    return result != NULL && result->status <= TAVRN_GTT_APPLICATION_UNAVAILABLE &&
        result->query_status <= TAVRN_GTT_EXPIRY_QUERY_UNAVAILABLE;
}

tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_init(
    tavrn_full_application_mailbox_t *mailbox)
{
    if (mailbox == NULL) {
        return TAVRN_FULL_APPLICATION_MAILBOX_INVALID;
    }
    memset(mailbox, 0, sizeof(*mailbox));
    return TAVRN_FULL_APPLICATION_MAILBOX_EMPTY;
}

tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_submit(
    tavrn_full_application_mailbox_t *mailbox,
    tavrn_gtt_application_command_t command)
{
    if (mailbox == NULL || !mailbox_command_is_valid(&command)) {
        return TAVRN_FULL_APPLICATION_MAILBOX_INVALID;
    }
    if (mailbox->command_pending != 0u || mailbox->owner_processing != 0u ||
        mailbox->result_ready != 0u) {
        return TAVRN_FULL_APPLICATION_MAILBOX_BUSY;
    }
    mailbox->command = command;
    mailbox->command_pending = 1u;
    return TAVRN_FULL_APPLICATION_MAILBOX_QUEUED;
}

tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_owner_take(
    tavrn_full_application_mailbox_t *mailbox,
    tavrn_gtt_application_command_t *command_out)
{
    if (mailbox == NULL || command_out == NULL) {
        return TAVRN_FULL_APPLICATION_MAILBOX_INVALID;
    }
    if (mailbox->owner_processing != 0u || mailbox->result_ready != 0u) {
        return TAVRN_FULL_APPLICATION_MAILBOX_BUSY;
    }
    if (mailbox->command_pending == 0u) {
        return TAVRN_FULL_APPLICATION_MAILBOX_EMPTY;
    }
    *command_out = mailbox->command;
    memset(&mailbox->command, 0, sizeof(mailbox->command));
    mailbox->command_pending = 0u;
    mailbox->owner_processing = 1u;
    return TAVRN_FULL_APPLICATION_MAILBOX_PROCESSING;
}

tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_owner_publish(
    tavrn_full_application_mailbox_t *mailbox,
    tavrn_gtt_application_command_result_t result)
{
    if (mailbox == NULL || !mailbox_result_is_valid(&result)) {
        return TAVRN_FULL_APPLICATION_MAILBOX_INVALID;
    }
    if (mailbox->owner_processing == 0u || mailbox->command_pending != 0u ||
        mailbox->result_ready != 0u) {
        return TAVRN_FULL_APPLICATION_MAILBOX_INVALID;
    }
    mailbox->result = result;
    mailbox->owner_processing = 0u;
    mailbox->result_ready = 1u;
    return TAVRN_FULL_APPLICATION_MAILBOX_READY;
}

tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_consumer_take(
    tavrn_full_application_mailbox_t *mailbox,
    tavrn_gtt_application_command_result_t *result_out)
{
    if (mailbox == NULL || result_out == NULL) {
        return TAVRN_FULL_APPLICATION_MAILBOX_INVALID;
    }
    if (mailbox->result_ready == 0u) {
        return TAVRN_FULL_APPLICATION_MAILBOX_EMPTY;
    }
    *result_out = mailbox->result;
    memset(&mailbox->result, 0, sizeof(mailbox->result));
    mailbox->result_ready = 0u;
    return TAVRN_FULL_APPLICATION_MAILBOX_READY;
}
