#include "mind_application_wire.h"

#include <string.h>

#define MIND_APPLICATION_SCHEMA_OFFSET_VERSION    0u
#define MIND_APPLICATION_SCHEMA_OFFSET_EVENT_TYPE 1u
#define MIND_APPLICATION_SCHEMA_OFFSET_CONFIDENCE 2u
#define MIND_APPLICATION_SCHEMA_OFFSET_ACCEL_SVM  3u
#define MIND_APPLICATION_SCHEMA_OFFSET_MIC_LEVEL  5u
#define MIND_APPLICATION_SCHEMA_OFFSET_SEQUENCE   6u

#define MIND_APPLICATION_REPORT_OFFSET_SCHEMA 3u
#define MIND_APPLICATION_OBSERVED_OFFSET_SCHEMA 3u
#define MIND_APPLICATION_OBSERVED_OFFSET_RSSI   9u

#define MIND_APPLICATION_ROOT_OFFSET_VERSION    0u
#define MIND_APPLICATION_ROOT_OFFSET_ACTIVE     1u
#define MIND_APPLICATION_ROOT_OFFSET_NONCE      2u
#define MIND_APPLICATION_ROOT_OFFSET_GENERATION 4u
#define MIND_APPLICATION_ACK_OFFSET_STATUS      6u

static void put_u16_le(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
}

static uint16_t get_u16_le(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static void put_u24_le(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
    bytes[2] = (uint8_t)((value >> 16) & 0xffu);
}

static uint32_t get_u24_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16);
}

static int wearable_source_is_valid(uint8_t wearable_source)
{
    return wearable_source != 0u && wearable_source != 0xffu;
}

static int root_ack_status_is_valid(mind_application_root_ack_status_t status)
{
    switch (status) {
    case MIND_APPLICATION_ROOT_ACK_ACCEPTED:
    case MIND_APPLICATION_ROOT_ACK_DUPLICATE:
    case MIND_APPLICATION_ROOT_ACK_CAPACITY_REJECTED:
    case MIND_APPLICATION_ROOT_ACK_STALE:
        return 1;
    default:
        return 0;
    }
}

static mind_application_wire_result_t validate_schema_payload(
    const uint8_t schema_payload[MIND_PAYLOAD_SIZE], uint32_t packet_id24)
{
    uint16_t accel_svm;

    if (schema_payload[MIND_APPLICATION_SCHEMA_OFFSET_VERSION] !=
        MIND_SCHEMA_VERSION) {
        return MIND_APPLICATION_WIRE_ERR_SCHEMA_VERSION;
    }
    if (schema_payload[MIND_APPLICATION_SCHEMA_OFFSET_EVENT_TYPE] >
        MIND_EVT_FALL_AND_SHOUT) {
        return MIND_APPLICATION_WIRE_ERR_EVENT_TYPE;
    }
    if (schema_payload[MIND_APPLICATION_SCHEMA_OFFSET_CONFIDENCE] > 100u) {
        return MIND_APPLICATION_WIRE_ERR_CONFIDENCE;
    }

    accel_svm = get_u16_le(
        &schema_payload[MIND_APPLICATION_SCHEMA_OFFSET_ACCEL_SVM]);
    if (accel_svm > 8000u) {
        return MIND_APPLICATION_WIRE_ERR_ACCEL_SVM;
    }
    if (schema_payload[MIND_APPLICATION_SCHEMA_OFFSET_EVENT_TYPE] ==
            MIND_EVT_HEARTBEAT &&
        (schema_payload[MIND_APPLICATION_SCHEMA_OFFSET_CONFIDENCE] != 0u ||
         schema_payload[MIND_APPLICATION_SCHEMA_OFFSET_MIC_LEVEL] != 0u)) {
        return MIND_APPLICATION_WIRE_ERR_HEARTBEAT_FIELDS;
    }
    if (schema_payload[MIND_APPLICATION_SCHEMA_OFFSET_SEQUENCE] !=
        (uint8_t)(packet_id24 & 0xffu)) {
        return MIND_APPLICATION_WIRE_ERR_REPORT_SEQUENCE;
    }
    return MIND_APPLICATION_WIRE_OK;
}

