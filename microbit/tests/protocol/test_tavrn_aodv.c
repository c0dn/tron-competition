#include "aodv_core.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;
static const char *reported[16];
static unsigned int reported_count;

static int first_for(const char *requirement)
{
    unsigned int i;

    for (i = 0u; i < reported_count; i++) {
        if (strcmp(reported[i], requirement) == 0) {
            return 0;
        }
    }
    reported[reported_count++] = requirement;
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

static const uint8_t adva_a[TAVRN_ADVA_LEN] = {
    0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu,
};
static const uint8_t adva_b[TAVRN_ADVA_LEN] = {
    0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u,
};
static const uint8_t adva_c[TAVRN_ADVA_LEN] = {
    0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0xc1u,
};

/* SID16 AODV_ONLY wire-v2 controls.  The test round-trips every vector through
 * tavrn_wire_v2 rather than retaining an AODV-side codec. */
static const uint8_t rreq16[] = {
    0x02u, 0x01u, 0x06u, 0x14u, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, 0x2au, 0x01u, 0x00u, 0x10u,
    0x18u, 0x42u, 0x01u, 0x10u, 0x11u, 0x22u, 0x03u,
    0x02u, 0x05u, 0x04u,
};
static const uint8_t rrep16[] = {
    0x02u, 0x01u, 0x06u, 0x16u, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, 0x2au, 0x02u, 0x40u, 0x10u,
    0xdcu, 0x4bu, 0x11u, 0x22u, 0x03u, 0x02u, 0x18u,
    0x42u, 0x01u, 0x10u, 0x2cu, 0x81u,
};
static const uint8_t rerr16[] = {
    0x02u, 0x01u, 0x06u, 0x1bu, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, 0x2au, 0x03u, 0x00u, 0xf0u,
    0xdcu, 0x4bu, 0x01u, 0x20u, 0x03u, 0x34u, 0x12u,
    0x07u, 0x06u, 0x11u, 0x22u, 0x03u, 0x02u, 0x18u,
    0x42u, 0x05u, 0x04u,
};
static const uint8_t rrep_ack16[] = {
    0x02u, 0x01u, 0x06u, 0x13u, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, 0x2au, 0x09u, 0x00u, 0x11u,
    0x22u, 0x11u, 0x22u, 0x03u, 0x02u, 0x18u, 0x42u,
    0x01u, 0x10u,
};

static tavrn_direct_peer_t make_peer(const uint8_t adva[TAVRN_ADVA_LEN])
{
    tavrn_direct_peer_t peer;

    memset(&peer, 0, sizeof(peer));
    peer.logical_id.width = TAVRN_IDENTITY_SID16;
    peer.logical_id.value = (uint16_t)adva[0] | ((uint16_t)adva[1] << 8);
    memcpy(peer.adva.bytes, adva, TAVRN_ADVA_LEN);
    return peer;
}

static tavrn_codec_config_t make_codec_config(const uint8_t local_adva[TAVRN_ADVA_LEN])
{
    tavrn_codec_config_t config;

    memset(&config, 0, sizeof(config));
    config.network_id = 0x2au;
    config.local_peer = make_peer(local_adva);
    return config;
}

static aodv_core_config_t make_core_config(const uint8_t local_adva[TAVRN_ADVA_LEN],
                                           uint16_t initial_sequence)
{
    aodv_core_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_peer(local_adva);
    config.network_id = 0x2au;
    config.net_diameter = 15u;
    config.rreq_retries = 2u;
    config.rreq_rate_per_second = 10u;
    config.rerr_rate_per_second = 10u;
    config.request_rrep_ack = 1u;
    config.initial_origin_sequence = initial_sequence;
    config.initial_request_id = 1u;
    config.initial_rerr_sequence = 1u;
    config.node_traversal_ms = 10u;
    config.path_discovery_ms = 600u;
    config.rreq_seen_ms = 10000u;
    config.active_route_ms = 36000u;
    config.pending_data_ms = 5000u;
    config.blacklist_ms = 600u;
    config.rrep_dedupe_ms = 10000u;
    config.rerr_dedupe_ms = 10000u;
    config.rrep_ack_wait_ms = 250u;
    return config;
}

static int init_core(aodv_core_t *core, const uint8_t local_adva[TAVRN_ADVA_LEN],
                     uint16_t initial_sequence)
{
    aodv_core_config_t config = make_core_config(local_adva, initial_sequence);

    return aodv_core_init(core, &config, 0u) == AODV_INIT_OK;
}

static int decode_control(const char *requirement,
                          const tavrn_codec_config_t *config,
                          const uint8_t outer_adva[TAVRN_ADVA_LEN],
                          const uint8_t *vector, size_t vector_len,
                          tavrn_wire_type_t expected_type,
                          tavrn_validated_control_t *control_out)
{
    tavrn_decoded_frame_t frame;
    uint8_t encoded[31];
    size_t encoded_len = 0u;

    memset(&frame, 0, sizeof(frame));
    CHECK(requirement, tavrn_wire_v2_decode(config, outer_adva, vector, vector_len,
                                             &frame) == TAVRN_CODEC_OK);
    CHECK(requirement, frame.type == expected_type &&
                       frame.detail.control.type == expected_type &&
                       frame.detail.control.pdu_len == vector_len - 7u);
    CHECK(requirement, tavrn_wire_v2_encode(config, &frame, encoded, sizeof(encoded),
                                             &encoded_len) == TAVRN_CODEC_OK &&
                       encoded_len == vector_len &&
                       memcmp(encoded, vector, vector_len) == 0);
    if (frame.type != expected_type || frame.detail.control.type != expected_type) {
        return 0;
    }
    *control_out = frame.detail.control;
    return 1;
}

static aodv_control_input_t make_control_input(
    const tavrn_direct_peer_t *transmitter,
    const tavrn_validated_control_t *control)
{
    aodv_control_input_t input;

    memset(&input, 0, sizeof(input));
    input.transmitter = *transmitter;
    input.control = *control;
    return input;
}

static tron_application_data_t make_application(tavrn_logical_id_t destination)
{
    tron_application_data_t data;

    memset(&data, 0, sizeof(data));
    data.final_destination = destination;
    data.app_kind = 0x7fu;
    data.app_len = 1u;
    data.app_bytes[0] = 0xa5u;
    return data;
}

static tavrn_link_data_t make_link_data(tavrn_logical_id_t origin,
                                         tavrn_logical_id_t destination,
                                         uint16_t sequence)
{
    tavrn_link_data_t data;

    memset(&data, 0, sizeof(data));
    data.origin = origin;
    data.final_destination = destination;
    data.data_seq = sequence;
    data.ttl = 15u;
    data.app_kind = 0x7fu;
    data.app_len = 1u;
    data.app_bytes[0] = 0xa5u;
    data.ownership = TAVRN_DATA_TRANSIT;
    return data;
}

typedef struct peer_discard_recorder {
    aodv_data_action_t actions[2];
    uint8_t count;
} peer_discard_recorder_t;

static void record_peer_data_discard(void *context,
                                     const aodv_data_action_t *action)
{
    peer_discard_recorder_t *recorder = context;

    if (recorder != NULL && action != NULL && recorder->count < 2u) {
        recorder->actions[recorder->count++] = *action;
    }
}

static int poll_control(const char *requirement, aodv_core_t *core,
                        aodv_action_type_t expected_type,
                        aodv_action_t *action_out)
{
    CHECK(requirement, aodv_core_poll_action(core, action_out) == AODV_ACTION_POLL_OK &&
                       action_out->type == expected_type);
    return action_out->type == expected_type;
}

static int transmit_control(const char *requirement,
                            const tavrn_codec_config_t *sender_codec,
                            const tavrn_codec_config_t *receiver_codec,
                            const tavrn_direct_peer_t *sender,
                            const aodv_action_t *action,
                            aodv_core_t *receiver, uint32_t now_ms)
{
    tavrn_decoded_frame_t outbound;
    tavrn_decoded_frame_t inbound;
    aodv_control_input_t input;
    aodv_status_t ingest_status;
    tavrn_codec_result_t codec_status;
    uint8_t adv_data[31];
    size_t adv_len = 0u;

    memset(&outbound, 0, sizeof(outbound));
    outbound.type = action->detail.control.control.type;
    outbound.network_id = 0x2au;
    outbound.transmitter = *sender;
    outbound.detail.control = action->detail.control.control;
    codec_status = tavrn_wire_v2_encode(sender_codec, &outbound, adv_data,
                                        sizeof(adv_data), &adv_len);
    CHECK(requirement, codec_status == TAVRN_CODEC_OK);
    if (codec_status != TAVRN_CODEC_OK) {
        return 0;
    }
    memset(&inbound, 0, sizeof(inbound));
    codec_status = tavrn_wire_v2_decode(receiver_codec, sender->adva.bytes, adv_data,
                                        adv_len, &inbound);
    CHECK(requirement, codec_status == TAVRN_CODEC_OK && inbound.type == outbound.type);
    if (codec_status != TAVRN_CODEC_OK || inbound.type != outbound.type) {
        return 0;
    }
    input = make_control_input(sender, &inbound.detail.control);
    ingest_status = aodv_core_ingest_control(receiver, &input, now_ms);
    CHECK(requirement, ingest_status == AODV_STATUS_OK);
    return ingest_status == AODV_STATUS_OK;
}

static uint16_t pdu_u16(const tavrn_validated_control_t *control, uint8_t offset)
{
    return (uint16_t)control->pdu[offset] |
        ((uint16_t)control->pdu[(uint8_t)(offset + 1u)] << 8);
}

static void pdu_set_u16(tavrn_validated_control_t *control, uint8_t offset,
                        uint16_t value)
{
    control->pdu[offset] = (uint8_t)value;
    control->pdu[(uint8_t)(offset + 1u)] = (uint8_t)(value >> 8);
}

static void make_matching_rrep(tavrn_validated_control_t *reply,
                               const tavrn_validated_control_t *template,
                               const tavrn_direct_peer_t *local,
                               tavrn_logical_id_t destination,
                               uint16_t destination_sequence,
                               const aodv_action_t *request_action);
static int install_route_through_next_hop(const char *requirement,
                                          aodv_core_t *core,
                                          const tavrn_direct_peer_t *local,
                                          const tavrn_direct_peer_t *next_hop,
                                          const tavrn_direct_peer_t *precursor,
                                          tavrn_logical_id_t destination,
                                          const tavrn_validated_control_t *template,
                                          uint32_t now_ms);
static int record_reverse_request(const char *requirement, aodv_core_t *core,
                                  const tavrn_direct_peer_t *origin,
                                  const tavrn_validated_control_t *template,
                                  uint16_t request_id, uint32_t now_ms,
                                  aodv_action_t *action_out);

static void test_serial_freshness_and_exact_half(void)
{
    aodv_core_t core;
    tavrn_codec_config_t codec = make_codec_config(adva_b);
    tavrn_validated_control_t request;
    aodv_control_input_t input;
    tavrn_direct_peer_t peer_a = make_peer(adva_a);

    CHECK("SERIAL-02", aodv_serial_is_newer(1u, 0u) &&
                       aodv_serial_is_newer(0u, 0xffffu) &&
                       !aodv_serial_is_newer(0xffffu, 0u));
    CHECK("SERIAL-03", !aodv_serial_is_newer(0x8000u, 0u) &&
                       !aodv_serial_is_newer(0u, 0x8000u));
    if (!init_core(&core, adva_b, 1u) ||
        !decode_control("AODV-02", &codec, adva_a, rreq16, sizeof(rreq16),
                        TAVRN_WIRE_E_RREQ, &request)) {
        return;
    }
    input = make_control_input(&peer_a, &request);
    CHECK("SERIAL-03", aodv_core_ingest_control(&core, &input, 0u) == AODV_STATUS_OK);
    CHECK("SERIAL-03", aodv_core_ingest_control(&core, &input, 1u) ==
                       AODV_STATUS_DUPLICATE);
}

static void test_aodv_01_fixed_routes_precursors_and_capacity(void)
{
    aodv_core_t core;
    tavrn_codec_config_t codec = make_codec_config(adva_b);
    tavrn_validated_control_t reply_template;
    aodv_route_snapshot_t route;
    aodv_action_t action;
    tron_application_data_t application;
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_logical_id_t destination;
    uint8_t i;

    CHECK("AODV-01", TAVRN_AODV_ROUTE_CAPACITY == 16u &&
                       TAVRN_AODV_PRECURSOR_CAPACITY == 8u &&
                       TAVRN_AODV_PENDING_DATA_CAPACITY == 8u);
    if (!init_core(&core, adva_b, 1u) ||
        !decode_control("AODV-03", &codec, adva_c, rrep16, sizeof(rrep16),
                        TAVRN_WIRE_E_RREP, &reply_template)) {
        return;
    }
    for (i = 0u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        destination.width = TAVRN_IDENTITY_SID16;
        destination.value = (uint16_t)(0x1000u + i);
        if (!install_route_through_next_hop("AODV-01", &core, &peer_b, &peer_c,
                                             &peer_a, destination, &reply_template,
                                            (uint32_t)i * 200u)) {
            return;
        }
    }
    destination.width = TAVRN_IDENTITY_SID16;
    destination.value = 0x2000u;
    application = make_application(destination);
    CHECK("AODV-01", aodv_core_submit_application(&core, &application, 4000u) ==
                           AODV_STATUS_BUSY &&
                       aodv_core_poll_action(&core, &action) ==
                           AODV_ACTION_POLL_EMPTY);
    route.destination.width = TAVRN_IDENTITY_SID16;
    route.destination.value = 0x1000u;
    CHECK("AODV-01", aodv_core_route_snapshot(&core, &route.destination, &route) ==
                       AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_VALID);
    CHECK("AODV-01", aodv_core_route_snapshot(&core, &route.destination, &route) ==
                       AODV_ROUTE_QUERY_FOUND && route.precursor_count == 1u &&
                       route.precursors[0].value == peer_a.logical_id.value);
}

static void test_aodv_02_rreq_reverse_route_dedupe_and_loop(void)
{
    aodv_core_t core;
    tavrn_codec_config_t codec = make_codec_config(adva_b);
    tavrn_validated_control_t request;
    aodv_control_input_t input;
    aodv_action_t action;
    aodv_route_snapshot_t route;
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);

    if (!init_core(&core, adva_b, 1u) ||
        !decode_control("AODV-02", &codec, adva_a, rreq16, sizeof(rreq16),
                        TAVRN_WIRE_E_RREQ, &request)) {
        return;
    }
    input = make_control_input(&peer_a, &request);
    CHECK("AODV-02", aodv_core_ingest_control(&core, &input, 0u) == AODV_STATUS_OK);
    CHECK("AODV-02", aodv_core_route_snapshot(&core, &peer_a.logical_id, &route) ==
                       AODV_ROUTE_QUERY_FOUND && route.next_hop.logical_id.value ==
                           peer_a.logical_id.value && route.hop_count == 1u);
    CHECK("AODV-02", poll_control("AODV-02", &core, AODV_ACTION_SEND_RREQ, &action) &&
                       action.detail.control.control.pdu[6] == 0x01u);
    CHECK("AODV-02", aodv_core_ingest_control(&core, &input, 1u) ==
                       AODV_STATUS_DUPLICATE);
    input.transmitter = peer_b;
    CHECK("AODV-02", aodv_core_ingest_control(&core, &input, 2u) ==
                       AODV_STATUS_LOOP_PREVENTED);
}

