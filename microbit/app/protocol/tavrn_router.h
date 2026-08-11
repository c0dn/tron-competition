#ifndef TAVRN_ROUTER_H
#define TAVRN_ROUTER_H

#include <stdint.h>

#include "aodv_core.h"
#include "tavrn_link_v2.h"

#define TAVRN_ROUTER_INCARNATION_API 1
#define TAVRN_ROUTER_EVIDENCE_CAPACITY 2u
#define TAVRN_ROUTER_FAILURE_CAPACITY TAVRN_AODV_ROUTE_CAPACITY
#define TAVRN_ROUTER_FAILURE_OVERFLOW_CAPACITY 1u
#define TAVRN_ROUTER_INCARNATION_PEER_CAPACITY TAVRN_AODV_ROUTE_CAPACITY
#define TAVRN_ROUTER_INCARNATION_PENDING_RESET_CAPACITY 1u

typedef uint16_t tavrn_router_delivery_token_t;
typedef uint16_t tavrn_router_candidate_reservation_token_t;

#define TAVRN_ROUTER_DELIVERY_TOKEN_NONE 0u
#define TAVRN_ROUTER_CANDIDATE_RESERVATION_TOKEN_NONE 0u

typedef char tavrn_router_evidence_capacity_guard[
    (TAVRN_ROUTER_EVIDENCE_CAPACITY == 2u) ? 1 : -1];
typedef char tavrn_router_failure_capacity_guard[
    (TAVRN_ROUTER_FAILURE_CAPACITY == TAVRN_AODV_ROUTE_CAPACITY) ? 1 : -1];
typedef char tavrn_router_failure_overflow_capacity_guard[
    (TAVRN_ROUTER_FAILURE_OVERFLOW_CAPACITY == 1u) ? 1 : -1];

/* The immediate transmitter is carried separately as a full direct peer.
 * These are the only additional Phase 3 semantic identity roles. */
typedef enum tavrn_router_evidence_role {
    TAVRN_ROUTER_EVIDENCE_ORIGIN = 0,
    TAVRN_ROUTER_EVIDENCE_ROUTE_DESTINATION,
    TAVRN_ROUTER_EVIDENCE_REPORTER,
} tavrn_router_evidence_role_t;

typedef enum tavrn_router_evidence_serial_kind {
    TAVRN_ROUTER_EVIDENCE_SERIAL_NONE = 0,
    TAVRN_ROUTER_EVIDENCE_SERIAL_NODE_SEQUENCE,
    TAVRN_ROUTER_EVIDENCE_SERIAL_DESTINATION_SEQUENCE,
} tavrn_router_evidence_serial_kind_t;

typedef enum tavrn_router_frame_admission {
    TAVRN_ROUTER_FRAME_COMMITTED = 0,
    TAVRN_ROUTER_FRAME_UNRESOLVED_DATA_CANDIDATE,
    TAVRN_ROUTER_FRAME_OVERHEARD_ADDRESSED_DATA,
    TAVRN_ROUTER_FRAME_MALFORMED,
    TAVRN_ROUTER_FRAME_AMBIGUOUS,
    TAVRN_ROUTER_FRAME_REJECTED,
} tavrn_router_frame_admission_t;

typedef struct tavrn_router_evidence_subject {
    tavrn_logical_id_t logical_id;
    /* Canonical identity is optional.  An unresolved SID16 remains a logical
     * fact and never authorizes synthesis of a full AdvA. */
    tavrn_adva_t canonical_identity;
    tavrn_router_evidence_role_t role;
    tavrn_router_evidence_serial_kind_t serial_kind;
    uint16_t serial;
    uint8_t hop_count;
    uint8_t hop_present;
    uint8_t canonical_present;
} tavrn_router_evidence_subject_t;

/* The callback receives only committed frame facts and must copy any retained
 * data before it returns.  No FULL_TAVRN type participates in this seam. */
typedef struct tavrn_router_observation {
    tavrn_wire_type_t frame_type;
    tavrn_direct_peer_t transmitter;
    uint8_t subject_count;
    tavrn_router_evidence_subject_t subjects[TAVRN_ROUTER_EVIDENCE_CAPACITY];
} tavrn_router_observation_t;

typedef struct tavrn_router_frame_observation {
    tavrn_router_frame_admission_t admission;
    tavrn_wire_type_t frame_type;
    tavrn_direct_peer_t transmitter;
    tavrn_hack_status_t hack_status;
    uint8_t subject_count;
    tavrn_router_evidence_subject_t subjects[TAVRN_ROUTER_EVIDENCE_CAPACITY];
} tavrn_router_frame_observation_t;

typedef struct tavrn_router_scope_hint {
    uint8_t has_hint;
    uint8_t initial_scope;
} tavrn_router_scope_hint_t;

typedef void (*tavrn_router_observation_fn)(
    void *context, const tavrn_router_observation_t *observation,
    uint32_t now_ms);