static mind_application_wire_result_t validate_observed_report_bytes(
    const uint8_t bytes[MIND_APPLICATION_REPORT_BYTES], uint32_t packet_id24)
{
    uint8_t schema_payload[MIND_PAYLOAD_SIZE];
    mind_application_wire_result_t result;

    if (bytes[MIND_APPLICATION_OBSERVED_OFFSET_RSSI] > 127u) {
        return MIND_APPLICATION_WIRE_ERR_RSSI_MAGNITUDE;
    }
    memcpy(schema_payload, &bytes[MIND_APPLICATION_OBSERVED_OFFSET_SCHEMA],
           MIND_PAYLOAD_SIZE - 1u);
    schema_payload[MIND_APPLICATION_SCHEMA_OFFSET_SEQUENCE] =
        (uint8_t)(packet_id24 & 0xffu);
    result = validate_schema_payload(schema_payload, packet_id24);
    return result;
}

static mind_application_wire_result_t validate_root_state_bytes(
    const uint8_t bytes[MIND_APPLICATION_ROOT_STATE_BYTES])
{
    if (bytes[MIND_APPLICATION_ROOT_OFFSET_VERSION] !=
        MIND_APPLICATION_ROOT_STATE_VERSION) {
        return MIND_APPLICATION_WIRE_ERR_ROOT_VERSION;
    }
    if (bytes[MIND_APPLICATION_ROOT_OFFSET_ACTIVE] > 1u) {
        return MIND_APPLICATION_WIRE_ERR_ROOT_ACTIVE;
    }
    if (get_u16_le(&bytes[MIND_APPLICATION_ROOT_OFFSET_NONCE]) == 0u) {
        return MIND_APPLICATION_WIRE_ERR_ROOT_NONCE;
    }
    if (get_u16_le(&bytes[MIND_APPLICATION_ROOT_OFFSET_GENERATION]) == 0u) {
        return MIND_APPLICATION_WIRE_ERR_ROOT_GENERATION;
    }
    return MIND_APPLICATION_WIRE_OK;
}

static mind_application_wire_result_t validate_root_state(
    const mind_application_root_state_t *state)
{
    if (state->active > 1u) {
        return MIND_APPLICATION_WIRE_ERR_ROOT_ACTIVE;
    }
    if (state->nonce == 0u) {
        return MIND_APPLICATION_WIRE_ERR_ROOT_NONCE;
    }
    if (state->generation == 0u) {
        return MIND_APPLICATION_WIRE_ERR_ROOT_GENERATION;
    }
    return MIND_APPLICATION_WIRE_OK;
}

int mind_application_wire_packet_id24_is_valid(uint32_t packet_id24)
{
    return packet_id24 <= MIND_APPLICATION_PACKET_ID24_MAX;
}

