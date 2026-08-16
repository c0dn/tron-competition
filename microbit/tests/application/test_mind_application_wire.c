#include "mind_application_wire.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int checks;
static unsigned int failures;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static void put_u16_le(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xffu);
    bytes[1] = (uint8_t)((value >> 8) & 0xffu);
}

static void make_report_payload(uint8_t payload[MIND_PAYLOAD_SIZE],
                                uint32_t packet_id24)
{
    payload[0] = MIND_SCHEMA_VERSION;
    payload[1] = MIND_EVT_FALL_AND_SHOUT;
    payload[2] = 73u;
    put_u16_le(&payload[3], 0x1a1fu);
    payload[5] = 0x7du;
    payload[6] = (uint8_t)(packet_id24 & 0xffu);
}

static mind_application_root_state_t make_root_state(void)
{
    mind_application_root_state_t state;

    state.active = 1u;
    state.nonce = 0x1234u;
    state.generation = 0xfedcu;
    return state;
}

static void test_golden_records_and_round_trips(void)
{
    uint8_t payload[MIND_PAYLOAD_SIZE];
    static const uint8_t expected_report[MIND_APPLICATION_REPORT_BYTES] = {
        0xefu, 0xcdu, 0xabu, 0x01u, 0x05u, 0x49u, 0x1fu, 0x1au, 0x7du, 0xefu,
    };
    static const uint8_t expected_root_state[MIND_APPLICATION_ROOT_STATE_BYTES] = {
        0x01u, 0x01u, 0x34u, 0x12u, 0xdcu, 0xfeu,
    };
    static const uint8_t expected_root_ack[MIND_APPLICATION_ROOT_ACK_BYTES] = {
        0x01u, 0x01u, 0x34u, 0x12u, 0xdcu, 0xfeu, 0x02u,
    };
    mind_application_wire_record_t record;
    mind_application_report_t report;
    mind_application_root_state_t state = make_root_state();
    mind_application_root_state_t unpacked_state;
    mind_application_root_ack_t ack;
    mind_application_root_ack_t unpacked_ack;

    make_report_payload(payload, 0xabcdefu);
    check(MIND_REPORT == 0x02u && ROOT_STATE == 0x03u && ROOT_ACK == 0x04u,
          "application kinds have their frozen values");
    check(mind_application_wire_pack_report(&record, 0x2au, 0xabcdefu,
                                            payload) == MIND_APPLICATION_WIRE_OK,
          "MIND_REPORT packs");
    check(record.app_kind == MIND_REPORT && record.app_source == 0x2au &&
          record.urgent == 1u &&
          record.app_len == MIND_APPLICATION_REPORT_BYTES,
          "MIND_REPORT fields are exact");
    check(memcmp(record.app_bytes, expected_report, sizeof(expected_report)) == 0,
          "MIND_REPORT golden bytes are exact");
    check(mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_OK,
          "MIND_REPORT validates after packing");
    memset(&report, 0, sizeof(report));
    check(mind_application_wire_unpack_report(&record, &report) ==
          MIND_APPLICATION_WIRE_OK,
          "MIND_REPORT unpacks");
    check(report.packet_id24 == 0xabcdefu &&
          memcmp(report.schema_payload, payload, MIND_PAYLOAD_SIZE) == 0,
          "MIND_REPORT round trip preserves packet id and schema bytes");
    record.urgent = 0u;
    check(mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_ERR_URGENT &&
          mind_application_wire_unpack_report(&record, &report) ==
              MIND_APPLICATION_WIRE_ERR_URGENT,
          "incident MIND_REPORT requires urgent generic DATA");
    record.urgent = 1u;

    check(mind_application_wire_pack_root_state(&record, &state) ==
          MIND_APPLICATION_WIRE_OK,
          "ROOT_STATE packs");
    check(record.app_kind == ROOT_STATE && record.app_source == 0u &&
          record.urgent == 0u &&
          record.app_len == MIND_APPLICATION_ROOT_STATE_BYTES,
          "ROOT_STATE fields are exact");
    check(memcmp(record.app_bytes, expected_root_state,
                 sizeof(expected_root_state)) == 0,
          "ROOT_STATE golden bytes are exact");
    memset(&unpacked_state, 0, sizeof(unpacked_state));
    check(mind_application_wire_unpack_root_state(&record, &unpacked_state) ==
          MIND_APPLICATION_WIRE_OK,
          "ROOT_STATE unpacks");
    check(unpacked_state.active == state.active &&
          unpacked_state.nonce == state.nonce &&
          unpacked_state.generation == state.generation,
          "ROOT_STATE round trip preserves all fields");
    record.urgent = 1u;
    check(mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_ERR_URGENT &&
          mind_application_wire_unpack_root_state(&record, &unpacked_state) ==
              MIND_APPLICATION_WIRE_ERR_URGENT,
          "ROOT_STATE requires nonurgent generic DATA");
    record.urgent = 0u;

    ack.state = state;
    ack.status = MIND_APPLICATION_ROOT_ACK_CAPACITY_REJECTED;
    check(mind_application_wire_pack_root_ack(&record, &ack) ==
          MIND_APPLICATION_WIRE_OK,
          "ROOT_ACK packs");
    check(record.app_kind == ROOT_ACK && record.app_source == 0u &&
          record.urgent == 0u &&
          record.app_len == MIND_APPLICATION_ROOT_ACK_BYTES,
          "ROOT_ACK fields are exact");
    check(memcmp(record.app_bytes, expected_root_ack, sizeof(expected_root_ack)) == 0,
          "ROOT_ACK golden bytes are exact");
    memset(&unpacked_ack, 0, sizeof(unpacked_ack));
    check(mind_application_wire_unpack_root_ack(&record, &unpacked_ack) ==
          MIND_APPLICATION_WIRE_OK,
          "ROOT_ACK unpacks");
    check(unpacked_ack.state.active == ack.state.active &&
          unpacked_ack.state.nonce == ack.state.nonce &&
          unpacked_ack.state.generation == ack.state.generation &&
          unpacked_ack.status == ack.status,
          "ROOT_ACK round trip preserves echoed state and status");
    record.urgent = 1u;
    check(mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_ERR_URGENT &&
          mind_application_wire_unpack_root_ack(&record, &unpacked_ack) ==
              MIND_APPLICATION_WIRE_ERR_URGENT,
          "ROOT_ACK requires nonurgent generic DATA");
}

