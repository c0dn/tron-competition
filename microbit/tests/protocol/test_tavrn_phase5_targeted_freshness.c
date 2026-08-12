#include "aodv_core.h"
#include "ble_mesh_scheduler.h"
#include "tavrn_full.h"
#include "tavrn_gtt.h"
#include "tavrn_link_v2.h"
#include "tavrn_maintenance.h"
#include "tavrn_mentorship.h"
#include "tavrn_phase5_targeted_freshness_contract.h"
#include "tavrn_router.h"
#include "tavrn_wire_v2.h"
#include "tron_timer_config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NETWORK_ID 0x2au
#define HARD_EXPIRY_MS 300000u
#define HELLO_DEDUPE_MS 10000u

static unsigned int failures;
static unsigned int structural_failures;
static const char *reported[6];
static unsigned int reported_count;

static const uint8_t adva_a[TAVRN_ADVA_LEN] = { 0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu };
static const uint8_t adva_b[TAVRN_ADVA_LEN] = { 0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u };
static const uint8_t adva_c[TAVRN_ADVA_LEN] = { 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0xc1u };
static const uint8_t adva_d[TAVRN_ADVA_LEN] = { 0x34u, 0x56u, 0x78u, 0x9au, 0xbcu, 0xc2u };
static const uint8_t adva_e[TAVRN_ADVA_LEN] = { 0x55u, 0x66u, 0x77u, 0x88u, 0x99u, 0xc3u };
static const uint8_t adva_f[TAVRN_ADVA_LEN] = { 0x66u, 0x77u, 0x88u, 0x99u, 0xaau, 0xc4u };

typedef struct targeted_fixture {
    ble_mesh_scheduler_t scheduler;
    tavrn_link_v2_t link;
    aodv_core_t aodv;
    tavrn_gtt_t gtt;
    tavrn_gtt_storage_t gtt_storage;
    tavrn_full_t full;
    tavrn_router_t router;
    tavrn_mentorship_t mentorship;
    tavrn_maintenance_t maintenance;
} targeted_fixture_t;

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

#define STRUCTURAL(label, expression) \
    do { \
        if (!(expression)) { \
            printf("STRUCTURAL %s: %s:%d: %s\n", (label), __FILE__, __LINE__, #expression); \
            structural_failures++; \
        } \
    } while (0)

#define REACHED(name) printf("REACHED %s\n", (name))

static tavrn_adva_t adva(const uint8_t bytes[TAVRN_ADVA_LEN])
{
    tavrn_adva_t value;

    memcpy(value.bytes, bytes, TAVRN_ADVA_LEN);
    return value;
}

static tavrn_logical_id_t sid8(const uint8_t bytes[TAVRN_ADVA_LEN])
{
    tavrn_logical_id_t value;

    value.width = TAVRN_IDENTITY_SID8;
    value.value = bytes[0];
    return value;
}

static tavrn_direct_peer_t peer(const uint8_t bytes[TAVRN_ADVA_LEN],
                                tavrn_identity_width_t width)
{
    tavrn_direct_peer_t value;

    memset(&value, 0, sizeof(value));
    value.logical_id.width = width;
    value.logical_id.value = width == TAVRN_IDENTITY_SID8 ? bytes[0] :
        (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    value.adva = adva(bytes);
    return value;
}

static int identity_allowed(void *context, const tavrn_logical_id_t *id,
                            const tavrn_adva_t *direct_or_null)
{
    (void)context;
    (void)id;
    (void)direct_or_null;
    return 0;
}

static tavrn_link_config_t link_config(const uint8_t local[TAVRN_ADVA_LEN])
{
    tavrn_link_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = peer(local, TAVRN_IDENTITY_SID16);
    config.network_id = NETWORK_ID;
    config.hack_max_attempts = 3u;
    config.busy_max_responses = 3u;
    config.hack_response_ms = 250u;
    config.hack_turnaround_ms = 8u;
    config.busy_backoff_ms = 500u;
    config.data_forward_deadline_ms = 5000u;
    config.candidate_resolve_ms = 10u;
    config.data_dedupe_ms = 10000u;
    config.flood_dedupe_ms = 10000u;
    config.flood_jitter_min_ms = 20u;
    config.flood_jitter_max_ms = 120u;
    return config;
}

static aodv_core_config_t aodv_config(const uint8_t local[TAVRN_ADVA_LEN])
{
    aodv_core_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = peer(local, TAVRN_IDENTITY_SID16);
    config.network_id = NETWORK_ID;
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

static int observe_member(targeted_fixture_t *fixture,
                          const uint8_t identity[TAVRN_ADVA_LEN], uint16_t serial,
                          uint32_t now_ms)
{
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_observe_status_t status;

    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = adva(identity);
    evidence.serial = serial;
    evidence.serial_present = 1u;
    evidence.hop_count = 1u;
    evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    status = tavrn_gtt_observe(&fixture->gtt, &evidence, now_ms);
    return status == TAVRN_GTT_OBSERVE_ADDED || status == TAVRN_GTT_OBSERVE_REFRESHED;
}

static void clear_queue(targeted_fixture_t *fixture)
{
    uint8_t index;

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (fixture->scheduler.routed_tx_queue.entries[index].occupied != 0u) {
            (void)ble_mesh_tx_queue_remove(&fixture->scheduler.routed_tx_queue,
                                           index, NULL);
        }
    }
}

static int fixture_init(targeted_fixture_t *fixture,
                        const uint8_t local[TAVRN_ADVA_LEN])
{
    tavrn_gtt_config_t gtt_config;
    tavrn_router_incarnation_config_t incarnation;
    tavrn_mentorship_config_t mentorship_config;
    tavrn_maintenance_config_t maintenance_config;
    tavrn_router_augmentation_hooks_t hooks;
    tavrn_link_config_t configured_link = link_config(local);
    aodv_core_config_t configured_aodv = aodv_config(local);
    tavrn_mentorship_state_snapshot_t state;
    const uint8_t *seed = memcmp(local, adva_b, TAVRN_ADVA_LEN) == 0 ? adva_a : adva_b;

    memset(fixture, 0, sizeof(*fixture));
    memset(&gtt_config, 0, sizeof(gtt_config));
    gtt_config.local_identity = adva(local);
    gtt_config.soft_expiry_ms = HARD_EXPIRY_MS / 2u;
    gtt_config.hard_expiry_ms = HARD_EXPIRY_MS;
    gtt_config.departed_retention_ms = HARD_EXPIRY_MS * 2u;
    memset(&incarnation, 0, sizeof(incarnation));
    incarnation.feature_level = TAVRN_ROUTER_FEATURE_FULL_TAVRN;
    incarnation.boot_nonce = 0x5a01u;
    incarnation.reboot_announce_ms = 30u;
    memset(&mentorship_config, 0, sizeof(mentorship_config));
    mentorship_config.offer_window_ms = 10u;
    mentorship_config.page_timeout_ms = 30u;
    mentorship_config.self_bootstrap_ms = 20u;
    mentorship_config.offer_suppression_ms = 100u;
    mentorship_config.join_dedupe_ms = 100u;
    mentorship_config.sync_dedupe_ms = 100u;
    mentorship_config.rssi_weak_magnitude_db = 90u;
    mentorship_config.rssi_strong_magnitude_db = 30u;
    mentorship_config.rssi_weak_delay_ms = 500u;
    mentorship_config.rssi_strong_delay_ms = 10u;
    mentorship_config.jitter_max_ms = 50u;
    mentorship_config.page_attempts = 3u;
    memset(&maintenance_config, 0, sizeof(maintenance_config));
    maintenance_config.hello_change_ms = 10u;
    maintenance_config.hello_stable_ms = 160u;
    maintenance_config.hello_alpha_permille = 800u;
    maintenance_config.hello_snap_permille = 950u;
    maintenance_config.topology_sample_ms = 25u;
    maintenance_config.hello_dedupe_ms = HELLO_DEDUPE_MS;
    maintenance_config.initial_node_sequence = 0xfffeu;

    ble_mesh_scheduler_init(&fixture->scheduler, 0u, local);
    if (tavrn_link_v2_init(&fixture->link, &fixture->scheduler, &configured_link, 0u) !=
            TAVRN_LINK_INIT_OK ||
        aodv_core_init(&fixture->aodv, &configured_aodv, 0u) != AODV_INIT_OK ||
        tavrn_gtt_init(&fixture->gtt, &fixture->gtt_storage, &gtt_config, 0u) !=
            TAVRN_GTT_INIT_OK ||
        tavrn_full_init(&fixture->full, &fixture->gtt) != TAVRN_FULL_INIT_OK) {
        return 0;
    }
    hooks = tavrn_full_router_hooks(&fixture->full);
    if (tavrn_router_init_with_incarnation(&fixture->router, &fixture->link,
                                           &fixture->aodv, &hooks, &incarnation, 0u) !=
            TAVRN_ROUTER_INCARNATION_OK ||
        tavrn_mentorship_init(&fixture->mentorship, &fixture->router, &fixture->gtt,
                              &mentorship_config, 0u) != TAVRN_MENTORSHIP_OK ||
        tavrn_maintenance_init(&fixture->maintenance, &fixture->router, &fixture->gtt,
                               &maintenance_config) != TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_activate(&fixture->maintenance, 0u) != TAVRN_MAINTENANCE_GATED ||
        !observe_member(fixture, seed, 1u, 1u) ||
        tavrn_mentorship_tick(&fixture->mentorship, 20u) != TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED ||
        tavrn_mentorship_state_snapshot(&fixture->mentorship, &state) != TAVRN_MENTORSHIP_OK ||
        state.state != TAVRN_MENTORSHIP_SID8_ACTIVE ||
        state.active_width != TAVRN_IDENTITY_SID8) {
        return 0;
    }
    clear_queue(fixture);
    return tavrn_maintenance_activate(&fixture->maintenance, 20u) == TAVRN_MAINTENANCE_OK;
}

static tron_application_data_t application(tavrn_logical_id_t destination)
{
    tron_application_data_t data;

    memset(&data, 0, sizeof(data));
    data.final_destination = destination;
    data.app_kind = 0x7fu;
    data.app_len = 1u;
    data.app_bytes[0] = 0xa5u;
    return data;
}

static uint16_t pdu_u16(const tavrn_validated_control_t *control, uint8_t offset)
{
    return (uint16_t)control->pdu[offset] | ((uint16_t)control->pdu[offset + 1u] << 8);
}

static int control_equal(const tavrn_validated_control_t *left,
                         const tavrn_validated_control_t *right)
{
    if (left == NULL || right == NULL || left->type != right->type ||
        left->pdu_len != right->pdu_len) {
        return 0;
    }
    return memcmp(left->pdu, right->pdu, left->pdu_len) == 0;
}

static tavrn_validated_control_t make_rrep(tavrn_logical_id_t receiver,
                                           tavrn_logical_id_t destination,
                                           tavrn_logical_id_t origin,
                                           uint16_t request_id, uint16_t lifetime_ms)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_E_RREP;
    control.pdu_len = 16u;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_E_RREP;
    control.pdu[5] = 0x80u;
    control.pdu[6] = 0x10u;
    control.pdu[7] = (uint8_t)receiver.value;
    control.pdu[8] = (uint8_t)destination.value;
    control.pdu[9] = 1u;
    control.pdu[10] = 0u;
    control.pdu[11] = (uint8_t)origin.value;
    control.pdu[12] = (uint8_t)request_id;
    control.pdu[13] = (uint8_t)(request_id >> 8);
    control.pdu[14] = (uint8_t)lifetime_ms;
    control.pdu[15] = (uint8_t)(lifetime_ms >> 8);
    return control;
}

static int install_route(targeted_fixture_t *fixture,
                         const uint8_t destination[TAVRN_ADVA_LEN],
                         const uint8_t next_hop[TAVRN_ADVA_LEN], uint32_t now_ms,
                         uint16_t lifetime_ms)
{
    tron_application_data_t data = application(sid8(destination));
    aodv_action_t action;
    aodv_control_input_t reply_input;
    tavrn_validated_control_t reply;
    aodv_route_snapshot_t route;

    if (tavrn_router_submit_application(&fixture->router, &data, now_ms) != AODV_STATUS_QUEUED ||
        aodv_core_poll_action(&fixture->aodv, &action) != AODV_ACTION_POLL_OK ||
        action.type != AODV_ACTION_SEND_RREQ) {
        return 0;
    }
    reply = make_rrep(fixture->aodv.config.local_peer.logical_id, sid8(destination),
                      fixture->aodv.config.local_peer.logical_id,
                      pdu_u16(&action.detail.control.control, 8u), lifetime_ms);
    memset(&reply_input, 0, sizeof(reply_input));
    reply_input.transmitter = peer(next_hop, TAVRN_IDENTITY_SID8);
    reply_input.control = reply;
    if (aodv_core_ingest_control(&fixture->aodv, &reply_input, now_ms + 1u) != AODV_STATUS_OK ||
        aodv_core_route_snapshot(&fixture->aodv, &data.final_destination, &route) !=
            AODV_ROUTE_QUERY_FOUND || route.state != AODV_ROUTE_VALID ||
        route.next_hop.logical_id.value != next_hop[0] ||
        aodv_core_poll_action(&fixture->aodv, &action) != AODV_ACTION_POLL_OK ||
        action.type != AODV_ACTION_FORWARD_DATA) {
        return 0;
    }
    return 1;
}

static int demand_subject(targeted_fixture_t *fixture,
                          const uint8_t subject[TAVRN_ADVA_LEN])
{
    tavrn_gtt_expiry_snapshot_t state;
    tavrn_adva_t identity = adva(subject);

    return observe_member(fixture, subject, 1u, 1u) &&
        tavrn_gtt_application_request(&fixture->gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                                      &identity, 2u, &state) ==
            TAVRN_GTT_APPLICATION_CHANGED;
}

static tavrn_validated_control_t ordinary_hello(const uint8_t origin[TAVRN_ADVA_LEN],
                                                uint16_t sequence)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_HELLO;
    control.pdu_len = 20u;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_HELLO;
    control.pdu[5] = 0x80u;
    control.pdu[6] = 0x10u;
    control.pdu[7] = 0xffu;
    control.pdu[8] = 0xffu;
    memcpy(&control.pdu[9], origin, TAVRN_ADVA_LEN);
    control.pdu[15] = (uint8_t)sequence;
    control.pdu[16] = (uint8_t)(sequence >> 8);
    return control;
}