mind_application_wire_result_t mind_application_wire_validate(
    const mind_application_wire_record_t *record)
{
    mind_application_wire_result_t result;

    if (record == NULL) {
        return MIND_APPLICATION_WIRE_ERR_NULL;
    }

    switch (record->app_kind) {
    case MIND_REPORT:
    case MIND_REPORT_OBSERVED:
        if (!wearable_source_is_valid(record->app_source)) {
            return MIND_APPLICATION_WIRE_ERR_SOURCE;
        }
        if (record->app_len != MIND_APPLICATION_REPORT_BYTES) {
            return MIND_APPLICATION_WIRE_ERR_LENGTH;
        }
        if (record->app_kind == MIND_REPORT) {
            result = validate_schema_payload(
                &record->app_bytes[MIND_APPLICATION_REPORT_OFFSET_SCHEMA],
                get_u24_le(record->app_bytes));
        } else {
            result = validate_observed_report_bytes(
                record->app_bytes, get_u24_le(record->app_bytes));
        }
        if (result != MIND_APPLICATION_WIRE_OK) {
            return result;
        }
        if (record->urgent !=
            (record->app_bytes[MIND_APPLICATION_REPORT_OFFSET_SCHEMA +
                               MIND_APPLICATION_SCHEMA_OFFSET_EVENT_TYPE] !=
              MIND_EVT_HEARTBEAT ? 1u : 0u)) {
            return MIND_APPLICATION_WIRE_ERR_URGENT;
        }
        return MIND_APPLICATION_WIRE_OK;

    case ROOT_STATE:
        if (record->app_source != 0u) {
            return MIND_APPLICATION_WIRE_ERR_SOURCE;
        }
        if (record->app_len != MIND_APPLICATION_ROOT_STATE_BYTES) {
            return MIND_APPLICATION_WIRE_ERR_LENGTH;
        }
        if (record->urgent != 0u) {
            return MIND_APPLICATION_WIRE_ERR_URGENT;
        }
        return validate_root_state_bytes(record->app_bytes);

    case ROOT_ACK:
        if (record->app_source != 0u) {
            return MIND_APPLICATION_WIRE_ERR_SOURCE;
        }
        if (record->app_len != MIND_APPLICATION_ROOT_ACK_BYTES) {
            return MIND_APPLICATION_WIRE_ERR_LENGTH;
        }
        if (record->urgent != 0u) {
            return MIND_APPLICATION_WIRE_ERR_URGENT;
        }
        result = validate_root_state_bytes(record->app_bytes);
        if (result != MIND_APPLICATION_WIRE_OK) {
            return result;
        }
        if (!root_ack_status_is_valid((mind_application_root_ack_status_t)
                                      record->app_bytes[
                                          MIND_APPLICATION_ACK_OFFSET_STATUS])) {
            return MIND_APPLICATION_WIRE_ERR_ACK_STATUS;
        }
        return MIND_APPLICATION_WIRE_OK;

    default:
        return MIND_APPLICATION_WIRE_ERR_UNKNOWN_KIND;
    }
}

mind_application_wire_result_t mind_application_wire_pack_report(
    mind_application_wire_record_t *record_out, uint8_t wearable_source,
    uint32_t packet_id24, const uint8_t schema_payload[MIND_PAYLOAD_SIZE])
{
    mind_application_wire_record_t record;
    mind_application_wire_result_t result;

    if (record_out == NULL || schema_payload == NULL) {
        return MIND_APPLICATION_WIRE_ERR_NULL;
    }
    if (!wearable_source_is_valid(wearable_source)) {
        return MIND_APPLICATION_WIRE_ERR_SOURCE;
    }
    if (!mind_application_wire_packet_id24_is_valid(packet_id24)) {
        return MIND_APPLICATION_WIRE_ERR_PACKET_ID24;
    }
    result = validate_schema_payload(schema_payload, packet_id24);
    if (result != MIND_APPLICATION_WIRE_OK) {
        return result;
    }

    memset(&record, 0, sizeof(record));
    record.app_kind = MIND_REPORT;
    record.app_source = wearable_source;
    record.urgent = schema_payload[MIND_APPLICATION_SCHEMA_OFFSET_EVENT_TYPE] !=
        MIND_EVT_HEARTBEAT ? 1u : 0u;
    record.app_len = MIND_APPLICATION_REPORT_BYTES;
    put_u24_le(record.app_bytes, packet_id24);
    memcpy(&record.app_bytes[3], schema_payload, MIND_PAYLOAD_SIZE);
    *record_out = record;
    return MIND_APPLICATION_WIRE_OK;
}