static void test_report_urgent_uses_schema_event_type(void)
{
    static const uint8_t expected_incident[MIND_APPLICATION_REPORT_BYTES] = {
        0x34u, 0x00u, 0x12u, 0x01u, 0x05u, 0x49u, 0x1fu, 0x1au, 0x7du, 0x34u,
    };
    static const uint8_t expected_heartbeat[MIND_APPLICATION_REPORT_BYTES] = {
        0x34u, 0x05u, 0x12u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x34u,
    };
    uint8_t payload[MIND_PAYLOAD_SIZE];
    mind_application_wire_record_t record;
    mind_application_report_t report;

    make_report_payload(payload, 0x120034u);
    check(mind_application_wire_pack_report(&record, 0x2au, 0x120034u,
                                            payload) == MIND_APPLICATION_WIRE_OK &&
          memcmp(record.app_bytes, expected_incident, sizeof(expected_incident)) == 0 &&
          record.app_bytes[1] == MIND_EVT_HEARTBEAT &&
          record.urgent == 1u &&
          mind_application_wire_unpack_report(&record, &report) ==
              MIND_APPLICATION_WIRE_OK,
          "incident urgency uses the schema event type, not packet-ID middle byte");

    memset(payload, 0, sizeof(payload));
    payload[0] = MIND_SCHEMA_VERSION;
    payload[1] = MIND_EVT_HEARTBEAT;
    payload[6] = 0x34u;
    check(mind_application_wire_pack_report(&record, 0x2au, 0x120534u,
                                            payload) == MIND_APPLICATION_WIRE_OK &&
          memcmp(record.app_bytes, expected_heartbeat,
                 sizeof(expected_heartbeat)) == 0 &&
          record.app_bytes[1] == MIND_EVT_FALL_AND_SHOUT &&
          record.urgent == 0u &&
          mind_application_wire_unpack_report(&record, &report) ==
              MIND_APPLICATION_WIRE_OK,
          "heartbeat urgency uses the schema event type, not packet-ID middle byte");
}

