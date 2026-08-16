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

static void add_saturating(uint32_t *value, uint32_t addend)
{
    if (value == NULL || *value == UINT32_MAX) {
        return;
    }
    if (UINT32_MAX - *value < addend) {
        *value = UINT32_MAX;
    } else {
        *value += addend;
    }
}

static int time_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static int time_before(uint32_t left_ms, uint32_t right_ms)
{
    return (int32_t)(left_ms - right_ms) < 0;
}

static int next_record_id(routed_benchmark_state_t *state, uint64_t *out)
{
    if (state == NULL || out == NULL || state->record_id_exhausted != 0u) {
        return 0;
    }
    *out = state->next_record_id;
    if (state->next_record_id == UINT64_MAX) {
        state->record_id_exhausted = 1u;
    } else {
        state->next_record_id++;
    }
    return 1;
}

/* Move through completely elapsed bursts without synthesizing their missed
 * traffic.  The arithmetic selects the current burst when `now_ms` lands in
 * its active window and otherwise advances to the following one. */
static void discard_expired_bursts(routed_benchmark_state_t *state,
                                   uint32_t now_ms)
{
    uint32_t elapsed;
    uint32_t periods;
    uint32_t offset;
    uint32_t advance;
    uint32_t skipped;

    if (!time_due(now_ms, state->burst_started_at_ms +
                  ROUTED_BENCHMARK_BURST_DURATION_MS)) {
        return;
    }
    elapsed = now_ms - state->burst_started_at_ms;
    periods = elapsed / ROUTED_BENCHMARK_BURST_PERIOD_MS;
    offset = elapsed % ROUTED_BENCHMARK_BURST_PERIOD_MS;
    advance = periods;
    if (offset >= ROUTED_BENCHMARK_BURST_DURATION_MS) {
        advance++;
    }
    if (advance == 0u) {
        advance = 1u;
    }
    skipped = ROUTED_BENCHMARK_BURST_SLOT_COUNT - state->throughput_sequence;
    if (advance > 1u) {
        uint32_t additional = advance - 1u;

        if (additional > UINT32_MAX / ROUTED_BENCHMARK_BURST_SLOT_COUNT) {
            skipped = UINT32_MAX;
        } else {
            add_saturating(&skipped,
                           additional * ROUTED_BENCHMARK_BURST_SLOT_COUNT);
        }
    }
    add_saturating(&state->skipped, skipped);
    add_saturating(&state->throughput_skipped, skipped);
    state->burst_started_at_ms += advance * ROUTED_BENCHMARK_BURST_PERIOD_MS;
    state->throughput_burst += advance;
    state->throughput_sequence = 0u;
}

int routed_benchmark_init(routed_benchmark_state_t *state,
                          uint32_t initialization_time_ms,
                          uint32_t session_id)
{
    if (state == NULL || session_id == 0u) {
        return 0;
    }
    memset(state, 0, sizeof(*state));
    state->started_at_ms = initialization_time_ms;
    state->heartbeat_next_deadline_ms = initialization_time_ms;
    state->burst_started_at_ms = initialization_time_ms +
        ROUTED_BENCHMARK_BURST_START_MS;
    state->session_id = session_id;
    state->next_record_id = 1u;
    state->initialized = 1u;
    return 1;
}

