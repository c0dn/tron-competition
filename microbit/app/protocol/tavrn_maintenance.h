#ifndef TAVRN_MAINTENANCE_H
#define TAVRN_MAINTENANCE_H

#include <stdint.h>

#include "tavrn_gtt.h"
#include "tavrn_router.h"
#include "tavrn_subject_demand.h"

#define TAVRN_MAINTENANCE_API 1
#define TAVRN_MAINTENANCE_EXPIRY_DEMAND_API 1
#define TAVRN_MAINTENANCE_TARGETED_FRESHNESS_API 1
#define TAVRN_MAINTENANCE_RREQ_VERIFICATION_API 1
#define TAVRN_MAINTENANCE_TC_METADATA_API 1
#define TAVRN_TC_METADATA_CANDIDATE_CAPACITY 4u
#define TAVRN_TC_METADATA_UUID_CAPACITY TAVRN_GTT_CAPACITY
#define TAVRN_TC_METADATA_SUBJECT_CAPACITY TAVRN_GTT_CAPACITY
#define TAVRN_TC_METADATA_ORIGIN_CAPACITY TAVRN_GTT_CAPACITY
#define TAVRN_TC_METADATA_RELAY_CAPACITY TAVRN_TC_METADATA_UUID_CAPACITY
#define TAVRN_TC_METADATA_RETRY_EXHAUSTED_LEAVE_OVERFLOW_CAPACITY 1u
#define TAVRN_METADATA_RESERVATION_SLOT_MASK 0x0003u
#define TAVRN_METADATA_RESERVATION_CONTEXT_SHIFT 2u
#define TAVRN_METADATA_RESERVATION_GENERATION_SHIFT 10u
#define TAVRN_METADATA_RESERVATION_GENERATION_MAX 0x003fffffu
#define TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY 4u
#define TAVRN_TARGETED_FRESHNESS_DEDUPE_CAPACITY TAVRN_MAINTENANCE_DEDUPE_CAPACITY
#define TAVRN_TARGETED_FRESHNESS_CANDIDATE_CAPACITY TAVRN_TC_METADATA_CANDIDATE_CAPACITY
#define TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY 4u
#define TAVRN_TARGETED_RESPONSE_CONTEXT_INDEX_BASE 0x80u
#define TAVRN_MAINTENANCE_PENDING_CAPACITY 1u
#define TAVRN_MAINTENANCE_DEDUPE_CAPACITY TAVRN_GTT_CAPACITY
#define TAVRN_MAINTENANCE_EPOCH_CAPACITY TAVRN_GTT_CAPACITY
#define TAVRN_MAINTENANCE_EXTERNAL_HIGH_TOKEN_OWNER_CAPACITY 1u

typedef char tavrn_maintenance_pending_capacity_guard[
    (TAVRN_MAINTENANCE_PENDING_CAPACITY == 1u) ? 1 : -1];
typedef char tavrn_maintenance_dedupe_capacity_guard[
    (TAVRN_MAINTENANCE_DEDUPE_CAPACITY == 16u) ? 1 : -1];
typedef char tavrn_maintenance_epoch_capacity_guard[
    (TAVRN_MAINTENANCE_EPOCH_CAPACITY == 16u) ? 1 : -1];
typedef char tavrn_maintenance_external_high_token_owner_capacity_guard[
    (TAVRN_MAINTENANCE_EXTERNAL_HIGH_TOKEN_OWNER_CAPACITY == 1u) ? 1 : -1];
typedef char tavrn_tc_metadata_candidate_capacity_guard[
    (TAVRN_TC_METADATA_CANDIDATE_CAPACITY == 4u) ? 1 : -1];
typedef char tavrn_tc_metadata_origin_capacity_guard[
    (TAVRN_TC_METADATA_ORIGIN_CAPACITY == TAVRN_GTT_CAPACITY &&
     TAVRN_TC_METADATA_ORIGIN_CAPACITY == 16u) ? 1 : -1];
typedef char tavrn_tc_metadata_relay_capacity_guard[
    (TAVRN_TC_METADATA_RELAY_CAPACITY == TAVRN_TC_METADATA_UUID_CAPACITY &&
     TAVRN_TC_METADATA_RELAY_CAPACITY == 16u) ? 1 : -1];
typedef char tavrn_tc_metadata_retry_exhausted_leave_overflow_capacity_guard[
    (TAVRN_TC_METADATA_RETRY_EXHAUSTED_LEAVE_OVERFLOW_CAPACITY == 1u) ? 1 : -1];

/* DEV-027 is owned by FULL maintenance.  These types deliberately live here,
 * not in AODV or link-v2: generic router ports exchange only copied controls
 * and terminal facts, while this owner retains the full-identity policy. */
typedef enum tavrn_tc_metadata_status {
    TAVRN_TC_METADATA_OK = 0,
    TAVRN_TC_METADATA_PREPARED,
    TAVRN_TC_METADATA_RETAINED,
    TAVRN_TC_METADATA_APPLIED,
    TAVRN_TC_METADATA_IGNORED,
    TAVRN_TC_METADATA_BUSY,
    TAVRN_TC_METADATA_INVALID,
    TAVRN_TC_METADATA_UNAVAILABLE,
} tavrn_tc_metadata_status_t;

