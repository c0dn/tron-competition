#include "aodv_core.h"
#include "ble_mesh_scheduler.h"
#include "tavrn_full.h"
#include "tavrn_gtt.h"
#include "tavrn_link_v2.h"
#include "tavrn_router.h"
#include "tavrn_wire_v2.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAVRN_NETWORK_ID 0x2au
#define RREQ_REQUEST_ID  0x9001u
#define RREP_DESTINATION_SEQUENCE 0x3141u
#define SCHEDULER_TRACE_WRONG_RECEIVER_RREP_STATUS TAVRN_ROUTER_EVENT_IGNORED

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
static const uint8_t adva_d[TAVRN_ADVA_LEN] = {
    0x12u, 0x23u, 0x34u, 0x45u, 0x56u, 0xc2u,
};
static const uint8_t adva_e[TAVRN_ADVA_LEN] = {
    0x13u, 0x24u, 0x35u, 0x46u, 0x57u, 0xc3u,
};
static const uint8_t adva_f[TAVRN_ADVA_LEN] = {
    0x14u, 0x25u, 0x36u, 0x47u, 0x58u, 0xc4u,
};

typedef struct integration_application_recorder {
    tavrn_router_delivery_status_t reserve_status;
    tavrn_router_delivery_status_t commit_status;
    tavrn_router_delivery_status_t cancel_status;
    tavrn_direct_peer_t transmitter;
    tavrn_link_data_t data;
    tavrn_router_delivery_token_t token;
    uint8_t reserve_count;
    uint8_t commit_count;
    uint8_t cancel_count;
    uint8_t reserve_zero_token;
} integration_application_recorder_t;

typedef struct integration_fixture {
    ble_mesh_scheduler_t scheduler;
    tavrn_link_v2_t link;
    aodv_core_t core;
    tavrn_gtt_t gtt;
    tavrn_gtt_storage_t gtt_storage;
    tavrn_full_t full;
    tavrn_router_t router;
    integration_application_recorder_t application_recorder;
} integration_fixture_t;

typedef struct integration_observation_recorder {
    tavrn_router_observation_t observations[8];
    uint32_t observed_at_ms[8];
    uint8_t count;
} integration_observation_recorder_t;

static void record_integration_observation(
    void *context, const tavrn_router_observation_t *observation,
    uint32_t now_ms)
{
    integration_observation_recorder_t *recorder = context;

    if (recorder != NULL && observation != NULL && recorder->count < 8u) {
        recorder->observations[recorder->count] = *observation;
        recorder->observed_at_ms[recorder->count] = now_ms;
        recorder->count++;
    }
}

static tavrn_router_delivery_status_t reserve_application(
    void *context, const tavrn_direct_peer_t *transmitter,
    const tavrn_link_data_t *data, tavrn_router_delivery_token_t *token_out,
    uint32_t now_ms)
{
    integration_application_recorder_t *recorder = context;

    (void)now_ms;
    if (recorder == NULL || transmitter == NULL || data == NULL || token_out == NULL) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    recorder->reserve_count++;
    recorder->transmitter = *transmitter;
    recorder->data = *data;
    if (recorder->reserve_status != TAVRN_ROUTER_DELIVERY_OK) {
        return recorder->reserve_status;
    }
    if (recorder->reserve_zero_token != 0u) {
        *token_out = TAVRN_ROUTER_DELIVERY_TOKEN_NONE;
        return TAVRN_ROUTER_DELIVERY_OK;
    }
    recorder->token++;
    if (recorder->token == TAVRN_ROUTER_DELIVERY_TOKEN_NONE) {
        recorder->token++;
    }
    *token_out = recorder->token;
    return TAVRN_ROUTER_DELIVERY_OK;
}

static tavrn_router_delivery_status_t commit_application(
    void *context, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t now_ms)
{
    integration_application_recorder_t *recorder = context;

    (void)now_ms;
    if (recorder == NULL || transmitter == NULL || data == NULL) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    recorder->commit_count++;
    if (token != recorder->token ||
        memcmp(transmitter, &recorder->transmitter, sizeof(*transmitter)) != 0 ||
        memcmp(data, &recorder->data, sizeof(*data)) != 0) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    return recorder->commit_status;
}

static tavrn_router_delivery_status_t cancel_application(
    void *context, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t now_ms)
{
    integration_application_recorder_t *recorder = context;

    (void)now_ms;
    if (recorder == NULL || transmitter == NULL || data == NULL) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    recorder->cancel_count++;
    if (token != recorder->token ||
        memcmp(transmitter, &recorder->transmitter, sizeof(*transmitter)) != 0 ||
        memcmp(data, &recorder->data, sizeof(*data)) != 0) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    return recorder->cancel_status;
}

static tavrn_direct_peer_t make_peer(const uint8_t adva[TAVRN_ADVA_LEN])
{
    tavrn_direct_peer_t peer;

    memset(&peer, 0, sizeof(peer));
    peer.logical_id.width = TAVRN_IDENTITY_SID16;
    peer.logical_id.value = (uint16_t)adva[0] | ((uint16_t)adva[1] << 8);
    memcpy(peer.adva.bytes, adva, TAVRN_ADVA_LEN);
    return peer;
}

static tavrn_link_config_t make_link_config(void)
{
    tavrn_link_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_peer(adva_a);
    config.network_id = TAVRN_NETWORK_ID;
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

static aodv_core_config_t make_core_config(void)
{
    aodv_core_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_peer(adva_a);
    config.network_id = TAVRN_NETWORK_ID;
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

static tavrn_gtt_config_t make_gtt_config(void)
{
    tavrn_gtt_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_identity = make_peer(adva_a).adva;
    config.soft_expiry_ms = 50u;
    config.hard_expiry_ms = 100u;
    config.departed_retention_ms = 200u;
    return config;
}

static tavrn_codec_config_t make_codec_config(void)
{
    tavrn_codec_config_t config;

    memset(&config, 0, sizeof(config));
    config.network_id = TAVRN_NETWORK_ID;
    config.local_peer = make_peer(adva_a);
    return config;
}

static int setup_fixture(integration_fixture_t *fixture, uint32_t now_ms)
{
    tavrn_link_config_t link_config = make_link_config();
    aodv_core_config_t core_config = make_core_config();
    tavrn_gtt_config_t gtt_config = make_gtt_config();

    if (fixture == NULL) {
        return 0;
    }
    memset(fixture, 0, sizeof(*fixture));
    ble_mesh_scheduler_init(&fixture->scheduler, now_ms, adva_a);
    if (tavrn_link_v2_init(&fixture->link, &fixture->scheduler, &link_config,
                           now_ms) != TAVRN_LINK_INIT_OK ||
        aodv_core_init(&fixture->core, &core_config, now_ms) != AODV_INIT_OK ||
        tavrn_gtt_init(&fixture->gtt, &fixture->gtt_storage, &gtt_config,
                       now_ms) != TAVRN_GTT_INIT_OK) {
        return 0;
    }
    return 1;
}

static int init_router(integration_fixture_t *fixture,
                       const tavrn_router_augmentation_hooks_t *augmentation_or_null)
{
    tavrn_router_application_hooks_t application_hooks;
    integration_application_recorder_t *recorder;

    if (fixture == NULL) {
        return 0;
    }
    recorder = &fixture->application_recorder;
    memset(recorder, 0, sizeof(*recorder));
    recorder->reserve_status = TAVRN_ROUTER_DELIVERY_OK;
    recorder->commit_status = TAVRN_ROUTER_DELIVERY_OK;
    recorder->cancel_status = TAVRN_ROUTER_DELIVERY_OK;
    memset(&application_hooks, 0, sizeof(application_hooks));
    application_hooks.context = recorder;
    application_hooks.reserve = reserve_application;
    application_hooks.commit = commit_application;
    application_hooks.cancel = cancel_application;
    return tavrn_router_init(&fixture->router, &fixture->link, &fixture->core,
                             augmentation_or_null) == TAVRN_ROUTER_INIT_OK &&
        tavrn_router_set_application_hooks(&fixture->router, &application_hooks) ==
            TAVRN_ROUTER_APPLICATION_HOOK_OK;
}

static int bind_full_router(integration_fixture_t *fixture)
{
    tavrn_router_augmentation_hooks_t hooks;

    if (fixture == NULL ||
        tavrn_full_init(&fixture->full, &fixture->gtt) != TAVRN_FULL_INIT_OK) {
        return 0;
    }
    hooks = tavrn_full_router_hooks(&fixture->full);
    return init_router(fixture, &hooks);
}

static tavrn_gtt_evidence_t make_liveness(const uint8_t adva[TAVRN_ADVA_LEN],
                                           uint16_t serial, uint8_t hop_count)
{
    tavrn_gtt_evidence_t evidence;

    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = make_peer(adva).adva;
    evidence.serial = serial;
    evidence.serial_present = 1u;
    evidence.hop_count = hop_count;
    evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    return evidence;
}

static tron_application_data_t make_application(tavrn_logical_id_t destination)
{
    tron_application_data_t application;

    memset(&application, 0, sizeof(application));
    application.final_destination = destination;
    application.app_kind = 0x7fu;
    application.app_source = 0u;
    application.app_len = 1u;
    application.app_bytes[0] = 0xa5u;
    return application;
}

static tavrn_link_data_t make_data(tavrn_logical_id_t origin,
                                    tavrn_logical_id_t destination,
                                    uint16_t sequence,
                                    tavrn_data_ownership_t ownership)
{
    tavrn_link_data_t data;

    memset(&data, 0, sizeof(data));
    data.origin = origin;
    data.final_destination = destination;
    data.data_seq = sequence;
    data.ttl = 15u;
    data.app_kind = 0x7fu;
    data.app_source = 0u;
    data.app_len = 1u;
    data.app_bytes[0] = 0xa5u;
    data.ownership = ownership;
    return data;
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

static tavrn_validated_control_t make_rreq(tavrn_logical_id_t origin,
                                            tavrn_logical_id_t destination,
                                            uint16_t request_id)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_E_RREQ;
    control.pdu_len = 17u;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = TAVRN_NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_E_RREQ;
    control.pdu[5] = 0x10u;
    control.pdu[6] = 0x10u;
    pdu_put_u16(&control, 7u, origin.value);
    pdu_put_u16(&control, 9u, request_id);
    pdu_put_u16(&control, 11u, destination.value);
    pdu_put_u16(&control, 13u, 0u);
    pdu_put_u16(&control, 15u, 1u);
    return control;
}

static tavrn_validated_control_t make_rrep(tavrn_logical_id_t destination,
                                            tavrn_logical_id_t origin,
                                            uint16_t request_id,
                                            uint16_t destination_sequence,
                                            uint16_t lifetime_ms)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_E_RREP;
    control.pdu_len = 19u;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = TAVRN_NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_E_RREP;
    control.pdu[6] = 0x10u;
    pdu_put_u16(&control, 7u, make_peer(adva_a).logical_id.value);
    pdu_put_u16(&control, 9u, destination.value);
    pdu_put_u16(&control, 11u, destination_sequence);
    pdu_put_u16(&control, 13u, origin.value);
    pdu_put_u16(&control, 15u, request_id);
    pdu_put_u16(&control, 17u, lifetime_ms);
    return control;
}

static tavrn_validated_control_t make_rrep_ack(
    const aodv_action_t *rrep_action)
{
    const tavrn_validated_control_t *rrep =
        &rrep_action->detail.control.control;
    tavrn_validated_control_t ack;

    memset(&ack, 0, sizeof(ack));
    ack.type = TAVRN_WIRE_E_RREP_ACK;
    ack.pdu_len = 16u;
    ack.pdu[0] = 0x54u;
    ack.pdu[1] = 0x52u;
    ack.pdu[2] = 0x02u;
    ack.pdu[3] = TAVRN_NETWORK_ID;
    ack.pdu[4] = TAVRN_WIRE_E_RREP_ACK;
    pdu_put_u16(&ack, 6u, make_peer(adva_a).logical_id.value);
    pdu_put_u16(&ack, 8u, pdu_u16(rrep, 9u));
    pdu_put_u16(&ack, 10u, pdu_u16(rrep, 11u));
    pdu_put_u16(&ack, 12u, pdu_u16(rrep, 13u));
    pdu_put_u16(&ack, 14u, pdu_u16(rrep, 15u));
    return ack;
}

static tavrn_validated_control_t make_rerr(
    tavrn_logical_id_t reporter, tavrn_logical_id_t unreachable)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_E_RERR;
    control.pdu_len = 16u;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = TAVRN_NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_E_RERR;
    control.pdu[6] = 0x10u;
    pdu_put_u16(&control, 7u, reporter.value);
    pdu_put_u16(&control, 9u, 0x8201u);
    control.pdu[11] = 1u;
    pdu_put_u16(&control, 12u, unreachable.value);
    pdu_put_u16(&control, 14u, 0x2201u);
    return control;
}

static int make_scheduler_rx_event(const tavrn_decoded_frame_t *frame,
                                   const uint8_t outer_adva[TAVRN_ADVA_LEN],
                                   ble_mesh_sched_event_t *event)
{
    tavrn_codec_config_t config = make_codec_config();
    size_t adv_len = 0u;

    if (frame == NULL || outer_adva == NULL || event == NULL) {
        return 0;
    }
    memset(event, 0, sizeof(*event));
    event->type = BLE_MESH_SCHED_EVENT_RX_ADV;
    event->rssi_magnitude_db = 55u;
    memcpy(event->adv_addr, outer_adva, TAVRN_ADVA_LEN);
    if (tavrn_wire_v2_encode(&config, frame, event->adv_data,
                             sizeof(event->adv_data), &adv_len) != TAVRN_CODEC_OK ||
        adv_len > sizeof(event->adv_data)) {
        return 0;
    }
    event->adv_len = (uint8_t)adv_len;
    return 1;
}

