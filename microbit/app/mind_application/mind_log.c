#include "mind_log.h"

#include <string.h>

void mind_log_queue_init(mind_log_queue_t *queue)
{
    if (queue != NULL) {
        memset(queue, 0, sizeof(*queue));
    }
}

int mind_log_queue_reserve(mind_log_queue_t *queue, uint8_t count,
                           mind_log_reservation_t *reservation_out)
{
    uint8_t cursor;
    uint8_t remaining;

    if (queue == NULL || reservation_out == NULL || count == 0u ||
        count > MIND_LOG_CAPACITY || queue->head >= MIND_LOG_STORAGE_CAPACITY ||
        queue->tail >= MIND_LOG_STORAGE_CAPACITY) {
        return 0;
    }
    cursor = queue->tail;
    remaining = count;
    while (remaining != 0u) {
        cursor = (uint8_t)((cursor + 1u) % MIND_LOG_STORAGE_CAPACITY);
        if (cursor == queue->head) {
            return 0;
        }
        remaining--;
    }
    reservation_out->start = queue->tail;
    reservation_out->count = count;
    reservation_out->active = 1u;
    return 1;
}

int mind_log_queue_commit(mind_log_queue_t *queue,
                          mind_log_reservation_t *reservation,
                          const mind_log_record_t *records)
{
    uint8_t index;
    uint8_t cursor;

    if (queue == NULL || reservation == NULL || records == NULL ||
        reservation->active == 0u || reservation->count == 0u ||
        reservation->count > MIND_LOG_CAPACITY ||
        reservation->start != queue->tail) {
        return 0;
    }
    cursor = reservation->start;
    for (index = 0u; index < reservation->count; index++) {
        queue->records[cursor] = records[index];
        cursor = (uint8_t)((cursor + 1u) % MIND_LOG_STORAGE_CAPACITY);
    }
    queue->tail = cursor;
    reservation->active = 0u;
    return 1;
}

void mind_log_queue_cancel(mind_log_reservation_t *reservation)
{
    if (reservation != NULL) {
        memset(reservation, 0, sizeof(*reservation));
    }
}

int mind_log_queue_take(mind_log_queue_t *queue, mind_log_record_t *record_out)
{
    uint8_t head;

    if (queue == NULL || record_out == NULL) {
        return 0;
    }
    head = queue->head;
    if (head >= MIND_LOG_STORAGE_CAPACITY || queue->tail >= MIND_LOG_STORAGE_CAPACITY ||
        head == queue->tail) {
        return 0;
    }
    *record_out = queue->records[head];
    queue->head = (uint8_t)((head + 1u) % MIND_LOG_STORAGE_CAPACITY);
    return 1;
}

int mind_log_record_is_heartbeat(const mind_log_record_t *record)
{
    return record != NULL && record->kind == MIND_LOG_EVENT &&
        record->detail.event.report.schema_payload[0] == MIND_SCHEMA_VERSION &&
        record->detail.event.report.schema_payload[1] == MIND_EVT_HEARTBEAT;
}
