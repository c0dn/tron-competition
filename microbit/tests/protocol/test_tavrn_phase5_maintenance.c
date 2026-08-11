#include "aodv_core.h"
#include "ble_mesh_scheduler.h"
#include "tavrn_full.h"
#include "tavrn_gtt.h"
#include "tavrn_link_v2.h"
#include "tavrn_phase5_maintenance_contract.h"
#include "tavrn_mentorship.h"
#include "tavrn_router.h"
#include "tavrn_wire_v2.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_NETWORK_ID 0x2au
#define TEST_HELLO_CHANGE_MS 20u
#define TEST_HELLO_DEDUPE_MS 10u
#define TEST_SELF_BOOTSTRAP_MS 20u

static unsigned int failures;
static const char *reported[4];
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
#if defined(TAVRN_MAINTENANCE_ADAPTIVE_CONTRACT)
static const uint8_t adva_d[TAVRN_ADVA_LEN] = {
    0x34u, 0x56u, 0x78u, 0x9au, 0xbcu, 0xc2u,
};
#endif

typedef struct maintenance_fixture {
    ble_mesh_scheduler_t scheduler;
    tavrn_link_v2_t link;
    aodv_core_t aodv;
    tavrn_gtt_t gtt;
    tavrn_gtt_storage_t gtt_storage;
    tavrn_full_t full;
    tavrn_router_t router;
    tavrn_mentorship_t mentorship;
    tavrn_maintenance_t maintenance;
} maintenance_fixture_t;

typedef struct protocol_snapshot {
    tavrn_gtt_storage_t gtt_storage;
    tavrn_gtt_counters_t gtt_counters;
    aodv_counters_t aodv_counters;
    tavrn_maintenance_snapshot_t maintenance;
    tavrn_maintenance_counters_t maintenance_counters;
    uint8_t queued_controls;
} protocol_snapshot_t;

static tavrn_adva_t make_adva(const uint8_t bytes[TAVRN_ADVA_LEN])
{
    tavrn_adva_t result;

    memcpy(result.bytes, bytes, TAVRN_ADVA_LEN);
    return result;
}

static tavrn_direct_peer_t make_peer(const uint8_t bytes[TAVRN_ADVA_LEN],
                                     tavrn_identity_width_t width)
{
    tavrn_direct_peer_t result;

    memset(&result, 0, sizeof(result));
    result.logical_id.width = width;
    result.logical_id.value = width == TAVRN_IDENTITY_SID8 ? bytes[0] :
        (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    result.adva = make_adva(bytes);
    return result;
}

static tavrn_link_config_t make_link_config(void)
{
    tavrn_link_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_peer(adva_a, TAVRN_IDENTITY_SID16);
    config.network_id = TEST_NETWORK_ID;
    config.hack_max_attempts = 3u;
    config.hack_response_ms = 250u;
    config.hack_turnaround_ms = 8u;
    config.retry_backoff_ms = 0u;
    config.busy_backoff_ms = 500u;
    config.busy_max_responses = 3u;
    config.data_forward_deadline_ms = 5000u;
    config.candidate_resolve_ms = 10u;
    config.data_dedupe_ms = 10000u;
    config.flood_dedupe_ms = 10000u;
    config.flood_jitter_min_ms = 20u;
    config.flood_jitter_max_ms = 120u;
    return config;
}

static aodv_core_config_t make_aodv_config(void)
{
    aodv_core_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_peer(adva_a, TAVRN_IDENTITY_SID16);
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

static tavrn_mentorship_config_t make_mentorship_config(void)
{
    tavrn_mentorship_config_t config;

    memset(&config, 0, sizeof(config));
    config.offer_window_ms = 10u;
    config.page_timeout_ms = 30u;
    config.self_bootstrap_ms = TEST_SELF_BOOTSTRAP_MS;
    config.offer_suppression_ms = 100u;
    config.join_dedupe_ms = 100u;
    config.sync_dedupe_ms = 100u;
    config.rssi_weak_magnitude_db = 90u;
    config.rssi_strong_magnitude_db = 30u;
    config.rssi_weak_delay_ms = 500u;
    config.rssi_strong_delay_ms = 10u;
    config.jitter_min_ms = 0u;
    config.jitter_max_ms = 50u;
    config.page_attempts = 3u;
    return config;
}

static tavrn_maintenance_config_t make_maintenance_config(void)
{
    tavrn_maintenance_config_t config;

    memset(&config, 0, sizeof(config));
    config.hello_change_ms = TEST_HELLO_CHANGE_MS;
    config.hello_dedupe_ms = TEST_HELLO_DEDUPE_MS;
    config.initial_node_sequence = 0xffffu;
    return config;
}

static int setup_base(maintenance_fixture_t *fixture, uint32_t now_ms)
{
    tavrn_link_config_t link_config = make_link_config();
    aodv_core_config_t aodv_config = make_aodv_config();
    tavrn_gtt_config_t gtt_config;
    tavrn_router_incarnation_config_t incarnation_config;
    tavrn_mentorship_config_t mentorship_config = make_mentorship_config();
    tavrn_router_augmentation_hooks_t hooks;

    if (fixture == NULL) {
        return 0;
    }
    memset(fixture, 0, sizeof(*fixture));
    memset(&gtt_config, 0, sizeof(gtt_config));
    gtt_config.local_identity = make_adva(adva_a);
    gtt_config.soft_expiry_ms = 50u;
    gtt_config.hard_expiry_ms = 100u;
    gtt_config.departed_retention_ms = 200u;
    memset(&incarnation_config, 0, sizeof(incarnation_config));
    incarnation_config.feature_level = TAVRN_ROUTER_FEATURE_FULL_TAVRN;
    incarnation_config.boot_nonce = 0x5a01u;
    incarnation_config.reboot_announce_ms = 30u;
    ble_mesh_scheduler_init(&fixture->scheduler, now_ms, adva_a);
    if (tavrn_link_v2_init(&fixture->link, &fixture->scheduler, &link_config,
                           now_ms) != TAVRN_LINK_INIT_OK ||
        aodv_core_init(&fixture->aodv, &aodv_config, now_ms) != AODV_INIT_OK ||
        tavrn_gtt_init(&fixture->gtt, &fixture->gtt_storage, &gtt_config,
                       now_ms) != TAVRN_GTT_INIT_OK ||
        tavrn_full_init(&fixture->full, &fixture->gtt) != TAVRN_FULL_INIT_OK) {
        return 0;
    }
    hooks = tavrn_full_router_hooks(&fixture->full);
    return tavrn_router_init_with_incarnation(&fixture->router, &fixture->link,
                                               &fixture->aodv, &hooks,
                                               &incarnation_config, now_ms) ==
               TAVRN_ROUTER_INCARNATION_OK &&
        tavrn_mentorship_init(&fixture->mentorship, &fixture->router,
                              &fixture->gtt, &mentorship_config, now_ms) ==
            TAVRN_MENTORSHIP_OK;
}

static int observe_member(maintenance_fixture_t *fixture,
                          const uint8_t identity[TAVRN_ADVA_LEN],
                          uint16_t serial, uint32_t now_ms)
{
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_observe_status_t status;

    if (fixture == NULL) {
        return 0;
    }
    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = make_adva(identity);
    evidence.serial = serial;
    evidence.serial_present = 1u;
    evidence.hop_count = 1u;
    evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    status = tavrn_gtt_observe(&fixture->gtt, &evidence, now_ms);
    return status == TAVRN_GTT_OBSERVE_ADDED ||
        status == TAVRN_GTT_OBSERVE_REFRESHED;
}

static void clear_queued_controls(maintenance_fixture_t *fixture)
{
    uint8_t index;

    if (fixture == NULL) {
        return;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (fixture->scheduler.routed_tx_queue.entries[index].occupied != 0u) {
            (void)ble_mesh_tx_queue_remove(&fixture->scheduler.routed_tx_queue,
                                           index, NULL);
        }
    }
}

static int activate_full(maintenance_fixture_t *fixture, uint32_t now_ms)
{
    tavrn_mentorship_state_snapshot_t state;

    if (fixture == NULL ||
        tavrn_mentorship_tick(&fixture->mentorship, now_ms) !=
            TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED ||
        tavrn_mentorship_state_snapshot(&fixture->mentorship, &state) !=
            TAVRN_MENTORSHIP_OK) {
        return 0;
    }
    clear_queued_controls(fixture);
    return state.state == TAVRN_MENTORSHIP_SID8_ACTIVE &&
        state.active_width == TAVRN_IDENTITY_SID8 &&
        state.ordinary_traffic_gated == 0u &&
        fixture->link.config.local_peer.logical_id.width == TAVRN_IDENTITY_SID8 &&
        fixture->router.incarnation.snapshot.state ==
            TAVRN_ROUTER_INCARNATION_ESTABLISHED;
}

static int setup_armed(maintenance_fixture_t *fixture, uint32_t start_ms,
                       uint32_t *armed_at_out)
{
    tavrn_maintenance_config_t config = make_maintenance_config();
    tavrn_maintenance_snapshot_t snapshot;
    uint32_t armed_at = start_ms + TEST_SELF_BOOTSTRAP_MS;

    if (fixture == NULL || !setup_base(fixture, start_ms) ||
        tavrn_maintenance_init(&fixture->maintenance, &fixture->router,
                               &fixture->gtt, &config) != TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_activate(&fixture->maintenance, start_ms) !=
            TAVRN_MAINTENANCE_GATED ||
        tavrn_maintenance_snapshot(&fixture->maintenance, &snapshot) !=
            TAVRN_MAINTENANCE_OK || snapshot.armed != 0u || snapshot.pending != 0u ||
        !observe_member(fixture, adva_b, 0xfffeu, start_ms + 1u) ||
        !activate_full(fixture, armed_at) ||
        tavrn_maintenance_activate(&fixture->maintenance, armed_at) !=
            TAVRN_MAINTENANCE_OK) {
        return 0;
    }
    if (armed_at_out != NULL) {
        *armed_at_out = armed_at;
    }
    return 1;
}

static void pdu_put_u16(tavrn_validated_control_t *control, uint8_t offset,
                        uint16_t value)
{
    control->pdu[offset] = (uint8_t)value;
    control->pdu[(uint8_t)(offset + 1u)] = (uint8_t)(value >> 8);
}

static uint16_t pdu_u16(const uint8_t *pdu, uint8_t offset)
{
    return (uint16_t)pdu[offset] | ((uint16_t)pdu[(uint8_t)(offset + 1u)] << 8);
}

static tavrn_validated_control_t make_sid8_ordinary_hello(
    const uint8_t origin[TAVRN_ADVA_LEN], uint16_t sequence)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_HELLO;
    control.pdu_len = 17u;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = TEST_NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_HELLO;
    control.pdu[5] = 0x80u;
    control.pdu[6] = 0x10u;
    control.pdu[7] = 0xffu;
    control.pdu[8] = 0xffu;
    memcpy(&control.pdu[9], origin, TAVRN_ADVA_LEN);
    pdu_put_u16(&control, 15u, sequence);
    return control;
}

static tavrn_validated_control_t make_sid16_bootstrap_hello(
    const uint8_t origin[TAVRN_ADVA_LEN], uint16_t nonce)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_HELLO;
    control.pdu_len = 19u;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = TEST_NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_HELLO;
    control.pdu[5] = 0x40u;
    control.pdu[6] = 0x10u;
    memset(&control.pdu[7], 0xff, 4u);
    memcpy(&control.pdu[11], origin, TAVRN_ADVA_LEN);
    pdu_put_u16(&control, 17u, nonce);
    return control;
}

