#include "aodv_core.h"
#include "ble_mesh_scheduler.h"
#include "tavrn_link_v2.h"
#include "tavrn_phase4_incarnation_contract.h"
#include "tavrn_router.h"
#include "tavrn_wire_v2.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_NETWORK_ID 0x2au
#define TEST_ANNOUNCE_MS 30u

static unsigned int failures;
static const char *reported[3];
static unsigned int reported_count;

static int first_for(const char *requirement)
{
    unsigned int index;

    for (index = 0u; index < reported_count; index++) {
        if (strcmp(reported[index], requirement) == 0) {
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
static const uint8_t adva_d[TAVRN_ADVA_LEN] = {
    0x21u, 0x32u, 0x43u, 0x54u, 0x65u, 0xc2u,
};
/* Same SID16 as B, distinct valid random-static full identity. */
static const uint8_t adva_b_sid16_collision[TAVRN_ADVA_LEN] = {
    0xdcu, 0x4bu, 0x7au, 0x61u, 0x50u, 0xc3u,
};
static const uint8_t adva_sid16_zero[TAVRN_ADVA_LEN] = {
    0x00u, 0x00u, 0x11u, 0x22u, 0x33u, 0xc4u,
};
static const uint8_t adva_sid16_reserved[TAVRN_ADVA_LEN] = {
    0xffu, 0xffu, 0x11u, 0x22u, 0x33u, 0xc4u,
};

typedef struct incarnation_fixture {
    ble_mesh_scheduler_t scheduler;
    tavrn_link_v2_t link;
    aodv_core_t aodv;
    tavrn_router_t router;
} incarnation_fixture_t;

typedef struct incarnation_delivery_recorder {
    tavrn_direct_peer_t reserved_transmitter;
    tavrn_link_data_t reserved_data;
    tavrn_direct_peer_t cancelled_transmitter;
    tavrn_link_data_t cancelled_data;
    tavrn_router_delivery_token_t token;
    tavrn_router_delivery_token_t cancelled_token;
    uint8_t reserve_count;
    uint8_t cancel_count;
    uint8_t cancel_busy_once;
} incarnation_delivery_recorder_t;

static int peer_snapshot(const incarnation_fixture_t *fixture,
                         const uint8_t peer_adva[TAVRN_ADVA_LEN],
                         tavrn_router_incarnation_peer_snapshot_t *out);

static tavrn_direct_peer_t make_peer(const uint8_t adva[TAVRN_ADVA_LEN],
                                     tavrn_identity_width_t width)
{
    tavrn_direct_peer_t peer;

    memset(&peer, 0, sizeof(peer));
    peer.logical_id.width = width;
    peer.logical_id.value = width == TAVRN_IDENTITY_SID8 ? adva[0] :
        (uint16_t)adva[0] | ((uint16_t)adva[1] << 8);
    memcpy(peer.adva.bytes, adva, TAVRN_ADVA_LEN);
    return peer;
}

static tavrn_adva_t make_adva(const uint8_t adva[TAVRN_ADVA_LEN])
{
    tavrn_adva_t result;

    memcpy(result.bytes, adva, TAVRN_ADVA_LEN);
    return result;
}

static int adva_equal(const tavrn_adva_t *left, const uint8_t right[TAVRN_ADVA_LEN])
{
    return left != NULL && memcmp(left->bytes, right, TAVRN_ADVA_LEN) == 0;
}

static aodv_core_config_t make_aodv_config(tavrn_identity_width_t width)
{
    aodv_core_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_peer(adva_a, width);
    config.network_id = TEST_NETWORK_ID;
    config.net_diameter = 15u;
    config.rreq_retries = 2u;
    config.rreq_rate_per_second = 10u;
    config.rerr_rate_per_second = 10u;
    config.request_rrep_ack = 1u;
    config.initial_origin_sequence = 1u;
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

static tavrn_link_config_t make_link_config(tavrn_identity_width_t width)
{
    tavrn_link_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_peer(adva_a, width);
    config.network_id = TEST_NETWORK_ID;
    config.hack_max_attempts = 3u;
    config.busy_max_responses = 3u;
    config.hack_response_ms = 250u;
    config.hack_turnaround_ms = 8u;
    config.retry_backoff_ms = 0u;
    config.busy_backoff_ms = 500u;
    config.data_forward_deadline_ms = 5000u;
    config.candidate_resolve_ms = 10u;
    config.data_dedupe_ms = 10000u;
    config.flood_dedupe_ms = 10000u;
    config.flood_jitter_min_ms = 20u;
    config.flood_jitter_max_ms = 120u;
    return config;
}

static tavrn_codec_config_t make_codec_config(tavrn_identity_width_t width)
{
    tavrn_codec_config_t config;

    memset(&config, 0, sizeof(config));
    config.network_id = TEST_NETWORK_ID;
    config.local_peer = make_peer(adva_a, width);
    return config;
}

static int setup_fixture(incarnation_fixture_t *fixture,
                         tavrn_router_feature_level_t level,
                         tavrn_identity_width_t width, uint16_t boot_nonce,
                         uint32_t now_ms)
{
    aodv_core_config_t aodv_config = make_aodv_config(width);
    tavrn_link_config_t link_config = make_link_config(width);
    tavrn_router_incarnation_config_t incarnation_config;

    if (fixture == NULL) {
        return 0;
    }
    memset(fixture, 0, sizeof(*fixture));
    ble_mesh_scheduler_init(&fixture->scheduler, now_ms, adva_a);
    memset(&incarnation_config, 0, sizeof(incarnation_config));
    incarnation_config.feature_level = level;
    incarnation_config.boot_nonce = boot_nonce;
    incarnation_config.reboot_announce_ms = TEST_ANNOUNCE_MS;
    return tavrn_link_v2_init(&fixture->link, &fixture->scheduler, &link_config,
                              now_ms) == TAVRN_LINK_INIT_OK &&
        aodv_core_init(&fixture->aodv, &aodv_config, now_ms) == AODV_INIT_OK &&
        tavrn_router_init_with_incarnation(&fixture->router, &fixture->link,
                                           &fixture->aodv, NULL,
                                           &incarnation_config, now_ms) ==
            TAVRN_ROUTER_INCARNATION_OK;
}

static tavrn_router_delivery_status_t accept_delivery(
    void *context, const tavrn_direct_peer_t *transmitter,
    const tavrn_link_data_t *data, tavrn_router_delivery_token_t *token_out,
    uint32_t now_ms)
{
    (void)context;
    (void)transmitter;
    (void)data;
    (void)now_ms;
    if (token_out == NULL) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    *token_out = 1u;
    return TAVRN_ROUTER_DELIVERY_OK;
}

static tavrn_router_delivery_status_t commit_delivery(
    void *context, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t now_ms)
{
    (void)context;
    (void)transmitter;
    (void)data;
    (void)now_ms;
    return token == 1u ? TAVRN_ROUTER_DELIVERY_OK : TAVRN_ROUTER_DELIVERY_INVALID;
}

static int install_delivery_hooks(incarnation_fixture_t *fixture)
{
    tavrn_router_application_hooks_t hooks;

    memset(&hooks, 0, sizeof(hooks));
    hooks.reserve = accept_delivery;
    hooks.commit = commit_delivery;
    hooks.cancel = commit_delivery;
    return tavrn_router_set_application_hooks(&fixture->router, &hooks) ==
        TAVRN_ROUTER_APPLICATION_HOOK_OK;
}

static tavrn_router_delivery_status_t reserve_recorded_delivery(
    void *context, const tavrn_direct_peer_t *transmitter,
    const tavrn_link_data_t *data, tavrn_router_delivery_token_t *token_out,
    uint32_t now_ms)
{
    incarnation_delivery_recorder_t *recorder = context;

    (void)now_ms;
    if (recorder == NULL || transmitter == NULL || data == NULL || token_out == NULL) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    recorder->reserve_count++;
    recorder->reserved_transmitter = *transmitter;
    recorder->reserved_data = *data;
    recorder->token++;
    if (recorder->token == TAVRN_ROUTER_DELIVERY_TOKEN_NONE) {
        recorder->token++;
    }
    *token_out = recorder->token;
    return TAVRN_ROUTER_DELIVERY_OK;
}

static tavrn_router_delivery_status_t commit_recorded_delivery(
    void *context, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t now_ms)
{
    incarnation_delivery_recorder_t *recorder = context;

    (void)now_ms;
    return recorder != NULL && transmitter != NULL && data != NULL &&
            token == recorder->token ? TAVRN_ROUTER_DELIVERY_OK :
                                      TAVRN_ROUTER_DELIVERY_INVALID;
}

static tavrn_router_delivery_status_t cancel_recorded_delivery(
    void *context, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t now_ms)
{
    incarnation_delivery_recorder_t *recorder = context;

    (void)now_ms;
    if (recorder == NULL || transmitter == NULL || data == NULL ||
        token != recorder->token) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    recorder->cancel_count++;
    recorder->cancelled_token = token;
    recorder->cancelled_transmitter = *transmitter;
    recorder->cancelled_data = *data;
    return recorder->cancel_busy_once != 0u && recorder->cancel_count == 1u ?
        TAVRN_ROUTER_DELIVERY_BUSY : TAVRN_ROUTER_DELIVERY_OK;
}

static int install_recording_delivery_hooks(
    incarnation_fixture_t *fixture, incarnation_delivery_recorder_t *recorder)
{
    tavrn_router_application_hooks_t hooks;

    if (fixture == NULL || recorder == NULL) {
        return 0;
    }
    memset(recorder, 0, sizeof(*recorder));
    memset(&hooks, 0, sizeof(hooks));
    hooks.context = recorder;
    hooks.reserve = reserve_recorded_delivery;
    hooks.commit = commit_recorded_delivery;
    hooks.cancel = cancel_recorded_delivery;
    return tavrn_router_set_application_hooks(&fixture->router, &hooks) ==
        TAVRN_ROUTER_APPLICATION_HOOK_OK;
}

static void pdu_put_u16(tavrn_validated_control_t *control, uint8_t offset,
                        uint16_t value)
{
    control->pdu[offset] = (uint8_t)value;
    control->pdu[(uint8_t)(offset + 1u)] = (uint8_t)(value >> 8);
}

static uint16_t pdu_u16(const tavrn_validated_control_t *control, uint8_t offset)
{
    return (uint16_t)control->pdu[offset] |
        ((uint16_t)control->pdu[(uint8_t)(offset + 1u)] << 8);
}

static tavrn_validated_control_t make_bootstrap_hello(
    const uint8_t origin[TAVRN_ADVA_LEN], tavrn_identity_width_t width,
    uint16_t nonce)
{
    tavrn_validated_control_t control;
    uint8_t width_len = width == TAVRN_IDENTITY_SID8 ? 1u : 2u;
    uint8_t origin_offset = (uint8_t)(7u + 2u * width_len);
    uint8_t nonce_offset = (uint8_t)(13u + 2u * width_len);

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_HELLO;
    control.pdu_len = (uint8_t)(15u + 2u * width_len);
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = TEST_NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_HELLO;
    control.pdu[5] = width == TAVRN_IDENTITY_SID8 ? 0xc0u : 0x40u;
    control.pdu[6] = 0x10u;
    memset(&control.pdu[7], 0xff, 2u * width_len);
    memcpy(&control.pdu[origin_offset], origin, TAVRN_ADVA_LEN);
    pdu_put_u16(&control, nonce_offset, nonce);
    return control;
}

static tavrn_validated_control_t make_ordinary_hello(
    const uint8_t origin[TAVRN_ADVA_LEN], uint16_t node_sequence)
{
    tavrn_validated_control_t control = make_bootstrap_hello(
        origin, TAVRN_IDENTITY_SID16, node_sequence);

    control.pdu[5] = 0u;
    return control;
}

static tavrn_validated_control_t make_rreq(
    tavrn_logical_id_t origin, tavrn_logical_id_t destination,
    uint16_t request_id)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_E_RREQ;
    control.pdu_len = 17u;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = TEST_NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_E_RREQ;
    control.pdu[5] = 0u;
    control.pdu[6] = 0x10u;
    pdu_put_u16(&control, 7u, origin.value);
    pdu_put_u16(&control, 9u, request_id);
    pdu_put_u16(&control, 11u, destination.value);
    pdu_put_u16(&control, 13u, 0u);
    pdu_put_u16(&control, 15u, 1u);
    return control;
}

static tavrn_validated_control_t make_rreq_from_b(uint16_t request_id)
{
    return make_rreq(make_peer(adva_b, TAVRN_IDENTITY_SID16).logical_id,
                     make_peer(adva_c, TAVRN_IDENTITY_SID16).logical_id,
                     request_id);
}

static tavrn_validated_control_t make_rrep(
    tavrn_logical_id_t immediate_receiver, tavrn_logical_id_t destination,
    tavrn_logical_id_t origin, uint16_t destination_sequence,
    uint16_t request_id, uint8_t ack_required)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_E_RREP;
    control.pdu_len = 19u;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = TEST_NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_E_RREP;
    control.pdu[5] = ack_required != 0u ? 0x40u : 0u;
    control.pdu[6] = 0x10u;
    pdu_put_u16(&control, 7u, immediate_receiver.value);
    pdu_put_u16(&control, 9u, destination.value);
    pdu_put_u16(&control, 11u, destination_sequence);
    pdu_put_u16(&control, 13u, origin.value);
    pdu_put_u16(&control, 15u, request_id);
    pdu_put_u16(&control, 17u, 10u);
    return control;
}

static int wrap_control(const tavrn_validated_control_t *control,
                        const tavrn_direct_peer_t *transmitter,
                        uint8_t out[BLE_ADV_MAX_DATA], uint8_t *out_len)
{
    tavrn_decoded_frame_t frame;
    tavrn_codec_config_t codec = make_codec_config(TAVRN_IDENTITY_SID16);
    size_t encoded_len = 0u;

    if (control == NULL || transmitter == NULL || out == NULL || out_len == NULL) {
        return 0;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = control->type;
    frame.network_id = TEST_NETWORK_ID;
    frame.transmitter = *transmitter;
    frame.detail.control = *control;
    if (tavrn_wire_v2_encode(&codec, &frame, out, BLE_ADV_MAX_DATA, &encoded_len) !=
            TAVRN_CODEC_OK || encoded_len > BLE_ADV_MAX_DATA) {
        return 0;
    }
    *out_len = (uint8_t)encoded_len;
    return 1;
}

static int wrap_raw_pdu(const tavrn_validated_control_t *control,
                        uint8_t out[BLE_ADV_MAX_DATA], uint8_t *out_len)
{
    size_t length;

    if (control == NULL || out == NULL || out_len == NULL || control->pdu_len > 24u) {
        return 0;
    }
    length = 7u + control->pdu_len;
    if (length > BLE_ADV_MAX_DATA) {
        return 0;
    }
    out[0] = 0x02u;
    out[1] = 0x01u;
    out[2] = 0x06u;
    out[3] = (uint8_t)(3u + control->pdu_len);
    out[4] = 0xffu;
    out[5] = 0xffu;
    out[6] = 0xffu;
    memcpy(&out[7], control->pdu, control->pdu_len);
    *out_len = (uint8_t)length;
    return 1;
}

static tavrn_router_event_status_t deliver_control(
    incarnation_fixture_t *fixture, const tavrn_validated_control_t *control,
    const uint8_t outer_adva[TAVRN_ADVA_LEN], uint32_t now_ms, uint8_t raw)
{
    ble_mesh_sched_event_t event;
    tavrn_direct_peer_t transmitter = make_peer(outer_adva, TAVRN_IDENTITY_SID16);

    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
    memcpy(event.adv_addr, outer_adva, TAVRN_ADVA_LEN);
    if ((raw != 0u ? wrap_raw_pdu(control, event.adv_data, &event.adv_len) :
         wrap_control(control, &transmitter, event.adv_data, &event.adv_len)) == 0) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    return tavrn_router_handle_scheduler_event(&fixture->router, &event, now_ms);
}

static tavrn_router_event_status_t deliver_data(
    incarnation_fixture_t *fixture, const uint8_t origin[TAVRN_ADVA_LEN],
    const uint8_t transmitter[TAVRN_ADVA_LEN], uint16_t sequence,
    uint32_t now_ms)
{
    tavrn_decoded_frame_t frame;
    tavrn_codec_config_t codec = make_codec_config(TAVRN_IDENTITY_SID16);
    ble_mesh_sched_event_t event;
    size_t encoded_len = 0u;

    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TEST_NETWORK_ID;
    frame.detail.data.immediate_receiver = make_peer(adva_a, TAVRN_IDENTITY_SID16).logical_id;
    frame.transmitter = make_peer(transmitter, TAVRN_IDENTITY_SID16);
    frame.detail.data.data.origin =
        make_peer(origin, TAVRN_IDENTITY_SID16).logical_id;
    frame.detail.data.data.final_destination =
        make_peer(adva_a, TAVRN_IDENTITY_SID16).logical_id;
    frame.detail.data.data.data_seq = sequence;
    frame.detail.data.data.ttl = 1u;
    frame.detail.data.data.hops = 0u;
    frame.detail.data.data.app_kind = 0x7fu;
    frame.detail.data.data.app_len = 1u;
    frame.detail.data.data.app_bytes[0] = 0xa5u;
    frame.detail.data.data.ownership = TAVRN_DATA_TRANSIT;
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
    memcpy(event.adv_addr, transmitter, TAVRN_ADVA_LEN);
    if (tavrn_wire_v2_encode(&codec, &frame, event.adv_data,
                             sizeof(event.adv_data), &encoded_len) != TAVRN_CODEC_OK ||
        encoded_len > BLE_ADV_MAX_DATA) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    event.adv_len = (uint8_t)encoded_len;
    return tavrn_router_handle_scheduler_event(&fixture->router, &event, now_ms);
}

static tavrn_router_event_status_t deliver_data_from_b(
    incarnation_fixture_t *fixture, uint16_t sequence, uint32_t now_ms)
{
    return deliver_data(fixture, adva_b, adva_b, sequence, now_ms);
}

static tavrn_router_event_status_t deliver_flood(
    incarnation_fixture_t *fixture, const uint8_t origin[TAVRN_ADVA_LEN],
    const uint8_t transmitter[TAVRN_ADVA_LEN], uint16_t sequence,
    uint32_t now_ms)
{
    tavrn_decoded_frame_t frame;
    tavrn_codec_config_t codec = make_codec_config(TAVRN_IDENTITY_SID16);
    ble_mesh_sched_event_t event;
    size_t encoded_len = 0u;

    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_FLOOD;
    frame.network_id = TEST_NETWORK_ID;
    frame.detail.flood.origin = make_peer(origin, TAVRN_IDENTITY_SID16).logical_id;
    frame.detail.flood.flood_seq = sequence;
    frame.detail.flood.ttl = 1u;
    frame.detail.flood.flood_class = 1u;
    frame.detail.flood.body_len = 1u;
    frame.detail.flood.body[0] = 0xa5u;
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
    memcpy(event.adv_addr, transmitter, TAVRN_ADVA_LEN);
    if (tavrn_wire_v2_encode(&codec, &frame, event.adv_data,
                             sizeof(event.adv_data), &encoded_len) != TAVRN_CODEC_OK ||
        encoded_len > BLE_ADV_MAX_DATA) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    event.adv_len = (uint8_t)encoded_len;
    return tavrn_router_handle_scheduler_event(&fixture->router, &event, now_ms);
}

static tavrn_router_event_status_t deliver_flood_from_b(
    incarnation_fixture_t *fixture, uint16_t sequence, uint32_t now_ms)
{
    return deliver_flood(fixture, adva_b, adva_b, sequence, now_ms);
}

static int snapshot(const incarnation_fixture_t *fixture,
                    tavrn_router_incarnation_snapshot_t *out)
{
    return tavrn_router_incarnation_snapshot(&fixture->router, out) ==
        TAVRN_ROUTER_INCARNATION_OK;
}

static int peer_snapshot(const incarnation_fixture_t *fixture,
                         const uint8_t peer_adva[TAVRN_ADVA_LEN],
                         tavrn_router_incarnation_peer_snapshot_t *out)
{
    tavrn_adva_t peer = make_adva(peer_adva);

    return tavrn_router_incarnation_peer_snapshot(&fixture->router, &peer, out) ==
        TAVRN_ROUTER_INCARNATION_OK;
}

static int control_is_direct_n1(const tavrn_validated_control_t *control,
                                const uint8_t origin[TAVRN_ADVA_LEN],
                                uint16_t nonce)
{
    return control != NULL && control->type == TAVRN_WIRE_HELLO &&
        control->pdu_len == 19u && control->pdu[0] == 0x54u &&
        control->pdu[1] == 0x52u && control->pdu[2] == 0x02u &&
        control->pdu[3] == TEST_NETWORK_ID && control->pdu[4] == TAVRN_WIRE_HELLO &&
        control->pdu[5] == 0x40u && control->pdu[6] == 0x10u &&
        control->pdu[7] == 0xffu && control->pdu[8] == 0xffu &&
        control->pdu[9] == 0xffu && control->pdu[10] == 0xffu &&
        memcmp(&control->pdu[11], origin, TAVRN_ADVA_LEN) == 0 &&
        pdu_u16(control, 17u) == nonce;
}

static int control_is_n0(const tavrn_validated_control_t *control,
                         const uint8_t origin[TAVRN_ADVA_LEN])
{
    return control != NULL && control->type == TAVRN_WIRE_HELLO &&
        control->pdu_len == 19u && control->pdu[5] == 0u &&
        control->pdu[6] == 0x10u && control->pdu[7] == 0xffu &&
        control->pdu[8] == 0xffu && control->pdu[9] == 0xffu &&
        control->pdu[10] == 0xffu &&
        memcmp(&control->pdu[11], origin, TAVRN_ADVA_LEN) == 0 &&
        pdu_u16(control, 17u) == 1u;
}

static int count_custody_for_peer(const tavrn_link_v2_t *link,
                                  const uint8_t peer_adva[TAVRN_ADVA_LEN])
{
    uint8_t index;
    int count = 0;

    for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
        if (link->custody[index].phase != TAVRN_CUSTODY_FREE &&
            adva_equal(&link->custody[index].next_hop.adva, peer_adva)) {
            count++;
        }
    }
    return count;
}

static int count_data_dedupe_for_origin(const tavrn_link_v2_t *link,
                                         uint16_t origin)
{
    uint8_t index;
    int count = 0;

    for (index = 0u; index < TAVRN_LINK_DATA_DEDUPE_CAPACITY; index++) {
        if (link->data_dedupe[index].valid != 0u &&
            link->data_dedupe[index].origin.width == TAVRN_IDENTITY_SID16 &&
            link->data_dedupe[index].origin.value == origin) {
            count++;
        }
    }
    return count;
}

static int pin_transit_data(tavrn_link_v2_t *link, const tavrn_link_data_t *data,
                            uint8_t slot)
{
    tavrn_data_dedupe_entry_t *entry;

    if (link == NULL || data == NULL || slot >= TAVRN_LINK_DATA_DEDUPE_CAPACITY) {
        return 0;
    }
    entry = &link->data_dedupe[slot];
    memset(entry, 0, sizeof(*entry));
    entry->valid = 1u;
    entry->custody_pinned = 1u;
    entry->origin = data->origin;
    entry->final_destination = data->final_destination;
    entry->data_seq = data->data_seq;
    entry->app_kind = data->app_kind;
    entry->app_source = data->app_source;
    entry->expires_at_ms = 10000u;
    return 1;
}

static int externally_pin_transit_data(tavrn_link_v2_t *link,
                                       const tavrn_link_data_t *data,
                                       uint8_t slot)
{
    return pin_transit_data(link, data, slot) &&
        tavrn_link_v2_transfer_rx_custody_to_external(link, data, 0u) ==
            TAVRN_LINK_RESOLVE_OK;
}

static int transit_pin_is_marked(const tavrn_link_v2_t *link,
                                 const tavrn_link_data_t *data,
                                 uint8_t slot)
{
    const tavrn_data_dedupe_entry_t *entry;

    if (link == NULL || data == NULL || slot >= TAVRN_LINK_DATA_DEDUPE_CAPACITY) {
        return 0;
    }
    entry = &link->data_dedupe[slot];
    return entry->valid != 0u && entry->custody_pinned != 0u &&
        entry->external_custody_owner != 0u &&
        entry->incarnation_clear_on_release != 0u &&
        entry->origin.width == data->origin.width &&
        entry->origin.value == data->origin.value &&
        entry->final_destination.width == data->final_destination.width &&
        entry->final_destination.value == data->final_destination.value &&
        entry->data_seq == data->data_seq && entry->app_kind == data->app_kind &&
        entry->app_source == data->app_source;
}

static int count_flood_dedupe_for_origin(const tavrn_link_v2_t *link,
                                          uint16_t origin)
{
    uint8_t index;
    int count = 0;

    for (index = 0u; index < TAVRN_LINK_FLOOD_DEDUPE_CAPACITY; index++) {
        if (link->flood_dedupe[index].valid != 0u &&
            link->flood_dedupe[index].origin.width == TAVRN_IDENTITY_SID16 &&
            link->flood_dedupe[index].origin.value == origin) {
            count++;
        }
    }
    return count;
}

static int queued_wire_type(const incarnation_fixture_t *fixture,
                            tavrn_wire_type_t type)
{
    uint8_t index;

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry =
            &fixture->scheduler.routed_tx_queue.entries[index];

        if (entry->occupied != 0u && entry->item.adv_len > 11u &&
            entry->item.adv_data[11] == (uint8_t)type) {
            return 1;
        }
    }
    return 0;
}

static int queued_rerr_has_entries(const incarnation_fixture_t *fixture,
                                   uint16_t first, uint16_t second,
                                   uint16_t third)
{
    uint8_t index;

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry =
            &fixture->scheduler.routed_tx_queue.entries[index];
        const uint8_t *pdu;

        if (entry->occupied == 0u || entry->item.adv_len < 31u ||
            entry->item.adv_data[11] != TAVRN_WIRE_E_RERR) {
            continue;
        }
        pdu = &entry->item.adv_data[7];
        return pdu[11] == 3u &&
            ((uint16_t)pdu[12] | ((uint16_t)pdu[13] << 8)) == first &&
            ((uint16_t)pdu[16] | ((uint16_t)pdu[17] << 8)) == second &&
            ((uint16_t)pdu[20] | ((uint16_t)pdu[21] << 8)) == third;
    }
    return 0;
}