typedef tavrn_router_scope_hint_t (*tavrn_router_initial_scope_fn)(
    void *context, const tavrn_logical_id_t *destination,
    uint8_t full_scope, uint32_t now_ms);

typedef struct tavrn_router_augmentation_hooks {
    void *context;
    tavrn_router_observation_fn observe;
    tavrn_router_initial_scope_fn initial_scope;
} tavrn_router_augmentation_hooks_t;

/* Application delivery callbacks are synchronous mesh-task callbacks.  They
 * MUST NOT call tavrn_router, tavrn_link_v2, or aodv_core APIs (including
 * dispatch, tick, submit, or any callback that can re-enter this router).
 * Each callback receives the exact direct transmitter and DATA copy for its
 * reservation; the nonzero token is opaque to router-common. */
typedef enum tavrn_router_delivery_status {
    TAVRN_ROUTER_DELIVERY_OK = 0,
    TAVRN_ROUTER_DELIVERY_BUSY,
    TAVRN_ROUTER_DELIVERY_INVALID,
} tavrn_router_delivery_status_t;

typedef tavrn_router_delivery_status_t (*tavrn_router_delivery_reserve_fn)(
    void *context, const tavrn_direct_peer_t *transmitter,
    const tavrn_link_data_t *data, tavrn_router_delivery_token_t *token_out,
    uint32_t now_ms);
typedef tavrn_router_delivery_status_t (*tavrn_router_delivery_commit_fn)(
    void *context, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t now_ms);
typedef tavrn_router_delivery_status_t (*tavrn_router_delivery_cancel_fn)(
    void *context, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t now_ms);

typedef struct tavrn_router_application_hooks {
    void *context;
    tavrn_router_delivery_reserve_fn reserve;
    tavrn_router_delivery_commit_fn commit;
    tavrn_router_delivery_cancel_fn cancel;
} tavrn_router_application_hooks_t;

/* Candidate reservation is an optional, synchronous policy port.  It is
 * invoked after ordinary router admission gates but before the read-only AODV
 * probe.  Callbacks must not re-enter router, link, or AODV APIs. */
typedef enum tavrn_router_candidate_reservation_status {
    TAVRN_ROUTER_CANDIDATE_NOT_APPLICABLE = 0,
    TAVRN_ROUTER_CANDIDATE_RESERVED,
    TAVRN_ROUTER_CANDIDATE_BUSY,
    TAVRN_ROUTER_CANDIDATE_INVALID,
} tavrn_router_candidate_reservation_status_t;

typedef enum tavrn_router_candidate_completion_status {
    TAVRN_ROUTER_CANDIDATE_COMPLETION_OK = 0,
    TAVRN_ROUTER_CANDIDATE_COMPLETION_INVALID,
} tavrn_router_candidate_completion_status_t;

typedef tavrn_router_candidate_reservation_status_t
    (*tavrn_router_candidate_reserve_fn)(
        void *context, const tavrn_rx_data_candidate_t *candidate,
        tavrn_router_candidate_reservation_token_t *token_out,
        uint32_t now_ms);
typedef tavrn_router_candidate_completion_status_t
    (*tavrn_router_candidate_commit_fn)(
        void *context, tavrn_router_candidate_reservation_token_t token,
        const tavrn_rx_data_candidate_t *candidate, uint32_t now_ms);
typedef tavrn_router_candidate_completion_status_t
    (*tavrn_router_candidate_rollback_fn)(
        void *context, tavrn_router_candidate_reservation_token_t token,
        const tavrn_rx_data_candidate_t *candidate, uint32_t now_ms);

typedef struct tavrn_router_candidate_reservation_port {
    void *context;
    tavrn_router_candidate_reserve_fn reserve;
    tavrn_router_candidate_commit_fn commit;
    tavrn_router_candidate_rollback_fn rollback;
} tavrn_router_candidate_reservation_port_t;

typedef enum tavrn_router_delivery_state {
    TAVRN_ROUTER_DELIVERY_NONE = 0,
    TAVRN_ROUTER_DELIVERY_RESERVED_PRE_ACK,
    TAVRN_ROUTER_DELIVERY_CANCEL_PENDING,
    TAVRN_ROUTER_DELIVERY_POST_ACK_COMMIT,
    TAVRN_ROUTER_DELIVERY_FAULTED_POST_ACK,
    TAVRN_ROUTER_DELIVERY_FAULTED_PRE_ACK,
} tavrn_router_delivery_state_t;

