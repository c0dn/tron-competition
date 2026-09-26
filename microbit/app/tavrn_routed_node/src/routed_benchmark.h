#ifndef ROUTED_BENCHMARK_H
#define ROUTED_BENCHMARK_H

#include <stdint.h>

/* This module contains only copied application-benchmark state.  It has no
 * router, radio, scheduler, RTOS, or FULL_TAVRN dependency so AODV_ONLY keeps
 * the same source boundary. */

#define ROUTED_BENCHMARK_HALF_RANGE 0x80000000u
#define ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY 1024u
#define ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY 1024u
#define ROUTED_BENCHMARK_APP_KIND 0x7fu
#define ROUTED_BENCHMARK_APP_PAYLOAD_BYTES 7u
#define ROUTED_BENCHMARK_HEARTBEAT_INTERVAL_MS 1000u
#define ROUTED_BENCHMARK_BURST_START_MS 60000u
#define ROUTED_BENCHMARK_BURST_PERIOD_MS 450000u
#define ROUTED_BENCHMARK_BURST_INTERVAL_MS 100u
#define ROUTED_BENCHMARK_BURST_DURATION_MS 60000u
#define ROUTED_BENCHMARK_BURST_SLOT_COUNT 600u
#define ROUTED_BENCHMARK_IDENTITY_WORKLOAD_BIT 23u
#define ROUTED_BENCHMARK_IDENTITY_SEQUENCE_BITS 10u
#define ROUTED_BENCHMARK_IDENTITY_SEQUENCE_MASK 0x03ffu
#define ROUTED_BENCHMARK_IDENTITY_BURST_MASK 0x00001fffu
#define ROUTED_BENCHMARK_HEARTBEAT_IDENTITY_MAX 0x007fffffu
#define ROUTED_BENCHMARK_IDENTITY_MAX 0x00ffffffu

typedef char routed_benchmark_accepted_fifo_capacity_guard[
    (ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY == 1024u) ? 1 : -1];
typedef char routed_benchmark_final_fifo_capacity_guard[
    (ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY == 1024u) ? 1 : -1];

typedef enum routed_benchmark_workload {
    ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT = 0,
    ROUTED_BENCHMARK_WORKLOAD_THROUGHPUT = 1,
} routed_benchmark_workload_t;

typedef enum routed_benchmark_schedule_status {
    ROUTED_BENCHMARK_SCHEDULE_NONE = 0,
    ROUTED_BENCHMARK_SCHEDULE_DUE,
    ROUTED_BENCHMARK_SCHEDULE_INVALID,
} routed_benchmark_schedule_status_t;

typedef enum routed_benchmark_accepted_fifo_status {
    ROUTED_BENCHMARK_ACCEPTED_FIFO_OK = 0,
    ROUTED_BENCHMARK_ACCEPTED_FIFO_EMPTY,
    ROUTED_BENCHMARK_ACCEPTED_FIFO_DROPPED,
    ROUTED_BENCHMARK_ACCEPTED_FIFO_INVALID,
} routed_benchmark_accepted_fifo_status_t;

typedef enum routed_benchmark_final_fifo_status {
    ROUTED_BENCHMARK_FINAL_FIFO_OK = 0,
    ROUTED_BENCHMARK_FINAL_FIFO_EMPTY,
    ROUTED_BENCHMARK_FINAL_FIFO_DROPPED,
    ROUTED_BENCHMARK_FINAL_FIFO_INVALID,
} routed_benchmark_final_fifo_status_t;

typedef struct routed_benchmark_state {
    uint32_t started_at_ms;
    uint32_t heartbeat_next_deadline_ms;
    uint32_t burst_started_at_ms;
    uint32_t heartbeat_sequence;
    uint32_t throughput_burst;
    uint16_t throughput_sequence;
    uint32_t session_id;
    uint64_t next_record_id;
    uint32_t offered;
    uint32_t accepted;
    uint32_t rejected;
    uint32_t skipped;
    uint32_t not_ready;
    uint32_t heartbeat_offered;
    uint32_t heartbeat_accepted;
    uint32_t heartbeat_rejected;
    uint32_t heartbeat_not_ready;
    uint32_t throughput_offered;
    uint32_t throughput_accepted;
    uint32_t throughput_rejected;
    uint32_t throughput_not_ready;
    uint32_t heartbeat_skipped;
    uint32_t throughput_skipped;
    uint8_t initialized;
    uint8_t record_id_exhausted;
} routed_benchmark_state_t;