typedef enum tavrn_tc_event {
    TAVRN_TC_EVENT_JOIN = 0,
    TAVRN_TC_EVENT_LEAVE = 1,
} tavrn_tc_event_t;

typedef enum tavrn_tc_origin_cause {
    TAVRN_TC_ORIGIN_MENTORSHIP_SELF_JOIN = 0,
    TAVRN_TC_ORIGIN_VERIFICATION_TERMINAL_DEPARTURE,
    TAVRN_TC_ORIGIN_DIRECT_TIMEOUT_DEPARTURE,
    /* Retained only so stale callers receive an ignored typed result; these
     * causes are not LEAVE origins. */
    TAVRN_TC_ORIGIN_DATA_RETRY_EXHAUSTED,
    TAVRN_TC_ORIGIN_NO_DEMAND_EXPIRY,
    TAVRN_TC_ORIGIN_RREQ_EXHAUSTED,
    TAVRN_TC_ORIGIN_BUSY,
    TAVRN_TC_ORIGIN_REJECTED,
    TAVRN_TC_ORIGIN_ENQUEUE_FAILURE,
    TAVRN_TC_ORIGIN_ZERO_CHANNEL_FAILURE,
    TAVRN_TC_ORIGIN_DEADLINE_EXPIRY,
    TAVRN_TC_ORIGIN_RREP_ACK_TIMEOUT,
    TAVRN_TC_ORIGIN_RADIO_FAULT,
    TAVRN_TC_ORIGIN_SERVICE_FAULT,
    TAVRN_TC_ORIGIN_REPAIR_FAILURE,
} tavrn_tc_origin_cause_t;

typedef enum tavrn_tc_admission {
    TAVRN_TC_ADMISSION_ADMITTED = 0,
    TAVRN_TC_ADMISSION_BUSY,
    TAVRN_TC_ADMISSION_REJECTED,
    TAVRN_TC_ADMISSION_ENQUEUE_FAILED,
    TAVRN_TC_ADMISSION_ZERO_CHANNEL_FAILED,
    TAVRN_TC_ADMISSION_LOCAL_NOT_ATTEMPTED,
    TAVRN_TC_ADMISSION_EVICTED,
} tavrn_tc_admission_t;

typedef enum tavrn_metadata_kind {
    TAVRN_METADATA_SOFT_REQUEST = 0,
    TAVRN_METADATA_SUBJECT_SELF_ANSWER,
    TAVRN_METADATA_INTERMEDIARY_ANSWER,
    TAVRN_METADATA_DELAYED_TARGETED_RESERVATION,
} tavrn_metadata_kind_t;

typedef enum tavrn_metadata_frame_kind {
    TAVRN_METADATA_FRAME_RREQ8 = 0,
    TAVRN_METADATA_FRAME_RREP8,
    TAVRN_METADATA_FRAME_RERR8,
    TAVRN_METADATA_FRAME_RREQ16,
    TAVRN_METADATA_FRAME_RREP16,
    TAVRN_METADATA_FRAME_RERR16,
    TAVRN_METADATA_FRAME_DATA,
    TAVRN_METADATA_FRAME_HELLO,
    TAVRN_METADATA_FRAME_RREP_ACK,
    TAVRN_METADATA_FRAME_SYNC,
    TAVRN_METADATA_FRAME_TC,
} tavrn_metadata_frame_kind_t;

typedef struct tavrn_tc_metadata_config {
    tavrn_adva_t local_identity;
    uint16_t initial_tc_sequence;
    uint32_t tc_uuid_ms;
    uint32_t tc_subject_ms;
    uint32_t metadata_cooldown_ms;
    uint8_t network_id;
} tavrn_tc_metadata_config_t;

typedef struct tavrn_tc_sequence_ticket {
    uint16_t sequence;
    tavrn_tc_event_t event;
    uint8_t valid;
} tavrn_tc_sequence_ticket_t;

typedef struct tavrn_tc_metadata_action {
    uint8_t pdu[TAVRN_LINK_CONTROL_PDU_MAX];
    tavrn_adva_t origin;
    tavrn_adva_t subject;
    uint16_t tc_sequence;
    /* Local verified/direct-timeout LEAVEs carry the exact tombstone revision
     * which authorized them.  JOINs and relayed controls retain zero. */
    uint32_t expected_gtt_revision;
    uint8_t ttl;
    uint8_t hops;
    tavrn_tc_event_t event;
    uint8_t valid;
} tavrn_tc_metadata_action_t;

typedef struct tavrn_metadata_candidate {
    tavrn_adva_t subject;
    uint8_t subject_sid8;
    uint8_t ttl_bucket;
    uint8_t freshness_request;
    uint8_t departed;
    tavrn_metadata_kind_t kind;
} tavrn_metadata_candidate_t;

typedef struct tavrn_metadata_selection {
    tavrn_metadata_candidate_t entries[TAVRN_TC_METADATA_CANDIDATE_CAPACITY];
    uint8_t count;
    uint8_t capacity;
    uint8_t valid;
} tavrn_metadata_selection_t;

