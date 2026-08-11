#ifndef ROUTED_BENCHMARK_H
#define ROUTED_BENCHMARK_H

#include <stdint.h>

/* This module contains only copied application-benchmark state.  It has no
 * router, radio, scheduler, RTOS, or FULL_TAVRN dependency so AODV_ONLY keeps
 * the same source boundary. */

#define ROUTED_BENCHMARK_HALF_RANGE 0x80000000u
#define ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY 512u
#define ROUTED_BENCHMARK_STATUS_NOT_READY 0xffffffffu
#define ROUTED_BENCHMARK_APP_KIND 0x7fu
#define ROUTED_BENCHMARK_APP_PAYLOAD_BYTES 8u
#define ROUTED_BENCHMARK_HEARTBEAT_INTERVAL_MS 1000u
#define ROUTED_BENCHMARK_BURST_START_MS 60000u
#define ROUTED_BENCHMARK_BURST_PERIOD_MS 450000u
#define ROUTED_BENCHMARK_BURST_INTERVAL_MS 100u
#define ROUTED_BENCHMARK_BURST_DURATION_MS 60000u
#define ROUTED_BENCHMARK_BURST_SLOT_COUNT 600u
#define ROUTED_BENCHMARK_IDENTITY_SEQUENCE_BITS 10u
#define ROUTED_BENCHMARK_IDENTITY_SEQUENCE_MASK 0x03ffu
#define ROUTED_BENCHMARK_IDENTITY_BURST_MASK 0x001fffffu

typedef char routed_benchmark_attempt_queue_capacity_guard[
    (ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY == 512u) ? 1 : -1];

typedef enum routed_benchmark_workload {
    ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT = 0,
    ROUTED_BENCHMARK_WORKLOAD_THROUGHPUT = 1,
} routed_benchmark_workload_t;

typedef enum routed_benchmark_record_kind {
    ROUTED_BENCHMARK_RECORD_OFFER = 0,
    ROUTED_BENCHMARK_RECORD_APPLICATION,
} routed_benchmark_record_kind_t;

typedef enum routed_benchmark_schedule_status {
    ROUTED_BENCHMARK_SCHEDULE_NONE = 0,
    ROUTED_BENCHMARK_SCHEDULE_DUE,
    ROUTED_BENCHMARK_SCHEDULE_INVALID,
} routed_benchmark_schedule_status_t;

typedef enum routed_benchmark_attempt_queue_status {
    ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK = 0,
    ROUTED_BENCHMARK_ATTEMPT_QUEUE_EMPTY,
    ROUTED_BENCHMARK_ATTEMPT_QUEUE_DROPPED,
    ROUTED_BENCHMARK_ATTEMPT_QUEUE_INVALID,
} routed_benchmark_attempt_queue_status_t;

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

typedef struct routed_benchmark_attempt {
    uint32_t event_at_ms;
    uint32_t deadline_ms;
    uint32_t identity;
    uint32_t burst;
    uint32_t session_id;
    uint32_t status;
    uint16_t destination;
    uint16_t sequence;
    uint8_t accepted;
    uint8_t attempted;
    uint8_t width;
    routed_benchmark_workload_t workload;
    routed_benchmark_record_kind_t kind;
} routed_benchmark_attempt_t;

typedef struct routed_benchmark_attempt_queue {
    routed_benchmark_attempt_t records[ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY];
    uint32_t dropped_count;
    uint16_t high_water;
    uint16_t head;
    uint16_t tail;
    uint16_t count;
} routed_benchmark_attempt_queue_t;

typedef struct routed_benchmark_attempt_queue_snapshot {
    uint32_t dropped_count;
    uint16_t high_water;
    uint16_t count;
} routed_benchmark_attempt_queue_snapshot_t;

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
                                         uint8_t accepted);
void routed_benchmark_record_not_ready(routed_benchmark_state_t *state);

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

void routed_benchmark_attempt_queue_init(routed_benchmark_attempt_queue_t *queue);
routed_benchmark_attempt_queue_status_t routed_benchmark_attempt_queue_offer(
    routed_benchmark_attempt_queue_t *queue,
    const routed_benchmark_attempt_t *attempt);
routed_benchmark_attempt_queue_status_t routed_benchmark_attempt_queue_take(
    routed_benchmark_attempt_queue_t *queue, routed_benchmark_attempt_t *attempt_out);
routed_benchmark_attempt_queue_status_t routed_benchmark_attempt_queue_snapshot(
    const routed_benchmark_attempt_queue_t *queue,
    routed_benchmark_attempt_queue_snapshot_t *snapshot_out);

/* Decode the benchmark payload: a nonzero origin session followed by its
 * workload/burst/sequence identity, both little-endian uint32 values.
 * Invalid application payloads return zero and write deterministic zeros. */
int routed_benchmark_decode_payload(uint8_t app_kind, uint8_t app_len,
                                    const uint8_t *app_bytes,
                                    uint32_t *origin_session_out,
                                    uint32_t *identity_out);

#endif /* ROUTED_BENCHMARK_H */