routed_benchmark_schedule_status_t routed_benchmark_schedule_due(
    routed_benchmark_state_t *state, uint32_t now_ms,
    routed_benchmark_slot_t *slot_out)
{
    uint32_t heartbeat_deadline = 0u;
    uint32_t heartbeat_skipped = 0u;
    uint32_t throughput_deadline = 0u;
    uint32_t throughput_skipped = 0u;
    uint16_t throughput_sequence = 0u;
    uint8_t heartbeat_due = 0u;
    uint8_t throughput_due = 0u;
    uint8_t choose_heartbeat;
    uint32_t sequence;

    if (slot_out != NULL) {
        memset(slot_out, 0, sizeof(*slot_out));
    }
    if (state == NULL || slot_out == NULL || state->initialized == 0u ||
        state->session_id == 0u) {
        return ROUTED_BENCHMARK_SCHEDULE_INVALID;
    }
    discard_expired_bursts(state, now_ms);
    if (time_due(now_ms, state->heartbeat_next_deadline_ms)) {
        heartbeat_skipped = (now_ms - state->heartbeat_next_deadline_ms) /
            ROUTED_BENCHMARK_HEARTBEAT_INTERVAL_MS;
        heartbeat_deadline = state->heartbeat_next_deadline_ms +
            heartbeat_skipped * ROUTED_BENCHMARK_HEARTBEAT_INTERVAL_MS;
        heartbeat_due = 1u;
    }
    if (time_due(now_ms, state->burst_started_at_ms) &&
        !time_due(now_ms, state->burst_started_at_ms +
                  ROUTED_BENCHMARK_BURST_DURATION_MS)) {
        uint32_t due_sequence = (now_ms - state->burst_started_at_ms) /
            ROUTED_BENCHMARK_BURST_INTERVAL_MS;

        if (due_sequence >= state->throughput_sequence &&
            due_sequence < ROUTED_BENCHMARK_BURST_SLOT_COUNT) {
            throughput_skipped = due_sequence - state->throughput_sequence;
            throughput_sequence = (uint16_t)due_sequence;
            throughput_deadline = state->burst_started_at_ms +
                due_sequence * ROUTED_BENCHMARK_BURST_INTERVAL_MS;
            throughput_due = 1u;
        }
    }
    if (heartbeat_due == 0u && throughput_due == 0u) {
        return ROUTED_BENCHMARK_SCHEDULE_NONE;
    }
    /* When both workloads are overdue, returning an older one first would
     * replay stale traffic on the next cycle.  Drop the strictly older latest
     * slot (including its identity) and retain equal deadlines for the next
     * cycle. */
    if (heartbeat_due != 0u && throughput_due != 0u) {
        if (time_before(heartbeat_deadline, throughput_deadline)) {
            sequence = state->heartbeat_sequence + heartbeat_skipped;
            add_saturating(&state->skipped, heartbeat_skipped);
            add_saturating(&state->heartbeat_skipped, heartbeat_skipped);
            increment_saturating(&state->skipped);
            increment_saturating(&state->heartbeat_skipped);
            state->heartbeat_sequence = sequence + 1u;
            state->heartbeat_next_deadline_ms = heartbeat_deadline +
                ROUTED_BENCHMARK_HEARTBEAT_INTERVAL_MS;
            heartbeat_due = 0u;
        } else if (time_before(throughput_deadline, heartbeat_deadline)) {
            add_saturating(&state->skipped, throughput_skipped);
            add_saturating(&state->throughput_skipped, throughput_skipped);
            increment_saturating(&state->skipped);
            increment_saturating(&state->throughput_skipped);
            state->throughput_sequence = (uint16_t)(throughput_sequence + 1u);
            throughput_due = 0u;
        }
    }
    choose_heartbeat = throughput_due == 0u ||
        (heartbeat_due != 0u &&
         time_before(throughput_deadline, heartbeat_deadline));
    if (choose_heartbeat != 0u) {
        sequence = state->heartbeat_sequence + heartbeat_skipped;
        add_saturating(&state->skipped, heartbeat_skipped);
        add_saturating(&state->heartbeat_skipped, heartbeat_skipped);
        state->heartbeat_sequence = sequence + 1u;
        state->heartbeat_next_deadline_ms = heartbeat_deadline +
            ROUTED_BENCHMARK_HEARTBEAT_INTERVAL_MS;
        slot_out->deadline_ms = heartbeat_deadline;
        slot_out->workload = ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT;
        slot_out->burst = (sequence >> ROUTED_BENCHMARK_IDENTITY_SEQUENCE_BITS) &
            ROUTED_BENCHMARK_IDENTITY_BURST_MASK;
        slot_out->sequence = (uint16_t)(sequence &
            ROUTED_BENCHMARK_IDENTITY_SEQUENCE_MASK);
    } else {
        add_saturating(&state->skipped, throughput_skipped);
        add_saturating(&state->throughput_skipped, throughput_skipped);
        state->throughput_sequence = (uint16_t)(throughput_sequence + 1u);
        slot_out->deadline_ms = throughput_deadline;
        slot_out->workload = ROUTED_BENCHMARK_WORKLOAD_THROUGHPUT;
        slot_out->burst = state->throughput_burst &
            ROUTED_BENCHMARK_IDENTITY_BURST_MASK;
        slot_out->sequence = throughput_sequence;
    }
    slot_out->identity = routed_benchmark_identity_encode(slot_out->workload,
                                                            slot_out->burst,
                                                            slot_out->sequence);
    slot_out->session_id = state->session_id;
    increment_saturating(&state->offered);
    return ROUTED_BENCHMARK_SCHEDULE_DUE;
}

