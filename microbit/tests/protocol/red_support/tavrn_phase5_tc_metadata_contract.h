#ifndef TAVRN_PHASE5_TC_METADATA_CONTRACT_H
#define TAVRN_PHASE5_TC_METADATA_CONTRACT_H

/*
 * Step 5g contract.  General social metadata and TC maintenance are FULL-only
 * maintenance behavior.  The RED backend is deliberately inert: it does not
 * emulate queue admission, GTT mutation, TC gossip, or metadata selection.
 * GREEN binds these names only when tavrn_maintenance.h exports the production
 * API marker below.
 */
#include <stdint.h>

#include "tavrn_maintenance.h"
#include "tavrn_wire_v2.h"

#if !defined(TAVRN_MAINTENANCE_TC_METADATA_API)

#define TAVRN_TC_METADATA_CANDIDATE_CAPACITY 4u

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
} tavrn_tc_metadata_config_t;

typedef struct tavrn_tc_metadata_state {
    uint8_t opaque;
} tavrn_tc_metadata_state_t;

typedef struct tavrn_tc_metadata_action {
    uint8_t pdu[24];
    tavrn_adva_t origin;
    tavrn_adva_t subject;
    uint16_t tc_sequence;
    uint8_t ttl;
    uint8_t hops;
    tavrn_tc_event_t event;
    uint8_t valid;
} tavrn_tc_metadata_action_t;

/* This ticket is the one generic local TC serial reservation shared by the
 * existing mentorship JOIN enqueue path and future maintenance LEAVE paths.
 * It is intentionally not a second mentorship-local counter. */
typedef struct tavrn_tc_sequence_ticket {
    uint16_t sequence;
    tavrn_tc_event_t event;
    uint8_t valid;
} tavrn_tc_sequence_ticket_t;

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

typedef struct tavrn_metadata_rx {
    tavrn_metadata_frame_kind_t frame_kind;
    tavrn_adva_t outer_transmitter;
    tavrn_metadata_candidate_t entries[TAVRN_TC_METADATA_CANDIDATE_CAPACITY + 1u];
    uint8_t sid8;
    uint8_t metadata_flag;
    uint8_t count;
    uint8_t unreachable_count;
    /* Caller has already resolved the candidate's local freshness/context
     * predicate. Stale is a fail-closed admission input, never a merge hint. */
    uint8_t stale;
} tavrn_metadata_rx_t;

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

tavrn_tc_metadata_status_t tavrn_maintenance_tc_metadata_init(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_metadata_config_t *config);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_sequence_prepare(
    tavrn_tc_metadata_state_t *state, tavrn_tc_event_t event,
    tavrn_tc_sequence_ticket_t *ticket_out);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_sequence_commit(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_sequence_ticket_t *ticket,
    tavrn_tc_admission_t admission, uint32_t now_ms);
/* Production runtime closure: these are called only from the corresponding
 * confirmed sources, never from generic local expiry or terminal-fault paths. */