typedef enum tavrn_router_fault_reason {
    TAVRN_ROUTER_FAULT_NONE = 0,
    TAVRN_ROUTER_FAULT_DELIVERY_CANCEL_INVALID,
    TAVRN_ROUTER_FAULT_DELIVERY_COMMIT_INVALID,
    TAVRN_ROUTER_FAULT_CONTROL_CANCEL_INVALID,
    TAVRN_ROUTER_FAULT_APPLICATION_RESERVE_TOKEN_ZERO,
    TAVRN_ROUTER_FAULT_DELIVERY_ACTION_MISMATCH,
    TAVRN_ROUTER_FAULT_POST_ACK_INGEST_INVALID,
    TAVRN_ROUTER_FAULT_TRANSIT_CUSTODY_RELEASE_INVALID,
    TAVRN_ROUTER_FAULT_TRANSIT_CUSTODY_DISCARD_INVALID,
    TAVRN_ROUTER_FAULT_FAILURE_OBLIGATION_OVERFLOW,
    TAVRN_ROUTER_FAULT_FAILURE_REPORT_INVALID,
    TAVRN_ROUTER_FAULT_DATA_TERMINAL_HOOK_INVALID,
    TAVRN_ROUTER_FAULT_CANDIDATE_RESERVATION_INVALID,
    TAVRN_ROUTER_FAULT_CANDIDATE_RESERVATION_TOKEN_ZERO,
    TAVRN_ROUTER_FAULT_CANDIDATE_COMMIT_INVALID,
    TAVRN_ROUTER_FAULT_CANDIDATE_ROLLBACK_INVALID,
} tavrn_router_fault_reason_t;

typedef struct tavrn_router_delivery_reservation {
    tavrn_direct_peer_t transmitter;
    tavrn_link_data_t data;
    tavrn_router_delivery_token_t token;
    tavrn_router_delivery_state_t state;
} tavrn_router_delivery_reservation_t;

typedef struct tavrn_router_pending_ingest {
    aodv_data_input_t input;
    uint8_t valid;
} tavrn_router_pending_ingest_t;

typedef struct tavrn_router_failure_obligation {
    tavrn_direct_peer_t peer;
    uint32_t admission_order;
    uint8_t valid;
} tavrn_router_failure_obligation_t;

/* The seventeenth failed-link event cannot enter the 16-obligation AODV
 * retry set.  Retain its exact transferred event and fail stop instead of
 * releasing it without a report path. */
typedef struct tavrn_router_failure_overflow {
    tavrn_owned_data_event_t owned;
    uint32_t admission_order;
    uint8_t valid;
} tavrn_router_failure_overflow_t;

typedef struct tavrn_router_counters {
    uint32_t stale_candidate;
    uint32_t application_reserve_busy;
    uint32_t application_reserve_invalid;
    uint32_t application_commit_busy;
    uint32_t application_commit_invalid;
    uint32_t application_cancel_invalid;
    uint32_t application_cancel_busy;
    uint32_t delivery_mismatch;
    uint32_t post_ack_commit_busy;
    uint32_t post_ack_commit_invalid;
    uint32_t release_invariant;
    uint32_t failure_set_overflow;
    uint32_t failure_invariant;
    uint32_t permanent_action_disposed;
} tavrn_router_counters_t;

typedef enum tavrn_router_feature_level {
    TAVRN_ROUTER_FEATURE_AODV_ONLY = 0,
    TAVRN_ROUTER_FEATURE_FULL_TAVRN,
} tavrn_router_feature_level_t;

typedef enum tavrn_router_incarnation_state {
    TAVRN_ROUTER_INCARNATION_REJOINING = 0,
    TAVRN_ROUTER_INCARNATION_ESTABLISHED,
} tavrn_router_incarnation_state_t;

typedef enum tavrn_router_incarnation_status {
    TAVRN_ROUTER_INCARNATION_OK = 0,
    TAVRN_ROUTER_INCARNATION_BUSY,
    TAVRN_ROUTER_INCARNATION_INVALID,
    TAVRN_ROUTER_INCARNATION_INVALID_CONFIG,
    TAVRN_ROUTER_INCARNATION_NOT_FOUND,
} tavrn_router_incarnation_status_t;

typedef struct tavrn_router_incarnation_config {
    tavrn_router_feature_level_t feature_level;
    uint16_t boot_nonce;
    uint32_t reboot_announce_ms;
} tavrn_router_incarnation_config_t;

typedef struct tavrn_router_incarnation_snapshot {
    tavrn_router_incarnation_state_t state;
    tavrn_validated_control_t latest_hello;
    ble_mesh_tx_token_t latest_hello_tx_token;
    uint16_t boot_nonce;
    uint32_t last_bootstrap_tx_done_ms;
    uint8_t bootstrap_tx_done_seen;
    uint8_t normal_hello_emitted;
} tavrn_router_incarnation_snapshot_t;

typedef struct tavrn_router_incarnation_peer_snapshot {
    tavrn_direct_peer_t direct_peer;
    uint16_t boot_nonce;
    uint8_t direct_binding_valid;
    uint8_t route_barred;
    uint8_t reset_pending;
    uint8_t pending_custody_count;
    uint8_t data_dedupe_count;
    uint8_t control_dedupe_count;
    uint8_t freshness_count;
    uint8_t route_count;
} tavrn_router_incarnation_peer_snapshot_t;

typedef struct tavrn_router_incarnation_counters {
    uint32_t direct_bootstrap_admitted;
    uint32_t same_tuple_idempotent;
    uint32_t reset_committed;
    uint32_t reset_busy;
    uint32_t reset_overflow;
    uint32_t invalid_bootstrap_rejected;
} tavrn_router_incarnation_counters_t;

