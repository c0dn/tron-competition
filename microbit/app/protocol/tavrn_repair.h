#ifndef TAVRN_REPAIR_H
#define TAVRN_REPAIR_H

#include <stdint.h>

#include "aodv_core.h"
#include "tavrn_link_v2.h"

#define TAVRN_REPAIR_API 1
#define TAVRN_REPAIR_CONTEXT_CAPACITY 1u
#define TAVRN_REPAIR_BUFFER_CAPACITY  4u
#define TAVRN_REPAIR_RESERVATION_NONE 0u

typedef char tavrn_repair_context_capacity_guard[
    (TAVRN_REPAIR_CONTEXT_CAPACITY == 1u) ? 1 : -1];
typedef char tavrn_repair_buffer_capacity_guard[
    (TAVRN_REPAIR_BUFFER_CAPACITY == 4u) ? 1 : -1];

typedef uint16_t tavrn_repair_reservation_token_t;

typedef enum tavrn_repair_status {
    TAVRN_REPAIR_OK = 0,
    TAVRN_REPAIR_DECLINED,
    TAVRN_REPAIR_BUSY,
    TAVRN_REPAIR_IGNORED,
    TAVRN_REPAIR_INVALID,
    TAVRN_REPAIR_UNAVAILABLE,
} tavrn_repair_status_t;

typedef enum tavrn_repair_state {
    TAVRN_REPAIR_STATE_IDLE = 0,
    TAVRN_REPAIR_STATE_WAIT_SMART_TTL_RREP,
    TAVRN_REPAIR_STATE_WAIT_FULL_SCOPE_RREP,
    TAVRN_REPAIR_STATE_FLUSHING,
    TAVRN_REPAIR_STATE_DROPPING,
    TAVRN_REPAIR_STATE_WAIT_LEAVE_RETRY,
    TAVRN_REPAIR_STATE_COOLDOWN,
} tavrn_repair_state_t;

typedef enum tavrn_repair_retry_disposition {
    TAVRN_REPAIR_RETRY_DECLINED = 0,
    TAVRN_REPAIR_RETRY_OWNED,
    TAVRN_REPAIR_RETRY_INVALID,
} tavrn_repair_retry_disposition_t;

typedef enum tavrn_repair_setup_state {
    TAVRN_REPAIR_SETUP_NONE = 0,
    TAVRN_REPAIR_SETUP_ATOMIC_OWNED,
    TAVRN_REPAIR_SETUP_NORMAL_FAILURE,
    TAVRN_REPAIR_SETUP_INVALID,
} tavrn_repair_setup_state_t;

typedef enum tavrn_repair_leave_state {
    TAVRN_REPAIR_LEAVE_NONE = 0,
    TAVRN_REPAIR_LEAVE_HELD,
    TAVRN_REPAIR_LEAVE_CANCELED,
    TAVRN_REPAIR_LEAVE_ORIGINATED,
} tavrn_repair_leave_state_t;

typedef enum tavrn_repair_candidate_status {
    TAVRN_REPAIR_CANDIDATE_NOT_APPLICABLE = 0,
    TAVRN_REPAIR_CANDIDATE_RESERVED,
    TAVRN_REPAIR_CANDIDATE_BUSY,
    TAVRN_REPAIR_CANDIDATE_INVALID,
} tavrn_repair_candidate_status_t;

typedef enum tavrn_repair_link_custody_terminal {
    TAVRN_REPAIR_LINK_CUSTODY_TRANSFERRED = 0,
    TAVRN_REPAIR_LINK_CUSTODY_REJECTED,
    TAVRN_REPAIR_LINK_CUSTODY_RETRY_EXHAUSTED,
    TAVRN_REPAIR_LINK_CUSTODY_INVALID,
} tavrn_repair_link_custody_terminal_t;

typedef enum tavrn_repair_rreq_purpose {
    TAVRN_REPAIR_RREQ_NONE = 0,
    TAVRN_REPAIR_RREQ_SMART_TTL,
    TAVRN_REPAIR_RREQ_FULL_SCOPE,
} tavrn_repair_rreq_purpose_t;