/* A slot is offered at its absolute deadline.  It owns an identity before
 * destination readiness or router submission is evaluated, so a rejected or
 * not-ready offer never reuses payload identity.  Logger record IDs are
 * allocated only at emission time, preserving serial order in the output. */
typedef struct routed_benchmark_slot {
    uint32_t deadline_ms;
    uint32_t identity;
    uint32_t burst;
    uint32_t session_id;
    uint16_t sequence;
    routed_benchmark_workload_t workload;
} routed_benchmark_slot_t;

/* Accepted source evidence is intentionally only one natural-alignment,
 * four-word record. */
typedef struct routed_benchmark_accepted_event {
    uint32_t offered_at_ms;
    uint32_t accepted_at_ms;
    uint32_t identity;
    uint32_t submit_status;
} routed_benchmark_accepted_event_t;

/* `identity` retains the decoded raw compact identity even for an invalid
 * payload.  The flag says whether origin_session and identity are valid
 * benchmark-payload fields.  Natural field ordering keeps this at 20 bytes on
 * the repository ABI without packed access. */
typedef struct routed_benchmark_final_event {
    uint32_t delivered_at_ms;
    uint32_t origin_session;
    uint32_t identity;
    uint16_t logical_origin;
    uint16_t logical_destination;
    uint8_t app_kind;
    uint8_t app_len;
    uint8_t identity_valid;
} routed_benchmark_final_event_t;

typedef struct routed_benchmark_final_accounting {
    uint32_t final_commits;
    uint32_t valid_heartbeat_finals;
    uint32_t valid_throughput_finals;
    uint32_t invalid_identity_finals;
} routed_benchmark_final_accounting_t;

typedef struct routed_benchmark_accepted_fifo {
    routed_benchmark_accepted_event_t
        records[ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY];
    uint32_t dropped_count;
    uint16_t high_water;
    uint16_t head;
    uint16_t tail;
    uint16_t count;
} routed_benchmark_accepted_fifo_t;

typedef struct routed_benchmark_accepted_fifo_snapshot {
    uint32_t dropped_count;
    uint16_t high_water;
    uint16_t count;
} routed_benchmark_accepted_fifo_snapshot_t;

typedef struct routed_benchmark_final_fifo {
    routed_benchmark_final_event_t records[ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY];
    uint32_t dropped_count;
    uint16_t high_water;
    uint16_t head;
    uint16_t tail;
    uint16_t count;
} routed_benchmark_final_fifo_t;

typedef struct routed_benchmark_final_fifo_snapshot {
    uint32_t dropped_count;
    uint16_t high_water;
    uint16_t count;
} routed_benchmark_final_fifo_snapshot_t;

typedef char routed_benchmark_accepted_fifo_index_type_guard[
    (sizeof(((routed_benchmark_accepted_fifo_t *)0)->head) == sizeof(uint16_t) &&
     sizeof(((routed_benchmark_accepted_fifo_t *)0)->tail) == sizeof(uint16_t) &&
     sizeof(((routed_benchmark_accepted_fifo_t *)0)->count) == sizeof(uint16_t)) ?
        1 : -1];
typedef char routed_benchmark_final_fifo_index_type_guard[
    (sizeof(((routed_benchmark_final_fifo_t *)0)->head) == sizeof(uint16_t) &&
     sizeof(((routed_benchmark_final_fifo_t *)0)->tail) == sizeof(uint16_t) &&
     sizeof(((routed_benchmark_final_fifo_t *)0)->count) == sizeof(uint16_t)) ?
        1 : -1];
typedef char routed_benchmark_accepted_fifo_index_range_guard[
    (ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY <= UINT16_MAX) ? 1 : -1];
typedef char routed_benchmark_final_fifo_index_range_guard[
    (ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY <= UINT16_MAX) ? 1 : -1];