static int fill_control_queue(targeted_fixture_t *fixture, uint32_t now_ms)
{
    uint8_t index;

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        tavrn_link_event_t outcome;
        tavrn_validated_control_t control = ordinary_hello(
            fixture->link.config.local_peer.adva.bytes, (uint16_t)(0x9000u + index));

        memset(&outcome, 0, sizeof(outcome));
        if (tavrn_link_v2_send_control(&fixture->link, &control, NULL, 0u, now_ms,
                                       &outcome) != TAVRN_LINK_SEND_OK) {
            return 0;
        }
    }
    return ble_mesh_tx_queue_count(&fixture->scheduler.routed_tx_queue) ==
        BLE_MESH_TX_QUEUE_CAPACITY;
}

static const uint8_t targeted_request[] = {
    0x02u, 0x01u, 0x06u, 0x17u, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, NETWORK_ID, 0x04u, 0xb8u, 0x10u,
    0xdcu, 0xdcu, 0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu,
    0x02u, 0x00u, 0x01u, 0xdcu, 0x01u,
};
static const uint8_t targeted_response[] = {
    0x02u, 0x01u, 0x06u, 0x17u, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, NETWORK_ID, 0x04u, 0xa8u, 0x10u,
    0x18u, 0x18u, 0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u,
    0x03u, 0x00u, 0x01u, 0xdcu, 0xf0u,
};
static const uint8_t ordinary_vector[] = {
    0x02u, 0x01u, 0x06u, 0x17u, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, NETWORK_ID, 0x04u, 0x80u, 0x10u,
    0xffu, 0xffu, 0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu,
    0x34u, 0x12u, 0x00u, 0x00u, 0x00u,
};

static tavrn_validated_control_t control_from_adv(const uint8_t *adv, size_t length)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_HELLO;
    control.pdu_len = (uint8_t)(length - 7u);
    memcpy(control.pdu, &adv[7], control.pdu_len);
    return control;
}

static tavrn_rx_control_event_t rx_event(const tavrn_validated_control_t *control,
                                         const uint8_t transmitter[TAVRN_ADVA_LEN])
{
    tavrn_rx_control_event_t event;

    memset(&event, 0, sizeof(event));
    event.transmitter = peer(transmitter, TAVRN_IDENTITY_SID8);
    event.control = *control;
    return event;
}

static const tavrn_targeted_freshness_context_snapshot_t *context_at(
    const tavrn_targeted_freshness_snapshot_t *state, uint8_t index,
    const char *scenario)
{
    if (state == NULL) {
        printf("STRUCTURAL context-index-%s: index=%u\n", scenario, (unsigned int)index);
        structural_failures++;
        return NULL;
    }
    if (index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY) {
        return &state->contexts[index];
    }
    if (index >= TAVRN_TARGETED_RESPONSE_CONTEXT_INDEX_BASE &&
        (uint8_t)(index - TAVRN_TARGETED_RESPONSE_CONTEXT_INDEX_BASE) <
            TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY) {
        return &state->response_contexts[
            index - TAVRN_TARGETED_RESPONSE_CONTEXT_INDEX_BASE];
    }
    printf("STRUCTURAL context-index-%s: index=%u\n", scenario, (unsigned int)index);
    structural_failures++;
    return NULL;
}

static void complete_targeted_control(targeted_fixture_t *fixture,
                                      const tavrn_targeted_freshness_action_t *action,
                                      uint32_t now_ms, const char *requirement);

#if !defined(TAVRN_PHASE5_TARGETED_RED_MODE)
typedef struct external_high_token_probe {
    tavrn_maintenance_high_token_completion_t completion;
    tavrn_maintenance_high_token_status_t status;
    tavrn_maintenance_high_token_status_t release_status;
    tavrn_maintenance_t *maintenance;
    tavrn_link_v2_t *link;
    uint8_t calls;
    uint8_t release_during_callback;
    uint8_t token_absent_during_callback;
} external_high_token_probe_t;

static tavrn_maintenance_high_token_status_t external_high_token_completion(
    void *context, const tavrn_maintenance_high_token_completion_t *completion)
{
    external_high_token_probe_t *probe = context;

    if (probe == NULL || completion == NULL) {
        return TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
    }
    probe->calls++;
    probe->completion = *completion;
    if (probe->link != NULL) {
        probe->token_absent_during_callback =
            (uint8_t)!tavrn_link_v2_tracked_token_in_use(probe->link,
                                                          completion->token);
    }
    if (probe->release_during_callback != 0u) {
        if (probe->maintenance == NULL || probe->link == NULL) {
            return TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
        }
        probe->release_status = tavrn_maintenance_high_token_release(
            probe->maintenance, probe->link, completion->owner, completion->purpose,
            completion->token);
        if (probe->release_status != TAVRN_MAINTENANCE_HIGH_TOKEN_OK) {
            return TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID;
        }
    }
    return probe->status;
}