static void test_aodv_03_rrep_freshness_tie_and_forward_route(void)
{
    aodv_core_t core;
    tavrn_codec_config_t codec = make_codec_config(adva_b);
    tavrn_validated_control_t request;
    tavrn_validated_control_t reply;
    tavrn_validated_control_t reply_template;
    aodv_control_input_t input;
    aodv_route_snapshot_t route;
    aodv_action_t action;
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);

    if (!init_core(&core, adva_b, 1u) ||
        !decode_control("AODV-02", &codec, adva_a, rreq16, sizeof(rreq16),
                        TAVRN_WIRE_E_RREQ, &request) ||
        !decode_control("AODV-03", &codec, adva_c, rrep16, sizeof(rrep16),
                        TAVRN_WIRE_E_RREP, &reply_template)) {
        return;
    }
    if (!record_reverse_request("AODV-03", &core, &peer_a, &request, 0x1001u, 0u,
                                &action)) {
        return;
    }
    make_matching_rrep(&reply, &reply_template, &peer_b, peer_c.logical_id, 0x0203u,
                       &action);
    reply.pdu[6] = 0x41u;
    input = make_control_input(&peer_c, &reply);
    CHECK("AODV-03", aodv_core_ingest_control(&core, &input, 0u) == AODV_STATUS_OK);
    if (!poll_control("AODV-03", &core, AODV_ACTION_FORWARD_RREP, &action)) {
        return;
    }
    CHECK("AODV-03", aodv_core_route_snapshot(&core, &peer_c.logical_id, &route) ==
                       AODV_ROUTE_QUERY_FOUND && route.destination_sequence == 0x0203u &&
                       route.hop_count == 2u);
    if (!record_reverse_request("AODV-03", &core, &peer_a, &request, 0x1002u, 1u,
                                &action)) {
        return;
    }
    make_matching_rrep(&reply, &reply_template, &peer_b, peer_c.logical_id, 0x0203u,
                       &action);
    reply.pdu[6] = 0x50u;
    input = make_control_input(&peer_c, &reply);
    CHECK("AODV-03", aodv_core_ingest_control(&core, &input, 1u) == AODV_STATUS_OK);
    if (!poll_control("AODV-03", &core, AODV_ACTION_FORWARD_RREP, &action)) {
        return;
    }
    CHECK("AODV-03", aodv_core_route_snapshot(&core, &peer_c.logical_id, &route) ==
                       AODV_ROUTE_QUERY_FOUND && route.hop_count == 1u);
    if (!record_reverse_request("AODV-03", &core, &peer_a, &request, 0x1003u, 2u,
                                &action)) {
        return;
    }
    make_matching_rrep(&reply, &reply_template, &peer_b, peer_c.logical_id, 0x0302u,
                       &action);
    reply.pdu[6] = 0x41u;
    input = make_control_input(&peer_c, &reply);
    CHECK("AODV-03", aodv_core_ingest_control(&core, &input, 2u) == AODV_STATUS_OK);
    if (!poll_control("AODV-03", &core, AODV_ACTION_FORWARD_RREP, &action)) {
        return;
    }
    CHECK("AODV-03", aodv_core_route_snapshot(&core, &peer_c.logical_id, &route) ==
                       AODV_ROUTE_QUERY_FOUND && route.destination_sequence == 0x0302u &&
                       route.hop_count == 2u);
    if (!record_reverse_request("AODV-03", &core, &peer_a, &request, 0x1004u, 3u,
                                &action)) {
        return;
    }
    make_matching_rrep(&reply, &reply_template, &peer_b, peer_c.logical_id, 0x0203u,
                       &action);
    input = make_control_input(&peer_c, &reply);
    CHECK("AODV-03", aodv_core_ingest_control(&core, &input, 3u) == AODV_STATUS_OK);
    if (!poll_control("AODV-03", &core, AODV_ACTION_FORWARD_RREP, &action)) {
        return;
    }
    CHECK("AODV-03", aodv_core_route_snapshot(&core, &peer_c.logical_id, &route) ==
                       AODV_ROUTE_QUERY_FOUND && route.destination_sequence == 0x0302u);
}