mind_application_wire_result_t mind_application_wire_pack_observed_report(
    mind_application_wire_record_t *record_out, uint8_t wearable_source,
    uint32_t packet_id24, const uint8_t schema_payload[MIND_PAYLOAD_SIZE],
    uint8_t rssi_magnitude_db)
{
    mind_application_wire_record_t record;
    mind_application_wire_result_t result;

    if (record_out == NULL || schema_payload == NULL) {
        return MIND_APPLICATION_WIRE_ERR_NULL;
    }
    if (!wearable_source_is_valid(wearable_source)) {
        return MIND_APPLICATION_WIRE_ERR_SOURCE;
    }
    if (!mind_application_wire_packet_id24_is_valid(packet_id24)) {
        return MIND_APPLICATION_WIRE_ERR_PACKET_ID24;
    }
    if (rssi_magnitude_db > 127u) {
        return MIND_APPLICATION_WIRE_ERR_RSSI_MAGNITUDE;
    }
    /* Validate the complete direct schema before its redundant sequence byte is
     * omitted from the observed record. */
    result = validate_schema_payload(schema_payload, packet_id24);
    if (result != MIND_APPLICATION_WIRE_OK) {
        return result;
    }

    memset(&record, 0, sizeof(record));
    record.app_kind = MIND_REPORT_OBSERVED;
    record.app_source = wearable_source;
    record.urgent = schema_payload[MIND_APPLICATION_SCHEMA_OFFSET_EVENT_TYPE] !=
        MIND_EVT_HEARTBEAT ? 1u : 0u;
    record.app_len = MIND_APPLICATION_REPORT_BYTES;
    put_u24_le(record.app_bytes, packet_id24);
    memcpy(&record.app_bytes[MIND_APPLICATION_OBSERVED_OFFSET_SCHEMA], schema_payload,
           MIND_PAYLOAD_SIZE - 1u);
    record.app_bytes[MIND_APPLICATION_OBSERVED_OFFSET_RSSI] = rssi_magnitude_db;
    *record_out = record;
    return MIND_APPLICATION_WIRE_OK;
}

mind_application_wire_result_t mind_application_wire_unpack_report(
    const mind_application_wire_record_t *record,
    mind_application_report_t *report_out)
{
    mind_application_wire_result_t result;

    if (report_out == NULL) {
        return MIND_APPLICATION_WIRE_ERR_NULL;
    }
    result = mind_application_wire_validate(record);
    if (result != MIND_APPLICATION_WIRE_OK) {
        return result;
    }
    if (record->app_kind != MIND_REPORT &&
        record->app_kind != MIND_REPORT_OBSERVED) {
        return MIND_APPLICATION_WIRE_ERR_UNKNOWN_KIND;
    }

    report_out->packet_id24 = get_u24_le(record->app_bytes);
    if (record->app_kind == MIND_REPORT) {
        memcpy(report_out->schema_payload,
               &record->app_bytes[MIND_APPLICATION_REPORT_OFFSET_SCHEMA],
               MIND_PAYLOAD_SIZE);
        report_out->rssi_magnitude_db = 0u;
    } else {
        memcpy(report_out->schema_payload,
               &record->app_bytes[MIND_APPLICATION_OBSERVED_OFFSET_SCHEMA],
               MIND_PAYLOAD_SIZE - 1u);
        report_out->schema_payload[MIND_APPLICATION_SCHEMA_OFFSET_SEQUENCE] =
            (uint8_t)(report_out->packet_id24 & 0xffu);
        report_out->rssi_magnitude_db =
            record->app_bytes[MIND_APPLICATION_OBSERVED_OFFSET_RSSI];
    }
    return MIND_APPLICATION_WIRE_OK;
}

mind_application_wire_result_t mind_application_wire_pack_root_state(
    mind_application_wire_record_t *record_out,
    const mind_application_root_state_t *state)
{
    mind_application_wire_record_t record;
    mind_application_wire_result_t result;

    if (record_out == NULL || state == NULL) {
        return MIND_APPLICATION_WIRE_ERR_NULL;
    }
    result = validate_root_state(state);
    if (result != MIND_APPLICATION_WIRE_OK) {
        return result;
    }

    memset(&record, 0, sizeof(record));
    record.app_kind = ROOT_STATE;
    record.urgent = 0u;
    record.app_len = MIND_APPLICATION_ROOT_STATE_BYTES;
    record.app_bytes[MIND_APPLICATION_ROOT_OFFSET_VERSION] =
        MIND_APPLICATION_ROOT_STATE_VERSION;
    record.app_bytes[MIND_APPLICATION_ROOT_OFFSET_ACTIVE] = state->active;
    put_u16_le(&record.app_bytes[MIND_APPLICATION_ROOT_OFFSET_NONCE], state->nonce);
    put_u16_le(&record.app_bytes[MIND_APPLICATION_ROOT_OFFSET_GENERATION],
               state->generation);
    *record_out = record;
    return MIND_APPLICATION_WIRE_OK;
}