static int allow_identity(void *context, const tavrn_logical_id_t *logical_id,
                          const tavrn_adva_t *direct_adva_or_null)
{
    (void)context;
    (void)logical_id;
    (void)direct_adva_or_null;
    return 0;
}

static int wrap_control(const tavrn_validated_control_t *control,
                        const uint8_t transmitter[TAVRN_ADVA_LEN],
                        uint8_t adv_data[BLE_ADV_MAX_DATA], uint8_t *adv_len_out)
{
    tavrn_decoded_frame_t frame;
    tavrn_codec_config_t codec;
    size_t encoded_len = 0u;

    if (control == NULL || transmitter == NULL || adv_data == NULL ||
        adv_len_out == NULL) {
        return 0;
    }
    memset(&codec, 0, sizeof(codec));
    codec.network_id = TEST_NETWORK_ID;
    codec.local_peer = make_peer(transmitter, TAVRN_IDENTITY_SID8);
    codec.identity_conflict = allow_identity;
    memset(&frame, 0, sizeof(frame));
    frame.type = control->type;
    frame.network_id = control->pdu[3];
    frame.transmitter = make_peer(transmitter, TAVRN_IDENTITY_SID8);
    frame.detail.control = *control;
    if (tavrn_wire_v2_encode(&codec, &frame, adv_data, BLE_ADV_MAX_DATA,
                             &encoded_len) != TAVRN_CODEC_OK ||
        encoded_len > BLE_ADV_MAX_DATA) {
        return 0;
    }
    *adv_len_out = (uint8_t)encoded_len;
    return 1;
}

static int wrap_raw_pdu(const tavrn_validated_control_t *control,
                        uint8_t adv_data[BLE_ADV_MAX_DATA], uint8_t *adv_len_out)
{
    size_t length;

    if (control == NULL || adv_data == NULL || adv_len_out == NULL ||
        control->pdu_len > TAVRN_LINK_CONTROL_PDU_MAX) {
        return 0;
    }
    length = 7u + control->pdu_len;
    if (length > BLE_ADV_MAX_DATA) {
        return 0;
    }
    adv_data[0] = 0x02u;
    adv_data[1] = 0x01u;
    adv_data[2] = 0x06u;
    adv_data[3] = (uint8_t)(3u + control->pdu_len);
    adv_data[4] = 0xffu;
    adv_data[5] = 0xffu;
    adv_data[6] = 0xffu;
    memcpy(&adv_data[7], control->pdu, control->pdu_len);
    *adv_len_out = (uint8_t)length;
    return 1;
}

static tavrn_maintenance_status_t deliver_control(
    maintenance_fixture_t *fixture, const tavrn_validated_control_t *control,
    const uint8_t outer_adva[TAVRN_ADVA_LEN], uint32_t now_ms, uint8_t raw)
{
    ble_mesh_sched_event_t event;
    tavrn_mentorship_event_trace_t trace;
    tavrn_mentorship_status_t mentorship_status;

    if (fixture == NULL || control == NULL || outer_adva == NULL) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
    memcpy(event.adv_addr, outer_adva, TAVRN_ADVA_LEN);
    if ((raw != 0u ? wrap_raw_pdu(control, event.adv_data, &event.adv_len) :
                      wrap_control(control, outer_adva, event.adv_data,
                                   &event.adv_len)) == 0) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    memset(&trace, 0, sizeof(trace));
    mentorship_status = tavrn_mentorship_handle_scheduler_event(
        &fixture->mentorship, &event, now_ms, &trace);
    if (mentorship_status == TAVRN_MENTORSHIP_INVALID) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (trace.rx_control_present == 0u) {
        return TAVRN_MAINTENANCE_IGNORED;
    }
    return tavrn_maintenance_handle_rx_control(&fixture->maintenance,
                                               &trace.rx_control, now_ms);
}

static unsigned int queued_ordinary_hello_count(const maintenance_fixture_t *fixture)
{
    unsigned int count = 0u;
    uint8_t index;

    if (fixture == NULL) {
        return 0u;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry =
            &fixture->scheduler.routed_tx_queue.entries[index];

        if (entry->occupied != 0u && entry->item.adv_len >= 24u &&
            entry->item.adv_data[7] == 0x54u && entry->item.adv_data[8] == 0x52u &&
            entry->item.adv_data[9] == 0x02u &&
            entry->item.adv_data[11] == TAVRN_WIRE_HELLO &&
            entry->item.adv_data[12] == 0x80u) {
            count++;
        }
    }
    return count;
}

static const ble_mesh_tx_queue_entry_t *sole_queued_ordinary_hello(
    const maintenance_fixture_t *fixture)
{
    const ble_mesh_tx_queue_entry_t *result = NULL;
    uint8_t index;

    if (fixture == NULL) {
        return NULL;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry =
            &fixture->scheduler.routed_tx_queue.entries[index];

        if (entry->occupied == 0u || entry->item.adv_len < 24u ||
            entry->item.adv_data[11] != TAVRN_WIRE_HELLO ||
            entry->item.adv_data[12] != 0x80u) {
            continue;
        }
        if (result != NULL) {
            return NULL;
        }
        result = entry;
    }
    return result;
}

static int queued_hello_matches(const ble_mesh_tx_queue_entry_t *entry,
                                const uint8_t origin[TAVRN_ADVA_LEN],
                                uint16_t sequence)
{
    const uint8_t *pdu;

    if (entry == NULL) {
        return 0;
    }
    pdu = &entry->item.adv_data[7];
    return entry->item.adv_len == 24u && entry->item.adv_data[3] == 20u &&
        pdu[0] == 0x54u && pdu[1] == 0x52u && pdu[2] == 0x02u &&
        pdu[3] == TEST_NETWORK_ID && pdu[4] == TAVRN_WIRE_HELLO &&
        pdu[5] == 0x80u && pdu[6] == 0x10u && pdu[7] == 0xffu &&
        pdu[8] == 0xffu && memcmp(&pdu[9], origin, TAVRN_ADVA_LEN) == 0 &&
        pdu_u16(pdu, 15u) == sequence;
}

static int fill_control_queue(maintenance_fixture_t *fixture, uint32_t now_ms)
{
    uint8_t index;

    if (fixture == NULL) {
        return 0;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        tavrn_validated_control_t control = make_sid8_ordinary_hello(
            adva_a, (uint16_t)(0x8000u + index));
        tavrn_link_event_t outcome;

        memset(&outcome, 0, sizeof(outcome));
        if (tavrn_link_v2_send_control(&fixture->link, &control, NULL, 0u,
                                       now_ms, &outcome) != TAVRN_LINK_SEND_OK) {
            return 0;
        }
    }
    return ble_mesh_tx_queue_count(&fixture->scheduler.routed_tx_queue) ==
        BLE_MESH_TX_QUEUE_CAPACITY;
}