static void test_report_ranges_and_schema_compatibility(void)
{
    uint8_t payload[MIND_PAYLOAD_SIZE];
    uint8_t heartbeat[MIND_PAYLOAD_SIZE];
    mind_application_wire_record_t record;
    mind_application_wire_record_t unchanged;
    mind_application_report_t report;
    uint8_t length;

    make_report_payload(payload, 0u);
    check(mind_application_wire_packet_id24_is_valid(0u) &&
          mind_application_wire_packet_id24_is_valid(0xffffffu) &&
          !mind_application_wire_packet_id24_is_valid(0x01000000u),
          "packet-id helper preserves the exact 24-bit range");
    check(mind_application_wire_pack_report(&record, 0x01u, 0u, payload) ==
          MIND_APPLICATION_WIRE_OK &&
          record.app_bytes[0] == 0u && record.app_bytes[1] == 0u &&
          record.app_bytes[2] == 0u,
          "packet ID zero is legal");
    make_report_payload(payload, 0xffffffu);
    check(mind_application_wire_pack_report(&record, 0xfeu, 0xffffffu,
                                            payload) == MIND_APPLICATION_WIRE_OK,
          "largest packet ID24 is legal");
    unchanged = record;
    check(mind_application_wire_pack_report(&record, 0xfeu, 0x01000000u,
                                            payload) ==
          MIND_APPLICATION_WIRE_ERR_PACKET_ID24 &&
          memcmp(&record, &unchanged, sizeof(record)) == 0,
          "wide packet ID is rejected without masking or publishing output");

    make_report_payload(payload, 0xabcdefu);
    check(mind_application_wire_pack_report(&record, 0x2au, 0xabcdefu,
                                            payload) == MIND_APPLICATION_WIRE_OK,
          "schema-v1 report fixture packs");
    check(record.urgent == 1u,
          "incident MIND_REPORT pack derives urgent");
    for (length = 0u; length <= MIND_APPLICATION_WIRE_APP_BYTES_MAX; length++) {
        if (length == MIND_APPLICATION_REPORT_BYTES) {
            continue;
        }
        record.app_len = length;
        check(mind_application_wire_validate(&record) ==
              MIND_APPLICATION_WIRE_ERR_LENGTH,
              "every in-buffer non-report length is rejected");
    }
    record.app_len = (uint8_t)(MIND_APPLICATION_WIRE_APP_BYTES_MAX + 1u);
    check(mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_ERR_LENGTH,
          "out-of-buffer reported length is rejected");
    record.app_len = MIND_APPLICATION_REPORT_BYTES;
    record.app_source = 0u;
    check(mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_ERR_SOURCE,
          "MIND_REPORT source zero is rejected");
    record.app_source = 0xffu;
    check(mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_ERR_SOURCE,
          "MIND_REPORT source ff is rejected");
    record.app_source = 0x2au;

    record.app_bytes[3] = 2u;
    check(mind_application_wire_validate(&record) ==
          MIND_APPLICATION_WIRE_ERR_SCHEMA_VERSION,
          "non-v1 report schema is rejected");
    record.app_bytes[3] = MIND_SCHEMA_VERSION;
    record.app_bytes[4] = (uint8_t)(MIND_EVT_FALL_AND_SHOUT + 1u);
    check(mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_ERR_EVENT_TYPE,
          "unknown schema-v1 event type is rejected");
    record.app_bytes[4] = MIND_EVT_FALL_AND_SHOUT;
    record.app_bytes[5] = 101u;
    check(mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_ERR_CONFIDENCE,
          "out-of-range confidence is rejected");
    record.app_bytes[5] = 73u;
    put_u16_le(&record.app_bytes[6], 8001u);
    check(mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_ERR_ACCEL_SVM,
          "out-of-range little-endian acceleration is rejected");
    put_u16_le(&record.app_bytes[6], 0x1a1fu);
    record.app_bytes[9] = 0u;
    check(mind_application_wire_validate(&record) ==
          MIND_APPLICATION_WIRE_ERR_REPORT_SEQUENCE,
          "report schema sequence must match packet ID low byte");
    record.app_bytes[9] = 0xefu;

    memset(heartbeat, 0, sizeof(heartbeat));
    heartbeat[0] = MIND_SCHEMA_VERSION;
    heartbeat[1] = MIND_EVT_HEARTBEAT;
    heartbeat[6] = 0x44u;
    check(mind_application_wire_pack_report(&record, 0x2au, 0x44u, heartbeat) ==
          MIND_APPLICATION_WIRE_OK && record.urgent == 0u,
          "valid heartbeat schema payload is compatible");
    record.urgent = 1u;
    check(mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_ERR_URGENT &&
          mind_application_wire_unpack_report(&record, &report) ==
              MIND_APPLICATION_WIRE_ERR_URGENT,
          "heartbeat MIND_REPORT rejects urgent generic DATA");
    record.urgent = 0u;
    heartbeat[2] = 1u;
    check(mind_application_wire_pack_report(&record, 0x2au, 0x44u, heartbeat) ==
          MIND_APPLICATION_WIRE_ERR_HEARTBEAT_FIELDS,
          "heartbeat confidence must be zero");
    heartbeat[2] = 0u;
    heartbeat[5] = 1u;
    check(mind_application_wire_pack_report(&record, 0x2au, 0x44u, heartbeat) ==
          MIND_APPLICATION_WIRE_ERR_HEARTBEAT_FIELDS,
          "heartbeat mic level must be zero");
}

