#include "routed_benchmark.h"

#include <string.h>

static int workload_is_valid(routed_benchmark_workload_t workload)
{
    return workload == ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT ||
        workload == ROUTED_BENCHMARK_WORKLOAD_THROUGHPUT;
}

static int state_is_initialized(const routed_benchmark_state_t *state)
{
    return state != NULL && state->initialized == 1u && state->session_id != 0u;
}

static int counter_has_pending_offer(uint32_t offered, uint32_t accepted,
                                     uint32_t rejected)
{
    return accepted <= offered && rejected <= offered - accepted &&
        accepted + rejected < offered;
}

static int accepted_fifo_is_valid(const routed_benchmark_accepted_fifo_t *fifo)
{
    uint16_t expected_tail;

    if (fifo == NULL || fifo->head >= ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY ||
        fifo->tail >= ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY ||
        fifo->count > ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY ||
        fifo->high_water < fifo->count ||
        fifo->high_water > ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY) {
        return 0;
    }
    expected_tail = (uint16_t)(fifo->head + fifo->count);
    if (expected_tail >= ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY) {
        expected_tail = (uint16_t)(expected_tail -
                                   ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY);
    }
    return fifo->tail == expected_tail;
}

static int final_fifo_is_valid(const routed_benchmark_final_fifo_t *fifo)
{
    uint16_t expected_tail;

    if (fifo == NULL || fifo->head >= ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY ||
        fifo->tail >= ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY ||
        fifo->count > ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY ||
        fifo->high_water < fifo->count ||
        fifo->high_water > ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY) {
        return 0;
    }
    expected_tail = (uint16_t)(fifo->head + fifo->count);
    if (expected_tail >= ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY) {
        expected_tail = (uint16_t)(expected_tail -
                                   ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY);
    }
    return fifo->tail == expected_tail;
}

static int accepted_event_is_valid(const routed_benchmark_accepted_event_t *event)
{
    return event != NULL && event->identity <= ROUTED_BENCHMARK_IDENTITY_MAX &&
        (event->submit_status == 0u || event->submit_status == 1u);
}