void routed_benchmark_record_submission(routed_benchmark_state_t *state,
                                        uint8_t accepted)
{
    if (state == NULL || state->initialized == 0u) {
        return;
    }
    if (accepted != 0u) {
        increment_saturating(&state->accepted);
    } else {
        increment_saturating(&state->rejected);
    }
}

int routed_benchmark_next_record_id(routed_benchmark_state_t *state,
                                    uint64_t *record_id_out)
{
    if (state == NULL || state->initialized == 0u || record_id_out == NULL) {
        return 0;
    }
    return next_record_id(state, record_id_out);
}

uint32_t routed_benchmark_identity_encode(routed_benchmark_workload_t workload,
                                          uint32_t burst, uint16_t sequence)
{
    return (((uint32_t)workload & 1u) <<
            ROUTED_BENCHMARK_IDENTITY_WORKLOAD_BIT) |
        ((burst & ROUTED_BENCHMARK_IDENTITY_BURST_MASK) <<
         ROUTED_BENCHMARK_IDENTITY_SEQUENCE_BITS) |
        ((uint32_t)sequence & ROUTED_BENCHMARK_IDENTITY_SEQUENCE_MASK);
}

int routed_benchmark_identity_decode(uint32_t identity,
                                     routed_benchmark_workload_t *workload_out,
                                     uint32_t *burst_out,
                                     uint16_t *sequence_out)
{
    if (workload_out == NULL || burst_out == NULL || sequence_out == NULL) {
        return 0;
    }
    *workload_out = ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT;
    *burst_out = 0u;
    *sequence_out = 0u;
    if (identity > ROUTED_BENCHMARK_IDENTITY_MAX) {
        return 0;
    }
    *workload_out = (identity >> ROUTED_BENCHMARK_IDENTITY_WORKLOAD_BIT) != 0u ?
        ROUTED_BENCHMARK_WORKLOAD_THROUGHPUT :
        ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT;
    *burst_out = (identity >> ROUTED_BENCHMARK_IDENTITY_SEQUENCE_BITS) &
        ROUTED_BENCHMARK_IDENTITY_BURST_MASK;
    *sequence_out = (uint16_t)(identity & ROUTED_BENCHMARK_IDENTITY_SEQUENCE_MASK);
    return 1;
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
    queue->tail = (uint16_t)((queue->tail + 1u) %
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
    queue->head = (uint16_t)((queue->head + 1u) %
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

int routed_benchmark_decode_payload(uint8_t app_kind, uint8_t app_len,
                                    const uint8_t *app_bytes,
                                    uint32_t *origin_session_out,
                                    uint32_t *identity_out)
{
    if (origin_session_out != NULL) {
        *origin_session_out = 0u;
    }
    if (identity_out != NULL) {
        *identity_out = 0u;
    }
    if (app_kind != ROUTED_BENCHMARK_APP_KIND ||
        app_len != ROUTED_BENCHMARK_APP_PAYLOAD_BYTES || app_bytes == NULL ||
        origin_session_out == NULL || identity_out == NULL) {
        return 0;
    }
    *origin_session_out = (uint32_t)app_bytes[0] |
        ((uint32_t)app_bytes[1] << 8) |
        ((uint32_t)app_bytes[2] << 16) |
        ((uint32_t)app_bytes[3] << 24);
    *identity_out = (uint32_t)app_bytes[4] |
        ((uint32_t)app_bytes[5] << 8) |
        ((uint32_t)app_bytes[6] << 16);
    if (*origin_session_out == 0u) {
        *identity_out = 0u;
        return 0;
    }
    return 1;
}