typedef enum tavrn_repair_high_token_owner {
    TAVRN_REPAIR_HIGH_TOKEN_OWNER_REPAIR = 0,
} tavrn_repair_high_token_owner_t;

typedef enum tavrn_repair_high_token_status {
    TAVRN_REPAIR_HIGH_TOKEN_OK = 0,
    TAVRN_REPAIR_HIGH_TOKEN_BUSY,
    TAVRN_REPAIR_HIGH_TOKEN_INVALID,
} tavrn_repair_high_token_status_t;

typedef enum tavrn_repair_leave_status {
    TAVRN_REPAIR_LEAVE_OK = 0,
    TAVRN_REPAIR_LEAVE_BUSY,
    TAVRN_REPAIR_LEAVE_INVALID,
} tavrn_repair_leave_status_t;

typedef enum tavrn_repair_action_type {
    TAVRN_REPAIR_ACTION_NONE = 0,
    TAVRN_REPAIR_ACTION_RREQ_CREATED,
    TAVRN_REPAIR_ACTION_FLUSH_DATA,
    TAVRN_REPAIR_ACTION_DROP_DATA,
} tavrn_repair_action_type_t;

typedef enum tavrn_repair_rreq_enqueue_status {
    TAVRN_REPAIR_RREQ_ENQUEUE_ADMITTED = 0,
    TAVRN_REPAIR_RREQ_ENQUEUE_BUSY,
    TAVRN_REPAIR_RREQ_ENQUEUE_LOCAL_NOT_ATTEMPTED,
    TAVRN_REPAIR_RREQ_ENQUEUE_INVALID,
} tavrn_repair_rreq_enqueue_status_t;

typedef enum tavrn_repair_completion_kind {
    TAVRN_REPAIR_COMPLETION_RREQ_TX_DONE = 0,
    TAVRN_REPAIR_COMPLETION_RREQ_TX_FAILED,
    TAVRN_REPAIR_COMPLETION_DATA_FLUSHED,
    TAVRN_REPAIR_COMPLETION_DATA_DROPPED,
} tavrn_repair_completion_kind_t;

typedef enum tavrn_repair_terminal_kind {
    TAVRN_REPAIR_TERMINAL_NONE = 0,
    TAVRN_REPAIR_TERMINAL_REPAIRED,
    TAVRN_REPAIR_TERMINAL_FAILED,
} tavrn_repair_terminal_kind_t;

typedef struct tavrn_repair_dependency_ops {
    void *context;
    aodv_failure_status_t (*begin_deferred_rerr)(
        void *context, const tavrn_direct_peer_t *failed_next_hop,
        const tavrn_logical_id_t *destination, uint32_t now_ms);
    aodv_failure_status_t (*finish_deferred_rerr)(
        void *context, const tavrn_logical_id_t *destination,
        aodv_deferred_rerr_decision_t decision, uint32_t now_ms);
    aodv_single_rreq_status_t (*create_single_rreq)(
        void *context, const tavrn_logical_id_t *destination, uint8_t scope,
        uint32_t now_ms, aodv_action_t *action_out);
    tavrn_repair_high_token_status_t (*reserve_high_token)(
        void *context, tavrn_repair_high_token_owner_t owner,
        tavrn_repair_rreq_purpose_t purpose, uint16_t *token_out);
    tavrn_repair_high_token_status_t (*release_high_token)(
        void *context, tavrn_repair_high_token_owner_t owner,
        tavrn_repair_rreq_purpose_t purpose, uint16_t token);
    tavrn_repair_leave_status_t (*originate_held_leave)(
        void *context, const tavrn_direct_peer_t *failed_next_hop,
        uint32_t now_ms);
} tavrn_repair_dependency_ops_t;