static int capture_protocol_snapshot(const maintenance_fixture_t *fixture,
                                     protocol_snapshot_t *snapshot_out)
{
    const tavrn_maintenance_counters_t *counters;

    if (fixture == NULL || snapshot_out == NULL ||
        tavrn_maintenance_snapshot(&fixture->maintenance,
                                   &snapshot_out->maintenance) !=
            TAVRN_MAINTENANCE_OK ||
        (counters = tavrn_maintenance_counters(&fixture->maintenance)) == NULL) {
        return 0;
    }
    snapshot_out->gtt_storage = fixture->gtt_storage;
    snapshot_out->gtt_counters = *tavrn_gtt_counters(&fixture->gtt);
    snapshot_out->aodv_counters = *aodv_core_counters(&fixture->aodv);
    snapshot_out->maintenance_counters = *counters;
    snapshot_out->queued_controls =
        ble_mesh_tx_queue_count(&fixture->scheduler.routed_tx_queue);
    return 1;
}

static int nonmaintenance_protocol_unchanged(const maintenance_fixture_t *fixture,
                                             const protocol_snapshot_t *before)
{
    return fixture != NULL && before != NULL &&
        memcmp(&fixture->gtt_storage, &before->gtt_storage,
               sizeof(before->gtt_storage)) == 0 &&
        memcmp(aodv_core_counters(&fixture->aodv), &before->aodv_counters,
               sizeof(before->aodv_counters)) == 0 &&
        ble_mesh_tx_queue_count(&fixture->scheduler.routed_tx_queue) ==
            before->queued_controls;
}

static int maintenance_shape_unchanged(const maintenance_fixture_t *fixture,
                                       const protocol_snapshot_t *before)
{
    tavrn_maintenance_snapshot_t current;

    return fixture != NULL && before != NULL &&
        tavrn_maintenance_snapshot(&fixture->maintenance, &current) ==
            TAVRN_MAINTENANCE_OK &&
        memcmp(&current, &before->maintenance, sizeof(current)) == 0;
}

