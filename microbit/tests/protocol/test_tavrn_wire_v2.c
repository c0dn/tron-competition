#include "tavrn_wire_v2.h"
#include "ble_radio.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;
static unsigned int emitted;

static int first_for(const char *requirement)
{
    unsigned int bit = requirement[8] == '1' ? 0u : 1u;
    unsigned int mask = 1u << bit;

    if ((emitted & mask) != 0u) {
        return 0;
    }
    emitted |= mask;
    return 1;
}

#define CHECK(requirement, expression) \
    do { \
        if (!(expression)) { \
            if (first_for(requirement)) { \
                printf("FAIL %s: %s:%d: assertion failed: %s\n", \
                       (requirement), __FILE__, __LINE__, #expression); \
            } \
            failures++; \
        } \
    } while (0)

static const uint8_t adva_a[6] = { 0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu };
static const uint8_t adva_b[6] = { 0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u };
static const uint8_t adva_c[6] = { 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0xc1u };

static const uint8_t data16[] = {
    0x02u, 0x01u, 0x06u, 0x1bu, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, 0x2au, 0x10u, 0x20u, 0x30u,
    0xdcu, 0x4bu, 0x18u, 0x42u, 0x11u, 0x22u, 0x34u,
    0x12u, 0x01u, 0x07u, 0x01u, 0x05u, 0x64u, 0x34u,
    0x12u, 0x50u, 0x7eu,
};
static const uint8_t data8_max[] = {
    0x02u, 0x01u, 0x06u, 0x1bu, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, 0x2au, 0x10u, 0x80u, 0xf0u,
    0xdcu, 0x18u, 0x11u, 0x34u, 0x56u, 0x7fu, 0x00u,
    0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u,
    0x07u, 0x08u, 0x09u,
};
static const uint8_t hack16[] = {
    0x02u, 0x01u, 0x06u, 0x14u, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, 0x2au, 0x11u, 0x00u, 0x18u,
    0x42u, 0x18u, 0x42u, 0x11u, 0x22u, 0x34u, 0x12u,
    0x01u, 0x07u, 0x00u,
};
static const uint8_t flood16[] = {
    0x02u, 0x01u, 0x06u, 0x13u, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, 0x2au, 0x12u, 0x00u, 0x40u,
    0x18u, 0x42u, 0x02u, 0x01u, 0x01u, 0x03u, 0xaau,
    0xbbu, 0xccu,
};
static const uint8_t rrep_ack8[] = {
    0x02u, 0x01u, 0x06u, 0x10u, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, 0x2au, 0x09u, 0x80u, 0x11u,
    0x11u, 0x03u, 0x02u, 0x18u, 0x01u, 0x10u,
};

typedef struct conflict_probe {
    unsigned int direct_calls;
    unsigned int remote_calls;
    tavrn_logical_id_t last_direct;
    tavrn_logical_id_t last_remote;
    tavrn_adva_t last_adva;
    int conflict;
} conflict_probe_t;

static int identity_probe(void *context, const tavrn_logical_id_t *logical_id,
                          const tavrn_adva_t *direct_adva_or_null)
{
    conflict_probe_t *probe = (conflict_probe_t *)context;

    if (direct_adva_or_null != NULL) {
        probe->direct_calls++;
        probe->last_direct = *logical_id;
        probe->last_adva = *direct_adva_or_null;
    } else {
        probe->remote_calls++;
        probe->last_remote = *logical_id;
    }
    return probe->conflict;
}

static tavrn_direct_peer_t make_peer(const uint8_t adva[6],
                                     tavrn_identity_width_t width)
{
    tavrn_direct_peer_t peer;

    memset(&peer, 0, sizeof(peer));
    peer.logical_id.width = width;
    peer.logical_id.value = width == TAVRN_IDENTITY_SID8 ? adva[0] :
        (uint16_t)adva[0] | ((uint16_t)adva[1] << 8);
    memcpy(peer.adva.bytes, adva, sizeof(peer.adva.bytes));
    return peer;
}

static tavrn_codec_config_t make_config(const uint8_t local_adva[6],
                                        tavrn_identity_width_t width,
                                        conflict_probe_t *probe)
{
    tavrn_codec_config_t config;

    memset(&config, 0, sizeof(config));
    config.network_id = 0x2au;
    config.local_peer = make_peer(local_adva, width);
    config.identity_conflict = probe == NULL ? NULL : identity_probe;
    config.identity_context = probe;
    return config;
}

static int zero_bytes(const void *value, size_t length)
{
    const uint8_t *bytes = (const uint8_t *)value;
    size_t i;

    for (i = 0u; i < length; i++) {
        if (bytes[i] != 0u) {
            return 0;
        }
    }
    return 1;
}

static void assert_data16_fields(const tavrn_decoded_frame_t *frame)
{
    CHECK("BEARER-01", frame->type == TAVRN_WIRE_DATA);
    CHECK("BEARER-01", frame->network_id == 0x2au);
    CHECK("BEARER-01", memcmp(frame->transmitter.adva.bytes, adva_a, 6u) == 0);
    CHECK("BEARER-01", frame->transmitter.logical_id.width == TAVRN_IDENTITY_SID16);
    CHECK("BEARER-01", frame->transmitter.logical_id.value == 0x4218u);
    CHECK("BEARER-01", frame->detail.data.immediate_receiver.value == 0x4bdcu);
    CHECK("BEARER-01", frame->detail.data.data.origin.value == 0x4218u);
    CHECK("BEARER-01", frame->detail.data.data.final_destination.value == 0x2211u);
    CHECK("BEARER-01", frame->detail.data.data.data_seq == 0x1234u);
    CHECK("BEARER-01", frame->detail.data.data.ttl == 3u &&
                         frame->detail.data.data.hops == 0u);
    CHECK("BEARER-01", frame->detail.data.data.urgent == 1u);
    CHECK("BEARER-01", frame->detail.data.data.app_kind == 0x01u &&
                         frame->detail.data.data.app_source == 0x07u);
    CHECK("BEARER-01", frame->detail.data.data.app_len == 7u);
    CHECK("BEARER-01", memcmp(frame->detail.data.data.app_bytes,
                                &data16[24], 7u) == 0);
}

static void test_bearer_01_wrapper_decode_and_exact_data16(void)
{
    tavrn_codec_config_t config = make_config(adva_b, TAVRN_IDENTITY_SID16, NULL);
    tavrn_decoded_frame_t frame;
    uint8_t encoded[sizeof(data16)];
    size_t encoded_len = 0u;
    uint8_t foreign[sizeof(data16)];
    uint8_t unsupported[sizeof(data16)];
    uint8_t legacy[sizeof(data16)];

    memset(&frame, 0xa5, sizeof(frame));
    CHECK("BEARER-01", tavrn_wire_v2_decode(&config, adva_a, data16,
                                              sizeof(data16), &frame) ==
                           TAVRN_CODEC_OK);
    assert_data16_fields(&frame);
    memset(encoded, 0, sizeof(encoded));
    CHECK("BEARER-01", tavrn_wire_v2_encode(&config, &frame, encoded,
                                              sizeof(encoded), &encoded_len) ==
                           TAVRN_CODEC_OK);
    CHECK("BEARER-01", encoded_len == sizeof(data16));
    CHECK("BEARER-01", memcmp(encoded, data16, sizeof(data16)) == 0);

    memcpy(foreign, data16, sizeof(foreign));
    foreign[10] = 0x2bu;
    CHECK("BEARER-01", tavrn_wire_v2_decode(&config, adva_a, foreign,
                                              sizeof(foreign), &frame) ==
                           TAVRN_CODEC_FOREIGN_NETWORK);
    memcpy(unsupported, data16, sizeof(unsupported));
    unsupported[11] = 0x7eu;
    CHECK("BEARER-01", tavrn_wire_v2_decode(&config, adva_a, unsupported,
                                              sizeof(unsupported), &frame) ==
                           TAVRN_CODEC_UNSUPPORTED_TYPE);
    memcpy(legacy, data16, sizeof(legacy));
    legacy[8] = 0x4du;
    legacy[9] = 0x01u;
    CHECK("BEARER-01", tavrn_wire_v2_decode(&config, adva_a, legacy,
                                              sizeof(legacy), &frame) !=
                           TAVRN_CODEC_OK);
}

static void test_bearer_02_exact_modes_callbacks_and_error_outputs(void)
{
    conflict_probe_t probe;
    tavrn_codec_config_t sid8_config;
    tavrn_codec_config_t sid16_config = make_config(adva_b, TAVRN_IDENTITY_SID16, NULL);
    tavrn_codec_config_t hack_config = make_config(adva_a, TAVRN_IDENTITY_SID16, NULL);
    tavrn_codec_config_t rrep_ack_config;
    tavrn_decoded_frame_t frame;
    tavrn_decoded_frame_t before;
    uint8_t encoded[BLE_ADV_MAX_DATA];
    uint8_t malformed[sizeof(data8_max)];
    uint8_t hack[sizeof(hack16)];
    size_t encoded_len;
    uint8_t status;

    CHECK("BEARER-02", TAVRN_LINK_CONTROL_PDU_MAX == 24u);
    CHECK("BEARER-02", BLE_ADV_MAX_DATA == 31u);
    CHECK("BEARER-02", sizeof(((tavrn_link_data_t *)0)->app_bytes) == 10u);

    memset(&probe, 0, sizeof(probe));
    sid8_config = make_config(adva_b, TAVRN_IDENTITY_SID8, &probe);
    memset(&frame, 0, sizeof(frame));
    CHECK("BEARER-02", tavrn_wire_v2_decode(&sid8_config, adva_a, data8_max,
                                              sizeof(data8_max), &frame) ==
                           TAVRN_CODEC_OK);
    CHECK("BEARER-02", frame.detail.data.immediate_receiver.width ==
                           TAVRN_IDENTITY_SID8 &&
                           frame.detail.data.immediate_receiver.value == 0xdcu);
    CHECK("BEARER-02", frame.detail.data.data.app_kind == 0x7fu &&
                           frame.detail.data.data.app_source == 0u &&
                           frame.detail.data.data.app_len == 10u);
    CHECK("BEARER-02", memcmp(frame.detail.data.data.app_bytes, &data8_max[21],
                                10u) == 0);
    CHECK("BEARER-02", probe.direct_calls == 1u && probe.remote_calls >= 2u);
    CHECK("BEARER-02", probe.last_direct.width == TAVRN_IDENTITY_SID8 &&
                           probe.last_direct.value == 0x18u &&
                           memcmp(probe.last_adva.bytes, adva_a, 6u) == 0);
    CHECK("BEARER-02", probe.last_remote.width == TAVRN_IDENTITY_SID8 &&
                           probe.last_remote.value == 0x11u);
    memset(encoded, 0, sizeof(encoded));
    encoded_len = 0u;
    CHECK("BEARER-02", tavrn_wire_v2_encode(&sid8_config, &frame, encoded,
                                              sizeof(encoded), &encoded_len) ==
                           TAVRN_CODEC_OK);
    CHECK("BEARER-02", encoded_len == sizeof(data8_max) &&
                           memcmp(encoded, data8_max, sizeof(data8_max)) == 0);

    for (status = TAVRN_HACK_ACCEPTED; status <= TAVRN_HACK_REJECTED; status++) {
        memcpy(hack, hack16, sizeof(hack));
        hack[23] = status;
        CHECK("BEARER-02", tavrn_wire_v2_decode(&hack_config, adva_b, hack,
                                                  sizeof(hack), &frame) ==
                               TAVRN_CODEC_OK);
        CHECK("BEARER-02", frame.type == TAVRN_WIRE_HACK &&
                               frame.detail.hack.status == (tavrn_hack_status_t)status &&
                               frame.detail.hack.immediate_receiver.value == 0x4218u &&
                               frame.detail.hack.data_origin.value == 0x4218u &&
                               frame.detail.hack.final_destination.value == 0x2211u &&
                               frame.detail.hack.data_seq == 0x1234u);
        memset(encoded, 0, sizeof(encoded));
        encoded_len = 0u;
        CHECK("BEARER-02", tavrn_wire_v2_encode(&hack_config, &frame, encoded,
                                                  sizeof(encoded), &encoded_len) ==
                               TAVRN_CODEC_OK);
        CHECK("BEARER-02", encoded_len == sizeof(hack) &&
                               memcmp(encoded, hack, sizeof(hack)) == 0);
    }

    CHECK("BEARER-02", tavrn_wire_v2_decode(&sid16_config, adva_a, flood16,
                                              sizeof(flood16), &frame) ==
                           TAVRN_CODEC_OK);
    CHECK("BEARER-02", frame.type == TAVRN_WIRE_FLOOD &&
                           frame.detail.flood.origin.value == 0x4218u &&
                           frame.detail.flood.flood_seq == 0x0102u &&
                           frame.detail.flood.ttl == 4u && frame.detail.flood.hops == 0u &&
                            frame.detail.flood.body_len == 3u &&
                            memcmp(frame.detail.flood.body, "\xaa\xbb\xcc", 3u) == 0);
    memset(encoded, 0, sizeof(encoded));
    encoded_len = 0u;
    CHECK("BEARER-02", tavrn_wire_v2_encode(&sid16_config, &frame, encoded,
                                              sizeof(encoded), &encoded_len) ==
                           TAVRN_CODEC_OK);
    CHECK("BEARER-02", encoded_len == sizeof(flood16) &&
                           memcmp(encoded, flood16, sizeof(flood16)) == 0);
    rrep_ack_config = make_config(adva_c, TAVRN_IDENTITY_SID8, &probe);
    probe.conflict = 0;
    CHECK("BEARER-02", tavrn_wire_v2_decode(&rrep_ack_config, adva_b, rrep_ack8,
                                              sizeof(rrep_ack8), &frame) ==
                           TAVRN_CODEC_OK);
    CHECK("BEARER-02", frame.type == TAVRN_WIRE_E_RREP_ACK &&
                           frame.detail.control.type == TAVRN_WIRE_E_RREP_ACK &&
                           frame.detail.control.pdu_len == 13u &&
                           memcmp(frame.detail.control.pdu, &rrep_ack8[7], 13u) == 0);
    memset(encoded, 0, sizeof(encoded));
    encoded_len = 0u;
    CHECK("BEARER-02", tavrn_wire_v2_encode(&rrep_ack_config, &frame, encoded,
                                              sizeof(encoded), &encoded_len) ==
                           TAVRN_CODEC_OK);
    CHECK("BEARER-02", encoded_len == sizeof(rrep_ack8) &&
                           memcmp(encoded, rrep_ack8, sizeof(rrep_ack8)) == 0);

    memcpy(malformed, data8_max, sizeof(malformed));
    malformed[3] = 0x1au;
    memset(&frame, 0xa5, sizeof(frame));
    CHECK("BEARER-02", tavrn_wire_v2_decode(&sid8_config, adva_a, malformed,
                                              sizeof(malformed), &frame) ==
                           TAVRN_CODEC_MALFORMED_EXACT_LENGTH);
    CHECK("BEARER-02", zero_bytes(&frame, sizeof(frame)));

    probe.conflict = 1;
    memset(&frame, 0xa5, sizeof(frame));
    CHECK("BEARER-02", tavrn_wire_v2_decode(&sid8_config, adva_a, data8_max,
                                              sizeof(data8_max), &frame) ==
                           TAVRN_CODEC_IDENTITY_CONFLICT);
    CHECK("BEARER-02", zero_bytes(&frame, sizeof(frame)));

    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = 0x2au;
    frame.detail.data.immediate_receiver = make_peer(adva_b, TAVRN_IDENTITY_SID16).logical_id;
    frame.detail.data.data.origin = make_peer(adva_a, TAVRN_IDENTITY_SID16).logical_id;
    frame.detail.data.data.final_destination = make_peer(adva_c, TAVRN_IDENTITY_SID16).logical_id;
    frame.detail.data.data.data_seq = 0x1234u;
    frame.detail.data.data.ttl = 3u;
    frame.detail.data.data.urgent = 1u;
    frame.detail.data.data.app_kind = 0x01u;
    frame.detail.data.data.app_source = 0x07u;
    frame.detail.data.data.app_len = 7u;
    memcpy(frame.detail.data.data.app_bytes, &data16[24], 7u);
    before = frame;
    memset(encoded, 0xa5, sizeof(encoded));
    encoded_len = 0x1234u;
    CHECK("BEARER-02", tavrn_wire_v2_encode(&sid16_config, &frame, encoded,
                                              30u, &encoded_len) ==
                           TAVRN_CODEC_OUTPUT_TOO_SMALL);
    CHECK("BEARER-02", encoded[0] == 0xa5u && encoded_len == 0x1234u &&
                           memcmp(&frame, &before, sizeof(frame)) == 0);

    frame.detail.data.data.app_kind = 0x7fu;
    frame.detail.data.data.app_source = 0u;
    frame.detail.data.data.app_len = 8u;
    memset(encoded, 0xa5, sizeof(encoded));
    encoded_len = 0x4321u;
    CHECK("BEARER-02", tavrn_wire_v2_encode(&sid16_config, &frame, encoded,
                                              sizeof(encoded), &encoded_len) ==
                           TAVRN_CODEC_MALFORMED_EXACT_LENGTH);
    CHECK("BEARER-02", encoded[0] == 0xa5u && encoded_len == 0x4321u);

    frame.detail.data.data.app_kind = 0x01u;
    frame.detail.data.data.app_source = 0x07u;
    frame.detail.data.data.app_len = 6u;
    memset(encoded, 0xa5, sizeof(encoded));
    encoded_len = 0x4321u;
    CHECK("BEARER-02", tavrn_wire_v2_encode(&sid16_config, &frame, encoded,
                                              sizeof(encoded), &encoded_len) !=
                           TAVRN_CODEC_OK);
    CHECK("BEARER-02", encoded[0] == 0xa5u && encoded_len == 0x4321u);

    frame.detail.data.immediate_receiver = make_peer(adva_b, TAVRN_IDENTITY_SID8).logical_id;
    frame.detail.data.data.origin = make_peer(adva_a, TAVRN_IDENTITY_SID8).logical_id;
    frame.detail.data.data.final_destination = make_peer(adva_c, TAVRN_IDENTITY_SID8).logical_id;
    frame.detail.data.data.data_seq = 0x5634u;
    frame.detail.data.data.ttl = 15u;
    frame.detail.data.data.app_kind = 0x01u;
    frame.detail.data.data.app_source = 0x07u;
    frame.detail.data.data.app_len = 7u;
    memcpy(frame.detail.data.data.app_bytes, &data16[24], 7u);
    memset(encoded, 0, sizeof(encoded));
    encoded_len = 0u;
    probe.conflict = 0;
    CHECK("BEARER-02", tavrn_wire_v2_encode(&sid8_config, &frame, encoded,
                                              sizeof(encoded), &encoded_len) ==
                           TAVRN_CODEC_OK);
    CHECK("BEARER-02", encoded_len == 28u && encoded[3] == 0x18u &&
                            encoded[11] == TAVRN_WIRE_DATA && encoded[12] == 0xa0u &&
                           encoded[19] == 0x01u && encoded[20] == 0x07u &&
                           memcmp(&encoded[21], &data16[24], 7u) == 0);
}

int main(void)
{
    test_bearer_01_wrapper_decode_and_exact_data16();
    test_bearer_02_exact_modes_callbacks_and_error_outputs();
    if (failures != 0u) {
        printf("tavrn_wire_v2 RED tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_wire_v2 tests passed\n");
    return 0;
}