static void test_aodv_03_overheard_rrep_is_unmatched(void)
{
    aodv_core_t core;
    tavrn_codec_config_t codec = make_codec_config(adva_b);
    tavrn_validated_control_t reply;
    aodv_control_input_t input;
    aodv_action_t action;
    aodv_route_snapshot_t route;
    aodv_counters_t counters_before;
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);

    if (!init_core(&core, adva_b, 1u) ||
        !decode_control("AODV-03", &codec, adva_c, rrep16, sizeof(rrep16),
                        TAVRN_WIRE_E_RREP, &reply)) {
        return;
    }
    input = make_control_input(&peer_c, &reply);
    counters_before = *aodv_core_counters(&core);
    pdu_set_u16(&input.control, 7u, 0u);
    CHECK("AODV-03", aodv_core_ingest_control(&core, &input, 1u) ==
                         AODV_STATUS_INVALID);
    pdu_set_u16(&input.control, 7u, 0xffffu);
    CHECK("AODV-03", aodv_core_ingest_control(&core, &input, 1u) ==
                         AODV_STATUS_INVALID);
    pdu_set_u16(&input.control, 7u, peer_a.logical_id.value);
    CHECK("AODV-03", pdu_u16(&input.control, 7u) != peer_b.logical_id.value &&
                          aodv_core_ingest_control(&core, &input, 1u) ==
                             AODV_STATUS_UNMATCHED &&
                         aodv_core_poll_action(&core, &action) ==
                             AODV_ACTION_POLL_EMPTY &&
                         aodv_core_route_snapshot(&core, &peer_c.logical_id, &route) ==
                             AODV_ROUTE_QUERY_NOT_FOUND &&
                         aodv_core_route_snapshot(&core, &peer_a.logical_id, &route) ==
                             AODV_ROUTE_QUERY_NOT_FOUND &&
                         memcmp(aodv_core_counters(&core), &counters_before,
                                sizeof(counters_before)) == 0);
}

static void test_aodv_04_expanding_rings(void)
{
    aodv_core_t core;
    tron_application_data_t data = make_application(make_peer(adva_c).logical_id);
    aodv_action_t action;
    uint8_t expected_ttl[] = { 1u, 3u, 5u, 7u, 15u };
    uint16_t previous_request = 0u;
    uint8_t i;

    if (!init_core(&core, adva_a, 1u)) {
        return;
    }
    CHECK("AODV-04", aodv_core_submit_application(&core, &data, 0u) ==
                       AODV_STATUS_QUEUED);
    for (i = 0u; i < sizeof(expected_ttl); i++) {
        CHECK("AODV-04", poll_control("AODV-04", &core, AODV_ACTION_SEND_RREQ,
                                       &action) &&
                           (action.detail.control.control.pdu[6] >> 4) == expected_ttl[i] &&
                           (i == 0u || pdu_u16(&action.detail.control.control, 9u) !=
                            previous_request));
        previous_request = pdu_u16(&action.detail.control.control, 9u);
        CHECK("AODV-04", aodv_core_tick(&core, (uint32_t)(i + 1u) * 600u) ==
                           AODV_STATUS_OK);
    }
    CHECK("AODV-04", core.config.net_diameter == 15u &&
                       core.config.rreq_rate_per_second == 10u);
}

static void test_aodv_05_two_node_pending_data_release(void)
{
    aodv_core_t node_a;
    aodv_core_t node_b;
    tavrn_codec_config_t codec_a = make_codec_config(adva_a);
    tavrn_codec_config_t codec_b = make_codec_config(adva_b);
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tron_application_data_t data = make_application(peer_b.logical_id);
    aodv_data_input_t data_input;
    aodv_action_t action;

    if (!init_core(&node_a, adva_a, 1u) || !init_core(&node_b, adva_b, 0x0203u)) {
        return;
    }
    CHECK("AODV-05", aodv_core_submit_application(&node_a, &data, 0u) ==
                       AODV_STATUS_QUEUED);
    if (!poll_control("AODV-05", &node_a, AODV_ACTION_SEND_RREQ, &action) ||
        !transmit_control("AODV-02", &codec_a, &codec_b, &peer_a, &action, &node_b,
                          1u) ||
        !poll_control("AODV-03", &node_b, AODV_ACTION_SEND_RREP, &action)) {
        return;
    }
    CHECK("AODV-07", aodv_core_mark_action_sent(&node_b, action.detail.control.token,
                                                   1u) == AODV_STATUS_OK);
    if (!transmit_control("AODV-03", &codec_b, &codec_a, &peer_b, &action, &node_a,
                          2u) ||
        !poll_control("AODV-07", &node_a, AODV_ACTION_SEND_RREP_ACK, &action) ||
        !transmit_control("AODV-07", &codec_a, &codec_b, &peer_a, &action, &node_b,
                          3u)) {
        return;
    }
    CHECK("AODV-05", aodv_core_poll_action(&node_a, &action) == AODV_ACTION_POLL_OK &&
                       action.type == AODV_ACTION_FORWARD_DATA &&
                       action.detail.data.next_hop.logical_id.value == peer_b.logical_id.value);
    memset(&data_input, 0, sizeof(data_input));
    data_input.transmitter = peer_a;
    data_input.data = action.detail.data.data;
    CHECK("AODV-05", aodv_core_ingest_data(&node_b, &data_input, 4u) == AODV_STATUS_OK &&
                       aodv_core_poll_action(&node_b, &action) == AODV_ACTION_POLL_OK &&
                       action.type == AODV_ACTION_DELIVER_DATA);
}

static void make_matching_rrep(tavrn_validated_control_t *reply,
                               const tavrn_validated_control_t *template,
                               const tavrn_direct_peer_t *local,
                               tavrn_logical_id_t destination,
                               uint16_t destination_sequence,
                               const aodv_action_t *request_action)
{
    *reply = *template;
    reply->pdu[5] &= (uint8_t)~0x40u;
    pdu_set_u16(reply, 7u, local->logical_id.value);
    pdu_set_u16(reply, 9u, destination.value);
    pdu_set_u16(reply, 11u, destination_sequence);
    pdu_set_u16(reply, 13u,
                pdu_u16(&request_action->detail.control.control, 7u));
    pdu_set_u16(reply, 15u, pdu_u16(&request_action->detail.control.control, 9u));
}

static int install_route_through_next_hop(const char *requirement,
                                          aodv_core_t *core,
                                          const tavrn_direct_peer_t *local,
                                          const tavrn_direct_peer_t *next_hop,
                                          const tavrn_direct_peer_t *precursor,
                                          tavrn_logical_id_t destination,
                                          const tavrn_validated_control_t *template,
                                          uint32_t now_ms)
{
    tron_application_data_t application = make_application(destination);
    tavrn_validated_control_t reply = *template;
    aodv_control_input_t input;
    aodv_data_input_t data_input;
    aodv_action_t action;

    CHECK(requirement, aodv_core_submit_application(core, &application, now_ms) ==
                       AODV_STATUS_QUEUED);
    if (!poll_control(requirement, core, AODV_ACTION_SEND_RREQ, &action)) {
        return 0;
    }
    make_matching_rrep(&reply, template, local, destination,
                       (uint16_t)(0x0100u + destination.value), &action);
    input = make_control_input(next_hop, &reply);
    CHECK(requirement, aodv_core_ingest_control(core, &input, now_ms + 1u) ==
                       AODV_STATUS_OK);
    if (!poll_control(requirement, core, AODV_ACTION_FORWARD_DATA, &action)) {
        return 0;
    }
    memset(&data_input, 0, sizeof(data_input));
    data_input.transmitter = *precursor;
    data_input.data = make_link_data(precursor->logical_id, destination,
                                     (uint16_t)(now_ms + 1u));
    CHECK(requirement, aodv_core_ingest_data(core, &data_input, now_ms + 2u) ==
                       AODV_STATUS_OK);
    return poll_control(requirement, core, AODV_ACTION_FORWARD_DATA, &action);
}

static int record_reverse_request(const char *requirement, aodv_core_t *core,
                                  const tavrn_direct_peer_t *origin,
                                  const tavrn_validated_control_t *template,
                                  uint16_t request_id, uint32_t now_ms,
                                  aodv_action_t *action_out)
{
    tavrn_validated_control_t request = *template;
    aodv_control_input_t input;

    pdu_set_u16(&request, 9u, request_id);
    input = make_control_input(origin, &request);
    CHECK(requirement, aodv_core_ingest_control(core, &input, now_ms) == AODV_STATUS_OK);
    return poll_control(requirement, core, AODV_ACTION_SEND_RREQ, action_out);
}

static void check_rerr_segment(const aodv_action_t *action, uint16_t first,
                               uint8_t expected_count)
{
    uint8_t i;

    CHECK("AODV-06", action->detail.control.control.type == TAVRN_WIRE_E_RERR &&
                       action->detail.control.control.pdu[11] == expected_count);
    for (i = 0u; i < expected_count; i++) {
        CHECK("AODV-06", pdu_u16(&action->detail.control.control,
                                  (uint8_t)(12u + 4u * i)) ==
                           (uint16_t)(first + i));
    }
}