static tavrn_decoded_frame_t make_control_frame(
    const tavrn_validated_control_t *control,
    const uint8_t transmitter_adva[TAVRN_ADVA_LEN])
{
    tavrn_decoded_frame_t frame;

    memset(&frame, 0, sizeof(frame));
    frame.type = control->type;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = make_peer(transmitter_adva);
    frame.detail.control = *control;
    return frame;
}

static tavrn_decoded_frame_t make_flood_frame(void)
{
    tavrn_decoded_frame_t frame;

    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_FLOOD;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = make_peer(adva_c);
    frame.detail.flood.origin = make_peer(adva_b).logical_id;
    frame.detail.flood.flood_seq = 0x8102u;
    frame.detail.flood.ttl = 2u;
    frame.detail.flood.flood_class = 1u;
    frame.detail.flood.body_len = 1u;
    frame.detail.flood.body[0] = 0xa5u;
    return frame;
}

static int logical_id_equal(const tavrn_logical_id_t *left,
                            const tavrn_logical_id_t *right)
{
    return left != NULL && right != NULL && left->width == right->width &&
        left->value == right->value;
}

static int transit_custody_is_pinned(const tavrn_link_v2_t *link,
                                     const tavrn_link_data_t *data)
{
    uint8_t i;

    if (link == NULL || data == NULL) {
        return 0;
    }
    for (i = 0u; i < TAVRN_LINK_DATA_DEDUPE_CAPACITY; i++) {
        const tavrn_data_dedupe_entry_t *entry = &link->data_dedupe[i];

        if (entry->valid != 0u && logical_id_equal(&entry->origin, &data->origin) &&
            entry->data_seq == data->data_seq && entry->app_kind == data->app_kind &&
            entry->app_source == data->app_source) {
            return entry->custody_pinned != 0u;
        }
    }
    return 0;
}

static int drain_actions(aodv_core_t *core)
{
    aodv_action_t action;

    if (core == NULL) {
        return 0;
    }
    while (aodv_core_poll_action(core, &action) == AODV_ACTION_POLL_OK) {
    }
    return aodv_core_poll_action(core, &action) == AODV_ACTION_POLL_EMPTY;
}

/* Every dispatch provides its typed local event.  These paths require NONE;
 * a pending-data or blacklist event is a test failure rather than discarded. */
static tavrn_router_event_status_t dispatch_without_local_event(
    tavrn_router_t *router, uint32_t now_ms)
{
    tavrn_router_dispatch_event_t event;
    tavrn_router_event_status_t status;

    memset(&event, 0, sizeof(event));
    status = tavrn_router_dispatch_ex(router, now_ms, &event);
    if (status != TAVRN_ROUTER_EVENT_INVALID &&
        event.type != TAVRN_ROUTER_DISPATCH_EVENT_NONE) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    return status;
}

static int install_route_via(integration_fixture_t *fixture,
                             tavrn_logical_id_t destination,
                             const tavrn_direct_peer_t *next_hop,
                             uint32_t now_ms)
{
    tron_application_data_t application = make_application(destination);
    tavrn_validated_control_t reply;
    tavrn_decoded_frame_t frame;
    ble_mesh_sched_event_t scheduler_event;
    aodv_action_t action;
    aodv_route_snapshot_t route;

    if (fixture == NULL || next_hop == NULL ||
        tavrn_router_submit_application(&fixture->router, &application, now_ms) !=
            AODV_STATUS_QUEUED ||
        aodv_core_poll_action(&fixture->core, &action) != AODV_ACTION_POLL_OK ||
        action.type != AODV_ACTION_SEND_RREQ) {
        return 0;
    }
    reply = make_rrep(destination, make_peer(adva_a).logical_id,
                      pdu_u16(&action.detail.control.control, 9u), 0x4321u,
                      100u);
    frame = make_control_frame(&reply, next_hop->adva.bytes);
    if (!make_scheduler_rx_event(&frame, next_hop->adva.bytes, &scheduler_event) ||
        tavrn_router_handle_scheduler_event(&fixture->router, &scheduler_event,
                                            now_ms + 1u) != TAVRN_ROUTER_EVENT_OK ||
        aodv_core_route_snapshot(&fixture->core, &destination, &route) !=
            AODV_ROUTE_QUERY_FOUND ||
        route.state != AODV_ROUTE_VALID ||
        memcmp(route.next_hop.adva.bytes, next_hop->adva.bytes, TAVRN_ADVA_LEN) != 0) {
        return 0;
    }
    return drain_actions(&fixture->core);
}

static int admit_pinned_transit(integration_fixture_t *fixture,
                                tavrn_link_data_t *data_out,
                                tavrn_link_data_t *forwarded_data_out,
                                uint32_t now_ms)
{
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_decoded_frame_t frame;
    ble_mesh_sched_event_t scheduler_event;
    aodv_action_t action;
    tavrn_link_data_t data = make_data(make_peer(adva_d).logical_id,
                                       peer_c.logical_id, 0x7a01u,
                                       TAVRN_DATA_TRANSIT);

    data.ttl = 3u;
    if (fixture == NULL ||
        !install_route_via(fixture, peer_c.logical_id, &peer_b, now_ms)) {
        return 0;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = make_peer(adva_d);
    frame.detail.data.immediate_receiver = make_peer(adva_a).logical_id;
    frame.detail.data.data = data;
    if (!make_scheduler_rx_event(&frame, adva_d, &scheduler_event) ||
        tavrn_router_handle_scheduler_event(&fixture->router, &scheduler_event,
                                            now_ms + 2u) != TAVRN_ROUTER_EVENT_OK ||
        aodv_core_poll_action(&fixture->core, &action) != AODV_ACTION_POLL_OK ||
        action.type != AODV_ACTION_FORWARD_DATA ||
        !transit_custody_is_pinned(&fixture->link, &data)) {
        return 0;
    }
    if (data_out != NULL) {
        *data_out = data;
    }
    if (forwarded_data_out != NULL) {
        *forwarded_data_out = action.detail.data.data;
    }
    return 1;
}

static int enqueue_data_priority_filler(ble_mesh_scheduler_t *scheduler,
                                        uint32_t now_ms)
{
    ble_mesh_tx_item_t item;
    ble_mesh_sched_enqueue_result_t result;

    if (scheduler == NULL) {
        return 0;
    }
    memset(&item, 0, sizeof(item));
    item.adv_len = 1u;
    item.adv_data[0] = 0xa5u;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = BLE_MESH_TX_PRIORITY_DATA;
    item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
    item.not_before_ms = now_ms + 1000u;
    result = ble_mesh_scheduler_enqueue_ex(scheduler, &item);
    return result.status == BLE_MESH_SCHED_ENQUEUE_OK;
}

static int enqueue_control_priority_filler(ble_mesh_scheduler_t *scheduler,
                                            uint32_t now_ms)
{
    ble_mesh_tx_item_t item;
    ble_mesh_sched_enqueue_result_t result;

    if (scheduler == NULL) {
        return 0;
    }
    memset(&item, 0, sizeof(item));
    item.adv_len = 1u;
    item.adv_data[0] = 0xa5u;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = BLE_MESH_TX_PRIORITY_CONTROL;
    item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
    item.not_before_ms = now_ms + 1000u;
    result = ble_mesh_scheduler_enqueue_ex(scheduler, &item);
    return result.status == BLE_MESH_SCHED_ENQUEUE_OK;
}

static uint8_t failure_obligation_count(const tavrn_router_t *router)
{
    uint8_t count = 0u;
    uint8_t i;

    if (router == NULL) {
        return 0u;
    }
    for (i = 0u; i < TAVRN_ROUTER_FAILURE_CAPACITY; i++) {
        if (router->failures[i].valid != 0u) {
            count++;
        }
    }
    return count;
}

static int prepare_evictable_transit(integration_fixture_t *fixture,
                                     tavrn_link_data_t *inbound_data_out,
                                     uint32_t now_ms)
{
    tavrn_link_data_t inbound_data;
    tavrn_link_data_t forwarded_data;
    tavrn_link_event_t local_outcome;
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    uint8_t filler_count = 0u;

    if (!admit_pinned_transit(fixture, &inbound_data, &forwarded_data, now_ms) ||
        tavrn_link_v2_send_unicast(&fixture->link, &peer_b, &forwarded_data,
                                   now_ms + 3u,
                                   &local_outcome) != TAVRN_LINK_SEND_OK ||
        tavrn_link_v2_dispatch(&fixture->link, now_ms + 3u, &local_outcome) !=
            TAVRN_LINK_STEP_NO_EVENT) {
        return 0;
    }
    while (fixture->scheduler.routed_tx_queue.count < BLE_MESH_TX_QUEUE_CAPACITY) {
        if (!enqueue_data_priority_filler(&fixture->scheduler, now_ms) ||
            ++filler_count > BLE_MESH_TX_QUEUE_CAPACITY) {
            return 0;
        }
    }
    if (inbound_data_out != NULL) {
        *inbound_data_out = inbound_data;
    }
    return 1;
}

static void test_link_02_stale_candidate_has_no_second_action(void)
{
    integration_fixture_t fixture;
    tavrn_decoded_frame_t frame;
    ble_mesh_sched_event_t scheduler_event;
    tavrn_link_event_t candidate_event;
    tavrn_link_event_t stale_candidate;
    tavrn_direct_peer_t local = make_peer(adva_a);
    tavrn_link_data_t data = make_data(make_peer(adva_d).logical_id,
                                       local.logical_id, 0x7b01u,
                                       TAVRN_DATA_TRANSIT);
    aodv_action_t action;
    aodv_core_t core_before_stale;

    if (!setup_fixture(&fixture, 0u) || !init_router(&fixture, NULL)) {
        CHECK("LINK-02", 0);
        return;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = make_peer(adva_b);
    frame.detail.data.immediate_receiver = local.logical_id;
    frame.detail.data.data = data;
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event) ||
        tavrn_link_v2_on_scheduler_event(&fixture.link, &scheduler_event, 1u,
                                         &candidate_event) !=
            TAVRN_LINK_STEP_EVENT ||
        candidate_event.type != TAVRN_LINK_EVENT_RX_DATA_CANDIDATE) {
        CHECK("LINK-02", 0);
        return;
    }
    stale_candidate = candidate_event;
    CHECK("LINK-02", tavrn_router_handle_link_event(&fixture.router,
                                                       &candidate_event, 1u) ==
                         TAVRN_ROUTER_EVENT_OK &&
                         aodv_core_poll_action(&fixture.core, &action) ==
                             AODV_ACTION_POLL_OK &&
                         action.type == AODV_ACTION_DELIVER_DATA);
    core_before_stale = fixture.core;
    (void)tavrn_router_handle_link_event(&fixture.router, &stale_candidate, 2u);
    CHECK("LINK-02", aodv_core_poll_action(&fixture.core, &action) ==
                         AODV_ACTION_POLL_EMPTY &&
                         memcmp(&fixture.core, &core_before_stale,
                                sizeof(fixture.core)) == 0 &&
                         fixture.link.counters.rx_candidate_accepted == 1u);
}

