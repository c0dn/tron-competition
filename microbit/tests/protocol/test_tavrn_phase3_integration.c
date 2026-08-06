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

static unsigned int failures;
static const char *reported[4];
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

typedef struct integration_fixture {
    ble_mesh_scheduler_t scheduler;
    tavrn_link_v2_t link;
    aodv_core_t core;
    tavrn_gtt_t gtt;
    tavrn_gtt_storage_t gtt_storage;
    tavrn_full_t full;
    tavrn_router_t router;
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

static int bind_full_router(integration_fixture_t *fixture)
{
    tavrn_router_augmentation_hooks_t hooks;

    if (fixture == NULL ||
        tavrn_full_init(&fixture->full, &fixture->gtt) != TAVRN_FULL_INIT_OK) {
        return 0;
    }
    hooks = tavrn_full_router_hooks(&fixture->full);
    return tavrn_router_init(&fixture->router, &fixture->link, &fixture->core,
                             &hooks) == TAVRN_ROUTER_INIT_OK;
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
    if (tavrn_router_init(&fixture.router, &fixture.link, &fixture.core,
                          &hooks) != TAVRN_ROUTER_INIT_OK) {
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

    if (!setup_fixture(&fixture, 0u) ||
        tavrn_router_init(&fixture.router, &fixture.link, &fixture.core, NULL) !=
            TAVRN_ROUTER_INIT_OK) {
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

int main(void)
{
    test_gtt_03_real_event_role_extraction();
    test_gtt_03_reachable_event_binding();
    test_gtt_06_full_router_scoped_submission();
    test_gtt_05_route_gtt_independence();
    test_aodv_only_router_submission();
    if (failures != 0u) {
        printf("tavrn_phase3_integration RED tests failed: %u assertion(s)\n",
               failures);
        return 1;
    }
    printf("tavrn_phase3_integration tests passed\n");
    return 0;
}