typedef char routed_benchmark_accepted_event_size_guard[
    (sizeof(routed_benchmark_accepted_event_t) == 16u) ? 1 : -1];
typedef char routed_benchmark_final_event_size_guard[
    (sizeof(routed_benchmark_final_event_t) <= 20u) ? 1 : -1];

/* The workload is fixed: role A offers a heartbeat every second forever and a
 * 10 Hz, 60 second burst beginning at 60 s and every 450 s after that.  The
 * nonzero session ID is emitted with every copied record. */
int routed_benchmark_init(routed_benchmark_state_t *state,
                          uint32_t initialization_time_ms,
                          uint32_t session_id);

/* Advances only from absolute deadlines.  A late cycle drops stale deadlines
 * and returns at most one newest due offer; it never catches up by replaying
 * old slots. */
routed_benchmark_schedule_status_t routed_benchmark_schedule_due(
    routed_benchmark_state_t *state, uint32_t now_ms,
    routed_benchmark_slot_t *slot_out);

void routed_benchmark_record_submission(routed_benchmark_state_t *state,
                                         routed_benchmark_workload_t workload,
                                         uint8_t accepted);
void routed_benchmark_record_not_ready(routed_benchmark_state_t *state,
                                        routed_benchmark_workload_t workload);

/* Allocates the next monotonically increasing record ID at copied logger
 * emission time.  The state fails closed rather than wrapping a record ID. */
int routed_benchmark_next_record_id(routed_benchmark_state_t *state,
                                    uint64_t *record_id_out);

uint32_t routed_benchmark_identity_encode(routed_benchmark_workload_t workload,
                                          uint32_t burst, uint16_t sequence);
int routed_benchmark_identity_decode(uint32_t identity,
                                      routed_benchmark_workload_t *workload_out,
                                      uint32_t *burst_out,
                                      uint16_t *sequence_out);

void routed_benchmark_final_accounting_init(
    routed_benchmark_final_accounting_t *accounting);
/* Call for every final delivery commit, regardless of final FIFO saturation. */
void routed_benchmark_record_final_commit(
    routed_benchmark_final_accounting_t *accounting, uint32_t identity,
    uint8_t identity_valid);

void routed_benchmark_accepted_fifo_init(routed_benchmark_accepted_fifo_t *fifo);
routed_benchmark_accepted_fifo_status_t routed_benchmark_accepted_fifo_offer(
    routed_benchmark_accepted_fifo_t *fifo,
    const routed_benchmark_accepted_event_t *event);
routed_benchmark_accepted_fifo_status_t routed_benchmark_accepted_fifo_take(
    routed_benchmark_accepted_fifo_t *fifo,
    routed_benchmark_accepted_event_t *event_out);
routed_benchmark_accepted_fifo_status_t routed_benchmark_accepted_fifo_snapshot(
    const routed_benchmark_accepted_fifo_t *fifo,
    routed_benchmark_accepted_fifo_snapshot_t *snapshot_out);

void routed_benchmark_final_fifo_init(routed_benchmark_final_fifo_t *fifo);
routed_benchmark_final_fifo_status_t routed_benchmark_final_fifo_offer(
    routed_benchmark_final_fifo_t *fifo,
    const routed_benchmark_final_event_t *event);
routed_benchmark_final_fifo_status_t routed_benchmark_final_fifo_take(
    routed_benchmark_final_fifo_t *fifo,
    routed_benchmark_final_event_t *event_out);
routed_benchmark_final_fifo_status_t routed_benchmark_final_fifo_snapshot(
    const routed_benchmark_final_fifo_t *fifo,
    routed_benchmark_final_fifo_snapshot_t *snapshot_out);

/* Decode the benchmark payload: a nonzero origin session followed by its
 * compact workload/burst/sequence identity, encoded little-endian in three
 * bytes.  The compact identity uses bit 23 for workload, bits 22..10 for
 * burst, and bits 9..0 for sequence.
 * Invalid application payloads return zero and write deterministic zeros. */
int routed_benchmark_decode_payload(uint8_t app_kind, uint8_t app_len,
                                    const uint8_t *app_bytes,
                                    uint32_t *origin_session_out,
                                    uint32_t *identity_out);

#endif /* ROUTED_BENCHMARK_H */