static void test_gtt_03_duplicate_data_and_flood_evidence(void)
{
    integration_fixture_t fixture;
    integration_observation_recorder_t recorder;
    tavrn_router_augmentation_hooks_t hooks;
    tavrn_decoded_frame_t frame;
    ble_mesh_sched_event_t scheduler_event;
    tavrn_direct_peer_t local = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_direct_peer_t peer_d = make_peer(adva_d);
    tavrn_link_data_t data = make_data(peer_d.logical_id, local.logical_id,
                                       0x7b02u, TAVRN_DATA_TRANSIT);
    aodv_action_t action;
    uint8_t relay_count;

    if (!setup_fixture(&fixture, 0u)) {
        CHECK("GTT-03", 0);
        return;
    }
    memset(&recorder, 0, sizeof(recorder));
    memset(&hooks, 0, sizeof(hooks));
    hooks.context = &recorder;
    hooks.observe = record_integration_observation;
    if (!init_router(&fixture, &hooks)) {
        CHECK("GTT-03", 0);
        return;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = peer_b;
    frame.detail.data.immediate_receiver = local.logical_id;
    frame.detail.data.data = data;
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    CHECK("GTT-03", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 1u) ==
                         TAVRN_ROUTER_EVENT_OK &&
                         aodv_core_poll_action(&fixture.core, &action) ==
                             AODV_ACTION_POLL_OK &&
                         action.type == AODV_ACTION_DELIVER_DATA);
    (void)tavrn_router_handle_scheduler_event(&fixture.router, &scheduler_event, 2u);
    CHECK("GTT-03", recorder.count == 2u &&
                         recorder.observations[1].frame_type == TAVRN_WIRE_DATA &&
                         memcmp(recorder.observations[1].transmitter.adva.bytes,
                                peer_b.adva.bytes, TAVRN_ADVA_LEN) == 0 &&
                         recorder.observations[1].subject_count == 1u &&
                         recorder.observations[1].subjects[0].role ==
                             TAVRN_ROUTER_EVIDENCE_ORIGIN &&
                         recorder.observations[1].subjects[0].logical_id.value ==
                             peer_d.logical_id.value &&
                         aodv_core_poll_action(&fixture.core, &action) ==
                             AODV_ACTION_POLL_EMPTY);

    frame = make_flood_frame();
    if (!make_scheduler_rx_event(&frame, adva_c, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    CHECK("GTT-03", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 3u) ==
                         TAVRN_ROUTER_EVENT_OK && recorder.count == 3u &&
                         fixture.link.counters.rx_flood_committed == 1u);
    relay_count = fixture.scheduler.routed_tx_queue.count;
    (void)tavrn_router_handle_scheduler_event(&fixture.router, &scheduler_event, 4u);
    CHECK("GTT-03", relay_count != 0u && recorder.count == 4u &&
                         recorder.observations[3].frame_type == TAVRN_WIRE_FLOOD &&
                         memcmp(recorder.observations[3].transmitter.adva.bytes,
                                peer_c.adva.bytes, TAVRN_ADVA_LEN) == 0 &&
                         recorder.observations[3].subject_count == 1u &&
                         recorder.observations[3].subjects[0].role ==
                             TAVRN_ROUTER_EVIDENCE_ORIGIN &&
                         recorder.observations[3].subjects[0].logical_id.value ==
                             peer_b.logical_id.value &&
                         fixture.scheduler.routed_tx_queue.count == relay_count);
}

static void test_gtt_03_max_hops_reports_direct_transmitter_only(void)
{
    integration_fixture_t fixture;
    integration_observation_recorder_t recorder;
    tavrn_router_augmentation_hooks_t hooks;
    tavrn_decoded_frame_t frame;
    tavrn_validated_control_t control;
    ble_mesh_sched_event_t scheduler_event;
    tavrn_direct_peer_t local = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_direct_peer_t peer_d = make_peer(adva_d);
    tavrn_link_data_t data = make_data(peer_d.logical_id, local.logical_id,
                                       0x7b03u, TAVRN_DATA_TRANSIT);
    aodv_action_t action;

    if (!setup_fixture(&fixture, 0u)) {
        CHECK("GTT-03", 0);
        return;
    }
    memset(&recorder, 0, sizeof(recorder));
    memset(&hooks, 0, sizeof(hooks));
    hooks.context = &recorder;
    hooks.observe = record_integration_observation;
    if (!init_router(&fixture, &hooks)) {
        CHECK("GTT-03", 0);
        return;
    }
    data.hops = 15u;
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = peer_b;
    frame.detail.data.immediate_receiver = local.logical_id;
    frame.detail.data.data = data;
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    CHECK("GTT-03", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 1u) ==
                         TAVRN_ROUTER_EVENT_OK && recorder.count == 1u &&
                         recorder.observations[0].subject_count == 0u &&
                         memcmp(recorder.observations[0].transmitter.adva.bytes,
                                peer_b.adva.bytes, TAVRN_ADVA_LEN) == 0 &&
                         aodv_core_poll_action(&fixture.core, &action) ==
                             AODV_ACTION_POLL_OK &&
                         action.type == AODV_ACTION_DELIVER_DATA);

    frame = make_flood_frame();
    frame.detail.flood.hops = 15u;
    if (!make_scheduler_rx_event(&frame, adva_c, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    CHECK("GTT-03", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 2u) ==
                         TAVRN_ROUTER_EVENT_OK && recorder.count == 2u &&
                         recorder.observations[1].subject_count == 0u &&
                         memcmp(recorder.observations[1].transmitter.adva.bytes,
                                peer_c.adva.bytes, TAVRN_ADVA_LEN) == 0);

    control = make_rreq(peer_d.logical_id, local.logical_id, 0x7b04u);
    control.pdu[6] = (uint8_t)((control.pdu[6] & 0xf0u) | 15u);
    frame = make_control_frame(&control, adva_b);
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    CHECK("GTT-03", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 3u) ==
                         TAVRN_ROUTER_EVENT_OK && recorder.count == 3u &&
                         recorder.observations[2].frame_type == TAVRN_WIRE_E_RREQ &&
                         recorder.observations[2].subject_count == 0u &&
                         memcmp(recorder.observations[2].transmitter.adva.bytes,
                                peer_b.adva.bytes, TAVRN_ADVA_LEN) == 0);
}

static void test_aodv_02_hop_15_transit_is_rejected(void)
{
    integration_fixture_t fixture;
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_decoded_frame_t frame;
    ble_mesh_sched_event_t scheduler_event;
    tavrn_link_data_t data = make_data(make_peer(adva_d).logical_id,
                                       peer_c.logical_id, 0x7b05u,
                                       TAVRN_DATA_TRANSIT);
    aodv_action_t action;

    if (!setup_fixture(&fixture, 0u) || !init_router(&fixture, NULL) ||
        !install_route_via(&fixture, peer_c.logical_id, &peer_b, 0u)) {
        CHECK("AODV-02", 0);
        return;
    }
    data.ttl = 1u;
    data.hops = 15u;
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = make_peer(adva_d);
    frame.detail.data.immediate_receiver = make_peer(adva_a).logical_id;
    frame.detail.data.data = data;
    if (!make_scheduler_rx_event(&frame, adva_d, &scheduler_event)) {
        CHECK("AODV-02", 0);
        return;
    }
    CHECK("AODV-02", tavrn_router_handle_scheduler_event(
                          &fixture.router, &scheduler_event, 2u) ==
                          TAVRN_ROUTER_EVENT_IGNORED &&
                          aodv_core_poll_action(&fixture.core, &action) ==
                              AODV_ACTION_POLL_EMPTY &&
                          fixture.link.counters.rx_candidate_accepted == 0u &&
                          fixture.link.counters.rx_candidate_rejected == 1u);
}

static void test_link_04_terminal_outcomes_release_transit_custody(void)
{
    static const tavrn_link_event_type_t terminal_types[] = {
        TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED,
        TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED,
        TAVRN_LINK_EVENT_CUSTODY_BUSY_EXPIRED,
        TAVRN_LINK_EVENT_CUSTODY_REJECTED,
        TAVRN_LINK_EVENT_RETRY_EXHAUSTED,
        TAVRN_LINK_EVENT_RADIO_FAULT_TERMINAL,
        TAVRN_LINK_EVENT_SERVICE_FAULT_TERMINAL,
    };
    uint8_t i;

    for (i = 0u; i < sizeof(terminal_types) / sizeof(terminal_types[0]); i++) {
        integration_fixture_t fixture;
        tavrn_link_data_t inbound_data;
        tavrn_link_data_t forwarded_data;
        tavrn_link_event_t event;
        tavrn_direct_peer_t peer_b = make_peer(adva_b);
        tavrn_direct_peer_t peer_c = make_peer(adva_c);
        aodv_route_snapshot_t route;
        aodv_action_t action;

        if (!setup_fixture(&fixture, 0u) || !init_router(&fixture, NULL) ||
            !admit_pinned_transit(&fixture, &inbound_data, &forwarded_data,
                                  (uint32_t)i * 10u)) {
            CHECK("LINK-04", 0);
            return;
        }
        memset(&event, 0, sizeof(event));
        event.type = terminal_types[i];
        if (event.type == TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED) {
            event.detail.transferred_data.next_hop = peer_b;
            event.detail.transferred_data.data = forwarded_data;
            event.detail.transferred_data.status = TAVRN_HACK_ACCEPTED;
        } else {
            event.detail.owned_data.next_hop = peer_b;
            event.detail.owned_data.data = forwarded_data;
        }
        CHECK("LINK-04", tavrn_router_handle_link_event(&fixture.router, &event,
                                                           (uint32_t)i * 10u + 3u) ==
                             TAVRN_ROUTER_EVENT_OK &&
                         tavrn_link_v2_release_rx_custody(&fixture.link,
                                                          &inbound_data,
                                                          (uint32_t)i * 10u + 3u) ==
                             TAVRN_LINK_RESOLVE_TOKEN_INVALID);
        if (event.type == TAVRN_LINK_EVENT_RETRY_EXHAUSTED) {
            CHECK("AODV-06", aodv_core_route_snapshot(&fixture.core,
                                                         &peer_c.logical_id,
                                                         &route) ==
                                  AODV_ROUTE_QUERY_FOUND &&
                                  route.state == AODV_ROUTE_INVALID &&
                                  aodv_core_poll_action(&fixture.core, &action) ==
                                      AODV_ACTION_POLL_OK &&
                                  action.type == AODV_ACTION_SEND_RERR);
        } else {
            CHECK("AODV-06", aodv_core_route_snapshot(&fixture.core,
                                                         &peer_c.logical_id,
                                                         &route) ==
                                  AODV_ROUTE_QUERY_FOUND &&
                                  route.state == AODV_ROUTE_VALID &&
                                  aodv_core_poll_action(&fixture.core, &action) ==
                                      AODV_ACTION_POLL_EMPTY);
        }
        (void)tavrn_router_handle_link_event(&fixture.router, &event,
                                             (uint32_t)i * 10u + 4u);
        CHECK("LINK-04", tavrn_link_v2_release_rx_custody(&fixture.link,
                                                            &inbound_data,
                                                            (uint32_t)i * 10u + 4u) ==
                             TAVRN_LINK_RESOLVE_TOKEN_INVALID);
    }
}

static void test_link_02_busy_paths_consume_evicted_transit_custody(void)
{
    integration_fixture_t fixture;
    tavrn_link_data_t inbound_data;
    tavrn_link_data_t candidate_data;
    tavrn_decoded_frame_t frame;
    ble_mesh_sched_event_t scheduler_event;
    tavrn_link_event_t candidate_event;
    aodv_route_snapshot_t route;
    tavrn_direct_peer_t peer_c = make_peer(adva_c);

    if (!setup_fixture(&fixture, 0u) || !init_router(&fixture, NULL) ||
        !prepare_evictable_transit(&fixture, &inbound_data, 0u)) {
        CHECK("LINK-02", 0);
        return;
    }
    candidate_data = make_data(make_peer(adva_d).logical_id,
                               make_peer(adva_a).logical_id, 0x7b10u,
                               TAVRN_DATA_TRANSIT);
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = make_peer(adva_d);
    frame.detail.data.immediate_receiver = make_peer(adva_a).logical_id;
    frame.detail.data.data = candidate_data;
    if (!make_scheduler_rx_event(&frame, adva_d, &scheduler_event) ||
        tavrn_link_v2_on_scheduler_event(&fixture.link, &scheduler_event, 10u,
                                         &candidate_event) !=
            TAVRN_LINK_STEP_EVENT ||
        candidate_event.type != TAVRN_LINK_EVENT_RX_DATA_CANDIDATE) {
        CHECK("LINK-02", 0);
        return;
    }
    candidate_data.data_seq++;
    frame.detail.data.data = candidate_data;
    if (!make_scheduler_rx_event(&frame, adva_d, &scheduler_event)) {
        CHECK("LINK-02", 0);
        return;
    }
    (void)tavrn_router_handle_scheduler_event(&fixture.router, &scheduler_event,
                                              11u);
    CHECK("LINK-02", fixture.link.counters.rx_additional_data_busy == 1u &&
                         fixture.link.counters.local_tx_not_attempted == 1u &&
                         !transit_custody_is_pinned(&fixture.link, &inbound_data) &&
                         aodv_core_route_snapshot(&fixture.core,
                                                  &peer_c.logical_id,
                                                  &route) == AODV_ROUTE_QUERY_FOUND &&
                         route.state == AODV_ROUTE_VALID);

    if (!setup_fixture(&fixture, 0u) || !init_router(&fixture, NULL) ||
        !prepare_evictable_transit(&fixture, &inbound_data, 0u)) {
        CHECK("LINK-02", 0);
        return;
    }
    candidate_data = make_data(make_peer(adva_d).logical_id,
                               make_peer(adva_a).logical_id, 0x7b11u,
                               TAVRN_DATA_TRANSIT);
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = make_peer(adva_d);
    frame.detail.data.immediate_receiver = make_peer(adva_a).logical_id;
    frame.detail.data.data = candidate_data;
    if (!make_scheduler_rx_event(&frame, adva_d, &scheduler_event) ||
        tavrn_link_v2_on_scheduler_event(&fixture.link, &scheduler_event, 10u,
                                         &candidate_event) !=
            TAVRN_LINK_STEP_EVENT ||
        candidate_event.type != TAVRN_LINK_EVENT_RX_DATA_CANDIDATE) {
        CHECK("LINK-02", 0);
        return;
    }
    CHECK("LINK-02", tavrn_router_tick(&fixture.router,
                                        10u + fixture.link.config.candidate_resolve_ms) ==
                         AODV_STATUS_OK && fixture.link.candidate_valid == 0u &&
                         fixture.link.counters.rx_candidate_timeout_busy == 1u &&
                         fixture.link.counters.local_tx_not_attempted == 1u &&
                         !transit_custody_is_pinned(&fixture.link, &inbound_data));
}