typedef struct tavrn_metadata_attached_control {
    tavrn_validated_control_t control;
    tavrn_metadata_selection_t emitted;
} tavrn_metadata_attached_control_t;

/* Router augmentation has no per-dispatch object.  The sole FULL owner retains
 * this bounded transaction so retry callbacks can commit the exact bytes and
 * candidate selection that prepare emitted. */
typedef struct tavrn_metadata_pending_transaction {
    tavrn_validated_control_t base;
    tavrn_metadata_attached_control_t attached;
    tavrn_direct_peer_t next_hop;
    ble_mesh_tx_token_t scheduler_token;
    uint8_t controlled_flood;
    uint8_t admission_started;
    uint8_t retry_pending;
    uint8_t terminal_failure;
    uint8_t valid;
} tavrn_metadata_pending_transaction_t;

typedef struct tavrn_metadata_rx {
    tavrn_metadata_frame_kind_t frame_kind;
    tavrn_adva_t outer_transmitter;
    tavrn_metadata_candidate_t entries[TAVRN_TC_METADATA_CANDIDATE_CAPACITY + 1u];
    uint8_t sid8;
    uint8_t metadata_flag;
    uint8_t count;
    uint8_t unreachable_count;
    uint8_t stale;
} tavrn_metadata_rx_t;

typedef struct tavrn_tc_metadata_uuid_entry {
    tavrn_adva_t origin;
    uint16_t sequence;
    uint32_t expires_at_ms;
    uint8_t valid;
} tavrn_tc_metadata_uuid_entry_t;

typedef struct tavrn_tc_metadata_subject_entry {
    tavrn_adva_t subject;
    uint32_t expires_at_ms;
    tavrn_tc_event_t event;
    uint8_t valid;
} tavrn_tc_metadata_subject_entry_t;

typedef struct tavrn_tc_metadata_slot {
    tavrn_metadata_candidate_t candidate;
    uint8_t targeted_context_index;
    uint32_t delayed_generation;
    uint8_t valid;
} tavrn_tc_metadata_slot_t;

typedef uint32_t tavrn_metadata_reservation_handle_t;

typedef struct tavrn_tc_metadata_origin_fact {
    tavrn_adva_t subject;
    /* Nonzero only for a locally originated LEAVE.  It survives every FIFO
     * promotion/retry so an old tombstone cannot outlive resurrection. */
    uint32_t expected_gtt_revision;
    tavrn_tc_event_t event;
    uint8_t valid;
} tavrn_tc_metadata_origin_fact_t;

/* Compatibility-only storage retained for the mentorship RFI preemption
 * predicate.  Failed-hop scheduling never populates it. */
typedef struct tavrn_tc_metadata_retry_exhausted_leave_overflow {
    tavrn_adva_t failed_next_hop;
    uint8_t valid;
} tavrn_tc_metadata_retry_exhausted_leave_overflow_t;

typedef struct tavrn_tc_metadata_cooldown {
    tavrn_metadata_candidate_t candidate;
    uint32_t expires_at_ms;
    uint8_t valid;
} tavrn_tc_metadata_cooldown_t;

/* Callback-local transactions may never copy this owner.  The GTT rollback
 * image and the four metadata/cooldown entries live in the one maintenance
 * owner, so bounded RX handling has only scalar stack locals. */
typedef struct tavrn_tc_metadata_transaction {
    tavrn_tc_metadata_slot_t slots[TAVRN_TC_METADATA_CANDIDATE_CAPACITY];
    tavrn_tc_metadata_cooldown_t cooldown[TAVRN_TC_METADATA_CANDIDATE_CAPACITY];
    tavrn_gtt_storage_t gtt_storage;
    uint32_t imported_non_direct_count;
    uint8_t round_robin_cursor;
} tavrn_tc_metadata_transaction_t;

typedef struct tavrn_tc_metadata_snapshot {
    uint16_t next_tc_sequence;
    uint32_t uuid_entries;
    uint32_t subject_entries;
    uint32_t local_apply_count;
    uint32_t relay_count;
    uint32_t imported_non_direct_count;
    uint8_t candidate_count;
    uint8_t round_robin_cursor;
    uint8_t cooldown_count;
} tavrn_tc_metadata_snapshot_t;

/* One copied FULL-only logger record.  It exposes bounded ownership state, not
 * raw scheduler/radio details, so the mesh task never prints it directly. */
typedef struct tavrn_tc_metadata_telemetry {
    uint32_t tc_origin_busy_count;
    uint32_t tc_relay_busy_count;
    uint32_t tc_apply_count;
    uint32_t tc_duplicate_count;
    uint32_t tc_relay_count;
    uint32_t tc_admission_count;
    uint32_t metadata_completion_commit_count;
    uint32_t metadata_completion_retry_count;
    uint32_t metadata_completion_failure_count;
    uint16_t current_tc_sequence;
    uint16_t last_tc_sequence;
    tavrn_tc_event_t current_tc_event;
    tavrn_tc_event_t last_tc_event;
    uint8_t current_subject_sid8;
    uint8_t last_subject_sid8;
    tavrn_wire_type_t metadata_base_type;
    uint8_t metadata_emitted_count;
    uint8_t metadata_pdu_len;
    uint8_t origin_queue_depth;
    uint8_t relay_queue_depth;
    uint8_t metadata_transaction_active;
} tavrn_tc_metadata_telemetry_t;

