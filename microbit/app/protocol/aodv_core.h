#ifndef AODV_CORE_H
#define AODV_CORE_H

#include <stdint.h>

#include "tavrn_wire_v2.h"

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
} aodv_control_action_t;

typedef struct aodv_data_action {
    tavrn_direct_peer_t next_hop;
    tavrn_link_data_t data;
} aodv_data_action_t;

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
aodv_status_t aodv_core_ingest_control(aodv_core_t *core,
                                        const aodv_control_input_t *input,
                                        uint32_t now_ms);
aodv_status_t aodv_core_ingest_data(aodv_core_t *core,
                                     const aodv_data_input_t *input,
                                     uint32_t now_ms);
aodv_status_t aodv_core_mark_action_sent(aodv_core_t *core,
                                          uint16_t action_token,
                                          uint32_t now_ms);
aodv_status_t aodv_core_tick(aodv_core_t *core, uint32_t now_ms);
aodv_action_poll_status_t aodv_core_poll_action(aodv_core_t *core,
                                                 aodv_action_t *action_out);
aodv_route_query_status_t aodv_core_route_snapshot(
    const aodv_core_t *core, const tavrn_logical_id_t *destination,
    aodv_route_snapshot_t *snapshot_out);
const aodv_counters_t *aodv_core_counters(const aodv_core_t *core);

aodv_failure_status_t aodv_core_report_link_failure(
    aodv_core_t *core, const tavrn_direct_peer_t *failed_next_hop,
    const tavrn_logical_id_t *repair_destination,
    aodv_link_failure_mode_t mode, uint32_t now_ms);
aodv_failure_status_t aodv_core_finish_deferred_rerr(
    aodv_core_t *core, const tavrn_logical_id_t *repair_destination,
    aodv_deferred_rerr_decision_t decision, uint32_t now_ms);

#endif /* AODV_CORE_H */
