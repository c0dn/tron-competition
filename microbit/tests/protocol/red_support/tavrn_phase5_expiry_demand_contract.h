#ifndef TAVRN_PHASE5_EXPIRY_DEMAND_CONTRACT_H
#define TAVRN_PHASE5_EXPIRY_DEMAND_CONTRACT_H

/*
 * Frozen Step 5f production API contract.  RED supplies only unavailable
 * definitions for these exact future production symbols.  With
 * TAVRN_MAINTENANCE_EXPIRY_DEMAND_API defined, production headers/sources must
 * declare and provide every symbol directly; no test-side model or bridge is
 * permitted.
 */
#include <stdint.h>

#include "aodv_core.h"
#include "tavrn_full.h"
#include "tavrn_maintenance.h"
#include "tavrn_mentorship.h"

#if defined(TAVRN_MAINTENANCE_EXPIRY_DEMAND_API)
#include "tavrn_full_maintenance_binding.h"
#endif

#ifndef TAVRN_MAINTENANCE_EXPIRY_DEMAND_API

#ifndef TAVRN_GTT_EXPIRY_API

typedef enum tavrn_phase5_expiry_lane {
    TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER = 0,
    TAVRN_PHASE5_EXPIRY_LANE_OTHER_TASK,
} tavrn_phase5_expiry_lane_t;

typedef enum tavrn_gtt_provenance {
    TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE = 0,
    TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
    TAVRN_GTT_PROVENANCE_DIRECT_OUTER_TRANSMITTER,
    TAVRN_GTT_PROVENANCE_DIRECT_BOOTSTRAP,
    TAVRN_GTT_PROVENANCE_IMPORTED_SYNC,
    TAVRN_GTT_PROVENANCE_IMPORTED_METADATA,
    TAVRN_GTT_PROVENANCE_IMPORTED_TC_SUBJECT,
    TAVRN_GTT_PROVENANCE_CONFIRMED_DEPARTURE,
    TAVRN_GTT_PROVENANCE_DIRECT_INCARNATION,
} tavrn_gtt_provenance_t;

typedef struct tavrn_gtt_expiry_snapshot {
    tavrn_adva_t identity;
    uint32_t last_evidence_ms;
    uint32_t last_direct_evidence_ms;
    uint32_t soft_deadline_ms;
    uint32_t hard_deadline_ms;
    uint32_t departed_deadline_ms;
    uint32_t revision;
    uint32_t storage_generation;
    uint16_t serial;
    uint8_t serial_present;
    uint8_t hop_count;
    uint8_t direct;
    uint8_t application_requested;
    tavrn_gtt_freshness_t freshness;
} tavrn_gtt_expiry_snapshot_t;

typedef struct tavrn_gtt_sync_record {
    tavrn_adva_t identity;
    uint32_t remaining_lifetime_ms;
    uint16_t serial;
    uint8_t serial_present;
    uint8_t hop_count;
    uint8_t departed;
} tavrn_gtt_sync_record_t;

typedef struct tavrn_gtt_departure_candidate {
    tavrn_adva_t identity;
    uint32_t revision;
    uint32_t hard_deadline_ms;
} tavrn_gtt_departure_candidate_t;

typedef enum tavrn_gtt_expiry_observe_status {
    TAVRN_GTT_EXPIRY_OBSERVE_ADDED = 0,
    TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED,
    TAVRN_GTT_EXPIRY_OBSERVE_DUPLICATE,
    TAVRN_GTT_EXPIRY_OBSERVE_STALE,
    TAVRN_GTT_EXPIRY_OBSERVE_SELF_IGNORED,
    TAVRN_GTT_EXPIRY_OBSERVE_REPLACED_DEPARTED,
    TAVRN_GTT_EXPIRY_OBSERVE_REPLACED_OLDEST_STALE,
    TAVRN_GTT_EXPIRY_OBSERVE_NO_VICTIM,
    TAVRN_GTT_EXPIRY_OBSERVE_INVALID,
    TAVRN_GTT_EXPIRY_OBSERVE_UNAVAILABLE,
} tavrn_gtt_expiry_observe_status_t;