typedef struct tavrn_tc_metadata_state {
    tavrn_tc_metadata_config_t config;
    tavrn_tc_metadata_uuid_entry_t uuid[TAVRN_TC_METADATA_UUID_CAPACITY];
    tavrn_tc_metadata_subject_entry_t subject[TAVRN_TC_METADATA_SUBJECT_CAPACITY];
    tavrn_tc_metadata_slot_t slots[TAVRN_TC_METADATA_CANDIDATE_CAPACITY];
    tavrn_tc_metadata_cooldown_t cooldown[TAVRN_TC_METADATA_CANDIDATE_CAPACITY];
    tavrn_tc_metadata_transaction_t metadata_transaction;
    tavrn_tc_metadata_action_t origin_pending;
    tavrn_tc_metadata_action_t relay_pending;
    tavrn_tc_metadata_origin_fact_t
        origin_facts[TAVRN_TC_METADATA_ORIGIN_CAPACITY];
    tavrn_tc_metadata_retry_exhausted_leave_overflow_t
        retry_exhausted_leave_overflow;
    tavrn_tc_metadata_action_t
        relay_backlog[TAVRN_TC_METADATA_RELAY_CAPACITY - 1u];
    tavrn_tc_sequence_ticket_t sequence_ticket;
    tavrn_gtt_t *gtt;
    uint32_t local_apply_count;
    uint32_t relay_count;
    uint32_t imported_non_direct_count;
    uint32_t duplicate_count;
    uint32_t origin_busy_count;
    uint32_t relay_busy_count;
    uint32_t admission_count;
    uint16_t next_tc_sequence;
    uint16_t last_tc_sequence;
    tavrn_tc_event_t last_tc_event;
    uint8_t last_subject_sid8;
    uint8_t round_robin_cursor;
    uint8_t origin_fact_count;
    uint8_t relay_queue_count;
    uint32_t delayed_generation[TAVRN_TC_METADATA_CANDIDATE_CAPACITY];
    uint8_t initialized;
} tavrn_tc_metadata_state_t;

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

/* FULL policy may synchronously preempt a private RFI transaction once a local
 * TC fact is retained.  Maintenance owns only this copied optional port, never
 * a mentorship type or symbol; an unbound port deliberately does nothing. */
typedef void (*tavrn_maintenance_local_tc_retained_fn)(void *context,
                                                        uint32_t now_ms);

typedef struct tavrn_maintenance_local_tc_retained_port {
    tavrn_maintenance_local_tc_retained_fn callback;
    void *context;
} tavrn_maintenance_local_tc_retained_port_t;

/* The optional repair owner crosses this generic FULL boundary by copied
 * discriminators only.  Maintenance deliberately has no repair dependency and
 * the single registration never creates a second token domain. */
typedef uint16_t tavrn_maintenance_high_token_owner_t;
typedef uint16_t tavrn_maintenance_high_token_purpose_t;

typedef enum tavrn_maintenance_high_token_status {
    TAVRN_MAINTENANCE_HIGH_TOKEN_OK = 0,
    TAVRN_MAINTENANCE_HIGH_TOKEN_BUSY,
    TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID,
    TAVRN_MAINTENANCE_HIGH_TOKEN_NOT_FOUND,
    TAVRN_MAINTENANCE_HIGH_TOKEN_MISMATCH,
} tavrn_maintenance_high_token_status_t;

typedef enum tavrn_maintenance_high_token_completion_kind {
    TAVRN_MAINTENANCE_HIGH_TOKEN_COMPLETION_TX_DONE = 0,
    TAVRN_MAINTENANCE_HIGH_TOKEN_COMPLETION_TX_FAILED,
    TAVRN_MAINTENANCE_HIGH_TOKEN_COMPLETION_RADIO_FAULT,
    TAVRN_MAINTENANCE_HIGH_TOKEN_COMPLETION_SERVICE_FAULT,
} tavrn_maintenance_high_token_completion_kind_t;

typedef struct tavrn_maintenance_high_token_completion {
    tavrn_maintenance_high_token_owner_t owner;
    tavrn_maintenance_high_token_purpose_t purpose;
    uint16_t token;
    tavrn_maintenance_high_token_completion_kind_t kind;
    uint8_t requested_channel_mask;
    uint8_t completed_channel_mask;
    uint32_t completed_at_ms;
} tavrn_maintenance_high_token_completion_t;

typedef tavrn_maintenance_high_token_status_t
    (*tavrn_maintenance_high_token_completion_fn)(
        void *context,
        const tavrn_maintenance_high_token_completion_t *completion);