tavrn_tc_metadata_status_t tavrn_maintenance_tc_on_verified_departure(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *subject, uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_on_direct_timeout_departure(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *subject, uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_on_retry_exhausted(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *failed_next_hop,
    const tavrn_adva_t *final_destination_or_null, uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_prepare_origin(
    tavrn_tc_metadata_state_t *state, tavrn_tc_origin_cause_t cause,
    const tavrn_adva_t *subject, const tavrn_adva_t *failed_next_hop_or_null,
    const tavrn_adva_t *final_destination_or_null, uint32_t now_ms,
    tavrn_tc_metadata_action_t *action_out);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_commit_origin(
    tavrn_tc_metadata_state_t *state, const tavrn_tc_metadata_action_t *action,
    tavrn_tc_admission_t admission, uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_receive(
    tavrn_tc_metadata_state_t *state, const uint8_t pdu[24], uint8_t pdu_len,
    const tavrn_adva_t *outer_transmitter, uint32_t now_ms,
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
    const tavrn_validated_control_t *base,
    const tavrn_metadata_selection_t *selection,
    tavrn_metadata_attached_control_t *attached_out);
tavrn_tc_metadata_status_t tavrn_maintenance_metadata_receive(
    tavrn_tc_metadata_state_t *state, const tavrn_metadata_rx_t *rx,
    uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_metadata_receive_control(
    tavrn_tc_metadata_state_t *state, const tavrn_rx_control_event_t *event,
    uint32_t now_ms);
tavrn_tc_metadata_status_t tavrn_maintenance_tc_metadata_snapshot(
    const tavrn_tc_metadata_state_t *state, tavrn_tc_metadata_snapshot_t *snapshot_out);

#endif /* production API absent */

#if defined(TAVRN_PHASE5_TC_METADATA_RED_MODE)
#define phase5_tc_metadata_init phase5_tc_metadata_red_init
#define phase5_tc_sequence_prepare phase5_tc_metadata_red_sequence_prepare
#define phase5_tc_sequence_commit phase5_tc_metadata_red_sequence_commit
#define phase5_tc_prepare_origin phase5_tc_metadata_red_prepare_origin
#define phase5_tc_commit_origin phase5_tc_metadata_red_commit_origin
#define phase5_tc_receive phase5_tc_metadata_red_receive
#define phase5_tc_commit_relay phase5_tc_metadata_red_commit_relay
#define phase5_metadata_create phase5_tc_metadata_red_create
#define phase5_metadata_select phase5_tc_metadata_red_select
#define phase5_metadata_commit phase5_tc_metadata_red_commit
#define phase5_metadata_attach_control phase5_tc_metadata_red_attach_control
#define phase5_metadata_receive phase5_tc_metadata_red_receive_metadata
#define phase5_metadata_receive_control phase5_tc_metadata_red_receive_control
#define phase5_tc_metadata_snapshot phase5_tc_metadata_red_snapshot

tavrn_tc_metadata_status_t phase5_tc_metadata_red_init(
    tavrn_tc_metadata_state_t *, const tavrn_tc_metadata_config_t *);
tavrn_tc_metadata_status_t phase5_tc_metadata_red_sequence_prepare(
    tavrn_tc_metadata_state_t *, tavrn_tc_event_t, tavrn_tc_sequence_ticket_t *);
tavrn_tc_metadata_status_t phase5_tc_metadata_red_sequence_commit(
    tavrn_tc_metadata_state_t *, const tavrn_tc_sequence_ticket_t *,
    tavrn_tc_admission_t, uint32_t);
tavrn_tc_metadata_status_t phase5_tc_metadata_red_prepare_origin(
    tavrn_tc_metadata_state_t *, tavrn_tc_origin_cause_t, const tavrn_adva_t *,
    const tavrn_adva_t *, const tavrn_adva_t *, uint32_t,
    tavrn_tc_metadata_action_t *);
tavrn_tc_metadata_status_t phase5_tc_metadata_red_commit_origin(
    tavrn_tc_metadata_state_t *, const tavrn_tc_metadata_action_t *,
    tavrn_tc_admission_t, uint32_t);
tavrn_tc_metadata_status_t phase5_tc_metadata_red_receive(
    tavrn_tc_metadata_state_t *, const uint8_t[24], uint8_t, const tavrn_adva_t *,
    uint32_t, tavrn_tc_metadata_action_t *);
tavrn_tc_metadata_status_t phase5_tc_metadata_red_commit_relay(
    tavrn_tc_metadata_state_t *, const tavrn_tc_metadata_action_t *,
    tavrn_tc_admission_t, uint32_t);
tavrn_tc_metadata_status_t phase5_tc_metadata_red_create(
    tavrn_tc_metadata_state_t *, const tavrn_metadata_candidate_t *, uint32_t);
tavrn_tc_metadata_status_t phase5_tc_metadata_red_select(
    const tavrn_tc_metadata_state_t *, tavrn_metadata_frame_kind_t, uint8_t,
    uint32_t, tavrn_metadata_selection_t *);
tavrn_tc_metadata_status_t phase5_tc_metadata_red_commit(
    tavrn_tc_metadata_state_t *, const tavrn_metadata_selection_t *,
    tavrn_tc_admission_t, uint32_t);
tavrn_tc_metadata_status_t phase5_tc_metadata_red_attach_control(
    const tavrn_validated_control_t *, const tavrn_metadata_selection_t *,
    tavrn_metadata_attached_control_t *);
tavrn_tc_metadata_status_t phase5_tc_metadata_red_receive_metadata(
    tavrn_tc_metadata_state_t *, const tavrn_metadata_rx_t *, uint32_t);
tavrn_tc_metadata_status_t phase5_tc_metadata_red_receive_control(
    tavrn_tc_metadata_state_t *, const tavrn_rx_control_event_t *, uint32_t);
tavrn_tc_metadata_status_t phase5_tc_metadata_red_snapshot(
    const tavrn_tc_metadata_state_t *, tavrn_tc_metadata_snapshot_t *);
#else
#define phase5_tc_metadata_init tavrn_maintenance_tc_metadata_init
#define phase5_tc_sequence_prepare tavrn_maintenance_tc_sequence_prepare
#define phase5_tc_sequence_commit tavrn_maintenance_tc_sequence_commit
#define phase5_tc_prepare_origin tavrn_maintenance_tc_prepare_origin
#define phase5_tc_commit_origin tavrn_maintenance_tc_commit_origin
#define phase5_tc_receive tavrn_maintenance_tc_receive
#define phase5_tc_commit_relay tavrn_maintenance_tc_commit_relay
#define phase5_metadata_create tavrn_maintenance_metadata_create
#define phase5_metadata_select tavrn_maintenance_metadata_select
#define phase5_metadata_commit tavrn_maintenance_metadata_commit
#define phase5_metadata_attach_control tavrn_maintenance_metadata_attach_control
#define phase5_metadata_receive tavrn_maintenance_metadata_receive
#define phase5_metadata_receive_control tavrn_maintenance_metadata_receive_control
#define phase5_tc_metadata_snapshot tavrn_maintenance_tc_metadata_snapshot
#endif

#endif /* TAVRN_PHASE5_TC_METADATA_CONTRACT_H */