static void test_aodv_06_backpressured_retry_exhausted_remains_due(void)
{
    integration_fixture_t fixture;
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_direct_peer_t peer_d = make_peer(adva_d);
    tron_application_data_t application;
    tavrn_link_event_t event_b;
    tavrn_link_event_t event_c;
    aodv_route_snapshot_t route_c;
    aodv_route_snapshot_t route_d;
    uint8_t i;

    if (!setup_fixture(&fixture, 0u) || !init_router(&fixture, NULL) ||
        !install_route_via(&fixture, peer_c.logical_id, &peer_b, 0u) ||
        !install_route_via(&fixture, peer_d.logical_id, &peer_c, 2u)) {
        CHECK("AODV-06", 0);
        return;
    }
    for (i = 0u; i < TAVRN_AODV_ACTION_CAPACITY; i++) {
        application = make_application((i & 1u) == 0u ? peer_c.logical_id :
                                                        peer_d.logical_id);
        CHECK("AODV-06", tavrn_router_submit_application(&fixture.router,
                                                           &application, 10u + i) ==
                             AODV_STATUS_OK);
    }
    memset(&event_b, 0, sizeof(event_b));
    event_b.type = TAVRN_LINK_EVENT_RETRY_EXHAUSTED;
    event_b.detail.owned_data.next_hop = peer_b;
    event_b.detail.owned_data.data = make_data(make_peer(adva_a).logical_id,
                                               peer_c.logical_id, 0x7b20u,
                                                TAVRN_DATA_ORIGINATED);
    memset(&event_c, 0, sizeof(event_c));
    event_c.type = TAVRN_LINK_EVENT_RETRY_EXHAUSTED;
    event_c.detail.owned_data.next_hop = peer_c;
    event_c.detail.owned_data.data = make_data(make_peer(adva_a).logical_id,
                                                peer_d.logical_id, 0x7b21u,
                                                TAVRN_DATA_ORIGINATED);
    (void)tavrn_router_handle_link_event(&fixture.router, &event_b, 20u);
    (void)tavrn_router_handle_link_event(&fixture.router, &event_c, 20u);
    CHECK("AODV-06", aodv_core_route_snapshot(&fixture.core, &peer_c.logical_id,
                                                &route_c) == AODV_ROUTE_QUERY_FOUND &&
                         route_c.state == AODV_ROUTE_VALID &&
                         aodv_core_route_snapshot(&fixture.core,
                                                  &peer_d.logical_id,
                                                  &route_d) == AODV_ROUTE_QUERY_FOUND &&
                         route_d.state == AODV_ROUTE_VALID &&
                         drain_actions(&fixture.core));
    CHECK("AODV-06", tavrn_router_tick(&fixture.router, 21u) == AODV_STATUS_BUSY &&
                           tavrn_router_tick(&fixture.router, 22u) == AODV_STATUS_OK &&
                          aodv_core_route_snapshot(&fixture.core, &peer_c.logical_id,
                                                   &route_c) == AODV_ROUTE_QUERY_FOUND &&
                          route_c.state == AODV_ROUTE_INVALID &&
                          aodv_core_route_snapshot(&fixture.core,
                                                   &peer_d.logical_id,
                                                   &route_d) == AODV_ROUTE_QUERY_FOUND &&
                          route_d.state == AODV_ROUTE_INVALID);
}

static void test_gtt_03_real_event_role_extraction(void)
{
    integration_fixture_t fixture;
    integration_observation_recorder_t recorder;
    tavrn_router_augmentation_hooks_t hooks;
    tavrn_direct_peer_t local = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_direct_peer_t peer_d = make_peer(adva_d);
    tavrn_decoded_frame_t frame;
    tavrn_validated_control_t control;
    ble_mesh_sched_event_t scheduler_event;
    aodv_action_t action;

    if (!setup_fixture(&fixture, 0u)) {
        CHECK("GTT-03", 0);
        return;
    }
    memset(&recorder, 0, sizeof(recorder));
    memset(&hooks, 0, sizeof(hooks));
    hooks.context = &recorder;
    hooks.observe = record_integration_observation;
    if (!init_router(&fixture, &hooks)) {
        CHECK("GTT-03", 0);
        return;
    }

    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = peer_b;
    frame.detail.data.immediate_receiver = local.logical_id;
    frame.detail.data.data = make_data(peer_d.logical_id, local.logical_id,
                                       0x7201u, TAVRN_DATA_TRANSIT);
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    CHECK("GTT-03", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 1u) ==
                         TAVRN_ROUTER_EVENT_OK && recorder.count == 1u &&
                         recorder.observed_at_ms[0] == 1u &&
                         recorder.observations[0].frame_type == TAVRN_WIRE_DATA &&
                         recorder.observations[0].subject_count == 1u &&
                         recorder.observations[0].subjects[0].role ==
                             TAVRN_ROUTER_EVIDENCE_ORIGIN &&
                         recorder.observations[0].subjects[0].logical_id.value ==
                             peer_d.logical_id.value &&
                         recorder.observations[0].subjects[0].serial_kind ==
                             TAVRN_ROUTER_EVIDENCE_SERIAL_NONE);
    memset(&action, 0, sizeof(action));
    CHECK("GTT-03", aodv_core_poll_action(&fixture.core, &action) ==
                        AODV_ACTION_POLL_OK &&
                        action.type == AODV_ACTION_DELIVER_DATA);

    control = make_rreq(peer_d.logical_id, local.logical_id, RREQ_REQUEST_ID);
    frame = make_control_frame(&control, adva_b);
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    CHECK("GTT-03", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 2u) ==
                         TAVRN_ROUTER_EVENT_OK && recorder.count == 2u &&
                         recorder.observed_at_ms[1] == 2u &&
                         recorder.observations[1].frame_type == TAVRN_WIRE_E_RREQ &&
                         recorder.observations[1].subject_count == 1u &&
                         recorder.observations[1].subjects[0].role ==
                             TAVRN_ROUTER_EVIDENCE_ORIGIN &&
                         recorder.observations[1].subjects[0].logical_id.value ==
                             peer_d.logical_id.value &&
                         recorder.observations[1].subjects[0].serial_kind ==
                             TAVRN_ROUTER_EVIDENCE_SERIAL_NODE_SEQUENCE &&
                         recorder.observations[1].subjects[0].serial == 1u &&
                         recorder.observations[1].subjects[0].hop_present != 0u &&
                         recorder.observations[1].subjects[0].hop_count == 1u);
    memset(&action, 0, sizeof(action));
    CHECK("GTT-03", aodv_core_poll_action(&fixture.core, &action) ==
                        AODV_ACTION_POLL_OK &&
                        action.type == AODV_ACTION_SEND_RREP &&
                        action.detail.control.token != 0u &&
                        aodv_core_mark_action_sent(
                            &fixture.core, action.detail.control.token, 2u) ==
                            AODV_STATUS_OK);

    control = make_rrep_ack(&action);
    frame = make_control_frame(&control, adva_b);
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    CHECK("GTT-03", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 3u) ==
                         TAVRN_ROUTER_EVENT_OK && recorder.count == 3u &&
                         recorder.observed_at_ms[2] == 3u &&
                         recorder.observations[2].frame_type ==
                             TAVRN_WIRE_E_RREP_ACK &&
                         recorder.observations[2].subject_count == 0u);

    control = make_rerr(peer_d.logical_id, peer_c.logical_id);
    frame = make_control_frame(&control, adva_b);
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    CHECK("GTT-03", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 4u) ==
                         TAVRN_ROUTER_EVENT_OK && recorder.count == 4u &&
                         recorder.observed_at_ms[3] == 4u &&
                         recorder.observations[3].frame_type == TAVRN_WIRE_E_RERR &&
                         recorder.observations[3].subject_count == 1u &&
                         recorder.observations[3].subjects[0].role ==
                             TAVRN_ROUTER_EVIDENCE_REPORTER &&
                         recorder.observations[3].subjects[0].logical_id.value ==
                             peer_d.logical_id.value &&
                         recorder.observations[3].subjects[0].serial_kind ==
                             TAVRN_ROUTER_EVIDENCE_SERIAL_NONE);
}

static void test_gtt_03_reachable_event_binding(void)
{
    integration_fixture_t fixture;
    tavrn_decoded_frame_t frame;
    tavrn_link_event_t transferred;
    ble_mesh_sched_event_t scheduler_event;
    tavrn_router_event_status_t router_status;
    aodv_action_t action;
    aodv_route_snapshot_t route;
    tavrn_gtt_snapshot_t snapshot;
    tavrn_validated_control_t control;
    tavrn_direct_peer_t local = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_direct_peer_t peer_d = make_peer(adva_d);
    tavrn_logical_id_t unknown_origin;

    if (!setup_fixture(&fixture, 0u) || !bind_full_router(&fixture)) {
        CHECK("GTT-03", 0);
        return;
    }

    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = peer_b;
    frame.detail.data.immediate_receiver = local.logical_id;
    frame.detail.data.data = make_data(peer_d.logical_id, local.logical_id,
                                       0x7101u, TAVRN_DATA_TRANSIT);
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK("GTT-03", tavrn_gtt_snapshot(&fixture.gtt, &peer_b.adva, 1u,
                                           &snapshot) == TAVRN_GTT_QUERY_NOT_FOUND);
    router_status = tavrn_router_handle_scheduler_event(
        &fixture.router, &scheduler_event, 1u);
    CHECK("GTT-03", router_status == TAVRN_ROUTER_EVENT_OK &&
                        fixture.link.candidate_valid == 0u &&
                        fixture.link.counters.rx_candidate == 1u &&
                        fixture.link.counters.rx_candidate_accepted == 1u);
    memset(&action, 0, sizeof(action));
    CHECK("GTT-03", aodv_core_poll_action(&fixture.core, &action) ==
                        AODV_ACTION_POLL_OK &&
                        action.type == AODV_ACTION_DELIVER_DATA &&
                        action.detail.data.data.data_seq == 0x7101u);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK("GTT-03", tavrn_gtt_snapshot(&fixture.gtt, &peer_b.adva, 1u,
                                          &snapshot) == TAVRN_GTT_QUERY_FOUND &&
                        memcmp(snapshot.identity.bytes, adva_b,
                               TAVRN_ADVA_LEN) == 0);

    control = make_rreq(peer_d.logical_id, peer_c.logical_id, RREQ_REQUEST_ID);
    frame = make_control_frame(&control, adva_b);
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    CHECK("GTT-03", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 2u) ==
                         TAVRN_ROUTER_EVENT_OK &&
                         fixture.link.counters.rx_control == 1u);
    memset(&route, 0, sizeof(route));
    CHECK("GTT-03", aodv_core_route_snapshot(&fixture.core, &peer_d.logical_id,
                                                &route) == AODV_ROUTE_QUERY_FOUND &&
                        route.state == AODV_ROUTE_VALID &&
                        route.next_hop.logical_id.value == peer_b.logical_id.value &&
                        route.hop_count == 1u);
    memset(&action, 0, sizeof(action));
    CHECK("GTT-03", aodv_core_poll_action(&fixture.core, &action) ==
                        AODV_ACTION_POLL_OK &&
                        action.type == AODV_ACTION_SEND_RREQ);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK("GTT-03", tavrn_gtt_snapshot(&fixture.gtt, &peer_c.adva, 2u,
                                          &snapshot) == TAVRN_GTT_QUERY_NOT_FOUND);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK("GTT-03", tavrn_gtt_snapshot(&fixture.gtt, &peer_d.adva, 2u,
                                          &snapshot) == TAVRN_GTT_QUERY_NOT_FOUND);

    frame = make_flood_frame();
    if (!make_scheduler_rx_event(&frame, adva_c, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    CHECK("GTT-03", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 3u) ==
                         TAVRN_ROUTER_EVENT_OK &&
                         fixture.link.counters.rx_flood_committed == 1u);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK("GTT-03", tavrn_gtt_snapshot(&fixture.gtt, &peer_c.adva, 3u,
                                           &snapshot) == TAVRN_GTT_QUERY_FOUND &&
                        memcmp(snapshot.identity.bytes, adva_c,
                               TAVRN_ADVA_LEN) == 0 &&
                        snapshot.last_evidence_ms == 3u &&
                        snapshot.serial_present == 0u);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK("GTT-03", tavrn_gtt_snapshot(&fixture.gtt, &peer_b.adva, 3u,
                                           &snapshot) == TAVRN_GTT_QUERY_FOUND &&
                        snapshot.last_evidence_ms == 3u);

    memset(&transferred, 0, sizeof(transferred));
    transferred.type = TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED;
    transferred.detail.transferred_data.next_hop = peer_b;
    transferred.detail.transferred_data.data = make_data(
        local.logical_id, peer_c.logical_id, 0x7102u, TAVRN_DATA_ORIGINATED);
    transferred.detail.transferred_data.status = TAVRN_HACK_ACCEPTED;
    transferred.detail.transferred_data.requested_channel_mask = BLE_RADIO_ADV_CH_ALL;
    transferred.detail.transferred_data.completed_channel_mask = BLE_RADIO_ADV_CH_ALL;
    transferred.detail.transferred_data.attempt_count = 1u;
    transferred.detail.transferred_data.first_tx_ms = 3u;
    transferred.detail.transferred_data.last_tx_ms = 3u;
    CHECK("GTT-03", tavrn_router_handle_link_event(&fixture.router, &transferred,
                                                       4u) == TAVRN_ROUTER_EVENT_OK);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK("GTT-03", tavrn_gtt_snapshot(&fixture.gtt, &peer_b.adva, 4u,
                                           &snapshot) == TAVRN_GTT_QUERY_FOUND &&
                        snapshot.last_evidence_ms == 4u &&
                        snapshot.serial_present == 0u);

    unknown_origin.width = TAVRN_IDENTITY_SID16;
    unknown_origin.value = 0x6655u;
    control = make_rrep(peer_c.logical_id, unknown_origin, 0x8123u,
                        0x7777u, 10u);
    frame = make_control_frame(&control, adva_b);
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    CHECK("GTT-03", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 5u) ==
                         TAVRN_ROUTER_EVENT_IGNORED &&
                         fixture.link.counters.rx_control == 2u);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK("GTT-03", tavrn_gtt_snapshot(&fixture.gtt, &peer_c.adva, 5u,
                                           &snapshot) == TAVRN_GTT_QUERY_FOUND &&
                        snapshot.serial_present == 0u &&
                        snapshot.last_evidence_ms == 3u);

    control = make_rrep(peer_c.logical_id, peer_d.logical_id, RREQ_REQUEST_ID,
                        RREP_DESTINATION_SEQUENCE, 10u);
    frame = make_control_frame(&control, adva_b);
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    CHECK("GTT-03", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 6u) ==
                         TAVRN_ROUTER_EVENT_OK &&
                         fixture.link.counters.rx_control == 3u);
    memset(&route, 0, sizeof(route));
    CHECK("GTT-03", aodv_core_route_snapshot(&fixture.core, &peer_c.logical_id,
                                                &route) == AODV_ROUTE_QUERY_FOUND &&
                        route.state == AODV_ROUTE_VALID &&
                        route.next_hop.logical_id.value == peer_b.logical_id.value);
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK("GTT-03", tavrn_gtt_snapshot(&fixture.gtt, &peer_c.adva, 6u,
                                           &snapshot) == TAVRN_GTT_QUERY_FOUND &&
                        memcmp(snapshot.identity.bytes, adva_c,
                               TAVRN_ADVA_LEN) == 0 &&
                        snapshot.serial_present != 0u &&
                        snapshot.serial == RREP_DESTINATION_SEQUENCE &&
                        snapshot.hop_count == 1u);
}