typedef struct tavrn_maintenance_external_high_token_registration {
    tavrn_maintenance_high_token_owner_t owner;
    tavrn_maintenance_high_token_purpose_t purpose;
    uint16_t token;
    tavrn_maintenance_high_token_completion_fn completion;
    void *context;
    uint8_t valid;
} tavrn_maintenance_external_high_token_registration_t;

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
    /* Selected generated timer values supplied by the FULL binding.  Production
     * initialization requires all three explicitly; legacy host fixtures may
     * opt into the linked test defaults. */
    uint32_t aodv_net_traversal_ms;
    uint32_t freshness_response_min_ms;
    uint32_t freshness_response_max_ms;
    uint32_t metadata_cooldown_ms;
    uint32_t tc_uuid_ms;
    uint32_t tc_subject_ms;
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
    uint32_t targeted_owner_ticks;
    uint32_t targeted_owner_enqueued;
    uint32_t targeted_rx;
    uint32_t targeted_scheduler_events;
    uint32_t targeted_invalid;
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

typedef enum tavrn_maintenance_failed_hop_verification_status {
    TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED = 0,
    TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_COALESCED,
    TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_BUSY,
    TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_RETIRED_NOT_FOUND,
    TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_RETIRED_DEPARTED,
    TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_INVALID,
    TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_UNAVAILABLE,
} tavrn_maintenance_failed_hop_verification_status_t;

typedef struct tavrn_maintenance_failed_hop_verification_snapshot {
    tavrn_adva_t subject;
    uint8_t pending;
} tavrn_maintenance_failed_hop_verification_snapshot_t;

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
    uint8_t final_target;
    uint8_t subject;
    uint32_t expires_at_ms;
    uint16_t node_sequence;
    uint8_t targeted;
    uint8_t request;
    uint8_t valid;
} tavrn_maintenance_dedupe_entry_t;

typedef enum tavrn_targeted_freshness_work_kind {
    TAVRN_TARGETED_WORK_NONE = 0,
    TAVRN_TARGETED_WORK_LOCAL_REQUEST,
    TAVRN_TARGETED_WORK_REQUEST_RELAY,
    TAVRN_TARGETED_WORK_TARGET_RESPONSE,
    TAVRN_TARGETED_WORK_INTERMEDIARY_RESPONSE,
    TAVRN_TARGETED_WORK_RESPONSE_RELAY,
} tavrn_targeted_freshness_work_kind_t;

typedef enum tavrn_targeted_freshness_stage {
    TAVRN_TARGETED_STAGE_NONE = 0,
    TAVRN_TARGETED_STAGE0_READY,
    TAVRN_TARGETED_STAGE0_WAIT_RESPONSE,
    TAVRN_TARGETED_STAGE1_READY,
    TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE,
    TAVRN_TARGETED_STAGE1_WAIT_RESPONSE,
    TAVRN_TARGETED_STAGE2_READY,
    TAVRN_TARGETED_STAGE2_WAIT_RESPONSE,
    TAVRN_TARGETED_STAGE_WAIT_DIRECT_DEADLINE,
    TAVRN_TARGETED_STAGE_DEPARTED,
    TAVRN_TARGETED_STAGE_DEPARTURE_DEFERRED,
    TAVRN_TARGETED_STAGE_LEAVE_RETRY,
} tavrn_targeted_freshness_stage_t;

typedef enum tavrn_rreq_verification_status {
    TAVRN_RREQ_VERIFICATION_OK = 0,
    TAVRN_RREQ_VERIFICATION_BUSY,
    TAVRN_RREQ_VERIFICATION_RATE_DEFERRED,
    TAVRN_RREQ_VERIFICATION_IGNORED,
    TAVRN_RREQ_VERIFICATION_CANCELED,
    TAVRN_RREQ_VERIFICATION_INVALID,
    TAVRN_RREQ_VERIFICATION_UNAVAILABLE,
} tavrn_rreq_verification_status_t;

typedef enum tavrn_rreq_verification_purpose {
    TAVRN_RREQ_VERIFICATION_PURPOSE_NONE = 0,
    TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE1_RETAINED_HOP,
    TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE2_FULL_DIAMETER,
} tavrn_rreq_verification_purpose_t;

typedef enum tavrn_rreq_verification_stage {
    TAVRN_RREQ_VERIFICATION_STAGE_NONE = 0,
    TAVRN_RREQ_VERIFICATION_STAGE1_READY,
    TAVRN_RREQ_VERIFICATION_STAGE1_WAIT_RESPONSE,
    TAVRN_RREQ_VERIFICATION_STAGE2_READY,
    TAVRN_RREQ_VERIFICATION_STAGE2_WAIT_RESPONSE,
    TAVRN_RREQ_VERIFICATION_WAIT_DIRECT_DEADLINE,
    TAVRN_RREQ_VERIFICATION_DEPARTED,
    TAVRN_RREQ_VERIFICATION_DEPARTURE_DEFERRED,
    TAVRN_RREQ_VERIFICATION_LEAVE_RETRY,
} tavrn_rreq_verification_stage_t;

typedef enum tavrn_rreq_verification_terminal_kind {
    TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE = 0,
    TAVRN_RREQ_VERIFICATION_TERMINAL_TX_FAILED,
    TAVRN_RREQ_VERIFICATION_TERMINAL_EVICTED,
    TAVRN_RREQ_VERIFICATION_TERMINAL_LOCAL_NOT_ATTEMPTED,
} tavrn_rreq_verification_terminal_kind_t;

