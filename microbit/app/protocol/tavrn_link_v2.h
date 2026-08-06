#ifndef TAVRN_LINK_V2_H
#define TAVRN_LINK_V2_H

#include <stdint.h>

#include "ble_mesh_scheduler.h"
#include "tavrn_wire_v2.h"

#define TAVRN_RX_CANDIDATE_TOKEN_NONE  0u
#define TAVRN_LINK_CUSTODY_CAPACITY     4u
#define TAVRN_CUSTODY_SLOT_NONE         0xffu
#define TAVRN_LINK_DATA_DEDUPE_CAPACITY 16u
#define TAVRN_LINK_FLOOD_DEDUPE_CAPACITY 16u

typedef uint16_t tavrn_rx_candidate_token_t;

typedef enum tavrn_rx_decision {
    TAVRN_RX_ACCEPTED = 0,
    TAVRN_RX_BUSY,
    TAVRN_RX_REJECTED,
} tavrn_rx_decision_t;

typedef enum tavrn_link_send_status {
    TAVRN_LINK_SEND_OK = 0,
    TAVRN_LINK_SEND_LOCAL_NOT_ATTEMPTED,
    TAVRN_LINK_SEND_BUSY,
    TAVRN_LINK_SEND_NO_SLOT,
    TAVRN_LINK_SEND_INVALID,
    TAVRN_LINK_SEND_IDENTITY_CONFLICT,
    TAVRN_LINK_SEND_MESH_FAULTED,
} tavrn_link_send_status_t;

typedef enum tavrn_link_init_status {
    TAVRN_LINK_INIT_OK = 0,
    TAVRN_LINK_INIT_INVALID_ARGUMENT,
    TAVRN_LINK_INIT_INVALID_LOCAL_IDENTITY,
    TAVRN_LINK_INIT_INVALID_CONFIG,
} tavrn_link_init_status_t;

typedef enum tavrn_link_resolve_status {
    TAVRN_LINK_RESOLVE_OK = 0,
    TAVRN_LINK_RESOLVE_TOKEN_INVALID,
    TAVRN_LINK_RESOLVE_ALREADY_RESOLVED,
    TAVRN_LINK_RESOLVE_INVALID_DECISION,
    TAVRN_LINK_RESOLVE_MESH_FAULTED,
} tavrn_link_resolve_status_t;

typedef enum tavrn_link_step_status {
    TAVRN_LINK_STEP_NO_EVENT = 0,
    TAVRN_LINK_STEP_EVENT,
    TAVRN_LINK_STEP_ADDITIONAL_DATA_BUSY,
    TAVRN_LINK_STEP_CANDIDATE_TIMEOUT_BUSY,
    TAVRN_LINK_STEP_INVALID,
} tavrn_link_step_status_t;

typedef enum tavrn_link_event_type {
    TAVRN_LINK_EVENT_NONE = 0,
    TAVRN_LINK_EVENT_RX_DATA_CANDIDATE,
    TAVRN_LINK_EVENT_RX_CONTROL,
    TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED,
    TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED,
    TAVRN_LINK_EVENT_CUSTODY_BUSY_EXPIRED,
    TAVRN_LINK_EVENT_CUSTODY_REJECTED,
    TAVRN_LINK_EVENT_RETRY_EXHAUSTED,
    TAVRN_LINK_EVENT_RADIO_FAULT_TERMINAL,
    TAVRN_LINK_EVENT_SERVICE_FAULT_TERMINAL,
} tavrn_link_event_type_t;

typedef enum tavrn_local_tx_reason {
    TAVRN_LOCAL_TX_ENCODE_FAILED = 0,
    TAVRN_LOCAL_TX_SCHEDULER_FULL,
    TAVRN_LOCAL_TX_SCHEDULER_EVICTED,
    TAVRN_LOCAL_TX_RADIO_FAILED,
    TAVRN_LOCAL_TX_DEADLINE_EXPIRED,
} tavrn_local_tx_reason_t;

typedef enum tavrn_mesh_fault_reason {
    TAVRN_MESH_FAULT_NONE = 0,
    TAVRN_MESH_FAULT_RADIO_AFTER_TX,
    TAVRN_MESH_FAULT_RADIO_UNAVAILABLE,
    TAVRN_MESH_FAULT_POLL_OVERRUN,
    TAVRN_MESH_FAULT_INTERNAL_STATE,
} tavrn_mesh_fault_reason_t;