typedef enum tavrn_gtt_expiry_query_status {
    TAVRN_GTT_EXPIRY_QUERY_FOUND = 0,
    TAVRN_GTT_EXPIRY_QUERY_NOT_FOUND,
    TAVRN_GTT_EXPIRY_QUERY_OUTPUT_TOO_SMALL,
    TAVRN_GTT_EXPIRY_QUERY_INVALID,
    TAVRN_GTT_EXPIRY_QUERY_MARSHAL_REQUIRED,
    TAVRN_GTT_EXPIRY_QUERY_UNAVAILABLE,
} tavrn_gtt_expiry_query_status_t;

typedef enum tavrn_gtt_application_request_status {
    TAVRN_GTT_APPLICATION_CHANGED = 0,
    TAVRN_GTT_APPLICATION_UNCHANGED_DUPLICATE,
    TAVRN_GTT_APPLICATION_CANCELED,
    TAVRN_GTT_APPLICATION_SELF,
    TAVRN_GTT_APPLICATION_NOT_FOUND,
    TAVRN_GTT_APPLICATION_DEPARTED,
    TAVRN_GTT_APPLICATION_MARSHAL_REQUIRED,
    TAVRN_GTT_APPLICATION_INVALID,
    TAVRN_GTT_APPLICATION_UNAVAILABLE,
} tavrn_gtt_application_request_status_t;

typedef enum tavrn_gtt_application_command_kind {
    TAVRN_GTT_APPLICATION_COMMAND_REQUEST = 0,
    TAVRN_GTT_APPLICATION_COMMAND_CANCEL,
    TAVRN_GTT_APPLICATION_COMMAND_QUERY,
} tavrn_gtt_application_command_kind_t;

/* The application-event owner receives this complete value from another task;
 * it never receives a GTT-storage pointer or a caller-owned identity pointer. */
typedef struct tavrn_gtt_application_command {
    tavrn_adva_t identity;
    uint32_t now_ms;
    uint32_t query_time_ms;
    tavrn_gtt_application_command_kind_t kind;
} tavrn_gtt_application_command_t;

typedef struct tavrn_gtt_application_command_result {
    tavrn_gtt_application_request_status_t status;
    tavrn_gtt_expiry_query_status_t query_status;
    tavrn_gtt_expiry_snapshot_t snapshot;
} tavrn_gtt_application_command_result_t;

typedef enum tavrn_gtt_sync_merge_status {
    TAVRN_GTT_SYNC_MERGE_COMMITTED = 0,
    TAVRN_GTT_SYNC_MERGE_UNCHANGED,
    TAVRN_GTT_SYNC_MERGE_INVALID,
    TAVRN_GTT_SYNC_MERGE_UNAVAILABLE,
} tavrn_gtt_sync_merge_status_t;

#endif /* !TAVRN_GTT_EXPIRY_API */

#ifndef TAVRN_SUBJECT_DEMAND_API
enum {
    TAVRN_MAINT_DEMAND_PENDING_DATA = 1u << 0,
    TAVRN_MAINT_DEMAND_QUEUED_DATA = 1u << 1,
    TAVRN_MAINT_DEMAND_VALID_ROUTE_TO_SUBJECT = 1u << 2,
    TAVRN_MAINT_DEMAND_VALID_ROUTE_VIA_SUBJECT = 1u << 3,
    TAVRN_MAINT_DEMAND_PRECURSOR = 1u << 4,
    TAVRN_MAINT_DEMAND_DEFERRED_REPAIR = 1u << 5,
    TAVRN_MAINT_DEMAND_CUSTODY_FINAL = 1u << 6,
    TAVRN_MAINT_DEMAND_CUSTODY_NEXT_HOP = 1u << 7,
    TAVRN_MAINT_DEMAND_PENDING_INGEST = 1u << 8,
    TAVRN_MAINT_DEMAND_RETAINED_FINAL = 1u << 9,
    TAVRN_MAINT_DEMAND_RETAINED_NEXT_HOP = 1u << 10,
    TAVRN_MAINT_DEMAND_APPLICATION_REQUEST = 1u << 11,
};

