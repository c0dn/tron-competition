#ifndef AODV_CORE_H
#define AODV_CORE_H

#include <stdint.h>

#include "tavrn_wire_v2.h"
#include "tavrn_subject_demand.h"

/* Fixed storage limits from BUILD-01.  The core owns the corresponding
 * storage; callers observe routes and work only through the APIs below. */
#define TAVRN_AODV_ROUTE_CAPACITY            16u
#define TAVRN_AODV_RREQ_SEEN_CAPACITY        32u
#define TAVRN_AODV_PENDING_DATA_CAPACITY      8u
#define TAVRN_AODV_PRECURSOR_CAPACITY         8u
#define TAVRN_AODV_RREP_ACK_WAIT_CAPACITY     4u
#define TAVRN_AODV_ACTION_CAPACITY            8u
#define TAVRN_AODV_RERR_BATCH_CAPACITY       16u
#define TAVRN_AODV_RERR_SID16_PER_ACTION      3u
#define TAVRN_AODV_RERR_SID8_PER_ACTION       4u

/* The opaque fixed buffer keeps forwarding-table mutation private to the
 * implementation while allowing caller-owned, heap-free core instances. */
#define TAVRN_AODV_CORE_PRIVATE_STORAGE_BYTES 4096u

typedef char tavrn_aodv_route_capacity_guard[
    (TAVRN_AODV_ROUTE_CAPACITY == 16u) ? 1 : -1];
typedef char tavrn_aodv_rreq_seen_capacity_guard[
    (TAVRN_AODV_RREQ_SEEN_CAPACITY == 32u) ? 1 : -1];
typedef char tavrn_aodv_pending_capacity_guard[
    (TAVRN_AODV_PENDING_DATA_CAPACITY == 8u) ? 1 : -1];
typedef char tavrn_aodv_precursor_capacity_guard[
    (TAVRN_AODV_PRECURSOR_CAPACITY == 8u) ? 1 : -1];
typedef char tavrn_aodv_ack_wait_capacity_guard[
    (TAVRN_AODV_RREP_ACK_WAIT_CAPACITY == 4u) ? 1 : -1];
typedef char tavrn_aodv_action_capacity_guard[
    (TAVRN_AODV_ACTION_CAPACITY == 8u) ? 1 : -1];
typedef char tavrn_aodv_private_storage_size_guard[
    (TAVRN_AODV_CORE_PRIVATE_STORAGE_BYTES % sizeof(uint32_t) == 0u) ? 1 : -1];

typedef enum aodv_route_state {
    AODV_ROUTE_VALID = 0,
    AODV_ROUTE_INVALID,
    AODV_ROUTE_IN_SEARCH,
} aodv_route_state_t;

/* This is an immutable copy returned by aodv_core_route_snapshot(). */
typedef struct aodv_route_snapshot {
    tavrn_logical_id_t destination;
    tavrn_direct_peer_t next_hop;
    uint16_t destination_sequence;
    uint32_t expires_at_ms;
    uint8_t hop_count;
    uint8_t sequence_valid;
    aodv_route_state_t state;
    uint8_t precursor_count;
    uint8_t precursor_overflow;
    tavrn_logical_id_t precursors[TAVRN_AODV_PRECURSOR_CAPACITY];
} aodv_route_snapshot_t;

typedef struct aodv_core_config {
    tavrn_direct_peer_t local_peer;
    uint8_t network_id;
    uint8_t net_diameter;
    uint8_t rreq_retries;
    uint8_t rreq_rate_per_second;
    uint8_t rerr_rate_per_second;
    uint8_t request_rrep_ack;
    uint16_t initial_origin_sequence;
    uint16_t initial_request_id;
    uint16_t initial_rerr_sequence;
    uint32_t node_traversal_ms;
    uint32_t path_discovery_ms;
    uint32_t rreq_seen_ms;
    uint32_t active_route_ms;
    uint32_t pending_data_ms;
    uint32_t blacklist_ms;
    uint32_t rrep_dedupe_ms;
    uint32_t rerr_dedupe_ms;
    uint32_t rrep_ack_wait_ms;
} aodv_core_config_t;