typedef struct tavrn_router_incarnation_peer {
    tavrn_direct_peer_t direct_peer;
    uint16_t boot_nonce;
    uint8_t valid;
    uint8_t route_barred;
    uint8_t identity_conflict;
    uint8_t control_dedupe_count;
    uint8_t freshness_count;
} tavrn_router_incarnation_peer_t;

typedef struct tavrn_router_pending_incarnation_reset {
    tavrn_direct_peer_t direct_peer;
    uint16_t boot_nonce;
    uint8_t valid;
    uint8_t preserve_bar;
    /* Set only after AODV has reserved its reset RERR work.  A later BUSY
     * retry may clear link/router/application state, but must not reset AODV
     * again or replace that required notification. */
    uint8_t aodv_reset_committed;
} tavrn_router_pending_incarnation_reset_t;

typedef struct tavrn_router_incarnation {
    tavrn_router_incarnation_config_t config;
    tavrn_router_incarnation_snapshot_t snapshot;
    tavrn_router_incarnation_peer_t peers[TAVRN_ROUTER_INCARNATION_PEER_CAPACITY];
    tavrn_router_pending_incarnation_reset_t pending_reset;
    tavrn_router_incarnation_counters_t counters;
    uint32_t next_bootstrap_announce_ms;
    uint8_t enabled;
} tavrn_router_incarnation_t;

/* All non-maintenance local broadcast admissions advance one copied generation.
 * The routed binding consumes it once; this has no scheduler or radio callback
 * ownership. */
typedef struct tavrn_router_local_broadcast_snapshot {
    uint32_t generation;
    uint32_t accepted_at_ms;
} tavrn_router_local_broadcast_snapshot_t;

typedef enum tavrn_router_local_broadcast_status {
    TAVRN_ROUTER_LOCAL_BROADCAST_OK = 0,
    TAVRN_ROUTER_LOCAL_BROADCAST_INVALID,
} tavrn_router_local_broadcast_status_t;

typedef enum tavrn_router_dispatch_event_type {
    TAVRN_ROUTER_DISPATCH_EVENT_NONE = 0,
    TAVRN_ROUTER_DISPATCH_EVENT_PENDING_DATA_FAILED,
    TAVRN_ROUTER_DISPATCH_EVENT_BLACKLIST_NEIGHBOR,
} tavrn_router_dispatch_event_type_t;

typedef struct tavrn_router_dispatch_event {
    tavrn_router_dispatch_event_type_t type;
    tavrn_logical_id_t destination;
    tavrn_direct_peer_t peer;
} tavrn_router_dispatch_event_t;

/* A caller may consume an already link-admitted control before ordinary AODV
 * dispatch. It is a single optional port, not an alternate wire/link path. */
typedef enum tavrn_router_control_intercept_status {
    TAVRN_ROUTER_CONTROL_INTERCEPT_IGNORED = 0,
    TAVRN_ROUTER_CONTROL_INTERCEPT_CONSUMED,
    TAVRN_ROUTER_CONTROL_INTERCEPT_BUSY,
    TAVRN_ROUTER_CONTROL_INTERCEPT_INVALID,
} tavrn_router_control_intercept_status_t;

typedef tavrn_router_control_intercept_status_t (*tavrn_router_control_intercept_fn)(
    void *context, const tavrn_rx_control_event_t *control_event, uint32_t now_ms);

typedef struct tavrn_router_control_interceptor {
    void *context;
    tavrn_router_control_intercept_fn receive;
} tavrn_router_control_interceptor_t;

/* These optional ports carry only copied common-router facts.  FULL policy may
 * decorate an otherwise admitted routed control or observe one after link
 * admission, but AODV never depends on either callback. */
typedef enum tavrn_router_control_augmentation_status {
    TAVRN_ROUTER_CONTROL_AUGMENTATION_OK = 0,
    TAVRN_ROUTER_CONTROL_AUGMENTATION_BUSY,
    TAVRN_ROUTER_CONTROL_AUGMENTATION_INVALID,
} tavrn_router_control_augmentation_status_t;

typedef tavrn_router_control_augmentation_status_t
    (*tavrn_router_control_prepare_fn)(
        void *context, const tavrn_validated_control_t *base,
        tavrn_validated_control_t *augmented_out, uint32_t now_ms);
/* Enqueue is not physical metadata completion.  A decorated control owner
 * receives its scheduler token here and consumes a matching completion below. */
typedef tavrn_router_control_augmentation_status_t
    (*tavrn_router_control_admission_fn)(
    void *context, const tavrn_validated_control_t *base,
    const tavrn_validated_control_t *sent,
    const tavrn_direct_peer_t *next_hop_or_null, uint8_t controlled_flood,
    tavrn_link_send_status_t status, ble_mesh_tx_token_t scheduler_token,
    uint32_t now_ms);
typedef tavrn_router_control_augmentation_status_t
    (*tavrn_router_control_completion_fn)(
        void *context, const ble_mesh_sched_event_t *event, uint32_t now_ms);