typedef struct tavrn_aodv_subject_demand_snapshot {
    uint16_t reason_mask;
    uint8_t valid_route_to_subject;
    uint8_t snapshot_available;
} tavrn_aodv_subject_demand_snapshot_t;

typedef struct tavrn_link_subject_demand_snapshot {
    uint16_t reason_mask;
    uint8_t snapshot_available;
} tavrn_link_subject_demand_snapshot_t;

typedef struct tavrn_router_subject_demand_snapshot {
    uint16_t reason_mask;
    uint8_t snapshot_available;
} tavrn_router_subject_demand_snapshot_t;

#endif /* !TAVRN_SUBJECT_DEMAND_API */

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

typedef enum tavrn_full_application_mailbox_status {
    TAVRN_FULL_APPLICATION_MAILBOX_QUEUED = 0,
    TAVRN_FULL_APPLICATION_MAILBOX_PROCESSING,
    TAVRN_FULL_APPLICATION_MAILBOX_BUSY,
    TAVRN_FULL_APPLICATION_MAILBOX_EMPTY,
    TAVRN_FULL_APPLICATION_MAILBOX_READY,
    TAVRN_FULL_APPLICATION_MAILBOX_INVALID,
    TAVRN_FULL_APPLICATION_MAILBOX_UNAVAILABLE,
} tavrn_full_application_mailbox_status_t;

typedef struct tavrn_full_application_mailbox {
    tavrn_gtt_application_command_t command;
    tavrn_gtt_application_command_result_t result;
    uint8_t command_pending;
    uint8_t owner_processing;
    uint8_t result_ready;
} tavrn_full_application_mailbox_t;

/* FULL-only binding: routed main alone owns mailbox take/publish under its
 * guard, then calls this once with one copied command plus exact 0/1 presence.
 * It never receives mailbox storage. Scheduler RX is already committed; generic
 * application submit/dispatch/link service remains after this boundary. */
typedef enum tavrn_full_maintenance_binding_status {
    TAVRN_FULL_MAINTENANCE_BINDING_OK = 0,
    TAVRN_FULL_MAINTENANCE_BINDING_INVALID,
    TAVRN_FULL_MAINTENANCE_BINDING_UNAVAILABLE,
} tavrn_full_maintenance_binding_status_t;

typedef struct tavrn_full_maintenance_binding_input {
    tavrn_router_t *router;
    tavrn_mentorship_t *mentorship;
    tavrn_maintenance_t *maintenance;
    tavrn_gtt_application_command_t application_command;
    uint8_t application_present;
} tavrn_full_maintenance_binding_input_t;

typedef struct tavrn_full_maintenance_binding_result {
    tavrn_full_maintenance_binding_status_t status;
    tavrn_maintenance_owner_pre_tick_result_t pre_tick;
    tavrn_gtt_application_command_result_t application_result;
    uint8_t application_present;
    aodv_status_t router_status;
    tavrn_router_phase_trace_t router_trace;
    tavrn_mentorship_status_t mentorship_status;
    tavrn_router_local_broadcast_status_t broadcast_snapshot_status;
    tavrn_router_local_broadcast_snapshot_t broadcast;
    tavrn_maintenance_status_t broadcast_observation_status;
    tavrn_maintenance_status_t activation_status;
    tavrn_maintenance_owner_post_tick_result_t post_tick;
} tavrn_full_maintenance_binding_result_t;

tavrn_gtt_expiry_observe_status_t tavrn_gtt_observe_with_provenance(
    tavrn_gtt_t *gtt, const tavrn_gtt_evidence_t *evidence,
    tavrn_gtt_provenance_t provenance, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out);
tavrn_gtt_expiry_query_status_t tavrn_gtt_expiry_snapshot(
    const tavrn_gtt_t *gtt, const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out);
tavrn_gtt_expiry_query_status_t tavrn_gtt_enumerate_known(
    const tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshots_out, uint8_t capacity,
    uint8_t *count_out);