mind_application_wire_result_t mind_application_wire_unpack_root_state(
    const mind_application_wire_record_t *record,
    mind_application_root_state_t *state_out)
{
    mind_application_wire_result_t result;

    if (state_out == NULL) {
        return MIND_APPLICATION_WIRE_ERR_NULL;
    }
    result = mind_application_wire_validate(record);
    if (result != MIND_APPLICATION_WIRE_OK) {
        return result;
    }
    if (record->app_kind != ROOT_STATE) {
        return MIND_APPLICATION_WIRE_ERR_UNKNOWN_KIND;
    }

    state_out->active = record->app_bytes[MIND_APPLICATION_ROOT_OFFSET_ACTIVE];
    state_out->nonce = get_u16_le(
        &record->app_bytes[MIND_APPLICATION_ROOT_OFFSET_NONCE]);
    state_out->generation = get_u16_le(
        &record->app_bytes[MIND_APPLICATION_ROOT_OFFSET_GENERATION]);
    return MIND_APPLICATION_WIRE_OK;
}

mind_application_wire_result_t mind_application_wire_pack_root_ack(
    mind_application_wire_record_t *record_out,
    const mind_application_root_ack_t *ack)
{
    mind_application_wire_record_t record;
    mind_application_wire_result_t result;

    if (record_out == NULL || ack == NULL) {
        return MIND_APPLICATION_WIRE_ERR_NULL;
    }
    result = validate_root_state(&ack->state);
    if (result != MIND_APPLICATION_WIRE_OK) {
        return result;
    }
    if (!root_ack_status_is_valid(ack->status)) {
        return MIND_APPLICATION_WIRE_ERR_ACK_STATUS;
    }

    memset(&record, 0, sizeof(record));
    record.app_kind = ROOT_ACK;
    record.urgent = 0u;
    record.app_len = MIND_APPLICATION_ROOT_ACK_BYTES;
    record.app_bytes[MIND_APPLICATION_ROOT_OFFSET_VERSION] =
        MIND_APPLICATION_ROOT_STATE_VERSION;
    record.app_bytes[MIND_APPLICATION_ROOT_OFFSET_ACTIVE] = ack->state.active;
    put_u16_le(&record.app_bytes[MIND_APPLICATION_ROOT_OFFSET_NONCE],
               ack->state.nonce);
    put_u16_le(&record.app_bytes[MIND_APPLICATION_ROOT_OFFSET_GENERATION],
               ack->state.generation);
    record.app_bytes[MIND_APPLICATION_ACK_OFFSET_STATUS] = (uint8_t)ack->status;
    *record_out = record;
    return MIND_APPLICATION_WIRE_OK;
}

mind_application_wire_result_t mind_application_wire_unpack_root_ack(
    const mind_application_wire_record_t *record,
    mind_application_root_ack_t *ack_out)
{
    mind_application_wire_result_t result;

    if (ack_out == NULL) {
        return MIND_APPLICATION_WIRE_ERR_NULL;
    }
    result = mind_application_wire_validate(record);
    if (result != MIND_APPLICATION_WIRE_OK) {
        return result;
    }
    if (record->app_kind != ROOT_ACK) {
        return MIND_APPLICATION_WIRE_ERR_UNKNOWN_KIND;
    }

    ack_out->state.active = record->app_bytes[MIND_APPLICATION_ROOT_OFFSET_ACTIVE];
    ack_out->state.nonce = get_u16_le(
        &record->app_bytes[MIND_APPLICATION_ROOT_OFFSET_NONCE]);
    ack_out->state.generation = get_u16_le(
        &record->app_bytes[MIND_APPLICATION_ROOT_OFFSET_GENERATION]);
    ack_out->status = (mind_application_root_ack_status_t)
        record->app_bytes[MIND_APPLICATION_ACK_OFFSET_STATUS];
    return MIND_APPLICATION_WIRE_OK;
}