typedef void (*tavrn_router_control_rx_fn)(
    void *context, const tavrn_rx_control_event_t *control_event, uint32_t now_ms);

/* The router invokes this for every DATA custody terminal, including a
 * successful transfer.  OWNED is deliberately narrow: it is valid only for a
 * transit RETRY_EXHAUSTED event. */
typedef enum tavrn_router_data_terminal_disposition {
    TAVRN_ROUTER_DATA_TERMINAL_DECLINED = 0,
    TAVRN_ROUTER_DATA_TERMINAL_OBSERVED,
    TAVRN_ROUTER_DATA_TERMINAL_OWNED,
    TAVRN_ROUTER_DATA_TERMINAL_INVALID,
} tavrn_router_data_terminal_disposition_t;

typedef tavrn_router_data_terminal_disposition_t
    (*tavrn_router_data_terminal_hook_fn)(
        void *context, const tavrn_link_event_t *event, uint32_t now_ms);

typedef struct tavrn_router_control_augmentation {
    void *context;
    tavrn_router_control_prepare_fn prepare;
    tavrn_router_control_admission_fn admitted;
    tavrn_router_control_completion_fn completed;
    tavrn_router_control_rx_fn received;
} tavrn_router_control_augmentation_t;

typedef struct tavrn_router_data_terminal_hook {
    void *context;
    tavrn_router_data_terminal_hook_fn handle;
} tavrn_router_data_terminal_hook_t;

typedef struct tavrn_router {
    tavrn_link_v2_t *link;
    aodv_core_t *aodv;
    tavrn_router_augmentation_hooks_t augmentation;
    tavrn_router_application_hooks_t application;
    tavrn_router_delivery_reservation_t delivery;
    tavrn_router_pending_ingest_t pending_ingest;
    tavrn_router_failure_obligation_t
        failures[TAVRN_ROUTER_FAILURE_CAPACITY];
    tavrn_router_failure_overflow_t failure_overflow;
    aodv_action_t retained_action;
    tavrn_router_counters_t counters;
    uint32_t next_failure_order;
    tavrn_router_fault_reason_t fault_reason;
    uint8_t retained_action_valid;
    tavrn_router_incarnation_t incarnation;
    tavrn_router_local_broadcast_snapshot_t local_broadcast;
    tavrn_router_control_interceptor_t control_interceptor;
    tavrn_router_control_augmentation_t control_augmentation;
    tavrn_router_data_terminal_hook_t data_terminal_hook;
    tavrn_router_candidate_reservation_port_t candidate_reservation;
} tavrn_router_t;

typedef enum tavrn_router_init_status {
    TAVRN_ROUTER_INIT_OK = 0,
    TAVRN_ROUTER_INIT_INVALID_ARGUMENT,
} tavrn_router_init_status_t;

typedef enum tavrn_router_application_hook_status {
    TAVRN_ROUTER_APPLICATION_HOOK_OK = 0,
    TAVRN_ROUTER_APPLICATION_HOOK_INVALID,
    TAVRN_ROUTER_APPLICATION_HOOK_BUSY,
} tavrn_router_application_hook_status_t;

typedef enum tavrn_router_observe_status {
    TAVRN_ROUTER_OBSERVE_REPORTED = 0,
    TAVRN_ROUTER_OBSERVE_IGNORED,
    TAVRN_ROUTER_OBSERVE_INVALID,
} tavrn_router_observe_status_t;

typedef enum tavrn_router_scope_status {
    TAVRN_ROUTER_SCOPE_OK = 0,
    TAVRN_ROUTER_SCOPE_INVALID,
} tavrn_router_scope_status_t;

typedef enum tavrn_router_event_status {
    TAVRN_ROUTER_EVENT_OK = 0,
    TAVRN_ROUTER_EVENT_IGNORED,
    TAVRN_ROUTER_EVENT_BUSY,
    TAVRN_ROUTER_EVENT_INVALID,
    TAVRN_ROUTER_EVENT_REJOINING,
} tavrn_router_event_status_t;

/* FULL maintenance owns cadence and retention.  Router-common owns the single
 * link-v2 admission path for its exact ordinary SID8 HELLO controls. */
typedef enum tavrn_router_hello_status {
    TAVRN_ROUTER_HELLO_OK = 0,
    TAVRN_ROUTER_HELLO_BUSY,
    TAVRN_ROUTER_HELLO_GATED,
    TAVRN_ROUTER_HELLO_INVALID,
} tavrn_router_hello_status_t;

/* FULL maintenance supplies a generic AODV action plus its caller-owned high
 * token.  The router validates and admits it through the same link-v2 control
 * path; purpose remains solely in the maintenance owner. */
typedef enum tavrn_router_tracked_rreq_status {
    TAVRN_ROUTER_TRACKED_RREQ_OK = 0,
    TAVRN_ROUTER_TRACKED_RREQ_BUSY,
    TAVRN_ROUTER_TRACKED_RREQ_LOCAL_NOT_ATTEMPTED,
    TAVRN_ROUTER_TRACKED_RREQ_INVALID,
} tavrn_router_tracked_rreq_status_t;