typedef struct tavrn_rreq_verification_action {
    aodv_action_t aodv_action;
    aodv_rreq_attempt_t attempt;
    tavrn_adva_t subject;
    uint16_t token;
    tavrn_rreq_verification_purpose_t purpose;
    uint8_t enqueued;
} tavrn_rreq_verification_action_t;

typedef struct tavrn_rreq_verification_completion {
    aodv_rreq_attempt_t attempt;
    uint16_t token;
    tavrn_rreq_verification_purpose_t purpose;
    tavrn_rreq_verification_terminal_kind_t kind;
    uint8_t completed_channel_mask;
} tavrn_rreq_verification_completion_t;

/* Every field is copied at the dispatcher boundary.  `handled` identifies the
 * owner that matched this event, while the status preserves that owner's exact
 * result for a future fail-closed runtime integration. */
typedef struct tavrn_maintenance_high_token_dispatch_result {
    tavrn_targeted_freshness_status_t targeted_status;
    tavrn_rreq_verification_status_t verification_status;
    tavrn_maintenance_high_token_status_t external_status;
    uint8_t targeted_handled;
    uint8_t verification_handled;
    uint8_t external_handled;
} tavrn_maintenance_high_token_dispatch_result_t;

typedef struct tavrn_rreq_verification_context_snapshot {
    tavrn_adva_t subject;
    uint32_t expected_gtt_revision;
    uint32_t response_deadline_ms;
    uint32_t direct_evidence_deadline_ms;
    uint32_t verification_deadline_ms;
    uint16_t stage1_request_id;
    uint16_t stage2_request_id;
    uint16_t token;
    uint8_t retained_hop;
    uint8_t direct_subject;
    uint8_t valid;
    tavrn_rreq_verification_stage_t stage;
} tavrn_rreq_verification_context_snapshot_t;

typedef struct tavrn_rreq_verification_snapshot {
    tavrn_rreq_verification_context_snapshot_t
        contexts[TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY];
    uint32_t rreq_rate_deferrals;
    uint32_t checked_departures;
    uint8_t active_contexts;
    uint8_t ordinary_pending_data_created;
    uint8_t ordinary_inner_retry_created;
    uint8_t ordinary_smart_ttl_fallback_created;
    uint8_t expiry_tc_or_leave_created;
} tavrn_rreq_verification_snapshot_t;

typedef struct tavrn_targeted_freshness_context_snapshot {
    tavrn_adva_t origin;
    tavrn_adva_t subject;
    tavrn_direct_peer_t receiver;
    tavrn_targeted_freshness_work_kind_t work_kind;
    tavrn_targeted_freshness_stage_t stage;
    uint32_t obligation_started_ms;
    uint32_t obligation_deadline_ms;
    uint32_t not_before_ms;
    uint16_t node_sequence;
    uint16_t token;
    uint8_t request_bucket;
    uint8_t response_bucket;
    uint8_t queued;
    uint8_t in_flight;
    uint8_t valid;
} tavrn_targeted_freshness_context_snapshot_t;

typedef struct tavrn_targeted_freshness_snapshot {
    tavrn_targeted_freshness_context_snapshot_t
        contexts[TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY];
    tavrn_targeted_freshness_context_snapshot_t
        response_contexts[TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY];
    uint16_t next_node_sequence;
    uint16_t reservation_frontier;
    uint8_t active_contexts;
    uint8_t active_response_contexts;
    uint8_t dedupe_live;
    uint8_t ordinary_pending;
} tavrn_targeted_freshness_snapshot_t;

typedef struct tavrn_targeted_freshness_action {
    tavrn_validated_control_t control;
    uint16_t token;
    uint8_t context_index;
    uint8_t enqueued;
} tavrn_targeted_freshness_action_t;

typedef struct tavrn_targeted_freshness_rx_input {
    tavrn_rx_control_event_t control_event;
} tavrn_targeted_freshness_rx_input_t;

typedef struct tavrn_targeted_freshness_context {
    tavrn_targeted_freshness_context_snapshot_t snapshot;
    tavrn_validated_control_t control;
    aodv_rreq_attempt_t stage1_attempt;
    aodv_rreq_attempt_t stage2_attempt;
    uint32_t verification_expected_gtt_revision;
    uint32_t verification_response_deadline_ms;
    uint32_t verification_direct_evidence_deadline_ms;
    uint32_t verification_deadline_ms;
    uint8_t verification_retained_hop;
    uint8_t verification_direct_subject;
    uint8_t verification_failed_hop;
    tavrn_rreq_verification_purpose_t verification_purpose;
    uint8_t dedupe_request;
    tavrn_metadata_reservation_handle_t candidate_reservation;
    uint8_t retry_pending;
} tavrn_targeted_freshness_context_t;

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
    tavrn_maintenance_local_tc_retained_port_t local_tc_retained;
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
    tavrn_targeted_freshness_context_t
        targeted[TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY];
    tavrn_targeted_freshness_context_t
        targeted_response[TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY];
    /* The physical four-slot candidate storage is tc_metadata.slots.  Delayed
     * targeted work holds one of those slots by context index; it is never a
     * second reservation array. */
    tavrn_tc_metadata_state_t tc_metadata;
    tavrn_metadata_pending_transaction_t metadata_pending;
    tavrn_maintenance_external_high_token_registration_t external_high_token;
    tavrn_maintenance_failed_hop_verification_snapshot_t failed_hop;
    uint16_t targeted_next_token;
    uint16_t targeted_reservation_frontier;
    uint8_t targeted_frontier_valid;
    uint32_t verification_rreq_rate_deferrals;
    uint32_t verification_checked_departures;
    uint32_t metadata_completion_commit_count;
    uint32_t metadata_completion_retry_count;
    uint32_t metadata_completion_failure_count;
    uint32_t metadata_frozen_rx_count;
    tavrn_wire_type_t metadata_last_base_type;
    uint8_t metadata_last_emitted_count;
    uint8_t metadata_last_pdu_len;
} tavrn_maintenance_t;