static int queued_bootstrap_hello(const incarnation_fixture_t *fixture)
{
    uint8_t index;

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry =
            &fixture->scheduler.routed_tx_queue.entries[index];

        if (entry->occupied != 0u && entry->item.adv_len >= 26u &&
            entry->item.adv_data[7] == 0x54u &&
            entry->item.adv_data[8] == 0x52u &&
            entry->item.adv_data[9] == 0x02u &&
            entry->item.adv_data[11] == TAVRN_WIRE_HELLO &&
            entry->item.adv_data[12] == 0x40u &&
            memcmp(&entry->item.adv_data[18], adva_a, TAVRN_ADVA_LEN) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Host scheduler-port completion: AODV work itself is always removed through
 * the public router dispatch/tick seam, never by direct core polling. */
static int complete_one_queued_tx(incarnation_fixture_t *fixture,
                                  uint32_t now_ms)
{
    uint8_t index;

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_item_t item;
        ble_mesh_sched_event_t event;

        if (fixture->scheduler.routed_tx_queue.entries[index].occupied == 0u ||
            !ble_mesh_tx_queue_remove(&fixture->scheduler.routed_tx_queue,
                                      index, &item)) {
            continue;
        }
        memset(&event, 0, sizeof(event));
        event.type = BLE_MESH_SCHED_EVENT_TX_DONE;
        event.tx_token = item.token;
        event.tx_requested_channel_mask = item.channel_mask;
        event.tx_completed_channel_mask = item.channel_mask;
        return tavrn_router_handle_scheduler_event(&fixture->router, &event,
                                                   now_ms) !=
            TAVRN_ROUTER_EVENT_INVALID;
    }
    return 1;
}

static int drain_pending_reset_through_router(incarnation_fixture_t *fixture,
                                              const uint8_t peer_adva[TAVRN_ADVA_LEN],
                                              uint32_t now_ms)
{
    uint8_t iteration;

    for (iteration = 0u; iteration < 2u * TAVRN_AODV_ACTION_CAPACITY; iteration++) {
        tavrn_router_dispatch_event_t event;
        tavrn_router_incarnation_peer_snapshot_t peer;
        tavrn_router_event_status_t dispatch_status = tavrn_router_dispatch_ex(
            &fixture->router, now_ms + iteration, &event);

        if (dispatch_status == TAVRN_ROUTER_EVENT_INVALID ||
            !complete_one_queued_tx(fixture, now_ms + iteration) ||
            tavrn_router_tick(&fixture->router, now_ms + iteration) ==
                AODV_STATUS_INVALID) {
            return 0;
        }
        if (peer_snapshot(fixture, peer_adva, &peer) && peer.reset_pending == 0u &&
            peer.route_barred == 0u) {
            return 1;
        }
    }
    return 0;
}

static int install_route_via(incarnation_fixture_t *fixture,
                             const tavrn_direct_peer_t *next_hop,
                             tavrn_logical_id_t destination, uint32_t now_ms)
{
    tron_application_data_t application;
    tavrn_validated_control_t reply;
    aodv_control_input_t input;
    aodv_action_t request;
    aodv_action_t residual;

    memset(&application, 0, sizeof(application));
    application.final_destination = destination;
    application.app_kind = 0x7fu;
    if (aodv_core_submit_application(&fixture->aodv, &application, now_ms) !=
            AODV_STATUS_QUEUED ||
        aodv_core_poll_action(&fixture->aodv, &request) != AODV_ACTION_POLL_OK ||
        request.type != AODV_ACTION_SEND_RREQ) {
        return 0;
    }
    memset(&reply, 0, sizeof(reply));
    reply.type = TAVRN_WIRE_E_RREP;
    reply.pdu_len = 19u;
    reply.pdu[0] = 0x54u;
    reply.pdu[1] = 0x52u;
    reply.pdu[2] = 0x02u;
    reply.pdu[3] = TEST_NETWORK_ID;
    reply.pdu[4] = TAVRN_WIRE_E_RREP;
    reply.pdu[5] = 0x40u;
    reply.pdu[6] = 0x10u;
    pdu_put_u16(&reply, 7u, make_peer(adva_a, TAVRN_IDENTITY_SID16).logical_id.value);
    pdu_put_u16(&reply, 9u, destination.value);
    pdu_put_u16(&reply, 11u, 0x0100u);
    pdu_put_u16(&reply, 13u, make_peer(adva_a, TAVRN_IDENTITY_SID16).logical_id.value);
    reply.pdu[15] = request.detail.control.control.pdu[9];
    reply.pdu[16] = request.detail.control.control.pdu[10];
    pdu_put_u16(&reply, 17u, 10u);
    memset(&input, 0, sizeof(input));
    input.transmitter = *next_hop;
    input.control = reply;
    if (aodv_core_ingest_control(&fixture->aodv, &input, now_ms + 1u) !=
        AODV_STATUS_OK) {
        return 0;
    }
    while (aodv_core_poll_action(&fixture->aodv, &residual) == AODV_ACTION_POLL_OK) {
    }
    return 1;
}

static int finish_aodv_only_boot(incarnation_fixture_t *fixture, uint32_t now_ms)
{
    tavrn_router_incarnation_snapshot_t state;
    ble_mesh_sched_event_t done;

    if (!snapshot(fixture, &state)) {
        return 0;
    }
    memset(&done, 0, sizeof(done));
    done.type = BLE_MESH_SCHED_EVENT_TX_DONE;
    done.tx_token = state.latest_hello_tx_token;
    done.tx_requested_channel_mask = BLE_MESH_SCHED_CH_ALL;
    done.tx_completed_channel_mask = BLE_MESH_SCHED_CH_ALL;
    (void)tavrn_router_handle_scheduler_event(&fixture->router, &done, now_ms);
    (void)tavrn_router_tick(&fixture->router, now_ms + TEST_ANNOUNCE_MS);
    return snapshot(fixture, &state) &&
        state.state == TAVRN_ROUTER_INCARNATION_ESTABLISHED;
}

static int test_serial_04_rejoining_and_reset(void)
{
    incarnation_fixture_t fixture;
    tavrn_router_incarnation_snapshot_t state;
    tavrn_router_incarnation_peer_snapshot_t before;
    tavrn_router_incarnation_peer_snapshot_t after;
    tavrn_router_incarnation_counters_t counters_before;
    const tavrn_router_incarnation_counters_t *counters;
    tavrn_validated_control_t n1 = make_bootstrap_hello(
        adva_b, TAVRN_IDENTITY_SID16, 0xfffeu);
    tavrn_validated_control_t rreq = make_rreq_from_b(0x1234u);
    tavrn_direct_peer_t peer_b = make_peer(adva_b, TAVRN_IDENTITY_SID16);
    tavrn_direct_peer_t peer_c = make_peer(adva_c, TAVRN_IDENTITY_SID16);
    tavrn_direct_peer_t peer_d = make_peer(adva_d, TAVRN_IDENTITY_SID16);
    tavrn_adva_t peer_c_adva = make_adva(adva_c);
    aodv_route_snapshot_t route_c;
    aodv_route_snapshot_t route_d;
    tron_application_data_t application;
    uint32_t duplicate_before;
    uint8_t index;
    int ok = 1;

    ok &= TAVRN_ROUTER_INCARNATION_PENDING_RESET_CAPACITY == 1u;
    ok &= setup_fixture(&fixture, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0u, 0u) == 0;
    ok &= setup_fixture(&fixture, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0xfffeu, 10u);
    ok &= snapshot(&fixture, &state) &&
        state.state == TAVRN_ROUTER_INCARNATION_REJOINING &&
        state.boot_nonce == 0xfffeu &&
        control_is_direct_n1(&state.latest_hello, adva_a, 0xfffeu) &&
        state.latest_hello_tx_token != BLE_MESH_TX_TOKEN_NONE;
    (void)tavrn_router_tick(&fixture.router, 11u);
    ok &= snapshot(&fixture, &state) &&
        control_is_direct_n1(&state.latest_hello, adva_a, 0xfffeu);

    memset(&application, 0, sizeof(application));
    application.final_destination = peer_c.logical_id;
    application.app_kind = 0x7fu;
    ok &= tavrn_router_submit_application(&fixture.router, &application, 12u) ==
        AODV_STATUS_REJOINING;
    ok &= deliver_data_from_b(&fixture, 1u, 13u) == TAVRN_ROUTER_EVENT_REJOINING &&
        fixture.link.counters.rx_candidate_busy == 1u;
    duplicate_before = fixture.aodv.counters.rreq_duplicate;
    ok &= deliver_control(&fixture, &rreq, adva_b, 14u, 0u) ==
            TAVRN_ROUTER_EVENT_REJOINING &&
        fixture.aodv.counters.rreq_duplicate == duplicate_before;

    /* Bypass router submission only to put a pre-existing forward action in
     * the one AODV core.  Router dispatch must still bar it while REJOINING. */
    application.final_destination.value = 0x3001u;
    ok &= aodv_core_submit_application(&fixture.aodv, &application, 15u) ==
            AODV_STATUS_QUEUED &&
        tavrn_router_dispatch_ex(&fixture.router, 16u,
                                 &(tavrn_router_dispatch_event_t){0}) ==
            TAVRN_ROUTER_EVENT_REJOINING;
    {
        aodv_core_config_t clean_aodv = make_aodv_config(TAVRN_IDENTITY_SID16);

        ok &= aodv_core_init(&fixture.aodv, &clean_aodv, 16u) == AODV_INIT_OK;
    }

    ok &= finish_aodv_only_boot(&fixture, 20u);
    ok &= deliver_control(&fixture, &n1, adva_b, 60u, 0u) == TAVRN_ROUTER_EVENT_OK &&
        peer_snapshot(&fixture, adva_b, &before) && before.direct_binding_valid != 0u &&
        before.boot_nonce == 0xfffeu;
    counters = tavrn_router_incarnation_counters(&fixture.router);
    ok &= counters != NULL;
    if (counters != NULL) {
        counters_before = *counters;
    } else {
        memset(&counters_before, 0, sizeof(counters_before));
    }
    ok &= deliver_control(&fixture, &n1, adva_b, 61u, 0u) == TAVRN_ROUTER_EVENT_OK &&
        peer_snapshot(&fixture, adva_b, &after) && after.boot_nonce == 0xfffeu &&
        tavrn_router_incarnation_counters(&fixture.router)->same_tuple_idempotent ==
            counters_before.same_tuple_idempotent + 1u &&
        tavrn_router_incarnation_counters(&fixture.router)->reset_committed ==
            counters_before.reset_committed;

    ok &= install_route_via(&fixture, &peer_b, peer_c.logical_id, 62u) &&
        install_route_via(&fixture, &peer_b, peer_d.logical_id, 64u) &&
        install_delivery_hooks(&fixture);
    ok &= tavrn_link_v2_send_unicast(&fixture.link, &peer_b,
                                     &(tavrn_link_data_t){
                                         .origin = make_peer(adva_a,
                                                             TAVRN_IDENTITY_SID16).logical_id,
                                         .final_destination = peer_c.logical_id,
                                         .data_seq = 9u,
                                         .ttl = 1u,
                                         .app_kind = 0x7fu,
                                         .app_len = 1u,
                                         .ownership = TAVRN_DATA_ORIGINATED,
                                     },
                                      66u, &(tavrn_link_event_t){0}) == TAVRN_LINK_SEND_OK &&
        count_custody_for_peer(&fixture.link, adva_b) == 1;
    for (index = 1u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
        tavrn_link_data_t queued_data = {
            .origin = make_peer(adva_a, TAVRN_IDENTITY_SID16).logical_id,
            .final_destination = peer_c.logical_id,
            .data_seq = (uint16_t)(9u + index),
            .ttl = 1u,
            .app_kind = 0x7fu,
            .app_len = 1u,
            .ownership = TAVRN_DATA_ORIGINATED,
        };

        ok &= tavrn_link_v2_send_unicast(&fixture.link, &peer_b, &queued_data,
                                         66u, &(tavrn_link_event_t){0}) ==
            TAVRN_LINK_SEND_OK;
    }
    {
        tron_application_data_t retained_data;
        tavrn_router_phase_trace_t retained_trace;

        memset(&retained_data, 0, sizeof(retained_data));
        retained_data.final_destination = peer_c.logical_id;
        retained_data.app_kind = 0x7fu;
        retained_data.app_len = 1u;
        ok &= tavrn_router_submit_application(&fixture.router, &retained_data, 66u) ==
                AODV_STATUS_OK &&
            tavrn_router_dispatch_trace_ex(&fixture.router, 66u, &retained_trace) ==
                TAVRN_ROUTER_EVENT_BUSY &&
            retained_trace.detail.dispatch.action_present == TAVRN_ROUTER_TRACE_PRESENT &&
            retained_trace.detail.dispatch.action.type == AODV_ACTION_FORWARD_DATA &&
            fixture.router.retained_action_valid != 0u;
    }
    ok &= deliver_data_from_b(&fixture, 7u, 67u) == TAVRN_ROUTER_EVENT_OK &&
        count_data_dedupe_for_origin(&fixture.link, peer_b.logical_id.value) == 1 &&
        deliver_flood_from_b(&fixture, 1u, 68u) != TAVRN_ROUTER_EVENT_INVALID &&
        count_flood_dedupe_for_origin(&fixture.link, peer_b.logical_id.value) == 1 &&
        queued_wire_type(&fixture, TAVRN_WIRE_HACK) &&
        queued_wire_type(&fixture, TAVRN_WIRE_FLOOD);
    pdu_put_u16(&rreq, 11u,
                make_peer(adva_a, TAVRN_IDENTITY_SID16).logical_id.value);
    ok &= deliver_control(&fixture, &rreq, adva_b, 68u, 0u) == TAVRN_ROUTER_EVENT_OK &&
        peer_snapshot(&fixture, adva_b, &before) &&
        before.pending_custody_count == TAVRN_LINK_CUSTODY_CAPACITY &&
        before.data_dedupe_count == 1u && before.control_dedupe_count != 0u;
    n1 = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID16, 0x0001u);
    ok &= deliver_control(&fixture, &n1, adva_b, 69u, 0u) == TAVRN_ROUTER_EVENT_OK &&
        peer_snapshot(&fixture, adva_b, &after) && after.direct_binding_valid != 0u &&
        after.boot_nonce == 0x0001u && after.reset_pending == 0u &&
        after.pending_custody_count == 0u && after.data_dedupe_count == 0u &&
        after.control_dedupe_count == 0u && after.freshness_count == 0u &&
        after.route_count == 0u && count_custody_for_peer(&fixture.link, adva_b) == 0 &&
        count_data_dedupe_for_origin(&fixture.link, peer_b.logical_id.value) == 0 &&
        count_flood_dedupe_for_origin(&fixture.link, peer_b.logical_id.value) == 0 &&
        !queued_wire_type(&fixture, TAVRN_WIRE_HACK) &&
        !queued_wire_type(&fixture, TAVRN_WIRE_FLOOD) &&
        fixture.router.retained_action_valid == 0u &&
        aodv_core_route_snapshot(&fixture.aodv, &peer_c.logical_id, &route_c) ==
            AODV_ROUTE_QUERY_FOUND && route_c.state == AODV_ROUTE_INVALID &&
        aodv_core_route_snapshot(&fixture.aodv, &peer_d.logical_id, &route_d) ==
            AODV_ROUTE_QUERY_FOUND && route_d.state == AODV_ROUTE_INVALID &&
        tavrn_router_incarnation_counters(&fixture.router)->reset_committed ==
            counters_before.reset_committed + 1u;
    {
        tavrn_router_phase_trace_t trace;

        ok &= tavrn_router_dispatch_trace_ex(&fixture.router, 69u, &trace) ==
                TAVRN_ROUTER_EVENT_OK &&
            trace.detail.dispatch.action_present == TAVRN_ROUTER_TRACE_PRESENT &&
            trace.detail.dispatch.action.type == AODV_ACTION_SEND_RERR &&
            trace.detail.dispatch.action.detail.control.control.pdu[11] == 3u &&
            pdu_u16(&trace.detail.dispatch.action.detail.control.control, 12u) ==
                peer_c.logical_id.value &&
            pdu_u16(&trace.detail.dispatch.action.detail.control.control, 16u) ==
                peer_d.logical_id.value &&
            pdu_u16(&trace.detail.dispatch.action.detail.control.control, 20u) ==
                peer_b.logical_id.value;
    }

    /* A reset that wraps numerically is still a distinct equality key. */
    n1 = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID16, 0xffffu);
    ok &= deliver_control(&fixture, &n1, adva_b, 70u, 0u) == TAVRN_ROUTER_EVENT_OK;
    n1 = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID16, 0x0001u);
    ok &= deliver_control(&fixture, &n1, adva_b, 71u, 0u) == TAVRN_ROUTER_EVENT_OK &&
        peer_snapshot(&fixture, adva_b, &after) && after.boot_nonce == 0x0001u;
    {
        aodv_core_config_t clean_aodv = make_aodv_config(TAVRN_IDENTITY_SID16);

        ok &= aodv_core_init(&fixture.aodv, &clean_aodv, 72u) == AODV_INIT_OK &&
            install_route_via(&fixture, &peer_b, peer_c.logical_id, 73u);
    }

    /* Make every action slot unavailable so the reset cannot partly mutate. */
    for (index = 0u; index < TAVRN_AODV_ACTION_CAPACITY; index++) {
        tron_application_data_t queued;

        memset(&queued, 0, sizeof(queued));
        queued.final_destination.width = TAVRN_IDENTITY_SID16;
        queued.final_destination.value = (uint16_t)(0x3000u + index);
        queued.app_kind = 0x7fu;
        ok &= aodv_core_submit_application(&fixture.aodv, &queued,
                                           (uint32_t)(80u + index)) ==
            AODV_STATUS_QUEUED;
    }
    n1 = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID16, 0x0002u);
    ok &= deliver_control(&fixture, &n1, adva_b, 90u, 0u) == TAVRN_ROUTER_EVENT_BUSY &&
        peer_snapshot(&fixture, adva_b, &after) && after.reset_pending != 0u &&
        after.route_barred != 0u && after.boot_nonce == 0x0001u &&
        tavrn_router_submit_application(&fixture.router, &application, 91u) ==
            AODV_STATUS_BUSY;
    {
        tavrn_router_phase_trace_t trace;

        ok &= complete_one_queued_tx(&fixture, 91u) &&
            tavrn_router_dispatch_trace_ex(&fixture.router, 91u, &trace) ==
                TAVRN_ROUTER_EVENT_OK &&
            trace.detail.dispatch.action_present == TAVRN_ROUTER_TRACE_PRESENT &&
            trace.detail.dispatch.action.type == AODV_ACTION_SEND_RREQ &&
            pdu_u16(&trace.detail.dispatch.action.detail.control.control, 11u) ==
                0x3000u;
    }
    /* The one pending reset slot is global: the same tuple is retained, while
     * a distinct peer/nonce cannot install a new direct binding. */
    n1 = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID16, 0x0002u);
    ok &= deliver_control(&fixture, &n1, adva_b, 91u, 0u) == TAVRN_ROUTER_EVENT_BUSY &&
        peer_snapshot(&fixture, adva_b, &after) && after.reset_pending != 0u;
    n1 = make_bootstrap_hello(adva_c, TAVRN_IDENTITY_SID16, 0x0003u);
    ok &= deliver_control(&fixture, &n1, adva_c, 92u, 0u) == TAVRN_ROUTER_EVENT_INVALID &&
        tavrn_router_incarnation_peer_snapshot(
            &fixture.router, &peer_c_adva, &after) ==
            TAVRN_ROUTER_INCARNATION_NOT_FOUND &&
        tavrn_router_incarnation_counters(&fixture.router)->reset_overflow ==
            counters_before.reset_overflow + 1u;
    ok &= drain_pending_reset_through_router(&fixture, adva_b, 100u) &&
        peer_snapshot(&fixture, adva_b, &after) && after.reset_pending == 0u &&
        after.boot_nonce == 0x0002u && after.route_barred == 0u;
    return ok;
}