static void test_root_state_malformed_fields(void)
{
    mind_application_root_state_t state = make_root_state();
    mind_application_wire_record_t record;
    uint8_t length;

    check(mind_application_wire_pack_root_state(&record, &state) ==
          MIND_APPLICATION_WIRE_OK,
          "ROOT_STATE malformed fixture packs");
    for (length = 0u; length <= MIND_APPLICATION_WIRE_APP_BYTES_MAX; length++) {
        if (length == MIND_APPLICATION_ROOT_STATE_BYTES) {
            continue;
        }
        record.app_len = length;
        check(mind_application_wire_validate(&record) ==
              MIND_APPLICATION_WIRE_ERR_LENGTH,
              "every in-buffer non-root-state length is rejected");
    }
    record.app_len = MIND_APPLICATION_ROOT_STATE_BYTES;
    record.app_source = 1u;
    check(mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_ERR_SOURCE,
          "ROOT_STATE nonzero source is rejected");
    record.app_source = 0u;
    record.app_bytes[0] = 0u;
    check(mind_application_wire_validate(&record) ==
          MIND_APPLICATION_WIRE_ERR_ROOT_VERSION,
          "ROOT_STATE version zero is rejected");
    record.app_bytes[0] = 2u;
    check(mind_application_wire_validate(&record) ==
          MIND_APPLICATION_WIRE_ERR_ROOT_VERSION,
          "ROOT_STATE future version is rejected");
    record.app_bytes[0] = MIND_APPLICATION_ROOT_STATE_VERSION;
    record.app_bytes[1] = 2u;
    check(mind_application_wire_validate(&record) ==
          MIND_APPLICATION_WIRE_ERR_ROOT_ACTIVE,
          "ROOT_STATE active outside zero or one is rejected");
    record.app_bytes[1] = 1u;
    put_u16_le(&record.app_bytes[2], 0u);
    check(mind_application_wire_validate(&record) ==
          MIND_APPLICATION_WIRE_ERR_ROOT_NONCE,
          "ROOT_STATE zero nonce is rejected");
    put_u16_le(&record.app_bytes[2], state.nonce);
    put_u16_le(&record.app_bytes[4], 0u);
    check(mind_application_wire_validate(&record) ==
          MIND_APPLICATION_WIRE_ERR_ROOT_GENERATION,
          "ROOT_STATE zero generation is rejected");

    state.active = 2u;
    check(mind_application_wire_pack_root_state(&record, &state) ==
          MIND_APPLICATION_WIRE_ERR_ROOT_ACTIVE,
          "ROOT_STATE pack rejects invalid active field");
    state = make_root_state();
    state.nonce = 0u;
    check(mind_application_wire_pack_root_state(&record, &state) ==
          MIND_APPLICATION_WIRE_ERR_ROOT_NONCE,
          "ROOT_STATE pack rejects zero nonce");
    state = make_root_state();
    state.generation = 0u;
    check(mind_application_wire_pack_root_state(&record, &state) ==
          MIND_APPLICATION_WIRE_ERR_ROOT_GENERATION,
          "ROOT_STATE pack rejects zero generation");
}