typedef enum aodv_init_status {
    AODV_INIT_OK = 0,
    AODV_INIT_INVALID_ARGUMENT,
    AODV_INIT_INVALID_CONFIG,
    AODV_INIT_INVALID_IDENTITY,
} aodv_init_status_t;

typedef enum aodv_status {
    AODV_STATUS_OK = 0,
    AODV_STATUS_QUEUED,
    AODV_STATUS_DUPLICATE,
    AODV_STATUS_NOT_FOUND,
    AODV_STATUS_BUSY,
    AODV_STATUS_INVALID,
    AODV_STATUS_LOOP_PREVENTED,
    AODV_STATUS_UNMATCHED,
    AODV_STATUS_REJOINING,
} aodv_status_t;

typedef enum aodv_route_query_status {
    AODV_ROUTE_QUERY_FOUND = 0,
    AODV_ROUTE_QUERY_NOT_FOUND,
    AODV_ROUTE_QUERY_INVALID,
} aodv_route_query_status_t;

typedef enum aodv_action_poll_status {
    AODV_ACTION_POLL_OK = 0,
    AODV_ACTION_POLL_EMPTY,
    AODV_ACTION_POLL_INVALID,
} aodv_action_poll_status_t;

/* Link-v2 is intentionally not included here.  tavrn_router translates its
 * copied control and DATA events into these by-value AODV inputs. */
typedef struct aodv_control_input {
    tavrn_direct_peer_t transmitter;
    tavrn_validated_control_t control;
} aodv_control_input_t;

typedef struct aodv_data_input {
    tavrn_direct_peer_t transmitter;
    tavrn_link_data_t data;
} aodv_data_input_t;

/* This copied identity travels with every locally-created RREQ action.  It is
 * route-core-only: a scheduler token is deliberately not part of it. */
typedef enum aodv_rreq_attempt_presence {
    AODV_RREQ_ATTEMPT_NOT_PRESENT = 0,
    AODV_RREQ_ATTEMPT_PRESENT,
} aodv_rreq_attempt_presence_t;

typedef struct aodv_rreq_attempt {
    uint32_t discovery_correlation;
    tavrn_logical_id_t origin;
    tavrn_logical_id_t destination;
    uint16_t request_id;
    uint8_t initial_scope;
    uint8_t current_scope;
    uint8_t ring_ordinal;
} aodv_rreq_attempt_t;

/* A single locally-owned RREQ with no DATA, discovery slot, or retry state.
 * The caller retains its off-wire purpose and completion lifecycle.  This
 * generic seam deliberately names no FULL/GTT type. */
typedef enum aodv_single_rreq_status {
    AODV_SINGLE_RREQ_OK = 0,
    AODV_SINGLE_RREQ_RATE_DEFERRED,
    AODV_SINGLE_RREQ_BUSY,
    AODV_SINGLE_RREQ_INVALID,
} aodv_single_rreq_status_t;

/* A caller may retain one locally created RREQ attempt without creating the
 * core's ordinary discovery/pending-DATA state.  This result is a pure
 * correlation check for an incoming RREP; it performs no route mutation. */
typedef enum aodv_rrep_attempt_match_status {
    AODV_RREP_ATTEMPT_MATCH = 0,
    AODV_RREP_ATTEMPT_NO_MATCH,
    AODV_RREP_ATTEMPT_INVALID,
} aodv_rrep_attempt_match_status_t;

