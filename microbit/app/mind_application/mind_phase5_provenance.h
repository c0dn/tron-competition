#ifndef MIND_PHASE5_PROVENANCE_H
#define MIND_PHASE5_PROVENANCE_H

/*
 * Application-owned provenance for the routed FULL phase-5 wrapper.  Status
 * fields are stored as their stable numeric protocol values so this header
 * remains a read-only observer of the frozen protocol interfaces.
 */

#include <stdint.h>
#include <string.h>

#include "mind_uart.h"

#define MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN 0xffu
#define MIND_PHASE5_PROVENANCE_QUEUE_UNKNOWN  0xffu

/* Keep these values explicit: captured firmware logs use them as a stable
 * branch identifier and each non-NONE value maps to one assignment of
 * AODV_STATUS_INVALID in routed_cycle_router_tick(). */
typedef enum mind_phase5_invalid_branch {
    MIND_PHASE5_INVALID_BRANCH_NONE = 0u,
    MIND_PHASE5_INVALID_BRANCH_INITIAL_QUEUE_GUARD = 1u,
    MIND_PHASE5_INVALID_BRANCH_FULL_MAILBOX_TAKE = 2u,
    MIND_PHASE5_INVALID_BRANCH_BINDING_CALL = 3u,
    MIND_PHASE5_INVALID_BRANCH_RAW_ROUTER_STATUS = 4u,
    MIND_PHASE5_INVALID_BRANCH_REPAIR_BINDING = 5u,
    MIND_PHASE5_INVALID_BRANCH_EXPIRY_SWEEP_ENQUEUE = 6u,
    MIND_PHASE5_INVALID_BRANCH_APPLICATION_PUBLISH_GUARD = 7u,
    MIND_PHASE5_INVALID_BRANCH_APPLICATION_PUBLISH = 8u,
    MIND_PHASE5_INVALID_BRANCH_APPLICATION_RESULT_RANGE = 9u,
    MIND_PHASE5_INVALID_BRANCH_FINAL_BINDING_RESULT = 10u,
    MIND_PHASE5_INVALID_BRANCH_COUNT = 11u,
} mind_phase5_invalid_branch_t;

/* All status fields begin at the explicit UNKNOWN sentinel.  Their associated
 * presence bit changes only after the producing call has returned, so an early
 * guard failure cannot be mistaken for an OK zero-valued status. */
typedef struct mind_phase5_provenance_snapshot {
    uint32_t timestamp_ms;
    mind_uart_snapshot_t uart;
    mind_phase5_invalid_branch_t first_invalid_branch;
    volatile uint8_t valid;
    volatile uint8_t capture_in_progress;
    volatile uint8_t logged;
    uint8_t application_present;

    uint8_t mailbox_take_present;
    uint8_t mailbox_take_status;
    uint8_t mailbox_publish_present;
    uint8_t mailbox_publish_status;
    uint8_t mailbox_occupancy_present;
    uint8_t mailbox_command_pending;
    uint8_t mailbox_owner_processing;
    uint8_t mailbox_result_ready;

    uint8_t binding_call_present;
    uint8_t binding_call_status;
    uint8_t binding_result_status;
    uint8_t raw_router_status_present;
    uint8_t raw_router_status;
    uint8_t mentorship_status;
    uint8_t broadcast_snapshot_status;
    uint8_t broadcast_observation_status;
    uint8_t activation_status;
    uint8_t targeted_owner_status;
    uint8_t verification_owner_status;
    uint8_t metadata_owner_status;
    uint8_t tc_owner_status;
    uint8_t application_result_status;
    uint8_t application_result_query_status;
    uint8_t post_tick_sweep_status;
    uint8_t post_tick_maintenance_status;

    uint8_t repair_call_present;
    uint8_t repair_binding_status;
    uint8_t repair_action_present;
    uint8_t repair_action_type;
    uint8_t repair_status;
    uint8_t repair_reforward_status;
    uint8_t repair_terminal_kind;
    uint8_t repair_eviction_status;

    uint8_t expiry_sweep_present;
    uint8_t expiry_sweep_pass;
    uint8_t expiry_enqueue_attempted;
    uint8_t expiry_enqueue_outcome;
    uint8_t expiry_queue_occupancy_present;
    uint8_t expiry_queue_count;

    uint8_t tick_trace_present;
    uint8_t tick_trace_phase;
    uint8_t tick_status;
    uint8_t tick_link_step_present;
    uint8_t tick_link_step_status;
    uint8_t tick_link_event_present;
    uint8_t tick_link_event_type;
    uint8_t router_terminal_present;
    uint8_t router_terminal_fault;
    uint8_t uart_present;
} mind_phase5_provenance_snapshot_t;

static inline void mind_phase5_provenance_set_sentinels(
    mind_phase5_provenance_snapshot_t *snapshot)
{
    snapshot->mailbox_take_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->mailbox_publish_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->binding_call_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->binding_result_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->raw_router_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->mentorship_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->broadcast_snapshot_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->broadcast_observation_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->activation_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->targeted_owner_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->verification_owner_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->metadata_owner_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->tc_owner_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->application_result_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->application_result_query_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->post_tick_sweep_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->post_tick_maintenance_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->repair_binding_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->repair_action_type = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->repair_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->repair_reforward_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->repair_terminal_kind = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->repair_eviction_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->expiry_enqueue_outcome = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->expiry_queue_count = MIND_PHASE5_PROVENANCE_QUEUE_UNKNOWN;
    snapshot->tick_trace_phase = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->tick_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->tick_link_step_status = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->tick_link_event_type = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
    snapshot->router_terminal_fault = MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN;
}

/* This transaction is used only by the ingress-gated, sole mesh owner.  The
 * separate in-progress bit rejects a second writer before validity is
 * published. */
static inline int mind_phase5_provenance_begin(
    mind_phase5_provenance_snapshot_t *snapshot,
    mind_phase5_invalid_branch_t branch, uint32_t timestamp_ms)
{
    if (snapshot == NULL || branch <= MIND_PHASE5_INVALID_BRANCH_NONE ||
        branch >= MIND_PHASE5_INVALID_BRANCH_COUNT || snapshot->valid != 0u ||
        snapshot->capture_in_progress != 0u || snapshot->logged != 0u) {
        return 0;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->timestamp_ms = timestamp_ms;
    snapshot->first_invalid_branch = branch;
    mind_phase5_provenance_set_sentinels(snapshot);
    snapshot->capture_in_progress = 1u;
    return 1;
}

/* Validity is deliberately the last store: logger code must not read a partial
 * capture, including for a fault at the initial queue guard. */
static inline void mind_phase5_provenance_publish(
    mind_phase5_provenance_snapshot_t *snapshot)
{
    if (snapshot != NULL && snapshot->capture_in_progress != 0u) {
        snapshot->capture_in_progress = 0u;
        snapshot->valid = 1u;
    }
}

/* The first logger observation consumes only the formatting right; it leaves
 * the captured evidence immutable for postmortem inspection. */
static inline int mind_phase5_provenance_mark_logged(
    mind_phase5_provenance_snapshot_t *snapshot)
{
    if (snapshot == NULL || snapshot->valid == 0u || snapshot->logged != 0u) {
        return 0;
    }
    snapshot->logged = 1u;
    return 1;
}

#endif /* MIND_PHASE5_PROVENANCE_H */