static int test_serial_04_committed_reset_retries_once(void)
{
    incarnation_fixture_t fixture;
    incarnation_delivery_recorder_t recorder;
    tavrn_router_incarnation_peer_snapshot_t peer;
    tavrn_router_phase_trace_t trace;
    tavrn_validated_control_t hello = make_bootstrap_hello(
        adva_b, TAVRN_IDENTITY_SID16, 0x0101u);
    tavrn_direct_peer_t peer_b = make_peer(adva_b, TAVRN_IDENTITY_SID16);
    tavrn_direct_peer_t peer_c = make_peer(adva_c, TAVRN_IDENTITY_SID16);
    tavrn_direct_peer_t peer_d = make_peer(adva_d, TAVRN_IDENTITY_SID16);
    uint32_t reset_committed_before;
    int ok = 1;

    ok &= setup_fixture(&fixture, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0x4141u, 0u) &&
        finish_aodv_only_boot(&fixture, 1u) &&
        deliver_control(&fixture, &hello, adva_b, 2u, 0u) ==
            TAVRN_ROUTER_EVENT_OK &&
        install_recording_delivery_hooks(&fixture, &recorder) &&
        install_route_via(&fixture, &peer_b, peer_b.logical_id, 3u) &&
        install_route_via(&fixture, &peer_b, peer_c.logical_id, 5u) &&
        install_route_via(&fixture, &peer_b, peer_d.logical_id, 7u) &&
        deliver_data_from_b(&fixture, 1u, 9u) == TAVRN_ROUTER_EVENT_OK &&
        tavrn_router_delivery_state(&fixture.router) ==
            TAVRN_ROUTER_DELIVERY_POST_ACK_COMMIT;
    recorder.cancel_busy_once = 1u;
    reset_committed_before =
        tavrn_router_incarnation_counters(&fixture.router)->reset_committed;
    hello = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID16, 0x0102u);
    ok &= deliver_control(&fixture, &hello, adva_b, 10u, 0u) ==
            TAVRN_ROUTER_EVENT_BUSY &&
        peer_snapshot(&fixture, adva_b, &peer) && peer.reset_pending != 0u &&
        peer.route_barred != 0u &&
        fixture.router.incarnation.pending_reset.aodv_reset_committed != 0u &&
        recorder.cancel_count == 1u &&
        tavrn_router_incarnation_counters(&fixture.router)->reset_committed ==
            reset_committed_before;
    ok &= tavrn_router_tick(&fixture.router, 11u) == AODV_STATUS_OK &&
        peer_snapshot(&fixture, adva_b, &peer) && peer.reset_pending == 0u &&
        peer.route_barred == 0u && peer.boot_nonce == 0x0102u &&
        recorder.cancel_count == 2u &&
        tavrn_router_delivery_state(&fixture.router) == TAVRN_ROUTER_DELIVERY_NONE &&
        tavrn_router_incarnation_counters(&fixture.router)->reset_committed ==
            reset_committed_before + 1u;
    ok &= tavrn_router_dispatch_trace_ex(&fixture.router, 12u, &trace) ==
            TAVRN_ROUTER_EVENT_OK &&
        trace.detail.dispatch.action_present == TAVRN_ROUTER_TRACE_PRESENT &&
        trace.detail.dispatch.action.type == AODV_ACTION_SEND_RERR &&
        trace.detail.dispatch.action.detail.control.control.pdu[11] == 3u &&
        pdu_u16(&trace.detail.dispatch.action.detail.control.control, 12u) ==
            peer_c.logical_id.value &&
        pdu_u16(&trace.detail.dispatch.action.detail.control.control, 16u) ==
            peer_d.logical_id.value &&
        pdu_u16(&trace.detail.dispatch.action.detail.control.control, 20u) ==
            peer_b.logical_id.value &&
        tavrn_router_dispatch_trace_ex(&fixture.router, 13u, &trace) ==
            TAVRN_ROUTER_EVENT_IGNORED;
    return ok;
}