static int final_event_is_valid(const routed_benchmark_final_event_t *event)
{
    uint8_t expected_identity_valid;

    if (event == NULL || event->identity_valid > 1u) {
        return 0;
    }
    expected_identity_valid = event->origin_session != 0u &&
        event->identity <= ROUTED_BENCHMARK_IDENTITY_MAX &&
        event->app_kind == ROUTED_BENCHMARK_APP_KIND &&
        event->app_len == ROUTED_BENCHMARK_APP_PAYLOAD_BYTES;
    return event->identity_valid == expected_identity_valid;
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
static int discard_expired_bursts(routed_benchmark_state_t *state,
                                  uint32_t now_ms)
{
    uint32_t elapsed;
    uint32_t periods;
    uint32_t offset;
    uint32_t advance;
    uint32_t skipped;

    if (!time_due(now_ms, state->burst_started_at_ms +
                  ROUTED_BENCHMARK_BURST_DURATION_MS)) {
        return 1;
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
    if ((uint64_t)state->throughput_burst + advance >
        ROUTED_BENCHMARK_IDENTITY_BURST_MASK) {
        return 0;
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
    return 1;
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
    if (!state_is_initialized(state) || slot_out == NULL) {
        return ROUTED_BENCHMARK_SCHEDULE_INVALID;
    }
    if (state->throughput_burst > ROUTED_BENCHMARK_IDENTITY_BURST_MASK) {
        return ROUTED_BENCHMARK_SCHEDULE_INVALID;
    }
    if (time_due(now_ms, state->heartbeat_next_deadline_ms)) {
        heartbeat_skipped = (now_ms - state->heartbeat_next_deadline_ms) /
            ROUTED_BENCHMARK_HEARTBEAT_INTERVAL_MS;
        heartbeat_deadline = state->heartbeat_next_deadline_ms +
            heartbeat_skipped * ROUTED_BENCHMARK_HEARTBEAT_INTERVAL_MS;
        heartbeat_due = 1u;
    }
    if (heartbeat_due != 0u &&
        (uint64_t)state->heartbeat_sequence + heartbeat_skipped >
            ROUTED_BENCHMARK_HEARTBEAT_IDENTITY_MAX) {
        return ROUTED_BENCHMARK_SCHEDULE_INVALID;
    }
    if (!discard_expired_bursts(state, now_ms)) {
        return ROUTED_BENCHMARK_SCHEDULE_INVALID;
    }
    if (state->throughput_burst > ROUTED_BENCHMARK_IDENTITY_BURST_MASK) {
        return ROUTED_BENCHMARK_SCHEDULE_INVALID;
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
    if (slot_out->workload == ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT) {
        increment_saturating(&state->heartbeat_offered);
    } else {
        increment_saturating(&state->throughput_offered);
    }
    return ROUTED_BENCHMARK_SCHEDULE_DUE;
}

void routed_benchmark_record_submission(routed_benchmark_state_t *state,
                                         routed_benchmark_workload_t workload,
                                         uint8_t accepted)
{
    if (!state_is_initialized(state) || !workload_is_valid(workload) ||
        accepted > 1u ||
        !counter_has_pending_offer(state->offered, state->accepted,
                                   state->rejected)) {
        return;
    }
    if (workload == ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT) {
        if (!counter_has_pending_offer(state->heartbeat_offered,
                                       state->heartbeat_accepted,
                                       state->heartbeat_rejected)) {
            return;
        }
    } else if (!counter_has_pending_offer(state->throughput_offered,
                                           state->throughput_accepted,
                                           state->throughput_rejected)) {
        return;
    }
    if (accepted == 1u) {
        increment_saturating(&state->accepted);
        if (workload == ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT) {
            increment_saturating(&state->heartbeat_accepted);
        } else {
            increment_saturating(&state->throughput_accepted);
        }
    } else {
        increment_saturating(&state->rejected);
        if (workload == ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT) {
            increment_saturating(&state->heartbeat_rejected);
        } else {
            increment_saturating(&state->throughput_rejected);
        }
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

void routed_benchmark_record_not_ready(routed_benchmark_state_t *state,
                                       routed_benchmark_workload_t workload)
{
    if (!state_is_initialized(state) || !workload_is_valid(workload) ||
        !counter_has_pending_offer(state->offered, state->accepted,
                                   state->rejected)) {
        return;
    }
    if (workload == ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT) {
        if (!counter_has_pending_offer(state->heartbeat_offered,
                                       state->heartbeat_accepted,
                                       state->heartbeat_rejected)) {
            return;
        }
    } else if (!counter_has_pending_offer(state->throughput_offered,
                                           state->throughput_accepted,
                                           state->throughput_rejected)) {
        return;
    }
    increment_saturating(&state->rejected);
    increment_saturating(&state->not_ready);
    if (workload == ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT) {
        increment_saturating(&state->heartbeat_rejected);
        increment_saturating(&state->heartbeat_not_ready);
    } else {
        increment_saturating(&state->throughput_rejected);
        increment_saturating(&state->throughput_not_ready);
    }
}

void routed_benchmark_final_accounting_init(
    routed_benchmark_final_accounting_t *accounting)
{
    if (accounting != NULL) {
        memset(accounting, 0, sizeof(*accounting));
    }
}

void routed_benchmark_record_final_commit(
    routed_benchmark_final_accounting_t *accounting, uint32_t identity,
    uint8_t identity_valid)
{
    routed_benchmark_workload_t workload;
    uint32_t burst;
    uint16_t sequence;

    if (accounting == NULL) {
        return;
    }
    increment_saturating(&accounting->final_commits);
    if (identity_valid != 1u || !routed_benchmark_identity_decode(
            identity, &workload, &burst, &sequence)) {
        increment_saturating(&accounting->invalid_identity_finals);
        return;
    }
    if (workload == ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT) {
        increment_saturating(&accounting->valid_heartbeat_finals);
    } else {
        increment_saturating(&accounting->valid_throughput_finals);
    }
}

void routed_benchmark_accepted_fifo_init(routed_benchmark_accepted_fifo_t *fifo)
{
    if (fifo != NULL) {
        memset(fifo, 0, sizeof(*fifo));
    }
}

routed_benchmark_accepted_fifo_status_t routed_benchmark_accepted_fifo_offer(
    routed_benchmark_accepted_fifo_t *fifo,
    const routed_benchmark_accepted_event_t *event)
{
    if (!accepted_fifo_is_valid(fifo) || !accepted_event_is_valid(event)) {
        return ROUTED_BENCHMARK_ACCEPTED_FIFO_INVALID;
    }
    if (fifo->count == ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY) {
        increment_saturating(&fifo->dropped_count);
        return ROUTED_BENCHMARK_ACCEPTED_FIFO_DROPPED;
    }
    fifo->records[fifo->tail] = *event;
    fifo->tail++;
    if (fifo->tail == ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY) {
        fifo->tail = 0u;
    }
    fifo->count++;
    if (fifo->count > fifo->high_water) {
        fifo->high_water = fifo->count;
    }
    return ROUTED_BENCHMARK_ACCEPTED_FIFO_OK;
}

routed_benchmark_accepted_fifo_status_t routed_benchmark_accepted_fifo_take(
    routed_benchmark_accepted_fifo_t *fifo,
    routed_benchmark_accepted_event_t *event_out)
{
    if (!accepted_fifo_is_valid(fifo) || event_out == NULL) {
        return ROUTED_BENCHMARK_ACCEPTED_FIFO_INVALID;
    }
    if (fifo->count == 0u) {
        return ROUTED_BENCHMARK_ACCEPTED_FIFO_EMPTY;
    }
    *event_out = fifo->records[fifo->head];
    fifo->head++;
    if (fifo->head == ROUTED_BENCHMARK_ACCEPTED_FIFO_CAPACITY) {
        fifo->head = 0u;
    }
    fifo->count--;
    return ROUTED_BENCHMARK_ACCEPTED_FIFO_OK;
}

routed_benchmark_accepted_fifo_status_t routed_benchmark_accepted_fifo_snapshot(
    const routed_benchmark_accepted_fifo_t *fifo,
    routed_benchmark_accepted_fifo_snapshot_t *snapshot_out)
{
    if (!accepted_fifo_is_valid(fifo) || snapshot_out == NULL) {
        return ROUTED_BENCHMARK_ACCEPTED_FIFO_INVALID;
    }
    snapshot_out->count = fifo->count;
    snapshot_out->high_water = fifo->high_water;
    snapshot_out->dropped_count = fifo->dropped_count;
    return ROUTED_BENCHMARK_ACCEPTED_FIFO_OK;
}

void routed_benchmark_final_fifo_init(routed_benchmark_final_fifo_t *fifo)
{
    if (fifo != NULL) {
        memset(fifo, 0, sizeof(*fifo));
    }
}

routed_benchmark_final_fifo_status_t routed_benchmark_final_fifo_offer(
    routed_benchmark_final_fifo_t *fifo,
    const routed_benchmark_final_event_t *event)
{
    if (!final_fifo_is_valid(fifo) || !final_event_is_valid(event)) {
        return ROUTED_BENCHMARK_FINAL_FIFO_INVALID;
    }
    if (fifo->count == ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY) {
        increment_saturating(&fifo->dropped_count);
        return ROUTED_BENCHMARK_FINAL_FIFO_DROPPED;
    }
    fifo->records[fifo->tail] = *event;
    fifo->tail++;
    if (fifo->tail == ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY) {
        fifo->tail = 0u;
    }
    fifo->count++;
    if (fifo->count > fifo->high_water) {
        fifo->high_water = fifo->count;
    }
    return ROUTED_BENCHMARK_FINAL_FIFO_OK;
}

routed_benchmark_final_fifo_status_t routed_benchmark_final_fifo_take(
    routed_benchmark_final_fifo_t *fifo, routed_benchmark_final_event_t *event_out)
{
    if (!final_fifo_is_valid(fifo) || event_out == NULL) {
        return ROUTED_BENCHMARK_FINAL_FIFO_INVALID;
    }
    if (fifo->count == 0u) {
        return ROUTED_BENCHMARK_FINAL_FIFO_EMPTY;
    }
    *event_out = fifo->records[fifo->head];
    fifo->head++;
    if (fifo->head == ROUTED_BENCHMARK_FINAL_FIFO_CAPACITY) {
        fifo->head = 0u;
    }
    fifo->count--;
    return ROUTED_BENCHMARK_FINAL_FIFO_OK;
}

routed_benchmark_final_fifo_status_t routed_benchmark_final_fifo_snapshot(
    const routed_benchmark_final_fifo_t *fifo,
    routed_benchmark_final_fifo_snapshot_t *snapshot_out)
{
    if (!final_fifo_is_valid(fifo) || snapshot_out == NULL) {
        return ROUTED_BENCHMARK_FINAL_FIFO_INVALID;
    }
    snapshot_out->count = fifo->count;
    snapshot_out->high_water = fifo->high_water;
    snapshot_out->dropped_count = fifo->dropped_count;
    return ROUTED_BENCHMARK_FINAL_FIFO_OK;
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