typedef struct tavrn_rx_data_candidate {
    tavrn_rx_candidate_token_t token;
    tavrn_direct_peer_t transmitter;
    uint8_t rssi_magnitude_db;
    tavrn_link_data_t data;
    uint32_t deadline_ms;
} tavrn_rx_data_candidate_t;

typedef struct tavrn_rx_control_event {
    tavrn_direct_peer_t transmitter;
    uint8_t rssi_magnitude_db;
    tavrn_validated_control_t control;
} tavrn_rx_control_event_t;

typedef struct tavrn_owned_data_event {
    tavrn_direct_peer_t next_hop;
    tavrn_link_data_t data;
    uint8_t requested_channel_mask;
    uint8_t completed_channel_mask;
    uint8_t attempt_count;
    uint8_t busy_response_count;
    uint32_t first_tx_ms;
    uint32_t last_tx_ms;
    uint32_t final_deadline_ms;
    tavrn_local_tx_reason_t local_reason;
    tavrn_mesh_fault_reason_t mesh_fault_reason;
} tavrn_owned_data_event_t;

typedef struct tavrn_transferred_data_event {
    tavrn_direct_peer_t next_hop;
    tavrn_link_data_t data;
    tavrn_hack_status_t status;
    uint8_t requested_channel_mask;
    uint8_t completed_channel_mask;
    uint8_t attempt_count;
    uint32_t first_tx_ms;
    uint32_t last_tx_ms;
} tavrn_transferred_data_event_t;

typedef struct tavrn_link_event {
    tavrn_link_event_type_t type;
    union {
        tavrn_rx_data_candidate_t candidate;
        tavrn_rx_control_event_t control;
        tavrn_transferred_data_event_t transferred_data;
        tavrn_owned_data_event_t owned_data;
    } detail;
} tavrn_link_event_t;

typedef struct tavrn_link_config {
    tavrn_direct_peer_t local_peer;
    uint8_t network_id;
    uint8_t hack_max_attempts;
    uint8_t busy_max_responses;
    uint32_t hack_response_ms;
    uint32_t hack_turnaround_ms;
    uint32_t retry_backoff_ms;
    uint32_t busy_backoff_ms;
    uint32_t data_forward_deadline_ms;
    uint32_t candidate_resolve_ms;
    uint32_t data_dedupe_ms;
    uint32_t flood_dedupe_ms;
    uint32_t flood_jitter_min_ms;
    uint32_t flood_jitter_max_ms;
} tavrn_link_config_t;

typedef struct tavrn_link_counters {
    uint32_t rx_candidate;
    uint32_t rx_candidate_accepted;
    uint32_t rx_candidate_busy;
    uint32_t rx_candidate_rejected;
    uint32_t rx_candidate_timeout_busy;
    uint32_t rx_additional_data_busy;
    uint32_t rx_committed_duplicate;
    uint32_t rx_control;
    uint32_t rx_flood_committed;
    uint32_t rx_malformed;
    uint32_t rx_wrong_next_hop;
    uint32_t rx_identity_conflict;
    uint32_t hack_accepted;
    uint32_t hack_duplicate;
    uint32_t hack_busy;
    uint32_t hack_rejected;
    uint32_t hack_unmatched;
    uint32_t hack_enqueue_failed;
    uint32_t tx_admitted;
    uint32_t tx_done;
    uint32_t tx_partial_done;
    uint32_t tx_failed;
    uint32_t retry_due;
    uint32_t custody_promoted;
    uint32_t custody_dispatch_blocked;
    uint32_t custody_transferred;
    uint32_t local_tx_not_attempted;
    uint32_t custody_busy_expired;
    uint32_t custody_rejected;
    uint32_t retry_exhausted;
    uint32_t radio_fault_terminal;
    uint32_t service_fault_terminal;
} tavrn_link_counters_t;

typedef enum tavrn_custody_phase {
    TAVRN_CUSTODY_FREE = 0,
    TAVRN_CUSTODY_READY_NOT_ELIGIBLE,
    TAVRN_CUSTODY_ACTIVE_QUEUED,
    TAVRN_CUSTODY_ACTIVE_WAIT_HACK,
    TAVRN_CUSTODY_BUSY_WAIT,
    TAVRN_CUSTODY_FAULT_PENDING,
} tavrn_custody_phase_t;

typedef struct tavrn_data_dedupe_entry {
    uint8_t valid;
    uint8_t custody_pinned;
    tavrn_logical_id_t origin;
    uint16_t data_seq;
    uint8_t app_kind;
    uint8_t app_source;
    uint32_t expires_at_ms;
} tavrn_data_dedupe_entry_t;