typedef struct tavrn_repair_config {
    uint8_t enabled;
    uint8_t net_diameter;
    uint8_t data_capacity;
    uint32_t path_discovery_ms;
    uint32_t repair_timeout_ms;
    uint32_t repair_cooldown_ms;
    const tavrn_repair_dependency_ops_t *operations;
} tavrn_repair_config_t;

typedef struct tavrn_repair_link_custody_record {
    tavrn_direct_peer_t next_hop;
    tavrn_link_data_t data;
} tavrn_repair_link_custody_record_t;

typedef struct tavrn_repair_link_custody_snapshot {
    tavrn_repair_link_custody_record_t records[TAVRN_REPAIR_BUFFER_CAPACITY];
    uint8_t count;
} tavrn_repair_link_custody_snapshot_t;

typedef struct tavrn_repair_retry_input {
    tavrn_link_event_type_t trigger;
    tavrn_owned_data_event_t owned;
    uint8_t smart_ttl_scope;
    tavrn_repair_link_custody_snapshot_t same_destination_link_custody;
} tavrn_repair_retry_input_t;

typedef struct tavrn_repair_setup_result {
    tavrn_repair_setup_state_t state;
    tavrn_repair_retry_disposition_t disposition;
    aodv_link_failure_mode_t failure_mode;
    aodv_failure_status_t deferred_rerr_status;
    tavrn_repair_leave_state_t leave_state;
    uint8_t data_copied;
    uint8_t deferred_rerr_started;
    uint8_t held_leave_created;
    uint8_t buffered_count;
    uint8_t reserved_link_slots;
} tavrn_repair_setup_result_t;

typedef struct tavrn_repair_reservation {
    tavrn_repair_reservation_token_t token;
    tavrn_logical_id_t destination;
    uint8_t valid;
} tavrn_repair_reservation_t;

typedef struct tavrn_repair_candidate_result {
    tavrn_repair_candidate_status_t status;
    tavrn_repair_reservation_t reservation;
    uint8_t actual_or_reserved_count;
} tavrn_repair_candidate_result_t;

typedef struct tavrn_repair_link_custody_result {
    uint8_t buffered_count;
    uint8_t reserved_link_slots;
    uint8_t released_reservation;
    uint8_t converted_to_buffer;
} tavrn_repair_link_custody_result_t;

typedef struct tavrn_repair_link_custody_reconcile_result {
    uint8_t released_count;
    uint8_t buffered_count;
    uint8_t reserved_link_slots;
} tavrn_repair_link_custody_reconcile_result_t;

/* Link retains ownership until its exact terminal.  A retry-exhausted terminal
 * must carry the complete by-value evidence so alternate-route flushing keeps
 * its source bytes, attempt facts, and transit ownership intact. */
typedef struct tavrn_repair_link_custody_terminal_input {
    tavrn_direct_peer_t next_hop;
    tavrn_link_data_t data;
    tavrn_owned_data_event_t owned;
    uint8_t owned_present;
} tavrn_repair_link_custody_terminal_input_t;

typedef struct tavrn_repair_action {
    tavrn_repair_action_type_t type;
    tavrn_repair_rreq_purpose_t purpose;
    tavrn_repair_high_token_owner_t token_owner;
    aodv_action_t aodv_action;
    aodv_rreq_attempt_t rreq_attempt;
    tavrn_owned_data_event_t owned;
    uint16_t token;
    uint8_t rreq_created;
    uint8_t buffer_index;
} tavrn_repair_action_t;

typedef struct tavrn_repair_rreq_enqueue {
    tavrn_repair_rreq_enqueue_status_t status;
    tavrn_repair_rreq_purpose_t purpose;
    tavrn_repair_high_token_owner_t token_owner;
    aodv_rreq_attempt_t rreq_attempt;
    uint16_t token;
} tavrn_repair_rreq_enqueue_t;

typedef struct tavrn_repair_completion {
    tavrn_repair_completion_kind_t kind;
    tavrn_repair_rreq_purpose_t purpose;
    tavrn_repair_high_token_owner_t token_owner;
    aodv_rreq_attempt_t rreq_attempt;
    uint16_t token;
    uint8_t completed_channel_mask;
    uint8_t buffer_index;
} tavrn_repair_completion_t;