static int test_serial_04_collision_prefilter(void)
{
    incarnation_fixture_t fixture;
    tavrn_router_incarnation_peer_snapshot_t peer_before;
    tavrn_router_incarnation_peer_snapshot_t peer_after;
    aodv_peer_incarnation_snapshot_t aodv_before;
    aodv_peer_incarnation_snapshot_t aodv_after;
    tavrn_link_counters_t link_before;
    tavrn_validated_control_t hello = make_bootstrap_hello(
        adva_b, TAVRN_IDENTITY_SID16, 0x0201u);
    tavrn_validated_control_t rreq = make_rreq_from_b(0x2201u);
    tavrn_validated_control_t n0 = make_ordinary_hello(adva_b, 1u);
    tavrn_direct_peer_t peer_b = make_peer(adva_b, TAVRN_IDENTITY_SID16);
    aodv_counters_t counters_before;
    int ok = 1;

    ok &= setup_fixture(&fixture, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0x4242u, 0u) &&
        finish_aodv_only_boot(&fixture, 1u) &&
        deliver_control(&fixture, &hello, adva_b, 2u, 0u) ==
            TAVRN_ROUTER_EVENT_OK;
    hello = make_bootstrap_hello(adva_b_sid16_collision,
                                 TAVRN_IDENTITY_SID16, 0x0202u);
    ok &= deliver_control(&fixture, &hello, adva_b_sid16_collision, 3u, 0u) ==
            TAVRN_ROUTER_EVENT_INVALID &&
        peer_snapshot(&fixture, adva_b, &peer_before) &&
        peer_before.route_barred != 0u &&
        fixture.router.incarnation.peers[0].identity_conflict != 0u &&
        aodv_core_peer_incarnation_snapshot(&fixture.aodv, &peer_b,
                                            &aodv_before) == AODV_FAILURE_OK;
    link_before = *tavrn_link_v2_counters(&fixture.link);
    counters_before = *aodv_core_counters(&fixture.aodv);
    ok &= deliver_data(&fixture, adva_b, adva_b_sid16_collision, 1u, 4u) ==
            TAVRN_ROUTER_EVENT_IGNORED &&
        deliver_flood(&fixture, adva_b, adva_b_sid16_collision, 1u, 5u) ==
            TAVRN_ROUTER_EVENT_IGNORED &&
        deliver_control(&fixture, &rreq, adva_b_sid16_collision, 6u, 0u) ==
            TAVRN_ROUTER_EVENT_IGNORED &&
        deliver_control(&fixture, &n0, adva_b_sid16_collision, 7u, 0u) ==
            TAVRN_ROUTER_EVENT_IGNORED &&
        peer_snapshot(&fixture, adva_b, &peer_after) &&
        peer_after.route_barred == peer_before.route_barred &&
        peer_after.control_dedupe_count == peer_before.control_dedupe_count &&
        count_data_dedupe_for_origin(&fixture.link, peer_b.logical_id.value) == 0 &&
        count_flood_dedupe_for_origin(&fixture.link, peer_b.logical_id.value) == 0 &&
        fixture.link.candidate_valid == 0u &&
        fixture.link.counters.rx_candidate == link_before.rx_candidate &&
        fixture.link.counters.rx_candidate_accepted == link_before.rx_candidate_accepted &&
        fixture.link.counters.rx_control == link_before.rx_control &&
        fixture.link.counters.rx_flood_committed == link_before.rx_flood_committed &&
        fixture.link.counters.rx_flood_duplicate == link_before.rx_flood_duplicate &&
        aodv_core_peer_incarnation_snapshot(&fixture.aodv, &peer_b,
                                            &aodv_after) == AODV_FAILURE_OK &&
        aodv_after.valid_route_count == aodv_before.valid_route_count &&
        aodv_after.control_dedupe_count == aodv_before.control_dedupe_count &&
        aodv_after.freshness_count == aodv_before.freshness_count &&
        fixture.aodv.counters.rreq_duplicate == counters_before.rreq_duplicate &&
        fixture.aodv.counters.action_backpressure == counters_before.action_backpressure;
    return ok;
}