/* A small read-only route truth seam for FULL maintenance.  It intentionally
 * exposes an AODV copy rather than a second table or a maintenance-owned next
 * hop. */
typedef enum tavrn_router_route_status {
    TAVRN_ROUTER_ROUTE_OK = 0,
    TAVRN_ROUTER_ROUTE_NOT_FOUND,
    TAVRN_ROUTER_ROUTE_INVALID,
} tavrn_router_route_status_t;

typedef enum tavrn_router_reforward_status {
    TAVRN_ROUTER_REFORWARD_OK = 0,
    TAVRN_ROUTER_REFORWARD_BUSY,
    TAVRN_ROUTER_REFORWARD_NOT_FOUND,
    TAVRN_ROUTER_REFORWARD_INVALID,
} tavrn_router_reforward_status_t;

/* By-value phase traces are the only observability output from the router's
 * production-facing _ex calls.  The discriminant prevents a scheduler/tick
 * operation from fabricating dispatch or link-service details. */
typedef enum tavrn_router_trace_phase {
    TAVRN_ROUTER_TRACE_NOT_APPLICABLE = 0,
    TAVRN_ROUTER_TRACE_SCHEDULER_EVENT,
    TAVRN_ROUTER_TRACE_TICK,
    TAVRN_ROUTER_TRACE_APPLICATION_SUBMIT,
    TAVRN_ROUTER_TRACE_DISPATCH,
    TAVRN_ROUTER_TRACE_LINK_SERVICE,
} tavrn_router_trace_phase_t;

typedef enum tavrn_router_trace_presence {
    TAVRN_ROUTER_TRACE_NOT_PRESENT = 0,
    TAVRN_ROUTER_TRACE_PRESENT,
} tavrn_router_trace_presence_t;

typedef struct tavrn_router_scheduler_trace {
    tavrn_router_event_status_t status;
    ble_mesh_sched_event_type_t input_event_type;
    ble_mesh_sched_fault_t input_fault;
    tavrn_codec_result_t wire_decode_result;
    tavrn_wire_type_t decoded_frame_type;
    tavrn_link_step_status_t link_step_status;
    tavrn_link_event_type_t link_event_type;
    tavrn_adva_t input_advertiser;
    tavrn_rx_control_event_t rx_control;
    uint8_t input_channel;
    uint8_t input_rssi_magnitude_db;
    uint8_t input_adv_len;
    tavrn_router_trace_presence_t input_present;
    tavrn_router_trace_presence_t wire_decode_present;
    tavrn_router_trace_presence_t decoded_frame_present;
    tavrn_router_trace_presence_t link_step_present;
    tavrn_router_trace_presence_t link_event_present;
    tavrn_router_trace_presence_t rx_control_present;
} tavrn_router_scheduler_trace_t;

typedef struct tavrn_router_tick_trace {
    aodv_status_t status;
    tavrn_link_step_status_t link_step_status;
    tavrn_link_event_t link_event;
    tavrn_router_trace_presence_t link_step_present;
    tavrn_router_trace_presence_t link_event_present;
} tavrn_router_tick_trace_t;

typedef struct tavrn_router_submit_trace {
    aodv_status_t status;
} tavrn_router_submit_trace_t;

typedef struct tavrn_router_dispatch_trace {
    tavrn_router_event_status_t status;
    tavrn_router_dispatch_event_t dispatch_event;
    aodv_action_t action;
    aodv_rreq_link_enqueue_t rreq_enqueue;
    tavrn_link_send_status_t link_send_status;
    tavrn_link_event_t link_event;
    tavrn_router_trace_presence_t action_present;
    tavrn_router_trace_presence_t rreq_enqueue_present;
    tavrn_router_trace_presence_t link_send_present;
    tavrn_router_trace_presence_t link_event_present;
} tavrn_router_dispatch_trace_t;

typedef char tavrn_router_scheduler_trace_fits_dispatch_guard[
    (sizeof(tavrn_router_scheduler_trace_t) <=
     sizeof(tavrn_router_dispatch_trace_t)) ? 1 : -1];

typedef struct tavrn_router_link_service_trace {
    tavrn_router_event_status_t status;
    tavrn_link_step_status_t link_step_status;
    tavrn_link_event_t link_event;
    tavrn_router_trace_presence_t link_event_present;
} tavrn_router_link_service_trace_t;

typedef struct tavrn_router_phase_trace {
    uint32_t completed_at_ms;
    tavrn_router_trace_phase_t phase;
    tavrn_router_trace_presence_t terminal_fault_present;
    tavrn_router_fault_reason_t terminal_fault;
    union {
        tavrn_router_scheduler_trace_t scheduler_event;
        tavrn_router_tick_trace_t tick;
        tavrn_router_submit_trace_t submit;
        tavrn_router_dispatch_trace_t dispatch;
        tavrn_router_link_service_trace_t link_service;
    } detail;
} tavrn_router_phase_trace_t;

