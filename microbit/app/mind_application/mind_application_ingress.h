#ifndef MIND_APPLICATION_INGRESS_H
#define MIND_APPLICATION_INGRESS_H

/*
 * Direct wearable ingress is a Layer-7-only RX filter.  It owns the short
 * local seen cache and the copied report FIFO; it has no TAVRN, root, or
 * forwarding policy.
 */

#include <stdint.h>

#include "ble_mesh_scheduler.h"
#include "mind_application_wire.h"

#define MIND_APPLICATION_INGRESS_SEEN_CAPACITY 16u
#define MIND_APPLICATION_INGRESS_QUEUE_CAPACITY 8u
#define MIND_APPLICATION_INGRESS_RETENTION_MS 2000u
#define MIND_APPLICATION_INGRESS_FRAME_BYTES 26u

typedef char mind_application_ingress_seen_capacity_guard[
    (MIND_APPLICATION_INGRESS_SEEN_CAPACITY == 16u) ? 1 : -1];
typedef char mind_application_ingress_queue_capacity_guard[
    (MIND_APPLICATION_INGRESS_QUEUE_CAPACITY == 8u) ? 1 : -1];
typedef char mind_application_ingress_retention_guard[
    (MIND_APPLICATION_INGRESS_RETENTION_MS == 2000u) ? 1 : -1];
typedef char mind_application_ingress_frame_bytes_guard[
    (MIND_APPLICATION_INGRESS_FRAME_BYTES == 26u) ? 1 : -1];

typedef struct mind_application_ingress_seen {
    uint32_t packet_id24;
    uint32_t committed_at_ms;
    uint16_t wearable_src16;
    uint8_t network_id;
    uint8_t occupied;
} mind_application_ingress_seen_t;

/* This is a copied MIND_REPORT plus RF observation evidence.  It intentionally
 * contains no root state, route, or TAVRN policy. */
typedef struct mind_application_ingress_event {
    mind_application_wire_record_t report;
    uint32_t observed_at_ms;
    uint32_t packet_id24;
    uint16_t wearable_src16;
    uint8_t network_id;
    uint8_t adv_addr[6];
    uint8_t channel;
    uint8_t rssi_magnitude_db;
} mind_application_ingress_event_t;

typedef enum mind_application_ingress_validation {
    MIND_APPLICATION_INGRESS_VALID = 0,
    MIND_APPLICATION_INGRESS_INVALID_ARGUMENT,
    MIND_APPLICATION_INGRESS_INVALID_FRAME_LENGTH,
    MIND_APPLICATION_INGRESS_INVALID_FLAGS,
    MIND_APPLICATION_INGRESS_INVALID_MANUFACTURER,
    MIND_APPLICATION_INGRESS_INVALID_COMPANY,
    MIND_APPLICATION_INGRESS_INVALID_MAGIC,
    MIND_APPLICATION_INGRESS_INVALID_VERSION,
    MIND_APPLICATION_INGRESS_INVALID_TYPE,
    MIND_APPLICATION_INGRESS_INVALID_NETWORK,
    MIND_APPLICATION_INGRESS_INVALID_TTL,
    MIND_APPLICATION_INGRESS_INVALID_SOURCE,
    MIND_APPLICATION_INGRESS_INVALID_ADVA,
    MIND_APPLICATION_INGRESS_INVALID_PAYLOAD_LENGTH,
    MIND_APPLICATION_INGRESS_INVALID_SCHEMA,
} mind_application_ingress_validation_t;

typedef enum mind_application_ingress_outcome {
    MIND_APPLICATION_INGRESS_PASSTHROUGH = 0,
    MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED,
    MIND_APPLICATION_INGRESS_CONSUMED_DUPLICATE,
    MIND_APPLICATION_INGRESS_CONSUMED_INVALID,
    MIND_APPLICATION_INGRESS_CONSUMED_CACHE_FULL,
    MIND_APPLICATION_INGRESS_CONSUMED_QUEUE_FULL,
    MIND_APPLICATION_INGRESS_INVALID_STATE,
} mind_application_ingress_outcome_t;

typedef struct mind_application_ingress_result {
    mind_application_ingress_outcome_t outcome;
    mind_application_ingress_validation_t validation;
} mind_application_ingress_result_t;

typedef struct mind_application_ingress_counters {
    uint32_t passed_non_mind;
    uint32_t accepted;
    uint32_t duplicate;
    uint32_t invalid;
    uint32_t cache_full;
    uint32_t queue_full;
    uint32_t expired_purged;
} mind_application_ingress_counters_t;

typedef enum mind_application_ingress_take_status {
    MIND_APPLICATION_INGRESS_TAKE_OK = 0,
    MIND_APPLICATION_INGRESS_TAKE_EMPTY,
    MIND_APPLICATION_INGRESS_TAKE_INVALID,
} mind_application_ingress_take_status_t;

typedef struct mind_application_ingress {
    mind_application_ingress_seen_t
        seen[MIND_APPLICATION_INGRESS_SEEN_CAPACITY];
    mind_application_ingress_event_t
        queue[MIND_APPLICATION_INGRESS_QUEUE_CAPACITY];
    mind_application_ingress_counters_t counters;
    uint8_t expected_network_id;
    uint8_t queue_head;
    uint8_t queue_tail;
    uint8_t queue_count;
} mind_application_ingress_t;

/* Initializes caller-owned fixed storage.  The production owner is the mesh
 * task, so admission and dequeue require no lock or allocation. */
void mind_application_ingress_init(mind_application_ingress_t *ingress,
                                   uint8_t expected_network_id);

/* Returns true only for a complete enough TM/01 MIND_EVENT manufacturer AD to
 * be application-owned.  A recognized MIND family is consumed even when later
 * validation rejects its wrapper, network, source, or payload. */
int mind_application_ingress_is_mind_family(
    const ble_mesh_sched_event_t *scheduler_event);

/* Validates, purges/checks, reserves, copies/publishes, and commits one direct
 * RX frame.  TX and fault events, other frame families, and state-independent
 * non-MIND RX return PASSTHROUGH unchanged. */
mind_application_ingress_result_t mind_application_ingress_receive(
    mind_application_ingress_t *ingress,
    const ble_mesh_sched_event_t *scheduler_event, uint32_t observed_at_ms);

mind_application_ingress_take_status_t mind_application_ingress_take(
    mind_application_ingress_t *ingress,
    mind_application_ingress_event_t *event_out);

#endif /* MIND_APPLICATION_INGRESS_H */