static int test_serial_04_queued_local_rerr_is_preserved(void)
{
    incarnation_fixture_t fixture;
    tavrn_router_phase_trace_t trace;
    tavrn_validated_control_t hello = make_bootstrap_hello(
        adva_b, TAVRN_IDENTITY_SID16, 0x0401u);
    tavrn_direct_peer_t peer_b = make_peer(adva_b, TAVRN_IDENTITY_SID16);
    tavrn_direct_peer_t peer_c = make_peer(adva_c, TAVRN_IDENTITY_SID16);
    tavrn_direct_peer_t peer_d = make_peer(adva_d, TAVRN_IDENTITY_SID16);
    int ok = 1;

    ok &= setup_fixture(&fixture, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0x4444u, 0u) &&
        finish_aodv_only_boot(&fixture, 1u) &&
        deliver_control(&fixture, &hello, adva_b, 2u, 0u) ==
            TAVRN_ROUTER_EVENT_OK &&
        install_route_via(&fixture, &peer_b, peer_b.logical_id, 3u) &&
        install_route_via(&fixture, &peer_b, peer_c.logical_id, 5u) &&
        install_route_via(&fixture, &peer_b, peer_d.logical_id, 7u) &&
        aodv_core_report_link_failure(&fixture.aodv, &peer_b, NULL,
                                      AODV_LINK_FAILURE_IMMEDIATE_RERR, 9u) ==
            AODV_FAILURE_OK &&
        tavrn_router_dispatch_trace_ex(&fixture.router, 10u, &trace) ==
            TAVRN_ROUTER_EVENT_OK &&
        trace.detail.dispatch.action_present == TAVRN_ROUTER_TRACE_PRESENT &&
        trace.detail.dispatch.action.type == AODV_ACTION_SEND_RERR &&
        queued_rerr_has_entries(&fixture, peer_c.logical_id.value,
                                peer_d.logical_id.value,
                                peer_b.logical_id.value) &&
        tavrn_link_v2_quarantine_peer_incarnation(&fixture.link, &peer_b) ==
            TAVRN_LINK_RESOLVE_OK &&
        queued_rerr_has_entries(&fixture, peer_c.logical_id.value,
                                peer_d.logical_id.value,
                                peer_b.logical_id.value);
    return ok;
}