tavrn_gtt_application_request_status_t tavrn_gtt_application_request(
    tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane,
    const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out);
tavrn_gtt_application_request_status_t tavrn_gtt_application_cancel(
    tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane,
    const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out);
tavrn_gtt_application_command_result_t tavrn_gtt_apply_application_command(
    tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane,
    tavrn_gtt_application_command_t command);
tavrn_gtt_sync_merge_status_t tavrn_gtt_sync_merge(
    tavrn_gtt_t *gtt, const tavrn_gtt_sync_record_t *records, uint8_t record_count,
    uint32_t now_ms);
tavrn_aodv_subject_demand_snapshot_t aodv_core_subject_demand_snapshot(
    const aodv_core_t *core, const tavrn_logical_id_t *subject,
    const tavrn_adva_t *canonical_subject, uint32_t now_ms);
tavrn_link_subject_demand_snapshot_t tavrn_link_v2_subject_demand_snapshot(
    const tavrn_link_v2_t *link, const tavrn_logical_id_t *subject,
    const tavrn_adva_t *canonical_subject, uint32_t now_ms);
tavrn_router_subject_demand_snapshot_t tavrn_router_subject_demand_snapshot(
    const tavrn_router_t *router, const tavrn_logical_id_t *subject,
    const tavrn_adva_t *canonical_subject, uint32_t now_ms);
tavrn_esc_context_status_t tavrn_full_resolve_unique_sid8(
    const tavrn_full_t *full, const tavrn_adva_t *identity,
    tavrn_logical_id_t *sid8_out);
tavrn_maintenance_demand_status_t tavrn_maintenance_demand_snapshot(
    const tavrn_maintenance_t *maintenance, const tavrn_adva_t *identity,
    uint32_t now_ms, tavrn_maintenance_demand_snapshot_t *snapshot_out);
tavrn_maintenance_checked_departure_status_t tavrn_maintenance_checked_local_departure(
    tavrn_maintenance_t *maintenance, const tavrn_gtt_departure_candidate_t *candidate,
    uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out);
tavrn_maintenance_expiry_sweep_status_t tavrn_maintenance_sweep_expiry(
    tavrn_maintenance_t *maintenance, uint32_t now_ms,
    tavrn_maintenance_expiry_sweep_snapshot_t *snapshot_out);
tavrn_maintenance_owner_pre_tick_result_t tavrn_maintenance_owner_pre_tick(
    tavrn_maintenance_t *maintenance, tavrn_maintenance_owner_pre_tick_input_t input,
    uint32_t now_ms);
tavrn_maintenance_owner_post_tick_result_t tavrn_maintenance_owner_post_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms);
tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_init(
    tavrn_full_application_mailbox_t *mailbox);
tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_submit(
    tavrn_full_application_mailbox_t *mailbox, tavrn_gtt_application_command_t command);
tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_owner_take(
    tavrn_full_application_mailbox_t *mailbox, tavrn_gtt_application_command_t *command_out);
tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_owner_publish(
    tavrn_full_application_mailbox_t *mailbox, tavrn_gtt_application_command_result_t result);
tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_consumer_take(
    tavrn_full_application_mailbox_t *mailbox,
    tavrn_gtt_application_command_result_t *result_out);
tavrn_full_maintenance_binding_status_t tavrn_full_maintenance_binding_tick(
    tavrn_full_maintenance_binding_input_t input, uint32_t now_ms,
    tavrn_full_maintenance_binding_result_t *result_out);

#endif /* !TAVRN_MAINTENANCE_EXPIRY_DEMAND_API */

/* Tests use phase5_* names exclusively.  RED uses unique symbols so it keeps
 * proving absence after production provides the real names.  GREEN uses exact
 * production names; its same-name fallbacks exist only before the marker. */