static void check_full_scope_pair(uint32_t now_ms, uint8_t seed_member,
                                  uint8_t expected_first_scope,
                                  uint8_t expected_second_scope)
{
    integration_fixture_t fixture;
    tavrn_gtt_evidence_t evidence = make_liveness(adva_c, 1u, 3u);
    tron_application_data_t application = make_application(make_peer(adva_c).logical_id);
    aodv_action_t action;
    uint16_t first_request_id = 0u;

    if (!setup_fixture(&fixture, 0u)) {
        CHECK("GTT-06", 0);
        return;
    }
    if (seed_member != 0u) {
        CHECK("GTT-06", tavrn_gtt_observe(&fixture.gtt, &evidence, 0u) ==
                            TAVRN_GTT_OBSERVE_ADDED);
    }
    if (!bind_full_router(&fixture)) {
        CHECK("GTT-06", 0);
        return;
    }
    CHECK("GTT-06", tavrn_router_submit_application(
                         &fixture.router, &application, now_ms) ==
                         AODV_STATUS_QUEUED);
    memset(&action, 0, sizeof(action));
    CHECK("GTT-06", aodv_core_poll_action(&fixture.core, &action) ==
                        AODV_ACTION_POLL_OK &&
                        action.type == AODV_ACTION_SEND_RREQ &&
                        (action.detail.control.control.pdu[6] >> 4) ==
                            expected_first_scope);
    first_request_id = pdu_u16(&action.detail.control.control, 9u);
    CHECK("GTT-06", tavrn_router_tick(
                         &fixture.router,
                         now_ms + fixture.core.config.path_discovery_ms) ==
                         AODV_STATUS_OK);
    memset(&action, 0, sizeof(action));
    CHECK("GTT-06", aodv_core_poll_action(&fixture.core, &action) ==
                        AODV_ACTION_POLL_OK &&
                        action.type == AODV_ACTION_SEND_RREQ &&
                        (action.detail.control.control.pdu[6] >> 4) ==
                            expected_second_scope &&
                        pdu_u16(&action.detail.control.control, 9u) !=
                            first_request_id);
}

static void test_gtt_06_full_router_scoped_submission(void)
{
    integration_fixture_t fixture;
    tavrn_gtt_evidence_t evidence = make_liveness(adva_c, 1u, 3u);
    tron_application_data_t application = make_application(make_peer(adva_c).logical_id);
    aodv_action_t action;
    uint16_t first_request_id;

    if (!setup_fixture(&fixture, 0u)) {
        CHECK("GTT-06", 0);
        return;
    }
    CHECK("GTT-06", tavrn_gtt_observe(&fixture.gtt, &evidence, 0u) ==
                        TAVRN_GTT_OBSERVE_ADDED);
    if (!bind_full_router(&fixture)) {
        CHECK("GTT-06", 0);
        return;
    }
    if (tavrn_router_submit_application(&fixture.router, &application, 0u) !=
        AODV_STATUS_QUEUED) {
        CHECK("GTT-06", 0);
        return;
    }
    memset(&action, 0, sizeof(action));
    if (aodv_core_poll_action(&fixture.core, &action) != AODV_ACTION_POLL_OK ||
        action.type != AODV_ACTION_SEND_RREQ) {
        CHECK("GTT-06", 0);
        return;
    }
    CHECK("GTT-06", (action.detail.control.control.pdu[6] >> 4) == 5u);
    first_request_id = pdu_u16(&action.detail.control.control, 9u);

    if (tavrn_router_tick(&fixture.router,
                          fixture.core.config.path_discovery_ms) != AODV_STATUS_OK) {
        CHECK("GTT-06", 0);
        return;
    }
    memset(&action, 0, sizeof(action));
    if (aodv_core_poll_action(&fixture.core, &action) != AODV_ACTION_POLL_OK ||
        action.type != AODV_ACTION_SEND_RREQ) {
        CHECK("GTT-06", 0);
        return;
    }
    CHECK("GTT-06", (action.detail.control.control.pdu[6] >> 4) == 15u &&
                        pdu_u16(&action.detail.control.control, 9u) !=
                            first_request_id);
    check_full_scope_pair(50u, 1u, 5u, 15u);
    check_full_scope_pair(100u, 1u, 1u, 3u);
    check_full_scope_pair(0u, 0u, 1u, 3u);
}

static void test_gtt_05_route_gtt_independence(void)
{
    integration_fixture_t fixture;
    tavrn_gtt_evidence_t evidence = make_liveness(adva_c, 1u, 3u);
    tron_application_data_t application = make_application(make_peer(adva_c).logical_id);
    tavrn_validated_control_t reply;
    tavrn_decoded_frame_t frame;
    ble_mesh_sched_event_t scheduler_event;
    aodv_action_t action;
    aodv_route_snapshot_t route;
    tavrn_gtt_snapshot_t member;
    tavrn_direct_peer_t local = make_peer(adva_a);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);

    if (!setup_fixture(&fixture, 0u) || !bind_full_router(&fixture)) {
        CHECK("GTT-05", 0);
        return;
    }
    CHECK("GTT-05", tavrn_gtt_observe(&fixture.gtt, &evidence, 0u) ==
                        TAVRN_GTT_OBSERVE_ADDED);
    if (tavrn_router_submit_application(&fixture.router, &application, 0u) !=
        AODV_STATUS_QUEUED) {
        CHECK("GTT-05", 0);
        return;
    }
    memset(&action, 0, sizeof(action));
    if (aodv_core_poll_action(&fixture.core, &action) != AODV_ACTION_POLL_OK ||
        action.type != AODV_ACTION_SEND_RREQ) {
        CHECK("GTT-05", 0);
        return;
    }
    reply = make_rrep(peer_c.logical_id, local.logical_id,
                      pdu_u16(&action.detail.control.control, 9u),
                      0x4321u, 10u);
    frame = make_control_frame(&reply, adva_b);
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event) ||
        tavrn_router_handle_scheduler_event(&fixture.router, &scheduler_event,
                                            1u) != TAVRN_ROUTER_EVENT_OK) {
        CHECK("GTT-05", 0);
        return;
    }
    memset(&route, 0, sizeof(route));
    CHECK("GTT-05", aodv_core_route_snapshot(&fixture.core, &peer_c.logical_id,
                                                &route) == AODV_ROUTE_QUERY_FOUND &&
                        route.state == AODV_ROUTE_VALID && route.expires_at_ms == 11u);

    if (tavrn_router_tick(&fixture.router, 12u) != AODV_STATUS_OK) {
        CHECK("GTT-05", 0);
        return;
    }
    memset(&route, 0, sizeof(route));
    memset(&member, 0, sizeof(member));
    CHECK("GTT-05", aodv_core_route_snapshot(&fixture.core, &peer_c.logical_id,
                                                &route) == AODV_ROUTE_QUERY_FOUND &&
                        route.state == AODV_ROUTE_INVALID &&
                        tavrn_gtt_snapshot(&fixture.gtt, &peer_c.adva, 12u,
                                           &member) == TAVRN_GTT_QUERY_FOUND &&
                        (member.freshness == TAVRN_GTT_FRESHNESS_ACTIVE ||
                         member.freshness == TAVRN_GTT_FRESHNESS_SOFT_STALE));
}

static void test_aodv_only_router_submission(void)
{
    static const uint8_t expected_ttl[] = { 1u, 3u, 5u, 7u, 15u };
    integration_fixture_t fixture;
    tron_application_data_t application = make_application(make_peer(adva_c).logical_id);
    aodv_action_t action;
    uint16_t previous_request = 0u;
    uint8_t i;

    if (!setup_fixture(&fixture, 0u) || !init_router(&fixture, NULL)) {
        CHECK("AODV-04", 0);
        return;
    }
    CHECK("AODV-04", tavrn_router_submit_application(
                         &fixture.router, &application, 0u) == AODV_STATUS_QUEUED);
    for (i = 0u; i < sizeof(expected_ttl); i++) {
        memset(&action, 0, sizeof(action));
        CHECK("AODV-04", aodv_core_poll_action(&fixture.core, &action) ==
                            AODV_ACTION_POLL_OK &&
                            action.type == AODV_ACTION_SEND_RREQ &&
                            (action.detail.control.control.pdu[6] >> 4) ==
                                expected_ttl[i] &&
                            (i == 0u ||
                             pdu_u16(&action.detail.control.control, 9u) !=
                                 previous_request));
        previous_request = pdu_u16(&action.detail.control.control, 9u);
        CHECK("AODV-04", tavrn_router_tick(
                             &fixture.router,
                             (uint32_t)(i + 1u) *
                                 fixture.core.config.path_discovery_ms) ==
                             AODV_STATUS_OK);
    }
}

static void test_link_02_final_delivery_requires_reservation(void)
{
    integration_fixture_t fixture;
    tavrn_decoded_frame_t frame;
    ble_mesh_sched_event_t scheduler_event;
    tavrn_direct_peer_t local = make_peer(adva_a);
    aodv_action_t action;

    if (!setup_fixture(&fixture, 0u) ||
        tavrn_router_init(&fixture.router, &fixture.link, &fixture.core, NULL) !=
            TAVRN_ROUTER_INIT_OK) {
        CHECK("LINK-02", 0);
        return;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = make_peer(adva_b);
    frame.detail.data.immediate_receiver = local.logical_id;
    frame.detail.data.data = make_data(make_peer(adva_d).logical_id,
                                       local.logical_id, 0x7c01u,
                                       TAVRN_DATA_TRANSIT);
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event)) {
        CHECK("LINK-02", 0);
        return;
    }
    CHECK("LINK-02", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 1u) ==
                         TAVRN_ROUTER_EVENT_BUSY &&
                         fixture.link.counters.rx_candidate_busy == 1u &&
                         fixture.link.counters.rx_candidate_accepted == 0u &&
                          aodv_core_poll_action(&fixture.core, &action) ==
                              AODV_ACTION_POLL_EMPTY);
    frame.detail.data.immediate_receiver = make_peer(adva_c).logical_id;
    CHECK("LINK-02", make_scheduler_rx_event(&frame, adva_b, &scheduler_event) &&
                         tavrn_router_handle_scheduler_event(
                             &fixture.router, &scheduler_event, 2u) ==
                             TAVRN_ROUTER_EVENT_IGNORED);
    for (uint8_t index = 0u; index < TAVRN_LINK_DATA_DEDUPE_CAPACITY; index++) {
        fixture.link.data_dedupe[index].valid = 1u;
        fixture.link.data_dedupe[index].custody_pinned = 1u;
        fixture.link.data_dedupe[index].expires_at_ms = 1000u;
    }
    frame.detail.data.immediate_receiver = local.logical_id;
    CHECK("LINK-02", make_scheduler_rx_event(&frame, adva_b, &scheduler_event) &&
                         tavrn_router_handle_scheduler_event(
                             &fixture.router, &scheduler_event, 3u) ==
                             TAVRN_ROUTER_EVENT_BUSY &&
                         tavrn_link_v2_counters(&fixture.link)
                             ->rx_data_dedupe_busy == 1u);
}

static void test_gtt_03_rejected_flood_is_ignored(void)
{
    integration_fixture_t fixture;
    tavrn_decoded_frame_t flood = make_flood_frame();
    ble_mesh_sched_event_t scheduler_event;

    if (!setup_fixture(&fixture, 0u) || !init_router(&fixture, NULL) ||
        !make_scheduler_rx_event(&flood, adva_c, &scheduler_event)) {
        CHECK("GTT-03", 0);
        return;
    }
    scheduler_event.adv_data[12] |= 0x01u;
    CHECK("GTT-03", tavrn_router_handle_scheduler_event(
                         &fixture.router, &scheduler_event, 1u) ==
                         TAVRN_ROUTER_EVENT_IGNORED &&
                         tavrn_link_v2_counters(&fixture.link)
                             ->rx_flood_committed == 0u &&
                         tavrn_link_v2_counters(&fixture.link)
                             ->rx_flood_duplicate == 0u);
}