static int test_serial_04_external_pins_survive_peer_clear_until_release(void)
{
    incarnation_fixture_t fixture;
    tavrn_direct_peer_t peer_b = make_peer(adva_b, TAVRN_IDENTITY_SID16);
    tavrn_link_data_t origin_pinned;
    tavrn_link_data_t destination_pinned;
    tavrn_link_data_t origin_internal;
    tavrn_link_data_t destination_internal;

    if (!setup_fixture(&fixture, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                       TAVRN_IDENTITY_SID16, 0x4a4au, 0u)) {
        return 0;
    }
    memset(&origin_pinned, 0, sizeof(origin_pinned));
    origin_pinned.origin = peer_b.logical_id;
    origin_pinned.final_destination = make_peer(adva_c, TAVRN_IDENTITY_SID16).logical_id;
    origin_pinned.data_seq = 0x6101u;
    origin_pinned.ttl = 1u;
    origin_pinned.app_kind = 0x7fu;
    origin_pinned.app_source = 0x31u;
    origin_pinned.app_len = 1u;
    origin_pinned.app_bytes[0] = 0xa5u;
    origin_pinned.ownership = TAVRN_DATA_TRANSIT;
    destination_pinned = origin_pinned;
    destination_pinned.origin = make_peer(adva_d, TAVRN_IDENTITY_SID16).logical_id;
    destination_pinned.final_destination = peer_b.logical_id;
    destination_pinned.data_seq++;
    origin_internal = origin_pinned;
    origin_internal.data_seq++;
    destination_internal = destination_pinned;
    destination_internal.data_seq++;
    return externally_pin_transit_data(&fixture.link, &origin_pinned, 0u) &&
        externally_pin_transit_data(&fixture.link, &destination_pinned, 1u) &&
        pin_transit_data(&fixture.link, &origin_internal, 2u) &&
        pin_transit_data(&fixture.link, &destination_internal, 3u) &&
        tavrn_link_v2_clear_peer_incarnation(&fixture.link, &peer_b, 1u) ==
            TAVRN_LINK_RESOLVE_OK &&
        transit_pin_is_marked(&fixture.link, &origin_pinned, 0u) &&
        transit_pin_is_marked(&fixture.link, &destination_pinned, 1u) &&
        fixture.link.data_dedupe[2].valid == 0u &&
        fixture.link.data_dedupe[3].valid == 0u &&
        tavrn_link_v2_release_rx_custody(&fixture.link, &origin_pinned, 2u) ==
            TAVRN_LINK_RESOLVE_OK &&
        tavrn_link_v2_release_rx_custody(&fixture.link, &destination_pinned, 3u) ==
            TAVRN_LINK_RESOLVE_OK &&
        fixture.link.data_dedupe[0].valid == 0u &&
        fixture.link.data_dedupe[1].valid == 0u;
}

static tavrn_link_data_t make_multihop_transit_data(uint16_t sequence)
{
    tavrn_link_data_t data;

    memset(&data, 0, sizeof(data));
    data.origin = make_peer(adva_c, TAVRN_IDENTITY_SID16).logical_id;
    data.final_destination = make_peer(adva_d, TAVRN_IDENTITY_SID16).logical_id;
    data.data_seq = sequence;
    data.ttl = 2u;
    data.app_kind = 0x7fu;
    data.app_source = 0x31u;
    data.app_len = 1u;
    data.app_bytes[0] = 0xa5u;
    data.ownership = TAVRN_DATA_TRANSIT;
    return data;
}

static int transit_pin_is_externally_owned(const tavrn_link_v2_t *link,
                                           const tavrn_link_data_t *data,
                                           uint8_t slot)
{
    const tavrn_data_dedupe_entry_t *entry;

    if (link == NULL || data == NULL || slot >= TAVRN_LINK_DATA_DEDUPE_CAPACITY) {
        return 0;
    }
    entry = &link->data_dedupe[slot];
    return entry->valid != 0u && entry->custody_pinned != 0u &&
        entry->external_custody_owner != 0u &&
        entry->origin.width == data->origin.width &&
        entry->origin.value == data->origin.value &&
        entry->final_destination.width == data->final_destination.width &&
        entry->final_destination.value == data->final_destination.value &&
        entry->data_seq == data->data_seq && entry->app_kind == data->app_kind &&
        entry->app_source == data->app_source;
}

static int test_serial_04_pending_ingest_pin_clears_on_transmitter_reset(void)
{
    incarnation_fixture_t fixture;
    tavrn_direct_peer_t peer_b = make_peer(adva_b, TAVRN_IDENTITY_SID16);
    tavrn_link_data_t pending_data = make_multihop_transit_data(0x6201u);
    tavrn_link_data_t externally_owned_data = make_multihop_transit_data(0x6202u);
    tavrn_validated_control_t hello = make_bootstrap_hello(
        adva_b, TAVRN_IDENTITY_SID16, 0x6201u);
    int ok = 1;

    ok &= setup_fixture(&fixture, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0x6262u, 0u) &&
        finish_aodv_only_boot(&fixture, 1u) &&
        deliver_control(&fixture, &hello, adva_b, 2u, 0u) == TAVRN_ROUTER_EVENT_OK &&
        pin_transit_data(&fixture.link, &pending_data, 0u) &&
        externally_pin_transit_data(&fixture.link, &externally_owned_data, 1u);
    /* The public candidate path immediately consumes accepted data.  Seed the
     * exposed retained input to isolate B as the transmitter-only reset key. */
    fixture.router.pending_ingest.input.transmitter = peer_b;
    fixture.router.pending_ingest.input.data = pending_data;
    fixture.router.pending_ingest.valid = 1u;
    hello = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID16, 0x6202u);
    ok &= fixture.link.data_dedupe[0].custody_pinned != 0u &&
        fixture.link.data_dedupe[0].external_custody_owner == 0u &&
        deliver_control(&fixture, &hello, adva_b, 3u, 0u) == TAVRN_ROUTER_EVENT_OK &&
        fixture.router.pending_ingest.valid == 0u &&
        fixture.link.data_dedupe[0].custody_pinned == 0u &&
        tavrn_link_v2_release_rx_custody(&fixture.link, &pending_data, 4u) ==
            TAVRN_LINK_RESOLVE_TOKEN_INVALID &&
        transit_pin_is_externally_owned(&fixture.link, &externally_owned_data, 1u) &&
        fixture.link.data_dedupe[1].incarnation_clear_on_release == 0u &&
        tavrn_link_v2_release_rx_custody(&fixture.link, &externally_owned_data, 4u) ==
            TAVRN_LINK_RESOLVE_OK;
    return ok;
}

static int test_serial_04_retained_forward_pin_clears_on_next_hop_reset(void)
{
    incarnation_fixture_t fixture;
    tavrn_direct_peer_t peer_b = make_peer(adva_b, TAVRN_IDENTITY_SID16);
    tavrn_link_data_t data = make_multihop_transit_data(0x6301u);
    tavrn_validated_control_t hello = make_bootstrap_hello(
        adva_b, TAVRN_IDENTITY_SID16, 0x6301u);
    int ok = 1;

    ok &= setup_fixture(&fixture, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0x6363u, 0u) &&
        finish_aodv_only_boot(&fixture, 1u) &&
        deliver_control(&fixture, &hello, adva_b, 2u, 0u) == TAVRN_ROUTER_EVENT_OK &&
        pin_transit_data(&fixture.link, &data, 0u);
    memset(&fixture.router.retained_action, 0, sizeof(fixture.router.retained_action));
    fixture.router.retained_action.type = AODV_ACTION_FORWARD_DATA;
    fixture.router.retained_action.detail.data.next_hop = peer_b;
    fixture.router.retained_action.detail.data.data = data;
    fixture.router.retained_action_valid = 1u;
    hello = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID16, 0x6302u);
    ok &= fixture.link.data_dedupe[0].custody_pinned != 0u &&
        fixture.link.data_dedupe[0].external_custody_owner == 0u &&
        deliver_control(&fixture, &hello, adva_b, 3u, 0u) == TAVRN_ROUTER_EVENT_OK &&
        fixture.router.retained_action_valid == 0u &&
        fixture.link.data_dedupe[0].custody_pinned == 0u &&
        tavrn_link_v2_release_rx_custody(&fixture.link, &data, 4u) ==
            TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    return ok;
}

static int test_serial_04_queued_forward_pin_clears_on_next_hop_reset(void)
{
    incarnation_fixture_t fixture;
    tavrn_direct_peer_t peer_b = make_peer(adva_b, TAVRN_IDENTITY_SID16);
    tavrn_direct_peer_t peer_d = make_peer(adva_d, TAVRN_IDENTITY_SID16);
    tavrn_link_data_t data = make_multihop_transit_data(0x6401u);
    tavrn_validated_control_t hello = make_bootstrap_hello(
        adva_b, TAVRN_IDENTITY_SID16, 0x6401u);
    aodv_data_input_t input;
    aodv_action_t action;
    int ok = 1;

    ok &= setup_fixture(&fixture, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0x6464u, 0u) &&
        finish_aodv_only_boot(&fixture, 1u) &&
        deliver_control(&fixture, &hello, adva_b, 2u, 0u) == TAVRN_ROUTER_EVENT_OK &&
        install_route_via(&fixture, &peer_b, peer_d.logical_id, 3u);
    memset(&input, 0, sizeof(input));
    input.transmitter = peer_b;
    input.data = data;
    ok &= aodv_core_ingest_data(&fixture.aodv, &input, 5u) == AODV_STATUS_OK &&
        pin_transit_data(&fixture.link, &data, 0u);
    hello = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID16, 0x6402u);
    ok &= fixture.link.data_dedupe[0].custody_pinned != 0u &&
        fixture.link.data_dedupe[0].external_custody_owner == 0u &&
        deliver_control(&fixture, &hello, adva_b, 6u, 0u) == TAVRN_ROUTER_EVENT_OK &&
        fixture.link.data_dedupe[0].custody_pinned == 0u &&
        tavrn_link_v2_release_rx_custody(&fixture.link, &data, 7u) ==
            TAVRN_LINK_RESOLVE_TOKEN_INVALID &&
        aodv_core_poll_action(&fixture.aodv, &action) == AODV_ACTION_POLL_OK &&
        action.type == AODV_ACTION_SEND_RERR;
    return ok;
}

static int test_serial_04_relay_delivery_cancelled_by_origin(void)
{
    incarnation_fixture_t fixture;
    incarnation_delivery_recorder_t recorder;
    tavrn_router_incarnation_peer_snapshot_t peer;
    tavrn_validated_control_t hello = make_bootstrap_hello(
        adva_b, TAVRN_IDENTITY_SID16, 0x0301u);
    int ok = 1;

    ok &= setup_fixture(&fixture, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0x4343u, 0u) &&
        finish_aodv_only_boot(&fixture, 1u) &&
        deliver_control(&fixture, &hello, adva_b, 2u, 0u) ==
            TAVRN_ROUTER_EVENT_OK &&
        install_recording_delivery_hooks(&fixture, &recorder) &&
        deliver_data(&fixture, adva_b, adva_c, 1u, 3u) == TAVRN_ROUTER_EVENT_OK &&
        recorder.reserve_count == 1u &&
        tavrn_router_delivery_state(&fixture.router) ==
            TAVRN_ROUTER_DELIVERY_POST_ACK_COMMIT;
    hello = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID16, 0x0302u);
    ok &= deliver_control(&fixture, &hello, adva_b, 4u, 0u) ==
            TAVRN_ROUTER_EVENT_OK &&
        recorder.cancel_count == 1u && recorder.cancelled_token == 1u &&
        adva_equal(&recorder.cancelled_transmitter.adva, adva_c) &&
        recorder.cancelled_data.origin.value ==
            make_peer(adva_b, TAVRN_IDENTITY_SID16).logical_id.value &&
        tavrn_router_delivery_state(&fixture.router) == TAVRN_ROUTER_DELIVERY_NONE &&
        peer_snapshot(&fixture, adva_b, &peer) && peer.boot_nonce == 0x0302u &&
        peer.route_barred == 0u &&
        deliver_data(&fixture, adva_b, adva_c, 2u, 5u) == TAVRN_ROUTER_EVENT_OK &&
        recorder.reserve_count == 2u &&
        tavrn_router_delivery_state(&fixture.router) ==
            TAVRN_ROUTER_DELIVERY_POST_ACK_COMMIT;
    return ok;
}

