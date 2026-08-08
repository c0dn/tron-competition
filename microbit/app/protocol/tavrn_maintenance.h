#ifndef TAVRN_MAINTENANCE_H
#define TAVRN_MAINTENANCE_H

#include <stdint.h>

#include "tavrn_gtt.h"
#include "tavrn_router.h"
#include "tavrn_subject_demand.h"

#define TAVRN_MAINTENANCE_API 1
#define TAVRN_MAINTENANCE_EXPIRY_DEMAND_API 1
#define TAVRN_MAINTENANCE_PENDING_CAPACITY 1u
#define TAVRN_MAINTENANCE_DEDUPE_CAPACITY TAVRN_GTT_CAPACITY
#define TAVRN_MAINTENANCE_EPOCH_CAPACITY TAVRN_GTT_CAPACITY

typedef char tavrn_maintenance_pending_capacity_guard[
    (TAVRN_MAINTENANCE_PENDING_CAPACITY == 1u) ? 1 : -1];
typedef char tavrn_maintenance_dedupe_capacity_guard[
    (TAVRN_MAINTENANCE_DEDUPE_CAPACITY == 16u) ? 1 : -1];
typedef char tavrn_maintenance_epoch_capacity_guard[
    (TAVRN_MAINTENANCE_EPOCH_CAPACITY == 16u) ? 1 : -1];

typedef enum tavrn_maintenance_status {
    TAVRN_MAINTENANCE_OK = 0,
    TAVRN_MAINTENANCE_GATED,
    TAVRN_MAINTENANCE_BUSY,
    TAVRN_MAINTENANCE_RX_UNIQUE,
    TAVRN_MAINTENANCE_RX_DUPLICATE,
    TAVRN_MAINTENANCE_RX_REJECTED,
    TAVRN_MAINTENANCE_IGNORED,
    TAVRN_MAINTENANCE_INVALID,
} tavrn_maintenance_status_t;

typedef struct tavrn_maintenance_config {
    uint32_t hello_change_ms;
    uint32_t hello_stable_ms;
    /* Main converts authoritative timer ratios to bounded integer permille
     * before this deterministic maintenance boundary.  All zero adaptive
     * fields retain the fixed-cadence compatibility configuration. */
    uint16_t hello_alpha_permille;
    uint16_t hello_snap_permille;
    uint32_t topology_sample_ms;
    uint32_t hello_dedupe_ms;
    uint16_t initial_node_sequence;
} tavrn_maintenance_config_t;

/* `pending` is a copied 0/1 gauge; all other members are monotonic. */
typedef struct tavrn_maintenance_counters {
    uint32_t due;
    uint32_t enqueued;
    uint32_t busy;
    uint32_t rx_unique;
    uint32_t rx_duplicate;
    uint32_t rx_rejected;
    uint32_t dedupe_capacity;
    uint32_t interval_advanced;
    uint32_t interval_snapped;
    uint32_t local_broadcast_suppressed;
    uint32_t topology_reset;
    uint32_t topology_unchanged;
    uint32_t liveness_sample;
    uint32_t liveness_floor_decayed;
    uint8_t pending;
} tavrn_maintenance_counters_t;

typedef struct tavrn_maintenance_snapshot {
    tavrn_validated_control_t pending_hello;
    uint32_t next_hello_due_ms;
    uint32_t next_topology_sample_ms;
    uint32_t current_interval_ms;
    /* Timeout is evaluated using the pre-decay floor; this retains the floor
     * for the following topology sample. */
    uint32_t liveness_floor_ms;
    uint32_t liveness_timeout_ms;
    uint16_t next_node_sequence;
    uint8_t direct_one_hop_count;
    uint8_t armed;
    uint8_t pending;
} tavrn_maintenance_snapshot_t;

typedef struct tavrn_maintenance_demand_snapshot {
    tavrn_adva_t identity;
    uint32_t gtt_revision;
    uint16_t reason_mask;
    uint8_t valid_route_to_subject;
    uint8_t stage_zero_eligible;
    uint8_t snapshot_available;
} tavrn_maintenance_demand_snapshot_t;

