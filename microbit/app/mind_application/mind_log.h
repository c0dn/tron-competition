#ifndef MIND_LOG_H
#define MIND_LOG_H

/* Logger-owned fixed records.  ISR, UART, and mesh code copy records here but
 * never format or transmit text directly. */

#include <stdint.h>

#include "mind_command.h"
#include "mind_root_plane.h"

/* Eight committed application records are retained.  The extra physical ring
 * sentinel preserves SPSC ownership without a shared count RMW. */
#define MIND_LOG_CAPACITY 8u
#define MIND_LOG_STORAGE_CAPACITY (MIND_LOG_CAPACITY + 1u)

typedef enum mind_log_kind {
    MIND_LOG_COMMAND = 0,
    MIND_LOG_ROOT,
    MIND_LOG_EVENT,
} mind_log_kind_t;

typedef struct mind_log_record {
    uint32_t now_ms;
    tavrn_adva_t local_adva;
    union {
        struct {
            mind_command_kind_t command;
            mind_command_status_t status;
        } command;
        struct {
            mind_root_plane_status_t state;
            uint32_t rootless_drop;
            uint8_t node_number;
            uint8_t local_active;
        } root;
        struct {
            mind_application_report_t report;
            tavrn_adva_t observer;
            uint8_t wearable;
            uint8_t path_local;
        } event;
    } detail;
    mind_log_kind_t kind;
} mind_log_record_t;

typedef struct mind_log_queue {
    mind_log_record_t records[MIND_LOG_STORAGE_CAPACITY];
    /* SPSC: mesh publishes tail, logger consumes head. */
    volatile uint8_t head;
    volatile uint8_t tail;
} mind_log_queue_t;

typedef struct mind_log_reservation {
    uint8_t start;
    uint8_t count;
    uint8_t active;
} mind_log_reservation_t;

void mind_log_queue_init(mind_log_queue_t *queue);
int mind_log_queue_reserve(mind_log_queue_t *queue, uint8_t count,
                           mind_log_reservation_t *reservation_out);
int mind_log_queue_commit(mind_log_queue_t *queue,
                          mind_log_reservation_t *reservation,
                          const mind_log_record_t *records);
void mind_log_queue_cancel(mind_log_reservation_t *reservation);
int mind_log_queue_take(mind_log_queue_t *queue, mind_log_record_t *record_out);
int mind_log_record_is_heartbeat(const mind_log_record_t *record);

#endif /* MIND_LOG_H */
