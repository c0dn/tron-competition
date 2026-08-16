#ifndef MIND_APPLICATION_WIRE_H
#define MIND_APPLICATION_WIRE_H

/*
 * Layer-7 MIND records carried as opaque generic TAVRN DATA payloads.
 *
 * This module intentionally owns only application semantics.  A caller copies
 * app_kind, app_source, urgent, app_len, and app_bytes from or to generic
 * transport DATA; TAVRN neither includes this header nor validates these
 * record kinds.
 */

#include <stdint.h>

#include "schema.h"

#define MIND_REPORT 0x02u
#define ROOT_STATE  0x03u
#define ROOT_ACK    0x04u

#define MIND_APPLICATION_WIRE_APP_BYTES_MAX 10u
#define MIND_APPLICATION_REPORT_BYTES       10u
#define MIND_APPLICATION_ROOT_STATE_BYTES   6u
#define MIND_APPLICATION_ROOT_ACK_BYTES     7u
#define MIND_APPLICATION_PACKET_ID24_MAX    0xFFFFFFu

#define MIND_APPLICATION_ROOT_STATE_VERSION 1u

typedef char mind_application_wire_report_size_guard[
    (MIND_APPLICATION_REPORT_BYTES == 10u) ? 1 : -1];
typedef char mind_application_wire_root_state_size_guard[
    (MIND_APPLICATION_ROOT_STATE_BYTES == 6u) ? 1 : -1];
typedef char mind_application_wire_root_ack_size_guard[
    (MIND_APPLICATION_ROOT_ACK_BYTES == 7u) ? 1 : -1];
typedef char mind_application_wire_schema_size_guard[
    (MIND_PAYLOAD_SIZE == 7u) ? 1 : -1];

/* Application-owned counterpart of generic DATA's app fields. */
typedef struct mind_application_wire_record {
    uint8_t app_kind;
    uint8_t app_source;
    uint8_t urgent;
    uint8_t app_len;
    uint8_t app_bytes[MIND_APPLICATION_WIRE_APP_BYTES_MAX];
} mind_application_wire_record_t;

typedef struct mind_application_report {
    uint32_t packet_id24;
    uint8_t schema_payload[MIND_PAYLOAD_SIZE];
} mind_application_report_t;

typedef struct mind_application_root_state {
    uint8_t active;
    uint16_t nonce;
    uint16_t generation;
} mind_application_root_state_t;

typedef enum mind_application_root_ack_status {
    MIND_APPLICATION_ROOT_ACK_ACCEPTED = 0,
    MIND_APPLICATION_ROOT_ACK_DUPLICATE = 1,
    MIND_APPLICATION_ROOT_ACK_CAPACITY_REJECTED = 2,
    MIND_APPLICATION_ROOT_ACK_STALE = 3,
} mind_application_root_ack_status_t;

typedef struct mind_application_root_ack {
    mind_application_root_state_t state;
    mind_application_root_ack_status_t status;
} mind_application_root_ack_t;

typedef enum mind_application_wire_result {
    MIND_APPLICATION_WIRE_OK = 0,
    MIND_APPLICATION_WIRE_ERR_NULL,
    MIND_APPLICATION_WIRE_ERR_UNKNOWN_KIND,
    MIND_APPLICATION_WIRE_ERR_SOURCE,
    MIND_APPLICATION_WIRE_ERR_URGENT,
    MIND_APPLICATION_WIRE_ERR_LENGTH,
    MIND_APPLICATION_WIRE_ERR_PACKET_ID24,
    MIND_APPLICATION_WIRE_ERR_SCHEMA_VERSION,
    MIND_APPLICATION_WIRE_ERR_EVENT_TYPE,
    MIND_APPLICATION_WIRE_ERR_CONFIDENCE,
    MIND_APPLICATION_WIRE_ERR_ACCEL_SVM,
    MIND_APPLICATION_WIRE_ERR_HEARTBEAT_FIELDS,
    MIND_APPLICATION_WIRE_ERR_REPORT_SEQUENCE,
    MIND_APPLICATION_WIRE_ERR_ROOT_VERSION,
    MIND_APPLICATION_WIRE_ERR_ROOT_ACTIVE,
    MIND_APPLICATION_WIRE_ERR_ROOT_NONCE,
    MIND_APPLICATION_WIRE_ERR_ROOT_GENERATION,
    MIND_APPLICATION_WIRE_ERR_ACK_STATUS,
} mind_application_wire_result_t;

/* Packet IDs are exact 24-bit application identities; values are not masked. */
int mind_application_wire_packet_id24_is_valid(uint32_t packet_id24);

/* Validate one Layer-7 record after generic transport has delivered it. */
mind_application_wire_result_t mind_application_wire_validate(
    const mind_application_wire_record_t *record);

mind_application_wire_result_t mind_application_wire_pack_report(
    mind_application_wire_record_t *record_out, uint8_t wearable_source,
    uint32_t packet_id24, const uint8_t schema_payload[MIND_PAYLOAD_SIZE]);

mind_application_wire_result_t mind_application_wire_unpack_report(
    const mind_application_wire_record_t *record,
    mind_application_report_t *report_out);

mind_application_wire_result_t mind_application_wire_pack_root_state(
    mind_application_wire_record_t *record_out,
    const mind_application_root_state_t *state);

mind_application_wire_result_t mind_application_wire_unpack_root_state(
    const mind_application_wire_record_t *record,
    mind_application_root_state_t *state_out);

mind_application_wire_result_t mind_application_wire_pack_root_ack(
    mind_application_wire_record_t *record_out,
    const mind_application_root_ack_t *ack);

mind_application_wire_result_t mind_application_wire_unpack_root_ack(
    const mind_application_wire_record_t *record,
    mind_application_root_ack_t *ack_out);

#endif /* MIND_APPLICATION_WIRE_H */