typedef enum aodv_action_type {
    AODV_ACTION_NONE = 0,
    AODV_ACTION_SEND_RREQ,
    AODV_ACTION_SEND_RREP,
    AODV_ACTION_FORWARD_RREP,
    AODV_ACTION_SEND_RERR,
    AODV_ACTION_SEND_RREP_ACK,
    AODV_ACTION_FORWARD_DATA,
    AODV_ACTION_DELIVER_DATA,
    AODV_ACTION_PENDING_DATA_FAILED,
    AODV_ACTION_BLACKLIST_NEIGHBOR,
} aodv_action_type_t;

typedef struct aodv_control_action {
    /* Nonzero only for a tracked action.  RREP actions that request
     * E_RREP_ACK are reserved before emission and become timed waits after
     * aodv_core_mark_action_sent(). */
    uint16_t token;
    uint8_t controlled_flood;
    tavrn_direct_peer_t next_hop;
    tavrn_validated_control_t control;
    aodv_rreq_attempt_t rreq_attempt;
    aodv_rreq_attempt_presence_t rreq_attempt_present;
} aodv_control_action_t;

typedef struct aodv_data_action {
    tavrn_direct_peer_t next_hop;
    tavrn_link_data_t data;
} aodv_data_action_t;

/* This synchronous observer sees each queued DATA action immediately before
 * peer-incarnation cleanup removes it.  It must not re-enter aodv_core. */
typedef void (*aodv_peer_incarnation_data_discard_fn)(
    void *context, const aodv_data_action_t *action);

typedef struct aodv_failure_action {
    tavrn_logical_id_t destination;
    tavrn_direct_peer_t peer;
} aodv_failure_action_t;

typedef struct aodv_action {
    aodv_action_type_t type;
    union {
        aodv_control_action_t control;
        aodv_data_action_t data;
        aodv_failure_action_t failure;
    } detail;
} aodv_action_t;

/* Generic RREQ lifecycle telemetry.  This is intentionally route-core-only:
 * it names neither a routed binding nor a link-v2 type.  A router reports the
 * second stage using the correlation copied from the created RREQ action. */
typedef enum aodv_rreq_value_state {
    AODV_RREQ_VALUE_NOT_APPLICABLE = 0,
    AODV_RREQ_VALUE_KNOWN,
    AODV_RREQ_VALUE_UNKNOWN,
} aodv_rreq_value_state_t;

typedef enum aodv_rreq_truth {
    AODV_RREQ_TRUTH_NOT_APPLICABLE = 0,
    AODV_RREQ_TRUTH_FALSE,
    AODV_RREQ_TRUTH_TRUE,
    AODV_RREQ_TRUTH_UNKNOWN,
} aodv_rreq_truth_t;

typedef enum aodv_rreq_scope_source {
    AODV_RREQ_SCOPE_NOT_APPLICABLE = 0,
    AODV_RREQ_SCOPE_ORDINARY,
    AODV_RREQ_SCOPE_APPLICATION_SCOPED,
    AODV_RREQ_SCOPE_ROUTER_HINT,
    AODV_RREQ_SCOPE_UNKNOWN,
} aodv_rreq_scope_source_t;

typedef enum aodv_rreq_lifecycle_stage {
    AODV_RREQ_STAGE_NOT_APPLICABLE = 0,
    AODV_RREQ_STAGE_ACTION_CREATED,
    AODV_RREQ_STAGE_ROUTER_LINK_ENQUEUE,
} aodv_rreq_lifecycle_stage_t;

typedef enum aodv_rreq_action_outcome {
    AODV_RREQ_ACTION_NOT_APPLICABLE = 0,
    AODV_RREQ_ACTION_CREATED,
    AODV_RREQ_ACTION_NOT_CREATED,
    AODV_RREQ_ACTION_UNKNOWN,
} aodv_rreq_action_outcome_t;

typedef enum aodv_rreq_enqueue_outcome {
    AODV_RREQ_ENQUEUE_NOT_APPLICABLE = 0,
    AODV_RREQ_ENQUEUE_ADMITTED,
    AODV_RREQ_ENQUEUE_NOT_ADMITTED,
    AODV_RREQ_ENQUEUE_UNKNOWN,
} aodv_rreq_enqueue_outcome_t;