#if defined(TAVRN_PHASE5_EXPIRY_RED_MODE)
#define phase5_gtt_observe phase5_red_gtt_observe
#define phase5_gtt_snapshot phase5_red_gtt_snapshot
#define phase5_gtt_enumerate_known phase5_red_gtt_enumerate_known
#define phase5_gtt_request phase5_red_gtt_request
#define phase5_gtt_cancel phase5_red_gtt_cancel
#define phase5_gtt_apply_application_command phase5_red_gtt_apply_application_command
#define phase5_gtt_sync_merge phase5_red_gtt_sync_merge
#define phase5_aodv_demand phase5_red_aodv_demand
#define phase5_link_demand phase5_red_link_demand
#define phase5_router_demand phase5_red_router_demand
#define phase5_full_resolve_sid8 phase5_red_full_resolve_sid8
#define phase5_maintenance_demand phase5_red_maintenance_demand
#define phase5_maintenance_checked_departure phase5_red_maintenance_checked_departure
#define phase5_maintenance_sweep phase5_red_maintenance_sweep
#define phase5_maintenance_owner_pre_tick phase5_red_maintenance_owner_pre_tick
#define phase5_maintenance_owner_post_tick phase5_red_maintenance_owner_post_tick
#define phase5_full_application_mailbox_init phase5_red_full_application_mailbox_init
#define phase5_full_application_mailbox_submit phase5_red_full_application_mailbox_submit
#define phase5_full_application_mailbox_owner_take phase5_red_full_application_mailbox_owner_take
#define phase5_full_application_mailbox_owner_publish phase5_red_full_application_mailbox_owner_publish
#define phase5_full_application_mailbox_consumer_take phase5_red_full_application_mailbox_consumer_take
#define phase5_full_maintenance_binding_tick phase5_red_full_maintenance_binding_tick

tavrn_gtt_expiry_observe_status_t phase5_gtt_observe(
    tavrn_gtt_t *, const tavrn_gtt_evidence_t *, tavrn_gtt_provenance_t, uint32_t,
    tavrn_gtt_expiry_snapshot_t *);
tavrn_gtt_expiry_query_status_t phase5_gtt_snapshot(
    const tavrn_gtt_t *, const tavrn_adva_t *, uint32_t, tavrn_gtt_expiry_snapshot_t *);
tavrn_gtt_expiry_query_status_t phase5_gtt_enumerate_known(
    const tavrn_gtt_t *, tavrn_phase5_expiry_lane_t, uint32_t,
    tavrn_gtt_expiry_snapshot_t *, uint8_t, uint8_t *);
tavrn_gtt_application_request_status_t phase5_gtt_request(
    tavrn_gtt_t *, tavrn_phase5_expiry_lane_t, const tavrn_adva_t *, uint32_t,
    tavrn_gtt_expiry_snapshot_t *);
tavrn_gtt_application_request_status_t phase5_gtt_cancel(
    tavrn_gtt_t *, tavrn_phase5_expiry_lane_t, const tavrn_adva_t *, uint32_t,
    tavrn_gtt_expiry_snapshot_t *);
tavrn_gtt_application_command_result_t phase5_gtt_apply_application_command(
    tavrn_gtt_t *, tavrn_phase5_expiry_lane_t, tavrn_gtt_application_command_t);
tavrn_gtt_sync_merge_status_t phase5_gtt_sync_merge(
    tavrn_gtt_t *, const tavrn_gtt_sync_record_t *, uint8_t, uint32_t);
tavrn_aodv_subject_demand_snapshot_t phase5_aodv_demand(
    const aodv_core_t *, const tavrn_logical_id_t *, const tavrn_adva_t *, uint32_t);
tavrn_link_subject_demand_snapshot_t phase5_link_demand(
    const tavrn_link_v2_t *, const tavrn_logical_id_t *, const tavrn_adva_t *, uint32_t);
tavrn_router_subject_demand_snapshot_t phase5_router_demand(
    const tavrn_router_t *, const tavrn_logical_id_t *, const tavrn_adva_t *, uint32_t);
tavrn_esc_context_status_t phase5_full_resolve_sid8(
    const tavrn_full_t *, const tavrn_adva_t *, tavrn_logical_id_t *);