static void test_external_high_token_fault_cancellation(
    ble_mesh_sched_event_type_t fault_type)
{
    targeted_fixture_t fixture;
    tavrn_targeted_freshness_action_t action;
    tavrn_maintenance_high_token_dispatch_result_t result;
    external_high_token_probe_t probe;
    tavrn_adva_t subject = adva(adva_b);
    uint16_t token = 0u;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t link_output;

    STRUCTURAL("external-fault-cancel-fixture", fixture_init(&fixture, adva_a));
    STRUCTURAL("external-fault-cancel-demand", demand_subject(&fixture, adva_b));
    STRUCTURAL("external-fault-cancel-route", install_route(&fixture, adva_b, adva_c,
                                                               HARD_EXPIRY_MS - 1u,
                                                               10000u));
    if (structural_failures != 0u) {
        return;
    }
    memset(&action, 0, sizeof(action));
    memset(&probe, 0, sizeof(probe));
    probe.status = TAVRN_MAINTENANCE_HIGH_TOKEN_OK;
    probe.maintenance = &fixture.maintenance;
    probe.link = &fixture.link;
    probe.release_during_callback = 1u;
    CHECK("MAINT-07", phase5_targeted_begin_stage0(
                          &fixture.maintenance, &fixture.router, &fixture.link,
                          &fixture.gtt, &subject, HARD_EXPIRY_MS, &action) ==
                          TAVRN_TARGETED_FRESHNESS_OK &&
                          action.context_index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY &&
                          tavrn_maintenance_high_token_reserve(
                              &fixture.maintenance, &fixture.link, 0x7000u,
                              (uint16_t)fault_type, external_high_token_completion,
                              &probe, &token) == TAVRN_MAINTENANCE_HIGH_TOKEN_OK &&
                          tavrn_link_v2_send_tracked_control(
                              &fixture.link, &action.control,
                              &fixture.maintenance.targeted[action.context_index]
                                   .snapshot.receiver,
                              token, HARD_EXPIRY_MS, NULL) == TAVRN_LINK_SEND_OK &&
                          tavrn_link_v2_tracked_token_in_use(&fixture.link, token));
    if (action.context_index >= TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.type = fault_type;
    CHECK("MAINT-07", tavrn_link_v2_on_scheduler_event(&fixture.link, &event,
                                                         HARD_EXPIRY_MS + 1u,
                                                         &link_output) ==
                          TAVRN_LINK_STEP_NO_EVENT &&
                          tavrn_maintenance_high_token_scheduler_event(
                              &fixture.maintenance, &fixture.router, &fixture.link,
                              &event, HARD_EXPIRY_MS + 1u, &result) ==
                              TAVRN_MAINTENANCE_HIGH_TOKEN_OK &&
                          result.external_handled != 0u &&
                          result.external_status == TAVRN_MAINTENANCE_HIGH_TOKEN_OK &&
                          probe.calls == 1u &&
                          probe.token_absent_during_callback != 0u &&
                          probe.release_status == TAVRN_MAINTENANCE_HIGH_TOKEN_OK &&
                          fixture.maintenance.external_high_token.valid == 0u &&
                          !tavrn_link_v2_tracked_token_in_use(&fixture.link, token));
    CHECK("MAINT-07", tavrn_maintenance_high_token_scheduler_event(
                          &fixture.maintenance, &fixture.router, &fixture.link, &event,
                          HARD_EXPIRY_MS + 2u, &result) == TAVRN_MAINTENANCE_HIGH_TOKEN_OK &&
                          result.external_handled == 0u && probe.calls == 1u);
}

static void test_external_high_token_authority(void)
{
    targeted_fixture_t fixture;
    targeted_fixture_t wrap_fixture;
    targeted_fixture_t fault_fixture;
    tavrn_targeted_freshness_action_t action;
    tavrn_targeted_freshness_action_t verification_action;
    tavrn_targeted_freshness_action_t fault_action;
    tavrn_targeted_freshness_snapshot_t state;
    tavrn_rreq_verification_action_t verification_rreq;
    tavrn_maintenance_high_token_dispatch_result_t result;
    external_high_token_probe_t probe;
    external_high_token_probe_t wrap_probe;
    external_high_token_probe_t fault_probe;
    tavrn_adva_t subject = adva(adva_b);
    tavrn_adva_t verification_subject = adva(adva_c);
    uint16_t token = 0u;
    uint16_t wrap_token = 0u;
    uint16_t fault_token = 0u;
    uint32_t verification_now;
    uint32_t scheduler_events_before;
    uint8_t canceled = 0u;
    ble_mesh_sched_event_t event;

    REACHED("external-high-token-authority");
    STRUCTURAL("external-token-fixture", fixture_init(&fixture, adva_a));
    STRUCTURAL("external-token-demand", demand_subject(&fixture, adva_b));
    STRUCTURAL("external-token-route", install_route(&fixture, adva_b, adva_c,
                                                       HARD_EXPIRY_MS - 1u, 10000u));
    if (structural_failures != 0u) {
        return;
    }
    memset(&probe, 0, sizeof(probe));
    probe.status = TAVRN_MAINTENANCE_HIGH_TOKEN_OK;
    CHECK("MAINT-07", tavrn_maintenance_high_token_reserve(
                          &fixture.maintenance, &fixture.link, 0u, 1u,
                          external_high_token_completion, &probe, &token) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_OK);
    CHECK("MAINT-07", token == 0x8000u);
    CHECK("MAINT-07", tavrn_maintenance_high_token_reserve(
                          &fixture.maintenance, &fixture.link, 0u, 2u,
                          external_high_token_completion, &probe, &wrap_token) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_BUSY && wrap_token == 0u);
    CHECK("MAINT-07", phase5_targeted_seed_high_token(&fixture.maintenance,
                                                         0xffffu) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    memset(&action, 0, sizeof(action));
    CHECK("MAINT-07", phase5_targeted_begin_stage0(
                          &fixture.maintenance, &fixture.router, &fixture.link,
                          &fixture.gtt, &subject, HARD_EXPIRY_MS, &action) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", action.token == 0x8001u && action.token != token);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    if (action.context_index >= TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY) {
        STRUCTURAL("external-token-context", 0);
        return;
    }
    CHECK("MAINT-07", tavrn_link_v2_send_tracked_control(
                          &fixture.link, &action.control,
                          &state.contexts[action.context_index].receiver, token,
                          HARD_EXPIRY_MS, NULL) == TAVRN_LINK_SEND_OK);
    CHECK("MAINT-07", tavrn_maintenance_high_token_release(
                          &fixture.maintenance, &fixture.link, 0u, 1u, token) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_BUSY);
    CHECK("MAINT-07", tavrn_maintenance_high_token_release(
                          &fixture.maintenance, &fixture.link, 1u, 1u, token) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_MISMATCH);
    CHECK("MAINT-07", fixture.maintenance.external_high_token.valid != 0u);
    CHECK("MAINT-07", tavrn_link_v2_cancel_queued_tracked_control(
                          &fixture.link, &action.control, token, &canceled) ==
          TAVRN_LINK_RESOLVE_OK && canceled != 0u);
    CHECK("MAINT-07", tavrn_maintenance_high_token_release(
                          &fixture.maintenance, &fixture.link, 0u, 2u, token) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_MISMATCH);
    CHECK("MAINT-07", tavrn_maintenance_high_token_release(
                          &fixture.maintenance, &fixture.link, 0u, 1u, token) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_OK);
    CHECK("MAINT-07", tavrn_maintenance_high_token_release(
                          &fixture.maintenance, &fixture.link, 0u, 1u, token) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_NOT_FOUND);

    memset(&probe, 0, sizeof(probe));
    probe.status = TAVRN_MAINTENANCE_HIGH_TOKEN_OK;
    CHECK("MAINT-07", tavrn_maintenance_high_token_reserve(
                          &fixture.maintenance, &fixture.link, 0u, 3u,
                          external_high_token_completion, &probe, &token) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_OK);
    scheduler_events_before = fixture.maintenance.counters.targeted_scheduler_events;
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_TX_DONE;
    event.tx_token = token;
    event.tx_requested_channel_mask = 0x07u;
    event.tx_completed_channel_mask = 0x01u;
    CHECK("MAINT-07", tavrn_maintenance_high_token_scheduler_event(
                          &fixture.maintenance, &fixture.router, &fixture.link,
                          &event, HARD_EXPIRY_MS, &result) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_OK);
    CHECK("MAINT-07", result.external_handled != 0u &&
                          result.targeted_handled == 0u &&
                          result.verification_handled == 0u && probe.calls == 1u &&
                          probe.completion.owner == 0u && probe.completion.purpose == 3u &&
                          probe.completion.token == token &&
                          probe.completion.kind ==
                              TAVRN_MAINTENANCE_HIGH_TOKEN_COMPLETION_TX_DONE &&
                           probe.completion.requested_channel_mask == 0x07u &&
                           probe.completion.completed_channel_mask == 0x01u &&
                           probe.completion.completed_at_ms == HARD_EXPIRY_MS &&
                          fixture.maintenance.counters.targeted_scheduler_events ==
                              scheduler_events_before);
    CHECK("MAINT-07", tavrn_maintenance_high_token_release(
                          &fixture.maintenance, &fixture.link, 0u, 3u, token) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_OK);
    CHECK("MAINT-07", tavrn_maintenance_high_token_scheduler_event(
                          &fixture.maintenance, &fixture.router, &fixture.link,
                          &event, HARD_EXPIRY_MS + 1u, &result) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_OK && probe.calls == 1u &&
                          result.external_handled == 0u);

    CHECK("MAINT-07", tavrn_maintenance_high_token_reserve(
                          &fixture.maintenance, &fixture.link, 0u, 4u,
                          external_high_token_completion, &probe, &token) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_OK);
    CHECK("MAINT-07", tavrn_maintenance_high_token_evicted(
                          &fixture.maintenance, &fixture.router, &fixture.link,
                          token, HARD_EXPIRY_MS + 2u, &result) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_OK);
    CHECK("MAINT-07", result.external_handled != 0u && probe.calls == 2u &&
                          probe.completion.purpose == 4u &&
                           probe.completion.kind ==
                               TAVRN_MAINTENANCE_HIGH_TOKEN_COMPLETION_TX_FAILED &&
                           probe.completion.requested_channel_mask == 0u &&
                           probe.completion.completed_channel_mask == 0u &&
                           probe.completion.completed_at_ms == HARD_EXPIRY_MS + 2u);
    CHECK("MAINT-07", tavrn_maintenance_high_token_release(
                          &fixture.maintenance, &fixture.link, 0u, 4u, token) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_OK);
    CHECK("MAINT-07", tavrn_maintenance_high_token_evicted(
                          &fixture.maintenance, &fixture.router, &fixture.link,
                          token, HARD_EXPIRY_MS + 3u, &result) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_OK && probe.calls == 2u &&
                          result.external_handled == 0u);

    STRUCTURAL("external-wrap-fixture", fixture_init(&wrap_fixture, adva_a));
    STRUCTURAL("external-wrap-demand", demand_subject(&wrap_fixture, adva_b));
    STRUCTURAL("external-wrap-route", install_route(&wrap_fixture, adva_b, adva_c,
                                                      HARD_EXPIRY_MS - 1u, 10000u));
    if (structural_failures != 0u) {
        return;
    }
    memset(&wrap_probe, 0, sizeof(wrap_probe));
    wrap_probe.status = TAVRN_MAINTENANCE_HIGH_TOKEN_OK;
    CHECK("MAINT-07", phase5_targeted_seed_high_token(&wrap_fixture.maintenance,
                                                         0xfffeu) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", tavrn_maintenance_high_token_reserve(
                          &wrap_fixture.maintenance, &wrap_fixture.link, 0u, 5u,
                          external_high_token_completion, &wrap_probe, &wrap_token) ==
          TAVRN_MAINTENANCE_HIGH_TOKEN_OK && wrap_token == 0xffffu);
    CHECK("MAINT-07", phase5_targeted_seed_high_token(&wrap_fixture.maintenance,
                                                         0xffffu) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    memset(&action, 0, sizeof(action));
    CHECK("MAINT-07", phase5_targeted_begin_stage0(
                          &wrap_fixture.maintenance, &wrap_fixture.router,
                          &wrap_fixture.link, &wrap_fixture.gtt, &subject,
                          HARD_EXPIRY_MS, &action) == TAVRN_TARGETED_FRESHNESS_OK &&
                          action.token == 0x8000u);
    CHECK("MAINT-07", tavrn_maintenance_high_token_release(
                          &wrap_fixture.maintenance, &wrap_fixture.link, 0u, 5u,
                          wrap_token) == TAVRN_MAINTENANCE_HIGH_TOKEN_OK);

    STRUCTURAL("external-fault-fixture", fixture_init(&fault_fixture, adva_a));
    STRUCTURAL("external-fault-demand-a", demand_subject(&fault_fixture, adva_b));
    STRUCTURAL("external-fault-route-a", install_route(&fault_fixture, adva_b, adva_d,
                                                         HARD_EXPIRY_MS - 1u, 10000u));
    STRUCTURAL("external-fault-demand-b", demand_subject(&fault_fixture, adva_c));
    STRUCTURAL("external-fault-route-b", install_route(&fault_fixture, adva_c, adva_d,
                                                         HARD_EXPIRY_MS - 1u, 10000u));
    if (structural_failures != 0u) {
        return;
    }
    memset(&fault_action, 0, sizeof(fault_action));
    memset(&verification_action, 0, sizeof(verification_action));
    memset(&verification_rreq, 0, sizeof(verification_rreq));
    CHECK("MAINT-07", phase5_targeted_begin_stage0(
                          &fault_fixture.maintenance, &fault_fixture.router,
                          &fault_fixture.link, &fault_fixture.gtt, &subject,
                          HARD_EXPIRY_MS, &fault_action) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_begin_stage0(
                          &fault_fixture.maintenance, &fault_fixture.router,
                          &fault_fixture.link, &fault_fixture.gtt,
                          &verification_subject, HARD_EXPIRY_MS,
                          &verification_action) == TAVRN_TARGETED_FRESHNESS_OK);
    complete_targeted_control(&fault_fixture, &verification_action,
                              HARD_EXPIRY_MS + 1u, "MAINT-07");
    verification_now = HARD_EXPIRY_MS + 1u +
        tron_timer_config.aodv_net_traversal_ms;
    CHECK("MAINT-07", phase5_targeted_owner_tick(
                          &fault_fixture.maintenance, &fault_fixture.router,
                          &fault_fixture.link, &fault_fixture.gtt, verification_now,
                          &verification_action) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", tavrn_maintenance_verification_owner_tick(
                          &fault_fixture.maintenance, &fault_fixture.router,
                          &fault_fixture.link, &fault_fixture.gtt,
                          verification_now + 1u, &verification_rreq) ==
          TAVRN_RREQ_VERIFICATION_OK && verification_rreq.enqueued != 0u);
    memset(&fault_probe, 0, sizeof(fault_probe));
    fault_probe.status = TAVRN_MAINTENANCE_HIGH_TOKEN_OK;
    CHECK("MAINT-07", tavrn_maintenance_high_token_reserve(
                          &fault_fixture.maintenance, &fault_fixture.link, 0u, 6u,
                          external_high_token_completion, &fault_probe,
                          &fault_token) == TAVRN_MAINTENANCE_HIGH_TOKEN_OK);
    CHECK("MAINT-07", fault_token != fault_action.token &&
                          fault_token != verification_rreq.token);
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_RADIO_FAULT;
    CHECK("MAINT-07", tavrn_maintenance_high_token_scheduler_event(
                          &fault_fixture.maintenance, &fault_fixture.router,
                          &fault_fixture.link, &event, verification_now + 2u,
                          &result) == TAVRN_MAINTENANCE_HIGH_TOKEN_OK);
    CHECK("MAINT-07", result.targeted_handled != 0u &&
                          result.verification_handled != 0u &&
                          result.external_handled != 0u && fault_probe.calls == 1u &&
                           fault_probe.completion.token == fault_token &&
                           fault_probe.completion.kind ==
                               TAVRN_MAINTENANCE_HIGH_TOKEN_COMPLETION_RADIO_FAULT &&
                           fault_probe.completion.completed_at_ms ==
                               verification_now + 2u);
    CHECK("MAINT-07", tavrn_maintenance_high_token_release(
                          &fault_fixture.maintenance, &fault_fixture.link, 0u, 6u,
                          fault_token) == TAVRN_MAINTENANCE_HIGH_TOKEN_OK);
    CHECK("MAINT-07", tavrn_maintenance_high_token_reserve(
                          &fault_fixture.maintenance, &fault_fixture.link, 0u, 7u,
                          external_high_token_completion, &fault_probe,
                          &fault_token) == TAVRN_MAINTENANCE_HIGH_TOKEN_OK);
    event.type = BLE_MESH_SCHED_EVENT_SERVICE_FAULT;
    CHECK("MAINT-07", tavrn_maintenance_high_token_scheduler_event(
                          &fault_fixture.maintenance, &fault_fixture.router,
                          &fault_fixture.link, &event, verification_now + 4u,
                          &result) == TAVRN_MAINTENANCE_HIGH_TOKEN_OK &&
                          result.external_handled != 0u && fault_probe.calls == 2u &&
                           fault_probe.completion.purpose == 7u &&
                           fault_probe.completion.kind ==
                               TAVRN_MAINTENANCE_HIGH_TOKEN_COMPLETION_SERVICE_FAULT &&
                           fault_probe.completion.completed_at_ms ==
                               verification_now + 4u);
    CHECK("MAINT-07", tavrn_maintenance_high_token_release(
                           &fault_fixture.maintenance, &fault_fixture.link, 0u, 7u,
                           fault_token) == TAVRN_MAINTENANCE_HIGH_TOKEN_OK);

    test_external_high_token_fault_cancellation(BLE_MESH_SCHED_EVENT_RADIO_FAULT);
    test_external_high_token_fault_cancellation(BLE_MESH_SCHED_EVENT_SERVICE_FAULT);
}
#endif

static void complete_targeted_control(targeted_fixture_t *fixture,
                                      const tavrn_targeted_freshness_action_t *action,
                                      uint32_t now_ms, const char *requirement)
{
    ble_mesh_sched_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_TX_DONE;
    event.tx_token = action->token;
    event.tx_completed_channel_mask = 0x01u;
    CHECK(requirement, phase5_targeted_terminal(&fixture->maintenance, &fixture->router,
                                                 &fixture->link, &event, now_ms) ==
          TAVRN_TARGETED_FRESHNESS_OK);
}

static uint16_t absent_high_token(const tavrn_targeted_freshness_snapshot_t *state)
{
    uint16_t candidate;

    for (candidate = 0x8000u; candidate != 0u; candidate++) {
        uint8_t index;
        int used = 0;

        for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
            if (state->contexts[index].valid != 0u && state->contexts[index].token == candidate) {
                used = 1;
            }
        }
        if (used == 0) {
            return candidate;
        }
    }
    return 0u;
}