/* discovery_correlation is a nonzero local generation.  request_id remains
 * the on-wire per-ring identifier, so a discovery can group fresh IDs without
 * conflating two concurrent destinations. */
typedef struct aodv_rreq_lifecycle_record {
    aodv_rreq_attempt_t attempt;
    uint32_t action_created_at_ms;
    uint32_t link_enqueue_at_ms;
    aodv_rreq_lifecycle_stage_t stage;
    aodv_rreq_scope_source_t scope_source;
    aodv_rreq_value_state_t action_created_at_state;
    aodv_rreq_value_state_t link_enqueue_at_state;
    aodv_rreq_truth_t ordinary;
    aodv_rreq_truth_t scoped;
    aodv_rreq_truth_t hint_used;
    aodv_rreq_truth_t fallback;
    aodv_rreq_truth_t full_diameter;
    aodv_rreq_action_outcome_t action_outcome;
    aodv_rreq_enqueue_outcome_t enqueue_outcome;
} aodv_rreq_lifecycle_record_t;

typedef struct aodv_rreq_link_enqueue {
    aodv_rreq_attempt_t attempt;
    uint32_t link_enqueue_at_ms;
    aodv_rreq_enqueue_outcome_t outcome;
} aodv_rreq_link_enqueue_t;

typedef void (*aodv_rreq_lifecycle_fn)(
    void *context, const aodv_rreq_lifecycle_record_t *record);

typedef struct aodv_rreq_telemetry_config {
    void *context;
    aodv_rreq_lifecycle_fn on_lifecycle;
} aodv_rreq_telemetry_config_t;

typedef enum aodv_rreq_telemetry_status {
    AODV_RREQ_TELEMETRY_OK = 0,
    AODV_RREQ_TELEMETRY_INVALID,
    AODV_RREQ_TELEMETRY_BUSY,
} aodv_rreq_telemetry_status_t;

typedef struct aodv_counters {
    uint32_t route_capacity_rejected;
    uint32_t precursor_overflow;
    uint32_t rreq_duplicate;
    uint32_t rreq_loop_prevented;
    uint32_t rreq_rate_limited;
    uint32_t rerr_rate_limited;
    uint32_t pending_busy;
    uint32_t pending_expired;
    uint32_t route_expired;
    uint32_t rrep_stale;
    uint32_t rrep_ack_unmatched;
    uint32_t rrep_ack_timeout;
    uint32_t action_backpressure;
} aodv_counters_t;

typedef union aodv_core_private_storage {
    uint32_t align_u32;
    uint8_t bytes[TAVRN_AODV_CORE_PRIVATE_STORAGE_BYTES];
} aodv_core_private_storage_t;

typedef struct aodv_core {
    aodv_core_config_t config;
    aodv_counters_t counters;
    aodv_rreq_telemetry_config_t rreq_telemetry;
    aodv_core_private_storage_t private_state;
} aodv_core_t;

/* Link failures have one core-owned mutation path.  Repair uses the deferred
 * mode later; AODV_ONLY uses immediate RERR. */
typedef enum aodv_link_failure_mode {
    AODV_LINK_FAILURE_IMMEDIATE_RERR = 0,
    AODV_LINK_FAILURE_DEFER_REPAIR_DESTINATION_RERR,
} aodv_link_failure_mode_t;

typedef enum aodv_failure_status {
    AODV_FAILURE_OK = 0,
    AODV_FAILURE_BUSY,
    AODV_FAILURE_INVALID,
} aodv_failure_status_t;

/* Copied, read-only peer facts used by routed-common incarnation handling.
 * No caller receives mutable route or dedupe storage. */