tavrn_router_init_status_t tavrn_router_init(
    tavrn_router_t *router, tavrn_link_v2_t *link, aodv_core_t *aodv,
    const tavrn_router_augmentation_hooks_t *augmentation_or_null);
/* Platform startup supplies the bounded nonzero nonce.  Legacy callers retain
 * tavrn_router_init() behavior; routed firmware uses this incarnation owner. */
tavrn_router_incarnation_status_t tavrn_router_init_with_incarnation(
    tavrn_router_t *router, tavrn_link_v2_t *link, aodv_core_t *aodv,
    const tavrn_router_augmentation_hooks_t *augmentation_or_null,
    const tavrn_router_incarnation_config_t *config, uint32_t now_ms);
/* Installs an all-or-nothing generic application delivery port.  Passing NULL
 * removes the port; local final DATA then resolves BUSY, never ACCEPTED. */
tavrn_router_application_hook_status_t tavrn_router_set_application_hooks(
    tavrn_router_t *router,
    const tavrn_router_application_hooks_t *application_or_null);
/* Passing NULL removes the port.  A non-NULL port must provide all three
 * callbacks so that every successful reservation has a terminal owner. */
tavrn_router_event_status_t tavrn_router_set_candidate_reservation_port(
    tavrn_router_t *router,
    const tavrn_router_candidate_reservation_port_t *port_or_null);
/* The callback runs after link-v2 admits an RX control and before ordinary
 * router/AODV dispatch. Returning IGNORED preserves ordinary handling. */
tavrn_router_event_status_t tavrn_router_set_control_interceptor(
    tavrn_router_t *router,
    const tavrn_router_control_interceptor_t *interceptor_or_null);
/* Installs copied control hooks without introducing FULL types into router or
 * AODV. `prepare` is optional; `admitted` and `received` may be installed
 * independently. */
tavrn_router_event_status_t tavrn_router_set_control_augmentation(
    tavrn_router_t *router,
    const tavrn_router_control_augmentation_t *augmentation_or_null);
tavrn_router_event_status_t tavrn_router_set_data_terminal_hook(
    tavrn_router_t *router,
    const tavrn_router_data_terminal_hook_t *hook_or_null);
tavrn_router_event_status_t tavrn_router_handle_scheduler_event(
    tavrn_router_t *router, const ble_mesh_sched_event_t *event,
    uint32_t now_ms);
tavrn_router_event_status_t tavrn_router_handle_link_event(
    tavrn_router_t *router, const tavrn_link_event_t *event, uint32_t now_ms);
aodv_status_t tavrn_router_submit_application(
    tavrn_router_t *router, const tron_application_data_t *data,
    uint32_t now_ms);
aodv_status_t tavrn_router_tick(tavrn_router_t *router, uint32_t now_ms);
/* _ex forms preserve the lower-profile wrappers while returning one
 * phase-discriminated, by-value trace with no mutable-router inspection. */
tavrn_router_event_status_t tavrn_router_handle_scheduler_event_ex(
    tavrn_router_t *router, const ble_mesh_sched_event_t *event,
    uint32_t now_ms, tavrn_router_phase_trace_t *trace_out);
aodv_status_t tavrn_router_tick_ex(tavrn_router_t *router, uint32_t now_ms,
                                   tavrn_router_phase_trace_t *trace_out);
aodv_status_t tavrn_router_submit_application_ex(
    tavrn_router_t *router, const tron_application_data_t *data,
    uint32_t now_ms, tavrn_router_phase_trace_t *trace_out);
/* The typed form preserves one locally-disposed AODV action by value.  It
 * always zeroes event_out before work, and processes no more than one action. */
tavrn_router_event_status_t tavrn_router_dispatch_ex(
    tavrn_router_t *router, uint32_t now_ms,
    tavrn_router_dispatch_event_t *event_out);
tavrn_router_event_status_t tavrn_router_dispatch_trace_ex(
    tavrn_router_t *router, uint32_t now_ms,
    tavrn_router_phase_trace_t *trace_out);
/* Promotes at most one already-link-owned DATA custody item into the link
 * scheduler and consumes its one local ownership outcome, if any. */
tavrn_router_event_status_t tavrn_router_service_link(tavrn_router_t *router,
                                                        uint32_t now_ms);
tavrn_router_event_status_t tavrn_router_service_link_ex(
    tavrn_router_t *router, uint32_t now_ms,
    tavrn_router_phase_trace_t *trace_out);
const tavrn_router_counters_t *tavrn_router_counters(
    const tavrn_router_t *router);
/* Pure copied router-owned DATA facts; no GTT/FULL dependency. */
tavrn_router_subject_demand_snapshot_t tavrn_router_subject_demand_snapshot(
    const tavrn_router_t *router, const tavrn_logical_id_t *subject,
    const tavrn_adva_t *canonical_subject, uint32_t now_ms);
tavrn_router_delivery_state_t tavrn_router_delivery_state(
    const tavrn_router_t *router);