static uint32_t fnv_delay_for_request(void)
{
    const uint8_t bytes[] = { NETWORK_ID, 0x18u, 0x42u, 0xdeu, 0x52u, 0x4au,
                              0xddu, 0x02u, 0x00u, 0xdcu, 0xdcu };
    uint32_t hash = 2166136261u;
    uint8_t index;

    for (index = 0u; index < sizeof(bytes); index++) {
        hash ^= bytes[index];
        hash *= 16777619u;
    }
    return 10u + hash % 91u;
}

static void test_wire_and_malformed_policy(void)
{
    tavrn_codec_config_t a_config;
    tavrn_codec_config_t b_config;
    tavrn_decoded_frame_t decoded;
    uint8_t encoded[31];
    uint8_t malformed[sizeof(targeted_response)];
    size_t encoded_len = 0u;

    REACHED("wire-and-malformed");
    memset(&a_config, 0, sizeof(a_config));
    a_config.network_id = NETWORK_ID;
    a_config.local_peer = peer(adva_a, TAVRN_IDENTITY_SID8);
    a_config.identity_conflict = identity_allowed;
    b_config = a_config;
    b_config.local_peer = peer(adva_b, TAVRN_IDENTITY_SID8);
    memset(&decoded, 0, sizeof(decoded));
    CHECK("MAINT-06", sizeof(targeted_request) == 27u);
    CHECK("MAINT-06", sizeof(targeted_response) == 27u);
    CHECK("MAINT-06", targeted_request[12] == 0xb8u);
    CHECK("MAINT-06", targeted_response[12] == 0xa8u);
    CHECK("META-03", targeted_request[26] == 0x01u);
    CHECK("META-03", targeted_response[26] == 0xf0u);
    CHECK("MAINT-06", tavrn_wire_v2_decode(&b_config, adva_a, targeted_request,
                                             sizeof(targeted_request), &decoded) == TAVRN_CODEC_OK);
    CHECK("MAINT-06", decoded.detail.control.pdu_len == 20u);
    CHECK("MAINT-06", tavrn_wire_v2_decode(&a_config, adva_b, targeted_response,
                                              sizeof(targeted_response), &decoded) == TAVRN_CODEC_OK);
    CHECK("META-03", decoded.detail.control.pdu_len == 20u);
    CHECK("META-03", decoded.detail.control.pdu[17] == 0x01u);
    CHECK("META-03", decoded.detail.control.pdu[18] == adva_b[0]);
    CHECK("META-03", decoded.detail.control.pdu[19] == 0xf0u);
    CHECK("MAINT-06", tavrn_wire_v2_encode(&a_config, &decoded, encoded, sizeof(encoded),
                                             &encoded_len) == TAVRN_CODEC_OK);
    CHECK("MAINT-06", encoded_len == sizeof(targeted_response));
    CHECK("MAINT-06", memcmp(encoded, targeted_response, sizeof(targeted_response)) == 0);
    CHECK("SERIAL-01", tavrn_wire_v2_decode(&a_config, adva_a, ordinary_vector,
                                              sizeof(ordinary_vector), &decoded) == TAVRN_CODEC_OK);
    CHECK("SERIAL-01", decoded.detail.control.pdu[5] == 0x80u);
    memcpy(malformed, targeted_request, sizeof(malformed));
    malformed[26] = 0x00u;
    CHECK("META-04", tavrn_wire_v2_decode(&b_config, adva_a, malformed,
                                            sizeof(malformed), &decoded) != TAVRN_CODEC_OK);
    memcpy(malformed, targeted_response, sizeof(malformed));
    malformed[26] = 0xf1u;
    CHECK("META-04", tavrn_wire_v2_decode(&a_config, adva_b, malformed,
                                            sizeof(malformed), &decoded) != TAVRN_CODEC_OK);
    memcpy(malformed, targeted_response, sizeof(malformed));
    malformed[13] = 0u;
    CHECK("META-04", tavrn_wire_v2_decode(&a_config, adva_b, malformed,
                                              sizeof(malformed), &decoded) != TAVRN_CODEC_OK);
    memcpy(malformed, targeted_request, sizeof(malformed));
    malformed[25] = adva_d[0];
    CHECK("META-04", tavrn_wire_v2_decode(&b_config, adva_a, malformed,
                                             sizeof(malformed), &decoded) != TAVRN_CODEC_OK);
    CHECK("META-04", tavrn_wire_v2_decode(&b_config, adva_c, targeted_request,
                                             sizeof(targeted_request), &decoded) != TAVRN_CODEC_OK);
    CHECK("META-04", tavrn_wire_v2_decode(&a_config, adva_c, targeted_response,
                                             sizeof(targeted_response), &decoded) != TAVRN_CODEC_OK);
}