typedef struct aodv_peer_incarnation_snapshot {
    uint8_t valid_route_count;
    uint8_t control_dedupe_count;
    uint8_t freshness_count;
} aodv_peer_incarnation_snapshot_t;

typedef enum aodv_deferred_rerr_decision {
    AODV_DEFERRED_RERR_ROUTE_REPAIRED = 0,
    AODV_DEFERRED_RERR_REPAIR_FAILED,
} aodv_deferred_rerr_decision_t;

/* The result is true only for 0 < candidate-current < 0x8000.  It is false
 * for equality and the exact half range; dedupe never uses this helper. */
int aodv_serial_is_newer(uint16_t candidate, uint16_t current);

aodv_init_status_t aodv_core_init(aodv_core_t *core,
                                  const aodv_core_config_t *config,
                                  uint32_t now_ms);
aodv_status_t aodv_core_submit_application(aodv_core_t *core,
                                           const tron_application_data_t *data,
                                           uint32_t now_ms);
/* Starts one hinted first RREQ scope.  If it does not discover a route, the
 * core's next RREQ uses net_diameter with a fresh request ID.  The ordinary
 * submit API retains the AODV_ONLY expanding-ring sequence. */
aodv_status_t aodv_core_submit_application_scoped(
    aodv_core_t *core, const tron_application_data_t *data,
    uint8_t initial_scope, uint32_t now_ms);
/* The existing scoped wrapper remains APPLICATION_SCOPED.  Router augmentation
 * uses this form to label a generic nonzero initial scope as a hint without
 * introducing a FULL/GTT dependency into the core. */
aodv_status_t aodv_core_submit_application_scoped_ex(
    aodv_core_t *core, const tron_application_data_t *data,
    uint8_t initial_scope, aodv_rreq_scope_source_t scope_source,
    uint32_t now_ms);
/* The shared RREQ limiter is checked before this allocates the on-wire request
 * ID.  On success the caller receives one complete SEND_RREQ action and owns
 * any physical retry/completion policy. */
aodv_single_rreq_status_t aodv_core_create_single_rreq(
    aodv_core_t *core, const tavrn_logical_id_t *destination, uint8_t scope,
    uint32_t now_ms, aodv_action_t *action_out);
/* Semantically validates an RREP for the supplied retained local attempt. It
 * applies normal RREP validation, blacklist, dedupe, route-install, and ACK
 * policy, but deliberately has no ordinary discovery-slot prerequisite or
 * pending-DATA/discovery release behavior. */
aodv_status_t aodv_core_ingest_rrep_for_attempt(
    aodv_core_t *core, const aodv_control_input_t *input,
    const aodv_rreq_attempt_t *attempt, uint32_t now_ms);
aodv_rrep_attempt_match_status_t aodv_core_rrep_matches_attempt(
    const aodv_core_t *core, const aodv_control_input_t *input,
    const aodv_rreq_attempt_t *attempt);
aodv_rreq_telemetry_status_t aodv_core_set_rreq_telemetry(
    aodv_core_t *core,
    const aodv_rreq_telemetry_config_t *config_or_null);
/* The router reports the immediate scheduler admission result only.  This
 * stage never claims physical transmission completion. */
aodv_rreq_telemetry_status_t aodv_core_report_rreq_link_enqueue(
    aodv_core_t *core, const aodv_rreq_link_enqueue_t *enqueue);
aodv_status_t aodv_core_ingest_control(aodv_core_t *core,
                                        const aodv_control_input_t *input,
                                        uint32_t now_ms);
/* Read-only DATA admission check for link candidate ownership.  It reports the
 * same acceptance status as ingress without changing routes, precursors,
 * actions, or counters. */
aodv_status_t aodv_core_probe_data(const aodv_core_t *core,
                                   const aodv_data_input_t *input,
                                   uint32_t now_ms);
aodv_status_t aodv_core_ingest_data(aodv_core_t *core,
                                     const aodv_data_input_t *input,
                                     uint32_t now_ms);