static int fill_rrep_ack_waits_from_c(incarnation_fixture_t *fixture,
                                      uint16_t first_request_id,
                                      uint32_t now_ms)
{
    tavrn_direct_peer_t peer_c = make_peer(adva_c, TAVRN_IDENTITY_SID16);
    tavrn_logical_id_t origin_d = make_peer(adva_d, TAVRN_IDENTITY_SID16).logical_id;
    tavrn_logical_id_t local_a = make_peer(adva_a, TAVRN_IDENTITY_SID16).logical_id;
    uint8_t index;

    for (index = 0u; index < TAVRN_AODV_RREP_ACK_WAIT_CAPACITY; index++) {
        aodv_control_input_t input;

        memset(&input, 0, sizeof(input));
        input.transmitter = peer_c;
        input.control = make_rreq(origin_d, local_a,
                                  (uint16_t)(first_request_id + index));
        if (aodv_core_ingest_control(&fixture->aodv, &input,
                                     now_ms + index) != AODV_STATUS_OK) {
            return 0;
        }
    }
    {
        aodv_control_input_t input;

        memset(&input, 0, sizeof(input));
        input.transmitter = peer_c;
        input.control = make_rreq(origin_d, local_a,
                                  (uint16_t)(first_request_id +
                                             TAVRN_AODV_RREP_ACK_WAIT_CAPACITY));
        return aodv_core_ingest_control(
                   &fixture->aodv, &input,
                   now_ms + TAVRN_AODV_RREP_ACK_WAIT_CAPACITY) == AODV_STATUS_BUSY;
    }
}

static int test_serial_04_rrep_and_ack_cleanup_through_unrelated_next_hop(void)
{
    incarnation_fixture_t fixture;
    incarnation_fixture_t ack_fixture;
    tavrn_direct_peer_t peer_b = make_peer(adva_b, TAVRN_IDENTITY_SID16);
    tavrn_direct_peer_t peer_c = make_peer(adva_c, TAVRN_IDENTITY_SID16);
    tavrn_direct_peer_t peer_d = make_peer(adva_d, TAVRN_IDENTITY_SID16);
    tavrn_logical_id_t local_a = make_peer(adva_a, TAVRN_IDENTITY_SID16).logical_id;
    tavrn_validated_control_t hello = make_bootstrap_hello(
        adva_b, TAVRN_IDENTITY_SID16, 0x5101u);
    aodv_control_input_t input;
    aodv_action_t action;
    int ok = 1;

    ok &= setup_fixture(&fixture, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0x5151u, 0u) &&
        finish_aodv_only_boot(&fixture, 1u) &&
        deliver_control(&fixture, &hello, adva_b, 2u, 0u) == TAVRN_ROUTER_EVENT_OK;
    memset(&input, 0, sizeof(input));
    input.transmitter = peer_c;
    input.control = make_rreq(peer_b.logical_id, local_a, 0x5101u);
    ok &= aodv_core_ingest_control(&fixture.aodv, &input, 3u) == AODV_STATUS_OK &&
        aodv_core_poll_action(&fixture.aodv, &action) == AODV_ACTION_POLL_OK &&
        action.type == AODV_ACTION_SEND_RREP &&
        action.detail.control.token != 0u &&
        action.detail.control.next_hop.logical_id.value == peer_c.logical_id.value &&
        pdu_u16(&action.detail.control.control, 13u) == peer_b.logical_id.value;
    fixture.router.retained_action = action;
    fixture.router.retained_action_valid = 1u;
    hello = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID16, 0x5102u);
    ok &= deliver_control(&fixture, &hello, adva_b, 4u, 0u) == TAVRN_ROUTER_EVENT_OK &&
        fixture.router.retained_action_valid == 0u &&
        fill_rrep_ack_waits_from_c(&fixture, 0x5200u, 5u);

    hello = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID16, 0x5301u);
    ok &= setup_fixture(&ack_fixture, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0x5353u, 0u) &&
        finish_aodv_only_boot(&ack_fixture, 1u) &&
        deliver_control(&ack_fixture, &hello, adva_b, 2u, 0u) ==
            TAVRN_ROUTER_EVENT_OK;
    memset(&input, 0, sizeof(input));
    input.transmitter = peer_c;
    input.control = make_rreq(peer_b.logical_id, local_a, 0x5301u);
    ok &= aodv_core_ingest_control(&ack_fixture.aodv, &input, 3u) == AODV_STATUS_OK &&
        aodv_core_poll_action(&ack_fixture.aodv, &action) == AODV_ACTION_POLL_OK &&
        action.type == AODV_ACTION_SEND_RREP &&
        aodv_core_cancel_unsent_action(&ack_fixture.aodv,
                                       action.detail.control.token) == AODV_STATUS_OK;
    memset(&input, 0, sizeof(input));
    input.transmitter = peer_c;
    input.control = make_rrep(local_a, peer_d.logical_id, peer_b.logical_id,
                              0x0200u, 0x5302u, 1u);
    ok &= aodv_core_ingest_control(&ack_fixture.aodv, &input, 4u) == AODV_STATUS_OK &&
        aodv_core_poll_action(&ack_fixture.aodv, &action) == AODV_ACTION_POLL_OK &&
        action.type == AODV_ACTION_SEND_RREP_ACK &&
        action.detail.control.next_hop.logical_id.value == peer_c.logical_id.value &&
        pdu_u16(&action.detail.control.control, 8u) == peer_d.logical_id.value &&
        pdu_u16(&action.detail.control.control, 12u) == peer_b.logical_id.value;
    ack_fixture.router.retained_action = action;
    ack_fixture.router.retained_action_valid = 1u;
    hello = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID16, 0x5302u);
    ok &= deliver_control(&ack_fixture, &hello, adva_b, 5u, 0u) ==
            TAVRN_ROUTER_EVENT_OK &&
        ack_fixture.router.retained_action_valid == 0u;
    return ok;
}

static int test_serial_04_sent_ack_wait_does_not_blacklist_next_hop(void)
{
    incarnation_fixture_t fixture;
    tavrn_direct_peer_t peer_b = make_peer(adva_b, TAVRN_IDENTITY_SID16);
    tavrn_direct_peer_t peer_c = make_peer(adva_c, TAVRN_IDENTITY_SID16);
    tavrn_logical_id_t local_a = make_peer(adva_a, TAVRN_IDENTITY_SID16).logical_id;
    tavrn_validated_control_t hello = make_bootstrap_hello(
        adva_b, TAVRN_IDENTITY_SID16, 0x5401u);
    aodv_control_input_t input;
    aodv_action_t action;
    tavrn_router_dispatch_event_t event;
    int ok = 1;

    ok &= setup_fixture(&fixture, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0x5454u, 0u) &&
        finish_aodv_only_boot(&fixture, 1u) &&
        deliver_control(&fixture, &hello, adva_b, 2u, 0u) == TAVRN_ROUTER_EVENT_OK;
    memset(&input, 0, sizeof(input));
    input.transmitter = peer_c;
    input.control = make_rreq(peer_b.logical_id, local_a, 0x5401u);
    ok &= aodv_core_ingest_control(&fixture.aodv, &input, 3u) == AODV_STATUS_OK &&
        aodv_core_poll_action(&fixture.aodv, &action) == AODV_ACTION_POLL_OK &&
        action.type == AODV_ACTION_SEND_RREP &&
        aodv_core_cancel_unsent_action(&fixture.aodv,
                                       action.detail.control.token) == AODV_STATUS_OK;
    memset(&input, 0, sizeof(input));
    input.transmitter = peer_c;
    input.control = make_rrep(local_a, peer_b.logical_id, peer_b.logical_id,
                              0x0300u, 0x5402u, 0u);
    ok &= aodv_core_ingest_control(&fixture.aodv, &input, 4u) == AODV_STATUS_OK &&
        tavrn_router_dispatch_ex(&fixture.router, 5u, &event) == TAVRN_ROUTER_EVENT_OK;
    hello = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID16, 0x5402u);
    ok &= deliver_control(&fixture, &hello, adva_b, 6u, 0u) == TAVRN_ROUTER_EVENT_OK &&
        tavrn_router_tick(&fixture.router, 300u) == AODV_STATUS_OK &&
        fixture.aodv.counters.rrep_ack_timeout == 0u &&
        fill_rrep_ack_waits_from_c(&fixture, 0x5500u, 301u) &&
        tavrn_router_dispatch_ex(&fixture.router, 310u, &event) == TAVRN_ROUTER_EVENT_OK &&
        event.type != TAVRN_ROUTER_DISPATCH_EVENT_BLACKLIST_NEIGHBOR;
    return ok;
}

