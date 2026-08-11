#include "routed_benchmark.h"

#include <string.h>

static int queue_is_valid(const routed_benchmark_attempt_queue_t *queue)
{
    return queue != NULL &&
        queue->head < ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY &&
        queue->tail < ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY &&
        queue->count <= ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY &&
        queue->high_water <= ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY;
}

static void increment_saturating(uint32_t *value)
{
    if (value != NULL && *value != UINT32_MAX) {
        (*value)++;
    }
}

int routed_benchmark_init(routed_benchmark_state_t *state,
                          uint32_t initialization_time_ms,
                          uint32_t warmup_ms, uint32_t interval_ms,
                          uint32_t target)
{
    if (state == NULL || warmup_ms >= ROUTED_BENCHMARK_HALF_RANGE ||
        interval_ms == 0u || interval_ms >= ROUTED_BENCHMARK_HALF_RANGE ||
        target == 0u) {
        return 0;
    }
    memset(state, 0, sizeof(*state));
    state->target = target;
    state->interval_ms = interval_ms;
    state->start_at_ms = initialization_time_ms + warmup_ms;
    state->next_deadline_ms = state->start_at_ms;
    state->initialized = 1u;
    return 1;
}

routed_benchmark_schedule_status_t routed_benchmark_schedule_due(
    routed_benchmark_state_t *state, uint32_t now_ms,
    routed_benchmark_slot_t *slot_out)
{
    uint32_t elapsed;
    uint32_t due_count;
    uint32_t remaining;
    uint32_t skipped;

    if (slot_out != NULL) {
        memset(slot_out, 0, sizeof(*slot_out));
    }
    if (state == NULL || slot_out == NULL || state->initialized == 0u ||
        state->target == 0u || state->interval_ms == 0u ||
        state->interval_ms >= ROUTED_BENCHMARK_HALF_RANGE ||
        state->offered > state->target) {
        return ROUTED_BENCHMARK_SCHEDULE_INVALID;
    }
    if (state->offered == state->target ||
        (int32_t)(now_ms - state->next_deadline_ms) < 0) {
        return ROUTED_BENCHMARK_SCHEDULE_NONE;
    }

    elapsed = now_ms - state->next_deadline_ms;
    due_count = elapsed / state->interval_ms + 1u;
    remaining = state->target - state->offered;
    if (due_count > remaining) {
        due_count = remaining;
    }
    /* One current slot may be submitted; every earlier elapsed deadline is a
     * denominator slot only.  due_count is nonzero after the due predicate. */
    skipped = due_count - 1u;
    state->offered += skipped;
    state->skipped += skipped;
    slot_out->deadline_ms = state->next_deadline_ms +
        (uint32_t)(skipped * state->interval_ms);
    slot_out->slot = state->offered;
    slot_out->counter = state->payload_counter;
    state->offered++;
    state->next_deadline_ms = slot_out->deadline_ms + state->interval_ms;
    return ROUTED_BENCHMARK_SCHEDULE_DUE;
}

void routed_benchmark_record_submission(routed_benchmark_state_t *state,
                                        uint8_t accepted)
{
    if (state == NULL || state->initialized == 0u) {
        return;
    }
    increment_saturating(&state->attempted);
    if (accepted != 0u) {
        increment_saturating(&state->accepted);
        state->payload_counter++;
    } else {
        increment_saturating(&state->rejected);
    }
}

void routed_benchmark_record_not_ready(routed_benchmark_state_t *state)
{
    if (state == NULL || state->initialized == 0u) {
        return;
    }
    increment_saturating(&state->rejected);
    increment_saturating(&state->not_ready);
}

void routed_benchmark_attempt_queue_init(routed_benchmark_attempt_queue_t *queue)
{
    if (queue != NULL) {
        memset(queue, 0, sizeof(*queue));
    }
}

routed_benchmark_attempt_queue_status_t routed_benchmark_attempt_queue_offer(
    routed_benchmark_attempt_queue_t *queue,
    const routed_benchmark_attempt_t *attempt)
{
    if (!queue_is_valid(queue) || attempt == NULL) {
        return ROUTED_BENCHMARK_ATTEMPT_QUEUE_INVALID;
    }
    if (queue->count == ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY) {
        increment_saturating(&queue->dropped_count);
        return ROUTED_BENCHMARK_ATTEMPT_QUEUE_DROPPED;
    }
    queue->records[queue->tail] = *attempt;
    queue->tail = (uint8_t)((queue->tail + 1u) %
                            ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY);
    queue->count++;
    if (queue->count > queue->high_water) {
        queue->high_water = queue->count;
    }
    return ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK;
}

routed_benchmark_attempt_queue_status_t routed_benchmark_attempt_queue_take(
    routed_benchmark_attempt_queue_t *queue, routed_benchmark_attempt_t *attempt_out)
{
    if (!queue_is_valid(queue) || attempt_out == NULL) {
        return ROUTED_BENCHMARK_ATTEMPT_QUEUE_INVALID;
    }
    if (queue->count == 0u) {
        return ROUTED_BENCHMARK_ATTEMPT_QUEUE_EMPTY;
    }
    *attempt_out = queue->records[queue->head];
    queue->head = (uint8_t)((queue->head + 1u) %
                            ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY);
    queue->count--;
    return ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK;
}

routed_benchmark_attempt_queue_status_t routed_benchmark_attempt_queue_snapshot(
    const routed_benchmark_attempt_queue_t *queue,
    routed_benchmark_attempt_queue_snapshot_t *snapshot_out)
{
    if (!queue_is_valid(queue) || snapshot_out == NULL) {
        return ROUTED_BENCHMARK_ATTEMPT_QUEUE_INVALID;
    }
    snapshot_out->count = queue->count;
    snapshot_out->high_water = queue->high_water;
    snapshot_out->dropped_count = queue->dropped_count;
    return ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK;
}

int routed_benchmark_decode_counter(uint8_t app_kind, uint8_t app_len,
                                    const uint8_t *app_bytes,
                                    uint32_t *counter_out)
{
    if (counter_out != NULL) {
        *counter_out = 0u;
    }
    if (app_kind != 0x7fu || app_len != 4u || app_bytes == NULL ||
        counter_out == NULL) {
        return 0;
    }
    *counter_out = (uint32_t)app_bytes[0] |
        ((uint32_t)app_bytes[1] << 8) |
        ((uint32_t)app_bytes[2] << 16) |
        ((uint32_t)app_bytes[3] << 24);
    return 1;
}