aodv_status_t aodv_core_mark_action_sent(aodv_core_t *core,
                                           uint16_t action_token,
                                           uint32_t now_ms);
/* Cancels only a reserved RREP-ACK wait that was never marked sent.  A sent
 * wait remains active; callers must not use this to suppress live probes. */
aodv_status_t aodv_core_cancel_unsent_action(aodv_core_t *core,
                                              uint16_t action_token);
aodv_status_t aodv_core_tick(aodv_core_t *core, uint32_t now_ms);
aodv_action_poll_status_t aodv_core_poll_action(aodv_core_t *core,
                                                 aodv_action_t *action_out);
aodv_route_query_status_t aodv_core_route_snapshot(
    const aodv_core_t *core, const tavrn_logical_id_t *destination,
    aodv_route_snapshot_t *snapshot_out);
const aodv_counters_t *aodv_core_counters(const aodv_core_t *core);
/* Pure copied owner facts for one canonical subject.  This API deliberately
 * names no GTT/FULL type and must remain linkable in AODV_ONLY. */
tavrn_aodv_subject_demand_snapshot_t aodv_core_subject_demand_snapshot(
    const aodv_core_t *core, const tavrn_logical_id_t *subject,
    const tavrn_adva_t *canonical_subject, uint32_t now_ms);

aodv_failure_status_t aodv_core_report_link_failure(
    aodv_core_t *core, const tavrn_direct_peer_t *failed_next_hop,
    const tavrn_logical_id_t *repair_destination,
    aodv_link_failure_mode_t mode, uint32_t now_ms);
aodv_failure_status_t aodv_core_finish_deferred_rerr(
    aodv_core_t *core, const tavrn_logical_id_t *repair_destination,
    aodv_deferred_rerr_decision_t decision, uint32_t now_ms);
/* Reserves the normal sorted RERR before clearing any peer-scoped AODV
 * freshness/dedupe state.  BUSY therefore leaves all peer state untouched. */
aodv_failure_status_t aodv_core_reset_peer_incarnation(
    aodv_core_t *core, const tavrn_direct_peer_t *peer, uint32_t now_ms);
/* As reset_peer_incarnation(), while reporting every queued DATA action that
 * this call discards.  Passing NULL preserves the ordinary reset behavior. */
aodv_failure_status_t aodv_core_reset_peer_incarnation_with_data_discard(
    aodv_core_t *core, const tavrn_direct_peer_t *peer, uint32_t now_ms,
    aodv_peer_incarnation_data_discard_fn on_data_discard, void *context);
/* Drops only queued actions that could send through or name a barred direct
 * peer.  It is the BUSY-time quarantine half of a later reset; routes and
 * freshness remain untouched until a reset reserves its RERR. */
aodv_failure_status_t aodv_core_quarantine_peer_incarnation(
    aodv_core_t *core, const tavrn_direct_peer_t *peer);
/* As quarantine_peer_incarnation(), while reporting every queued DATA action
 * that this call discards.  Passing NULL preserves the ordinary quarantine
 * behavior. */
aodv_failure_status_t aodv_core_quarantine_peer_incarnation_with_data_discard(
    aodv_core_t *core, const tavrn_direct_peer_t *peer,
    aodv_peer_incarnation_data_discard_fn on_data_discard, void *context);
aodv_failure_status_t aodv_core_peer_incarnation_snapshot(
    const aodv_core_t *core, const tavrn_direct_peer_t *peer,
    aodv_peer_incarnation_snapshot_t *snapshot_out);
/* Clears the one core's width-dependent route, discovery, pending, action and
 * dedupe state before exposing a new local identity width. */
aodv_init_status_t aodv_core_reconfigure_identity(
    aodv_core_t *core, const tavrn_direct_peer_t *local_peer,
    uint32_t now_ms);

#endif /* AODV_CORE_H */