static void test_aodv_06_all_routes_rerr_segments_and_backpressure(void)
{
    aodv_core_t core;
    aodv_core_t backpressure_core;
    aodv_core_t expiry_core;
    tavrn_codec_config_t codec = make_codec_config(adva_b);
    tavrn_validated_control_t reply;
    tavrn_validated_control_t rerr_vector;
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    aodv_route_snapshot_t route;
    aodv_action_t action;
    tron_application_data_t application;
    tavrn_logical_id_t destination;
    uint8_t i;

    CHECK("AODV-06", TAVRN_AODV_RERR_SID16_PER_ACTION == 3u &&
                       TAVRN_AODV_RERR_SID8_PER_ACTION == 4u &&
                       TAVRN_AODV_ACTION_CAPACITY == 8u);
    if (!decode_control("AODV-06", &codec, adva_c, rrep16, sizeof(rrep16),
                        TAVRN_WIRE_E_RREP, &reply) ||
        !decode_control("AODV-06", &codec, adva_b, rerr16, sizeof(rerr16),
                        TAVRN_WIRE_E_RERR, &rerr_vector) ||
        !init_core(&core, adva_b, 1u)) {
        return;
    }
    for (i = 0u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        destination.width = TAVRN_IDENTITY_SID16;
        destination.value = (uint16_t)(0x1000u + i);
        if (!install_route_through_next_hop("AODV-06", &core, &peer_b, &peer_c,
                                            &peer_a,
                                            destination, &reply,
                                            (uint32_t)i * 200u)) {
            return;
        }
    }
    CHECK("AODV-06", aodv_core_report_link_failure(&core, &peer_c, NULL,
                                                     AODV_LINK_FAILURE_IMMEDIATE_RERR,
                                                     4000u) == AODV_FAILURE_OK);
    for (i = 0u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        destination.width = TAVRN_IDENTITY_SID16;
        destination.value = (uint16_t)(0x1000u + i);
        CHECK("AODV-06", aodv_core_route_snapshot(&core, &destination, &route) ==
                           AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_INVALID);
    }
    for (i = 0u; i < 5u; i++) {
        CHECK("AODV-06", aodv_core_tick(&core, 4000u + (uint32_t)i * 100u) ==
                           AODV_STATUS_OK);
        if (!poll_control("AODV-06", &core, AODV_ACTION_SEND_RERR, &action)) {
            return;
        }
        check_rerr_segment(&action, (uint16_t)(0x1000u + 3u * i), 3u);
    }
    CHECK("AODV-06", aodv_core_tick(&core, 4500u) == AODV_STATUS_OK);
    if (!poll_control("AODV-06", &core, AODV_ACTION_SEND_RERR, &action)) {
        return;
    }
    check_rerr_segment(&action, 0x100fu, 1u);

    if (!init_core(&expiry_core, adva_b, 1u) ||
        !install_route_through_next_hop("AODV-06", &expiry_core, &peer_b, &peer_c,
                                        &peer_a, peer_c.logical_id, &reply, 0u)) {
        return;
    }
    CHECK("AODV-06", aodv_core_tick(&expiry_core, 36000u) == AODV_STATUS_OK &&
                       aodv_core_route_snapshot(&expiry_core, &peer_c.logical_id, &route) ==
                           AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_INVALID);

    if (!init_core(&backpressure_core, adva_b, 1u)) {
        return;
    }
    for (i = 0u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        destination.width = TAVRN_IDENTITY_SID16;
        destination.value = (uint16_t)(0x1000u + i);
        if (!install_route_through_next_hop("AODV-06", &backpressure_core, &peer_b,
                                            &peer_c,
                                            &peer_a, destination, &reply,
                                            (uint32_t)i * 200u)) {
            return;
        }
    }
    for (i = 0u; i < TAVRN_AODV_ACTION_CAPACITY; i++) {
        destination.width = TAVRN_IDENTITY_SID16;
        destination.value = (uint16_t)(0x1000u + i);
        application = make_application(destination);
        CHECK("AODV-06", aodv_core_submit_application(&backpressure_core,
                                                          &application,
                                                          10000u + (uint32_t)i * 1000u) ==
                            AODV_STATUS_OK);
    }
    CHECK("AODV-06", aodv_core_report_link_failure(&backpressure_core, &peer_c, NULL,
                                                     AODV_LINK_FAILURE_IMMEDIATE_RERR,
                                                     20000u) == AODV_FAILURE_BUSY);
    destination.width = TAVRN_IDENTITY_SID16;
    destination.value = 0x1000u;
    CHECK("AODV-06", aodv_core_route_snapshot(&backpressure_core, &destination,
                                                &route) == AODV_ROUTE_QUERY_FOUND &&
                       route.state == AODV_ROUTE_VALID);
}

static void test_aodv_07_exact_ack_and_scoped_timeout(void)
{
    aodv_core_t success_core;
    aodv_core_t timeout_core;
    tavrn_codec_config_t codec_c = make_codec_config(adva_c);
    tavrn_validated_control_t request;
    tavrn_validated_control_t ack;
    aodv_control_input_t input;
    aodv_action_t action;
    tavrn_direct_peer_t peer_b = make_peer(adva_b);

    if (!decode_control("AODV-07", &codec_c, adva_b, rreq16, sizeof(rreq16),
                        TAVRN_WIRE_E_RREQ, &request) ||
        !decode_control("AODV-07", &codec_c, adva_b, rrep_ack16,
                        sizeof(rrep_ack16), TAVRN_WIRE_E_RREP_ACK, &ack) ||
        !init_core(&success_core, adva_c, 0x0203u)) {
        return;
    }
    input = make_control_input(&peer_b, &request);
    CHECK("AODV-07", aodv_core_ingest_control(&success_core, &input, 0u) ==
                       AODV_STATUS_OK);
    if (!poll_control("AODV-07", &success_core, AODV_ACTION_SEND_RREP, &action)) {
        return;
    }
    CHECK("AODV-07", action.detail.control.token != 0u &&
                       aodv_core_mark_action_sent(&success_core,
                                                  action.detail.control.token, 1u) ==
                           AODV_STATUS_OK);
    ack.pdu[10] ^= 0x01u;
    input = make_control_input(&peer_b, &ack);
    CHECK("AODV-07", aodv_core_ingest_control(&success_core, &input, 2u) ==
                       AODV_STATUS_UNMATCHED);
    ack.pdu[10] ^= 0x01u;
    input.control = ack;
    CHECK("AODV-07", aodv_core_ingest_control(&success_core, &input, 3u) ==
                       AODV_STATUS_OK);
    CHECK("AODV-07", aodv_core_tick(&success_core, 251u) == AODV_STATUS_OK &&
                       aodv_core_poll_action(&success_core, &action) ==
                           AODV_ACTION_POLL_EMPTY);

    if (!init_core(&timeout_core, adva_c, 0x0203u)) {
        return;
    }
    input = make_control_input(&peer_b, &request);
    CHECK("AODV-07", aodv_core_ingest_control(&timeout_core, &input, 0u) ==
                       AODV_STATUS_OK);
    if (!poll_control("AODV-07", &timeout_core, AODV_ACTION_SEND_RREP, &action)) {
        return;
    }
    CHECK("AODV-07", aodv_core_mark_action_sent(&timeout_core,
                                                   action.detail.control.token, 1u) ==
                       AODV_STATUS_OK);
    CHECK("AODV-07", aodv_core_tick(&timeout_core, 251u) == AODV_STATUS_OK &&
                       aodv_core_poll_action(&timeout_core, &action) ==
                           AODV_ACTION_POLL_OK &&
                       action.type == AODV_ACTION_BLACKLIST_NEIGHBOR &&
                       action.detail.failure.peer.logical_id.value == peer_b.logical_id.value &&
                       action.type != AODV_ACTION_SEND_RERR &&
                       aodv_core_poll_action(&timeout_core, &action) ==
                           AODV_ACTION_POLL_EMPTY);
}

static void test_serial_04_rejoin_invalidation_has_route_and_precursor(void)
{
    aodv_core_t core;
    tavrn_codec_config_t codec = make_codec_config(adva_a);
    tavrn_validated_control_t reply;
    aodv_action_t action;
    aodv_route_snapshot_t route;
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_logical_id_t destination;

    if (!init_core(&core, adva_a, 1u) ||
        !decode_control("SERIAL-04", &codec, adva_b, rrep16, sizeof(rrep16),
                        TAVRN_WIRE_E_RREP, &reply)) {
        return;
    }
    destination.width = TAVRN_IDENTITY_SID16;
    destination.value = 0x3001u;
    if (!install_route_through_next_hop("SERIAL-04", &core, &peer_a, &peer_b,
                                        &peer_c, destination, &reply, 0u)) {
        return;
    }
    /* tavrn_router admits a new {AdvA, boot_nonce} once, then calls this
     * AODV seam.  The core must invalidate the established route and notify
     * its recorded precursor; it does not own HELLO equality dedupe. */
    CHECK("SERIAL-04", aodv_core_report_link_failure(&core, &peer_b, NULL,
                                                       AODV_LINK_FAILURE_IMMEDIATE_RERR,
                                                       2u) == AODV_FAILURE_OK);
    CHECK("SERIAL-04", aodv_core_route_snapshot(&core, &destination, &route) ==
                       AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_INVALID);
    CHECK("SERIAL-04", poll_control("SERIAL-04", &core, AODV_ACTION_SEND_RERR,
                                     &action));
}