static void test_link_02_delivery_reservation_exactness(void)
{
    integration_fixture_t fixture;
    integration_fixture_t cancel_fixture;
    integration_fixture_t cancel_invalid_fixture;
    integration_fixture_t commit_invalid_fixture;
    tavrn_decoded_frame_t frame;
    ble_mesh_sched_event_t scheduler_event;
    tavrn_link_event_t candidate;
    tavrn_link_event_t stale;
    tavrn_direct_peer_t local = make_peer(adva_a);
    tavrn_link_data_t data = make_data(make_peer(adva_d).logical_id,
                                       local.logical_id, 0x7c02u,
                                       TAVRN_DATA_TRANSIT);
    aodv_action_t action;

    if (!setup_fixture(&fixture, 0u) || !init_router(&fixture, NULL)) {
        CHECK("LINK-02", 0);
        return;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = make_peer(adva_b);
    frame.detail.data.immediate_receiver = local.logical_id;
    frame.detail.data.data = data;
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event) ||
        tavrn_link_v2_on_scheduler_event(&fixture.link, &scheduler_event, 1u,
                                          &candidate) != TAVRN_LINK_STEP_EVENT ||
        candidate.type != TAVRN_LINK_EVENT_RX_DATA_CANDIDATE) {
        CHECK("LINK-02", 0);
        return;
    }
    stale = candidate;
    CHECK("LINK-02", tavrn_router_handle_link_event(&fixture.router, &candidate, 1u) ==
                         TAVRN_ROUTER_EVENT_OK &&
                         fixture.application_recorder.reserve_count == 1u &&
                         fixture.application_recorder.commit_count == 0u &&
                         tavrn_router_delivery_state(&fixture.router) ==
                             TAVRN_ROUTER_DELIVERY_POST_ACK_COMMIT &&
                         tavrn_router_handle_link_event(&fixture.router, &stale, 2u) ==
                             TAVRN_ROUTER_EVENT_INVALID &&
                         fixture.application_recorder.reserve_count == 1u &&
                         fixture.application_recorder.cancel_count == 0u &&
                          dispatch_without_local_event(&fixture.router, 2u) ==
                             TAVRN_ROUTER_EVENT_OK &&
                         fixture.application_recorder.commit_count == 1u &&
                         tavrn_router_delivery_state(&fixture.router) ==
                             TAVRN_ROUTER_DELIVERY_NONE &&
                         aodv_core_poll_action(&fixture.core, &action) ==
                             AODV_ACTION_POLL_EMPTY);

    if (!setup_fixture(&cancel_fixture, 0u) || !init_router(&cancel_fixture, NULL) ||
        !make_scheduler_rx_event(&frame, adva_b, &scheduler_event) ||
        tavrn_link_v2_on_scheduler_event(&cancel_fixture.link, &scheduler_event, 1u,
                                          &candidate) != TAVRN_LINK_STEP_EVENT) {
        CHECK("LINK-02", 0);
        return;
    }
    cancel_fixture.application_recorder.cancel_status = TAVRN_ROUTER_DELIVERY_BUSY;
    cancel_fixture.link.mesh_fault_latched = 1u;
    CHECK("LINK-02", tavrn_router_handle_link_event(&cancel_fixture.router,
                                                        &candidate, 1u) ==
                          TAVRN_ROUTER_EVENT_INVALID &&
                          cancel_fixture.application_recorder.reserve_count == 1u &&
                          cancel_fixture.application_recorder.cancel_count == 1u &&
                          tavrn_router_delivery_state(&cancel_fixture.router) ==
                              TAVRN_ROUTER_DELIVERY_CANCEL_PENDING &&
                          cancel_fixture.router.pending_ingest.valid == 0u &&
                          tavrn_router_set_application_hooks(&cancel_fixture.router, NULL) ==
                              TAVRN_ROUTER_APPLICATION_HOOK_BUSY &&
                          tavrn_router_tick(&cancel_fixture.router, 2u) ==
                              AODV_STATUS_BUSY &&
                          cancel_fixture.application_recorder.cancel_count == 2u);
    cancel_fixture.application_recorder.cancel_status = TAVRN_ROUTER_DELIVERY_OK;
    CHECK("LINK-02", tavrn_router_tick(&cancel_fixture.router, 3u) ==
                         AODV_STATUS_OK &&
                         cancel_fixture.application_recorder.cancel_count == 3u &&
                         tavrn_router_delivery_state(&cancel_fixture.router) ==
                             TAVRN_ROUTER_DELIVERY_NONE &&
                         aodv_core_poll_action(&cancel_fixture.core, &action) ==
                             AODV_ACTION_POLL_EMPTY);

    if (!setup_fixture(&cancel_invalid_fixture, 0u) ||
        !init_router(&cancel_invalid_fixture, NULL) ||
        !make_scheduler_rx_event(&frame, adva_b, &scheduler_event) ||
        tavrn_link_v2_on_scheduler_event(&cancel_invalid_fixture.link,
                                          &scheduler_event, 1u, &candidate) !=
            TAVRN_LINK_STEP_EVENT) {
        CHECK("LINK-02", 0);
        return;
    }
    cancel_invalid_fixture.application_recorder.cancel_status =
        TAVRN_ROUTER_DELIVERY_INVALID;
    cancel_invalid_fixture.link.mesh_fault_latched = 1u;
    CHECK("LINK-02", tavrn_router_handle_link_event(&cancel_invalid_fixture.router,
                                                       &candidate, 1u) ==
                         TAVRN_ROUTER_EVENT_INVALID &&
                         cancel_invalid_fixture.application_recorder.cancel_count == 1u &&
                         tavrn_router_delivery_state(&cancel_invalid_fixture.router) ==
                             TAVRN_ROUTER_DELIVERY_CANCEL_PENDING &&
                         tavrn_router_fault_reason(&cancel_invalid_fixture.router) ==
                             TAVRN_ROUTER_FAULT_DELIVERY_CANCEL_INVALID &&
                         tavrn_router_tick(&cancel_invalid_fixture.router, 2u) ==
                             AODV_STATUS_INVALID &&
                         cancel_invalid_fixture.application_recorder.cancel_count == 1u &&
                         tavrn_router_set_application_hooks(&cancel_invalid_fixture.router,
                                                            NULL) ==
                             TAVRN_ROUTER_APPLICATION_HOOK_INVALID);

    if (!setup_fixture(&commit_invalid_fixture, 0u) ||
        !init_router(&commit_invalid_fixture, NULL) ||
        !make_scheduler_rx_event(&frame, adva_b, &scheduler_event) ||
        tavrn_router_handle_scheduler_event(&commit_invalid_fixture.router,
                                            &scheduler_event, 1u) !=
            TAVRN_ROUTER_EVENT_OK) {
        CHECK("LINK-04", 0);
        return;
    }
    commit_invalid_fixture.application_recorder.commit_status =
        TAVRN_ROUTER_DELIVERY_INVALID;
    CHECK("LINK-04", dispatch_without_local_event(&commit_invalid_fixture.router, 2u) ==
                         TAVRN_ROUTER_EVENT_INVALID &&
                         commit_invalid_fixture.application_recorder.commit_count == 1u &&
                         tavrn_router_delivery_state(&commit_invalid_fixture.router) ==
                             TAVRN_ROUTER_DELIVERY_FAULTED_POST_ACK &&
                         tavrn_router_fault_reason(&commit_invalid_fixture.router) ==
                             TAVRN_ROUTER_FAULT_DELIVERY_COMMIT_INVALID &&
                          dispatch_without_local_event(&commit_invalid_fixture.router, 3u) ==
                             TAVRN_ROUTER_EVENT_INVALID &&
                         commit_invalid_fixture.application_recorder.commit_count == 1u &&
                         tavrn_router_set_application_hooks(&commit_invalid_fixture.router,
                                                            NULL) ==
                             TAVRN_ROUTER_APPLICATION_HOOK_INVALID);
}

static void test_link_04_retained_action_dispatch(void)
{
    integration_fixture_t control_fixture;
    integration_fixture_t forward_fixture;
    integration_fixture_t delivery_fixture;
    tavrn_direct_peer_t local = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_decoded_frame_t frame;
    ble_mesh_sched_event_t scheduler_event;
    tavrn_link_data_t data;
    tron_application_data_t application;
    uint8_t i;

    if (!setup_fixture(&control_fixture, 0u) || !init_router(&control_fixture, NULL)) {
        CHECK("LINK-04", 0);
        return;
    }
    application = make_application(peer_c.logical_id);
    for (i = 0u; i < BLE_MESH_TX_QUEUE_CAPACITY; i++) {
        if (!enqueue_control_priority_filler(&control_fixture.scheduler, 0u)) {
            CHECK("LINK-04", 0);
            return;
        }
    }
    CHECK("LINK-04", tavrn_router_submit_application(&control_fixture.router,
                                                        &application, 0u) ==
                         AODV_STATUS_QUEUED &&
                          dispatch_without_local_event(&control_fixture.router, 0u) ==
                             TAVRN_ROUTER_EVENT_BUSY &&
                         control_fixture.router.retained_action_valid != 0u);
    ble_mesh_scheduler_init(&control_fixture.scheduler, 1u, adva_a);
    CHECK("LINK-04", dispatch_without_local_event(&control_fixture.router, 1u) ==
                         TAVRN_ROUTER_EVENT_OK &&
                         control_fixture.router.retained_action_valid == 0u);

    if (!setup_fixture(&forward_fixture, 0u) || !init_router(&forward_fixture, NULL) ||
        !install_route_via(&forward_fixture, peer_c.logical_id, &peer_b, 0u)) {
        CHECK("LINK-04", 0);
        return;
    }
    data = make_data(make_peer(adva_d).logical_id, peer_c.logical_id, 0x7c03u,
                     TAVRN_DATA_TRANSIT);
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = make_peer(adva_d);
    frame.detail.data.immediate_receiver = local.logical_id;
    frame.detail.data.data = data;
    if (!make_scheduler_rx_event(&frame, adva_d, &scheduler_event) ||
        tavrn_router_handle_scheduler_event(&forward_fixture.router, &scheduler_event,
                                            1u) != TAVRN_ROUTER_EVENT_OK) {
        CHECK("LINK-04", 0);
        return;
    }
    for (i = 0u; i < TAVRN_LINK_CUSTODY_CAPACITY; i++) {
        tavrn_link_data_t filler = make_data(local.logical_id, peer_c.logical_id,
                                             (uint16_t)(0x7d00u + i),
                                             TAVRN_DATA_ORIGINATED);
        tavrn_link_event_t local_outcome;

        if (tavrn_link_v2_send_unicast(&forward_fixture.link, &peer_b, &filler, 2u,
                                       &local_outcome) != TAVRN_LINK_SEND_OK) {
            CHECK("LINK-04", 0);
            return;
        }
    }
    CHECK("LINK-04", dispatch_without_local_event(&forward_fixture.router, 2u) ==
                         TAVRN_ROUTER_EVENT_BUSY &&
                         forward_fixture.router.retained_action_valid != 0u);
    for (i = 0u; i < TAVRN_LINK_CUSTODY_CAPACITY; i++) {
        if (tavrn_router_tick(&forward_fixture.router, 5002u) != AODV_STATUS_OK) {
            CHECK("LINK-04", 0);
            return;
        }
    }
    CHECK("LINK-04", dispatch_without_local_event(&forward_fixture.router, 5003u) ==
                         TAVRN_ROUTER_EVENT_OK &&
                         forward_fixture.router.retained_action_valid == 0u &&
                         tavrn_router_service_link(&forward_fixture.router, 5003u) ==
                             TAVRN_ROUTER_EVENT_IGNORED);

    if (!setup_fixture(&delivery_fixture, 0u) || !init_router(&delivery_fixture, NULL)) {
        CHECK("LINK-04", 0);
        return;
    }
    data = make_data(make_peer(adva_d).logical_id, local.logical_id, 0x7c04u,
                     TAVRN_DATA_TRANSIT);
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = peer_b;
    frame.detail.data.immediate_receiver = local.logical_id;
    frame.detail.data.data = data;
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event) ||
        tavrn_router_handle_scheduler_event(&delivery_fixture.router, &scheduler_event,
                                            1u) != TAVRN_ROUTER_EVENT_OK) {
        CHECK("LINK-04", 0);
        return;
    }
    delivery_fixture.application_recorder.commit_status = TAVRN_ROUTER_DELIVERY_BUSY;
    CHECK("LINK-04", dispatch_without_local_event(&delivery_fixture.router, 2u) ==
                         TAVRN_ROUTER_EVENT_BUSY &&
                         delivery_fixture.router.retained_action_valid != 0u &&
                         tavrn_router_delivery_state(&delivery_fixture.router) ==
                             TAVRN_ROUTER_DELIVERY_POST_ACK_COMMIT);
    delivery_fixture.application_recorder.commit_status = TAVRN_ROUTER_DELIVERY_OK;
    CHECK("LINK-04", dispatch_without_local_event(&delivery_fixture.router, 3u) ==
                         TAVRN_ROUTER_EVENT_OK &&
                         delivery_fixture.application_recorder.commit_count == 2u &&
                         delivery_fixture.router.retained_action_valid == 0u &&
                         tavrn_router_delivery_state(&delivery_fixture.router) ==
                             TAVRN_ROUTER_DELIVERY_NONE);
}