tavrn_maintenance_status_t tavrn_maintenance_init(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router, tavrn_gtt_t *gtt,
    const tavrn_maintenance_config_t *config);
/* Copies a FULL-owned optional local-TC port.  Passing NULL clears it; a
 * copied NULL callback is also a valid no-op binding. */
tavrn_maintenance_status_t tavrn_maintenance_set_local_tc_retained_port(
    tavrn_maintenance_t *maintenance,
    const tavrn_maintenance_local_tc_retained_port_t *port_or_null);
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
tavrn_maintenance_failed_hop_verification_status_t
tavrn_maintenance_schedule_failed_hop_verification(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *failed_next_hop,
    uint32_t now_ms);
tavrn_maintenance_failed_hop_verification_status_t
tavrn_maintenance_failed_hop_verification_owner_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms);
tavrn_maintenance_failed_hop_verification_status_t
tavrn_maintenance_failed_hop_verification_snapshot(
    const tavrn_maintenance_t *maintenance,
    tavrn_maintenance_failed_hop_verification_snapshot_t *snapshot_out);
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

tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_begin_stage0(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, const tavrn_adva_t *subject,
    uint32_t now_ms, tavrn_targeted_freshness_action_t *action_out);
/* FULL bindings classify copied HELLO RX with this strict shape before calling
 * the ordinary `0x80` maintenance path. */
int tavrn_maintenance_is_targeted_control(
    const tavrn_rx_control_event_t *control_event);
/* Malformed targeted controls are INVALID.  Structurally valid controls for a
 * different immediate receiver are harmless overheard traffic and are DROPPED. */
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_receive(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt,
    const tavrn_targeted_freshness_rx_input_t *input, uint32_t now_ms,
    tavrn_targeted_freshness_action_t *action_out);
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_owner_tick(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, uint32_t now_ms,
    tavrn_targeted_freshness_action_t *action_out);
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_revalidate(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, const tavrn_adva_t *subject,
    uint32_t now_ms);
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_terminal(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, const ble_mesh_sched_event_t *event,
    uint32_t now_ms);
/* Raw scheduler bridge for FULL bindings.  Non-targeted tokens and unrelated
 * events are no-ops; fault events first cancel exact queued tracked controls. */
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_scheduler_event(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, const ble_mesh_sched_event_t *event,
    uint32_t now_ms);
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_cancel_subject(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, const tavrn_adva_t *subject,
    uint32_t now_ms);
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_evicted(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, uint16_t token, uint32_t now_ms);
#if defined(BLE_RADIO_HOST_TEST)
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_seed_high_token(
    tavrn_maintenance_t *maintenance, uint16_t next_token);
#endif
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_snapshot(
    const tavrn_maintenance_t *maintenance, const tavrn_router_t *router,
    const tavrn_link_v2_t *link, const tavrn_gtt_t *gtt,
    tavrn_targeted_freshness_snapshot_t *snapshot_out);

tavrn_rreq_verification_status_t tavrn_maintenance_verification_owner_tick(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, uint32_t now_ms,
    tavrn_rreq_verification_action_t *action_out);
tavrn_rreq_verification_status_t tavrn_maintenance_verification_terminal(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link,
    const tavrn_rreq_verification_completion_t *completion, uint32_t now_ms);
/* Consume only a uniquely correlated, normally admitted RREP for a live
 * retained verification attempt. Remote malformed or unrelated RREPs leave all
 * maintenance state untouched and return IGNORED. */
tavrn_rreq_verification_status_t tavrn_maintenance_verification_receive_rrep(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt,
    const tavrn_rx_control_event_t *control_event, uint32_t now_ms);
tavrn_rreq_verification_status_t tavrn_maintenance_verification_scheduler_event(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, const ble_mesh_sched_event_t *event,
    uint32_t now_ms);

/* Reserve and release are the only public external-owner lifecycle.  A
 * release is exact-owner/purpose/token only and cannot clear a token that the
 * link or scheduler still tracks. */
tavrn_maintenance_high_token_status_t tavrn_maintenance_high_token_reserve(
    tavrn_maintenance_t *maintenance, tavrn_link_v2_t *link,
    tavrn_maintenance_high_token_owner_t owner,
    tavrn_maintenance_high_token_purpose_t purpose,
    tavrn_maintenance_high_token_completion_fn completion, void *context,
    uint16_t *token_out);