static void test_deterministic_three_node_chain(void)
{
    aodv_core_t node_a;
    aodv_core_t node_b;
    aodv_core_t node_c;
    tavrn_codec_config_t codec_a = make_codec_config(adva_a);
    tavrn_codec_config_t codec_b = make_codec_config(adva_b);
    tavrn_codec_config_t codec_c = make_codec_config(adva_c);
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tron_application_data_t data = make_application(peer_c.logical_id);
    aodv_data_input_t data_input;
    aodv_action_t action;

    if (!init_core(&node_a, adva_a, 1u) || !init_core(&node_b, adva_b, 1u) ||
        !init_core(&node_c, adva_c, 0x0203u)) {
        return;
    }
    CHECK("AODV-05", aodv_core_submit_application(&node_a, &data, 0u) ==
                       AODV_STATUS_QUEUED);
    if (!poll_control("AODV-02", &node_a, AODV_ACTION_SEND_RREQ, &action) ||
        !transmit_control("AODV-02", &codec_a, &codec_b, &peer_a, &action, &node_b,
                          1u) ||
        !poll_control("AODV-02", &node_b, AODV_ACTION_SEND_RREQ, &action) ||
        !transmit_control("AODV-02", &codec_b, &codec_c, &peer_b, &action, &node_c,
                          2u) ||
        !poll_control("AODV-03", &node_c, AODV_ACTION_SEND_RREP, &action)) {
        return;
    }
    CHECK("AODV-07", aodv_core_mark_action_sent(&node_c, action.detail.control.token,
                                                   2u) == AODV_STATUS_OK);
    if (!transmit_control("AODV-03", &codec_c, &codec_b, &peer_c, &action, &node_b,
                          3u) ||
        !poll_control("AODV-07", &node_b, AODV_ACTION_SEND_RREP_ACK, &action) ||
        !transmit_control("AODV-07", &codec_b, &codec_c, &peer_b, &action, &node_c,
                          4u) ||
        !poll_control("AODV-03", &node_b, AODV_ACTION_FORWARD_RREP, &action)) {
        return;
    }
    CHECK("AODV-07", aodv_core_mark_action_sent(&node_b, action.detail.control.token,
                                                   4u) == AODV_STATUS_OK);
    if (!transmit_control("AODV-03", &codec_b, &codec_a, &peer_b, &action, &node_a,
                          5u) ||
        !poll_control("AODV-07", &node_a, AODV_ACTION_SEND_RREP_ACK, &action) ||
        !transmit_control("AODV-07", &codec_a, &codec_b, &peer_a, &action, &node_b,
                          6u)) {
        return;
    }
    CHECK("AODV-05", aodv_core_poll_action(&node_a, &action) == AODV_ACTION_POLL_OK &&
                       action.type == AODV_ACTION_FORWARD_DATA &&
                       action.detail.data.next_hop.logical_id.value == peer_b.logical_id.value);
    memset(&data_input, 0, sizeof(data_input));
    data_input.transmitter = peer_a;
    data_input.data = action.detail.data.data;
    CHECK("AODV-05", aodv_core_ingest_data(&node_b, &data_input, 7u) == AODV_STATUS_OK &&
                       aodv_core_poll_action(&node_b, &action) == AODV_ACTION_POLL_OK &&
                       action.type == AODV_ACTION_FORWARD_DATA &&
                       action.detail.data.next_hop.logical_id.value == peer_c.logical_id.value);
    data_input.transmitter = peer_b;
    data_input.data = action.detail.data.data;
    CHECK("AODV-05", aodv_core_ingest_data(&node_c, &data_input, 8u) == AODV_STATUS_OK &&
                        aodv_core_poll_action(&node_c, &action) == AODV_ACTION_POLL_OK &&
                        action.type == AODV_ACTION_DELIVER_DATA);
}

static void drain_actions(aodv_core_t *core)
{
    aodv_action_t action;

    while (aodv_core_poll_action(core, &action) == AODV_ACTION_POLL_OK) {
    }
}

static void fill_action_queue_with_discoveries(aodv_core_t *core, uint16_t first,
                                                uint32_t now_ms)
{
    tron_application_data_t application;
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_ACTION_CAPACITY; i++) {
        tavrn_logical_id_t destination;

        destination.width = TAVRN_IDENTITY_SID16;
        destination.value = (uint16_t)(first + i);
        application = make_application(destination);
        CHECK("AODV-05", aodv_core_submit_application(core, &application, now_ms) ==
                            AODV_STATUS_QUEUED);
    }
}

static void test_aodv_data_probe_hop_bounds_and_commit(void)
{
    aodv_core_t core;
    aodv_core_t busy_core;
    aodv_core_t expired_core;
    aodv_core_t before;
    tavrn_codec_config_t codec = make_codec_config(adva_b);
    tavrn_validated_control_t reply_template;
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_logical_id_t destination = peer_c.logical_id;
    aodv_data_input_t input;
    aodv_action_t action;
    aodv_route_snapshot_t route;

    if (!init_core(&core, adva_b, 1u) ||
        !decode_control("AODV-05", &codec, adva_c, rrep16, sizeof(rrep16),
                        TAVRN_WIRE_E_RREP, &reply_template) ||
        !install_route_through_next_hop("AODV-05", &core, &peer_b, &peer_c,
                                        &peer_a, destination, &reply_template, 0u)) {
        return;
    }
    memset(&input, 0, sizeof(input));
    input.transmitter = peer_a;
    input.data = make_link_data(peer_a.logical_id, destination, 0x7d01u);
    input.data.ttl = 1u;
    input.data.hops = 14u;
    before = core;
    CHECK("AODV-05", aodv_core_probe_data(&core, &input, 3u) == AODV_STATUS_OK &&
                       memcmp(&core, &before, sizeof(core)) == 0);
    CHECK("AODV-05", aodv_core_ingest_data(&core, &input, 3u) == AODV_STATUS_OK &&
                       poll_control("AODV-05", &core, AODV_ACTION_FORWARD_DATA,
                                    &action) && action.detail.data.data.hops == 15u);

    input.data.hops = 15u;
    before = core;
    CHECK("AODV-02", aodv_core_probe_data(&core, &input, 4u) == AODV_STATUS_INVALID &&
                       memcmp(&core, &before, sizeof(core)) == 0 &&
                       aodv_core_ingest_data(&core, &input, 4u) == AODV_STATUS_INVALID &&
                       aodv_core_poll_action(&core, &action) == AODV_ACTION_POLL_EMPTY);

    input.data.final_destination = peer_b.logical_id;
    input.data.ttl = 0u;
    before = core;
    CHECK("AODV-05", aodv_core_probe_data(&core, &input, 5u) == AODV_STATUS_OK &&
                       memcmp(&core, &before, sizeof(core)) == 0 &&
                       aodv_core_ingest_data(&core, &input, 5u) == AODV_STATUS_OK &&
                       poll_control("AODV-05", &core, AODV_ACTION_DELIVER_DATA,
                                    &action) && action.detail.data.data.hops == 15u);

    if (!init_core(&expired_core, adva_b, 1u) ||
        !install_route_through_next_hop("AODV-05", &expired_core, &peer_b, &peer_c,
                                        &peer_a, destination, &reply_template, 0u)) {
        return;
    }
    memset(&input, 0, sizeof(input));
    input.transmitter = peer_a;
    input.data = make_link_data(peer_a.logical_id, destination, 0x7d02u);
    input.data.ttl = 1u;
    input.data.hops = 14u;
    before = expired_core;
    CHECK("AODV-05", aodv_core_probe_data(&expired_core, &input, 36001u) ==
                           AODV_STATUS_NOT_FOUND &&
                       memcmp(&expired_core, &before, sizeof(expired_core)) == 0 &&
                       aodv_core_counters(&expired_core)->route_expired == 0u);
    CHECK("AODV-05", aodv_core_ingest_data(&expired_core, &input, 36001u) ==
                           AODV_STATUS_NOT_FOUND &&
                       aodv_core_counters(&expired_core)->route_expired == 1u &&
                       aodv_core_route_snapshot(&expired_core, &destination, &route) ==
                           AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_INVALID);

    if (!init_core(&busy_core, adva_b, 1u)) {
        return;
    }
    fill_action_queue_with_discoveries(&busy_core, 0x7800u, 0u);
    memset(&input, 0, sizeof(input));
    input.transmitter = peer_a;
    input.data = make_link_data(peer_a.logical_id, peer_b.logical_id, 0x7d03u);
    input.data.ttl = 0u;
    input.data.hops = 15u;
    before = busy_core;
    CHECK("AODV-05", aodv_core_probe_data(&busy_core, &input, 1u) ==
                           AODV_STATUS_BUSY &&
                       memcmp(&busy_core, &before, sizeof(busy_core)) == 0 &&
                       aodv_core_ingest_data(&busy_core, &input, 1u) ==
                           AODV_STATUS_BUSY &&
                       aodv_core_counters(&busy_core)->action_backpressure == 1u);
}