static void test_aodv_07_unsent_rrep_cancel_and_local_action_events(void)
{
    integration_fixture_t fixture;
    tavrn_direct_peer_t local = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_d = make_peer(adva_d);
    tavrn_link_event_t event;
    tavrn_router_dispatch_event_t dispatch_event;
    uint8_t i;

    if (!setup_fixture(&fixture, 0u) || !init_router(&fixture, NULL)) {
        CHECK("AODV-07", 0);
        return;
    }
    fixture.link.mesh_fault_latched = 1u;
    memset(&event, 0, sizeof(event));
    event.type = TAVRN_LINK_EVENT_RX_CONTROL;
    event.detail.control.transmitter = peer_b;
    for (i = 0u; i < TAVRN_AODV_RREP_ACK_WAIT_CAPACITY + 1u; i++) {
        event.detail.control.control = make_rreq(peer_d.logical_id, local.logical_id,
                                                 (uint16_t)(0x7f20u + i));
        CHECK("AODV-07", tavrn_router_handle_link_event(&fixture.router, &event,
                                                           (uint32_t)i) ==
                             TAVRN_ROUTER_EVENT_OK &&
                              dispatch_without_local_event(&fixture.router,
                                                           (uint32_t)i) ==
                                 TAVRN_ROUTER_EVENT_IGNORED &&
                             tavrn_router_fault_reason(&fixture.router) ==
                                 TAVRN_ROUTER_FAULT_NONE);
    }

    memset(&dispatch_event, 0xa5, sizeof(dispatch_event));
    memset(&fixture.router.retained_action, 0, sizeof(fixture.router.retained_action));
    fixture.router.retained_action.type = AODV_ACTION_PENDING_DATA_FAILED;
    fixture.router.retained_action.detail.failure.destination = peer_d.logical_id;
    fixture.router.retained_action_valid = 1u;
    CHECK("AODV-05", tavrn_router_dispatch_ex(&fixture.router, 10u,
                                                &dispatch_event) ==
                         TAVRN_ROUTER_EVENT_OK &&
                         dispatch_event.type ==
                             TAVRN_ROUTER_DISPATCH_EVENT_PENDING_DATA_FAILED &&
                         dispatch_event.destination.value == peer_d.logical_id.value &&
                         fixture.router.retained_action_valid == 0u);
    memset(&dispatch_event, 0xa5, sizeof(dispatch_event));
    memset(&fixture.router.retained_action, 0, sizeof(fixture.router.retained_action));
    fixture.router.retained_action.type = AODV_ACTION_BLACKLIST_NEIGHBOR;
    fixture.router.retained_action.detail.failure.destination = peer_d.logical_id;
    fixture.router.retained_action.detail.failure.peer = peer_b;
    fixture.router.retained_action_valid = 1u;
    CHECK("AODV-07", tavrn_router_dispatch_ex(&fixture.router, 11u,
                                                &dispatch_event) ==
                         TAVRN_ROUTER_EVENT_OK &&
                         dispatch_event.type ==
                             TAVRN_ROUTER_DISPATCH_EVENT_BLACKLIST_NEIGHBOR &&
                         dispatch_event.destination.value == peer_d.logical_id.value &&
                         memcmp(dispatch_event.peer.adva.bytes, peer_b.adva.bytes,
                                TAVRN_ADVA_LEN) == 0 &&
                         fixture.router.retained_action_valid == 0u);
}

static void test_aodv_06_failure_obligation_bounds_and_release_invariant(void)
{
    integration_fixture_t fixture;
    const uint8_t *const identities[] = { adva_b, adva_c, adva_d, adva_e, adva_f };
    tavrn_direct_peer_t peers[sizeof(identities) / sizeof(identities[0])];
    tavrn_link_event_t event;
    tron_application_data_t application;
    aodv_action_t action;
    aodv_route_snapshot_t route;
    uint8_t i;

    if (!setup_fixture(&fixture, 0u) || !init_router(&fixture, NULL)) {
        CHECK("AODV-06", 0);
        return;
    }
    for (i = 0u; i < sizeof(peers) / sizeof(peers[0]); i++) {
        peers[i] = make_peer(identities[i]);
        if (!install_route_via(&fixture, peers[i].logical_id, &peers[i],
                               (uint32_t)i * 2u)) {
            CHECK("AODV-06", 0);
            return;
        }
    }
    for (i = 0u; i < TAVRN_AODV_ACTION_CAPACITY; i++) {
        application = make_application(peers[i % (sizeof(peers) / sizeof(peers[0]))]
                                           .logical_id);
        if (tavrn_router_submit_application(&fixture.router, &application,
                                            (uint32_t)(20u + i)) != AODV_STATUS_OK) {
            CHECK("AODV-06", 0);
            return;
        }
    }
    memset(&event, 0, sizeof(event));
    event.type = TAVRN_LINK_EVENT_RETRY_EXHAUSTED;
    for (i = 0u; i < sizeof(peers) / sizeof(peers[0]); i++) {
        event.detail.owned_data.next_hop = peers[i];
        event.detail.owned_data.data = make_data(make_peer(adva_a).logical_id,
                                                  peers[i].logical_id,
                                                  (uint16_t)(0x7e00u + i),
                                                  TAVRN_DATA_ORIGINATED);
        CHECK("AODV-06", tavrn_router_handle_link_event(&fixture.router, &event,
                                                           40u) ==
                             TAVRN_ROUTER_EVENT_BUSY);
    }
    event.detail.owned_data.next_hop = peers[0];
    CHECK("AODV-06", tavrn_router_handle_link_event(&fixture.router, &event, 40u) ==
                         TAVRN_ROUTER_EVENT_BUSY &&
                         failure_obligation_count(&fixture.router) ==
                             sizeof(peers) / sizeof(peers[0]) &&
                         tavrn_router_counters(&fixture.router)
                             ->failure_set_overflow == 0u);
    CHECK("AODV-06", drain_actions(&fixture.core));
    for (i = 0u; i < sizeof(peers) / sizeof(peers[0]); i++) {
        CHECK("AODV-06", tavrn_router_tick(&fixture.router,
                                             (uint32_t)(41u + i)) ==
                             (i + 1u < sizeof(peers) / sizeof(peers[0]) ?
                                  AODV_STATUS_BUSY : AODV_STATUS_OK) &&
                         aodv_core_route_snapshot(&fixture.core,
                                                  &peers[i].logical_id, &route) ==
                             AODV_ROUTE_QUERY_FOUND &&
                         route.state == AODV_ROUTE_INVALID &&
                         (i + 1u >= sizeof(peers) / sizeof(peers[0]) ||
                          (aodv_core_route_snapshot(&fixture.core,
                                                    &peers[i + 1u].logical_id,
                                                    &route) == AODV_ROUTE_QUERY_FOUND &&
                           route.state == AODV_ROUTE_VALID)));
    }
    CHECK("AODV-06", failure_obligation_count(&fixture.router) == 0u);

    memset(&event, 0, sizeof(event));
    event.type = TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED;
    event.detail.owned_data.next_hop = peers[0];
    event.detail.owned_data.data = make_data(make_peer(adva_a).logical_id,
                                              peers[0].logical_id, 0x7f00u,
                                              TAVRN_DATA_TRANSIT);
    CHECK("LINK-04", tavrn_router_handle_link_event(&fixture.router, &event, 50u) ==
                          TAVRN_ROUTER_EVENT_INVALID &&
                          tavrn_router_counters(&fixture.router)->release_invariant == 1u &&
                          tavrn_router_fault_reason(&fixture.router) ==
                              TAVRN_ROUTER_FAULT_TRANSIT_CUSTODY_RELEASE_INVALID &&
                          tavrn_router_tick(&fixture.router, 51u) ==
                              AODV_STATUS_INVALID);
    (void)action;
}

static void test_aodv_06_failure_capacity_rate_and_expiry_order(void)
{
    integration_fixture_t fixture;
    uint8_t identities[TAVRN_AODV_ROUTE_CAPACITY][TAVRN_ADVA_LEN];
    tavrn_direct_peer_t peers[TAVRN_AODV_ROUTE_CAPACITY];
    tavrn_direct_peer_t failure_peers[TAVRN_AODV_ROUTE_CAPACITY];
    tavrn_direct_peer_t local = make_peer(adva_a);
    tavrn_link_event_t failure_event;
    tron_application_data_t application;
    aodv_route_snapshot_t route;
    uint8_t i;

    if (!setup_fixture(&fixture, 0u) || !init_router(&fixture, NULL)) {
        CHECK("AODV-06", 0);
        return;
    }
    for (i = 0u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        identities[i][0] = (uint8_t)(0x20u + i);
        identities[i][1] = 0x61u;
        identities[i][2] = 0x32u;
        identities[i][3] = 0x43u;
        identities[i][4] = 0x54u;
        identities[i][5] = 0xc1u;
        peers[i] = make_peer(identities[i]);
        failure_peers[i] = peers[i];
    }
    for (i = 0u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        if (!install_route_via(&fixture, peers[i].logical_id, &peers[i],
                               (uint32_t)(1u + i * 1001u))) {
            CHECK("AODV-06", 0);
            return;
        }
    }
    for (i = 0u; i < TAVRN_AODV_ACTION_CAPACITY; i++) {
        application = make_application(peers[i].logical_id);
        if (tavrn_router_submit_application(&fixture.router, &application,
                                            (uint32_t)(40u + i)) != AODV_STATUS_OK) {
            CHECK("AODV-06", 0);
            return;
        }
    }
    memset(&failure_event, 0, sizeof(failure_event));
    failure_event.type = TAVRN_LINK_EVENT_RETRY_EXHAUSTED;
    for (i = 0u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        failure_event.detail.owned_data.next_hop = failure_peers[i];
        failure_event.detail.owned_data.data = make_data(
            local.logical_id, peers[i].logical_id, (uint16_t)(0x7f90u + i),
            TAVRN_DATA_ORIGINATED);
        CHECK("AODV-06", tavrn_router_handle_link_event(&fixture.router,
                                                           &failure_event, 20000u) ==
                             TAVRN_ROUTER_EVENT_BUSY);
    }
    CHECK("AODV-06", TAVRN_ROUTER_FAILURE_CAPACITY == TAVRN_AODV_ROUTE_CAPACITY &&
                         failure_obligation_count(&fixture.router) ==
                             TAVRN_AODV_ROUTE_CAPACITY &&
                         tavrn_router_counters(&fixture.router)
                             ->failure_set_overflow == 0u &&
                         drain_actions(&fixture.core));
    for (i = 0u; i < 10u; i++) {
        CHECK("AODV-06", tavrn_router_tick(&fixture.router, 20000u) ==
                             AODV_STATUS_BUSY &&
                         aodv_core_route_snapshot(&fixture.core,
                                                  &peers[i].logical_id, &route) ==
                             AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_INVALID &&
                         aodv_core_route_snapshot(&fixture.core,
                                                  &peers[i + 1u].logical_id,
                                                  &route) == AODV_ROUTE_QUERY_FOUND &&
                         route.state == AODV_ROUTE_VALID && drain_actions(&fixture.core));
    }
    CHECK("AODV-06", tavrn_router_tick(&fixture.router, 20001u) ==
                         AODV_STATUS_BUSY &&
                         aodv_core_counters(&fixture.core)->rerr_rate_limited != 0u &&
                         aodv_core_counters(&fixture.core)->route_expired == 0u &&
                         aodv_core_route_snapshot(&fixture.core,
                                                  &peers[10].logical_id, &route) ==
                             AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_VALID);
    for (i = 10u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        CHECK("AODV-06", tavrn_router_tick(&fixture.router,
                                             (uint32_t)(21000u + i)) ==
                             (i + 1u < TAVRN_AODV_ROUTE_CAPACITY ?
                                  AODV_STATUS_BUSY : AODV_STATUS_OK) &&
                         aodv_core_route_snapshot(&fixture.core,
                                                  &peers[i].logical_id, &route) ==
                             AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_INVALID);
        if (i + 1u < TAVRN_AODV_ROUTE_CAPACITY && !drain_actions(&fixture.core)) {
            CHECK("AODV-06", 0);
            return;
        }
    }
    CHECK("AODV-06", failure_obligation_count(&fixture.router) == 0u);
}