typedef struct tavrn_repair_rrep_completion {
    tavrn_repair_rreq_purpose_t purpose;
    tavrn_repair_high_token_owner_t token_owner;
    aodv_rreq_attempt_t rreq_attempt;
    uint16_t token;
    tavrn_direct_peer_t transmitter;
    tavrn_direct_peer_t installed_next_hop;
    uint8_t router_admitted;
    uint8_t route_installed;
} tavrn_repair_rrep_completion_t;

/* Complete copied correlation for a composed pre-AODV RREP interceptor. */
typedef struct tavrn_repair_active_rreq_snapshot {
    tavrn_repair_state_t state;
    tavrn_repair_rreq_purpose_t purpose;
    tavrn_repair_high_token_owner_t token_owner;
    aodv_rreq_attempt_t attempt;
    tavrn_direct_peer_t failed_next_hop;
    tavrn_logical_id_t destination;
    uint32_t total_deadline_ms;
    uint32_t stage_response_deadline_ms;
    uint16_t token;
    uint8_t active;
    uint8_t tx_done;
} tavrn_repair_active_rreq_snapshot_t;

typedef struct tavrn_repair_terminal {
    tavrn_repair_terminal_kind_t kind;
    aodv_deferred_rerr_decision_t deferred_rerr_decision;
    tavrn_repair_leave_state_t leave_state;
    tavrn_direct_peer_t failed_next_hop;
    tavrn_logical_id_t destination;
    uint8_t buffered_count;
} tavrn_repair_terminal_t;

typedef struct tavrn_repair_snapshot {
    tavrn_repair_state_t state;
    tavrn_direct_peer_t failed_next_hop;
    tavrn_logical_id_t destination;
    uint32_t deadline_ms;
    uint32_t cooldown_deadline_ms;
    uint16_t smart_ttl_request_id;
    uint16_t full_scope_request_id;
    uint16_t active_token;
    uint8_t buffered_count;
    uint8_t reserved_link_slots;
    uint8_t actual_or_reserved_count;
    uint8_t smart_ttl_emissions;
    uint8_t full_scope_emissions;
    uint8_t inner_retry_count;
    uint8_t flush_count;
    uint8_t drop_count;
    uint8_t deferred_rerr_active;
    tavrn_repair_leave_state_t leave_state;
} tavrn_repair_snapshot_t;

/* The complete state is caller-owned fixed storage.  A slot is either copied
 * DATA, a link-owned same-D reservation, or a pre-AODV candidate reservation. */
#define TAVRN_REPAIR_SLOT_EMPTY              0u
#define TAVRN_REPAIR_SLOT_BUFFERED           1u
#define TAVRN_REPAIR_SLOT_LINK_RESERVED      2u
#define TAVRN_REPAIR_SLOT_CANDIDATE_RESERVED 3u

typedef struct tavrn_repair_slot {
    tavrn_owned_data_event_t owned;
    uint16_t reservation_token;
    uint16_t completion_token;
    uint8_t kind;
    uint8_t action_in_flight;
} tavrn_repair_slot_t;

typedef struct tavrn_repair {
    tavrn_repair_config_t config;
    tavrn_repair_slot_t slots[TAVRN_REPAIR_BUFFER_CAPACITY];
    tavrn_direct_peer_t failed_next_hop;
    tavrn_logical_id_t destination;
    aodv_action_t active_aodv_action;
    aodv_rreq_attempt_t active_attempt;
    uint32_t deadline_ms;
    uint32_t stage_response_deadline_ms;
    uint32_t cooldown_deadline_ms;
    uint16_t next_reservation_token;
    uint16_t next_completion_token;
    uint16_t active_token;
    uint16_t smart_ttl_request_id;
    uint16_t full_scope_request_id;
    tavrn_repair_state_t state;
    tavrn_repair_rreq_purpose_t active_purpose;
    tavrn_repair_leave_state_t leave_state;
    uint8_t initialized;
    uint8_t deferred_rerr_active;
    uint8_t smart_ttl_scope;
    uint8_t smart_ttl_emissions;
    uint8_t full_scope_emissions;
    uint8_t flush_count;
    uint8_t drop_count;
    uint8_t active_created;
    uint8_t active_enqueued;
    uint8_t active_tx_done;
    uint8_t active_tx_failed;
    uint8_t active_token_held;
    uint8_t active_release_only;
    uint8_t failure_pending;
} tavrn_repair_t;