static void test_stage0_route_revalidation_and_timeout(void)
{
    targeted_fixture_t fixture;
    targeted_fixture_t route_loss;
    tavrn_targeted_freshness_action_t action;
    tavrn_targeted_freshness_action_t loss_action;
    tavrn_targeted_freshness_snapshot_t state;
    tavrn_targeted_freshness_snapshot_t loss_state;
    const tavrn_targeted_freshness_context_snapshot_t *context;
    ble_mesh_sched_event_t event;
    aodv_action_t aodv_action;
    tavrn_adva_t subject = adva(adva_b);
    uint32_t remote_deadline;

    REACHED("stage0-route-timeout");
    STRUCTURAL("stage0-fixture", fixture_init(&fixture, adva_a));
    if (structural_failures != 0u) {
        return;
    }
    STRUCTURAL("stage0-demand", demand_subject(&fixture, adva_b));
    STRUCTURAL("stage0-route", install_route(&fixture, adva_b, adva_c, HARD_EXPIRY_MS - 1u,
                                               1u));
    if (structural_failures != 0u) {
        return;
    }
    memset(&action, 0, sizeof(action));
    CHECK("MAINT-06", phase5_targeted_begin_stage0(&fixture.maintenance, &fixture.router,
                                                     &fixture.link, &fixture.gtt, &subject,
                                                     HARD_EXPIRY_MS, &action) ==
          TAVRN_TARGETED_FRESHNESS_OK);
#if defined(TAVRN_PHASE5_TARGETED_RED_MODE)
    CHECK("META-03", action.control.pdu[19] == 0x01u);
#endif
    CHECK("MAINT-06", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, action.context_index, "stage0");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-06", action.control.pdu_len == 20u);
    CHECK("MAINT-06", action.control.pdu[5] == 0xb8u);
    CHECK("MAINT-06", action.control.pdu[6] == 0x10u);
    CHECK("MAINT-06", action.control.pdu[7] == adva_c[0]);
    CHECK("MAINT-06", action.control.pdu[8] == adva_b[0]);
    CHECK("MAINT-06", action.control.pdu[19] == 0x01u);
    CHECK("MAINT-07", action.token >= 0x8000u);
    CHECK("MAINT-07", context->valid != 0u);
    {
        tavrn_validated_control_t malformed_outbound = action.control;

        malformed_outbound.pdu[19] = 0u;
        CHECK("META-04", tavrn_link_v2_send_tracked_control(
                              &fixture.link, &malformed_outbound,
                              &context->receiver, 0x9000u, HARD_EXPIRY_MS,
                              NULL) == TAVRN_LINK_SEND_INVALID);
    }
    CHECK("MAINT-07", aodv_core_poll_action(&fixture.aodv, &aodv_action) ==
          AODV_ACTION_POLL_EMPTY);
    CHECK("MAINT-07", context->obligation_started_ms == HARD_EXPIRY_MS);
    CHECK("MAINT-07", context->obligation_deadline_ms == HARD_EXPIRY_MS + HELLO_DEDUPE_MS);

    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_TX_DONE;
    event.tx_token = action.token;
    event.tx_completed_channel_mask = 0x01u;
    remote_deadline = 0xfffffff0u + tron_timer_config.aodv_net_traversal_ms;
    CHECK("MAINT-07", phase5_targeted_terminal(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &event, 0xfffffff0u) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, action.context_index, "timeout-start");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-07", context->stage == TAVRN_TARGETED_STAGE0_WAIT_RESPONSE);
    CHECK("MAINT-07", context->obligation_deadline_ms == remote_deadline);
    CHECK("MAINT-07", context->token == BLE_MESH_TX_TOKEN_NONE);
    CHECK("MAINT-07", phase5_targeted_owner_tick(&fixture.maintenance, &fixture.router,
                                                    &fixture.link, &fixture.gtt,
                                                    remote_deadline - 1u,
                                                    &action) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, action.context_index, "timeout-minus-one");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-07", context->stage == TAVRN_TARGETED_STAGE0_WAIT_RESPONSE);
    CHECK("MAINT-07", phase5_targeted_owner_tick(&fixture.maintenance, &fixture.router,
                                                    &fixture.link, &fixture.gtt,
                                                    remote_deadline,
                                                    &action) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, action.context_index, "timeout-equality");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-06", context->stage == TAVRN_TARGETED_STAGE1_READY);
    CHECK("MAINT-07", context->token == BLE_MESH_TX_TOKEN_NONE);

    STRUCTURAL("route-loss-fixture", fixture_init(&route_loss, adva_a));
    STRUCTURAL("route-loss-demand", demand_subject(&route_loss, adva_b));
    STRUCTURAL("route-loss-route", install_route(&route_loss, adva_b, adva_c,
                                                   HARD_EXPIRY_MS - 1u, 1u));
    if (structural_failures != 0u) {
        return;
    }
    subject = adva(adva_b);
    CHECK("MAINT-06", phase5_targeted_begin_stage0(&route_loss.maintenance,
                                                     &route_loss.router, &route_loss.link,
                                                     &route_loss.gtt, &subject, HARD_EXPIRY_MS,
                                                     &loss_action) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-06", aodv_core_tick(&route_loss.aodv, HARD_EXPIRY_MS + 1u) ==
          AODV_STATUS_OK);
    CHECK("MAINT-06", phase5_targeted_revalidate(&route_loss.maintenance,
                                                   &route_loss.router, &route_loss.link,
                                                   &route_loss.gtt, &subject,
                                                   HARD_EXPIRY_MS + 1u) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_snapshot(&route_loss.maintenance,
                                                 &route_loss.router, &route_loss.link,
                                                 &route_loss.gtt, &loss_state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&loss_state, loss_action.context_index, "route-loss");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-06", context->stage == TAVRN_TARGETED_STAGE1_READY);
    CHECK("MAINT-07", context->token == BLE_MESH_TX_TOKEN_NONE);
    subject = adva(adva_e);
    STRUCTURAL("stage1-only-demand", demand_subject(&route_loss, adva_e));
    if (structural_failures != 0u) {
        return;
    }
    CHECK("MAINT-06", phase5_targeted_begin_stage0(&route_loss.maintenance,
                                                     &route_loss.router, &route_loss.link,
                                                     &route_loss.gtt, &subject, HARD_EXPIRY_MS,
                                                     &loss_action) ==
          TAVRN_TARGETED_FRESHNESS_DEFERRED);
    CHECK("MAINT-07", phase5_targeted_snapshot(&route_loss.maintenance,
                                                 &route_loss.router, &route_loss.link,
                                                 &route_loss.gtt, &loss_state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&loss_state, loss_action.context_index, "stage1-only");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-06", context->stage == TAVRN_TARGETED_STAGE1_READY);
    CHECK("MAINT-07", context->token == BLE_MESH_TX_TOKEN_NONE);
}

static void test_relay_dedupe_and_metadata_policy(void)
{
    targeted_fixture_t fixture;
    targeted_fixture_t response_timeout;
    tavrn_targeted_freshness_rx_input_t input;
    tavrn_targeted_freshness_action_t action;
    tavrn_targeted_freshness_action_t relay_action;
    tavrn_targeted_freshness_action_t response_relay_action;
    tavrn_targeted_freshness_action_t busy_retry;
    tavrn_targeted_freshness_snapshot_t state;
    tavrn_validated_control_t request;
    tavrn_validated_control_t response;
    const tavrn_targeted_freshness_context_snapshot_t *context;
    uint8_t index;

    REACHED("relay-dedupe-metadata");
    STRUCTURAL("relay-fixture", fixture_init(&fixture, adva_c));
    if (structural_failures != 0u) {
        return;
    }
    STRUCTURAL("relay-gtt", observe_member(&fixture, adva_b, 1u, 1u));
    STRUCTURAL("relay-requester-route", install_route(&fixture, adva_a, adva_d, 100u,
                                                        10000u));
    STRUCTURAL("relay-final-target-route", install_route(&fixture, adva_d, adva_e, 100u,
                                                          10000u));
    if (structural_failures != 0u) {
        return;
    }
    request = control_from_adv(targeted_request, sizeof(targeted_request));
    request.pdu[6] = 0x30u;
    request.pdu[7] = adva_c[0];
    request.pdu[8] = adva_d[0];
    request.pdu[18] = adva_d[0];
    memset(&input, 0, sizeof(input));
    input.control_event = rx_event(&request, adva_a);
    request.pdu[6] = 0u;
    input.control_event = rx_event(&request, adva_a);
    CHECK("META-04", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                                &fixture.link, &fixture.gtt, &input, 100u,
                                                &action) == TAVRN_TARGETED_FRESHNESS_INVALID);
    CHECK("META-04", ble_mesh_tx_queue_count(&fixture.scheduler.routed_tx_queue) == 0u);
    request.pdu[6] = 0x30u;
    input.control_event = rx_event(&request, adva_a);
    CHECK("META-04", tavrn_maintenance_is_targeted_control(&input.control_event));
    CHECK("META-04", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt, &input, 100u,
                                               &action) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("META-04", action.control.pdu[5] == 0xb8u);
    CHECK("META-04", action.control.pdu[6] == 0x21u);
    CHECK("META-04", action.control.pdu[7] == adva_e[0]);
    CHECK("META-04", memcmp(&action.control.pdu[9], adva_a, TAVRN_ADVA_LEN) == 0);
    CHECK("META-04", action.control.pdu[15] == 0x02u);
    CHECK("META-04", action.control.pdu[16] == 0u);
    CHECK("META-04", action.control.pdu[17] == 0x01u);
    CHECK("META-04", action.control.pdu[18] == adva_d[0]);
    CHECK("META-04", action.control.pdu[19] == 0x01u);
    relay_action = action;
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.dedupe_live == 1u);
    context = context_at(&state, relay_action.context_index, "request-relay");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-07", context->work_kind == TAVRN_TARGETED_WORK_REQUEST_RELAY);
    CHECK("MAINT-07", context->obligation_started_ms == 100u);
    CHECK("MAINT-07", context->obligation_deadline_ms == 10100u);
    CHECK("MAINT-07", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                                &fixture.link, &fixture.gtt, &input, 101u,
                                                &action) == TAVRN_TARGETED_FRESHNESS_DUPLICATE);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.dedupe_live == 1u);
    complete_targeted_control(&fixture, &relay_action, 103u, "MAINT-07");

    response = control_from_adv(targeted_response, sizeof(targeted_response));
    response.pdu[6] = 0x30u;
    response.pdu[7] = adva_c[0];
    memcpy(&response.pdu[9], adva_b, TAVRN_ADVA_LEN);
    input.control_event = rx_event(&response, adva_b);
    CHECK("META-04", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt, &input, 102u,
                                               &action) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("META-04", action.control.pdu[5] == 0xa8u);
    CHECK("META-04", action.control.pdu[6] == 0x21u);
    CHECK("META-04", memcmp(&action.control.pdu[9], adva_b, TAVRN_ADVA_LEN) == 0);
    CHECK("META-04", action.control.pdu[15] == 0x03u);
    CHECK("META-04", action.control.pdu[16] == 0u);
    CHECK("META-04", action.control.pdu[17] == 0x01u);
    CHECK("META-04", action.control.pdu[18] == adva_b[0]);
    CHECK("META-04", action.control.pdu[19] == 0xf0u);
    response_relay_action = action;
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, response_relay_action.context_index, "response-relay");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-07", context->work_kind == TAVRN_TARGETED_WORK_RESPONSE_RELAY);
    CHECK("MAINT-07", context->obligation_started_ms == 102u);
    CHECK("MAINT-07", context->obligation_deadline_ms == 10102u);
    CHECK("META-04", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                                &fixture.link, &fixture.gtt, &input, 103u,
                                                &action) == TAVRN_TARGETED_FRESHNESS_DUPLICATE);
    complete_targeted_control(&fixture, &response_relay_action, 103u, "MAINT-07");

    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_DEDUPE_CAPACITY - 2u; index++) {
        request.pdu[15] = (uint8_t)(0x20u + index);
        request.pdu[16] = 0u;
        input.control_event = rx_event(&request, adva_a);
        CHECK("MAINT-07", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                                     &fixture.link, &fixture.gtt, &input,
                                                     103u, &action) ==
              TAVRN_TARGETED_FRESHNESS_OK);
        complete_targeted_control(&fixture, &action, 103u, "MAINT-07");
    }
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.dedupe_live == TAVRN_TARGETED_FRESHNESS_DEDUPE_CAPACITY);
    request.pdu[15] = 0x40u;
    input.control_event = rx_event(&request, adva_a);
    CHECK("MAINT-07", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &fixture.gtt, &input, 103u,
                                                 &action) == TAVRN_TARGETED_FRESHNESS_BUSY);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.dedupe_live == TAVRN_TARGETED_FRESHNESS_DEDUPE_CAPACITY);

    STRUCTURAL("response-timeout-fixture", fixture_init(&response_timeout, adva_c));
    STRUCTURAL("response-timeout-route", install_route(&response_timeout, adva_a, adva_d,
                                                          0xfffffff0u, 20000u));
    STRUCTURAL("response-timeout-queue", fill_control_queue(&response_timeout, 0xfffffff8u));
    if (structural_failures != 0u) {
        return;
    }
    input.control_event = rx_event(&response, adva_b);
    CHECK("MAINT-07", phase5_targeted_receive(&response_timeout.maintenance,
                                                &response_timeout.router, &response_timeout.link,
                                                &response_timeout.gtt, &input, 0xfffffff8u,
                                                &response_relay_action) ==
          TAVRN_TARGETED_FRESHNESS_BUSY);
    CHECK("MAINT-07", phase5_targeted_snapshot(&response_timeout.maintenance,
                                                  &response_timeout.router, &response_timeout.link,
                                                  &response_timeout.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, response_relay_action.context_index, "response-busy");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-07", context->obligation_started_ms == 0xfffffff8u);
    CHECK("MAINT-07", context->obligation_deadline_ms == 0x00002708u);
    CHECK("MAINT-07", phase5_targeted_owner_tick(&response_timeout.maintenance,
                                                    &response_timeout.router,
                                                    &response_timeout.link, &response_timeout.gtt,
                                                    0x00002707u, &busy_retry) ==
          TAVRN_TARGETED_FRESHNESS_BUSY);
    CHECK("MAINT-07", control_equal(&busy_retry.control, &response_relay_action.control));
    clear_queue(&response_timeout);
    CHECK("MAINT-07", phase5_targeted_owner_tick(&response_timeout.maintenance,
                                                    &response_timeout.router,
                                                    &response_timeout.link, &response_timeout.gtt,
                                                    0x00002708u, &busy_retry) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_snapshot(&response_timeout.maintenance,
                                                  &response_timeout.router, &response_timeout.link,
                                                  &response_timeout.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, response_relay_action.context_index, "response-deadline");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-07", context->valid == 0u);
}

static void test_target_and_intermediary_response_policy(void)
{
    targeted_fixture_t target;
    targeted_fixture_t intermediary;
    targeted_fixture_t bucket_zero_intermediary;
    targeted_fixture_t bucket_one_intermediary;
    tavrn_targeted_freshness_rx_input_t input;
    tavrn_targeted_freshness_action_t action;
    tavrn_targeted_freshness_action_t delayed_action;
    tavrn_targeted_freshness_action_t queued_intermediary_action;
    tavrn_targeted_freshness_snapshot_t state;
    tavrn_targeted_freshness_snapshot_t boundary_state;
    tavrn_validated_control_t request;
    tavrn_validated_control_t response;
    tavrn_gtt_expiry_snapshot_t expiry;
    tavrn_adva_t boundary_subject = adva(adva_b);
    const tavrn_targeted_freshness_context_snapshot_t *context;
    tavrn_targeted_freshness_status_t receive_status;
    uint32_t boundary_now;
    uint8_t active_before;

    REACHED("target-intermediary-suppression");
    STRUCTURAL("target-fixture", fixture_init(&target, adva_b));
    STRUCTURAL("target-route", install_route(&target, adva_a, adva_a, 100u, 10000u));
    if (structural_failures != 0u) {
        return;
    }
    request = control_from_adv(targeted_request, sizeof(targeted_request));
    memset(&input, 0, sizeof(input));
    input.control_event = rx_event(&request, adva_a);
    CHECK("META-01", phase5_targeted_receive(&target.maintenance, &target.router, &target.link,
                                               &target.gtt, &input, 300u, &action) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("META-01", action.control.pdu[5] == 0xa8u);
    CHECK("META-01", action.control.pdu[19] == 0xf0u);
    CHECK("META-01", action.enqueued != 0u);
    CHECK("MAINT-07", phase5_targeted_snapshot(&target.maintenance, &target.router,
                                                 &target.link, &target.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, action.context_index, "target");
    if (context == NULL) {
        return;
    }
    CHECK("META-01", context->work_kind == TAVRN_TARGETED_WORK_TARGET_RESPONSE);
    CHECK("META-01", context->response_bucket == 15u);
    CHECK("MAINT-07", context->obligation_started_ms == 300u);
    CHECK("MAINT-07", context->obligation_deadline_ms == 10300u);

    STRUCTURAL("intermediary-fixture", fixture_init(&intermediary, adva_c));
    STRUCTURAL("intermediary-gtt", observe_member(&intermediary, adva_b, 1u, 1u));
    STRUCTURAL("intermediary-route", install_route(&intermediary, adva_a, adva_d, 100u,
                                                    10000u));
    if (structural_failures != 0u) {
        return;
    }
    request.pdu[7] = adva_c[0];
    input.control_event = rx_event(&request, adva_a);
    CHECK("META-01", fnv_delay_for_request() == 81u);
    CHECK("META-01", phase5_targeted_receive(&intermediary.maintenance, &intermediary.router,
                                                &intermediary.link, &intermediary.gtt, &input,
                                                301u, &action) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("META-01", action.enqueued == 0u);
    CHECK("META-01", action.control.pdu[19] == 0xe0u);
    CHECK("MAINT-07", phase5_targeted_snapshot(&intermediary.maintenance, &intermediary.router,
                                                 &intermediary.link, &intermediary.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, action.context_index, "intermediary-delay");
    if (context == NULL) {
        return;
    }
    CHECK("META-01", context->work_kind == TAVRN_TARGETED_WORK_INTERMEDIARY_RESPONSE);
    CHECK("META-01", context->not_before_ms == 382u);
    CHECK("MAINT-07", context->obligation_started_ms == 301u);
    CHECK("MAINT-07", context->obligation_deadline_ms == 10301u);
    delayed_action = action;

    response = control_from_adv(targeted_response, sizeof(targeted_response));
    response.pdu[7] = adva_c[0];
    input.control_event = rx_event(&response, adva_b);
    CHECK("META-01", phase5_targeted_receive(&intermediary.maintenance, &intermediary.router,
                                               &intermediary.link, &intermediary.gtt, &input,
                                               302u, &action) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_snapshot(&intermediary.maintenance, &intermediary.router,
                                                 &intermediary.link, &intermediary.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, delayed_action.context_index, "delayed-suppressed");
    if (context == NULL) {
        return;
    }
    CHECK("META-01", context->valid == 0u);

    request.pdu[15] = 3u;
    input.control_event = rx_event(&request, adva_a);
    CHECK("META-01", phase5_targeted_receive(&intermediary.maintenance, &intermediary.router,
                                               &intermediary.link, &intermediary.gtt, &input,
                                               303u, &action) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("META-01", phase5_targeted_owner_tick(&intermediary.maintenance, &intermediary.router,
                                                   &intermediary.link, &intermediary.gtt, 384u,
                                                   &action) == TAVRN_TARGETED_FRESHNESS_OK);
    queued_intermediary_action = action;
    CHECK("MAINT-07", phase5_targeted_snapshot(&intermediary.maintenance, &intermediary.router,
                                                 &intermediary.link, &intermediary.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, queued_intermediary_action.context_index, "delayed-queued");
    if (context == NULL) {
        return;
    }
    CHECK("META-01", context->queued != 0u || context->in_flight != 0u);
    input.control_event = rx_event(&response, adva_b);
    CHECK("META-01", phase5_targeted_receive(&intermediary.maintenance, &intermediary.router,
                                                &intermediary.link, &intermediary.gtt, &input,
                                                385u, &action) == TAVRN_TARGETED_FRESHNESS_DUPLICATE);
    CHECK("MAINT-07", phase5_targeted_snapshot(&intermediary.maintenance, &intermediary.router,
                                                 &intermediary.link, &intermediary.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, queued_intermediary_action.context_index,
                         "queued-not-retracted");
    if (context == NULL) {
        return;
    }
    CHECK("META-01", context->valid != 0u);
    CHECK("META-01", context->queued != 0u || context->in_flight != 0u);
    request.pdu[15] = 4u;
    input.control_event = rx_event(&request, adva_a);
    CHECK("META-01", phase5_targeted_receive(&intermediary.maintenance, &intermediary.router,
                                                &intermediary.link, &intermediary.gtt, &input,
                                                303u, &action) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("META-01", action.enqueued == 0u);
    CHECK("MAINT-07", phase5_targeted_snapshot(&intermediary.maintenance, &intermediary.router,
                                                 &intermediary.link, &intermediary.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("META-01", state.active_contexts == 0u);
    CHECK("META-01", state.active_response_contexts == 3u);

    /* A targeted request is normatively bucket zero, so the strict
     * intermediary threshold reduces to response bucket > 0.  Exercise both
     * sides directly: active bucket zero is silent; bucket one responds. */
    STRUCTURAL("bucket-zero-fixture", fixture_init(&bucket_zero_intermediary, adva_c));
    STRUCTURAL("bucket-zero-gtt", observe_member(&bucket_zero_intermediary, adva_b, 1u, 1u));
    STRUCTURAL("bucket-zero-snapshot",
               tavrn_gtt_expiry_snapshot(&bucket_zero_intermediary.gtt,
                                         &boundary_subject, 2u, &expiry) ==
                   TAVRN_GTT_EXPIRY_QUERY_FOUND);
    if (structural_failures != 0u) {
        return;
    }
    boundary_now = expiry.hard_deadline_ms - 1000u;
    STRUCTURAL("bucket-zero-route",
               install_route(&bucket_zero_intermediary, adva_a, adva_d,
                             boundary_now - 2u, 10000u));
    if (structural_failures != 0u) {
        return;
    }
    request = control_from_adv(targeted_request, sizeof(targeted_request));
    request.pdu[7] = adva_c[0];
    memset(&input, 0, sizeof(input));
    input.control_event = rx_event(&request, adva_a);
    CHECK("META-01", phase5_targeted_snapshot(&bucket_zero_intermediary.maintenance,
                                                 &bucket_zero_intermediary.router,
                                                 &bucket_zero_intermediary.link,
                                                 &bucket_zero_intermediary.gtt,
                                                 &boundary_state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    active_before = boundary_state.active_contexts;
    memset(&action, 0, sizeof(action));
    receive_status = phase5_targeted_receive(&bucket_zero_intermediary.maintenance,
                                               &bucket_zero_intermediary.router,
                                               &bucket_zero_intermediary.link,
                                               &bucket_zero_intermediary.gtt,
                                               &input, boundary_now, &action);
    CHECK("META-01", receive_status == TAVRN_TARGETED_FRESHNESS_OK ||
                         receive_status == TAVRN_TARGETED_FRESHNESS_DROPPED);
    CHECK("META-01", action.control.pdu_len == 0u);
    CHECK("META-01", action.enqueued == 0u);
    CHECK("META-01", phase5_targeted_snapshot(&bucket_zero_intermediary.maintenance,
                                                 &bucket_zero_intermediary.router,
                                                 &bucket_zero_intermediary.link,
                                                 &bucket_zero_intermediary.gtt,
                                                 &boundary_state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("META-01", boundary_state.active_contexts == active_before);

    STRUCTURAL("bucket-one-fixture", fixture_init(&bucket_one_intermediary, adva_c));
    STRUCTURAL("bucket-one-gtt", observe_member(&bucket_one_intermediary, adva_b, 1u, 1u));
    STRUCTURAL("bucket-one-snapshot",
               tavrn_gtt_expiry_snapshot(&bucket_one_intermediary.gtt,
                                         &boundary_subject, 2u, &expiry) ==
                   TAVRN_GTT_EXPIRY_QUERY_FOUND);
    if (structural_failures != 0u) {
        return;
    }
    boundary_now = expiry.hard_deadline_ms - 25000u;
    STRUCTURAL("bucket-one-route",
               install_route(&bucket_one_intermediary, adva_a, adva_d,
                             boundary_now - 2u, 30000u));
    if (structural_failures != 0u) {
        return;
    }
    request = control_from_adv(targeted_request, sizeof(targeted_request));
    request.pdu[7] = adva_c[0];
    memset(&input, 0, sizeof(input));
    input.control_event = rx_event(&request, adva_a);
    memset(&action, 0, sizeof(action));
    CHECK("META-01", phase5_targeted_receive(&bucket_one_intermediary.maintenance,
                                                &bucket_one_intermediary.router,
                                                &bucket_one_intermediary.link,
                                                &bucket_one_intermediary.gtt,
                                                &input, boundary_now, &action) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("META-01", action.control.pdu_len == 20u);
    CHECK("META-01", action.control.pdu[19] == 0x10u);
    CHECK("META-01", action.enqueued == 0u);
    CHECK("META-01", phase5_targeted_snapshot(&bucket_one_intermediary.maintenance,
                                                 &bucket_one_intermediary.router,
                                                 &bucket_one_intermediary.link,
                                                 &bucket_one_intermediary.gtt,
                                                 &boundary_state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&boundary_state, action.context_index, "bucket-one");
    if (context == NULL) {
        return;
    }
    CHECK("META-01", context->work_kind == TAVRN_TARGETED_WORK_INTERMEDIARY_RESPONSE);
    CHECK("META-01", context->response_bucket == 1u);
}

static void test_tokens_terminal_tombstones_and_no_side_effects(void)
{
    targeted_fixture_t fixture;
    tavrn_targeted_freshness_action_t actions[4];
    tavrn_targeted_freshness_action_t deferred_action;
    tavrn_targeted_freshness_snapshot_t state;
    ble_mesh_sched_event_t event;
    tavrn_adva_t subjects[5] = { adva(adva_b), adva(adva_c), adva(adva_d), adva(adva_e), adva(adva_f) };
    uint16_t low_custody = 0u;
    uint16_t low_bootstrap = 0u;
    uint16_t wrong_token;
    uint16_t next_sequence_before_retirement;
    uint8_t index;
    const tavrn_targeted_freshness_context_snapshot_t *context;

    REACHED("tokens-terminal-tombstones");
    STRUCTURAL("token-fixture", fixture_init(&fixture, adva_a));
    if (structural_failures != 0u) {
        return;
    }
    fixture.link.next_scheduler_token = 0x7ffeu;
    CHECK("MAINT-07", phase5_targeted_reserve_low_token(&fixture.link,
                                                          TAVRN_TARGETED_LOW_CUSTODY,
                                                          &low_custody) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_reserve_low_token(&fixture.link,
                                                          TAVRN_TARGETED_LOW_BOOTSTRAP,
                                                          &low_bootstrap) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", low_custody == 0x7fffu);
    CHECK("MAINT-07", low_bootstrap == 0x0001u);
    CHECK("MAINT-07", low_custody != low_bootstrap);
    CHECK("MAINT-07", phase5_targeted_seed_high_token(&fixture.maintenance, 0xfffeu) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    for (index = 0u; index < 5u; index++) {
        STRUCTURAL("token-demand", demand_subject(&fixture, subjects[index].bytes));
        if (index < 4u) {
            STRUCTURAL("token-route", install_route(&fixture, subjects[index].bytes, adva_c,
                                                     HARD_EXPIRY_MS - 2u, 10000u));
        }
    }
    if (structural_failures != 0u) {
        return;
    }
    memset(actions, 0, sizeof(actions));
    memset(&deferred_action, 0, sizeof(deferred_action));
    for (index = 0u; index < 5u; index++) {
        if (index < 4u) {
            CHECK("MAINT-07", phase5_targeted_begin_stage0(&fixture.maintenance, &fixture.router,
                                                             &fixture.link, &fixture.gtt,
                                                             &subjects[index], HARD_EXPIRY_MS,
                                                             &actions[index]) ==
                  TAVRN_TARGETED_FRESHNESS_OK);
            CHECK("MAINT-07", actions[index].token >= 0x8000u);
            CHECK("MAINT-07", actions[index].token != low_custody);
            CHECK("MAINT-07", actions[index].token != low_bootstrap);
            if (index != 0u) {
                CHECK("MAINT-07", actions[index].token != actions[index - 1u].token);
            }
        } else {
            CHECK("MAINT-07", phase5_targeted_begin_stage0(&fixture.maintenance, &fixture.router,
                                                              &fixture.link, &fixture.gtt,
                                                              &subjects[index], HARD_EXPIRY_MS,
                                                              &deferred_action) ==
                   TAVRN_TARGETED_FRESHNESS_DEFERRED);
        }
    }
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.active_contexts == 4u);
    CHECK("MAINT-07", actions[0].token == 0xffffu);
    CHECK("MAINT-07", actions[1].token == 0x8000u);
    wrong_token = absent_high_token(&state);
    CHECK("MAINT-07", wrong_token >= 0x8000u);
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_TX_FAILED;
    event.tx_token = wrong_token;
    CHECK("MAINT-07", phase5_targeted_terminal(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &event, HARD_EXPIRY_MS) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, actions[0].context_index, "wrong-token");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-07", context->stage == TAVRN_TARGETED_STAGE0_READY);
    event.tx_token = actions[0].token;
    event.tx_completed_channel_mask = 0u;
    CHECK("MAINT-07", phase5_targeted_terminal(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &event, HARD_EXPIRY_MS + 1u) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, actions[0].context_index, "zero-failed");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-07", context->stage == TAVRN_TARGETED_STAGE0_READY);
    CHECK("MAINT-07", phase5_targeted_cancel_subject(&fixture.maintenance, &fixture.router,
                                                       &fixture.link, &fixture.gtt, &subjects[0],
                                                       HARD_EXPIRY_MS + 2u) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, actions[0].context_index, "cancel-tombstone");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-07", context->stage == TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE);
    CHECK("MAINT-07", context->token == actions[0].token);
    next_sequence_before_retirement = state.next_node_sequence;
    event.type = BLE_MESH_SCHED_EVENT_TX_DONE;
    event.tx_completed_channel_mask = 0x01u;
    CHECK("MAINT-07", phase5_targeted_terminal(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &event, HARD_EXPIRY_MS + 3u) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, actions[0].context_index, "late-terminal");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-07", context->valid == 0u);
    CHECK("MAINT-07", state.next_node_sequence == next_sequence_before_retirement);
    CHECK("MAINT-07", phase5_targeted_cancel_subject(&fixture.maintenance, &fixture.router,
                                                       &fixture.link, &fixture.gtt, &subjects[1],
                                                       HARD_EXPIRY_MS + 4u) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    event.type = BLE_MESH_SCHED_EVENT_TX_FAILED;
    event.tx_token = actions[1].token;
    event.tx_completed_channel_mask = 0u;
    CHECK("MAINT-07", phase5_targeted_terminal(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &event, HARD_EXPIRY_MS + 5u) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_cancel_subject(&fixture.maintenance, &fixture.router,
                                                       &fixture.link, &fixture.gtt, &subjects[2],
                                                       HARD_EXPIRY_MS + 6u) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_evicted(&fixture.maintenance, &fixture.router,
                                                &fixture.link, actions[2].token,
                                                HARD_EXPIRY_MS + 7u) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    context = context_at(&state, actions[1].context_index, "late-failed");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-07", context->valid == 0u);
    context = context_at(&state, actions[2].context_index, "late-evicted");
    if (context == NULL) {
        return;
    }
    CHECK("MAINT-07", context->valid == 0u);
}

static void test_local_terminal_response_and_sequence_frontier(void)
{
    targeted_fixture_t fixture;
    tavrn_targeted_freshness_action_t request_action;
    tavrn_targeted_freshness_action_t terminal_action;
    tavrn_targeted_freshness_snapshot_t targeted_state;
    tavrn_maintenance_snapshot_t maintenance_state;
    tavrn_gtt_expiry_snapshot_t origin_state;
    tavrn_validated_control_t response;
    tavrn_targeted_freshness_rx_input_t input;
    tavrn_adva_t subject = adva(adva_b);
    uint16_t ordinary_sequences[3];
    uint8_t index;

    REACHED("local-terminal-sequence-frontier");
    STRUCTURAL("terminal-fixture", fixture_init(&fixture, adva_a));
    STRUCTURAL("terminal-demand", demand_subject(&fixture, adva_b));
    STRUCTURAL("terminal-route", install_route(&fixture, adva_b, adva_c,
                                                 HARD_EXPIRY_MS - 1u, 10000u));
    if (structural_failures != 0u) {
        return;
    }
    CHECK("SERIAL-01", phase5_targeted_begin_stage0(
                               &fixture.maintenance, &fixture.router, &fixture.link,
                               &fixture.gtt, &subject, HARD_EXPIRY_MS,
                               &request_action) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("SERIAL-01", pdu_u16(&request_action.control, 15u) == 0xfffeu);
    for (index = 0u; index < 3u; index++) {
        uint32_t now_ms = fixture.maintenance.snapshot.next_hello_due_ms;

        CHECK("SERIAL-01", tavrn_maintenance_tick(&fixture.maintenance, now_ms) ==
              TAVRN_MAINTENANCE_OK);
        ordinary_sequences[index] = pdu_u16(&fixture.maintenance.queued_hello, 15u);
    }
    CHECK("SERIAL-01", ordinary_sequences[0] == 0xffffu);
    CHECK("SERIAL-01", ordinary_sequences[1] == 0u);
    CHECK("SERIAL-01", ordinary_sequences[2] == 1u);
    CHECK("SERIAL-01", ordinary_sequences[0] != pdu_u16(&request_action.control, 15u));
    CHECK("SERIAL-01", ordinary_sequences[1] != ordinary_sequences[0]);
    CHECK("SERIAL-01", ordinary_sequences[2] != ordinary_sequences[1]);
    CHECK("SERIAL-01", tavrn_maintenance_snapshot(&fixture.maintenance,
                                                      &maintenance_state) ==
          TAVRN_MAINTENANCE_OK);
    CHECK("SERIAL-01", maintenance_state.next_node_sequence == 2u);

    complete_targeted_control(&fixture, &request_action, HARD_EXPIRY_MS + 1u,
                              "MAINT-07");
    response = control_from_adv(targeted_response, sizeof(targeted_response));
    response.pdu[7] = adva_a[0];
    response.pdu[8] = adva_a[0];
    memcpy(&response.pdu[9], adva_b, TAVRN_ADVA_LEN);
    memset(&input, 0, sizeof(input));
    input.control_event = rx_event(&response, adva_b);
    CHECK("META-01", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                                &fixture.link, &fixture.gtt, &input,
                                                HARD_EXPIRY_MS + 2u,
                                                &terminal_action) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("META-01", terminal_action.enqueued == 0u);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt,
                                                  &targeted_state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("META-01", targeted_state.contexts[request_action.context_index].valid == 0u);
    CHECK("META-01", tavrn_gtt_expiry_snapshot(&fixture.gtt, &subject,
                                                 HARD_EXPIRY_MS + 2u,
                                                 &origin_state) ==
          TAVRN_GTT_EXPIRY_QUERY_FOUND);
    CHECK("META-01", origin_state.serial == pdu_u16(&response, 15u));
    CHECK("META-01", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                                 &fixture.link, &fixture.gtt, &input,
                                                 HARD_EXPIRY_MS + 3u,
                                                 &terminal_action) ==
          TAVRN_TARGETED_FRESHNESS_DUPLICATE);
}

static void test_intermediary_terminal_response_evidence(void)
{
    targeted_fixture_t fixture;
    tavrn_targeted_freshness_action_t b_request;
    tavrn_targeted_freshness_action_t c_request;
    tavrn_targeted_freshness_action_t terminal_action;
    tavrn_targeted_freshness_snapshot_t state;
    tavrn_gtt_expiry_snapshot_t b_state;
    tavrn_gtt_expiry_snapshot_t c_state;
    tavrn_maintenance_expiry_sweep_snapshot_t sweep;
    tavrn_gtt_evidence_t duplicate_evidence;
    tavrn_validated_control_t response;
    tavrn_targeted_freshness_rx_input_t input;
    tavrn_adva_t subject_b = adva(adva_b);
    tavrn_adva_t subject_c = adva(adva_c);
    uint32_t started_at = HARD_EXPIRY_MS;

    REACHED("intermediary-terminal-evidence");
#if defined(TAVRN_PHASE5_TARGETED_RED_MODE)
    return;
#endif
    STRUCTURAL("intermediary-terminal-fixture", fixture_init(&fixture, adva_a));
    STRUCTURAL("intermediary-terminal-demand-b", demand_subject(&fixture, adva_b));
    STRUCTURAL("intermediary-terminal-demand-c", demand_subject(&fixture, adva_c));
    STRUCTURAL("intermediary-terminal-route-b", install_route(&fixture, adva_b, adva_d,
                                                                 started_at - 1u, 10000u));
    STRUCTURAL("intermediary-terminal-route-c", install_route(&fixture, adva_c, adva_e,
                                                                 started_at - 1u, 10000u));
    STRUCTURAL("intermediary-terminal-sweep-arm",
               tavrn_maintenance_sweep_expiry(&fixture.maintenance, 0u, &sweep) ==
                   TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK);
    if (structural_failures != 0u) {
        return;
    }
    CHECK("MAINT-06", phase5_targeted_begin_stage0(
                          &fixture.maintenance, &fixture.router, &fixture.link,
                          &fixture.gtt, &subject_b, started_at,
                          &b_request) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-06", phase5_targeted_begin_stage0(
                          &fixture.maintenance, &fixture.router, &fixture.link,
                          &fixture.gtt, &subject_c, started_at,
                          &c_request) == TAVRN_TARGETED_FRESHNESS_OK);

    response = control_from_adv(targeted_response, sizeof(targeted_response));
    response.pdu[7] = adva_a[0];
    response.pdu[8] = adva_a[0];
    response.pdu[18] = adva_b[0];
    response.pdu[15] = 0u;
    response.pdu[16] = 0u;
    memcpy(&response.pdu[9], adva_b, TAVRN_ADVA_LEN);
    memset(&input, 0, sizeof(input));
    input.control_event = rx_event(&response, adva_b);
    /* A target-self stale serial is not accepted evidence and therefore must
     * not complete B's verification. */
    CHECK("META-01", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                                &fixture.link, &fixture.gtt, &input,
                                                started_at + 1u,
                                                &terminal_action) ==
          TAVRN_TARGETED_FRESHNESS_DROPPED);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.contexts[b_request.context_index].valid != 0u);
    CHECK("MAINT-07", state.contexts[c_request.context_index].valid != 0u);
    CHECK("MAINT-07", tavrn_maintenance_sweep_expiry(&fixture.maintenance,
                                                        started_at + 1u, &sweep) ==
          TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK);
    CHECK("MAINT-07", sweep.received_evidence_count == 1u);

    /* Seed same-tick imported liveness so C's otherwise valid intermediary
     * response is rejected as a duplicate.  It still cannot cancel B. */
    memset(&duplicate_evidence, 0, sizeof(duplicate_evidence));
    duplicate_evidence.identity = subject_b;
    duplicate_evidence.hop_count = 0u;
    duplicate_evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    CHECK("META-01", tavrn_gtt_observe_with_provenance(
                          &fixture.gtt, &duplicate_evidence,
                          TAVRN_GTT_PROVENANCE_IMPORTED_METADATA,
                          started_at + 2u, NULL) == TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED);
    response.pdu[15] = 0x44u;
    response.pdu[16] = 0u;
    memcpy(&response.pdu[9], adva_c, TAVRN_ADVA_LEN);
    input.control_event = rx_event(&response, adva_c);
    CHECK("META-01", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                                &fixture.link, &fixture.gtt, &input,
                                                started_at + 2u,
                                                &terminal_action) ==
          TAVRN_TARGETED_FRESHNESS_DROPPED);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.contexts[b_request.context_index].valid != 0u);
    CHECK("MAINT-07", state.contexts[c_request.context_index].valid != 0u);

    response.pdu[19] = 0xf1u;
    input.control_event = rx_event(&response, adva_c);
    CHECK("META-04", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                                &fixture.link, &fixture.gtt, &input,
                                                started_at + 3u,
                                                &terminal_action) ==
          TAVRN_TARGETED_FRESHNESS_INVALID);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.contexts[b_request.context_index].valid != 0u);
    CHECK("MAINT-07", state.contexts[c_request.context_index].valid != 0u);

    /* C is the full origin/evidence source, but B is the metadata subject.
     * The accepted serial-absent imported observation refreshes B, preserves
     * B's serial, and completes only B's matching local request. */
    response.pdu[19] = 0xf0u;
    input.control_event = rx_event(&response, adva_c);
    CHECK("META-01", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                                &fixture.link, &fixture.gtt, &input,
                                                started_at + 4u,
                                                &terminal_action) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("META-01", terminal_action.enqueued == 0u);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.contexts[b_request.context_index].valid == 0u);
    CHECK("MAINT-07", state.contexts[c_request.context_index].valid != 0u);
    CHECK("META-01", tavrn_gtt_expiry_snapshot(&fixture.gtt, &subject_b,
                                                  started_at + 4u, &b_state) ==
          TAVRN_GTT_EXPIRY_QUERY_FOUND);
    CHECK("META-01", tavrn_gtt_expiry_snapshot(&fixture.gtt, &subject_c,
                                                  started_at + 4u, &c_state) ==
          TAVRN_GTT_EXPIRY_QUERY_FOUND);
    CHECK("META-01", b_state.last_evidence_ms == started_at + 4u);
    CHECK("META-01", b_state.serial_present != 0u && b_state.serial == 1u);
    CHECK("META-01", c_state.last_evidence_ms == 1u && c_state.serial == 1u);
    CHECK("MAINT-07", tavrn_maintenance_sweep_expiry(&fixture.maintenance,
                                                        started_at + 25001u, &sweep) ==
          TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK);
    CHECK("MAINT-07", sweep.received_evidence_count == 2u);
}

static void test_response_obligation_capacity(void)
{
    targeted_fixture_t fixture;
    tavrn_targeted_freshness_action_t action;
    tavrn_targeted_freshness_snapshot_t state;
    tavrn_targeted_freshness_rx_input_t input;
    tavrn_validated_control_t request;
    tavrn_adva_t subjects[4] = { adva(adva_b), adva(adva_c), adva(adva_d), adva(adva_e) };
    uint8_t index;

    REACHED("response-obligation-capacity");
#if defined(TAVRN_PHASE5_TARGETED_RED_MODE)
    return;
#endif
    STRUCTURAL("capacity-fixture", fixture_init(&fixture, adva_a));
    for (index = 0u; index < 4u; index++) {
        STRUCTURAL("capacity-demand", demand_subject(&fixture, subjects[index].bytes));
    }
    if (structural_failures != 0u) {
        return;
    }
    for (index = 0u; index < 4u; index++) {
        CHECK("MAINT-06", phase5_targeted_begin_stage0(
                               &fixture.maintenance, &fixture.router, &fixture.link,
                               &fixture.gtt, &subjects[index], HARD_EXPIRY_MS,
                               &action) == TAVRN_TARGETED_FRESHNESS_DEFERRED);
    }
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.active_contexts == 4u);
    STRUCTURAL("capacity-requester-route", install_route(&fixture, adva_b, adva_c,
                                                           HARD_EXPIRY_MS - 1u, 10000u));
    if (structural_failures != 0u) {
        return;
    }

    request = control_from_adv(targeted_request, sizeof(targeted_request));
    request.pdu[7] = adva_a[0];
    request.pdu[8] = adva_a[0];
    request.pdu[18] = adva_a[0];
    memcpy(&request.pdu[9], adva_b, TAVRN_ADVA_LEN);
    memset(&input, 0, sizeof(input));
    input.control_event = rx_event(&request, adva_b);
    CHECK("META-01", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                                &fixture.link, &fixture.gtt, &input,
                                                HARD_EXPIRY_MS + 1u, &action) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", action.context_index >=
          TAVRN_TARGETED_RESPONSE_CONTEXT_INDEX_BASE);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.active_contexts == 4u);
    CHECK("MAINT-07", state.active_response_contexts == 1u);
    CHECK("META-01", state.response_contexts[
              action.context_index - TAVRN_TARGETED_RESPONSE_CONTEXT_INDEX_BASE].work_kind ==
          TAVRN_TARGETED_WORK_TARGET_RESPONSE);
}

static void test_response_pool_does_not_starve_local_request(void)
{
    targeted_fixture_t fixture;
    tavrn_targeted_freshness_action_t action;
    tavrn_targeted_freshness_action_t local_action;
    tavrn_targeted_freshness_snapshot_t state;
    tavrn_targeted_freshness_rx_input_t input;
    tavrn_validated_control_t request;
    tavrn_adva_t requesters[4] = { adva(adva_b), adva(adva_c), adva(adva_d),
                                   adva(adva_e) };
    tavrn_adva_t local_subject = adva(adva_b);
    uint8_t index;

    REACHED("response-pool-local-start");
#if defined(TAVRN_PHASE5_TARGETED_RED_MODE)
    return;
#endif
    STRUCTURAL("response-pool-fixture", fixture_init(&fixture, adva_a));
    STRUCTURAL("response-pool-demand", demand_subject(&fixture, adva_b));
    for (index = 0u; index < 4u; index++) {
        STRUCTURAL("response-pool-route", install_route(&fixture,
                                                           requesters[index].bytes,
                                                           requesters[index].bytes,
                                                           HARD_EXPIRY_MS - 1u,
                                                           10000u));
    }
    if (structural_failures != 0u) {
        return;
    }
    request = control_from_adv(targeted_request, sizeof(targeted_request));
    request.pdu[7] = adva_a[0];
    request.pdu[8] = adva_a[0];
    request.pdu[18] = adva_a[0];
    memset(&input, 0, sizeof(input));
    for (index = 0u; index < 4u; index++) {
        request.pdu[15] = (uint8_t)(0x20u + index);
        request.pdu[16] = 0u;
        memcpy(&request.pdu[9], requesters[index].bytes, TAVRN_ADVA_LEN);
        input.control_event = rx_event(&request, requesters[index].bytes);
        CHECK("META-01", phase5_targeted_receive(&fixture.maintenance, &fixture.router,
                                                    &fixture.link, &fixture.gtt, &input,
                                                    HARD_EXPIRY_MS + index,
                                                    &action) ==
              TAVRN_TARGETED_FRESHNESS_OK);
        CHECK("MAINT-07", action.context_index >=
              TAVRN_TARGETED_RESPONSE_CONTEXT_INDEX_BASE);
    }
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.active_contexts == 0u);
    CHECK("MAINT-07", state.active_response_contexts ==
          TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY);
    CHECK("MAINT-06", phase5_targeted_begin_stage0(
                          &fixture.maintenance, &fixture.router, &fixture.link,
                          &fixture.gtt, &local_subject, HARD_EXPIRY_MS + 4u,
                          &local_action) == TAVRN_TARGETED_FRESHNESS_BUSY);
    CHECK("MAINT-07", local_action.context_index <
          TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.active_contexts == 1u);
    CHECK("MAINT-07", state.active_response_contexts ==
          TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY);
    CHECK("MAINT-07", state.contexts[local_action.context_index].work_kind ==
          TAVRN_TARGETED_WORK_LOCAL_REQUEST);
}

static void test_queued_deadline_cancellation(void)
{
    targeted_fixture_t fixture;
    tavrn_targeted_freshness_action_t action;
    tavrn_targeted_freshness_snapshot_t state;
    tavrn_adva_t subject = adva(adva_b);

    REACHED("queued-deadline-cancellation");
    STRUCTURAL("deadline-fixture", fixture_init(&fixture, adva_a));
    STRUCTURAL("deadline-demand", demand_subject(&fixture, adva_b));
    STRUCTURAL("deadline-route", install_route(&fixture, adva_b, adva_c,
                                                 HARD_EXPIRY_MS - 1u, 10000u));
    if (structural_failures != 0u) {
        return;
    }
    CHECK("MAINT-07", phase5_targeted_begin_stage0(&fixture.maintenance, &fixture.router,
                                                       &fixture.link, &fixture.gtt, &subject,
                                                       HARD_EXPIRY_MS, &action) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", ble_mesh_tx_queue_count(&fixture.scheduler.routed_tx_queue) == 1u);
    /* Passive GTT evidence bypasses the direct-HELLO cancellation seam.  The
     * runtime owner tick must therefore revalidate local hard-expiry demand
     * before allowing this already queued stage-0 request to remain live. */
    CHECK("MAINT-07", observe_member(&fixture, adva_b, 2u,
                                      HARD_EXPIRY_MS + 1u));
    CHECK("MAINT-07", phase5_targeted_owner_tick(&fixture.maintenance, &fixture.router,
                                                     &fixture.link, &fixture.gtt,
                                                     HARD_EXPIRY_MS + 1u,
                                                     &action) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", ble_mesh_tx_queue_count(&fixture.scheduler.routed_tx_queue) == 0u);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.contexts[action.context_index].valid != 0u);
    CHECK("MAINT-07", state.contexts[action.context_index].stage ==
          TAVRN_TARGETED_STAGE1_READY);
    CHECK("MAINT-07", state.contexts[action.context_index].queued == 0u);
    CHECK("MAINT-07", state.contexts[action.context_index].token ==
          BLE_MESH_TX_TOKEN_NONE);
}

static void test_liveness_cancels_same_subject(void)
{
    targeted_fixture_t fixture;
    tavrn_targeted_freshness_action_t action;
    tavrn_targeted_freshness_snapshot_t state;
    tavrn_validated_control_t hello;
    tavrn_rx_control_event_t event;
    tavrn_adva_t subject = adva(adva_b);

    REACHED("liveness-cancels-subject");
    STRUCTURAL("liveness-fixture", fixture_init(&fixture, adva_a));
    STRUCTURAL("liveness-demand", demand_subject(&fixture, adva_b));
    STRUCTURAL("liveness-route", install_route(&fixture, adva_b, adva_c,
                                                 HARD_EXPIRY_MS - 1u, 10000u));
    if (structural_failures != 0u) {
        return;
    }
    CHECK("MAINT-07", phase5_targeted_begin_stage0(&fixture.maintenance, &fixture.router,
                                                      &fixture.link, &fixture.gtt, &subject,
                                                      HARD_EXPIRY_MS, &action) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    hello = ordinary_hello(adva_b, 2u);
    event = rx_event(&hello, adva_b);
    CHECK("MAINT-07", tavrn_maintenance_handle_rx_control(&fixture.maintenance, &event,
                                                              HARD_EXPIRY_MS + 1u) ==
          TAVRN_MAINTENANCE_RX_UNIQUE);
    CHECK("MAINT-07", ble_mesh_tx_queue_count(&fixture.scheduler.routed_tx_queue) == 0u);
    CHECK("MAINT-07", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("MAINT-07", state.contexts[action.context_index].valid == 0u);
}

static void test_serial_shared_ordinary_admission_and_retry(void)
{
    targeted_fixture_t fixture;
    tavrn_targeted_freshness_action_t actions[4];
    tavrn_targeted_freshness_action_t retry;
    tavrn_targeted_freshness_action_t busy_retry;
    tavrn_targeted_freshness_snapshot_t state;
    tavrn_maintenance_snapshot_t maintenance_state;
    tavrn_adva_t subjects[5] = { adva(adva_b), adva(adva_c), adva(adva_d), adva(adva_e), adva(adva_f) };
    uint8_t index;

    REACHED("serial-ordinary-admission-retry");
    STRUCTURAL("serial-fixture", fixture_init(&fixture, adva_a));
    if (structural_failures != 0u) {
        return;
    }
    STRUCTURAL("serial-pending", fill_control_queue(&fixture, 29u));
    STRUCTURAL("serial-busy", tavrn_maintenance_tick(&fixture.maintenance, 30u) ==
               TAVRN_MAINTENANCE_BUSY);
    STRUCTURAL("serial-snapshot", tavrn_maintenance_snapshot(&fixture.maintenance,
                                                               &maintenance_state) ==
               TAVRN_MAINTENANCE_OK);
    CHECK("SERIAL-01", maintenance_state.pending != 0u);
    CHECK("SERIAL-01", maintenance_state.next_node_sequence == 0xfffeu);
    for (index = 0u; index < 5u; index++) {
        STRUCTURAL("serial-demand", demand_subject(&fixture, subjects[index].bytes));
        STRUCTURAL("serial-route", install_route(&fixture, subjects[index].bytes, adva_b,
                                                   HARD_EXPIRY_MS - 2u, 10000u));
    }
    if (structural_failures != 0u) {
        return;
    }
    for (index = 0u; index < 4u; index++) {
        CHECK("SERIAL-01", phase5_targeted_begin_stage0(&fixture.maintenance, &fixture.router,
                                                           &fixture.link, &fixture.gtt, &subjects[index],
                                                           HARD_EXPIRY_MS, &actions[index]) ==
              TAVRN_TARGETED_FRESHNESS_BUSY);
        CHECK("SERIAL-01", pdu_u16(&actions[index].control, 15u) ==
              (uint16_t)(0xffffu + index));
    }
    CHECK("SERIAL-01", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                  &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("SERIAL-01", state.ordinary_pending != 0u);
    CHECK("SERIAL-01", state.next_node_sequence == 0xfffeu);
    CHECK("SERIAL-01", state.reservation_frontier == 3u);
    CHECK("SERIAL-01", phase5_targeted_owner_tick(&fixture.maintenance, &fixture.router,
                                                    &fixture.link, &fixture.gtt, HARD_EXPIRY_MS + 1u,
                                                    &busy_retry) == TAVRN_TARGETED_FRESHNESS_BUSY);
    CHECK("SERIAL-01", control_equal(&busy_retry.control, &actions[0].control));
    CHECK("MAINT-07", phase5_targeted_cancel_subject(&fixture.maintenance, &fixture.router,
                                                       &fixture.link, &fixture.gtt, &subjects[1],
                                                       HARD_EXPIRY_MS + 1u) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    clear_queue(&fixture);
    CHECK("SERIAL-01", phase5_targeted_owner_tick(&fixture.maintenance, &fixture.router,
                                                    &fixture.link, &fixture.gtt, HARD_EXPIRY_MS + 2u,
                                                    &retry) == TAVRN_TARGETED_FRESHNESS_OK);
    CHECK("SERIAL-01", control_equal(&retry.control, &actions[0].control));
    {
        ble_mesh_sched_event_t zero_failed;

        memset(&zero_failed, 0, sizeof(zero_failed));
        zero_failed.type = BLE_MESH_SCHED_EVENT_TX_FAILED;
        zero_failed.tx_token = actions[0].token;
        CHECK("SERIAL-01", phase5_targeted_terminal(&fixture.maintenance, &fixture.router,
                                                      &fixture.link, &zero_failed,
                                                      HARD_EXPIRY_MS + 3u) ==
              TAVRN_TARGETED_FRESHNESS_OK);
        CHECK("SERIAL-01", phase5_targeted_owner_tick(&fixture.maintenance, &fixture.router,
                                                        &fixture.link, &fixture.gtt,
                                                        HARD_EXPIRY_MS + 4u, &retry) ==
              TAVRN_TARGETED_FRESHNESS_OK);
        CHECK("SERIAL-01", control_equal(&retry.control, &actions[0].control));
    }
    CHECK("SERIAL-01", tavrn_maintenance_tick(&fixture.maintenance, 31u) ==
          TAVRN_MAINTENANCE_OK);
    CHECK("SERIAL-01", tavrn_maintenance_snapshot(&fixture.maintenance,
                                                     &maintenance_state) == TAVRN_MAINTENANCE_OK);
    CHECK("SERIAL-01", maintenance_state.pending == 0u);
    CHECK("SERIAL-01", maintenance_state.next_node_sequence == 3u);
    CHECK("SERIAL-01", phase5_targeted_begin_stage0(&fixture.maintenance, &fixture.router,
                                                        &fixture.link, &fixture.gtt, &subjects[4],
                                                        HARD_EXPIRY_MS + 2u, &retry) ==
          TAVRN_TARGETED_FRESHNESS_OK);
    /* The canceled `0000` reservation remains a gap; it does not advance the
     * merged frontier past the next free value `0003`. */
    CHECK("SERIAL-01", pdu_u16(&retry.control, 15u) == 3u);
    CHECK("SERIAL-01", phase5_targeted_snapshot(&fixture.maintenance, &fixture.router,
                                                   &fixture.link, &fixture.gtt, &state) ==
          TAVRN_TARGETED_FRESHNESS_OK);
}

int main(void)
{
    test_wire_and_malformed_policy();
    test_stage0_route_revalidation_and_timeout();
    test_relay_dedupe_and_metadata_policy();
    test_target_and_intermediary_response_policy();
    test_tokens_terminal_tombstones_and_no_side_effects();
    test_local_terminal_response_and_sequence_frontier();
    test_intermediary_terminal_response_evidence();
    test_response_obligation_capacity();
    test_response_pool_does_not_starve_local_request();
    test_queued_deadline_cancellation();
    test_liveness_cancels_same_subject();
    test_serial_shared_ordinary_admission_and_retry();
#if !defined(TAVRN_PHASE5_TARGETED_RED_MODE)
    test_external_high_token_authority();
#endif
    if (structural_failures != 0u) {
        printf("tavrn_phase5_targeted_freshness structural failures: %u\n",
               structural_failures);
        return 3;
    }
    if (failures != 0u) {
        printf("tavrn_phase5_targeted_freshness targeted policy failures: %u\n", failures);
        return 1;
    }
    printf("tavrn_phase5_targeted_freshness tests passed\n");
    return 0;
}