static void test_router_fail_stop_delivery_invariants(void)
{
    integration_fixture_t reserve_fixture;
    integration_fixture_t mismatch_fixture;
    integration_fixture_t ingest_fixture;
    tavrn_decoded_frame_t frame;
    ble_mesh_sched_event_t scheduler_event;
    tavrn_router_dispatch_event_t dispatch_event;
    tavrn_direct_peer_t local = make_peer(adva_a);
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_link_data_t data = make_data(make_peer(adva_d).logical_id,
                                       local.logical_id, 0x7fd0u,
                                       TAVRN_DATA_TRANSIT);

    if (!setup_fixture(&reserve_fixture, 0u) ||
        !init_router(&reserve_fixture, NULL)) {
        CHECK("LINK-02", 0);
        return;
    }
    reserve_fixture.application_recorder.reserve_zero_token = 1u;
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TAVRN_NETWORK_ID;
    frame.transmitter = peer_b;
    frame.detail.data.immediate_receiver = local.logical_id;
    frame.detail.data.data = data;
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event)) {
        CHECK("LINK-02", 0);
        return;
    }
    CHECK("LINK-02", tavrn_router_handle_scheduler_event(&reserve_fixture.router,
                                                            &scheduler_event, 1u) ==
                          TAVRN_ROUTER_EVENT_INVALID &&
                          tavrn_router_fault_reason(&reserve_fixture.router) ==
                              TAVRN_ROUTER_FAULT_APPLICATION_RESERVE_TOKEN_ZERO &&
                          tavrn_router_delivery_state(&reserve_fixture.router) ==
                              TAVRN_ROUTER_DELIVERY_FAULTED_PRE_ACK &&
                          reserve_fixture.link.candidate_valid != 0u &&
                          reserve_fixture.application_recorder.reserve_count == 1u &&
                          tavrn_router_tick(&reserve_fixture.router, 2u) ==
                              AODV_STATUS_INVALID &&
                          reserve_fixture.application_recorder.reserve_count == 1u);

    if (!setup_fixture(&mismatch_fixture, 0u) ||
        !init_router(&mismatch_fixture, NULL)) {
        CHECK("LINK-04", 0);
        return;
    }
    mismatch_fixture.router.delivery.transmitter = peer_b;
    mismatch_fixture.router.delivery.data = data;
    mismatch_fixture.router.delivery.token = 1u;
    mismatch_fixture.router.delivery.state = TAVRN_ROUTER_DELIVERY_POST_ACK_COMMIT;
    mismatch_fixture.router.retained_action.type = AODV_ACTION_DELIVER_DATA;
    mismatch_fixture.router.retained_action.detail.data.data = data;
    mismatch_fixture.router.retained_action.detail.data.data.data_seq++;
    mismatch_fixture.router.retained_action_valid = 1u;
    memset(&dispatch_event, 0xa5, sizeof(dispatch_event));
    CHECK("LINK-04", tavrn_router_dispatch_ex(&mismatch_fixture.router, 1u,
                                                 &dispatch_event) ==
                          TAVRN_ROUTER_EVENT_INVALID &&
                          dispatch_event.type == TAVRN_ROUTER_DISPATCH_EVENT_NONE &&
                          tavrn_router_fault_reason(&mismatch_fixture.router) ==
                              TAVRN_ROUTER_FAULT_DELIVERY_ACTION_MISMATCH &&
                          mismatch_fixture.router.retained_action_valid != 0u &&
                          tavrn_router_delivery_state(&mismatch_fixture.router) ==
                              TAVRN_ROUTER_DELIVERY_POST_ACK_COMMIT &&
                          mismatch_fixture.application_recorder.commit_count == 0u &&
                          tavrn_router_service_link(&mismatch_fixture.router, 2u) ==
                              TAVRN_ROUTER_EVENT_INVALID);

    if (!setup_fixture(&ingest_fixture, 0u) || !init_router(&ingest_fixture, NULL)) {
        CHECK("LINK-02", 0);
        return;
    }
    ingest_fixture.router.delivery.transmitter = peer_b;
    ingest_fixture.router.delivery.data = data;
    ingest_fixture.router.delivery.token = 1u;
    ingest_fixture.router.delivery.state = TAVRN_ROUTER_DELIVERY_POST_ACK_COMMIT;
    ingest_fixture.router.pending_ingest.input.transmitter = peer_b;
    ingest_fixture.router.pending_ingest.input.data = data;
    ingest_fixture.router.pending_ingest.input.data.app_len = TAVRN_LINK_APP_BYTES + 1u;
    ingest_fixture.router.pending_ingest.valid = 1u;
    CHECK("LINK-02", tavrn_router_tick(&ingest_fixture.router, 1u) ==
                          AODV_STATUS_INVALID &&
                          tavrn_router_fault_reason(&ingest_fixture.router) ==
                              TAVRN_ROUTER_FAULT_POST_ACK_INGEST_INVALID &&
                          ingest_fixture.router.pending_ingest.valid != 0u &&
                          tavrn_router_delivery_state(&ingest_fixture.router) ==
                              TAVRN_ROUTER_DELIVERY_POST_ACK_COMMIT &&
                          ingest_fixture.application_recorder.commit_count == 0u &&
                          tavrn_router_tick(&ingest_fixture.router, 2u) ==
                              AODV_STATUS_INVALID);
}

static void test_aodv_06_failure_overflow_and_wrap_order(void)
{
    integration_fixture_t overflow_fixture;
    integration_fixture_t wrap_fixture;
    tavrn_link_event_t event;
    tavrn_router_dispatch_event_t dispatch_event;
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_direct_peer_t peer_d = make_peer(adva_d);
    uint8_t i;

    if (!setup_fixture(&overflow_fixture, 0u) ||
        !init_router(&overflow_fixture, NULL)) {
        CHECK("AODV-06", 0);
        return;
    }
    for (i = 0u; i < TAVRN_ROUTER_FAILURE_CAPACITY; i++) {
        overflow_fixture.router.failures[i].peer = (i % 3u) == 0u ? peer_b :
            ((i % 3u) == 1u ? peer_c : peer_d);
        overflow_fixture.router.failures[i].peer.adva.bytes[2] = (uint8_t)(0x20u + i);
        overflow_fixture.router.failures[i].peer.adva.bytes[5] = 0xc1u;
        overflow_fixture.router.failures[i].peer.logical_id.value =
            (uint16_t)overflow_fixture.router.failures[i].peer.adva.bytes[0] |
            ((uint16_t)overflow_fixture.router.failures[i].peer.adva.bytes[1] << 8);
        overflow_fixture.router.failures[i].admission_order = (uint32_t)i + 1u;
        overflow_fixture.router.failures[i].valid = 1u;
    }
    memset(&event, 0, sizeof(event));
    event.type = TAVRN_LINK_EVENT_RETRY_EXHAUSTED;
    event.detail.owned_data.next_hop = peer_b;
    event.detail.owned_data.data = make_data(make_peer(adva_a).logical_id,
                                             peer_b.logical_id, 0x7fe0u,
                                             TAVRN_DATA_ORIGINATED);
    CHECK("AODV-06", tavrn_router_handle_link_event(&overflow_fixture.router, &event,
                                                        1u) == TAVRN_ROUTER_EVENT_INVALID &&
                          tavrn_router_fault_reason(&overflow_fixture.router) ==
                              TAVRN_ROUTER_FAULT_FAILURE_OBLIGATION_OVERFLOW &&
                          failure_obligation_count(&overflow_fixture.router) ==
                              TAVRN_ROUTER_FAILURE_CAPACITY &&
                          overflow_fixture.router.failure_overflow.valid != 0u &&
                          memcmp(&overflow_fixture.router.failure_overflow.owned, &event.detail.owned_data,
                                 sizeof(event.detail.owned_data)) == 0 &&
                           tavrn_router_counters(&overflow_fixture.router)
                               ->failure_set_overflow == 1u);
    event.detail.owned_data.data.ownership = TAVRN_DATA_TRANSIT;
    CHECK("AODV-06", tavrn_router_handle_link_event(&overflow_fixture.router, &event,
                                                        1u) == TAVRN_ROUTER_EVENT_INVALID &&
                           memcmp(&overflow_fixture.router.failure_overflow.owned,
                                  &event.detail.owned_data,
                                  sizeof(event.detail.owned_data)) != 0 &&
                           overflow_fixture.router.failure_overflow.owned.data.ownership ==
                               TAVRN_DATA_ORIGINATED);
    memset(&dispatch_event, 0xa5, sizeof(dispatch_event));
    CHECK("AODV-06", tavrn_router_dispatch_ex(&overflow_fixture.router, 2u,
                                                 &dispatch_event) ==
                          TAVRN_ROUTER_EVENT_INVALID &&
                          dispatch_event.type == TAVRN_ROUTER_DISPATCH_EVENT_NONE &&
                          tavrn_router_tick(&overflow_fixture.router, 2u) ==
                              AODV_STATUS_INVALID &&
                          tavrn_router_handle_link_event(&overflow_fixture.router, &event,
                                                         3u) == TAVRN_ROUTER_EVENT_INVALID &&
                          overflow_fixture.router.failure_overflow.owned.data.data_seq ==
                              0x7fe0u);

    if (!setup_fixture(&wrap_fixture, 0u) || !init_router(&wrap_fixture, NULL)) {
        CHECK("AODV-06", 0);
        return;
    }
    wrap_fixture.router.failures[0].peer = peer_b;
    wrap_fixture.router.failures[0].admission_order = UINT32_MAX;
    wrap_fixture.router.failures[0].valid = 1u;
    wrap_fixture.router.failures[1].peer = peer_c;
    wrap_fixture.router.failures[1].admission_order = 1u;
    wrap_fixture.router.failures[1].valid = 1u;
    wrap_fixture.router.failures[2].peer = peer_d;
    wrap_fixture.router.failures[2].admission_order = 2u;
    wrap_fixture.router.failures[2].valid = 1u;
    CHECK("AODV-06", tavrn_router_tick(&wrap_fixture.router, 1u) == AODV_STATUS_BUSY &&
                          wrap_fixture.router.failures[0].valid == 0u &&
                          wrap_fixture.router.failures[1].valid != 0u &&
                          tavrn_router_tick(&wrap_fixture.router, 2u) == AODV_STATUS_BUSY &&
                          wrap_fixture.router.failures[1].valid == 0u &&
                          tavrn_router_tick(&wrap_fixture.router, 3u) == AODV_STATUS_OK &&
                          wrap_fixture.router.failures[2].valid == 0u);
}

static void test_scheduler_trace_captures_wrong_receiver_rrep(void)
{
    integration_fixture_t fixture;
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_direct_peer_t peer_d = make_peer(adva_d);
    tavrn_validated_control_t control;
    tavrn_decoded_frame_t frame;
    ble_mesh_sched_event_t scheduler_event;
    tavrn_router_phase_trace_t trace;
    tavrn_router_event_status_t status;

    if (!setup_fixture(&fixture, 0u) || !init_router(&fixture, NULL)) {
        CHECK("AODV-03", 0);
        return;
    }
    control = make_rrep(peer_c.logical_id, peer_d.logical_id, 0x8161u,
                        RREP_DESTINATION_SEQUENCE, 10u);
    pdu_put_u16(&control, 7u, peer_c.logical_id.value);
    frame = make_control_frame(&control, adva_b);
    if (!make_scheduler_rx_event(&frame, adva_b, &scheduler_event)) {
        CHECK("AODV-03", 0);
        return;
    }
    scheduler_event.channel = 38u;
    memset(&trace, 0xa5, sizeof(trace));
    status = tavrn_router_handle_scheduler_event_ex(&fixture.router,
                                                     &scheduler_event, 1u, &trace);
    CHECK("AODV-03", status == SCHEDULER_TRACE_WRONG_RECEIVER_RREP_STATUS &&
                          trace.phase == TAVRN_ROUTER_TRACE_SCHEDULER_EVENT &&
                          trace.detail.scheduler_event.status == status &&
                          trace.detail.scheduler_event.input_present ==
                              TAVRN_ROUTER_TRACE_PRESENT &&
                          trace.detail.scheduler_event.input_event_type ==
                              scheduler_event.type &&
                          trace.detail.scheduler_event.input_fault ==
                              scheduler_event.fault &&
                          trace.detail.scheduler_event.input_channel ==
                              scheduler_event.channel &&
                          trace.detail.scheduler_event.input_rssi_magnitude_db ==
                              scheduler_event.rssi_magnitude_db &&
                          memcmp(trace.detail.scheduler_event.input_advertiser.bytes,
                                 scheduler_event.adv_addr, TAVRN_ADVA_LEN) == 0 &&
                          trace.detail.scheduler_event.input_adv_len ==
                              scheduler_event.adv_len &&
                          trace.detail.scheduler_event.wire_decode_present ==
                              TAVRN_ROUTER_TRACE_PRESENT &&
                          trace.detail.scheduler_event.wire_decode_result ==
                              TAVRN_CODEC_OK &&
                          trace.detail.scheduler_event.decoded_frame_present ==
                              TAVRN_ROUTER_TRACE_PRESENT &&
                          trace.detail.scheduler_event.decoded_frame_type ==
                              TAVRN_WIRE_E_RREP &&
                          trace.detail.scheduler_event.link_step_present ==
                              TAVRN_ROUTER_TRACE_PRESENT &&
                          trace.detail.scheduler_event.link_step_status ==
                              TAVRN_LINK_STEP_EVENT &&
                          trace.detail.scheduler_event.link_event_present ==
                              TAVRN_ROUTER_TRACE_PRESENT &&
                          trace.detail.scheduler_event.link_event_type ==
                              TAVRN_LINK_EVENT_RX_CONTROL &&
                          trace.detail.scheduler_event.rx_control_present ==
                              TAVRN_ROUTER_TRACE_PRESENT &&
                          memcmp(&trace.detail.scheduler_event.rx_control.transmitter,
                                 &peer_b, sizeof(peer_b)) == 0 &&
                          trace.detail.scheduler_event.rx_control.rssi_magnitude_db ==
                              scheduler_event.rssi_magnitude_db &&
                          trace.detail.scheduler_event.rx_control.control.type ==
                              TAVRN_WIRE_E_RREP &&
                          trace.detail.scheduler_event.rx_control.control.pdu_len ==
                              control.pdu_len &&
                          memcmp(trace.detail.scheduler_event.rx_control.control.pdu,
                                 control.pdu, TAVRN_LINK_CONTROL_PDU_MAX) == 0 &&
                          pdu_u16(&trace.detail.scheduler_event.rx_control.control,
                                  7u) == peer_c.logical_id.value);
}

int main(void)
{
    test_link_02_stale_candidate_has_no_second_action();
    test_gtt_03_duplicate_data_and_flood_evidence();
    test_gtt_03_max_hops_reports_direct_transmitter_only();
    test_aodv_02_hop_15_transit_is_rejected();
    test_link_04_terminal_outcomes_release_transit_custody();
    test_link_02_busy_paths_consume_evicted_transit_custody();
    test_aodv_06_backpressured_retry_exhausted_remains_due();
    test_gtt_03_real_event_role_extraction();
    test_gtt_03_reachable_event_binding();
    test_gtt_06_full_router_scoped_submission();
    test_gtt_05_route_gtt_independence();
    test_aodv_only_router_submission();
    test_link_02_final_delivery_requires_reservation();
    test_gtt_03_rejected_flood_is_ignored();
    test_link_02_delivery_reservation_exactness();
    test_link_04_retained_action_dispatch();
    test_aodv_07_unsent_rrep_cancel_and_local_action_events();
    test_aodv_06_failure_obligation_bounds_and_release_invariant();
    test_aodv_06_failure_capacity_rate_and_expiry_order();
    test_router_fail_stop_delivery_invariants();
    test_aodv_06_failure_overflow_and_wrap_order();
    test_scheduler_trace_captures_wrong_receiver_rrep();
    if (failures != 0u) {
        printf("tavrn_phase3_integration RED tests failed: %u assertion(s)\n",
               failures);
        return 1;
    }
    printf("tavrn_phase3_integration tests passed\n");
    return 0;
}