static void test_root_ack_malformed_fields_and_statuses(void)
{
    mind_application_root_ack_t ack;
    mind_application_wire_record_t record;
    mind_application_wire_record_t unchanged;
    uint8_t length;
    uint8_t status;

    ack.state = make_root_state();
    ack.status = MIND_APPLICATION_ROOT_ACK_ACCEPTED;
    check(mind_application_wire_pack_root_ack(&record, &ack) ==
          MIND_APPLICATION_WIRE_OK,
          "ROOT_ACK malformed fixture packs");
    for (length = 0u; length <= MIND_APPLICATION_WIRE_APP_BYTES_MAX; length++) {
        if (length == MIND_APPLICATION_ROOT_ACK_BYTES) {
            continue;
        }
        record.app_len = length;
        check(mind_application_wire_validate(&record) ==
              MIND_APPLICATION_WIRE_ERR_LENGTH,
              "every in-buffer non-root-ack length is rejected");
    }
    record.app_len = MIND_APPLICATION_ROOT_ACK_BYTES;
    record.app_source = 1u;
    check(mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_ERR_SOURCE,
          "ROOT_ACK nonzero source is rejected");
    record.app_source = 0u;
    record.app_bytes[0] = 2u;
    check(mind_application_wire_validate(&record) ==
          MIND_APPLICATION_WIRE_ERR_ROOT_VERSION,
          "ROOT_ACK rejects malformed echoed version");
    record.app_bytes[0] = MIND_APPLICATION_ROOT_STATE_VERSION;
    record.app_bytes[1] = 2u;
    check(mind_application_wire_validate(&record) ==
          MIND_APPLICATION_WIRE_ERR_ROOT_ACTIVE,
          "ROOT_ACK rejects malformed echoed active state");
    record.app_bytes[1] = 1u;
    put_u16_le(&record.app_bytes[2], 0u);
    check(mind_application_wire_validate(&record) ==
          MIND_APPLICATION_WIRE_ERR_ROOT_NONCE,
          "ROOT_ACK rejects malformed echoed nonce");
    put_u16_le(&record.app_bytes[2], ack.state.nonce);
    put_u16_le(&record.app_bytes[4], 0u);
    check(mind_application_wire_validate(&record) ==
          MIND_APPLICATION_WIRE_ERR_ROOT_GENERATION,
          "ROOT_ACK rejects malformed echoed generation");
    put_u16_le(&record.app_bytes[4], ack.state.generation);
    record.app_bytes[6] = 4u;
    check(mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_ERR_ACK_STATUS,
          "ROOT_ACK unknown status is rejected");

    for (status = MIND_APPLICATION_ROOT_ACK_ACCEPTED;
         status <= MIND_APPLICATION_ROOT_ACK_STALE; status++) {
        ack.status = (mind_application_root_ack_status_t)status;
        check(mind_application_wire_pack_root_ack(&record, &ack) ==
              MIND_APPLICATION_WIRE_OK &&
              record.app_bytes[6] == status &&
              mind_application_wire_validate(&record) == MIND_APPLICATION_WIRE_OK,
              "each frozen ROOT_ACK status packs and validates");
    }
    ack.status = (mind_application_root_ack_status_t)4u;
    check(mind_application_wire_pack_root_ack(&record, &ack) ==
          MIND_APPLICATION_WIRE_ERR_ACK_STATUS,
          "ROOT_ACK pack rejects unknown status");
    unchanged = record;
    ack.status = (mind_application_root_ack_status_t)-1;
    check(mind_application_wire_pack_root_ack(&record, &ack) ==
          MIND_APPLICATION_WIRE_ERR_ACK_STATUS &&
          memcmp(&record, &unchanged, sizeof(record)) == 0,
          "ROOT_ACK pack rejects negative enum status without publishing output");
}

static void test_unknown_kind_and_null_inputs(void)
{
    mind_application_wire_record_t record;
    mind_application_root_state_t state = make_root_state();
    uint8_t payload[MIND_PAYLOAD_SIZE];

    memset(&record, 0, sizeof(record));
    record.app_kind = 0x7fu;
    check(mind_application_wire_validate(&record) ==
          MIND_APPLICATION_WIRE_ERR_UNKNOWN_KIND,
          "unknown application kind is rejected only at Layer 7");
    check(mind_application_wire_validate(NULL) == MIND_APPLICATION_WIRE_ERR_NULL,
          "null record is rejected");
    make_report_payload(payload, 1u);
    check(mind_application_wire_pack_report(NULL, 1u, 1u, payload) ==
          MIND_APPLICATION_WIRE_ERR_NULL,
          "report pack requires output");
    check(mind_application_wire_pack_report(&record, 1u, 1u, NULL) ==
          MIND_APPLICATION_WIRE_ERR_NULL,
          "report pack requires schema bytes");
    check(mind_application_wire_pack_root_state(NULL, &state) ==
          MIND_APPLICATION_WIRE_ERR_NULL,
          "root-state pack requires output");
    check(mind_application_wire_unpack_root_state(NULL, &state) ==
          MIND_APPLICATION_WIRE_ERR_NULL,
          "root-state unpack rejects null record");
}

int main(void)
{
    printf("mind_application_wire tests\n\n");
    test_golden_records_and_round_trips();
    test_report_urgent_uses_schema_event_type();
    test_report_ranges_and_schema_compatibility();
    test_root_state_malformed_fields();
    test_root_ack_malformed_fields_and_statuses();
    test_unknown_kind_and_null_inputs();
    printf("\n%u checks, %u failures\n", checks, failures);
    return failures == 0u ? 0 : 1;
}