static void test_blocker_route_freshness_after_invalidation(void)
{
    aodv_core_t core;
    tavrn_codec_config_t codec = make_codec_config(adva_b);
    tavrn_validated_control_t request;
    tavrn_validated_control_t reply_template;
    tavrn_validated_control_t reply;
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_logical_id_t destination = make_peer(adva_c).logical_id;
    aodv_action_t action;
    aodv_control_input_t input;
    aodv_route_snapshot_t route;
    uint16_t invalid_sequence = (uint16_t)(0x0100u + destination.value + 1u);

    if (!init_core(&core, adva_b, 1u) ||
        !decode_control("AODV-03", &codec, adva_a, rreq16, sizeof(rreq16),
                        TAVRN_WIRE_E_RREQ, &request) ||
        !decode_control("AODV-03", &codec, adva_c, rrep16, sizeof(rrep16),
                        TAVRN_WIRE_E_RREP, &reply_template) ||
        !install_route_through_next_hop("AODV-03", &core, &peer_b, &peer_c, &peer_a,
                                        destination, &reply_template, 0u)) {
        return;
    }
    CHECK("AODV-03", aodv_core_report_link_failure(&core, &peer_c, NULL,
                                                     AODV_LINK_FAILURE_IMMEDIATE_RERR,
                                                     10u) == AODV_FAILURE_OK);
    drain_actions(&core);
    if (!record_reverse_request("AODV-03", &core, &peer_a, &request, 0x2001u, 11u,
                                &action)) {
        return;
    }
    make_matching_rrep(&reply, &reply_template, &peer_b, destination,
                       (uint16_t)(invalid_sequence - 0x8000u), &action);
    input = make_control_input(&peer_c, &reply);
    CHECK("AODV-03", aodv_core_ingest_control(&core, &input, 11u) == AODV_STATUS_OK);
    drain_actions(&core);
    CHECK("AODV-03", aodv_core_route_snapshot(&core, &destination, &route) ==
                        AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_INVALID &&
                        route.destination_sequence == invalid_sequence);

    if (!record_reverse_request("AODV-03", &core, &peer_a, &request, 0x2002u, 12u,
                                &action)) {
        return;
    }
    make_matching_rrep(&reply, &reply_template, &peer_b, destination, invalid_sequence,
                       &action);
    input.control = reply;
    CHECK("AODV-03", aodv_core_ingest_control(&core, &input, 12u) == AODV_STATUS_OK);
    drain_actions(&core);
    CHECK("AODV-03", aodv_core_route_snapshot(&core, &destination, &route) ==
                        AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_VALID &&
                        route.destination_sequence == invalid_sequence);
}

static void test_blocker_control_backpressure_retries(void)
{
    aodv_core_t rreq_core;
    aodv_core_t rrep_core;
    aodv_core_t rerr_core;
    tavrn_codec_config_t codec_a = make_codec_config(adva_a);
    tavrn_codec_config_t codec_b = make_codec_config(adva_b);
    tavrn_validated_control_t request;
    tavrn_validated_control_t reply_template;
    tavrn_validated_control_t reply;
    tavrn_validated_control_t error;
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tron_application_data_t application = make_application(peer_c.logical_id);
    aodv_control_input_t input;
    aodv_action_t action;
    uint8_t i;

    if (!decode_control("AODV-02", &codec_b, adva_a, rreq16, sizeof(rreq16),
                        TAVRN_WIRE_E_RREQ, &request) ||
        !decode_control("AODV-03", &codec_a, adva_c, rrep16, sizeof(rrep16),
                        TAVRN_WIRE_E_RREP, &reply_template) ||
        !decode_control("AODV-06", &codec_a, adva_b, rerr16, sizeof(rerr16),
                        TAVRN_WIRE_E_RERR, &error)) {
        return;
    }

    if (!init_core(&rreq_core, adva_b, 1u)) {
        return;
    }
    fill_action_queue_with_discoveries(&rreq_core, 0x4000u, 0u);
    input = make_control_input(&peer_a, &request);
    CHECK("AODV-02", aodv_core_ingest_control(&rreq_core, &input, 1u) ==
                        AODV_STATUS_BUSY);
    drain_actions(&rreq_core);
    CHECK("AODV-02", aodv_core_ingest_control(&rreq_core, &input, 2u) ==
                        AODV_STATUS_OK &&
                        poll_control("AODV-02", &rreq_core, AODV_ACTION_SEND_RREQ,
                                     &action));

    if (!init_core(&rrep_core, adva_a, 1u) ||
        aodv_core_submit_application(&rrep_core, &application, 0u) !=
            AODV_STATUS_QUEUED ||
        !poll_control("AODV-04", &rrep_core, AODV_ACTION_SEND_RREQ, &action)) {
        return;
    }
    make_matching_rrep(&reply, &reply_template, &peer_a, peer_c.logical_id, 0x0203u,
                       &action);
    CHECK("AODV-04", aodv_core_tick(&rrep_core, 600u) == AODV_STATUS_OK);
    for (i = 0u; i < TAVRN_AODV_ACTION_CAPACITY - 1u; i++) {
        tavrn_logical_id_t destination;

        destination.width = TAVRN_IDENTITY_SID16;
        destination.value = (uint16_t)(0x5000u + i);
        application = make_application(destination);
        CHECK("AODV-05", aodv_core_submit_application(&rrep_core, &application, 600u) ==
                            AODV_STATUS_QUEUED);
    }
    input = make_control_input(&peer_c, &reply);
    CHECK("AODV-03", aodv_core_ingest_control(&rrep_core, &input, 600u) ==
                        AODV_STATUS_OK);
    drain_actions(&rrep_core);

    if (!init_core(&rerr_core, adva_a, 1u)) {
        return;
    }
    fill_action_queue_with_discoveries(&rerr_core, 0x6000u, 0u);
    input = make_control_input(&peer_b, &error);
    CHECK("AODV-06", aodv_core_ingest_control(&rerr_core, &input, 1u) ==
                        AODV_STATUS_BUSY);
    drain_actions(&rerr_core);
    CHECK("AODV-06", aodv_core_ingest_control(&rerr_core, &input, 2u) ==
                        AODV_STATUS_OK &&
                        poll_control("AODV-06", &rerr_core, AODV_ACTION_SEND_RERR,
                                     &action));
}

static void test_blocker_pending_release_and_busy_submit(void)
{
    aodv_core_t core;
    tavrn_codec_config_t codec = make_codec_config(adva_b);
    tavrn_validated_control_t reply_template;
    tavrn_validated_control_t reply;
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_logical_id_t destination;
    tron_application_data_t application;
    aodv_control_input_t input;
    aodv_action_t action;
    uint8_t i;

    if (!init_core(&core, adva_b, 1u) ||
        !decode_control("AODV-05", &codec, adva_c, rrep16, sizeof(rrep16),
                        TAVRN_WIRE_E_RREP, &reply_template)) {
        return;
    }
    for (i = 0u; i < TAVRN_AODV_ACTION_CAPACITY; i++) {
        destination.width = TAVRN_IDENTITY_SID16;
        destination.value = (uint16_t)(0x7000u + i);
        if (!install_route_through_next_hop("AODV-05", &core, &peer_b, &peer_c,
                                            &peer_b, destination, &reply_template,
                                            (uint32_t)i * 200u)) {
            return;
        }
        drain_actions(&core);
    }
    for (i = 0u; i < TAVRN_AODV_ACTION_CAPACITY; i++) {
        destination.width = TAVRN_IDENTITY_SID16;
        destination.value = (uint16_t)(0x7000u + i);
        application = make_application(destination);
        CHECK("AODV-05", aodv_core_submit_application(&core, &application,
                                                         10000u + i) == AODV_STATUS_OK);
    }
    destination.width = TAVRN_IDENTITY_SID16;
    destination.value = 0x7100u;
    application = make_application(destination);
    CHECK("AODV-05", aodv_core_submit_application(&core, &application, 11000u) ==
                        AODV_STATUS_BUSY);
    drain_actions(&core);
    CHECK("AODV-05", aodv_core_submit_application(&core, &application, 11001u) ==
                        AODV_STATUS_QUEUED &&
                        poll_control("AODV-04", &core, AODV_ACTION_SEND_RREQ, &action));
    make_matching_rrep(&reply, &reply_template, &peer_b, destination, 0x0701u, &action);
    input = make_control_input(&peer_c, &reply);
    CHECK("AODV-05", aodv_core_ingest_control(&core, &input, 11002u) ==
                        AODV_STATUS_OK &&
                        poll_control("AODV-05", &core, AODV_ACTION_FORWARD_DATA,
                                     &action) &&
                        aodv_core_poll_action(&core, &action) == AODV_ACTION_POLL_EMPTY);
}

static void test_blocker_late_ack_and_blacklist_admission(void)
{
    aodv_core_t core;
    aodv_core_t sequence_core;
    tavrn_codec_config_t codec = make_codec_config(adva_c);
    tavrn_validated_control_t request;
    tavrn_validated_control_t request_after_blacklist;
    tavrn_validated_control_t ack;
    tavrn_validated_control_t reply_template;
    tavrn_validated_control_t reply;
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tron_application_data_t application = make_application(peer_b.logical_id);
    aodv_control_input_t input;
    aodv_action_t action;

    if (!decode_control("AODV-07", &codec, adva_b, rreq16, sizeof(rreq16),
                        TAVRN_WIRE_E_RREQ, &request) ||
        !decode_control("AODV-07", &codec, adva_b, rrep_ack16,
                        sizeof(rrep_ack16), TAVRN_WIRE_E_RREP_ACK, &ack) ||
        !decode_control("AODV-03", &codec, adva_b, rrep16, sizeof(rrep16),
                        TAVRN_WIRE_E_RREP, &reply_template) ||
        !init_core(&core, adva_c, 0x0203u)) {
        return;
    }
    input = make_control_input(&peer_b, &request);
    CHECK("AODV-07", aodv_core_ingest_control(&core, &input, 0u) == AODV_STATUS_OK &&
                        poll_control("AODV-07", &core, AODV_ACTION_SEND_RREP, &action) &&
                        pdu_u16(&action.detail.control.control, 11u) == 0x0203u &&
                        aodv_core_mark_action_sent(&core, action.detail.control.token, 1u) ==
                            AODV_STATUS_OK);
    input.control = ack;
    CHECK("AODV-07", aodv_core_ingest_control(&core, &input, 251u) ==
                        AODV_STATUS_UNMATCHED &&
                        poll_control("AODV-07", &core, AODV_ACTION_BLACKLIST_NEIGHBOR,
                                     &action));

    request_after_blacklist = request;
    pdu_set_u16(&request_after_blacklist, 9u, 0x1002u);
    input.control = request_after_blacklist;
    CHECK("AODV-07", aodv_core_ingest_control(&core, &input, 252u) ==
                        AODV_STATUS_BUSY);

    CHECK("AODV-05", aodv_core_submit_application(&core, &application, 253u) ==
                        AODV_STATUS_QUEUED &&
                        poll_control("AODV-04", &core, AODV_ACTION_SEND_RREQ, &action));
    make_matching_rrep(&reply, &reply_template, &peer_c, peer_b.logical_id,
                       0x0303u, &action);
    input = make_control_input(&peer_b, &reply);
    CHECK("AODV-07", aodv_core_ingest_control(&core, &input, 253u) ==
                        AODV_STATUS_BUSY);
    CHECK("AODV-07", aodv_core_ingest_control(&core, &input, 852u) ==
                        AODV_STATUS_OK &&
                        poll_control("AODV-05", &core, AODV_ACTION_FORWARD_DATA,
                                     &action));

    if (!init_core(&sequence_core, adva_c, 0x0203u)) {
        return;
    }
    CHECK("AODV-03", aodv_core_submit_application(&sequence_core, &application, 0u) ==
                        AODV_STATUS_QUEUED &&
                        poll_control("AODV-04", &sequence_core, AODV_ACTION_SEND_RREQ,
                                     &action));
    input = make_control_input(&peer_b, &request);
    CHECK("AODV-03", aodv_core_ingest_control(&sequence_core, &input, 1u) ==
                        AODV_STATUS_OK &&
                        poll_control("AODV-03", &sequence_core, AODV_ACTION_SEND_RREP,
                                     &action) &&
                        pdu_u16(&action.detail.control.control, 11u) == 0x0204u);
}