tavrn_router_fault_reason_t tavrn_router_fault_reason(
    const tavrn_router_t *router);
tavrn_router_observe_status_t tavrn_router_observe_frame(
    tavrn_router_t *router,
    const tavrn_router_frame_observation_t *frame_observation,
    uint32_t now_ms);
tavrn_router_scope_status_t tavrn_router_initial_scope(
    const tavrn_router_t *router, const tavrn_logical_id_t *destination,
    uint8_t full_scope, uint32_t now_ms,
    tavrn_router_scope_hint_t *hint_out);
tavrn_router_incarnation_status_t tavrn_router_incarnation_snapshot(
    const tavrn_router_t *router,
    tavrn_router_incarnation_snapshot_t *snapshot_out);
tavrn_router_incarnation_status_t tavrn_router_incarnation_peer_snapshot(
    const tavrn_router_t *router, const tavrn_adva_t *peer_adva,
    tavrn_router_incarnation_peer_snapshot_t *snapshot_out);
const tavrn_router_incarnation_counters_t *tavrn_router_incarnation_counters(
    const tavrn_router_t *router);
/* FULL_TAVRN-only policy calls this generic shared-router operation after its
 * full-identity context is validated.  It retains the same router/link/AODV
 * objects and augmentation/application ports, but clears all incompatible
 * width-scoped work before making the new width visible. */
tavrn_router_incarnation_status_t tavrn_router_reconfigure_identity(
    tavrn_router_t *router, const tavrn_direct_peer_t *local_peer,
    uint32_t now_ms);
/* Completing a validated FULL bootstrap is distinct from AODV_ONLY's timed
 * self-establishment. */
tavrn_router_incarnation_status_t tavrn_router_complete_full_bootstrap(
    tavrn_router_t *router, uint32_t now_ms);
/* These are intentionally ordinary SID8 N=0 HELLO-only.  They do not expose a
 * generic FULL control injection path to router-common callers. */
tavrn_router_hello_status_t tavrn_router_build_ordinary_hello(
    const tavrn_router_t *router, uint16_t node_sequence,
    tavrn_validated_control_t *control_out);
tavrn_router_hello_status_t tavrn_router_enqueue_ordinary_hello(
    tavrn_router_t *router, const tavrn_validated_control_t *control,
    uint32_t now_ms);
tavrn_router_hello_status_t tavrn_router_cancel_ordinary_hello(
    tavrn_router_t *router, const tavrn_validated_control_t *control);
tavrn_router_tracked_rreq_status_t tavrn_router_enqueue_tracked_rreq(
    tavrn_router_t *router, const aodv_action_t *action, uint16_t token,
    uint32_t now_ms, uint16_t *evicted_token_out);
/* Retry an already prepared exact decorated control.  This does not allocate a
 * route action or request ID; a retained existing action is marked sent only
 * after the retry is accepted into the scheduler. */
tavrn_router_event_status_t tavrn_router_retry_retained_control(
    tavrn_router_t *router, const tavrn_validated_control_t *base,
    const tavrn_validated_control_t *sent,
    const tavrn_direct_peer_t *next_hop_or_null, uint8_t controlled_flood,
    uint32_t now_ms);
/* Admit one locally addressed RREP for a caller-retained local RREQ attempt.
 * The normal router observation path runs only for core-accepted or duplicate
 * controls; this adds no router discovery or pending-DATA state. */
aodv_status_t tavrn_router_ingest_rrep_for_attempt(
    tavrn_router_t *router, const tavrn_rx_control_event_t *control_event,
    const aodv_rreq_attempt_t *attempt, uint32_t now_ms);
tavrn_router_route_status_t tavrn_router_route_to_subject(
    const tavrn_router_t *router, const tavrn_logical_id_t *subject,
    uint32_t now_ms, aodv_route_snapshot_t *route_out);
/* Re-enqueues an already-owned transit DATA copy through one currently valid
 * route.  It performs no AODV DATA ingress, discovery, pending-DATA, or TTL /
 * hop mutation. */
tavrn_router_reforward_status_t tavrn_router_reforward_transit_data(
    tavrn_router_t *router, const tavrn_link_data_t *data, uint32_t now_ms,
    tavrn_direct_peer_t *next_hop_out);
/* Releases the original inbound transit dedupe pin after a policy owner has
 * terminally dropped its copied DATA. */
tavrn_router_event_status_t tavrn_router_release_transit_pin(
    tavrn_router_t *router, const tavrn_link_data_t *data, uint32_t now_ms);
/* Mentorship and router-common call this only after a non-maintenance local
 * broadcast control has been admitted by link-v2. */
void tavrn_router_note_local_broadcast(tavrn_router_t *router,
                                       uint32_t accepted_at_ms);
tavrn_router_local_broadcast_status_t tavrn_router_local_broadcast_snapshot(
    const tavrn_router_t *router,
    tavrn_router_local_broadcast_snapshot_t *snapshot_out);

#endif /* TAVRN_ROUTER_H */