static int test_boot_01_vector_identity_and_ordering(void)
{
    incarnation_fixture_t fixture;
    tavrn_router_incarnation_peer_snapshot_t before;
    tavrn_router_incarnation_peer_snapshot_t after;
    tavrn_router_incarnation_counters_t counters_before;
    tavrn_validated_control_t n1 = make_bootstrap_hello(
        adva_b, TAVRN_IDENTITY_SID16, 0x1234u);
    tavrn_validated_control_t n0 = make_ordinary_hello(adva_b, 1u);
    tavrn_validated_control_t malformed;
    tavrn_decoded_frame_t decoded;
    tavrn_codec_config_t codec = make_codec_config(TAVRN_IDENTITY_SID16);
    tavrn_direct_peer_t peer_b = make_peer(adva_b, TAVRN_IDENTITY_SID16);
    tavrn_adva_t collision_adva = make_adva(adva_b_sid16_collision);
    uint8_t adv[BLE_ADV_MAX_DATA];
    uint8_t adv_len = 0u;
    int ok = 1;

    ok &= n1.pdu_len == 19u && control_is_direct_n1(&n1, adva_b, 0x1234u) &&
        wrap_control(&n1, &peer_b, adv, &adv_len) &&
        tavrn_wire_v2_decode(&codec, adva_b, adv, adv_len, &decoded) == TAVRN_CODEC_OK &&
        decoded.type == TAVRN_WIRE_HELLO && decoded.detail.control.pdu_len == 19u;
    malformed = n1;
    pdu_put_u16(&malformed, 17u, 0u);
    ok &= tavrn_wire_v2_encode(&codec,
                                &(tavrn_decoded_frame_t){
                                    .type = TAVRN_WIRE_HELLO,
                                    .network_id = TEST_NETWORK_ID,
                                    .transmitter = make_peer(adva_b,
                                                             TAVRN_IDENTITY_SID16),
                                    .detail.control = malformed,
                                }, adv, sizeof(adv), &(size_t){0}) ==
        TAVRN_CODEC_MALFORMED_FIELD;
    malformed = n1;
    malformed.pdu[6] = 0x21u;
    ok &= tavrn_wire_v2_encode(&codec,
                                &(tavrn_decoded_frame_t){
                                    .type = TAVRN_WIRE_HELLO,
                                    .network_id = TEST_NETWORK_ID,
                                    .transmitter = make_peer(adva_b,
                                                             TAVRN_IDENTITY_SID16),
                                    .detail.control = malformed,
                                }, adv, sizeof(adv), &(size_t){0}) ==
        TAVRN_CODEC_MALFORMED_FIELD;
    malformed = make_bootstrap_hello(adva_b, TAVRN_IDENTITY_SID8, 0x1234u);
    ok &= malformed.pdu_len == 17u &&
        tavrn_wire_v2_encode(&codec,
                             &(tavrn_decoded_frame_t){
                                 .type = TAVRN_WIRE_HELLO,
                                 .network_id = TEST_NETWORK_ID,
                                 .transmitter = make_peer(adva_b,
                                                          TAVRN_IDENTITY_SID16),
                                 .detail.control = malformed,
                             }, adv, sizeof(adv), &(size_t){0}) ==
        TAVRN_CODEC_MALFORMED_FIELD;

    ok &= setup_fixture(&fixture, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0x4444u, 0u);
    ok &= deliver_control(&fixture, &n1, adva_b, 2u, 0u) == TAVRN_ROUTER_EVENT_OK &&
        peer_snapshot(&fixture, adva_b, &before) && before.direct_binding_valid != 0u &&
        before.boot_nonce == 0x1234u;
    counters_before = *tavrn_router_incarnation_counters(&fixture.router);
    ok &= deliver_control(&fixture, &n0, adva_b, 3u, 0u) != TAVRN_ROUTER_EVENT_INVALID &&
        peer_snapshot(&fixture, adva_b, &after) && after.boot_nonce == 0x1234u &&
        after.direct_binding_valid == before.direct_binding_valid &&
        tavrn_router_incarnation_counters(&fixture.router)->reset_committed ==
            counters_before.reset_committed;

    /* A second full AdvA with B's standalone SID16 must never become a second
     * direct binding or leave B's route identity usable. */
    malformed = make_bootstrap_hello(adva_b_sid16_collision,
                                     TAVRN_IDENTITY_SID16, 0x5678u);
    counters_before = *tavrn_router_incarnation_counters(&fixture.router);
    ok &= deliver_control(&fixture, &malformed, adva_b_sid16_collision, 31u, 0u) ==
            TAVRN_ROUTER_EVENT_INVALID &&
        peer_snapshot(&fixture, adva_b, &after) && after.route_barred != 0u &&
        tavrn_router_incarnation_peer_snapshot(
            &fixture.router, &collision_adva, &after) ==
            TAVRN_ROUTER_INCARNATION_NOT_FOUND &&
        tavrn_router_incarnation_counters(&fixture.router)->invalid_bootstrap_rejected ==
            counters_before.invalid_bootstrap_rejected + 1u;

    counters_before = *tavrn_router_incarnation_counters(&fixture.router);
    malformed = n1;
    memcpy(&malformed.pdu[11], adva_c, TAVRN_ADVA_LEN);
    ok &= deliver_control(&fixture, &malformed, adva_b, 4u, 1u) ==
            TAVRN_ROUTER_EVENT_IGNORED &&
        peer_snapshot(&fixture, adva_b, &after) && after.boot_nonce == 0x1234u &&
        tavrn_router_incarnation_counters(&fixture.router)->invalid_bootstrap_rejected ==
            counters_before.invalid_bootstrap_rejected + 1u;
    malformed = make_bootstrap_hello(adva_sid16_zero, TAVRN_IDENTITY_SID16, 2u);
    ok &= deliver_control(&fixture, &malformed, adva_sid16_zero, 5u, 1u) ==
        TAVRN_ROUTER_EVENT_IGNORED;
    malformed = make_bootstrap_hello(adva_sid16_reserved, TAVRN_IDENTITY_SID16, 2u);
    ok &= deliver_control(&fixture, &malformed, adva_sid16_reserved, 6u, 1u) ==
        TAVRN_ROUTER_EVENT_IGNORED;
    return ok;
}

static int test_boot_06_tx_done_establishment(void)
{
    incarnation_fixture_t aodv_only;
    incarnation_fixture_t no_tx_done;
    incarnation_fixture_t full;
    tavrn_router_incarnation_snapshot_t state;
    ble_mesh_sched_event_t done;
    int ok = 1;

    ok &= setup_fixture(&aodv_only, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0x5555u, 100u) &&
        snapshot(&aodv_only, &state) &&
        state.state == TAVRN_ROUTER_INCARNATION_REJOINING &&
        control_is_direct_n1(&state.latest_hello, adva_a, 0x5555u) &&
        state.latest_hello_tx_token != BLE_MESH_TX_TOKEN_NONE;
    memset(&done, 0, sizeof(done));
    done.type = BLE_MESH_SCHED_EVENT_TX_DONE;
    done.tx_token = (ble_mesh_tx_token_t)(state.latest_hello_tx_token + 1u);
    done.tx_requested_channel_mask = BLE_MESH_SCHED_CH_ALL;
    done.tx_completed_channel_mask = BLE_MESH_SCHED_CH_ALL;
    (void)tavrn_router_handle_scheduler_event(&aodv_only.router, &done, 105u);
    ok &= snapshot(&aodv_only, &state) &&
        state.state == TAVRN_ROUTER_INCARNATION_REJOINING &&
        state.bootstrap_tx_done_seen == 0u;
    done.tx_token = state.latest_hello_tx_token;
    done.tx_completed_channel_mask = 0u;
    (void)tavrn_router_handle_scheduler_event(&aodv_only.router, &done, 106u);
    ok &= snapshot(&aodv_only, &state) &&
        state.bootstrap_tx_done_seen == 0u;
    done.tx_completed_channel_mask = BLE_MESH_SCHED_CH_ALL;

    /* Every half-window announcement is eligible evidence, but only the first
     * matching nonzero-mask completion establishes the elapsed-time epoch. */
    (void)tavrn_router_tick(&aodv_only.router, 115u);
    ok &= snapshot(&aodv_only, &state) &&
        state.latest_hello_tx_token != BLE_MESH_TX_TOKEN_NONE;
    done.tx_token = state.latest_hello_tx_token;
    (void)tavrn_router_handle_scheduler_event(&aodv_only.router, &done, 116u);
    ok &= snapshot(&aodv_only, &state) && state.bootstrap_tx_done_seen != 0u &&
        state.last_bootstrap_tx_done_ms == 116u;
    (void)tavrn_router_tick(&aodv_only.router, 130u);
    ok &= snapshot(&aodv_only, &state) &&
        state.latest_hello_tx_token != BLE_MESH_TX_TOKEN_NONE;
    done.tx_token = state.latest_hello_tx_token;
    (void)tavrn_router_handle_scheduler_event(&aodv_only.router, &done, 131u);
    (void)tavrn_router_tick(&aodv_only.router, 145u);
    ok &= snapshot(&aodv_only, &state) &&
        state.latest_hello_tx_token != BLE_MESH_TX_TOKEN_NONE;
    done.tx_token = state.latest_hello_tx_token;
    (void)tavrn_router_handle_scheduler_event(&aodv_only.router, &done, 146u);
    ok &= snapshot(&aodv_only, &state) &&
        state.last_bootstrap_tx_done_ms == 116u;
    (void)tavrn_router_tick(&aodv_only.router, 145u);
    ok &= snapshot(&aodv_only, &state) &&
        state.state == TAVRN_ROUTER_INCARNATION_REJOINING;
    (void)tavrn_router_tick(&aodv_only.router, 146u);
    ok &= snapshot(&aodv_only, &state) &&
        state.state == TAVRN_ROUTER_INCARNATION_ESTABLISHED &&
        state.normal_hello_emitted != 0u && control_is_n0(&state.latest_hello, adva_a) &&
        !queued_bootstrap_hello(&aodv_only);

    ok &= setup_fixture(&no_tx_done, TAVRN_ROUTER_FEATURE_AODV_ONLY,
                        TAVRN_IDENTITY_SID16, 0x6666u, 200u);
    (void)tavrn_router_tick(&no_tx_done.router, 1000u);
    ok &= snapshot(&no_tx_done, &state) &&
        state.state == TAVRN_ROUTER_INCARNATION_REJOINING &&
        state.bootstrap_tx_done_seen == 0u && state.normal_hello_emitted == 0u;

    ok &= setup_fixture(&full, TAVRN_ROUTER_FEATURE_FULL_TAVRN,
                        TAVRN_IDENTITY_SID16, 0x7777u, 300u) &&
        snapshot(&full, &state);
    memset(&done, 0, sizeof(done));
    done.type = BLE_MESH_SCHED_EVENT_TX_DONE;
    done.tx_token = state.latest_hello_tx_token;
    done.tx_requested_channel_mask = BLE_MESH_SCHED_CH_ALL;
    done.tx_completed_channel_mask = BLE_MESH_SCHED_CH_ALL;
    (void)tavrn_router_handle_scheduler_event(&full.router, &done, 301u);
    (void)tavrn_router_tick(&full.router, 1000u);
    ok &= snapshot(&full, &state) &&
        state.state == TAVRN_ROUTER_INCARNATION_REJOINING &&
        state.normal_hello_emitted == 0u;
    return ok;
}

static int test_rejoining_status_is_distinct_nonterminal(void)
{
    return TAVRN_ROUTER_EVENT_REJOINING != TAVRN_ROUTER_EVENT_BUSY &&
        TAVRN_ROUTER_EVENT_REJOINING != TAVRN_ROUTER_EVENT_INVALID &&
        TAVRN_ROUTER_EVENT_BUSY != TAVRN_ROUTER_EVENT_INVALID;
}

int main(void)
{
    CHECK("SERIAL-04", test_serial_04_rejoining_and_reset());
    CHECK("SERIAL-04", test_serial_04_committed_reset_retries_once());
    CHECK("SERIAL-04", test_serial_04_collision_prefilter());
    CHECK("SERIAL-04", test_serial_04_queued_local_rerr_is_preserved());
    CHECK("SERIAL-04",
          test_serial_04_external_pins_survive_peer_clear_until_release());
    CHECK("SERIAL-04",
          test_serial_04_pending_ingest_pin_clears_on_transmitter_reset());
    CHECK("SERIAL-04",
          test_serial_04_retained_forward_pin_clears_on_next_hop_reset());
    CHECK("SERIAL-04",
          test_serial_04_queued_forward_pin_clears_on_next_hop_reset());
    CHECK("SERIAL-04", test_serial_04_relay_delivery_cancelled_by_origin());
    CHECK("SERIAL-04",
          test_serial_04_rrep_and_ack_cleanup_through_unrelated_next_hop());
    CHECK("SERIAL-04", test_serial_04_sent_ack_wait_does_not_blacklist_next_hop());
    CHECK("BOOT-01", test_boot_01_vector_identity_and_ordering());
    CHECK("BOOT-06", test_boot_06_tx_done_establishment());
    CHECK("SERIAL-04", test_rejoining_status_is_distinct_nonterminal());
    if (failures != 0u) {
        printf("tavrn_phase4_incarnation RED tests failed: %u assertion(s)\n",
               failures);
        return 1;
    }
    printf("tavrn_phase4_incarnation tests passed\n");
    return 0;
}