tavrn_repair_status_t tavrn_repair_init(
    tavrn_repair_t *repair, const tavrn_repair_config_t *config);
tavrn_repair_status_t tavrn_repair_retry_exhausted(
    tavrn_repair_t *repair, const tavrn_repair_retry_input_t *input,
    uint32_t now_ms, tavrn_repair_setup_result_t *result_out);
tavrn_repair_status_t tavrn_repair_reserve_candidate(
    tavrn_repair_t *repair, const tavrn_link_data_t *data, uint32_t now_ms,
    tavrn_repair_candidate_result_t *result_out);
tavrn_repair_status_t tavrn_repair_commit_candidate(
    tavrn_repair_t *repair, const tavrn_repair_reservation_t *reservation,
    const tavrn_link_data_t *data, uint32_t now_ms);
tavrn_repair_status_t tavrn_repair_rollback_candidate(
    tavrn_repair_t *repair, const tavrn_repair_reservation_t *reservation,
    uint32_t now_ms);
tavrn_repair_status_t tavrn_repair_observe_link_custody(
    tavrn_repair_t *repair,
    const tavrn_repair_link_custody_terminal_input_t *input,
    tavrn_repair_link_custody_terminal_t terminal, uint32_t now_ms,
    tavrn_repair_link_custody_result_t *result_out);
/* Removes only LINK_RESERVED records absent from the current exact copied link
 * custody snapshot.  Buffered DATA and records present with another next hop
 * are retained. */
tavrn_repair_status_t tavrn_repair_reconcile_link_custody(
    tavrn_repair_t *repair,
    const tavrn_repair_link_custody_snapshot_t *current_link_custody,
    uint32_t now_ms,
    tavrn_repair_link_custody_reconcile_result_t *result_out);
tavrn_repair_status_t tavrn_repair_owner_tick(
    tavrn_repair_t *repair, uint32_t now_ms, tavrn_repair_action_t *action_out);
tavrn_repair_status_t tavrn_repair_rreq_enqueue(
    tavrn_repair_t *repair, const tavrn_repair_rreq_enqueue_t *enqueue,
    uint32_t now_ms);
/* Reject an exact not-admitted FLUSH/DROP action without consuming its copied
 * DATA.  A later owner tick returns the same slot for retry. */
tavrn_repair_status_t tavrn_repair_data_action_not_admitted(
    tavrn_repair_t *repair, const tavrn_repair_action_t *action,
    uint32_t now_ms);
tavrn_repair_status_t tavrn_repair_complete(
    tavrn_repair_t *repair, const tavrn_repair_completion_t *completion,
    uint32_t now_ms, tavrn_repair_terminal_t *terminal_out);
tavrn_repair_status_t tavrn_repair_receive_rrep(
    tavrn_repair_t *repair, const tavrn_repair_rrep_completion_t *completion,
    uint32_t now_ms, tavrn_repair_terminal_t *terminal_out);
tavrn_repair_status_t tavrn_repair_snapshot(
    const tavrn_repair_t *repair, tavrn_repair_snapshot_t *snapshot_out);
tavrn_repair_status_t tavrn_repair_active_rreq_snapshot(
    const tavrn_repair_t *repair,
    tavrn_repair_active_rreq_snapshot_t *snapshot_out);

#endif /* TAVRN_REPAIR_H */