static int test_maint_02_arm_cadence_wire_and_busy(void)
{
    maintenance_fixture_t fixture;
    maintenance_fixture_t busy_fixture;
    maintenance_fixture_t teardown_fixture;
    tavrn_maintenance_snapshot_t state;
    const tavrn_maintenance_counters_t *counters;
    const ble_mesh_tx_queue_entry_t *queued;
    tavrn_direct_peer_t sid16_local = make_peer(adva_a, TAVRN_IDENTITY_SID16);
    uint32_t armed_at;
    uint32_t due_at;
    uint32_t busy_armed_at;
    uint32_t busy_due_at;
    uint32_t teardown_armed_at;
    uint32_t teardown_due_at;
    int ok = 1;

    /* The handover and first change-cadence deadline both cross uint32 wrap. */
    ok &= setup_armed(&fixture, 0xffffffe0u, &armed_at);
    due_at = armed_at + TEST_HELLO_CHANGE_MS;
    ok &= armed_at == 0xfffffff4u && due_at == 0x00000008u;
    ok &= tavrn_maintenance_snapshot(&fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK &&
        state.armed != 0u && state.pending == 0u &&
        state.next_node_sequence == 0xffffu && state.next_hello_due_ms == due_at;
    ok &= tavrn_maintenance_tick(&fixture.maintenance, due_at - 1u) ==
            TAVRN_MAINTENANCE_OK &&
        queued_ordinary_hello_count(&fixture) == 0u;
    ok &= tavrn_maintenance_tick(&fixture.maintenance, due_at) ==
            TAVRN_MAINTENANCE_OK &&
        queued_ordinary_hello_count(&fixture) == 1u;
    queued = sole_queued_ordinary_hello(&fixture);
    counters = tavrn_maintenance_counters(&fixture.maintenance);
    ok &= queued_hello_matches(queued, adva_a, 0xffffu) && counters != NULL &&
        counters->due == 1u && counters->enqueued == 1u && counters->busy == 0u &&
        counters->pending == 0u &&
        tavrn_maintenance_snapshot(&fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK &&
        state.next_node_sequence == 0u && state.next_hello_due_ms ==
            due_at + TEST_HELLO_CHANGE_MS;

    ok &= setup_armed(&busy_fixture, 0xffffffe0u, &busy_armed_at);
    busy_due_at = busy_armed_at + TEST_HELLO_CHANGE_MS;
    ok &= fill_control_queue(&busy_fixture, busy_due_at - 1u) &&
        tavrn_maintenance_tick(&busy_fixture.maintenance, busy_due_at) ==
            TAVRN_MAINTENANCE_BUSY &&
        tavrn_maintenance_snapshot(&busy_fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK &&
        state.pending != 0u && state.next_node_sequence == 0xffffu &&
        state.next_hello_due_ms == busy_due_at &&
        state.pending_hello.pdu_len == 17u &&
        state.pending_hello.pdu[5] == 0x80u &&
        pdu_u16(state.pending_hello.pdu, 15u) == 0xffffu &&
        tavrn_maintenance_counters(&busy_fixture.maintenance)->due == 1u &&
        tavrn_maintenance_counters(&busy_fixture.maintenance)->enqueued == 0u &&
        tavrn_maintenance_counters(&busy_fixture.maintenance)->busy == 1u &&
        tavrn_maintenance_counters(&busy_fixture.maintenance)->pending == 1u;
    ok &= tavrn_maintenance_tick(&busy_fixture.maintenance, busy_due_at + 1u) ==
            TAVRN_MAINTENANCE_BUSY &&
        tavrn_maintenance_snapshot(&busy_fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK && state.pending != 0u &&
        pdu_u16(state.pending_hello.pdu, 15u) == 0xffffu &&
        tavrn_maintenance_counters(&busy_fixture.maintenance)->pending == 1u;
    clear_queued_controls(&busy_fixture);
    ok &= tavrn_maintenance_tick(&busy_fixture.maintenance, busy_due_at + 2u) ==
            TAVRN_MAINTENANCE_OK &&
        queued_ordinary_hello_count(&busy_fixture) == 1u &&
        queued_hello_matches(sole_queued_ordinary_hello(&busy_fixture), adva_a,
                              0xffffu) &&
        tavrn_maintenance_snapshot(&busy_fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK && state.pending == 0u &&
        state.next_node_sequence == 0u && state.next_hello_due_ms ==
            busy_due_at + 2u + TEST_HELLO_CHANGE_MS &&
        tavrn_maintenance_counters(&busy_fixture.maintenance)->enqueued == 1u &&
        tavrn_maintenance_counters(&busy_fixture.maintenance)->pending == 0u;

    /* Maintenance owns the queued control's cancellation when FULL handover is
     * revoked; no stale SID8 HELLO remains after the router is reconfigured. */
    ok &= setup_armed(&teardown_fixture, 0u, &teardown_armed_at);
    teardown_due_at = teardown_armed_at + TEST_HELLO_CHANGE_MS;
    ok &= tavrn_maintenance_tick(&teardown_fixture.maintenance, teardown_due_at) ==
            TAVRN_MAINTENANCE_OK &&
        queued_ordinary_hello_count(&teardown_fixture) == 1u &&
        tavrn_router_reconfigure_identity(&teardown_fixture.router, &sid16_local,
                                          teardown_due_at + 1u) ==
            TAVRN_ROUTER_INCARNATION_OK &&
        tavrn_maintenance_tick(&teardown_fixture.maintenance,
                               teardown_due_at + 1u) == TAVRN_MAINTENANCE_GATED &&
        queued_ordinary_hello_count(&teardown_fixture) == 0u &&
        tavrn_maintenance_snapshot(&teardown_fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK && state.armed == 0u && state.pending == 0u;
    return ok;
}

static int test_gtt_03_unique_sid8_hello_is_passive(void)
{
    maintenance_fixture_t fixture;
    maintenance_fixture_t reboot_fixture;
    tavrn_validated_control_t hello = make_sid8_ordinary_hello(adva_b, 0xffffu);
    tavrn_validated_control_t reboot_hello = make_sid8_ordinary_hello(adva_b, 2u);
    tavrn_validated_control_t same_epoch_stale =
        make_sid8_ordinary_hello(adva_b, 1u);
    tavrn_validated_control_t first_bootstrap =
        make_sid16_bootstrap_hello(adva_b, 0x6001u);
    tavrn_validated_control_t second_bootstrap =
        make_sid16_bootstrap_hello(adva_b, 0x6002u);
    tavrn_adva_t reboot_peer = make_adva(adva_b);
    tavrn_gtt_snapshot_t member;
    tavrn_gtt_snapshot_t before_clear;
    tavrn_gtt_snapshot_t after_clear;
    tavrn_router_incarnation_peer_snapshot_t peer;
    protocol_snapshot_t before;
    const tavrn_maintenance_counters_t *counters;
    uint32_t armed_at;
    int ok = 1;
    int reboot_ok;

    ok &= setup_armed(&fixture, 0u, &armed_at) && armed_at == TEST_SELF_BOOTSTRAP_MS;
    ok &= capture_protocol_snapshot(&fixture, &before);
    ok &= deliver_control(&fixture, &hello, adva_b, armed_at + 1u, 0u) ==
            TAVRN_MAINTENANCE_RX_UNIQUE &&
        tavrn_gtt_snapshot(&fixture.gtt, &(tavrn_adva_t){ .bytes = {
            adva_b[0], adva_b[1], adva_b[2], adva_b[3], adva_b[4], adva_b[5],
        } }, armed_at + 1u, &member) == TAVRN_GTT_QUERY_FOUND &&
        member.serial_present != 0u && member.serial == 0xffffu &&
        member.hop_count == 1u && member.last_evidence_ms == armed_at + 1u;
    counters = tavrn_maintenance_counters(&fixture.maintenance);
    ok &= counters != NULL && counters->rx_unique == before.maintenance_counters.rx_unique +
            1u && counters->rx_duplicate == before.maintenance_counters.rx_duplicate &&
        counters->rx_rejected == before.maintenance_counters.rx_rejected &&
        memcmp(tavrn_gtt_counters(&fixture.gtt), &before.gtt_counters,
               sizeof(before.gtt_counters)) == 0;
    ok &= memcmp(aodv_core_counters(&fixture.aodv), &before.aodv_counters,
                 sizeof(before.aodv_counters)) == 0 &&
        ble_mesh_tx_queue_count(&fixture.scheduler.routed_tx_queue) ==
            before.queued_controls;

    /* The production scheduler -> mentorship -> copied RX_CONTROL path must
     * learn a committed boot nonce before evaluating its ordinary N=0 HELLOs.
     * A reboot may restart node sequence at an older value without inheriting
     * either GTT serial rejection or the per-HELLO duplicate entry. */
    reboot_ok = setup_armed(&reboot_fixture, 0u, &armed_at) &&
        tavrn_gtt_snapshot(&reboot_fixture.gtt, &(tavrn_adva_t){ .bytes = {
            adva_b[0], adva_b[1], adva_b[2], adva_b[3], adva_b[4], adva_b[5],
        } }, armed_at, &before_clear) == TAVRN_GTT_QUERY_FOUND &&
        before_clear.serial_present != 0u && before_clear.serial == 0xfffeu &&
        tavrn_gtt_clear_serial(&reboot_fixture.gtt, &before_clear.identity,
                               armed_at) == TAVRN_GTT_SERIAL_CLEAR_FOUND &&
        tavrn_gtt_snapshot(&reboot_fixture.gtt, &before_clear.identity, armed_at,
                           &after_clear) == TAVRN_GTT_QUERY_FOUND &&
        memcmp(&after_clear.identity, &before_clear.identity,
               sizeof(after_clear.identity)) == 0 &&
        after_clear.last_evidence_ms == before_clear.last_evidence_ms &&
        after_clear.soft_deadline_ms == before_clear.soft_deadline_ms &&
        after_clear.hard_deadline_ms == before_clear.hard_deadline_ms &&
        after_clear.departed_deadline_ms == before_clear.departed_deadline_ms &&
        after_clear.hop_count == before_clear.hop_count &&
        after_clear.freshness == before_clear.freshness &&
        after_clear.serial_present == 0u && after_clear.serial == 0u &&
        tavrn_gtt_clear_serial(&reboot_fixture.gtt,
                               &reboot_fixture.gtt.config.local_identity,
                               armed_at) == TAVRN_GTT_SERIAL_CLEAR_INVALID &&
        observe_member(&reboot_fixture, adva_b, 0xfffeu, armed_at + 1u);
    if (reboot_ok != 0) {
        reboot_ok = deliver_control(&reboot_fixture, &first_bootstrap, adva_b,
                                    armed_at + 2u, 0u) ==
                TAVRN_MAINTENANCE_IGNORED &&
            tavrn_router_incarnation_peer_snapshot(
                &reboot_fixture.router, &reboot_peer, &peer) ==
                TAVRN_ROUTER_INCARNATION_OK && peer.direct_binding_valid != 0u &&
        peer.reset_pending == 0u && peer.boot_nonce == 0x6001u &&
        tavrn_gtt_snapshot(&reboot_fixture.gtt, &before_clear.identity,
                           armed_at + 2u, &after_clear) == TAVRN_GTT_QUERY_FOUND &&
        after_clear.serial_present == 0u && after_clear.serial == 0u &&
        after_clear.hop_count == 1u && after_clear.last_evidence_ms == armed_at + 2u;
    }
    if (reboot_ok != 0) {
        reboot_ok = deliver_control(&reboot_fixture, &reboot_hello, adva_b,
                                    armed_at + 3u, 0u) ==
                TAVRN_MAINTENANCE_RX_UNIQUE &&
        deliver_control(&reboot_fixture, &reboot_hello, adva_b,
                        armed_at + 4u, 0u) == TAVRN_MAINTENANCE_RX_DUPLICATE;
    }
    if (reboot_ok != 0) {
        reboot_ok = deliver_control(&reboot_fixture, &first_bootstrap, adva_b,
                                    armed_at + 5u, 0u) ==
                TAVRN_MAINTENANCE_IGNORED &&
            deliver_control(&reboot_fixture, &same_epoch_stale, adva_b,
                            armed_at + 6u, 0u) ==
                TAVRN_MAINTENANCE_RX_REJECTED &&
            tavrn_gtt_snapshot(&reboot_fixture.gtt, &before_clear.identity,
                               armed_at + 6u, &after_clear) ==
                TAVRN_GTT_QUERY_FOUND &&
            after_clear.serial_present != 0u && after_clear.serial == 2u &&
            after_clear.last_evidence_ms == armed_at + 3u;
    }
    if (reboot_ok != 0) {
        reboot_ok = deliver_control(&reboot_fixture, &second_bootstrap, adva_b,
                                    armed_at + 7u, 0u) ==
                TAVRN_MAINTENANCE_IGNORED &&
        tavrn_router_incarnation_peer_snapshot(
            &reboot_fixture.router, &reboot_peer, &peer) ==
            TAVRN_ROUTER_INCARNATION_OK && peer.direct_binding_valid != 0u &&
        peer.reset_pending == 0u && peer.boot_nonce == 0x6002u &&
        tavrn_gtt_snapshot(&reboot_fixture.gtt, &before_clear.identity,
                           armed_at + 7u, &after_clear) == TAVRN_GTT_QUERY_FOUND &&
        after_clear.serial_present == 0u && after_clear.serial == 0u;
    }
    if (reboot_ok != 0) {
        reboot_ok = deliver_control(&reboot_fixture, &reboot_hello, adva_b,
                                    armed_at + 8u, 0u) ==
                TAVRN_MAINTENANCE_RX_UNIQUE &&
        tavrn_gtt_snapshot(&reboot_fixture.gtt, &before_clear.identity,
                           armed_at + 8u, &after_clear) == TAVRN_GTT_QUERY_FOUND &&
        after_clear.serial_present != 0u && after_clear.serial == 2u;
    }
    if (reboot_ok != 0) {
        tavrn_validated_control_t hello3 =
            make_sid8_ordinary_hello(adva_b, 3u);
        tavrn_validated_control_t hello4 =
            make_sid8_ordinary_hello(adva_b, 4u);
        tavrn_validated_control_t hello5 =
            make_sid8_ordinary_hello(adva_b, 5u);

        reboot_ok = deliver_control(&reboot_fixture, &hello3, adva_b,
                                    armed_at + 50u, 0u) ==
                TAVRN_MAINTENANCE_RX_UNIQUE &&
            deliver_control(&reboot_fixture, &hello4, adva_b,
                            armed_at + 100u, 0u) ==
                TAVRN_MAINTENANCE_RX_UNIQUE &&
            deliver_control(&reboot_fixture, &hello5, adva_b,
                            armed_at + 140u, 0u) ==
                TAVRN_MAINTENANCE_RX_UNIQUE &&
            tavrn_gtt_snapshot(&reboot_fixture.gtt, &before_clear.identity,
                               armed_at + 160u, &after_clear) ==
                TAVRN_GTT_QUERY_FOUND &&
            after_clear.freshness == TAVRN_GTT_FRESHNESS_ACTIVE &&
            after_clear.serial == 5u &&
            after_clear.last_evidence_ms == armed_at + 140u;
    }
    ok &= reboot_ok;
    return ok;
}

static int test_serial_03_hello_equality_dedupe(void)
{
    maintenance_fixture_t fixture;
    tavrn_validated_control_t wrapped = make_sid8_ordinary_hello(adva_b, 0xffffu);
    tavrn_validated_control_t zero = make_sid8_ordinary_hello(adva_b, 0u);
    tavrn_gtt_snapshot_t before_duplicate;
    tavrn_gtt_snapshot_t after_duplicate;
    protocol_snapshot_t before;
    uint32_t armed_at;
    uint32_t expiry_at;
    int ok = 1;

    ok &= setup_armed(&fixture, 0xffffffe0u, &armed_at);
    expiry_at = armed_at + 2u + TEST_HELLO_DEDUPE_MS;
    ok &= armed_at == 0xfffffff4u && expiry_at == 0u;
    ok &= deliver_control(&fixture, &wrapped, adva_b, armed_at + 1u, 0u) ==
            TAVRN_MAINTENANCE_RX_UNIQUE &&
        tavrn_gtt_snapshot(&fixture.gtt, &(tavrn_adva_t){ .bytes = {
            adva_b[0], adva_b[1], adva_b[2], adva_b[3], adva_b[4], adva_b[5],
        } }, armed_at + 1u, &before_duplicate) == TAVRN_GTT_QUERY_FOUND &&
        deliver_control(&fixture, &zero, adva_b, armed_at + 2u, 0u) ==
            TAVRN_MAINTENANCE_RX_UNIQUE;
    ok &= capture_protocol_snapshot(&fixture, &before) &&
        tavrn_gtt_snapshot(&fixture.gtt, &(tavrn_adva_t){ .bytes = {
            adva_b[0], adva_b[1], adva_b[2], adva_b[3], adva_b[4], adva_b[5],
        } }, armed_at + 2u, &before_duplicate) == TAVRN_GTT_QUERY_FOUND &&
        deliver_control(&fixture, &zero, adva_b, armed_at + 3u, 0u) ==
            TAVRN_MAINTENANCE_RX_DUPLICATE &&
        tavrn_gtt_snapshot(&fixture.gtt, &(tavrn_adva_t){ .bytes = {
            adva_b[0], adva_b[1], adva_b[2], adva_b[3], adva_b[4], adva_b[5],
        } }, armed_at + 3u, &after_duplicate) == TAVRN_GTT_QUERY_FOUND &&
        after_duplicate.last_evidence_ms == before_duplicate.last_evidence_ms &&
        after_duplicate.serial == before_duplicate.serial &&
        tavrn_maintenance_counters(&fixture.maintenance)->rx_unique ==
            before.maintenance_counters.rx_unique &&
        tavrn_maintenance_counters(&fixture.maintenance)->rx_duplicate ==
            before.maintenance_counters.rx_duplicate + 1u &&
        nonmaintenance_protocol_unchanged(&fixture, &before) &&
        maintenance_shape_unchanged(&fixture, &before);
    ok &= deliver_control(&fixture, &zero, adva_b, expiry_at, 0u) ==
            TAVRN_MAINTENANCE_RX_REJECTED &&
        tavrn_gtt_snapshot(&fixture.gtt, &(tavrn_adva_t){ .bytes = {
            adva_b[0], adva_b[1], adva_b[2], adva_b[3], adva_b[4], adva_b[5],
        } }, expiry_at, &after_duplicate) ==
            TAVRN_GTT_QUERY_FOUND &&
        after_duplicate.last_evidence_ms == before_duplicate.last_evidence_ms &&
        after_duplicate.serial == 0u &&
        nonmaintenance_protocol_unchanged(&fixture, &before) &&
        memcmp(tavrn_gtt_counters(&fixture.gtt), &before.gtt_counters,
               sizeof(before.gtt_counters)) == 0 &&
        tavrn_maintenance_counters(&fixture.maintenance)->rx_unique ==
            before.maintenance_counters.rx_unique &&
        tavrn_maintenance_counters(&fixture.maintenance)->rx_duplicate ==
            before.maintenance_counters.rx_duplicate + 1u &&
        tavrn_maintenance_counters(&fixture.maintenance)->rx_rejected ==
            before.maintenance_counters.rx_rejected + 1u;
    return ok;
}

static int test_bearer_04_rx_rejects_without_feedback(void)
{
    maintenance_fixture_t fixture;
    tavrn_validated_control_t targeted = make_sid8_ordinary_hello(adva_b, 7u);
    tavrn_validated_control_t foreign = make_sid8_ordinary_hello(adva_b, 8u);
    tavrn_validated_control_t mismatched = make_sid8_ordinary_hello(adva_b, 9u);
    tavrn_validated_control_t malformed = make_sid8_ordinary_hello(adva_b, 10u);
    tavrn_validated_control_t bootstrap = make_sid16_bootstrap_hello(adva_b, 0x6001u);
    protocol_snapshot_t before;
    protocol_snapshot_t after_n1;
    uint32_t armed_at;
    int ok = 1;

    targeted.pdu[5] = 0xa0u;
    targeted.pdu[7] = adva_a[0];
    targeted.pdu[8] = adva_a[0];
    foreign.pdu[3] = 0x55u;
    malformed.pdu[5] = 0x81u;
    ok &= setup_armed(&fixture, 0u, &armed_at) && capture_protocol_snapshot(&fixture,
                                                                              &before);
    ok &= deliver_control(&fixture, &targeted, adva_b, armed_at + 1u, 0u) ==
            TAVRN_MAINTENANCE_RX_REJECTED &&
        nonmaintenance_protocol_unchanged(&fixture, &before) &&
        maintenance_shape_unchanged(&fixture, &before) &&
        tavrn_maintenance_counters(&fixture.maintenance)->rx_rejected ==
            before.maintenance_counters.rx_rejected + 1u &&
        tavrn_maintenance_counters(&fixture.maintenance)->enqueued ==
            before.maintenance_counters.enqueued;
    ok &= capture_protocol_snapshot(&fixture, &after_n1) &&
        deliver_control(&fixture, &foreign, adva_b, armed_at + 2u, 1u) ==
            TAVRN_MAINTENANCE_IGNORED &&
        nonmaintenance_protocol_unchanged(&fixture, &after_n1) &&
        maintenance_shape_unchanged(&fixture, &after_n1) &&
        memcmp(tavrn_maintenance_counters(&fixture.maintenance),
               &after_n1.maintenance_counters,
               sizeof(after_n1.maintenance_counters)) == 0;
    ok &= capture_protocol_snapshot(&fixture, &after_n1) &&
        deliver_control(&fixture, &mismatched, adva_c, armed_at + 3u, 1u) ==
            TAVRN_MAINTENANCE_IGNORED &&
        nonmaintenance_protocol_unchanged(&fixture, &after_n1) &&
        maintenance_shape_unchanged(&fixture, &after_n1) &&
        memcmp(tavrn_maintenance_counters(&fixture.maintenance),
               &after_n1.maintenance_counters,
               sizeof(after_n1.maintenance_counters)) == 0;
    ok &= capture_protocol_snapshot(&fixture, &after_n1) &&
        deliver_control(&fixture, &malformed, adva_b, armed_at + 4u, 1u) ==
            TAVRN_MAINTENANCE_IGNORED &&
        nonmaintenance_protocol_unchanged(&fixture, &after_n1) &&
        maintenance_shape_unchanged(&fixture, &after_n1) &&
        memcmp(tavrn_maintenance_counters(&fixture.maintenance),
               &after_n1.maintenance_counters,
               sizeof(after_n1.maintenance_counters)) == 0;
    ok &= capture_protocol_snapshot(&fixture, &after_n1) &&
        deliver_control(&fixture, &bootstrap, adva_b, armed_at + 5u, 0u) ==
            TAVRN_MAINTENANCE_IGNORED &&
        maintenance_shape_unchanged(&fixture, &after_n1) &&
        memcmp(tavrn_maintenance_counters(&fixture.maintenance),
               &after_n1.maintenance_counters,
               sizeof(after_n1.maintenance_counters)) == 0;
    return ok;
}

#if defined(TAVRN_MAINTENANCE_ADAPTIVE_CONTRACT)

#define TEST_HELLO_STABLE_MS 80u
#define TEST_HELLO_ALPHA_PERMILLE 800u
#define TEST_HELLO_SNAP_PERMILLE 950u
#define TEST_TOPOLOGY_SAMPLE_MS 1u

static tavrn_maintenance_config_t make_adaptive_maintenance_config(void)
{
    tavrn_maintenance_config_t config;

    memset(&config, 0, sizeof(config));
    config.hello_change_ms = TEST_HELLO_CHANGE_MS;
    config.hello_stable_ms = TEST_HELLO_STABLE_MS;
    config.hello_alpha_permille = TEST_HELLO_ALPHA_PERMILLE;
    config.hello_snap_permille = TEST_HELLO_SNAP_PERMILLE;
    config.topology_sample_ms = TEST_TOPOLOGY_SAMPLE_MS;
    config.hello_dedupe_ms = TEST_HELLO_DEDUPE_MS;
    config.initial_node_sequence = 0xffffu;
    return config;
}

static int observe_member_with_hop(maintenance_fixture_t *fixture,
                                   const uint8_t identity[TAVRN_ADVA_LEN],
                                   uint16_t serial, uint8_t hop_count,
                                   tavrn_gtt_evidence_kind_t kind,
                                   uint32_t now_ms)
{
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_observe_status_t status;

    if (fixture == NULL) {
        return 0;
    }
    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = make_adva(identity);
    evidence.serial = serial;
    evidence.serial_present = 1u;
    evidence.hop_count = hop_count;
    evidence.kind = kind;
    status = tavrn_gtt_observe(&fixture->gtt, &evidence, now_ms);
    return status == TAVRN_GTT_OBSERVE_ADDED ||
        status == TAVRN_GTT_OBSERVE_REFRESHED ||
        status == TAVRN_GTT_OBSERVE_DEPARTED;
}

static int setup_adaptive_armed_with_config(
    maintenance_fixture_t *fixture, const tavrn_maintenance_config_t *config,
    uint32_t start_ms, uint32_t *armed_at_out)
{
    tavrn_maintenance_snapshot_t snapshot;
    tavrn_gtt_config_t gtt_config;
    uint32_t armed_at = start_ms + TEST_SELF_BOOTSTRAP_MS;

    if (fixture == NULL || config == NULL || !setup_base(fixture, start_ms)) {
        return 0;
    }
    /* Long test-local GTT retention keeps topology sampling independent of
     * expiry, which Step 6.6 owns. */
    gtt_config = fixture->gtt.config;
    gtt_config.soft_expiry_ms = 10000u;
    gtt_config.hard_expiry_ms = 20000u;
    gtt_config.departed_retention_ms = 40000u;
    if (tavrn_gtt_init(&fixture->gtt, &fixture->gtt_storage, &gtt_config,
                       start_ms) != TAVRN_GTT_INIT_OK ||
        tavrn_maintenance_init(&fixture->maintenance, &fixture->router,
                               &fixture->gtt, config) != TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_activate(&fixture->maintenance, start_ms) !=
            TAVRN_MAINTENANCE_GATED ||
        !observe_member_with_hop(fixture, adva_b, 0xfffeu, 1u,
                                 TAVRN_GTT_EVIDENCE_LIVENESS, start_ms + 1u) ||
        !activate_full(fixture, armed_at) ||
        tavrn_maintenance_activate(&fixture->maintenance, armed_at) !=
            TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_snapshot(&fixture->maintenance, &snapshot) !=
            TAVRN_MAINTENANCE_OK) {
        return 0;
    }
    if (armed_at_out != NULL) {
        *armed_at_out = armed_at;
    }
    return snapshot.armed != 0u;
}

static int setup_adaptive_armed(maintenance_fixture_t *fixture,
                                uint32_t start_ms, uint32_t *armed_at_out)
{
    tavrn_maintenance_config_t config = make_adaptive_maintenance_config();

    return setup_adaptive_armed_with_config(fixture, &config, start_ms,
                                            armed_at_out);
}

static uint32_t adaptive_ema_step(uint32_t current,
                                  const tavrn_maintenance_config_t *config)
{
    uint64_t numerator;
    uint32_t next;
    uint32_t snap_at;

    numerator = (uint64_t)config->hello_alpha_permille * current +
        (uint64_t)(1000u - config->hello_alpha_permille) *
            config->hello_stable_ms + 500u;
    next = (uint32_t)(numerator / 1000u);
    if (next > config->hello_stable_ms) {
        next = config->hello_stable_ms;
    }
    snap_at = (uint32_t)(((uint64_t)config->hello_snap_permille *
                          config->hello_stable_ms) / 1000u);
    return next >= snap_at ? config->hello_stable_ms : next;
}

static int adaptive_config_contract_is_validated(void)
{
    maintenance_fixture_t fixture;
    tavrn_maintenance_config_t config = make_adaptive_maintenance_config();
    int ok;

    if (!setup_base(&fixture, 0u)) {
        return 0;
    }
    ok = tavrn_maintenance_init(&fixture.maintenance, &fixture.router,
                                &fixture.gtt, &config) == TAVRN_MAINTENANCE_OK;
    config.hello_change_ms = 0u;
    ok &= tavrn_maintenance_init(&fixture.maintenance, &fixture.router,
                                 &fixture.gtt, &config) == TAVRN_MAINTENANCE_INVALID;
    config = make_adaptive_maintenance_config();
    config.hello_stable_ms = config.hello_change_ms - 1u;
    ok &= tavrn_maintenance_init(&fixture.maintenance, &fixture.router,
                                 &fixture.gtt, &config) == TAVRN_MAINTENANCE_INVALID;
    config = make_adaptive_maintenance_config();
    config.hello_stable_ms = 0x80000000u;
    ok &= tavrn_maintenance_init(&fixture.maintenance, &fixture.router,
                                 &fixture.gtt, &config) == TAVRN_MAINTENANCE_INVALID;
    config = make_adaptive_maintenance_config();
    config.hello_alpha_permille = 0u;
    ok &= tavrn_maintenance_init(&fixture.maintenance, &fixture.router,
                                 &fixture.gtt, &config) == TAVRN_MAINTENANCE_INVALID;
    config = make_adaptive_maintenance_config();
    config.hello_alpha_permille = 1001u;
    ok &= tavrn_maintenance_init(&fixture.maintenance, &fixture.router,
                                 &fixture.gtt, &config) == TAVRN_MAINTENANCE_INVALID;
    config = make_adaptive_maintenance_config();
    config.hello_snap_permille = 0u;
    ok &= tavrn_maintenance_init(&fixture.maintenance, &fixture.router,
                                 &fixture.gtt, &config) == TAVRN_MAINTENANCE_INVALID;
    config = make_adaptive_maintenance_config();
    config.hello_snap_permille = 1001u;
    ok &= tavrn_maintenance_init(&fixture.maintenance, &fixture.router,
                                 &fixture.gtt, &config) == TAVRN_MAINTENANCE_INVALID;
    config = make_adaptive_maintenance_config();
    config.topology_sample_ms = 0u;
    ok &= tavrn_maintenance_init(&fixture.maintenance, &fixture.router,
                                 &fixture.gtt, &config) == TAVRN_MAINTENANCE_INVALID;
    config = make_adaptive_maintenance_config();
    config.topology_sample_ms = 0x80000000u;
    return ok && tavrn_maintenance_init(&fixture.maintenance, &fixture.router,
                                        &fixture.gtt, &config) ==
        TAVRN_MAINTENANCE_INVALID;
}

static int advance_to_stable(maintenance_fixture_t *fixture,
                             uint32_t armed_at, uint32_t *stable_due_out)
{
    tavrn_maintenance_config_t config = make_adaptive_maintenance_config();
    tavrn_maintenance_snapshot_t snapshot;
    uint32_t due = armed_at + config.hello_change_ms;
    uint32_t interval = config.hello_change_ms;
    uint8_t iteration;
    int ok = 1;

    for (iteration = 0u; interval != config.hello_stable_ms && iteration < 16u;
         iteration++) {
        interval = adaptive_ema_step(interval, &config);
        ok &= tavrn_maintenance_tick(&fixture->maintenance, due) ==
            TAVRN_MAINTENANCE_OK;
        ok &= tavrn_maintenance_snapshot(&fixture->maintenance, &snapshot) ==
                TAVRN_MAINTENANCE_OK &&
            snapshot.current_interval_ms == interval &&
            snapshot.next_hello_due_ms == due + interval;
        clear_queued_controls(fixture);
        due += interval;
    }
    if (stable_due_out != NULL) {
        *stable_due_out = due;
    }
    return ok && interval == config.hello_stable_ms;
}

static unsigned int source_occurrences(const char *path, const char *needle)
{
    FILE *file;
    size_t matched = 0u;
    size_t needle_length;
    unsigned int count = 0u;
    int character;

    if (path == NULL || needle == NULL || needle[0] == '\0') {
        return 0u;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return 0u;
    }
    needle_length = strlen(needle);
    while ((character = fgetc(file)) != EOF) {
        if ((char)character == needle[matched]) {
            matched++;
            if (matched == needle_length) {
                count++;
                matched = 0u;
            }
        } else {
            matched = (char)character == needle[0] ? 1u : 0u;
        }
    }
    (void)fclose(file);
    return count;
}

static int source_lines_are_ordered(const char *path, const char *first,
                                    const char *second)
{
    FILE *file;
    char line[512];
    uint8_t first_seen = 0u;

    if (path == NULL || first == NULL || second == NULL) {
        return 0;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        if (strstr(line, first) != NULL) {
            first_seen = 1u;
        }
        if (strstr(line, second) != NULL) {
            (void)fclose(file);
            return first_seen != 0u;
        }
    }
    (void)fclose(file);
    return 0;
}

static int bounded_suppression_normal_time(void)
{
    maintenance_fixture_t fixture;
    tavrn_maintenance_snapshot_t state;
    const tavrn_maintenance_counters_t *counters;
    uint32_t armed_at;

    if (!setup_adaptive_armed(&fixture, 0u, &armed_at) || armed_at != 20u ||
        tavrn_maintenance_observe_local_broadcast(&fixture.maintenance, 30u) !=
            TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_snapshot(&fixture.maintenance, &state) !=
            TAVRN_MAINTENANCE_OK ||
        state.current_interval_ms != 32u || state.next_hello_due_ms != 62u ||
        state.next_node_sequence != 0xffffu) {
        return 0;
    }
    if (tavrn_maintenance_observe_local_broadcast(&fixture.maintenance, 31u) !=
            TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_observe_local_broadcast(&fixture.maintenance, 61u) !=
            TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_observe_local_broadcast(&fixture.maintenance, 62u) !=
            TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_snapshot(&fixture.maintenance, &state) !=
            TAVRN_MAINTENANCE_OK) {
        return 0;
    }
    counters = tavrn_maintenance_counters(&fixture.maintenance);
    if (state.current_interval_ms != 32u || state.next_hello_due_ms != 62u ||
        state.next_node_sequence != 0xffffu || state.pending != 0u ||
        queued_ordinary_hello_count(&fixture) != 0u || counters == NULL ||
        counters->local_broadcast_suppressed != 4u ||
        counters->interval_advanced != 1u ||
        tavrn_maintenance_tick(&fixture.maintenance, 61u) != TAVRN_MAINTENANCE_OK ||
        queued_ordinary_hello_count(&fixture) != 0u ||
        tavrn_maintenance_tick(&fixture.maintenance, 62u) != TAVRN_MAINTENANCE_OK ||
        !queued_hello_matches(sole_queued_ordinary_hello(&fixture), adva_a, 0xffffu) ||
        tavrn_maintenance_snapshot(&fixture.maintenance, &state) !=
            TAVRN_MAINTENANCE_OK ||
        state.current_interval_ms != 42u || state.next_hello_due_ms != 104u ||
        state.next_node_sequence != 0u) {
        return 0;
    }
    clear_queued_controls(&fixture);
    if (tavrn_maintenance_observe_local_broadcast(&fixture.maintenance, 63u) !=
            TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_observe_local_broadcast(&fixture.maintenance, 64u) !=
            TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_snapshot(&fixture.maintenance, &state) !=
            TAVRN_MAINTENANCE_OK) {
        return 0;
    }
    counters = tavrn_maintenance_counters(&fixture.maintenance);
    return state.current_interval_ms == 50u && state.next_hello_due_ms == 113u &&
        state.next_node_sequence == 0u && state.pending == 0u && counters != NULL &&
        counters->local_broadcast_suppressed == 6u &&
        counters->interval_advanced == 3u;
}

static int bounded_suppression_pending_busy(void)
{
    maintenance_fixture_t fixture;
    tavrn_maintenance_snapshot_t state;
    const tavrn_maintenance_counters_t *counters;
    uint32_t armed_at;

    if (!setup_adaptive_armed(&fixture, 0u, &armed_at) || armed_at != 20u ||
        tavrn_maintenance_observe_local_broadcast(&fixture.maintenance, 30u) !=
            TAVRN_MAINTENANCE_OK ||
        !fill_control_queue(&fixture, 61u) ||
        tavrn_maintenance_tick(&fixture.maintenance, 62u) !=
            TAVRN_MAINTENANCE_BUSY ||
        tavrn_maintenance_snapshot(&fixture.maintenance, &state) !=
            TAVRN_MAINTENANCE_OK ||
        state.pending == 0u || state.next_hello_due_ms != 62u ||
        state.next_node_sequence != 0xffffu ||
        tavrn_maintenance_observe_local_broadcast(&fixture.maintenance, 63u) !=
            TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_snapshot(&fixture.maintenance, &state) !=
            TAVRN_MAINTENANCE_OK) {
        return 0;
    }
    counters = tavrn_maintenance_counters(&fixture.maintenance);
    if (state.pending != 0u || state.next_hello_due_ms != 62u ||
        state.current_interval_ms != 32u || state.next_node_sequence != 0xffffu ||
        counters == NULL || counters->local_broadcast_suppressed != 2u ||
        counters->interval_advanced != 1u) {
        return 0;
    }
    clear_queued_controls(&fixture);
    return tavrn_maintenance_tick(&fixture.maintenance, 64u) ==
            TAVRN_MAINTENANCE_OK &&
        queued_hello_matches(sole_queued_ordinary_hello(&fixture), adva_a, 0xffffu) &&
        tavrn_maintenance_snapshot(&fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK &&
        state.pending == 0u && state.next_node_sequence == 0u;
}

static int bounded_suppression_wraparound(void)
{
    maintenance_fixture_t fixture;
    tavrn_maintenance_snapshot_t state;
    const tavrn_maintenance_counters_t *counters;
    uint32_t armed_at;

    if (!setup_adaptive_armed(&fixture, 0xffffffe0u, &armed_at) ||
        armed_at != 0xfffffff4u ||
        tavrn_maintenance_observe_local_broadcast(&fixture.maintenance,
                                                   0xfffffffau) !=
            TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_observe_local_broadcast(&fixture.maintenance,
                                                   0xffffffffu) !=
            TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_observe_local_broadcast(&fixture.maintenance, 0u) !=
            TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_observe_local_broadcast(&fixture.maintenance, 0x19u) !=
            TAVRN_MAINTENANCE_OK ||
        tavrn_maintenance_snapshot(&fixture.maintenance, &state) !=
            TAVRN_MAINTENANCE_OK) {
        return 0;
    }
    counters = tavrn_maintenance_counters(&fixture.maintenance);
    return state.current_interval_ms == 32u && state.next_hello_due_ms == 0x1au &&
        state.next_node_sequence == 0xffffu && counters != NULL &&
        counters->local_broadcast_suppressed == 4u &&
        counters->interval_advanced == 1u &&
        tavrn_maintenance_tick(&fixture.maintenance, 0x19u) ==
            TAVRN_MAINTENANCE_OK &&
        queued_ordinary_hello_count(&fixture) == 0u &&
        tavrn_maintenance_tick(&fixture.maintenance, 0x1au) ==
            TAVRN_MAINTENANCE_OK &&
        queued_hello_matches(sole_queued_ordinary_hello(&fixture), adva_a, 0xffffu);
}

static int test_maint_02_adaptive_contract(void)
{
    maintenance_fixture_t fixture;
    maintenance_fixture_t busy_fixture;
    maintenance_fixture_t suppressed_busy_fixture;
    maintenance_fixture_t topology_fixture;
    maintenance_fixture_t saturation_fixture;
    tavrn_maintenance_config_t config = make_adaptive_maintenance_config();
    tavrn_maintenance_snapshot_t before_rx;
    tavrn_maintenance_snapshot_t state;
    const tavrn_maintenance_counters_t *counters;
    tavrn_validated_control_t remote = make_sid8_ordinary_hello(adva_b, 7u);
    uint32_t armed_at;
    uint32_t due;
    uint32_t stable_due;
    uint32_t topology_at;
    uint32_t topology_unchanged_before;
    uint32_t liveness_decay_before;
    uint8_t iteration;
    int ok = 1;

    ok &= adaptive_config_contract_is_validated();
    ok &= bounded_suppression_normal_time();
    ok &= bounded_suppression_pending_busy();
    ok &= bounded_suppression_wraparound();
    ok &= setup_adaptive_armed(&fixture, 0xffffffe0u, &armed_at) &&
        armed_at == 0xfffffff4u &&
        tavrn_maintenance_snapshot(&fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK && state.current_interval_ms == 20u &&
        state.next_hello_due_ms == 0x00000008u &&
        state.next_topology_sample_ms == 0xfffffff5u &&
        state.direct_one_hop_count == 1u && state.liveness_floor_ms == 0u &&
        state.liveness_timeout_ms == 60u;

    ok &= setup_adaptive_armed(&fixture, 0u, &armed_at);
    due = armed_at + config.hello_change_ms;
    for (iteration = 0u; iteration < 16u &&
                        config.hello_change_ms != config.hello_stable_ms;
         iteration++) {
        uint32_t expected = adaptive_ema_step(
            iteration == 0u ? config.hello_change_ms : state.current_interval_ms,
            &config);

        ok &= tavrn_maintenance_tick(&fixture.maintenance, due) ==
            TAVRN_MAINTENANCE_OK;
        ok &= tavrn_maintenance_snapshot(&fixture.maintenance, &state) ==
                TAVRN_MAINTENANCE_OK &&
            state.current_interval_ms == expected &&
            state.next_hello_due_ms == due + expected;
        clear_queued_controls(&fixture);
        due += expected;
        if (expected == config.hello_stable_ms) {
            break;
        }
    }
    counters = tavrn_maintenance_counters(&fixture.maintenance);
    ok &= state.current_interval_ms == config.hello_stable_ms && counters != NULL &&
        counters->interval_advanced == iteration + 1u &&
        counters->interval_snapped == 1u;

    ok &= setup_adaptive_armed(&busy_fixture, 0u, &armed_at);
    due = armed_at + config.hello_change_ms;
    ok &= fill_control_queue(&busy_fixture, due - 1u) &&
        tavrn_maintenance_tick(&busy_fixture.maintenance, due) ==
            TAVRN_MAINTENANCE_BUSY &&
        tavrn_maintenance_snapshot(&busy_fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK && state.pending != 0u &&
        state.current_interval_ms == config.hello_change_ms &&
        state.next_hello_due_ms == due && state.next_node_sequence == 0xffffu &&
        tavrn_maintenance_counters(&busy_fixture.maintenance)->interval_advanced == 0u;
    clear_queued_controls(&busy_fixture);
    ok &= tavrn_maintenance_tick(&busy_fixture.maintenance, due + 1u) ==
            TAVRN_MAINTENANCE_OK &&
        tavrn_maintenance_snapshot(&busy_fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK && state.pending == 0u &&
        state.current_interval_ms == 32u &&
        state.next_hello_due_ms == due + 1u + 32u &&
        state.next_node_sequence == 0u;

    ok &= setup_adaptive_armed(&suppressed_busy_fixture, 0u, &armed_at);
    due = armed_at + config.hello_change_ms;
    ok &= fill_control_queue(&suppressed_busy_fixture, due - 1u) &&
        tavrn_maintenance_tick(&suppressed_busy_fixture.maintenance, due) ==
            TAVRN_MAINTENANCE_BUSY &&
        tavrn_maintenance_observe_local_broadcast(
            &suppressed_busy_fixture.maintenance, due + 1u) ==
            TAVRN_MAINTENANCE_OK &&
        tavrn_maintenance_snapshot(&suppressed_busy_fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK &&
        state.pending == 0u && state.next_node_sequence == 0xffffu &&
        state.current_interval_ms == 32u &&
        state.next_hello_due_ms == due + 1u + 32u &&
        tavrn_maintenance_counters(&suppressed_busy_fixture.maintenance)
                ->local_broadcast_suppressed == 1u;
    clear_queued_controls(&suppressed_busy_fixture);
    ok &= tavrn_maintenance_tick(&suppressed_busy_fixture.maintenance,
                                  due + 2u) == TAVRN_MAINTENANCE_OK &&
        queued_ordinary_hello_count(&suppressed_busy_fixture) == 0u;

    ok &= setup_adaptive_armed(&fixture, 0u, &armed_at);
    due = armed_at + config.hello_change_ms;
    ok &= tavrn_maintenance_observe_local_broadcast(&fixture.maintenance,
                                                     armed_at + 10u) ==
            TAVRN_MAINTENANCE_OK &&
        tavrn_maintenance_snapshot(&fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK && state.current_interval_ms == 32u &&
        state.next_hello_due_ms == armed_at + 42u &&
        state.next_node_sequence == 0xffffu &&
        tavrn_maintenance_counters(&fixture.maintenance)->local_broadcast_suppressed ==
            1u && tavrn_maintenance_tick(&fixture.maintenance, due) ==
            TAVRN_MAINTENANCE_OK &&
        queued_ordinary_hello_count(&fixture) == 0u &&
        tavrn_maintenance_tick(&fixture.maintenance, armed_at + 42u) ==
            TAVRN_MAINTENANCE_OK &&
        tavrn_maintenance_snapshot(&fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK && state.current_interval_ms == 42u &&
        state.next_hello_due_ms == armed_at + 84u &&
        tavrn_maintenance_counters(&fixture.maintenance)->local_broadcast_suppressed ==
            1u;
    clear_queued_controls(&fixture);
    before_rx = state;
    ok &= deliver_control(&fixture, &remote, adva_b, armed_at + 43u, 0u) ==
            TAVRN_MAINTENANCE_RX_UNIQUE &&
        tavrn_maintenance_snapshot(&fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK &&
        memcmp(&state, &before_rx, sizeof(state)) == 0 &&
        tavrn_maintenance_counters(&fixture.maintenance)->rx_unique == 1u &&
        tavrn_maintenance_counters(&fixture.maintenance)->interval_advanced == 2u;

    ok &= setup_adaptive_armed(&topology_fixture, 0u, &armed_at) &&
        advance_to_stable(&topology_fixture, armed_at, &stable_due);
    topology_at = stable_due + 1u;
    ok &= observe_member_with_hop(&topology_fixture, adva_c, 1u, 1u,
                                  TAVRN_GTT_EVIDENCE_LIVENESS, topology_at) &&
        observe_member_with_hop(&topology_fixture, adva_d, 1u, 2u,
                                TAVRN_GTT_EVIDENCE_LIVENESS, topology_at) &&
        tavrn_maintenance_tick(&topology_fixture.maintenance, topology_at) ==
            TAVRN_MAINTENANCE_OK &&
        tavrn_maintenance_snapshot(&topology_fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK && state.direct_one_hop_count == 2u &&
        state.current_interval_ms == 20u &&
        state.next_hello_due_ms == topology_at + 20u &&
        state.liveness_timeout_ms == 240u && state.liveness_floor_ms == 240u &&
        tavrn_maintenance_counters(&topology_fixture.maintenance)->topology_reset ==
            1u;
    clear_queued_controls(&topology_fixture);
    topology_unchanged_before =
        tavrn_maintenance_counters(&topology_fixture.maintenance)->topology_unchanged;
    liveness_decay_before =
        tavrn_maintenance_counters(&topology_fixture.maintenance)->liveness_floor_decayed;
    for (iteration = 0u; iteration < 5u; iteration++) {
        static const uint32_t expected_timeout[] = { 240u, 150u, 105u, 83u, 72u };
        static const uint32_t expected_floor[] = { 150u, 105u, 83u, 72u, 66u };
        uint32_t sample_at = topology_at + (uint32_t)iteration + 1u;

        ok &= tavrn_maintenance_tick(&topology_fixture.maintenance, sample_at) ==
            TAVRN_MAINTENANCE_OK;
        ok &= tavrn_maintenance_snapshot(&topology_fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK &&
            state.current_interval_ms == 20u &&
            state.liveness_timeout_ms == expected_timeout[iteration] &&
            state.liveness_floor_ms == expected_floor[iteration] &&
            ble_mesh_tx_queue_count(&topology_fixture.scheduler.routed_tx_queue) == 0u;
    }
    ok &= tavrn_maintenance_counters(&topology_fixture.maintenance)->topology_unchanged ==
            topology_unchanged_before + 5u &&
        tavrn_maintenance_counters(&topology_fixture.maintenance)->liveness_floor_decayed ==
            liveness_decay_before + 5u;
    ok &= observe_member_with_hop(&topology_fixture, adva_c, 2u, 1u,
                                  TAVRN_GTT_EVIDENCE_DEPARTED,
                                  topology_at + 6u) &&
        tavrn_maintenance_tick(&topology_fixture.maintenance, topology_at + 6u) ==
            TAVRN_MAINTENANCE_OK &&
        tavrn_maintenance_snapshot(&topology_fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK && state.direct_one_hop_count == 1u &&
        state.current_interval_ms == 20u && state.liveness_floor_ms == 66u &&
        state.liveness_timeout_ms == 66u &&
        tavrn_maintenance_counters(&topology_fixture.maintenance)->topology_reset ==
            2u;

    config.hello_change_ms = 0x7ffffffeu;
    config.hello_stable_ms = 0x7ffffffeu;
    config.topology_sample_ms = 1u;
    ok &= setup_adaptive_armed_with_config(&saturation_fixture, &config, 0u,
                                           &armed_at) &&
        observe_member_with_hop(&saturation_fixture, adva_c, 1u, 1u,
                                TAVRN_GTT_EVIDENCE_LIVENESS, armed_at + 1u) &&
        tavrn_maintenance_tick(&saturation_fixture.maintenance, armed_at + 1u) ==
            TAVRN_MAINTENANCE_OK &&
        tavrn_maintenance_snapshot(&saturation_fixture.maintenance, &state) ==
            TAVRN_MAINTENANCE_OK && state.liveness_floor_ms == UINT32_MAX &&
        state.liveness_timeout_ms == UINT32_MAX;

    /* The FULL binding is the common observation sink after router and
     * mentorship work, before activation and owner post-tick processing. */
    ok &= source_occurrences(TAVRN_PHASE5_FULL_BINDING_SOURCE_PATH,
                               "tavrn_maintenance_observe_local_broadcast(") == 1u &&
        source_lines_are_ordered(
            TAVRN_PHASE5_FULL_BINDING_SOURCE_PATH,
            "result_out->router_status = tavrn_router_tick_ex",
            "result_out->mentorship_status = tavrn_mentorship_tick") &&
        source_lines_are_ordered(
            TAVRN_PHASE5_FULL_BINDING_SOURCE_PATH,
            "result_out->mentorship_status = tavrn_mentorship_tick",
            "result_out->broadcast_snapshot_status = tavrn_router_local_broadcast_snapshot") &&
        source_lines_are_ordered(
            TAVRN_PHASE5_FULL_BINDING_SOURCE_PATH,
            "result_out->broadcast_snapshot_status = tavrn_router_local_broadcast_snapshot",
            "result_out->broadcast_observation_status = tavrn_maintenance_observe_local_broadcast") &&
        source_lines_are_ordered(
            TAVRN_PHASE5_FULL_BINDING_SOURCE_PATH,
            "result_out->broadcast_observation_status = tavrn_maintenance_observe_local_broadcast",
            "result_out->activation_status = tavrn_maintenance_activate") &&
        source_lines_are_ordered(
            TAVRN_PHASE5_FULL_BINDING_SOURCE_PATH,
            "result_out->activation_status = tavrn_maintenance_activate",
            "result_out->post_tick = tavrn_maintenance_owner_post_tick") &&
        source_occurrences(TAVRN_PHASE5_ROUTED_MAIN_PATH,
                           "maintenance_config.topology_sample_ms = "
                           "tron_timer_config.gtt_maintenance_ms;") == 1u;
    return ok;
}

#endif /* TAVRN_MAINTENANCE_ADAPTIVE_CONTRACT */

int main(void)
{
#if defined(TAVRN_MAINTENANCE_ADAPTIVE_CONTRACT)
    /* Keep the frozen fixed-cadence cases compiled in the adaptive binary;
     * adaptive modes intentionally execute only their one MAINT-02 gate. */
    if (0) {
        (void)test_maint_02_arm_cadence_wire_and_busy();
        (void)test_gtt_03_unique_sid8_hello_is_passive();
        (void)test_serial_03_hello_equality_dedupe();
        (void)test_bearer_04_rx_rejects_without_feedback();
    }
    CHECK("MAINT-02", test_maint_02_adaptive_contract());
    if (failures != 0u) {
        printf("tavrn_phase5_maintenance ADAPTIVE RED tests failed: %u assertion(s)\n",
               failures);
        return 1;
    }
    printf("tavrn_phase5_maintenance adaptive tests passed\n");
    return 0;
#else
    CHECK("MAINT-02", test_maint_02_arm_cadence_wire_and_busy());
    CHECK("GTT-03", test_gtt_03_unique_sid8_hello_is_passive());
    CHECK("SERIAL-03", test_serial_03_hello_equality_dedupe());
    CHECK("BEARER-04", test_bearer_04_rx_rejects_without_feedback());
    if (failures != 0u) {
        printf("tavrn_phase5_maintenance RED tests failed: %u assertion(s)\n",
               failures);
        return 1;
    }
    printf("tavrn_phase5_maintenance tests passed\n");
    return 0;
#endif
}