typedef struct tavrn_flood_dedupe_entry {
    uint8_t valid;
    uint8_t frame_type;
    tavrn_logical_id_t origin;
    uint16_t sequence;
    uint32_t expires_at_ms;
} tavrn_flood_dedupe_entry_t;

typedef struct tavrn_custody_slot {
    tavrn_custody_phase_t phase;
    tavrn_direct_peer_t next_hop;
    tavrn_link_data_t data;
    ble_mesh_tx_token_t scheduler_token;
    uint8_t attempt_count;
    uint8_t busy_response_count;
    uint8_t attempt_requested_channel_mask;
    uint8_t attempt_completed_channel_mask;
    uint8_t adv_len;
    uint8_t adv_data[BLE_ADV_MAX_DATA];
    uint32_t transaction_deadline_ms;
    uint32_t admission_order;
    uint32_t response_deadline_ms;
    uint32_t retry_not_before_ms;
    uint32_t first_tx_ms;
    uint32_t last_tx_ms;
} tavrn_custody_slot_t;

typedef struct tavrn_link_v2 {
    ble_mesh_scheduler_t *scheduler;
    tavrn_link_config_t config;
    tavrn_link_counters_t counters;
    tavrn_custody_slot_t custody[TAVRN_LINK_CUSTODY_CAPACITY];
    tavrn_data_dedupe_entry_t data_dedupe[TAVRN_LINK_DATA_DEDUPE_CAPACITY];
    tavrn_flood_dedupe_entry_t flood_dedupe[TAVRN_LINK_FLOOD_DEDUPE_CAPACITY];
    tavrn_rx_data_candidate_t candidate;
    uint16_t next_scheduler_token;
    uint16_t next_candidate_token;
    uint32_t next_custody_order;
    uint8_t active_custody_index;
    uint8_t candidate_valid;
    uint8_t mesh_fault_latched;
    tavrn_mesh_fault_reason_t latched_mesh_fault_reason;
} tavrn_link_v2_t;

tavrn_link_init_status_t tavrn_link_v2_init(
    tavrn_link_v2_t *link, ble_mesh_scheduler_t *sched,
    const tavrn_link_config_t *config, uint32_t now_ms);
tavrn_link_send_status_t tavrn_link_v2_send_unicast(
    tavrn_link_v2_t *link, const tavrn_direct_peer_t *next_hop,
    const tavrn_link_data_t *data, uint32_t now_ms,
    tavrn_link_event_t *local_outcome);
tavrn_link_send_status_t tavrn_link_v2_send_flood(
    tavrn_link_v2_t *link, const tavrn_codec_flood_t *flood,
    uint32_t now_ms, tavrn_link_event_t *local_outcome);
/* Sends one validated AODV control frame as best-effort scheduler work.  RREQ
 * and RERR are controlled floods and take no next hop; RREP and RREP_ACK are
 * direct and must name the PDU's immediate receiver.  This never allocates
 * DATA custody or a nonzero scheduler token. */
tavrn_link_send_status_t tavrn_link_v2_send_control(
    tavrn_link_v2_t *link, const tavrn_validated_control_t *control,
    const tavrn_direct_peer_t *next_hop_or_null, uint8_t controlled_flood,
    uint32_t now_ms, tavrn_link_event_t *local_outcome);
tavrn_link_resolve_status_t tavrn_link_v2_resolve_rx(
    tavrn_link_v2_t *link, tavrn_rx_candidate_token_t token,
    tavrn_rx_decision_t decision, uint32_t now_ms,
    tavrn_link_event_t *local_outcome);
tavrn_link_resolve_status_t tavrn_link_v2_release_rx_custody(
    tavrn_link_v2_t *link, const tavrn_link_data_t *data,
    uint32_t now_ms);
tavrn_link_step_status_t tavrn_link_v2_on_scheduler_event(
    tavrn_link_v2_t *link, const ble_mesh_sched_event_t *input,
    uint32_t now_ms, tavrn_link_event_t *output);
tavrn_link_step_status_t tavrn_link_v2_tick(
    tavrn_link_v2_t *link, uint32_t now_ms, tavrn_link_event_t *output);
tavrn_link_step_status_t tavrn_link_v2_dispatch(
    tavrn_link_v2_t *link, uint32_t now_ms,
    tavrn_link_event_t *local_outcome);
const tavrn_link_counters_t *tavrn_link_v2_counters(
    const tavrn_link_v2_t *link);

#endif /* TAVRN_LINK_V2_H */