static void test_aodv_07_cancel_unsent_action(void)
{
    aodv_core_t core;
    tavrn_codec_config_t codec = make_codec_config(adva_c);
    tavrn_validated_control_t request;
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    aodv_control_input_t input;
    aodv_action_t action;
    uint8_t i;

    if (!decode_control("AODV-07", &codec, adva_b, rreq16, sizeof(rreq16),
                        TAVRN_WIRE_E_RREQ, &request) ||
        !init_core(&core, adva_c, 0x0203u)) {
        return;
    }
    input = make_control_input(&peer_b, &request);
    for (i = 0u; i < TAVRN_AODV_RREP_ACK_WAIT_CAPACITY + 1u; i++) {
        pdu_set_u16(&input.control, 9u, (uint16_t)(0x6700u + i));
        CHECK("AODV-07", aodv_core_ingest_control(&core, &input, i) ==
                             AODV_STATUS_OK &&
                             poll_control("AODV-07", &core, AODV_ACTION_SEND_RREP,
                                          &action) &&
                             action.detail.control.token != 0u &&
                             aodv_core_cancel_unsent_action(
                                 &core, action.detail.control.token) == AODV_STATUS_OK &&
                             aodv_core_mark_action_sent(
                                 &core, action.detail.control.token, i) ==
                                 AODV_STATUS_NOT_FOUND);
    }
    pdu_set_u16(&input.control, 9u, 0x6710u);
    CHECK("AODV-07", aodv_core_ingest_control(&core, &input, 10u) ==
                         AODV_STATUS_OK &&
                         poll_control("AODV-07", &core, AODV_ACTION_SEND_RREP,
                                      &action) &&
                         aodv_core_mark_action_sent(&core, action.detail.control.token,
                                                    10u) == AODV_STATUS_OK &&
                         aodv_core_cancel_unsent_action(&core,
                                                        action.detail.control.token) ==
                             AODV_STATUS_BUSY);
}

static void test_aodv_07_purge_unsent_rrep_ack_wait_capacity(void)
{
    aodv_core_t core;
    tavrn_codec_config_t codec = make_codec_config(adva_c);
    tavrn_validated_control_t request;
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    aodv_control_input_t input;
    aodv_action_t action;
    uint8_t pass;
    uint8_t index;

    if (!decode_control("AODV-07", &codec, adva_b, rreq16, sizeof(rreq16),
                        TAVRN_WIRE_E_RREQ, &request) ||
        !init_core(&core, adva_c, 0x0203u)) {
        return;
    }
    input = make_control_input(&peer_b, &request);
    for (pass = 0u; pass < 3u; pass++) {
        for (index = 0u; index < TAVRN_AODV_RREP_ACK_WAIT_CAPACITY; index++) {
            pdu_set_u16(&input.control, 9u,
                        (uint16_t)(0x6800u +
                                   pass * TAVRN_AODV_RREP_ACK_WAIT_CAPACITY + index));
            CHECK("AODV-07", aodv_core_ingest_control(
                                 &core, &input,
                                 (uint32_t)(pass * 10u + index)) == AODV_STATUS_OK);
        }
        CHECK("AODV-07", aodv_core_quarantine_peer_incarnation(&core, &peer_b) ==
                             AODV_FAILURE_OK &&
                             aodv_core_poll_action(&core, &action) ==
                                 AODV_ACTION_POLL_EMPTY);
    }
}

static void test_aodv_peer_quarantine_reports_discarded_data(void)
{
    aodv_core_t core;
    tavrn_codec_config_t codec = make_codec_config(adva_a);
    tavrn_validated_control_t reply;
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    aodv_data_input_t forward_input;
    aodv_data_input_t delivery_input;
    aodv_action_t action;
    peer_discard_recorder_t recorder;

    memset(&recorder, 0, sizeof(recorder));
    if (!init_core(&core, adva_a, 1u) ||
        !decode_control("AODV-07", &codec, adva_c, rrep16, sizeof(rrep16),
                        TAVRN_WIRE_E_RREP, &reply) ||
        !install_route_through_next_hop("AODV-07", &core, &peer_a, &peer_b,
                                        &peer_a, peer_c.logical_id, &reply, 1u)) {
        return;
    }
    memset(&forward_input, 0, sizeof(forward_input));
    forward_input.transmitter = peer_a;
    forward_input.data = make_link_data(peer_a.logical_id, peer_c.logical_id,
                                        0x680fu);
    memset(&delivery_input, 0, sizeof(delivery_input));
    delivery_input.transmitter = peer_b;
    delivery_input.data = make_link_data(peer_b.logical_id, peer_a.logical_id,
                                         0x6810u);
    CHECK("AODV-07", aodv_core_ingest_data(&core, &forward_input, 4u) ==
                         AODV_STATUS_OK &&
                         aodv_core_ingest_data(&core, &delivery_input, 5u) ==
                         AODV_STATUS_OK);
    CHECK("AODV-07", aodv_core_quarantine_peer_incarnation_with_data_discard(
                         &core, &peer_b, record_peer_data_discard, &recorder) ==
                         AODV_FAILURE_OK);
    CHECK("AODV-07", recorder.count == 2u &&
                         ((recorder.actions[0].next_hop.logical_id.value ==
                               peer_b.logical_id.value &&
                           recorder.actions[0].data.final_destination.value ==
                               peer_c.logical_id.value &&
                           recorder.actions[1].data.origin.value ==
                               peer_b.logical_id.value &&
                           recorder.actions[1].data.final_destination.value ==
                               peer_a.logical_id.value) ||
                          (recorder.actions[1].next_hop.logical_id.value ==
                               peer_b.logical_id.value &&
                           recorder.actions[1].data.final_destination.value ==
                               peer_c.logical_id.value &&
                           recorder.actions[0].data.origin.value ==
                               peer_b.logical_id.value &&
                           recorder.actions[0].data.final_destination.value ==
                               peer_a.logical_id.value)));
    CHECK("AODV-07", aodv_core_poll_action(&core, &action) ==
                         AODV_ACTION_POLL_EMPTY);
}

static void test_blocker_failure_rediscovery_resumes_data(void)
{
    aodv_core_t node_a;
    aodv_core_t node_c;
    tavrn_codec_config_t codec_a = make_codec_config(adva_a);
    tavrn_codec_config_t codec_c = make_codec_config(adva_c);
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tron_application_data_t application = make_application(peer_c.logical_id);
    aodv_data_input_t data_input;
    aodv_route_snapshot_t route;
    aodv_action_t action;

    if (!init_core(&node_a, adva_a, 1u) || !init_core(&node_c, adva_c, 0x0203u)) {
        return;
    }
    CHECK("AODV-05", aodv_core_submit_application(&node_a, &application, 0u) ==
                        AODV_STATUS_QUEUED &&
                        poll_control("AODV-04", &node_a, AODV_ACTION_SEND_RREQ, &action) &&
                        transmit_control("AODV-02", &codec_a, &codec_c, &peer_a, &action,
                                         &node_c, 1u) &&
                        poll_control("AODV-03", &node_c, AODV_ACTION_SEND_RREP, &action) &&
                        aodv_core_mark_action_sent(&node_c, action.detail.control.token,
                                                   1u) == AODV_STATUS_OK &&
                        transmit_control("AODV-03", &codec_c, &codec_a, &peer_c, &action,
                                         &node_a, 2u) &&
                        poll_control("AODV-07", &node_a, AODV_ACTION_SEND_RREP_ACK,
                                     &action) &&
                        transmit_control("AODV-07", &codec_a, &codec_c, &peer_a, &action,
                                         &node_c, 3u) &&
                        poll_control("AODV-05", &node_a, AODV_ACTION_FORWARD_DATA,
                                     &action));
    memset(&data_input, 0, sizeof(data_input));
    data_input.transmitter = peer_a;
    data_input.data = action.detail.data.data;
    CHECK("AODV-05", aodv_core_ingest_data(&node_c, &data_input, 4u) == AODV_STATUS_OK &&
                        poll_control("AODV-05", &node_c, AODV_ACTION_DELIVER_DATA,
                                     &action));

    CHECK("AODV-06", aodv_core_report_link_failure(&node_a, &peer_c, NULL,
                                                      AODV_LINK_FAILURE_IMMEDIATE_RERR,
                                                      5u) == AODV_FAILURE_OK &&
                        poll_control("AODV-06", &node_a, AODV_ACTION_SEND_RERR, &action));
    CHECK("AODV-05", aodv_core_submit_application(&node_a, &application, 6u) ==
                        AODV_STATUS_QUEUED &&
                        poll_control("AODV-04", &node_a, AODV_ACTION_SEND_RREQ, &action) &&
                        (action.detail.control.control.pdu[5] & 0x10u) == 0u &&
                        pdu_u16(&action.detail.control.control, 13u) == 0x0204u &&
                        transmit_control("AODV-02", &codec_a, &codec_c, &peer_a, &action,
                                         &node_c, 7u) &&
                        poll_control("AODV-03", &node_c, AODV_ACTION_SEND_RREP, &action) &&
                        pdu_u16(&action.detail.control.control, 11u) == 0x0204u &&
                        aodv_core_mark_action_sent(&node_c, action.detail.control.token,
                                                   7u) == AODV_STATUS_OK &&
                        transmit_control("AODV-03", &codec_c, &codec_a, &peer_c, &action,
                                         &node_a, 8u) &&
                        poll_control("AODV-07", &node_a, AODV_ACTION_SEND_RREP_ACK,
                                     &action) &&
                        transmit_control("AODV-07", &codec_a, &codec_c, &peer_a, &action,
                                         &node_c, 9u) &&
                        poll_control("AODV-05", &node_a, AODV_ACTION_FORWARD_DATA,
                                     &action));
    data_input.transmitter = peer_a;
    data_input.data = action.detail.data.data;
    CHECK("AODV-05", aodv_core_ingest_data(&node_c, &data_input, 10u) ==
                        AODV_STATUS_OK &&
                        poll_control("AODV-05", &node_c, AODV_ACTION_DELIVER_DATA,
                                     &action));
    CHECK("AODV-03", aodv_core_route_snapshot(&node_a, &peer_c.logical_id, &route) ==
                        AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_VALID &&
                        route.destination_sequence == 0x0204u);
}