typedef enum tavrn_maintenance_demand_status {
    TAVRN_MAINTENANCE_DEMAND_OK = 0,
    TAVRN_MAINTENANCE_DEMAND_UNAVAILABLE,
    TAVRN_MAINTENANCE_DEMAND_INVALID,
} tavrn_maintenance_demand_status_t;

typedef enum tavrn_maintenance_checked_departure_status {
    TAVRN_GTT_CHECKED_DEPARTED = 0,
    TAVRN_GTT_CHECKED_NOT_FOUND,
    TAVRN_GTT_CHECKED_STALE_REVISION,
    TAVRN_GTT_CHECKED_NOT_HARD_EXPIRED,
    TAVRN_GTT_CHECKED_SELF,
    TAVRN_GTT_CHECKED_ALREADY_DEPARTED,
    TAVRN_GTT_CHECKED_DEMAND_DEFERRED,
    TAVRN_GTT_CHECKED_DIRECT_DEADLINE_DEFERRED,
    TAVRN_GTT_CHECKED_UNAVAILABLE,
    TAVRN_GTT_CHECKED_INVALID,
} tavrn_maintenance_checked_departure_status_t;

typedef struct tavrn_maintenance_expiry_sweep_snapshot {
    uint32_t next_due_ms;
    uint8_t trace_count;
    uint8_t trace_slots[TAVRN_GTT_CAPACITY];
    uint8_t maximum_copied_candidates;
    uint8_t soft_stale_control_count;
    uint8_t pass_performed;
    uint8_t next_cursor;
    uint8_t soft_selected_count;
    uint8_t hard_selected_count;
    uint8_t demand_deferred_count;
    uint8_t unavailable_count;
    uint8_t departed_count;
    uint8_t purged_count;
    uint8_t targeted_control_count;
    uint8_t rreq_control_count;
    uint8_t expiry_tc_control_count;
    uint32_t received_evidence_count;
    uint32_t scheduler_controls;
    uint32_t aodv_actions;
    uint32_t router_broadcast_generation;
    uint32_t mentorship_tc_generation;
} tavrn_maintenance_expiry_sweep_snapshot_t;

typedef enum tavrn_maintenance_expiry_sweep_status {
    TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK = 0,
    TAVRN_MAINTENANCE_EXPIRY_SWEEP_INVALID,
    TAVRN_MAINTENANCE_EXPIRY_SWEEP_UNAVAILABLE,
} tavrn_maintenance_expiry_sweep_status_t;

typedef struct tavrn_maintenance_owner_pre_tick_input {
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_provenance_t provenance;
    tavrn_gtt_application_command_t application_command;
    uint8_t evidence_present;
    uint8_t application_present;
} tavrn_maintenance_owner_pre_tick_input_t;

typedef struct tavrn_maintenance_owner_pre_tick_result {
    tavrn_gtt_expiry_observe_status_t observe_status;
    tavrn_gtt_application_command_result_t application_result;
    uint8_t observe_present;
    uint8_t application_present;
} tavrn_maintenance_owner_pre_tick_result_t;

typedef struct tavrn_maintenance_owner_post_tick_result {
    tavrn_maintenance_expiry_sweep_status_t sweep_status;
    tavrn_maintenance_expiry_sweep_snapshot_t sweep;
    tavrn_maintenance_status_t maintenance_status;
    tavrn_maintenance_snapshot_t maintenance;
} tavrn_maintenance_owner_post_tick_result_t;

typedef struct tavrn_maintenance_dedupe_entry {
    tavrn_adva_t origin;
    uint32_t expires_at_ms;
    uint16_t node_sequence;
    uint8_t valid;
} tavrn_maintenance_dedupe_entry_t;

/* Direct bootstrap admission is owned by router-common.  Maintenance retains
 * only the committed nonce needed to reset its own serial epoch. */