tavrn_maintenance_demand_status_t phase5_maintenance_demand(
    const tavrn_maintenance_t *, const tavrn_adva_t *, uint32_t,
    tavrn_maintenance_demand_snapshot_t *);
tavrn_maintenance_checked_departure_status_t phase5_maintenance_checked_departure(
    tavrn_maintenance_t *, const tavrn_gtt_departure_candidate_t *, uint32_t,
    tavrn_gtt_expiry_snapshot_t *);
tavrn_maintenance_expiry_sweep_status_t phase5_maintenance_sweep(
    tavrn_maintenance_t *, uint32_t, tavrn_maintenance_expiry_sweep_snapshot_t *);
tavrn_maintenance_owner_pre_tick_result_t phase5_maintenance_owner_pre_tick(
    tavrn_maintenance_t *, tavrn_maintenance_owner_pre_tick_input_t, uint32_t);
tavrn_maintenance_owner_post_tick_result_t phase5_maintenance_owner_post_tick(
    tavrn_maintenance_t *, uint32_t);
tavrn_full_application_mailbox_status_t phase5_full_application_mailbox_init(
    tavrn_full_application_mailbox_t *);
tavrn_full_application_mailbox_status_t phase5_full_application_mailbox_submit(
    tavrn_full_application_mailbox_t *, tavrn_gtt_application_command_t);
tavrn_full_application_mailbox_status_t phase5_full_application_mailbox_owner_take(
    tavrn_full_application_mailbox_t *, tavrn_gtt_application_command_t *);
tavrn_full_application_mailbox_status_t phase5_full_application_mailbox_owner_publish(
    tavrn_full_application_mailbox_t *, tavrn_gtt_application_command_result_t);
tavrn_full_application_mailbox_status_t phase5_full_application_mailbox_consumer_take(
    tavrn_full_application_mailbox_t *, tavrn_gtt_application_command_result_t *);
tavrn_full_maintenance_binding_status_t phase5_full_maintenance_binding_tick(
    tavrn_full_maintenance_binding_input_t, uint32_t,
    tavrn_full_maintenance_binding_result_t *);
#else
#define phase5_gtt_observe tavrn_gtt_observe_with_provenance
#define phase5_gtt_snapshot tavrn_gtt_expiry_snapshot
#define phase5_gtt_enumerate_known tavrn_gtt_enumerate_known
#define phase5_gtt_request tavrn_gtt_application_request
#define phase5_gtt_cancel tavrn_gtt_application_cancel
#define phase5_gtt_apply_application_command tavrn_gtt_apply_application_command
#define phase5_gtt_sync_merge tavrn_gtt_sync_merge
#define phase5_aodv_demand aodv_core_subject_demand_snapshot
#define phase5_link_demand tavrn_link_v2_subject_demand_snapshot
#define phase5_router_demand tavrn_router_subject_demand_snapshot
#define phase5_full_resolve_sid8 tavrn_full_resolve_unique_sid8
#define phase5_maintenance_demand tavrn_maintenance_demand_snapshot
#define phase5_maintenance_checked_departure tavrn_maintenance_checked_local_departure
#define phase5_maintenance_sweep tavrn_maintenance_sweep_expiry
#define phase5_maintenance_owner_pre_tick tavrn_maintenance_owner_pre_tick
#define phase5_maintenance_owner_post_tick tavrn_maintenance_owner_post_tick
#define phase5_full_application_mailbox_init tavrn_full_application_mailbox_init
#define phase5_full_application_mailbox_submit tavrn_full_application_mailbox_submit
#define phase5_full_application_mailbox_owner_take tavrn_full_application_mailbox_owner_take
#define phase5_full_application_mailbox_owner_publish tavrn_full_application_mailbox_owner_publish
#define phase5_full_application_mailbox_consumer_take tavrn_full_application_mailbox_consumer_take
#define phase5_full_maintenance_binding_tick tavrn_full_maintenance_binding_tick
#endif

#endif /* TAVRN_PHASE5_EXPIRY_DEMAND_CONTRACT_H */