static void test_blocker_eight_pending_with_rrep_ack_drains(void)
{
    aodv_core_t core;
    tavrn_codec_config_t codec = make_codec_config(adva_a);
    tavrn_validated_control_t reply_template;
    tavrn_validated_control_t reply;
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tron_application_data_t application = make_application(peer_c.logical_id);
    aodv_control_input_t input;
    aodv_route_snapshot_t route;
    aodv_action_t action;
    uint8_t i;

    if (!init_core(&core, adva_a, 1u) ||
        !decode_control("AODV-03", &codec, adva_c, rrep16, sizeof(rrep16),
                        TAVRN_WIRE_E_RREP, &reply_template)) {
        return;
    }
    for (i = 0u; i < TAVRN_AODV_PENDING_DATA_CAPACITY; i++) {
        CHECK("AODV-05", aodv_core_submit_application(&core, &application, 0u) ==
                            AODV_STATUS_QUEUED);
    }
    if (!poll_control("AODV-04", &core, AODV_ACTION_SEND_RREQ, &action)) {
        return;
    }
    make_matching_rrep(&reply, &reply_template, &peer_a, peer_c.logical_id, 0x0203u,
                       &action);
    reply.pdu[5] |= 0x40u;
    input = make_control_input(&peer_c, &reply);
    CHECK("AODV-07", aodv_core_ingest_control(&core, &input, 1u) == AODV_STATUS_OK &&
                        poll_control("AODV-07", &core, AODV_ACTION_SEND_RREP_ACK,
                                     &action));
    for (i = 0u; i < TAVRN_AODV_PENDING_DATA_CAPACITY; i++) {
        CHECK("AODV-05", poll_control("AODV-05", &core, AODV_ACTION_FORWARD_DATA,
                                        &action));
    }
    CHECK("AODV-05", aodv_core_poll_action(&core, &action) == AODV_ACTION_POLL_EMPTY &&
                        aodv_core_route_snapshot(&core, &peer_c.logical_id, &route) ==
                            AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_VALID);
}

static void test_serial_04_incarnation_destination_reset_and_relearn(void)
{
    aodv_core_t core;
    tavrn_codec_config_t codec = make_codec_config(adva_a);
    tavrn_validated_control_t reply_template;
    tavrn_validated_control_t reply;
    tavrn_direct_peer_t peer_a = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    aodv_route_snapshot_t route_b;
    aodv_route_snapshot_t route_c;
    tron_application_data_t application;
    aodv_control_input_t input;
    aodv_action_t action;

    if (!init_core(&core, adva_a, 1u) ||
        !decode_control("SERIAL-04", &codec, adva_c, rrep16, sizeof(rrep16),
                        TAVRN_WIRE_E_RREP, &reply_template) ||
        !install_route_through_next_hop("SERIAL-04", &core, &peer_a, &peer_c,
                                        &peer_a, peer_b.logical_id,
                                        &reply_template, 0u) ||
        !install_route_through_next_hop("SERIAL-04", &core, &peer_a, &peer_b,
                                        &peer_a, peer_c.logical_id,
                                        &reply_template, 10u)) {
        return;
    }
    CHECK("SERIAL-04", aodv_core_route_snapshot(&core, &peer_b.logical_id, &route_b) ==
                           AODV_ROUTE_QUERY_FOUND &&
                           route_b.state == AODV_ROUTE_VALID &&
                           route_b.next_hop.logical_id.value ==
                               peer_c.logical_id.value &&
                           aodv_core_route_snapshot(&core, &peer_c.logical_id,
                                                    &route_c) ==
                               AODV_ROUTE_QUERY_FOUND &&
                           route_c.state == AODV_ROUTE_VALID &&
                           route_c.next_hop.logical_id.value ==
                               peer_b.logical_id.value);
    CHECK("SERIAL-04", aodv_core_reset_peer_incarnation(&core, &peer_b, 20u) ==
                           AODV_FAILURE_OK &&
                           poll_control("SERIAL-04", &core, AODV_ACTION_SEND_RERR,
                                        &action) &&
                           action.detail.control.control.pdu[11] == 2u &&
                           pdu_u16(&action.detail.control.control, 12u) ==
                               peer_c.logical_id.value &&
                           pdu_u16(&action.detail.control.control, 14u) ==
                               (uint16_t)(0x0100u + peer_c.logical_id.value) &&
                           pdu_u16(&action.detail.control.control, 16u) ==
                               peer_b.logical_id.value &&
                           pdu_u16(&action.detail.control.control, 18u) ==
                               (uint16_t)(0x0100u + peer_b.logical_id.value));
    CHECK("SERIAL-04", aodv_core_route_snapshot(&core, &peer_b.logical_id, &route_b) ==
                           AODV_ROUTE_QUERY_FOUND &&
                           route_b.state == AODV_ROUTE_INVALID &&
                           route_b.sequence_valid == 0u &&
                           route_b.destination_sequence == 0u &&
                           aodv_core_route_snapshot(&core, &peer_c.logical_id,
                                                    &route_c) ==
                               AODV_ROUTE_QUERY_FOUND &&
                           route_c.state == AODV_ROUTE_INVALID &&
                           route_c.sequence_valid != 0u &&
                           route_c.destination_sequence ==
                               (uint16_t)(0x0101u + peer_c.logical_id.value));

    application = make_application(peer_b.logical_id);
    CHECK("SERIAL-04", aodv_core_submit_application(&core, &application, 21u) ==
                           AODV_STATUS_QUEUED &&
                           poll_control("SERIAL-04", &core, AODV_ACTION_SEND_RREQ,
                                        &action));
    make_matching_rrep(&reply, &reply_template, &peer_a, peer_b.logical_id, 1u,
                       &action);
    input = make_control_input(&peer_b, &reply);
    CHECK("SERIAL-04", aodv_core_ingest_control(&core, &input, 22u) ==
                           AODV_STATUS_OK &&
                           aodv_core_route_snapshot(&core, &peer_b.logical_id,
                                                    &route_b) ==
                               AODV_ROUTE_QUERY_FOUND &&
                           route_b.state == AODV_ROUTE_VALID &&
                           route_b.destination_sequence == 1u &&
                           route_b.next_hop.logical_id.value ==
                               peer_b.logical_id.value);
}

int main(void)
{
    test_serial_freshness_and_exact_half();
    test_aodv_01_fixed_routes_precursors_and_capacity();
    test_aodv_02_rreq_reverse_route_dedupe_and_loop();
    test_aodv_03_rrep_freshness_tie_and_forward_route();
    test_aodv_03_overheard_rrep_is_unmatched();
    test_aodv_04_expanding_rings();
    test_aodv_05_two_node_pending_data_release();
    test_aodv_06_all_routes_rerr_segments_and_backpressure();
    test_aodv_07_exact_ack_and_scoped_timeout();
    test_serial_04_rejoin_invalidation_has_route_and_precursor();
    test_deterministic_three_node_chain();
    test_aodv_data_probe_hop_bounds_and_commit();
    test_blocker_route_freshness_after_invalidation();
    test_blocker_control_backpressure_retries();
    test_blocker_pending_release_and_busy_submit();
    test_blocker_late_ack_and_blacklist_admission();
    test_aodv_07_cancel_unsent_action();
    test_aodv_07_purge_unsent_rrep_ack_wait_capacity();
    test_aodv_peer_quarantine_reports_discarded_data();
    test_blocker_failure_rediscovery_resumes_data();
    test_blocker_eight_pending_with_rrep_ack_drains();
    test_serial_04_incarnation_destination_reset_and_relearn();
    if (failures != 0u) {
        printf("tavrn_aodv RED tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_aodv tests passed\n");
    return 0;
}