tavrn_maintenance_high_token_status_t tavrn_maintenance_high_token_release(
    tavrn_maintenance_t *maintenance, tavrn_link_v2_t *link,
    tavrn_maintenance_high_token_owner_t owner,
    tavrn_maintenance_high_token_purpose_t purpose, uint16_t token);
/* This is the sole composed FULL scheduler boundary for high-domain work.  An
 * exact external terminal reaches only that copied callback; ordinary high
 * tokens retain the targeted and verification handlers. */
tavrn_maintenance_high_token_status_t
tavrn_maintenance_high_token_scheduler_event(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, const ble_mesh_sched_event_t *event,
    uint32_t now_ms, tavrn_maintenance_high_token_dispatch_result_t *result_out);
/* Synchronous scheduler eviction uses the same ownership dispatch as terminal
 * events.  The external callback receives a zero-channel TX_FAILED fact. */
tavrn_maintenance_high_token_status_t tavrn_maintenance_high_token_evicted(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, uint16_t token, uint32_t now_ms,
    tavrn_maintenance_high_token_dispatch_result_t *result_out);
tavrn_rreq_verification_status_t tavrn_maintenance_verification_snapshot(
    const tavrn_maintenance_t *maintenance, const tavrn_router_t *router,
    const tavrn_link_v2_t *link, const tavrn_gtt_t *gtt,
    tavrn_rreq_verification_snapshot_t *snapshot_out);

/* DEV-027 public policy surface.  `state` is either the one embedded FULL
 * maintenance owner or a deliberately isolated host fixture. */
tavrn_tc_metadata_status_t tavrn_maintenance_tc_metadata_init(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_metadata_config_t *config);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_metadata_bind_gtt(
    tavrn_tc_metadata_state_t *state, tavrn_gtt_t *gtt);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_sequence_prepare(
    tavrn_tc_metadata_state_t *state, tavrn_tc_event_t event,
    tavrn_tc_sequence_ticket_t *ticket_out);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_sequence_commit(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_sequence_ticket_t *ticket,
    tavrn_tc_admission_t admission, uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_prepare_origin(
    tavrn_tc_metadata_state_t *state, tavrn_tc_origin_cause_t cause,
    const tavrn_adva_t *subject, const tavrn_adva_t *failed_next_hop_or_null,
    const tavrn_adva_t *final_destination_or_null, uint32_t now_ms,
    tavrn_tc_metadata_action_t *action_out);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_commit_origin(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_metadata_action_t *action,
    tavrn_tc_admission_t admission, uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_receive(
    tavrn_tc_metadata_state_t *state, const uint8_t pdu[TAVRN_LINK_CONTROL_PDU_MAX],
    uint8_t pdu_len, const tavrn_adva_t *outer_transmitter, uint32_t now_ms,
    tavrn_tc_metadata_action_t *relay_out);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_commit_relay(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_metadata_action_t *action,
    tavrn_tc_admission_t admission, uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_metadata_create(
    tavrn_tc_metadata_state_t *state, const tavrn_metadata_candidate_t *candidate,
    uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_metadata_select(
    const tavrn_tc_metadata_state_t *state, tavrn_metadata_frame_kind_t frame_kind,
    uint8_t unreachable_count, uint32_t now_ms,
    tavrn_metadata_selection_t *selection_out);
tavrn_tc_metadata_status_t tavrn_maintenance_metadata_commit(
    tavrn_tc_metadata_state_t *state, const tavrn_metadata_selection_t *selection,
    tavrn_tc_admission_t admission, uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_metadata_attach_control(
    const tavrn_validated_control_t *base, const tavrn_metadata_selection_t *selection,
    tavrn_metadata_attached_control_t *attached_out);
tavrn_tc_metadata_status_t tavrn_maintenance_metadata_receive(
    tavrn_tc_metadata_state_t *state, const tavrn_metadata_rx_t *rx,
    uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_metadata_receive_control(
    tavrn_tc_metadata_state_t *state, const tavrn_rx_control_event_t *event,
    uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_metadata_snapshot(
    const tavrn_tc_metadata_state_t *state,
    tavrn_tc_metadata_snapshot_t *snapshot_out);

/* Runtime-only glue used by the FULL binding after its explicit router/link
 * admission callbacks. */
tavrn_tc_metadata_status_t tavrn_maintenance_tc_on_verified_departure(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *subject, uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_on_direct_timeout_departure(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *subject, uint32_t now_ms);
/* Compatibility shim for older FULL callers.  It schedules H verification and
 * never prepares a TC/LEAVE.  New code calls the failed-hop API directly. */
tavrn_tc_metadata_status_t tavrn_maintenance_tc_on_retry_exhausted(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *failed_next_hop,
    const tavrn_adva_t *final_destination_or_null, uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_owner_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_metadata_owner_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_metadata_telemetry(
    const tavrn_maintenance_t *maintenance,
    tavrn_tc_metadata_telemetry_t *telemetry_out);
tavrn_metadata_reservation_handle_t tavrn_maintenance_metadata_reserve_delayed(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *subject,
    uint8_t context_index, uint32_t now_ms);
void tavrn_maintenance_metadata_release_delayed(
    tavrn_maintenance_t *maintenance,
    tavrn_metadata_reservation_handle_t handle);

#endif /* TAVRN_MAINTENANCE_H */