typedef struct tavrn_maintenance_epoch_entry {
    tavrn_adva_t origin;
    uint16_t boot_nonce;
    uint8_t valid;
} tavrn_maintenance_epoch_entry_t;

typedef struct tavrn_maintenance {
    tavrn_router_t *router;
    tavrn_gtt_t *gtt;
    tavrn_maintenance_config_t config;
    tavrn_maintenance_counters_t counters;
    tavrn_maintenance_snapshot_t snapshot;
    tavrn_maintenance_dedupe_entry_t
        dedupe[TAVRN_MAINTENANCE_DEDUPE_CAPACITY];
    tavrn_maintenance_epoch_entry_t epochs[TAVRN_MAINTENANCE_EPOCH_CAPACITY];
    tavrn_validated_control_t queued_hello;
    uint8_t queued_hello_valid;
    uint8_t local_broadcast_suppression_active;
    uint32_t expiry_next_due_ms;
    uint32_t expiry_received_evidence_count;
    uint32_t local_broadcast_generation_seen;
    uint8_t expiry_cursor;
    uint8_t expiry_schedule_initialized;
} tavrn_maintenance_t;

tavrn_maintenance_status_t tavrn_maintenance_init(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router, tavrn_gtt_t *gtt,
    const tavrn_maintenance_config_t *config);
/* Activation is legal only after FULL mentorship has made the router/link local
 * identity SID8 and the common router incarnation established.  It arms exactly
 * one `now + hello_change_ms` deadline and never sends retroactively. */
tavrn_maintenance_status_t tavrn_maintenance_activate(
    tavrn_maintenance_t *maintenance, uint32_t now_ms);
/* A due control is retained byte-for-byte on BUSY.  Cadence and node sequence
 * advance only after router queue admission succeeds. */
tavrn_maintenance_status_t tavrn_maintenance_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms);
/* This consumes only a copied router RX_CONTROL event.  It accepts the ordinary
 * SID8 N=0 direct HELLO shape and never re-enters router dispatch. */
tavrn_maintenance_status_t tavrn_maintenance_handle_rx_control(
    tavrn_maintenance_t *maintenance,
    const tavrn_rx_control_event_t *control_event, uint32_t now_ms);
/* The single caller reports a successfully admitted non-maintenance local
 * broadcast.  It may aggregate a copied router generation rather than invoke
 * this once per producer; maintenance's own HELLO must never use this API. */
tavrn_maintenance_status_t tavrn_maintenance_observe_local_broadcast(
    tavrn_maintenance_t *maintenance, uint32_t accepted_at_ms);
tavrn_maintenance_status_t tavrn_maintenance_snapshot(
    const tavrn_maintenance_t *maintenance,
    tavrn_maintenance_snapshot_t *snapshot_out);
const tavrn_maintenance_counters_t *tavrn_maintenance_counters(
    const tavrn_maintenance_t *maintenance);
tavrn_maintenance_demand_status_t tavrn_maintenance_demand_snapshot(
    const tavrn_maintenance_t *maintenance, const tavrn_adva_t *identity,
    uint32_t now_ms, tavrn_maintenance_demand_snapshot_t *snapshot_out);
tavrn_maintenance_checked_departure_status_t tavrn_maintenance_checked_local_departure(
    tavrn_maintenance_t *maintenance,
    const tavrn_gtt_departure_candidate_t *candidate, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out);
tavrn_maintenance_expiry_sweep_status_t tavrn_maintenance_sweep_expiry(
    tavrn_maintenance_t *maintenance, uint32_t now_ms,
    tavrn_maintenance_expiry_sweep_snapshot_t *snapshot_out);
tavrn_maintenance_owner_pre_tick_result_t tavrn_maintenance_owner_pre_tick(
    tavrn_maintenance_t *maintenance, tavrn_maintenance_owner_pre_tick_input_t input,
    uint32_t now_ms);
tavrn_maintenance_owner_post_tick_result_t tavrn_maintenance_owner_post_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms);

#endif /* TAVRN_MAINTENANCE_H */
