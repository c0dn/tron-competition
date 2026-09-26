#ifndef ROUTED_EXPIRY_TELEMETRY_H
#define ROUTED_EXPIRY_TELEMETRY_H

#include <stddef.h>
#include <stdint.h>

/* The expiry logger owns one copied record.  A full queue retains that oldest
 * record and accounts for each newer observability record it cannot retain. */
#define ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY 1u

typedef enum routed_expiry_sweep_enqueue_status {
    ROUTED_EXPIRY_SWEEP_ENQUEUE_QUEUED = 0u,
    ROUTED_EXPIRY_SWEEP_ENQUEUE_DROPPED_FULL,
    ROUTED_EXPIRY_SWEEP_ENQUEUE_INVALID,
} routed_expiry_sweep_enqueue_status_t;

typedef struct routed_expiry_telemetry_queue_state {
    uint32_t dropped_records;
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} routed_expiry_telemetry_queue_state_t;

typedef char routed_expiry_sweep_telemetry_capacity_guard[
    (ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY == 1u) ? 1 : -1];

static inline int routed_expiry_telemetry_queue_is_valid(
    const routed_expiry_telemetry_queue_state_t *state)
{
    uint8_t expected_tail;

    if (state == NULL ||
        state->head >= ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY ||
        state->tail >= ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY ||
        state->count > ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY) {
        return 0;
    }
    expected_tail = (uint8_t)((state->head + state->count) %
                              ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY);
    return state->tail == expected_tail;
}

static inline routed_expiry_sweep_enqueue_status_t
routed_expiry_telemetry_queue_offer(routed_expiry_telemetry_queue_state_t *state)
{
    if (!routed_expiry_telemetry_queue_is_valid(state)) {
        return ROUTED_EXPIRY_SWEEP_ENQUEUE_INVALID;
    }
    if (state->count == ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY) {
        if (state->dropped_records != UINT32_MAX) {
            state->dropped_records++;
        }
        return ROUTED_EXPIRY_SWEEP_ENQUEUE_DROPPED_FULL;
    }
    return ROUTED_EXPIRY_SWEEP_ENQUEUE_QUEUED;
}

static inline int routed_expiry_telemetry_queue_commit_enqueue(
    routed_expiry_telemetry_queue_state_t *state)
{
    if (!routed_expiry_telemetry_queue_is_valid(state) ||
        state->count >= ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY) {
        return 0;
    }
    state->tail = (uint8_t)((state->tail + 1u) %
                            ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY);
    state->count++;
    return 1;
}

static inline int routed_expiry_telemetry_queue_commit_dequeue(
    routed_expiry_telemetry_queue_state_t *state)
{
    if (!routed_expiry_telemetry_queue_is_valid(state) || state->count == 0u) {
        return 0;
    }
    state->head = (uint8_t)((state->head + 1u) %
                            ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY);
    state->count--;
    return 1;
}

static inline uint8_t routed_expiry_sweep_enqueue_needs_scheduler_return(
    routed_expiry_sweep_enqueue_status_t status)
{
    return status == ROUTED_EXPIRY_SWEEP_ENQUEUE_QUEUED ||
        status == ROUTED_EXPIRY_SWEEP_ENQUEUE_DROPPED_FULL;
}

#endif /* ROUTED_EXPIRY_TELEMETRY_H */
