#ifndef ROUTED_BENCHMARK_H
#define ROUTED_BENCHMARK_H

#include <stdint.h>

/* This module contains only copied application-benchmark state.  It has no
 * router, radio, scheduler, RTOS, or FULL_TAVRN dependency so AODV_ONLY keeps
 * the same source boundary. */

#define ROUTED_BENCHMARK_HALF_RANGE 0x80000000u
#define ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY 16u
#define ROUTED_BENCHMARK_STATUS_NOT_READY 0xffffffffu

typedef char routed_benchmark_attempt_queue_capacity_guard[
    (ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY == 16u) ? 1 : -1];

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
    uint32_t target;
    uint32_t interval_ms;
    uint32_t start_at_ms;
    uint32_t next_deadline_ms;
    uint32_t payload_counter;
    uint32_t offered;
    uint32_t attempted;
    uint32_t accepted;
    uint32_t rejected;
    uint32_t skipped;
    uint32_t not_ready;
    uint8_t initialized;
} routed_benchmark_state_t;

/* A slot is offered at its absolute deadline.  slot and counter are both
 * zero-based; counter advances only after an accepted submission. */
typedef struct routed_benchmark_slot {
    uint32_t deadline_ms;
    uint32_t slot;
    uint32_t counter;
} routed_benchmark_slot_t;

typedef struct routed_benchmark_attempt {
    uint32_t now_ms;
    uint32_t slot;
    uint32_t counter;
    uint32_t status;
    uint16_t destination;
    uint8_t accepted;
    uint8_t width;
} routed_benchmark_attempt_t;

typedef struct routed_benchmark_attempt_queue {
    routed_benchmark_attempt_t records[ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY];
    uint32_t dropped_count;
    uint8_t high_water;
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} routed_benchmark_attempt_queue_t;

typedef struct routed_benchmark_attempt_queue_snapshot {
    uint32_t dropped_count;
    uint8_t high_water;
    uint8_t count;
} routed_benchmark_attempt_queue_snapshot_t;

/* warmup, interval, and target must be nonzero and below the uint32 half
 * range. start_at is initialization_time + warmup in serial-number space. */
int routed_benchmark_init(routed_benchmark_state_t *state,
                          uint32_t initialization_time_ms,
                          uint32_t warmup_ms, uint32_t interval_ms,
                          uint32_t target);

/* Advances from the prior absolute deadline, not operation completion.  When
 * late, all older due slots are counted as skipped and only the newest due slot
 * is returned, guaranteeing one possible real submission per mesh cycle. */
routed_benchmark_schedule_status_t routed_benchmark_schedule_due(
    routed_benchmark_state_t *state, uint32_t now_ms,
    routed_benchmark_slot_t *slot_out);

void routed_benchmark_record_submission(routed_benchmark_state_t *state,
                                        uint8_t accepted);
void routed_benchmark_record_not_ready(routed_benchmark_state_t *state);

void routed_benchmark_attempt_queue_init(routed_benchmark_attempt_queue_t *queue);
routed_benchmark_attempt_queue_status_t routed_benchmark_attempt_queue_offer(
    routed_benchmark_attempt_queue_t *queue,
    const routed_benchmark_attempt_t *attempt);
routed_benchmark_attempt_queue_status_t routed_benchmark_attempt_queue_take(
    routed_benchmark_attempt_queue_t *queue, routed_benchmark_attempt_t *attempt_out);
routed_benchmark_attempt_queue_status_t routed_benchmark_attempt_queue_snapshot(
    const routed_benchmark_attempt_queue_t *queue,
    routed_benchmark_attempt_queue_snapshot_t *snapshot_out);

/* Decode the benchmark's existing 4-byte little-endian application counter.
 * Invalid application payloads return zero and write a deterministic zero. */
int routed_benchmark_decode_counter(uint8_t app_kind, uint8_t app_len,
                                    const uint8_t *app_bytes,
                                    uint32_t *counter_out);

#endif /* ROUTED_BENCHMARK_H */
