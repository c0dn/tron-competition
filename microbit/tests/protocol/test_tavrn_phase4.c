#include "aodv_core.h"
#include "ble_mesh_scheduler.h"
#include "tavrn_full.h"
#include "tavrn_gtt.h"
#include "tavrn_link_v2.h"
#include "tavrn_phase4_esc_mentor_contract.h"
#include "tavrn_router.h"
#include "tavrn_wire_v2.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_NETWORK_ID 0x2au

static unsigned int failures;
static const char *reported[9];
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
/* B and these identities share SID8 only; all full identities are valid. */
static const uint8_t adva_b_sid8_tombstone[TAVRN_ADVA_LEN] = {
    0xdcu, 0x88u, 0x4au, 0x61u, 0x50u, 0xc3u,
};
static const uint8_t adva_b_sid8_collision[TAVRN_ADVA_LEN] = {
    0xdcu, 0x99u, 0x7au, 0x62u, 0x51u, 0xc4u,
};

typedef struct phase4_fixture {
    ble_mesh_scheduler_t scheduler;
    tavrn_link_v2_t link;
    aodv_core_t aodv;
    tavrn_gtt_t gtt;
    tavrn_gtt_storage_t gtt_storage;
    tavrn_full_t full;
    tavrn_router_t router;
    tavrn_mentorship_t mentorship;
} phase4_fixture_t;

typedef struct phase4_bystander_snapshot {
    tavrn_mentorship_state_snapshot_t state;
    tavrn_mentorship_counters_t mentorship_counters;
    tavrn_gtt_storage_t gtt_storage;
    tavrn_gtt_counters_t gtt_counters;
    uint8_t queued_controls;
} phase4_bystander_snapshot_t;

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

static int adva_equal(const tavrn_adva_t *left,
                      const uint8_t right[TAVRN_ADVA_LEN])
{
    return left != NULL && memcmp(left->bytes, right, TAVRN_ADVA_LEN) == 0;
}

static tavrn_link_config_t make_link_config(
    const uint8_t local_adva[TAVRN_ADVA_LEN])
{
    tavrn_link_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_peer(local_adva, TAVRN_IDENTITY_SID16);
    config.network_id = TEST_NETWORK_ID;
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

static aodv_core_config_t make_aodv_config(
    const uint8_t local_adva[TAVRN_ADVA_LEN])
{
    aodv_core_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_peer(local_adva, TAVRN_IDENTITY_SID16);
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
    config.rerr_dedupe_ms = 250u;
    config.rrep_ack_wait_ms = 250u;
    return config;
}

static tavrn_mentorship_config_t make_mentorship_config(void)
{
    tavrn_mentorship_config_t config;

    memset(&config, 0, sizeof(config));
    config.offer_window_ms = 20u;
    config.page_timeout_ms = 30u;
    config.self_bootstrap_ms = 50u;
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

static tavrn_router_delivery_status_t reserve_delivery(
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

static tavrn_router_delivery_status_t complete_delivery(
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

static int setup_fixture_for_local(phase4_fixture_t *fixture,
                                   const uint8_t local_adva[TAVRN_ADVA_LEN],
                                   uint32_t now_ms)
{
    tavrn_link_config_t link_config = make_link_config(local_adva);
    aodv_core_config_t aodv_config = make_aodv_config(local_adva);
    tavrn_gtt_config_t gtt_config;
    tavrn_router_incarnation_config_t incarnation_config;
    tavrn_router_augmentation_hooks_t full_hooks;
    tavrn_router_application_hooks_t application_hooks;
    tavrn_mentorship_config_t mentorship_config = make_mentorship_config();

    if (fixture == NULL || local_adva == NULL) {
        return 0;
    }
    memset(fixture, 0, sizeof(*fixture));
    memset(&gtt_config, 0, sizeof(gtt_config));
    gtt_config.local_identity = make_adva(local_adva);
    gtt_config.soft_expiry_ms = 50u;
    gtt_config.hard_expiry_ms = 100u;
    gtt_config.departed_retention_ms = 200u;
    memset(&incarnation_config, 0, sizeof(incarnation_config));
    incarnation_config.feature_level = TAVRN_ROUTER_FEATURE_FULL_TAVRN;
    incarnation_config.boot_nonce = 0x5a01u;
    incarnation_config.reboot_announce_ms = 30u;
    ble_mesh_scheduler_init(&fixture->scheduler, now_ms, local_adva);
    if (tavrn_link_v2_init(&fixture->link, &fixture->scheduler, &link_config,
                           now_ms) != TAVRN_LINK_INIT_OK ||
        aodv_core_init(&fixture->aodv, &aodv_config, now_ms) != AODV_INIT_OK ||
        tavrn_gtt_init(&fixture->gtt, &fixture->gtt_storage, &gtt_config,
                       now_ms) != TAVRN_GTT_INIT_OK ||
        tavrn_full_init(&fixture->full, &fixture->gtt) != TAVRN_FULL_INIT_OK) {
        return 0;
    }
    full_hooks = tavrn_full_router_hooks(&fixture->full);
    memset(&application_hooks, 0, sizeof(application_hooks));
    application_hooks.reserve = reserve_delivery;
    application_hooks.commit = complete_delivery;
    application_hooks.cancel = complete_delivery;
    return tavrn_router_init_with_incarnation(&fixture->router, &fixture->link,
                                              &fixture->aodv, &full_hooks,
                                              &incarnation_config, now_ms) ==
               TAVRN_ROUTER_INCARNATION_OK &&
        tavrn_router_set_application_hooks(&fixture->router, &application_hooks) ==
            TAVRN_ROUTER_APPLICATION_HOOK_OK &&
        tavrn_mentorship_init(&fixture->mentorship, &fixture->router,
                              &fixture->gtt, &mentorship_config, now_ms) ==
            TAVRN_MENTORSHIP_OK;
}

static int setup_fixture(phase4_fixture_t *fixture, uint32_t now_ms)
{
    return setup_fixture_for_local(fixture, adva_a, now_ms);
}

static int remove_queued_control(phase4_fixture_t *fixture,
                                 tavrn_wire_type_t type);

static int begin_sync_page_wait(phase4_fixture_t *fixture,
                                const tavrn_mentorship_offer_t *offer,
                                uint32_t offer_received_at_ms)
{
    uint32_t selection_at_ms;

    if (fixture == NULL || offer == NULL) {
        return 0;
    }
    selection_at_ms = offer_received_at_ms + fixture->mentorship.config.offer_window_ms;
    return tavrn_mentorship_collect_offer(&fixture->mentorship, offer,
                                          offer_received_at_ms) ==
               TAVRN_MENTORSHIP_OFFER_ACCEPTED &&
        tavrn_mentorship_tick(&fixture->mentorship, selection_at_ms) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&fixture->mentorship, selection_at_ms + 1u) ==
            TAVRN_MENTORSHIP_OK &&
        fixture->mentorship.page_deadline_valid != 0u &&
        remove_queued_control(fixture, TAVRN_WIRE_SYNC_PULL);
}

static int observe_member(phase4_fixture_t *fixture,
                          const uint8_t identity[TAVRN_ADVA_LEN], uint16_t serial,
                          uint8_t hop_count, tavrn_gtt_evidence_kind_t kind,
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

/* B stays active at t=150; C is retained hard-expired; D is a retained
 * departed tombstone.  This is intentionally not tavrn_gtt_enumerate_active(). */
static int seed_retained_records(phase4_fixture_t *fixture)
{
    return observe_member(fixture, adva_c, 0x0101u, 3u,
                          TAVRN_GTT_EVIDENCE_LIVENESS, 0u) &&
        observe_member(fixture, adva_b, 0x0102u, 1u,
                       TAVRN_GTT_EVIDENCE_LIVENESS, 110u) &&
        observe_member(fixture, adva_d, 0x0103u, 2u,
                       TAVRN_GTT_EVIDENCE_DEPARTED, 120u);
}

static tavrn_codec_config_t make_codec_config(tavrn_identity_width_t width)
{
    tavrn_codec_config_t config;

    memset(&config, 0, sizeof(config));
    config.network_id = TEST_NETWORK_ID;
    config.local_peer = make_peer(adva_a, width);
    return config;
}

static void pdu_put_u16(tavrn_validated_control_t *control, uint8_t offset,
                        uint16_t value)
{
    control->pdu[offset] = (uint8_t)value;
    control->pdu[(uint8_t)(offset + 1u)] = (uint8_t)(value >> 8);
}

static tavrn_validated_control_t make_n1_hello(
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

static tavrn_validated_control_t make_sync_data(
    const uint8_t mentee[TAVRN_ADVA_LEN], uint16_t snapshot_id, uint8_t index,
    const uint8_t identity_or_null[TAVRN_ADVA_LEN], uint16_t serial,
    uint8_t ttl_state_hops, uint8_t last)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_SYNC_DATA;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = TEST_NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_SYNC_DATA;
    control.pdu[5] = last != 0u ? 0x40u : 0u;
    memcpy(&control.pdu[6], mentee, TAVRN_ADVA_LEN);
    pdu_put_u16(&control, 12u, snapshot_id);
    control.pdu[14] = index;
    if (identity_or_null == NULL) {
        control.pdu_len = 15u;
        return control;
    }
    control.pdu[5] |= 0x80u;
    memcpy(&control.pdu[15], identity_or_null, TAVRN_ADVA_LEN);
    pdu_put_u16(&control, 21u, serial);
    control.pdu[23] = ttl_state_hops;
    control.pdu_len = 24u;
    return control;
}

static tavrn_validated_control_t make_sid8_rreq(uint8_t origin, uint8_t destination,
                                                uint16_t request_id)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_E_RREQ;
    control.pdu_len = 15u;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = TEST_NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_E_RREQ;
    control.pdu[5] = 0x80u;
    control.pdu[6] = 0x10u;
    control.pdu[7] = origin;
    pdu_put_u16(&control, 8u, request_id);
    control.pdu[10] = destination;
    pdu_put_u16(&control, 11u, 0u);
    pdu_put_u16(&control, 13u, 1u);
    return control;
}

static tavrn_validated_control_t make_sid8_rrep(uint8_t receiver,
                                                uint8_t destination,
                                                uint8_t origin,
                                                uint16_t request_id)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_E_RREP;
    control.pdu_len = 16u;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = TEST_NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_E_RREP;
    control.pdu[5] = 0x80u;
    control.pdu[6] = 0x10u;
    control.pdu[7] = receiver;
    control.pdu[8] = destination;
    pdu_put_u16(&control, 9u, 1u);
    control.pdu[11] = origin;
    pdu_put_u16(&control, 12u, request_id);
    pdu_put_u16(&control, 14u, 10u);
    return control;
}

static tavrn_validated_control_t make_join(
    const uint8_t origin[TAVRN_ADVA_LEN], const uint8_t subject[TAVRN_ADVA_LEN],
    uint16_t sequence)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_TC_UPDATE;
    control.pdu_len = 24u;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = TEST_NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_TC_UPDATE;
    control.pdu[6] = 0xf0u;
    memcpy(&control.pdu[7], origin, TAVRN_ADVA_LEN);
    pdu_put_u16(&control, 13u, sequence);
    memcpy(&control.pdu[15], subject, TAVRN_ADVA_LEN);
    control.pdu[21] = 0u;
    pdu_put_u16(&control, 22u, 1u);
    return control;
}

static int wrap_control(const tavrn_validated_control_t *control,
                        const uint8_t transmitter[TAVRN_ADVA_LEN],
                        tavrn_identity_width_t width,
                        uint8_t adv_data[BLE_ADV_MAX_DATA], uint8_t *adv_len_out)
{
    tavrn_decoded_frame_t frame;
    tavrn_codec_config_t config = make_codec_config(width);
    size_t adv_len = 0u;

    if (control == NULL || transmitter == NULL || adv_data == NULL ||
        adv_len_out == NULL) {
        return 0;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = control->type;
    frame.network_id = TEST_NETWORK_ID;
    frame.transmitter = make_peer(transmitter, width);
    frame.detail.control = *control;
    if (tavrn_wire_v2_encode(&config, &frame, adv_data, BLE_ADV_MAX_DATA,
                             &adv_len) != TAVRN_CODEC_OK ||
        adv_len > BLE_ADV_MAX_DATA) {
        return 0;
    }
    *adv_len_out = (uint8_t)adv_len;
    return 1;
}

static tavrn_mentorship_status_t deliver_control(
    phase4_fixture_t *fixture, const tavrn_validated_control_t *control,
    const uint8_t transmitter[TAVRN_ADVA_LEN], tavrn_identity_width_t width,
    uint32_t now_ms, tavrn_mentorship_event_trace_t *trace_out)
{
    ble_mesh_sched_event_t event;

    if (fixture == NULL || control == NULL || transmitter == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
    memcpy(event.adv_addr, transmitter, TAVRN_ADVA_LEN);
    if (!wrap_control(control, transmitter, width, event.adv_data, &event.adv_len)) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    return tavrn_mentorship_handle_scheduler_event(&fixture->mentorship, &event,
                                                    now_ms, trace_out);
}

static tavrn_mentorship_status_t deliver_sid8_data(
    phase4_fixture_t *fixture, const uint8_t transmitter[TAVRN_ADVA_LEN],
    uint16_t sequence, uint32_t now_ms, tavrn_mentorship_event_trace_t *trace_out)
{
    tavrn_decoded_frame_t frame;
    tavrn_codec_config_t codec = make_codec_config(TAVRN_IDENTITY_SID8);
    ble_mesh_sched_event_t event;
    size_t encoded_len = 0u;

    if (fixture == NULL || transmitter == NULL) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = TEST_NETWORK_ID;
    frame.transmitter = make_peer(transmitter, TAVRN_IDENTITY_SID8);
    frame.detail.data.immediate_receiver =
        make_peer(adva_a, TAVRN_IDENTITY_SID8).logical_id;
    frame.detail.data.data.origin = make_peer(transmitter, TAVRN_IDENTITY_SID8).logical_id;
    frame.detail.data.data.final_destination =
        make_peer(adva_a, TAVRN_IDENTITY_SID8).logical_id;
    frame.detail.data.data.data_seq = sequence;
    frame.detail.data.data.ttl = 1u;
    frame.detail.data.data.app_kind = 0x7fu;
    frame.detail.data.data.app_len = 1u;
    frame.detail.data.data.app_bytes[0] = 0xa5u;
    frame.detail.data.data.ownership = TAVRN_DATA_TRANSIT;
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
    memcpy(event.adv_addr, transmitter, TAVRN_ADVA_LEN);
    if (tavrn_wire_v2_encode(&codec, &frame, event.adv_data, sizeof(event.adv_data),
                             &encoded_len) != TAVRN_CODEC_OK ||
        encoded_len > sizeof(event.adv_data)) {
        return TAVRN_MENTORSHIP_INVALID;
    }
    event.adv_len = (uint8_t)encoded_len;
    return tavrn_mentorship_handle_scheduler_event(&fixture->mentorship, &event,
                                                    now_ms, trace_out);
}

static unsigned int custody_count(const tavrn_link_v2_t *link)
{
    unsigned int count = 0u;
    uint8_t index;

    if (link == NULL) {
        return 0u;
    }
    for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
        if (link->custody[index].phase != TAVRN_CUSTODY_FREE) {
            count++;
        }
    }
    return count;
}

static unsigned int gtt_identity_count(const tavrn_gtt_storage_t *storage,
                                       const uint8_t identity[TAVRN_ADVA_LEN])
{
    unsigned int count = 0u;
    uint8_t index;

    if (storage == NULL || identity == NULL) {
        return 0u;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        if (storage->entries[index].occupied != 0u &&
            memcmp(storage->entries[index].identity.bytes, identity,
                   TAVRN_ADVA_LEN) == 0) {
            count++;
        }
    }
    return count;
}

static tavrn_gtt_entry_t *gtt_entry_for(tavrn_gtt_storage_t *storage,
                                         const uint8_t identity[TAVRN_ADVA_LEN])
{
    uint8_t index;

    if (storage == NULL || identity == NULL) {
        return NULL;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        tavrn_gtt_entry_t *entry = &storage->entries[index];

        if (entry->occupied != 0u &&
            memcmp(entry->identity.bytes, identity, TAVRN_ADVA_LEN) == 0) {
            return entry;
        }
    }
    return NULL;
}

static unsigned int completed_sync_count(const tavrn_mentorship_t *mentorship)
{
    unsigned int count = 0u;
    uint8_t index;

    if (mentorship == NULL) {
        return 0u;
    }
    for (index = 0u; index < TAVRN_MENTORSHIP_SYNC_DEDUPE_CAPACITY; index++) {
        if (mentorship->completed_sync[index].valid != 0u) {
            count++;
        }
    }
    return count;
}

static unsigned int queued_control_count(const phase4_fixture_t *fixture,
                                         tavrn_wire_type_t type)
{
    unsigned int count = 0u;
    uint8_t index;

    if (fixture == NULL) {
        return 0u;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry =
            &fixture->scheduler.routed_tx_queue.entries[index];

        if (entry->occupied != 0u && entry->item.adv_len >= 12u &&
            entry->item.adv_data[11] == (uint8_t)type) {
            count++;
        }
    }
    return count;
}

static unsigned int pending_join_obligation_count(
    const tavrn_mentorship_t *mentorship)
{
    unsigned int count = 0u;
    uint8_t index;

    if (mentorship == NULL) {
        return 0u;
    }
    for (index = 0u; index < TAVRN_MENTORSHIP_JOIN_OBLIGATION_CAPACITY; index++) {
        if (mentorship->join_obligations[index].valid != 0u) {
            count++;
        }
    }
    return count;
}

static int remove_queued_control(phase4_fixture_t *fixture,
                                 tavrn_wire_type_t type)
{
    uint8_t index;

    if (fixture == NULL) {
        return 0;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_queue_entry_t *entry =
            &fixture->scheduler.routed_tx_queue.entries[index];

        if (entry->occupied != 0u && entry->item.adv_len >= 12u &&
            entry->item.adv_data[11] == (uint8_t)type) {
            return ble_mesh_tx_queue_remove(&fixture->scheduler.routed_tx_queue,
                                            index, NULL);
        }
    }
    return 0;
}

static int join_key_present(const tavrn_mentorship_t *mentorship,
                            const uint8_t origin[TAVRN_ADVA_LEN],
                            uint16_t sequence)
{
    uint8_t index;

    if (mentorship == NULL || origin == NULL) {
        return 0;
    }
    for (index = 0u; index < TAVRN_MENTORSHIP_JOIN_DEDUPE_CAPACITY; index++) {
        const tavrn_mentorship_join_key_t *key = &mentorship->joins[index];

        if (key->occupied != 0u && key->sequence == sequence &&
            adva_equal(&key->origin, origin)) {
            return 1;
        }
    }
    return 0;
}

static int queued_sid8_rreq(const phase4_fixture_t *fixture)
{
    uint8_t index;

    if (fixture == NULL) {
        return 0;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry =
            &fixture->scheduler.routed_tx_queue.entries[index];

        if (entry->occupied != 0u && entry->item.adv_len >= 22u &&
            entry->item.adv_data[11] == TAVRN_WIRE_E_RREQ &&
            (entry->item.adv_data[12] & 0x80u) != 0u) {
            return 1;
        }
    }
    return 0;
}

static int transfer_queued_control(phase4_fixture_t *source,
                                    phase4_fixture_t *destination,
                                   tavrn_wire_type_t type, uint32_t now_ms,
                                   tavrn_mentorship_status_t *status_out)
{
    uint8_t index;

    if (status_out != NULL) {
        *status_out = TAVRN_MENTORSHIP_INVALID;
    }
    if (source == NULL || destination == NULL) {
        return 0;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_queue_entry_t *entry =
            &source->scheduler.routed_tx_queue.entries[index];
        ble_mesh_tx_item_t item;
        ble_mesh_sched_event_t event;

        if (entry->occupied == 0u || entry->item.adv_len < 12u ||
            entry->item.adv_data[11] != (uint8_t)type) {
            continue;
        }
        if (!ble_mesh_tx_queue_remove(&source->scheduler.routed_tx_queue, index,
                                      &item)) {
            return 0;
        }
        memset(&event, 0, sizeof(event));
        event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
        memcpy(event.adv_addr, source->link.config.local_peer.adva.bytes,
               TAVRN_ADVA_LEN);
        event.adv_len = item.adv_len;
        memcpy(event.adv_data, item.adv_data, item.adv_len);
        if (status_out != NULL) {
            *status_out = tavrn_mentorship_handle_scheduler_event(
                &destination->mentorship, &event, now_ms, NULL);
        } else {
            (void)tavrn_mentorship_handle_scheduler_event(
                &destination->mentorship, &event, now_ms, NULL);
        }
        return 1;
    }
    return 0;
}

/* Copy one actual link-enqueued advertisement to every listening board.  This
 * deliberately avoids rebuilding a control at the test boundary, so the
 * broadcast assertions cover production queue -> codec -> link -> router. */
static int broadcast_queued_control(phase4_fixture_t *source,
                                    phase4_fixture_t *intended,
                                    phase4_fixture_t *bystander,
                                    tavrn_wire_type_t type, uint32_t now_ms,
                                    tavrn_mentorship_status_t *intended_status_out,
                                    tavrn_mentorship_status_t *bystander_status_out)
{
    uint8_t index;

    if (intended_status_out != NULL) {
        *intended_status_out = TAVRN_MENTORSHIP_INVALID;
    }
    if (bystander_status_out != NULL) {
        *bystander_status_out = TAVRN_MENTORSHIP_INVALID;
    }
    if (source == NULL || intended == NULL || bystander == NULL) {
        return 0;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_queue_entry_t *entry =
            &source->scheduler.routed_tx_queue.entries[index];
        ble_mesh_tx_item_t item;
        ble_mesh_sched_event_t event;

        if (entry->occupied == 0u || entry->item.adv_len < 12u ||
            entry->item.adv_data[11] != (uint8_t)type ||
            !ble_mesh_tx_queue_remove(&source->scheduler.routed_tx_queue, index,
                                      &item)) {
            continue;
        }
        memset(&event, 0, sizeof(event));
        event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
        memcpy(event.adv_addr, source->link.config.local_peer.adva.bytes,
               TAVRN_ADVA_LEN);
        event.adv_len = item.adv_len;
        memcpy(event.adv_data, item.adv_data, item.adv_len);
        if (intended_status_out != NULL) {
            *intended_status_out = tavrn_mentorship_handle_scheduler_event(
                &intended->mentorship, &event, now_ms, NULL);
        } else {
            (void)tavrn_mentorship_handle_scheduler_event(&intended->mentorship,
                                                           &event, now_ms, NULL);
        }
        if (bystander_status_out != NULL) {
            *bystander_status_out = tavrn_mentorship_handle_scheduler_event(
                &bystander->mentorship, &event, now_ms, NULL);
        } else {
            (void)tavrn_mentorship_handle_scheduler_event(&bystander->mentorship,
                                                           &event, now_ms, NULL);
        }
        return 1;
    }
    return 0;
}

static int capture_bystander(const phase4_fixture_t *fixture,
                             phase4_bystander_snapshot_t *snapshot_out)
{
    if (fixture == NULL || snapshot_out == NULL ||
        tavrn_mentorship_state_snapshot(&fixture->mentorship,
                                        &snapshot_out->state) !=
            TAVRN_MENTORSHIP_OK) {
        return 0;
    }
    snapshot_out->mentorship_counters = *tavrn_mentorship_counters(
        &fixture->mentorship);
    snapshot_out->gtt_storage = fixture->gtt_storage;
    snapshot_out->gtt_counters = *tavrn_gtt_counters(&fixture->gtt);
    snapshot_out->queued_controls = ble_mesh_tx_queue_count(
        &fixture->scheduler.routed_tx_queue);
    return 1;
}

static int bystander_is_unchanged(const phase4_fixture_t *fixture,
                                  const phase4_bystander_snapshot_t *before)
{
    tavrn_mentorship_state_snapshot_t state;

    return fixture != NULL && before != NULL &&
        tavrn_mentorship_state_snapshot(&fixture->mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        memcmp(&state, &before->state, sizeof(state)) == 0 &&
        memcmp(tavrn_mentorship_counters(&fixture->mentorship),
               &before->mentorship_counters,
               sizeof(before->mentorship_counters)) == 0 &&
        memcmp(&fixture->gtt_storage, &before->gtt_storage,
               sizeof(before->gtt_storage)) == 0 &&
        memcmp(tavrn_gtt_counters(&fixture->gtt), &before->gtt_counters,
               sizeof(before->gtt_counters)) == 0 &&
        ble_mesh_tx_queue_count(&fixture->scheduler.routed_tx_queue) ==
            before->queued_controls;
}

static int fill_control_queue(phase4_fixture_t *fixture, uint32_t now_ms)
{
    uint8_t index;

    if (fixture == NULL) {
        return 0;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        tavrn_validated_control_t hello = make_n1_hello(
            fixture->link.config.local_peer.adva.bytes, (uint16_t)(0x8100u + index));
        tavrn_link_event_t outcome;

        memset(&outcome, 0, sizeof(outcome));
        if (tavrn_link_v2_send_control(&fixture->link, &hello, NULL, 0u, now_ms,
                                       &outcome) != TAVRN_LINK_SEND_OK) {
            break;
        }
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (fixture->scheduler.routed_tx_queue.entries[index].occupied == 0u) {
            return 0;
        }
    }
    return 1;
}

static int snapshot_identities(const tavrn_mentorship_snapshot_data_t *snapshot,
                               const uint8_t first[TAVRN_ADVA_LEN],
                               const uint8_t second[TAVRN_ADVA_LEN],
                               const uint8_t third[TAVRN_ADVA_LEN],
                               const uint8_t fourth[TAVRN_ADVA_LEN])
{
    return snapshot != NULL && snapshot->count == 4u &&
        adva_equal(&snapshot->records[0].identity, first) &&
        adva_equal(&snapshot->records[1].identity, second) &&
        adva_equal(&snapshot->records[2].identity, third) &&
        adva_equal(&snapshot->records[3].identity, fourth);
}

static int test_esc_01_fixed_k_and_retained_snapshot(void)
{
    phase4_fixture_t fixture;
    tavrn_mentorship_snapshot_data_t snapshot;
    int ok = 1;

    ok &= TAVRN_ESC_FIXED_K == 1u;
    ok &= setup_fixture(&fixture, 0u);
    ok &= seed_retained_records(&fixture);
    memset(&snapshot, 0, sizeof(snapshot));
    ok &= tavrn_mentorship_freeze_snapshot(&fixture.mentorship, 0x4401u, 150u,
                                            &snapshot) == TAVRN_MENTORSHIP_OK;
    ok &= snapshot.snapshot_id == 0x4401u &&
        snapshot_identities(&snapshot, adva_c, adva_a, adva_d, adva_b) &&
        snapshot.records[0].departed == 0u && snapshot.records[0].ttl_bucket == 1u &&
        snapshot.records[2].departed != 0u && snapshot.records[2].ttl_bucket == 0u &&
        snapshot.records[3].serial == 0x0102u;
    return ok;
}

static int test_esc_02_context_fail_closed(void)
{
    phase4_fixture_t fixture;
    tavrn_esc_context_match_t match;
    tavrn_mentorship_state_snapshot_t state;
    int ok = 1;

    ok &= setup_fixture(&fixture, 0u);
    ok &= observe_member(&fixture, adva_b, 1u, 1u,
                         TAVRN_GTT_EVIDENCE_LIVENESS, 100u);
    ok &= observe_member(&fixture, adva_c, 2u, 1u,
                         TAVRN_GTT_EVIDENCE_LIVENESS, 0u);
    ok &= observe_member(&fixture, adva_b_sid8_tombstone, 2u, 1u,
                         TAVRN_GTT_EVIDENCE_DEPARTED, 110u);
    memset(&match, 0, sizeof(match));
    ok &= tavrn_mentorship_resolve_sid8(&fixture.mentorship, 0x00u, 120u,
                                        &match) == TAVRN_ESC_CONTEXT_RESERVED;
    ok &= tavrn_mentorship_resolve_sid8(&fixture.mentorship, 0xffu, 120u,
                                        &match) == TAVRN_ESC_CONTEXT_RESERVED;
    ok &= tavrn_mentorship_resolve_sid8(&fixture.mentorship, 0x7eu, 120u,
                                        &match) == TAVRN_ESC_CONTEXT_UNKNOWN;
    ok &= tavrn_mentorship_resolve_sid8(&fixture.mentorship, adva_b[0], 120u,
                                        &match) == TAVRN_ESC_CONTEXT_UNIQUE &&
        adva_equal(&match.identity, adva_b);
    ok &= tavrn_mentorship_resolve_sid8(&fixture.mentorship, adva_c[0], 120u,
                                        &match) == TAVRN_ESC_CONTEXT_UNIQUE &&
        adva_equal(&match.identity, adva_c);
    ok &= observe_member(&fixture, adva_b_sid8_collision, 3u, 1u,
                         TAVRN_GTT_EVIDENCE_LIVENESS, 120u);
    ok &= tavrn_mentorship_resolve_sid8(&fixture.mentorship, adva_b[0], 121u,
                                        &match) == TAVRN_ESC_CONTEXT_COLLIDING;
    ok &= tavrn_mentorship_activate_sid8(&fixture.mentorship, 121u) ==
        TAVRN_MENTORSHIP_COLLISION;
    ok &= tavrn_mentorship_state_snapshot(&fixture.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        state.active_width == TAVRN_IDENTITY_SID16 &&
        state.ordinary_traffic_gated != 0u;
    return ok;
}

static int test_esc_03_atomic_width_transitions(void)
{
    phase4_fixture_t fixture;
    tavrn_direct_peer_t peer_b = make_peer(adva_b, TAVRN_IDENTITY_SID16);
    tavrn_link_data_t data;
    tavrn_mentorship_state_snapshot_t state;
    int ok = 1;

    memset(&data, 0, sizeof(data));
    data.origin = make_peer(adva_a, TAVRN_IDENTITY_SID16).logical_id;
    data.final_destination = peer_b.logical_id;
    data.data_seq = 1u;
    data.ttl = 1u;
    data.app_kind = 0x7fu;
    data.app_len = 1u;
    data.app_bytes[0] = 0xa5u;
    data.ownership = TAVRN_DATA_ORIGINATED;
    ok &= setup_fixture(&fixture, 0u);
    ok &= observe_member(&fixture, adva_b, 1u, 1u,
                         TAVRN_GTT_EVIDENCE_LIVENESS, 1u);
    ok &= tavrn_link_v2_send_unicast(&fixture.link, &peer_b, &data, 2u,
                                     &(tavrn_link_event_t){0}) == TAVRN_LINK_SEND_OK &&
        custody_count(&fixture.link) == 1u;
    ok &= tavrn_mentorship_activate_sid8(&fixture.mentorship, 3u) ==
        TAVRN_MENTORSHIP_INVALID;
    ok &= tavrn_mentorship_tick(&fixture.mentorship, 50u) ==
        TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED;
    ok &= tavrn_mentorship_state_snapshot(&fixture.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        state.active_width == TAVRN_IDENTITY_SID8 && state.ordinary_traffic_gated == 0u &&
        custody_count(&fixture.link) == 0u && fixture.link.candidate_valid == 0u &&
        fixture.link.data_dedupe[0].valid == 0u;
    ok &= tavrn_mentorship_recover_sid16(&fixture.mentorship, 4u) ==
        TAVRN_MENTORSHIP_OK;
    ok &= tavrn_mentorship_state_snapshot(&fixture.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        state.active_width == TAVRN_IDENTITY_SID16 && state.ordinary_traffic_gated != 0u &&
        custody_count(&fixture.link) == 0u && fixture.link.candidate_valid == 0u;
    return ok;
}

static int test_boot_01_full_identity_and_real_composed_admission(void)
{
    phase4_fixture_t fixture;
    tavrn_validated_control_t hello = make_n1_hello(adva_b, 0x6001u);
    tavrn_mentorship_event_trace_t hello_trace;
    tavrn_router_phase_trace_t dispatch_trace;
    tavrn_adva_t peer = make_adva(adva_b);
    tavrn_gtt_snapshot_t peer_snapshot;
    tavrn_mentorship_state_snapshot_t state;
    tavrn_mentorship_status_t hello_status;
    int ok = 1;

    ok &= setup_fixture(&fixture, 0u);
    memset(&dispatch_trace, 0, sizeof(dispatch_trace));
    ok &= tavrn_mentorship_dispatch(&fixture.mentorship, 0u, &dispatch_trace) ==
            TAVRN_MENTORSHIP_OK &&
        dispatch_trace.phase == TAVRN_ROUTER_TRACE_DISPATCH &&
        dispatch_trace.detail.dispatch.status == TAVRN_ROUTER_EVENT_REJOINING &&
        dispatch_trace.detail.dispatch.dispatch_event.type ==
            TAVRN_ROUTER_DISPATCH_EVENT_NONE;
    memset(&hello_trace, 0, sizeof(hello_trace));
    hello_status = deliver_control(&fixture, &hello, adva_b,
                                   TAVRN_IDENTITY_SID16, 1u, &hello_trace);
    ok &= hello.pdu_len == 19u && hello.pdu[5] == 0x40u &&
        memcmp(&hello.pdu[11], adva_b, TAVRN_ADVA_LEN) == 0 &&
        hello_trace.reached_wire_link_router != 0u &&
        hello_trace.wire_decode_result == TAVRN_CODEC_OK &&
        hello_trace.wire_type == TAVRN_WIRE_HELLO &&
        hello_trace.link_event_type == TAVRN_LINK_EVENT_RX_CONTROL &&
        hello_trace.router_result == TAVRN_ROUTER_EVENT_OK &&
        hello_status == TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED;
    memset(&peer_snapshot, 0, sizeof(peer_snapshot));
    ok &= tavrn_gtt_snapshot(&fixture.gtt, &peer, 1u, &peer_snapshot) ==
            TAVRN_GTT_QUERY_FOUND &&
        peer_snapshot.serial_present == 0u;
    ok &= tavrn_mentorship_state_snapshot(&fixture.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        state.active_width == TAVRN_IDENTITY_SID16 &&
        state.full_bootstrap_admission_enabled != 0u &&
        state.ordinary_traffic_gated != 0u;
    return ok;
}

static int test_boot_02_deterministic_offer_timing_and_suppression(void)
{
    phase4_fixture_t fixture;
    tavrn_mentorship_config_t config = make_mentorship_config();
    tavrn_mentorship_offer_t offer;
    uint32_t delay = 0u;
    int ok = 1;

    ok &= setup_fixture(&fixture, 0u);
    ok &= tavrn_mentorship_offer_delay_ms(&config, 90u, 0u, &delay) ==
            TAVRN_MENTORSHIP_OK && delay == 500u;
    ok &= tavrn_mentorship_offer_delay_ms(&config, 30u, 0u, &delay) ==
            TAVRN_MENTORSHIP_OK && delay == 10u;
    ok &= tavrn_mentorship_offer_delay_ms(&config, 60u, 7u, &delay) ==
            TAVRN_MENTORSHIP_OK && delay == 262u;
    memset(&offer, 0, sizeof(offer));
    offer.mentor = make_adva(adva_b);
    offer.mentee = make_adva(adva_a);
    offer.snapshot_id = 0x3301u;
    offer.boot_nonce = 0x5a01u;
    offer.snapshot_count = 2u;
    offer.rssi_magnitude_db = 50u;
    ok &= tavrn_mentorship_collect_offer(&fixture.mentorship, &offer, 1u) ==
        TAVRN_MENTORSHIP_OFFER_ACCEPTED;
    ok &= tavrn_mentorship_collect_offer(&fixture.mentorship, &offer, 2u) ==
        TAVRN_MENTORSHIP_OFFER_DUPLICATE;
    ok &= tavrn_mentorship_schedule_offer(&fixture.mentorship, &offer, 262u) ==
        TAVRN_MENTORSHIP_OFFER_ACCEPTED;
    ok &= tavrn_mentorship_overhear_offer(&fixture.mentorship, &offer, 3u) ==
        TAVRN_MENTORSHIP_OFFER_SUPPRESSED;
    ok &= tavrn_mentorship_schedule_offer(&fixture.mentorship, &offer, 263u) ==
        TAVRN_MENTORSHIP_OFFER_SUPPRESSED;
    ok &= tavrn_mentorship_counters(&fixture.mentorship)->offer_collected == 1u &&
        tavrn_mentorship_counters(&fixture.mentorship)->offer_scheduled == 1u &&
        tavrn_mentorship_counters(&fixture.mentorship)->offer_suppressed == 1u &&
        tavrn_mentorship_counters(&fixture.mentorship)->offer_selected == 0u;
    return ok;
}

static int test_boot_03_total_offer_selection_order(void)
{
    phase4_fixture_t count_fixture;
    phase4_fixture_t rssi_fixture;
    phase4_fixture_t lexical_fixture;
    tavrn_mentorship_offer_t weaker_larger;
    tavrn_mentorship_offer_t stronger_smaller;
    tavrn_mentorship_offer_t selected;
    int ok = 1;

    memset(&weaker_larger, 0, sizeof(weaker_larger));
    weaker_larger.mentor = make_adva(adva_d);
    weaker_larger.mentee = make_adva(adva_a);
    weaker_larger.snapshot_id = 0x0301u;
    weaker_larger.boot_nonce = 0x5a01u;
    weaker_larger.snapshot_count = 4u;
    weaker_larger.rssi_magnitude_db = 90u;
    stronger_smaller = weaker_larger;
    stronger_smaller.mentor = make_adva(adva_b);
    stronger_smaller.snapshot_id = 0x0302u;
    stronger_smaller.snapshot_count = 3u;
    stronger_smaller.rssi_magnitude_db = 30u;

    /* First key: a larger advertised snapshot wins despite weaker RSSI. */
    ok &= setup_fixture(&count_fixture, 0u);
    ok &= tavrn_mentorship_collect_offer(&count_fixture.mentorship,
                                         &weaker_larger, 1u) ==
        TAVRN_MENTORSHIP_OFFER_ACCEPTED;
    ok &= tavrn_mentorship_collect_offer(&count_fixture.mentorship,
                                         &stronger_smaller, 2u) ==
        TAVRN_MENTORSHIP_OFFER_ACCEPTED;
    memset(&selected, 0, sizeof(selected));
    ok &= tavrn_mentorship_select_offer(&count_fixture.mentorship, 20u, &selected) ==
            TAVRN_MENTORSHIP_OK &&
        adva_equal(&selected.mentor, adva_d) && selected.snapshot_id == 0x0301u;
    ok &= tavrn_mentorship_counters(&count_fixture.mentorship)->offer_collected == 2u &&
        tavrn_mentorship_counters(&count_fixture.mentorship)->offer_selected == 1u;

    /* Second key: equal count resolves to the strongest (smallest magnitude)
     * received RSSI. */
    stronger_smaller.snapshot_count = weaker_larger.snapshot_count;
    ok &= setup_fixture(&rssi_fixture, 0u);
    ok &= tavrn_mentorship_collect_offer(&rssi_fixture.mentorship,
                                         &weaker_larger, 1u) ==
        TAVRN_MENTORSHIP_OFFER_ACCEPTED;
    ok &= tavrn_mentorship_collect_offer(&rssi_fixture.mentorship,
                                         &stronger_smaller, 2u) ==
        TAVRN_MENTORSHIP_OFFER_ACCEPTED;
    memset(&selected, 0, sizeof(selected));
    ok &= tavrn_mentorship_select_offer(&rssi_fixture.mentorship, 20u, &selected) ==
            TAVRN_MENTORSHIP_OK &&
        adva_equal(&selected.mentor, adva_b) && selected.snapshot_id == 0x0302u;

    /* Third key: equal count and RSSI use canonical AdvA order.  adva_d's
     * first byte 0x21 sorts before adva_b's 0xdc. */
    stronger_smaller.rssi_magnitude_db = weaker_larger.rssi_magnitude_db;
    ok &= setup_fixture(&lexical_fixture, 0u);
    ok &= tavrn_mentorship_collect_offer(&lexical_fixture.mentorship,
                                         &weaker_larger, 1u) ==
        TAVRN_MENTORSHIP_OFFER_ACCEPTED;
    ok &= tavrn_mentorship_collect_offer(&lexical_fixture.mentorship,
                                         &stronger_smaller, 2u) ==
        TAVRN_MENTORSHIP_OFFER_ACCEPTED;
    memset(&selected, 0, sizeof(selected));
    ok &= tavrn_mentorship_select_offer(&lexical_fixture.mentorship, 20u,
                                        &selected) == TAVRN_MENTORSHIP_OK &&
        adva_equal(&selected.mentor, adva_d) && selected.snapshot_id == 0x0301u;
    return ok;
}

static int test_boot_03_offer_capacity_retention(void)
{
    phase4_fixture_t better_fixture;
    phase4_fixture_t forward_fixture;
    phase4_fixture_t reverse_fixture;
    tavrn_mentorship_offer_t offer;
    tavrn_mentorship_offer_t candidate;
    tavrn_mentorship_offer_t selected;
    uint8_t index;
    int ok = 1;

    memset(&offer, 0, sizeof(offer));
    offer.mentee = make_adva(adva_a);
    offer.boot_nonce = 0x5a01u;
    offer.snapshot_count = 1u;
    offer.rssi_magnitude_db = 50u;

    /* The ninth offer is lexicographically better, so it must displace the
     * deterministic lexical worst of eight otherwise equal offers. */
    ok &= setup_fixture(&better_fixture, 0u);
    for (index = 0u; index < TAVRN_MENTORSHIP_OFFER_CAPACITY; index++) {
        offer.mentor = make_adva(adva_d);
        offer.mentor.bytes[0] = (uint8_t)(0x20u + index);
        offer.snapshot_id = (uint16_t)(0x8300u + index);
        ok &= tavrn_mentorship_collect_offer(&better_fixture.mentorship, &offer,
                                             (uint32_t)index + 1u) ==
            TAVRN_MENTORSHIP_OFFER_ACCEPTED;
    }
    candidate = offer;
    candidate.mentor.bytes[0] = 0x10u;
    candidate.snapshot_id = 0x8310u;
    ok &= tavrn_mentorship_collect_offer(&better_fixture.mentorship, &candidate,
                                         20u) == TAVRN_MENTORSHIP_OFFER_ACCEPTED &&
        better_fixture.mentorship.state.active_offer_count ==
            TAVRN_MENTORSHIP_OFFER_CAPACITY &&
        better_fixture.mentorship.offers[7].mentor.bytes[0] == 0x10u &&
        tavrn_mentorship_counters(&better_fixture.mentorship)->offer_collected == 9u &&
        tavrn_mentorship_counters(&better_fixture.mentorship)->offer_capacity_full == 1u &&
        tavrn_mentorship_counters(&better_fixture.mentorship)->offer_not_retained == 0u &&
        tavrn_mentorship_select_offer(&better_fixture.mentorship, 21u, &selected) ==
            TAVRN_MENTORSHIP_OK &&
        selected.mentor.bytes[0] == 0x10u;

    /* A worse ninth offer is rejected regardless of the first eight offers'
     * arrival order, while both collections retain the same selected offer. */
    ok &= setup_fixture(&forward_fixture, 0u) &&
        setup_fixture(&reverse_fixture, 0u);
    for (index = 0u; index < TAVRN_MENTORSHIP_OFFER_CAPACITY; index++) {
        offer.mentor = make_adva(adva_d);
        offer.mentor.bytes[0] = (uint8_t)(0x20u + index);
        offer.snapshot_id = (uint16_t)(0x8400u + index);
        ok &= tavrn_mentorship_collect_offer(&forward_fixture.mentorship, &offer,
                                             (uint32_t)index + 1u) ==
            TAVRN_MENTORSHIP_OFFER_ACCEPTED;
        offer.mentor.bytes[0] = (uint8_t)(0x27u - index);
        offer.snapshot_id = (uint16_t)(0x8500u + index);
        ok &= tavrn_mentorship_collect_offer(&reverse_fixture.mentorship, &offer,
                                             (uint32_t)index + 1u) ==
            TAVRN_MENTORSHIP_OFFER_ACCEPTED;
    }
    candidate = offer;
    candidate.mentor = make_adva(adva_d);
    candidate.mentor.bytes[0] = 0x01u;
    candidate.snapshot_id = 0x8600u;
    candidate.snapshot_count = 0u;
    ok &= tavrn_mentorship_collect_offer(&forward_fixture.mentorship, &candidate,
                                         20u) == TAVRN_MENTORSHIP_OFFER_STORAGE_FULL &&
        tavrn_mentorship_collect_offer(&reverse_fixture.mentorship, &candidate,
                                       20u) == TAVRN_MENTORSHIP_OFFER_STORAGE_FULL &&
        tavrn_mentorship_counters(&forward_fixture.mentorship)->offer_collected == 8u &&
        tavrn_mentorship_counters(&reverse_fixture.mentorship)->offer_collected == 8u &&
        tavrn_mentorship_counters(&forward_fixture.mentorship)->offer_capacity_full == 1u &&
        tavrn_mentorship_counters(&reverse_fixture.mentorship)->offer_capacity_full == 1u &&
        tavrn_mentorship_counters(&forward_fixture.mentorship)->offer_not_retained == 1u &&
        tavrn_mentorship_counters(&reverse_fixture.mentorship)->offer_not_retained == 1u &&
        tavrn_mentorship_select_offer(&forward_fixture.mentorship, 21u, &selected) ==
            TAVRN_MENTORSHIP_OK &&
        selected.mentor.bytes[0] == 0x20u &&
        tavrn_mentorship_select_offer(&reverse_fixture.mentorship, 21u, &selected) ==
            TAVRN_MENTORSHIP_OK &&
        selected.mentor.bytes[0] == 0x20u;
    return ok;
}

static int test_boot_04_immutable_canonical_snapshot(void)
{
    phase4_fixture_t fixture;
    tavrn_mentorship_snapshot_data_t snapshot;
    tavrn_mentorship_snapshot_data_t before_mutation;
    int ok = 1;

    ok &= setup_fixture(&fixture, 0u);
    ok &= seed_retained_records(&fixture);
    memset(&snapshot, 0, sizeof(snapshot));
    ok &= tavrn_mentorship_freeze_snapshot(&fixture.mentorship, 0x4402u, 150u,
                                            &snapshot) == TAVRN_MENTORSHIP_OK;
    before_mutation = snapshot;
    ok &= observe_member(&fixture, adva_b_sid8_collision, 4u, 1u,
                         TAVRN_GTT_EVIDENCE_LIVENESS, 151u);
    ok &= memcmp(&snapshot, &before_mutation, sizeof(snapshot)) == 0 &&
        snapshot_identities(&snapshot, adva_c, adva_a, adva_d, adva_b) &&
        snapshot.count <= TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY;
    return ok;
}

static int test_boot_05_production_exchange(void)
{
    phase4_fixture_t mentor;
    phase4_fixture_t mentee;
    phase4_fixture_t timeout_mentee;
    tavrn_validated_control_t hello = make_n1_hello(adva_a, 0x5a01u);
    tavrn_validated_control_t reordered;
    tavrn_validated_control_t false_final;
    tavrn_validated_control_t wrong_mentor;
    tavrn_validated_control_t timeout_page;
    tavrn_mentorship_state_snapshot_t state;
    tavrn_mentorship_status_t transfer_status;
    tavrn_mentorship_offer_t timeout_offer;
    tavrn_gtt_storage_t mentee_storage_before_page;
    tavrn_gtt_storage_t timeout_storage_before_page;
    tavrn_gtt_counters_t mentee_counters_before_page;
    tavrn_gtt_counters_t timeout_counters_before_page;
    uint16_t snapshot_id;
    int ok = 1;

    ok &= setup_fixture_for_local(&mentor, adva_b, 0u);
    ok &= setup_fixture(&mentee, 0u);
    /* The mentor is established through the only permitted no-offer path;
     * its retained origin JOIN is flushed before it serves another node. */
    ok &= tavrn_mentorship_tick(&mentor.mentorship, 50u) ==
        TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED;
    ok &= tavrn_mentorship_tick(&mentor.mentorship, 51u) == TAVRN_MENTORSHIP_OK;
    ok &= deliver_control(&mentor, &hello, adva_a, TAVRN_IDENTITY_SID16, 52u,
                          NULL) == TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED;
    /* Builders use the frozen vector network only; retained live controls
     * must be stamped from the currently configured link network. */
    mentor.link.config.network_id = 0x55u;
    mentee.link.config.network_id = 0x55u;
    /* The deterministic strong-RSSI offer becomes due, is retained, then is
     * admitted by the production link queue. */
    ok &= tavrn_mentorship_tick(&mentor.mentorship, 120u) == TAVRN_MENTORSHIP_OK;
    ok &= tavrn_mentorship_tick(&mentor.mentorship, 121u) == TAVRN_MENTORSHIP_OK;
    ok &= transfer_queued_control(&mentor, &mentee, TAVRN_WIRE_SYNC_OFFER, 122u,
                                  &transfer_status) &&
        transfer_status == TAVRN_MENTORSHIP_OK;
    ok &= tavrn_mentorship_tick(&mentee.mentorship, 142u) == TAVRN_MENTORSHIP_OK;
    ok &= tavrn_mentorship_state_snapshot(&mentee.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        state.state == TAVRN_MENTORSHIP_SYNCING && state.selected_mentor_present != 0u;
    snapshot_id = state.selected_snapshot_id;
    reordered = make_sync_data(adva_a, snapshot_id, 1u, adva_b, 1u, 0x11u, 1u);
    false_final = make_sync_data(adva_a, snapshot_id, 0u, adva_b, 1u, 0x11u, 1u);
    wrong_mentor = make_sync_data(adva_a, snapshot_id, 0u, adva_c, 1u, 0x11u, 0u);
    reordered.pdu[3] = 0x55u;
    false_final.pdu[3] = 0x55u;
    wrong_mentor.pdu[3] = 0x55u;
    ok &= deliver_control(&mentee, &wrong_mentor, adva_c, TAVRN_IDENTITY_SID16, 142u,
                          NULL) == TAVRN_MENTORSHIP_INVALID;
    ok &= deliver_control(&mentee, &reordered, adva_b, TAVRN_IDENTITY_SID16, 142u,
                          NULL) == TAVRN_MENTORSHIP_INVALID;
    ok &= deliver_control(&mentee, &false_final, adva_b, TAVRN_IDENTITY_SID16, 142u,
                          NULL) == TAVRN_MENTORSHIP_INVALID;
    ok &= tavrn_mentorship_state_snapshot(&mentee.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        state.state == TAVRN_MENTORSHIP_SYNCING &&
        state.active_width == TAVRN_IDENTITY_SID16;
    mentee_storage_before_page = mentee.gtt_storage;
    mentee_counters_before_page = *tavrn_gtt_counters(&mentee.gtt);
    ok &= tavrn_mentorship_tick(&mentee.mentorship, 143u) == TAVRN_MENTORSHIP_OK;
    ok &= transfer_queued_control(&mentee, &mentor, TAVRN_WIRE_SYNC_PULL, 144u,
                                  &transfer_status) &&
        transfer_status == TAVRN_MENTORSHIP_OK;
    ok &= tavrn_mentorship_tick(&mentor.mentorship, 145u) == TAVRN_MENTORSHIP_OK;
    ok &= transfer_queued_control(&mentor, &mentee, TAVRN_WIRE_SYNC_DATA, 146u,
                                  &transfer_status) &&
        transfer_status == TAVRN_MENTORSHIP_OK &&
        memcmp(&mentee.gtt_storage, &mentee_storage_before_page,
               sizeof(mentee.gtt_storage)) == 0 &&
        memcmp(tavrn_gtt_counters(&mentee.gtt), &mentee_counters_before_page,
               sizeof(mentee_counters_before_page)) == 0 &&
        gtt_identity_count(&mentee.gtt_storage, adva_b) == 0u;
    ok &= tavrn_mentorship_tick(&mentee.mentorship, 147u) == TAVRN_MENTORSHIP_OK;
    ok &= transfer_queued_control(&mentee, &mentor, TAVRN_WIRE_SYNC_PULL, 148u,
                                  &transfer_status) &&
        transfer_status == TAVRN_MENTORSHIP_OK;
    ok &= tavrn_mentorship_tick(&mentor.mentorship, 149u) == TAVRN_MENTORSHIP_OK;
    ok &= transfer_queued_control(&mentor, &mentee, TAVRN_WIRE_SYNC_DATA, 150u,
                                  &transfer_status) &&
        transfer_status == TAVRN_MENTORSHIP_OK;
    ok &= tavrn_mentorship_state_snapshot(&mentee.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        state.state == TAVRN_MENTORSHIP_SID8_ACTIVE &&
        state.active_width == TAVRN_IDENTITY_SID8 &&
        state.ordinary_traffic_gated == 0u &&
        gtt_identity_count(&mentee.gtt_storage, adva_b) == 1u;

    /* A staged first page cannot leak through a mentor timeout into the later
     * no-offer self-bootstrap path. */
    memset(&timeout_offer, 0, sizeof(timeout_offer));
    ok &= setup_fixture(&timeout_mentee, 0u);
    timeout_offer.mentor = make_adva(adva_b);
    timeout_offer.mentee = make_adva(adva_a);
    timeout_offer.snapshot_id = 0x6601u;
    timeout_offer.boot_nonce = 0x5a01u;
    timeout_offer.snapshot_count = 2u;
    timeout_offer.rssi_magnitude_db = 40u;
    ok &= begin_sync_page_wait(&timeout_mentee, &timeout_offer, 1u);
    timeout_page = make_sync_data(adva_a, timeout_offer.snapshot_id, 0u, adva_b,
                                  1u, 0x11u, 0u);
    timeout_storage_before_page = timeout_mentee.gtt_storage;
    timeout_counters_before_page = *tavrn_gtt_counters(&timeout_mentee.gtt);
    ok &= deliver_control(&timeout_mentee, &timeout_page, adva_b,
                          TAVRN_IDENTITY_SID16, 23u, NULL) == TAVRN_MENTORSHIP_OK &&
        memcmp(&timeout_mentee.gtt_storage, &timeout_storage_before_page,
               sizeof(timeout_mentee.gtt_storage)) == 0 &&
        memcmp(tavrn_gtt_counters(&timeout_mentee.gtt),
               &timeout_counters_before_page,
               sizeof(timeout_counters_before_page)) == 0 &&
        tavrn_mentorship_tick(&timeout_mentee.mentorship, 24u) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&timeout_mentee.mentorship, 54u) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&timeout_mentee.mentorship, 55u) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&timeout_mentee.mentorship, 85u) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&timeout_mentee.mentorship, 86u) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&timeout_mentee.mentorship, 116u) ==
            TAVRN_MENTORSHIP_RESTARTED &&
        tavrn_mentorship_tick(&timeout_mentee.mentorship, 166u) ==
            TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED &&
        gtt_identity_count(&timeout_mentee.gtt_storage, adva_b) == 0u &&
        memcmp(&timeout_mentee.gtt_storage, &timeout_storage_before_page,
               sizeof(timeout_mentee.gtt_storage)) == 0;
    return ok;
}

/* BOOT-05 broadcast contract: all three controls are copied from the real
 * queued advertisement and heard by a third board.  The listener must never
 * turn another mentee's valid exchange into a terminal local failure. */
static int test_boot_05_broadcast_production_exchange(void)
{
    phase4_fixture_t mentor;
    phase4_fixture_t mentee;
    phase4_fixture_t bystander;
    tavrn_validated_control_t hello = make_n1_hello(adva_a, 0x5a01u);
    phase4_bystander_snapshot_t bystander_before;
    tavrn_mentorship_state_snapshot_t mentee_state;
    tavrn_mentorship_status_t intended_status;
    tavrn_mentorship_status_t bystander_status;
    int ok = 1;

    ok &= setup_fixture_for_local(&mentor, adva_b, 0u) &&
        setup_fixture(&mentee, 0u) &&
        setup_fixture_for_local(&bystander, adva_c, 0u);
    ok &= tavrn_mentorship_tick(&mentor.mentorship, 50u) ==
            TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED &&
        tavrn_mentorship_tick(&mentor.mentorship, 51u) == TAVRN_MENTORSHIP_OK &&
        deliver_control(&mentor, &hello, adva_a, TAVRN_IDENTITY_SID16, 52u,
                        NULL) == TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED &&
        tavrn_mentorship_tick(&mentor.mentorship, 120u) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&mentor.mentorship, 121u) == TAVRN_MENTORSHIP_OK;

    ok &= capture_bystander(&bystander, &bystander_before) &&
        broadcast_queued_control(&mentor, &mentee, &bystander,
                                 TAVRN_WIRE_SYNC_OFFER, 122u,
                                 &intended_status, &bystander_status) &&
        intended_status == TAVRN_MENTORSHIP_OK &&
        bystander_status == TAVRN_MENTORSHIP_OK &&
        bystander_is_unchanged(&bystander, &bystander_before);

    ok &= tavrn_mentorship_tick(&mentee.mentorship, 142u) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&mentee.mentorship, 143u) == TAVRN_MENTORSHIP_OK &&
        capture_bystander(&bystander, &bystander_before) &&
        broadcast_queued_control(&mentee, &mentor, &bystander,
                                 TAVRN_WIRE_SYNC_PULL, 144u,
                                 &intended_status, &bystander_status) &&
        intended_status == TAVRN_MENTORSHIP_OK &&
        bystander_status == TAVRN_MENTORSHIP_OK &&
        bystander_is_unchanged(&bystander, &bystander_before);

    ok &= tavrn_mentorship_tick(&mentor.mentorship, 145u) == TAVRN_MENTORSHIP_OK &&
        capture_bystander(&bystander, &bystander_before) &&
        broadcast_queued_control(&mentor, &mentee, &bystander,
                                 TAVRN_WIRE_SYNC_DATA, 146u,
                                 &intended_status, &bystander_status) &&
        intended_status == TAVRN_MENTORSHIP_OK &&
        bystander_status == TAVRN_MENTORSHIP_OK &&
        bystander_is_unchanged(&bystander, &bystander_before);

    ok &= tavrn_mentorship_tick(&mentee.mentorship, 147u) == TAVRN_MENTORSHIP_OK &&
        capture_bystander(&bystander, &bystander_before) &&
        broadcast_queued_control(&mentee, &mentor, &bystander,
                                 TAVRN_WIRE_SYNC_PULL, 148u,
                                 &intended_status, &bystander_status) &&
        intended_status == TAVRN_MENTORSHIP_OK &&
        bystander_status == TAVRN_MENTORSHIP_OK &&
        bystander_is_unchanged(&bystander, &bystander_before) &&
        tavrn_mentorship_tick(&mentor.mentorship, 149u) == TAVRN_MENTORSHIP_OK &&
        capture_bystander(&bystander, &bystander_before) &&
        broadcast_queued_control(&mentor, &mentee, &bystander,
                                 TAVRN_WIRE_SYNC_DATA, 150u,
                                 &intended_status, &bystander_status) &&
        intended_status == TAVRN_MENTORSHIP_OK &&
        bystander_status == TAVRN_MENTORSHIP_OK &&
        bystander_is_unchanged(&bystander, &bystander_before);

    ok &= tavrn_mentorship_state_snapshot(&mentee.mentorship, &mentee_state) ==
            TAVRN_MENTORSHIP_OK &&
        mentee_state.state == TAVRN_MENTORSHIP_SID8_ACTIVE &&
        mentee_state.active_width == TAVRN_IDENTITY_SID8 &&
        mentee_state.ordinary_traffic_gated == 0u;
    return ok;
}

static int test_boot_05_one_entry_pages_and_fresh_merge(void)
{
    phase4_fixture_t fixture;
    phase4_fixture_t wire_mentee;
    phase4_fixture_t wire_mentor;
    tavrn_adva_t mentor = make_adva(adva_b);
    tavrn_adva_t mentee = make_adva(adva_a);
    tavrn_mentorship_snapshot_data_t snapshot;
    tavrn_mentorship_snapshot_data_t empty_snapshot;
    tavrn_validated_control_t page;
    tavrn_validated_control_t self_page;
    tavrn_validated_control_t empty_page;
    tavrn_validated_control_t wire_offer;
    tavrn_validated_control_t wire_pull;
    tavrn_gtt_snapshot_t merged;
    tavrn_mentorship_offer_t offer;
    tavrn_mentorship_state_snapshot_t state;
    tavrn_mentorship_event_trace_t offer_trace;
    tavrn_mentorship_event_trace_t pull_trace;
    tavrn_mentorship_status_t offer_status;
    tavrn_mentorship_status_t pull_status;
    uint8_t wrapped[BLE_ADV_MAX_DATA];
    uint8_t wrapped_len = 0u;
    tavrn_decoded_frame_t decoded;
    tavrn_codec_config_t codec = make_codec_config(TAVRN_IDENTITY_SID16);
    int ok = test_boot_05_production_exchange();
    ok &= setup_fixture(&fixture, 0u);
    ok &= observe_member(&fixture, adva_b, 1u, 1u,
                         TAVRN_GTT_EVIDENCE_LIVENESS, 1u);
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.snapshot_id = 0x5501u;
    snapshot.count = 2u;
    snapshot.records[0].identity = make_adva(adva_b);
    snapshot.records[0].serial = 2u;
    snapshot.records[0].serial_present = 1u;
    snapshot.records[0].ttl_bucket = 1u;
    snapshot.records[0].hop_count = 0u;
    snapshot.records[1].identity = mentee;
    snapshot.records[1].serial = 1u;
    snapshot.records[1].serial_present = 1u;
    snapshot.records[1].ttl_bucket = 1u;
    snapshot.records[1].hop_count = 0u;
    memset(&wire_offer, 0, sizeof(wire_offer));
    memset(&wire_pull, 0, sizeof(wire_pull));
    ok &= setup_fixture(&wire_mentee, 0u);
    ok &= setup_fixture_for_local(&wire_mentor, adva_b, 0u);
    ok &= tavrn_mentorship_build_sync_offer(&snapshot, &mentor, &mentee,
                                            0x5a01u, &wire_offer) ==
        TAVRN_MENTORSHIP_OK;
    ok &= tavrn_mentorship_build_sync_pull(&mentee, &mentor, snapshot.snapshot_id,
                                           0u, &wire_pull) == TAVRN_MENTORSHIP_OK;
    memset(&offer_trace, 0, sizeof(offer_trace));
    offer_status = deliver_control(&wire_mentee, &wire_offer, adva_b,
                                   TAVRN_IDENTITY_SID16, 3u, &offer_trace);
    memset(&pull_trace, 0, sizeof(pull_trace));
    pull_status = deliver_control(&wire_mentor, &wire_pull, adva_a,
                                  TAVRN_IDENTITY_SID16, 4u, &pull_trace);
    ok &= wire_offer.pdu_len == 24u && wire_offer.pdu[19] == snapshot.count &&
        offer_trace.reached_wire_link_router != 0u &&
        offer_trace.wire_type == TAVRN_WIRE_SYNC_OFFER &&
        offer_trace.link_event_type == TAVRN_LINK_EVENT_RX_CONTROL &&
        offer_status == TAVRN_MENTORSHIP_OK;
    ok &= wire_pull.pdu_len == 22u && wire_pull.pdu[20] == 0u &&
        wire_pull.pdu[21] == 1u && pull_trace.reached_wire_link_router != 0u &&
        pull_trace.wire_type == TAVRN_WIRE_SYNC_PULL &&
        pull_trace.link_event_type == TAVRN_LINK_EVENT_RX_CONTROL &&
        pull_status == TAVRN_MENTORSHIP_OK;
    memset(&offer, 0, sizeof(offer));
    offer.mentor = mentor;
    offer.mentee = mentee;
    offer.snapshot_id = snapshot.snapshot_id;
    offer.boot_nonce = 0x5a01u;
    offer.snapshot_count = snapshot.count;
    offer.rssi_magnitude_db = 40u;
    ok &= begin_sync_page_wait(&fixture, &offer, 2u) &&
        adva_equal(&fixture.mentorship.state.selected_mentor, adva_b);
    memset(&page, 0, sizeof(page));
    ok &= tavrn_mentorship_build_sync_data(&snapshot, &mentor, &mentee, 0u, &page) ==
            TAVRN_MENTORSHIP_OK;
    ok &= page.type == TAVRN_WIRE_SYNC_DATA && page.pdu_len == 24u &&
        page.pdu[5] == 0x80u && page.pdu[14] == 0u &&
        memcmp(&page.pdu[15], adva_b, TAVRN_ADVA_LEN) == 0 &&
        page.pdu[23] == 0x10u &&
        wrap_control(&page, adva_b, TAVRN_IDENTITY_SID16, wrapped, &wrapped_len) &&
        tavrn_wire_v2_decode(&codec, adva_b, wrapped, wrapped_len, &decoded) ==
            TAVRN_CODEC_OK && decoded.type == TAVRN_WIRE_SYNC_DATA;
    ok &= deliver_control(&fixture, &page, adva_b, TAVRN_IDENTITY_SID16, 24u,
                          NULL) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&fixture.mentorship, 25u) == TAVRN_MENTORSHIP_OK;
    memset(&self_page, 0, sizeof(self_page));
    ok &= tavrn_mentorship_build_sync_data(&snapshot, &mentor, &mentee, 1u,
                                           &self_page) == TAVRN_MENTORSHIP_OK &&
        self_page.pdu_len == 24u && self_page.pdu[5] == 0xc0u &&
        memcmp(&self_page.pdu[15], adva_a, TAVRN_ADVA_LEN) == 0 &&
        tavrn_mentorship_ingest_sync_data(&fixture.mentorship, &mentor,
                                          &self_page, 26u) == TAVRN_MENTORSHIP_OK &&
        gtt_identity_count(&fixture.gtt_storage, adva_a) == 1u;
    ok &= tavrn_mentorship_ingest_sync_data(&fixture.mentorship, &mentor,
                                            &page, 26u) ==
        TAVRN_MENTORSHIP_SYNC_PAGE_DUPLICATE;
    ok &= tavrn_gtt_snapshot(&fixture.gtt, &mentor, 26u, &merged) ==
            TAVRN_GTT_QUERY_FOUND &&
        merged.serial == 2u && merged.hop_count == 1u;
    ok &= tavrn_mentorship_state_snapshot(&fixture.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        state.sync_pages_committed == 2u;
    memset(&empty_snapshot, 0, sizeof(empty_snapshot));
    empty_snapshot.snapshot_id = 0x5502u;
    memset(&empty_page, 0, sizeof(empty_page));
    ok &= tavrn_mentorship_build_sync_data(&empty_snapshot, &mentor, &mentee, 0u,
                                           &empty_page) ==
            TAVRN_MENTORSHIP_OK &&
        empty_page.pdu_len == 15u && empty_page.pdu[5] == 0x40u &&
        empty_page.pdu[14] == 0u;
    ok &= tavrn_mentorship_build_sync_data(&snapshot, &mentor, &mentee, 2u, &page) ==
        TAVRN_MENTORSHIP_INVALID;
    return ok;
}

static int test_boot_05_deadlines_ttl_and_sync_dedupe(void)
{
    phase4_fixture_t wrap_retry;
    phase4_fixture_t pre_pull_mentee;
    phase4_fixture_t deadline_mentee;
    phase4_fixture_t deadline_mentor;
    phase4_fixture_t ttl_fixture;
    phase4_fixture_t receive_fixture;
    phase4_fixture_t dedupe_fixture;
    phase4_fixture_t wrap_dedupe_fixture;
    phase4_fixture_t capacity_fixture;
    tavrn_mentorship_offer_t offer;
    tavrn_mentorship_snapshot_data_t snapshot;
    tavrn_validated_control_t page;
    tavrn_validated_control_t page_next;
    tavrn_validated_control_t pull;
    tavrn_validated_control_t hello = make_n1_hello(adva_a, 0x7801u);
    tavrn_gtt_storage_t storage_before;
    tavrn_mentorship_sync_dedupe_t
        dedupe_before[TAVRN_MENTORSHIP_SYNC_DEDUPE_CAPACITY];
    tavrn_mentorship_record_t receiving_before[TAVRN_MENTORSHIP_SNAPSHOT_CAPACITY];
    tavrn_gtt_entry_t *entry;
    uint32_t wrap_start = UINT32_MAX - 50u;
    uint32_t serving_deadline;
    uint32_t deadline_before;
    uint8_t index;
    int ok = 1;

    memset(&offer, 0, sizeof(offer));
    offer.mentor = make_adva(adva_b);
    offer.mentee = make_adva(adva_a);
    offer.snapshot_id = 0x8a01u;
    offer.boot_nonce = 0x5a01u;
    offer.snapshot_count = 1u;
    offer.rssi_magnitude_db = 40u;

    /* A zero-valued deadline is valid when the actual first pull crosses the
     * uint32_t wrap.  It still drives two retries and the terminal restart. */
    ok &= setup_fixture(&wrap_retry, wrap_start) &&
        tavrn_mentorship_collect_offer(&wrap_retry.mentorship, &offer, wrap_start) ==
            TAVRN_MENTORSHIP_OFFER_ACCEPTED &&
        tavrn_mentorship_tick(&wrap_retry.mentorship, wrap_start + 20u) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&wrap_retry.mentorship, wrap_start + 21u) ==
            TAVRN_MENTORSHIP_OK &&
        wrap_retry.mentorship.page_deadline_valid != 0u &&
        wrap_retry.mentorship.page_deadline_ms == 0u &&
        tavrn_mentorship_tick(&wrap_retry.mentorship, 0u) == TAVRN_MENTORSHIP_OK &&
        wrap_retry.mentorship.pending_control.valid != 0u &&
        tavrn_mentorship_tick(&wrap_retry.mentorship, 1u) == TAVRN_MENTORSHIP_OK &&
        wrap_retry.mentorship.state.pull_attempts == 2u &&
        wrap_retry.mentorship.page_deadline_ms == 31u &&
        tavrn_mentorship_tick(&wrap_retry.mentorship, 31u) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&wrap_retry.mentorship, 32u) == TAVRN_MENTORSHIP_OK &&
        wrap_retry.mentorship.state.pull_attempts == 3u &&
        tavrn_mentorship_tick(&wrap_retry.mentorship, 62u) ==
            TAVRN_MENTORSHIP_RESTARTED;

    /* Selection retains a pull first.  Until the next tick physically admits
     * that pull, a matching page is rejected without staging or GTT mutation. */
    ok &= setup_fixture(&pre_pull_mentee, 0u) &&
        tavrn_mentorship_collect_offer(&pre_pull_mentee.mentorship, &offer, 1u) ==
            TAVRN_MENTORSHIP_OFFER_ACCEPTED &&
        tavrn_mentorship_tick(&pre_pull_mentee.mentorship, 21u) ==
            TAVRN_MENTORSHIP_OK;
    page = make_sync_data(adva_a, offer.snapshot_id, 0u, adva_b, 1u, 0x11u, 1u);
    storage_before = pre_pull_mentee.gtt_storage;
    ok &= pre_pull_mentee.mentorship.pending_control.valid != 0u &&
        pre_pull_mentee.mentorship.page_deadline_valid == 0u &&
        deliver_control(&pre_pull_mentee, &page, adva_b, TAVRN_IDENTITY_SID16, 21u,
                        NULL) == TAVRN_MENTORSHIP_SYNC_PAGE_DUPLICATE &&
        pre_pull_mentee.mentorship.receiving_page_bitmap == 0u &&
        memcmp(&pre_pull_mentee.gtt_storage, &storage_before,
               sizeof(storage_before)) == 0 &&
        tavrn_mentorship_tick(&pre_pull_mentee.mentorship, 22u) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_ingest_sync_data(&pre_pull_mentee.mentorship,
                                          &offer.mentor, &page, 23u) ==
            TAVRN_MENTORSHIP_OK;

    /* A page at the mentee deadline cannot consume the page before the timeout
     * tick schedules its deterministic retry. */
    ok &= setup_fixture(&deadline_mentee, 0u) &&
        tavrn_mentorship_collect_offer(&deadline_mentee.mentorship, &offer, 1u) ==
            TAVRN_MENTORSHIP_OFFER_ACCEPTED &&
        tavrn_mentorship_tick(&deadline_mentee.mentorship, 21u) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&deadline_mentee.mentorship, 22u) ==
            TAVRN_MENTORSHIP_OK;
    page = make_sync_data(adva_a, offer.snapshot_id, 0u, adva_b, 1u, 0x11u, 1u);
    storage_before = deadline_mentee.gtt_storage;
    ok &= deadline_mentee.mentorship.page_deadline_valid != 0u &&
        deadline_mentee.mentorship.page_deadline_ms == 52u &&
        deliver_control(&deadline_mentee, &page, adva_b, TAVRN_IDENTITY_SID16, 52u,
                        NULL) == TAVRN_MENTORSHIP_SYNC_PAGE_DUPLICATE &&
        deadline_mentee.mentorship.receiving_page_bitmap == 0u &&
        deadline_mentee.mentorship.page_deadline_valid != 0u &&
        deadline_mentee.mentorship.page_deadline_ms == 52u &&
        memcmp(&deadline_mentee.gtt_storage, &storage_before,
               sizeof(storage_before)) == 0 &&
        tavrn_mentorship_tick(&deadline_mentee.mentorship, 52u) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&deadline_mentee.mentorship, 53u) ==
            TAVRN_MENTORSHIP_OK &&
        deadline_mentee.mentorship.state.pull_attempts == 2u;

    /* A serving deadline is an ingress boundary too: a valid old pull cannot
     * recreate or extend the mentor session before its tick runs. */
    ok &= setup_fixture_for_local(&deadline_mentor, adva_b, 0u) &&
        tavrn_mentorship_tick(&deadline_mentor.mentorship, 50u) ==
            TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED &&
        tavrn_mentorship_tick(&deadline_mentor.mentorship, 51u) ==
            TAVRN_MENTORSHIP_OK &&
        deliver_control(&deadline_mentor, &hello, adva_a, TAVRN_IDENTITY_SID16, 52u,
                        NULL) == TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED;
    serving_deadline = deadline_mentor.mentorship.serving_deadline_ms;
    ok &= tavrn_mentorship_build_sync_pull(&offer.mentee, &offer.mentor,
                                           deadline_mentor.mentorship.serving_snapshot
                                               .snapshot_id,
                                           0u, &pull) == TAVRN_MENTORSHIP_OK &&
        deliver_control(&deadline_mentor, &pull, adva_a, TAVRN_IDENTITY_SID16,
                        serving_deadline, NULL) == TAVRN_MENTORSHIP_OK &&
        deadline_mentor.mentorship.serving_session_valid == 0u &&
        deadline_mentor.mentorship.pending_control.valid == 0u &&
        queued_control_count(&deadline_mentor, TAVRN_WIRE_SYNC_DATA) == 0u;

    /* Active records use rounded-up 20-second hard-TTL buckets, departed
     * records use zero, and retained hard-expired context uses bucket one. */
    ok &= setup_fixture(&ttl_fixture, 0u) &&
        observe_member(&ttl_fixture, adva_b, 1u, 1u,
                       TAVRN_GTT_EVIDENCE_LIVENESS, 0u) &&
        observe_member(&ttl_fixture, adva_c, 2u, 1u,
                       TAVRN_GTT_EVIDENCE_LIVENESS, 0u) &&
        observe_member(&ttl_fixture, adva_d, 3u, 1u,
                       TAVRN_GTT_EVIDENCE_DEPARTED, 0u);
    entry = gtt_entry_for(&ttl_fixture.gtt_storage, adva_b);
    if (entry != NULL) {
        entry->hard_deadline_ms = 20051u;
    }
    entry = gtt_entry_for(&ttl_fixture.gtt_storage, adva_c);
    if (entry != NULL) {
        entry->hard_deadline_ms = 50u;
    }
    ttl_fixture.gtt_storage.entries[0].hard_deadline_ms = 50u;
    memset(&snapshot, 0, sizeof(snapshot));
    ok &= tavrn_mentorship_freeze_snapshot(&ttl_fixture.mentorship, 0x8a02u, 50u,
                                            &snapshot) == TAVRN_MENTORSHIP_OK &&
        snapshot_identities(&snapshot, adva_c, adva_a, adva_d, adva_b) &&
        snapshot.records[0].ttl_bucket == 1u &&
        snapshot.records[1].ttl_bucket == 1u &&
        snapshot.records[2].ttl_bucket == 0u &&
        snapshot.records[3].ttl_bucket == 2u;

    /* Receipt time becomes local evidence, but the transferred bucket bounds
     * the new hard lifetime instead of restoring the configured full expiry. */
    ok &= setup_fixture(&receive_fixture, 0u) &&
        begin_sync_page_wait(&receive_fixture, &offer, 1u);
    page = make_sync_data(adva_a, offer.snapshot_id, 0u, adva_b, 2u, 0x21u, 1u);
    ok &= tavrn_mentorship_ingest_sync_data(&receive_fixture.mentorship,
                                            &offer.mentor, &page, 23u) ==
            TAVRN_MENTORSHIP_OK;
    entry = gtt_entry_for(&receive_fixture.gtt_storage, adva_b);
    ok &= entry != NULL && entry->last_evidence_ms == 23u &&
        entry->soft_deadline_ms == 73u && entry->hard_deadline_ms == 40023u;

    /* Active-page equality is distinct from retained equality.  Complete two
     * different sessions, then prove that both tuple/page bitmaps coexist. */
    offer.snapshot_id = 0x8a03u;
    offer.snapshot_count = 2u;
    page = make_sync_data(adva_a, offer.snapshot_id, 0u, adva_b, 3u, 0x11u, 0u);
    page_next = make_sync_data(adva_a, offer.snapshot_id, 1u, adva_c, 4u, 0x11u, 1u);
    ok &= setup_fixture(&dedupe_fixture, 0u) &&
        begin_sync_page_wait(&dedupe_fixture, &offer, 1u) &&
        deliver_control(&dedupe_fixture, &page, adva_b, TAVRN_IDENTITY_SID16, 23u,
                        NULL) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&dedupe_fixture.mentorship, 24u) ==
            TAVRN_MENTORSHIP_OK &&
        remove_queued_control(&dedupe_fixture, TAVRN_WIRE_SYNC_PULL) &&
        tavrn_mentorship_ingest_sync_data(&dedupe_fixture.mentorship,
                                          &offer.mentor, &page, 25u) ==
            TAVRN_MENTORSHIP_SYNC_PAGE_DUPLICATE &&
        tavrn_mentorship_ingest_sync_data(&dedupe_fixture.mentorship,
                                          &offer.mentor, &page_next, 26u) ==
            TAVRN_MENTORSHIP_OK &&
        completed_sync_count(&dedupe_fixture.mentorship) == 1u &&
        tavrn_mentorship_recover_sid16(&dedupe_fixture.mentorship, 27u) ==
            TAVRN_MENTORSHIP_OK;
    offer.snapshot_id = 0x8a04u;
    offer.snapshot_count = 1u;
    page_next = make_sync_data(adva_a, offer.snapshot_id, 0u, adva_d, 5u, 0x11u, 1u);
    ok &= begin_sync_page_wait(&dedupe_fixture, &offer, 28u) &&
        tavrn_mentorship_ingest_sync_data(&dedupe_fixture.mentorship,
                                          &offer.mentor, &page_next, 50u) ==
            TAVRN_MENTORSHIP_OK &&
        completed_sync_count(&dedupe_fixture.mentorship) == 2u &&
        dedupe_fixture.mentorship.completed_sync[0].snapshot_id == 0x8a03u &&
        dedupe_fixture.mentorship.completed_sync[1].snapshot_id == 0x8a04u;

    offer.snapshot_id = 0x8a03u;
    offer.snapshot_count = 2u;
    ok &= tavrn_mentorship_recover_sid16(&dedupe_fixture.mentorship, 51u) ==
            TAVRN_MENTORSHIP_OK &&
        begin_sync_page_wait(&dedupe_fixture, &offer, 52u) &&
        tavrn_mentorship_ingest_sync_data(&dedupe_fixture.mentorship,
                                          &offer.mentor, &page, 74u) ==
            TAVRN_MENTORSHIP_SYNC_PAGE_DUPLICATE &&
        dedupe_fixture.mentorship.receiving_page_bitmap == 0u &&
        tavrn_mentorship_recover_sid16(&dedupe_fixture.mentorship, 75u) ==
            TAVRN_MENTORSHIP_OK;
    offer.snapshot_id = 0x8a04u;
    offer.snapshot_count = 1u;
    ok &= begin_sync_page_wait(&dedupe_fixture, &offer, 76u) &&
        tavrn_mentorship_ingest_sync_data(&dedupe_fixture.mentorship,
                                          &offer.mentor, &page_next, 98u) ==
            TAVRN_MENTORSHIP_SYNC_PAGE_DUPLICATE &&
        dedupe_fixture.mentorship.receiving_page_bitmap == 0u;

    /* The first tuple expires exactly at 126 ms.  It becomes admissible again
     * while the second unexpired tuple remains resident. */
    offer.snapshot_id = 0x8a03u;
    offer.snapshot_count = 2u;
    ok &= tavrn_mentorship_recover_sid16(&dedupe_fixture.mentorship, 99u) ==
            TAVRN_MENTORSHIP_OK &&
        begin_sync_page_wait(&dedupe_fixture, &offer, 100u) &&
        tavrn_mentorship_ingest_sync_data(&dedupe_fixture.mentorship,
                                          &offer.mentor, &page, 126u) ==
            TAVRN_MENTORSHIP_OK &&
        completed_sync_count(&dedupe_fixture.mentorship) == 1u &&
        dedupe_fixture.mentorship.completed_sync[1].snapshot_id == 0x8a04u;

    /* Retention expiry remains exact across wrap, while accepted pages still
     * require a production-sent pull. */
    offer.snapshot_id = 0x8a05u;
    offer.snapshot_count = 1u;
    page = make_sync_data(adva_a, offer.snapshot_id, 0u, adva_b, 6u, 0x11u, 1u);
    ok &= setup_fixture(&wrap_dedupe_fixture, wrap_start) &&
        begin_sync_page_wait(&wrap_dedupe_fixture, &offer, wrap_start) &&
        tavrn_mentorship_ingest_sync_data(&wrap_dedupe_fixture.mentorship,
                                          &offer.mentor, &page, wrap_start + 22u) ==
            TAVRN_MENTORSHIP_OK &&
        wrap_dedupe_fixture.mentorship.completed_sync[0].expires_at_ms == 71u &&
        tavrn_mentorship_recover_sid16(&wrap_dedupe_fixture.mentorship,
                                       wrap_start + 23u) == TAVRN_MENTORSHIP_OK &&
        begin_sync_page_wait(&wrap_dedupe_fixture, &offer, wrap_start + 30u) &&
        tavrn_mentorship_ingest_sync_data(&wrap_dedupe_fixture.mentorship,
                                          &offer.mentor, &page, 30u) ==
            TAVRN_MENTORSHIP_SYNC_PAGE_DUPLICATE &&
        tavrn_mentorship_tick(&wrap_dedupe_fixture.mentorship, 31u) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&wrap_dedupe_fixture.mentorship, 32u) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&wrap_dedupe_fixture.mentorship, 62u) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&wrap_dedupe_fixture.mentorship, 63u) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_ingest_sync_data(&wrap_dedupe_fixture.mentorship,
                                          &offer.mentor, &page, 71u) ==
            TAVRN_MENTORSHIP_OK;

    /* A full cache refuses the final page before staging or GTT commit, keeps
     * all unexpired tuples, and retries into the deterministic first expired
     * slot. */
    offer.snapshot_id = 0x8a06u;
    offer.snapshot_count = 1u;
    page = make_sync_data(adva_a, offer.snapshot_id, 0u, adva_b, 7u, 0x11u, 1u);
    ok &= setup_fixture(&capacity_fixture, 0u) &&
        begin_sync_page_wait(&capacity_fixture, &offer, 1u);
    for (index = 0u; index < TAVRN_MENTORSHIP_SYNC_DEDUPE_CAPACITY; index++) {
        tavrn_mentorship_sync_dedupe_t *completed =
            &capacity_fixture.mentorship.completed_sync[index];

        memset(completed, 0, sizeof(*completed));
        completed->mentor = make_adva(adva_d);
        completed->mentee = make_adva(adva_a);
        completed->snapshot_id = (uint16_t)(0x9000u + index);
        completed->page_bitmap = 1u;
        completed->expires_at_ms = 1000u;
        completed->valid = 1u;
    }
    storage_before = capacity_fixture.gtt_storage;
    memcpy(dedupe_before, capacity_fixture.mentorship.completed_sync,
           sizeof(dedupe_before));
    memcpy(receiving_before, capacity_fixture.mentorship.receiving_records,
           sizeof(receiving_before));
    deadline_before = capacity_fixture.mentorship.page_deadline_ms;
    ok &= tavrn_mentorship_ingest_sync_data(&capacity_fixture.mentorship,
                                            &offer.mentor, &page, 23u) ==
            TAVRN_MENTORSHIP_BUSY &&
        memcmp(&capacity_fixture.gtt_storage, &storage_before,
               sizeof(storage_before)) == 0 &&
        memcmp(capacity_fixture.mentorship.completed_sync, dedupe_before,
               sizeof(dedupe_before)) == 0 &&
        memcmp(capacity_fixture.mentorship.receiving_records, receiving_before,
               sizeof(receiving_before)) == 0 &&
        capacity_fixture.mentorship.receiving_page_bitmap == 0u &&
        capacity_fixture.mentorship.state.sync_pages_committed == 0u &&
        capacity_fixture.mentorship.state.sync_next_index == 0u &&
        capacity_fixture.mentorship.page_deadline_valid != 0u &&
        capacity_fixture.mentorship.page_deadline_ms == deadline_before &&
        capacity_fixture.mentorship.counters.sync_dedupe_capacity_full == 1u;
    capacity_fixture.mentorship.completed_sync[0].expires_at_ms = 23u;
    ok &= tavrn_mentorship_ingest_sync_data(&capacity_fixture.mentorship,
                                            &offer.mentor, &page, 23u) ==
            TAVRN_MENTORSHIP_OK &&
        capacity_fixture.mentorship.completed_sync[0].snapshot_id == offer.snapshot_id &&
        memcmp(&capacity_fixture.mentorship.completed_sync[1], &dedupe_before[1],
               sizeof(dedupe_before[1])) == 0;
    return ok;
}

static int test_boot_06_production_collision_and_join_backpressure(void)
{
    phase4_fixture_t mismatch;
    phase4_fixture_t join_collision;
    phase4_fixture_t page_collision;
    phase4_fixture_t origin;
    tavrn_validated_control_t hello = make_n1_hello(adva_b, 0x7001u);
    tavrn_validated_control_t collision_hello = make_n1_hello(
        adva_b_sid8_collision, 0x7002u);
    tavrn_validated_control_t rrep = make_sid8_rrep(adva_a[0], adva_b[0],
                                                     adva_a[0], 1u);
    tavrn_validated_control_t join = make_join(adva_c, adva_b_sid8_collision, 0x7702u);
    tavrn_validated_control_t relay_join = make_join(adva_c, adva_d, 0x7703u);
    tavrn_validated_control_t collision_page = make_sync_data(
        adva_a, 0x7704u, 0u, adva_b_sid8_collision, 1u, 0x11u, 1u);
    tavrn_mentorship_offer_t page_offer;
    tavrn_mentorship_state_snapshot_t state;
    tavrn_router_phase_trace_t dispatch_trace;
    tavrn_mentorship_event_trace_t sid8_trace;
    tron_application_data_t application;
    aodv_counters_t counters;
    int ok = 1;

    ok &= setup_fixture(&mismatch, 0u);
    ok &= tavrn_mentorship_tick(&mismatch.mentorship, 50u) ==
        TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED;
    ok &= tavrn_mentorship_tick(&mismatch.mentorship, 51u) == TAVRN_MENTORSHIP_OK;
    ok &= deliver_control(&mismatch, &hello, adva_b, TAVRN_IDENTITY_SID16, 52u,
                          NULL) == TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED;
    memset(&application, 0, sizeof(application));
    application.final_destination = make_peer(adva_b, TAVRN_IDENTITY_SID8).logical_id;
    application.app_kind = 0x7fu;
    application.app_len = 1u;
    application.app_bytes[0] = 0xa5u;
    memset(&dispatch_trace, 0, sizeof(dispatch_trace));
    memset(&sid8_trace, 0, sizeof(sid8_trace));
    ok &= tavrn_mentorship_submit_application(&mismatch.mentorship, &application, 52u) ==
            AODV_STATUS_QUEUED &&
        tavrn_mentorship_dispatch(&mismatch.mentorship, 52u, &dispatch_trace) ==
            TAVRN_MENTORSHIP_OK && queued_sid8_rreq(&mismatch) &&
        deliver_control(&mismatch, &rrep, adva_b, TAVRN_IDENTITY_SID8, 52u,
                        &sid8_trace) == TAVRN_MENTORSHIP_OK &&
        sid8_trace.wire_type == TAVRN_WIRE_E_RREP &&
        deliver_sid8_data(&mismatch, adva_b, 2u, 52u, &sid8_trace) ==
            TAVRN_MENTORSHIP_OK && sid8_trace.wire_type == TAVRN_WIRE_DATA;
    counters = mismatch.aodv.counters;
    memset(&sid8_trace, 0, sizeof(sid8_trace));
    ok &= deliver_control(&mismatch, &collision_hello, adva_b_sid8_collision,
                          TAVRN_IDENTITY_SID16, 53u, NULL) ==
            TAVRN_MENTORSHIP_COLLISION &&
        gtt_identity_count(&mismatch.gtt_storage, adva_b_sid8_collision) == 0u &&
        memcmp(&counters, &mismatch.aodv.counters, sizeof(counters)) == 0 &&
        deliver_sid8_data(&mismatch, adva_b, 3u, 54u, &sid8_trace) ==
            TAVRN_MENTORSHIP_GATED &&
        sid8_trace.router_result == TAVRN_ROUTER_EVENT_REJOINING;
    ok &= tavrn_mentorship_state_snapshot(&mismatch.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        state.state == TAVRN_MENTORSHIP_IDENTITY_CONFLICT &&
        state.active_width == TAVRN_IDENTITY_SID16 && state.ordinary_traffic_gated != 0u;
    ok &= deliver_control(&mismatch, &hello, adva_b, TAVRN_IDENTITY_SID16, 55u,
                          NULL) == TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED;

    ok &= setup_fixture(&join_collision, 0u);
    ok &= observe_member(&join_collision, adva_b, 1u, 1u,
                         TAVRN_GTT_EVIDENCE_LIVENESS, 1u);
    ok &= tavrn_mentorship_tick(&join_collision.mentorship, 50u) ==
        TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED;
    ok &= tavrn_mentorship_tick(&join_collision.mentorship, 51u) ==
        TAVRN_MENTORSHIP_OK;
    counters = join_collision.aodv.counters;
    ok &= deliver_control(&join_collision, &join, adva_c, TAVRN_IDENTITY_SID16, 52u,
                          NULL) == TAVRN_MENTORSHIP_COLLISION &&
        memcmp(&counters, &join_collision.aodv.counters, sizeof(counters)) == 0;
    ok &= tavrn_mentorship_state_snapshot(&join_collision.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        state.state == TAVRN_MENTORSHIP_IDENTITY_CONFLICT &&
        state.ordinary_traffic_gated != 0u;

    memset(&page_offer, 0, sizeof(page_offer));
    page_offer.mentor = make_adva(adva_b);
    page_offer.mentee = make_adva(adva_a);
    page_offer.snapshot_id = 0x7704u;
    page_offer.boot_nonce = 0x5a01u;
    page_offer.snapshot_count = 1u;
    page_offer.rssi_magnitude_db = 40u;
    ok &= setup_fixture(&page_collision, 0u);
    ok &= observe_member(&page_collision, adva_b, 1u, 1u,
                         TAVRN_GTT_EVIDENCE_LIVENESS, 1u);
    ok &= begin_sync_page_wait(&page_collision, &page_offer, 1u) &&
        tavrn_mentorship_ingest_sync_data(&page_collision.mentorship,
                                           &page_offer.mentor, &collision_page,
                                           23u) == TAVRN_MENTORSHIP_COLLISION;
    ok &= tavrn_mentorship_state_snapshot(&page_collision.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        state.state == TAVRN_MENTORSHIP_IDENTITY_CONFLICT &&
        state.active_width == TAVRN_IDENTITY_SID16 && state.ordinary_traffic_gated != 0u;

    /* Own JOIN stays pending under scheduler backpressure while a simultaneous
     * relay JOIN takes a distinct fixed obligation.  Neither dedupe nor
     * accounting commits before the physical scheduler enqueue succeeds. */
    ok &= setup_fixture(&origin, 0u);
    ok &= tavrn_mentorship_tick(&origin.mentorship, 50u) ==
        TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED && fill_control_queue(&origin, 50u);
    ok &= tavrn_mentorship_state_snapshot(&origin.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK && state.join_originated == 0u &&
        pending_join_obligation_count(&origin.mentorship) == 1u &&
        tavrn_mentorship_counters(&origin.mentorship)->join_originated == 0u;
    ok &= deliver_control(&origin, &relay_join, adva_c, TAVRN_IDENTITY_SID16, 51u,
                          NULL) == TAVRN_MENTORSHIP_OK &&
        deliver_control(&origin, &relay_join, adva_c, TAVRN_IDENTITY_SID16, 51u,
                        NULL) == TAVRN_MENTORSHIP_OK &&
        pending_join_obligation_count(&origin.mentorship) == 2u &&
        tavrn_mentorship_counters(&origin.mentorship)->join_received == 0u &&
        tavrn_mentorship_counters(&origin.mentorship)->join_relayed == 0u;
    ok &= tavrn_mentorship_tick(&origin.mentorship, 52u) == TAVRN_MENTORSHIP_BUSY &&
        pending_join_obligation_count(&origin.mentorship) == 2u &&
        tavrn_mentorship_counters(&origin.mentorship)->join_originated == 0u &&
        tavrn_mentorship_counters(&origin.mentorship)->join_received == 0u;
    ok &= ble_mesh_tx_queue_remove(&origin.scheduler.routed_tx_queue, 0u, NULL) &&
        tavrn_mentorship_tick(&origin.mentorship, 53u) == TAVRN_MENTORSHIP_OK &&
        pending_join_obligation_count(&origin.mentorship) == 1u &&
        tavrn_mentorship_counters(&origin.mentorship)->join_originated == 1u &&
        tavrn_mentorship_counters(&origin.mentorship)->join_received == 0u;
    ok &= ble_mesh_tx_queue_remove(&origin.scheduler.routed_tx_queue, 1u, NULL) &&
        tavrn_mentorship_tick(&origin.mentorship, 54u) == TAVRN_MENTORSHIP_OK &&
        pending_join_obligation_count(&origin.mentorship) == 0u &&
        queued_control_count(&origin, TAVRN_WIRE_TC_UPDATE) == 2u &&
        tavrn_mentorship_counters(&origin.mentorship)->join_originated == 1u &&
        tavrn_mentorship_counters(&origin.mentorship)->join_received == 1u &&
        tavrn_mentorship_counters(&origin.mentorship)->join_relayed == 1u;
    return ok;
}

/* A mentor that already compressed its common core must reset a rebooted
 * mentee by the active SID8 identity, even though N=1 remains full-identity. */
static int test_boot_06_known_peer_reboot_after_sid8_activation(void)
{
    phase4_fixture_t mentor;
    tavrn_validated_control_t old_hello = make_n1_hello(adva_b, 0x7001u);
    tavrn_validated_control_t new_hello = make_n1_hello(adva_b, 0x7002u);
    tavrn_mentorship_event_trace_t trace;
    tavrn_mentorship_state_snapshot_t state;

    memset(&trace, 0, sizeof(trace));
    return setup_fixture(&mentor, 0u) &&
        deliver_control(&mentor, &old_hello, adva_b, TAVRN_IDENTITY_SID16, 1u,
                        NULL) == TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED &&
        tavrn_mentorship_tick(&mentor.mentorship, 50u) ==
            TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED &&
        deliver_control(&mentor, &new_hello, adva_b, TAVRN_IDENTITY_SID16, 51u,
                        &trace) == TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED &&
        trace.router_result == TAVRN_ROUTER_EVENT_OK &&
        tavrn_router_fault_reason(&mentor.router) == TAVRN_ROUTER_FAULT_NONE &&
        tavrn_mentorship_state_snapshot(&mentor.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        state.state == TAVRN_MENTORSHIP_SID8_ACTIVE &&
        mentor.mentorship.serving_session_valid != 0u &&
        mentor.mentorship.serving_boot_nonce == 0x7002u &&
        mentor.mentorship.pending_offer_valid != 0u;
}

/* BOOT-06 single-serving-session contract: an established mentor may defer a
 * second N=1, but it must preserve every field belonging to the first mentee
 * until that session expires. */
static int test_boot_06_single_serving_session(void)
{
    phase4_fixture_t mentor;
    tavrn_validated_control_t first_hello = make_n1_hello(adva_a, 0x7201u);
    tavrn_validated_control_t second_hello = make_n1_hello(adva_d, 0x7202u);
    tavrn_mentorship_snapshot_data_t serving_snapshot_before;
    tavrn_mentorship_snapshot_data_t frozen_snapshot_before;
    tavrn_adva_t serving_mentee_before;
    tavrn_mentorship_pending_control_t pending_control_before;
    tavrn_mentorship_offer_t pending_offer_before;
    uint32_t serving_deadline_before;
    uint32_t offer_suppressed_until_before;
    uint16_t serving_nonce_before;
    uint8_t pending_offer_valid_before;
    int ok = 1;

    ok &= setup_fixture_for_local(&mentor, adva_b, 0u) &&
        tavrn_mentorship_tick(&mentor.mentorship, 50u) ==
            TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED &&
        tavrn_mentorship_tick(&mentor.mentorship, 51u) == TAVRN_MENTORSHIP_OK &&
        deliver_control(&mentor, &first_hello, adva_a, TAVRN_IDENTITY_SID16,
                        52u, NULL) == TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED;
    serving_snapshot_before = mentor.mentorship.serving_snapshot;
    frozen_snapshot_before = mentor.mentorship.frozen_snapshot;
    serving_mentee_before = mentor.mentorship.serving_mentee;
    pending_control_before = mentor.mentorship.pending_control;
    pending_offer_before = mentor.mentorship.pending_offer;
    serving_deadline_before = mentor.mentorship.serving_deadline_ms;
    offer_suppressed_until_before = mentor.mentorship.offer_suppressed_until_ms;
    serving_nonce_before = mentor.mentorship.serving_boot_nonce;
    pending_offer_valid_before = mentor.mentorship.pending_offer_valid;

    ok &= mentor.mentorship.serving_session_valid != 0u &&
        adva_equal(&mentor.mentorship.serving_mentee, adva_a) &&
        deliver_control(&mentor, &second_hello, adva_d, TAVRN_IDENTITY_SID16,
                        53u, NULL) == TAVRN_MENTORSHIP_BUSY &&
        memcmp(&mentor.mentorship.serving_snapshot, &serving_snapshot_before,
               sizeof(serving_snapshot_before)) == 0 &&
        memcmp(&mentor.mentorship.frozen_snapshot, &frozen_snapshot_before,
               sizeof(frozen_snapshot_before)) == 0 &&
        memcmp(&mentor.mentorship.serving_mentee, &serving_mentee_before,
               sizeof(serving_mentee_before)) == 0 &&
        mentor.mentorship.serving_boot_nonce == serving_nonce_before &&
        mentor.mentorship.serving_deadline_ms == serving_deadline_before &&
        memcmp(&mentor.mentorship.pending_control, &pending_control_before,
               sizeof(pending_control_before)) == 0 &&
        memcmp(&mentor.mentorship.pending_offer, &pending_offer_before,
               sizeof(pending_offer_before)) == 0 &&
        mentor.mentorship.pending_offer_valid == pending_offer_valid_before &&
        mentor.mentorship.offer_suppressed_until_ms == offer_suppressed_until_before;
    ok &= deliver_control(&mentor, &first_hello, adva_a, TAVRN_IDENTITY_SID16,
                          54u, NULL) == TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED &&
        memcmp(&mentor.mentorship.serving_snapshot, &serving_snapshot_before,
               sizeof(serving_snapshot_before)) == 0 &&
        memcmp(&mentor.mentorship.frozen_snapshot, &frozen_snapshot_before,
               sizeof(frozen_snapshot_before)) == 0 &&
        memcmp(&mentor.mentorship.serving_mentee, &serving_mentee_before,
               sizeof(serving_mentee_before)) == 0 &&
        mentor.mentorship.serving_boot_nonce == serving_nonce_before &&
        mentor.mentorship.serving_deadline_ms == serving_deadline_before &&
        memcmp(&mentor.mentorship.pending_offer, &pending_offer_before,
               sizeof(pending_offer_before)) == 0 &&
        memcmp(&mentor.mentorship.pending_control, &pending_control_before,
               sizeof(pending_control_before)) == 0 &&
        mentor.mentorship.pending_offer_valid == pending_offer_valid_before &&
        mentor.mentorship.offer_suppressed_until_ms == offer_suppressed_until_before;
    ok &= deliver_control(&mentor, &second_hello, adva_d, TAVRN_IDENTITY_SID16,
                          serving_deadline_before, NULL) ==
            TAVRN_MENTORSHIP_BOOTSTRAP_ADMITTED &&
        mentor.mentorship.serving_session_valid != 0u &&
        adva_equal(&mentor.mentorship.serving_mentee, adva_d) &&
        mentor.mentorship.serving_boot_nonce == 0x7202u &&
        mentor.mentorship.serving_deadline_ms > serving_deadline_before &&
        mentor.mentorship.pending_offer_valid != 0u &&
        adva_equal(&mentor.mentorship.pending_offer.mentee, adva_d);
    return ok;
}

/* BOOT-06 deferred-relay contract: backpressure may retain two full JOINs,
 * but a later flush may commit only the first SID8 claimant. */
static int test_boot_06_revalidate_colliding_pending_joins(void)
{
    phase4_fixture_t relay;
    tavrn_validated_control_t first_join = make_join(adva_c, adva_b, 0x7801u);
    tavrn_validated_control_t second_join = make_join(
        adva_d, adva_b_sid8_collision, 0x7802u);
    tavrn_validated_control_t overflow_join = make_join(adva_b, adva_c, 0x7803u);
    tavrn_mentorship_state_snapshot_t state;
    int ok = 1;

    ok &= setup_fixture(&relay, 0u) &&
        tavrn_mentorship_tick(&relay.mentorship, 50u) ==
            TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED &&
        fill_control_queue(&relay, 50u) &&
        deliver_control(&relay, &first_join, adva_c, TAVRN_IDENTITY_SID16,
                        51u, NULL) == TAVRN_MENTORSHIP_OK &&
        deliver_control(&relay, &second_join, adva_d, TAVRN_IDENTITY_SID16,
                        51u, NULL) == TAVRN_MENTORSHIP_OK &&
        pending_join_obligation_count(&relay.mentorship) == 3u &&
        deliver_control(&relay, &overflow_join, adva_b, TAVRN_IDENTITY_SID16,
                        51u, NULL) == TAVRN_MENTORSHIP_BUSY &&
        pending_join_obligation_count(&relay.mentorship) == 3u &&
        tavrn_mentorship_counters(&relay.mentorship)->join_obligation_overflow == 1u &&
        tavrn_mentorship_tick(&relay.mentorship, 52u) == TAVRN_MENTORSHIP_BUSY;
    ok &= ble_mesh_tx_queue_remove(&relay.scheduler.routed_tx_queue, 0u, NULL) &&
        tavrn_mentorship_tick(&relay.mentorship, 53u) == TAVRN_MENTORSHIP_OK &&
        pending_join_obligation_count(&relay.mentorship) == 2u &&
        tavrn_mentorship_counters(&relay.mentorship)->join_originated == 1u;
    ok &= ble_mesh_tx_queue_remove(&relay.scheduler.routed_tx_queue, 1u, NULL) &&
        tavrn_mentorship_tick(&relay.mentorship, 54u) == TAVRN_MENTORSHIP_OK &&
        pending_join_obligation_count(&relay.mentorship) == 1u &&
        queued_control_count(&relay, TAVRN_WIRE_TC_UPDATE) == 2u &&
        gtt_identity_count(&relay.gtt_storage, adva_b) == 1u &&
        gtt_identity_count(&relay.gtt_storage, adva_b_sid8_collision) == 0u &&
        tavrn_mentorship_counters(&relay.mentorship)->join_relayed == 1u;
    ok &= ble_mesh_tx_queue_remove(&relay.scheduler.routed_tx_queue, 2u, NULL) &&
        tavrn_mentorship_tick(&relay.mentorship, 55u) == TAVRN_MENTORSHIP_COLLISION &&
        tavrn_mentorship_state_snapshot(&relay.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        state.state == TAVRN_MENTORSHIP_IDENTITY_CONFLICT &&
        state.active_width == TAVRN_IDENTITY_SID16 &&
        pending_join_obligation_count(&relay.mentorship) == 0u &&
        queued_control_count(&relay, TAVRN_WIRE_TC_UPDATE) == 0u &&
        gtt_identity_count(&relay.gtt_storage, adva_b) == 1u &&
        gtt_identity_count(&relay.gtt_storage, adva_b_sid8_collision) == 0u &&
        tavrn_mentorship_counters(&relay.mentorship)->join_received == 1u &&
        tavrn_mentorship_counters(&relay.mentorship)->join_relayed == 1u;
    return ok;
}

static int test_boot_06_join_dedupe_expiry_and_replacement(void)
{
    phase4_fixture_t duplicate_fixture;
    phase4_fixture_t wrap_fixture;
    phase4_fixture_t capacity_fixture;
    tavrn_validated_control_t join = make_join(adva_c, adva_d, 0x7901u);
    tavrn_validated_control_t capacity_join;
    uint32_t now_ms;
    uint8_t index;
    int ok = 1;

    /* One committed key suppresses its exact retransmission, then expires so
     * the same origin/sequence can represent a reboot-era reuse. */
    ok &= setup_fixture(&duplicate_fixture, 0u) &&
        deliver_control(&duplicate_fixture, &join, adva_c, TAVRN_IDENTITY_SID16,
                        1u, NULL) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&duplicate_fixture.mentorship, 2u) ==
            TAVRN_MENTORSHIP_OK &&
        remove_queued_control(&duplicate_fixture, TAVRN_WIRE_TC_UPDATE) &&
        deliver_control(&duplicate_fixture, &join, adva_c, TAVRN_IDENTITY_SID16,
                        101u, NULL) == TAVRN_MENTORSHIP_OK &&
        pending_join_obligation_count(&duplicate_fixture.mentorship) == 0u &&
        tavrn_mentorship_counters(&duplicate_fixture.mentorship)->join_duplicate == 1u &&
        deliver_control(&duplicate_fixture, &join, adva_c, TAVRN_IDENTITY_SID16,
                        102u, NULL) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&duplicate_fixture.mentorship, 102u) ==
            TAVRN_MENTORSHIP_OK &&
        remove_queued_control(&duplicate_fixture, TAVRN_WIRE_TC_UPDATE) &&
        tavrn_mentorship_counters(&duplicate_fixture.mentorship)->join_received == 2u &&
        tavrn_mentorship_counters(&duplicate_fixture.mentorship)->join_duplicate == 1u &&
        tavrn_mentorship_counters(&duplicate_fixture.mentorship)->join_dedupe_expired == 1u;

    /* Expiry uses the same signed-difference rule across the uint32_t wrap. */
    now_ms = UINT32_MAX - 20u;
    ok &= setup_fixture(&wrap_fixture, now_ms) &&
        deliver_control(&wrap_fixture, &join, adva_c, TAVRN_IDENTITY_SID16,
                        now_ms, NULL) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&wrap_fixture.mentorship, now_ms + 1u) ==
            TAVRN_MENTORSHIP_OK &&
        remove_queued_control(&wrap_fixture, TAVRN_WIRE_TC_UPDATE) &&
        deliver_control(&wrap_fixture, &join, adva_c, TAVRN_IDENTITY_SID16,
                        80u - 1u, NULL) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_counters(&wrap_fixture.mentorship)->join_duplicate == 1u &&
        deliver_control(&wrap_fixture, &join, adva_c, TAVRN_IDENTITY_SID16,
                        80u, NULL) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&wrap_fixture.mentorship, 80u) ==
            TAVRN_MENTORSHIP_OK &&
        remove_queued_control(&wrap_fixture, TAVRN_WIRE_TC_UPDATE) &&
        tavrn_mentorship_counters(&wrap_fixture.mentorship)->join_received == 2u &&
        tavrn_mentorship_counters(&wrap_fixture.mentorship)->join_dedupe_expired == 1u;

    /* Fill the cache, let only slot zero expire, and reuse it.  Slot one now
     * holds the oldest live key, so the next full admission must replace it,
     * not simply overwrite slot zero. */
    ok &= setup_fixture(&capacity_fixture, 0u);
    for (index = 0u; index < TAVRN_MENTORSHIP_JOIN_DEDUPE_CAPACITY; index++) {
        capacity_join = make_join(adva_c, adva_d, (uint16_t)(index + 1u));
        now_ms = (uint32_t)index * 2u + 1u;
        ok &= deliver_control(&capacity_fixture, &capacity_join, adva_c,
                              TAVRN_IDENTITY_SID16, now_ms, NULL) ==
                TAVRN_MENTORSHIP_OK &&
            tavrn_mentorship_tick(&capacity_fixture.mentorship, now_ms + 1u) ==
                TAVRN_MENTORSHIP_OK &&
            remove_queued_control(&capacity_fixture, TAVRN_WIRE_TC_UPDATE);
    }
    capacity_join = make_join(adva_c, adva_d, 17u);
    ok &= deliver_control(&capacity_fixture, &capacity_join, adva_c,
                          TAVRN_IDENTITY_SID16, 102u, NULL) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&capacity_fixture.mentorship, 102u) ==
            TAVRN_MENTORSHIP_OK &&
        remove_queued_control(&capacity_fixture, TAVRN_WIRE_TC_UPDATE);
    capacity_join = make_join(adva_c, adva_d, 18u);
    ok &= deliver_control(&capacity_fixture, &capacity_join, adva_c,
                          TAVRN_IDENTITY_SID16, 103u, NULL) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&capacity_fixture.mentorship, 103u) ==
            TAVRN_MENTORSHIP_OK &&
        remove_queued_control(&capacity_fixture, TAVRN_WIRE_TC_UPDATE) &&
        !join_key_present(&capacity_fixture.mentorship, adva_c, 1u) &&
        !join_key_present(&capacity_fixture.mentorship, adva_c, 2u) &&
        join_key_present(&capacity_fixture.mentorship, adva_c, 17u) &&
        join_key_present(&capacity_fixture.mentorship, adva_c, 18u) &&
        tavrn_mentorship_counters(&capacity_fixture.mentorship)->join_dedupe_expired ==
            1u &&
        tavrn_mentorship_counters(&capacity_fixture.mentorship)->join_dedupe_replaced ==
            1u;
    return ok;
}

static int test_boot_06_gates_recovery_and_join_convergence(void)
{
    phase4_fixture_t fixture;
    phase4_fixture_t mentored;
    phase4_fixture_t retrying;
    tron_application_data_t application;
    tavrn_mentorship_state_snapshot_t state;
    tavrn_mentorship_event_trace_t trace;
    tavrn_validated_control_t hello = make_n1_hello(adva_b, 0x7001u);
    tavrn_validated_control_t unknown_rreq = make_sid8_rreq(0x7eu, adva_a[0], 1u);
    tavrn_validated_control_t rrep = make_sid8_rrep(adva_a[0], adva_b[0],
                                                     adva_a[0], 1u);
    tavrn_validated_control_t sync = make_sync_data(adva_a, 0x6601u, 0u,
                                                     adva_b, 1u, 0x11u, 1u);
    tavrn_validated_control_t join = make_join(adva_c, adva_d, 0x7701u);
    tavrn_validated_control_t colliding_rreq = make_sid8_rreq(adva_b[0], adva_a[0],
                                                               2u);
    tavrn_validated_control_t join_conflict = make_join(adva_c,
                                                        adva_b_sid8_collision,
                                                        0x7702u);
    tavrn_validated_control_t local_join;
    tavrn_validated_control_t mentored_page;
    tavrn_router_phase_trace_t dispatch_trace;
    aodv_counters_t counters_before_unknown;
    aodv_counters_t counters_before_collision;
    tavrn_mentorship_status_t rrep_status;
    tavrn_mentorship_status_t data_status;
    tavrn_mentorship_snapshot_data_t mentored_snapshot;
    tavrn_mentorship_offer_t mentored_offer;
    tavrn_mentorship_offer_t retry_offer;
    tavrn_mentorship_state_snapshot_t mentored_state;
    tavrn_mentorship_state_snapshot_t retry_state;
    tavrn_mentorship_counters_t join_counters_before_conflict;
    uint8_t page_index;
    int ok = test_boot_06_production_collision_and_join_backpressure();

    memset(&application, 0, sizeof(application));
    application.final_destination = make_peer(adva_b, TAVRN_IDENTITY_SID8).logical_id;
    application.app_kind = 0x7fu;
    application.app_len = 1u;
    application.app_bytes[0] = 0xa5u;
    ok &= setup_fixture(&fixture, 0u);
    ok &= tavrn_mentorship_submit_application(&fixture.mentorship, &application, 1u) ==
        AODV_STATUS_REJOINING;
    ok &= tavrn_mentorship_tick(&fixture.mentorship, 31u) == TAVRN_MENTORSHIP_OK;
    ok &= tavrn_mentorship_tick(&fixture.mentorship, 50u) ==
        TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED;
    ok &= tavrn_mentorship_state_snapshot(&fixture.mentorship, &state) ==
            TAVRN_MENTORSHIP_OK &&
        state.state == TAVRN_MENTORSHIP_SID8_ACTIVE &&
        state.active_width == TAVRN_IDENTITY_SID8 &&
        state.ordinary_traffic_gated == 0u && state.join_originated == 0u &&
        pending_join_obligation_count(&fixture.mentorship) == 1u;
    memset(&trace, 0, sizeof(trace));
    (void)deliver_control(&fixture, &hello, adva_b, TAVRN_IDENTITY_SID16, 51u, &trace);
    ok &= trace.reached_wire_link_router != 0u && trace.wire_decode_result ==
            TAVRN_CODEC_OK && trace.wire_type == TAVRN_WIRE_HELLO &&
        tavrn_mentorship_counters(&fixture.mentorship)->bootstrap_admitted_while_sid8 != 0u;
    memset(&trace, 0, sizeof(trace));
    (void)deliver_control(&fixture, &sync, adva_b, TAVRN_IDENTITY_SID16, 52u, &trace);
    ok &= trace.reached_wire_link_router != 0u && trace.wire_type ==
            TAVRN_WIRE_SYNC_DATA;
    memset(&trace, 0, sizeof(trace));
    counters_before_unknown = fixture.aodv.counters;
    (void)deliver_control(&fixture, &unknown_rreq, adva_b, TAVRN_IDENTITY_SID8,
                          53u, &trace);
    ok &= tavrn_mentorship_counters(&fixture.mentorship)->sid8_unknown_drop != 0u &&
        memcmp(&fixture.aodv.counters, &counters_before_unknown,
               sizeof(counters_before_unknown)) == 0;
    memset(&dispatch_trace, 0, sizeof(dispatch_trace));
    ok &= tavrn_mentorship_submit_application(&fixture.mentorship, &application, 54u) ==
            AODV_STATUS_QUEUED &&
        tavrn_mentorship_dispatch(&fixture.mentorship, 54u, &dispatch_trace) ==
            TAVRN_MENTORSHIP_OK &&
        dispatch_trace.phase == TAVRN_ROUTER_TRACE_DISPATCH &&
        queued_sid8_rreq(&fixture);
    memset(&trace, 0, sizeof(trace));
    rrep_status = deliver_control(&fixture, &rrep, adva_b, TAVRN_IDENTITY_SID8,
                                  55u, &trace);
    ok &= rrep_status == TAVRN_MENTORSHIP_OK &&
        trace.reached_wire_link_router != 0u && trace.wire_decode_result ==
            TAVRN_CODEC_OK && trace.wire_type == TAVRN_WIRE_E_RREP;
    memset(&trace, 0, sizeof(trace));
    data_status = deliver_sid8_data(&fixture, adva_b, 2u, 56u, &trace);
    ok &= data_status == TAVRN_MENTORSHIP_OK &&
        trace.reached_wire_link_router != 0u && trace.wire_decode_result ==
            TAVRN_CODEC_OK && trace.wire_type == TAVRN_WIRE_DATA;
    memset(&dispatch_trace, 0, sizeof(dispatch_trace));
    ok &= tavrn_mentorship_dispatch(&fixture.mentorship, 57u, &dispatch_trace) ==
            TAVRN_MENTORSHIP_OK &&
        dispatch_trace.phase == TAVRN_ROUTER_TRACE_DISPATCH;
    ok &= observe_member(&fixture, adva_b_sid8_collision, 3u, 1u,
                         TAVRN_GTT_EVIDENCE_LIVENESS, 58u);
    counters_before_collision = fixture.aodv.counters;
    memset(&trace, 0, sizeof(trace));
    (void)deliver_control(&fixture, &colliding_rreq, adva_b,
                          TAVRN_IDENTITY_SID8, 58u, &trace);
    ok &= tavrn_mentorship_counters(&fixture.mentorship)->sid8_collision_drop != 0u &&
        memcmp(&fixture.aodv.counters, &counters_before_collision,
               sizeof(counters_before_collision)) == 0;
    memset(&trace, 0, sizeof(trace));
    (void)deliver_control(&fixture, &join, adva_c, TAVRN_IDENTITY_SID16, 59u, &trace);
    (void)deliver_control(&fixture, &join, adva_c, TAVRN_IDENTITY_SID16, 60u, &trace);
    memset(&local_join, 0, sizeof(local_join));
    ok &= tavrn_mentorship_tick(&fixture.mentorship, 60u) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&fixture.mentorship, 61u) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_build_join(&fixture.mentorship, 0x7702u, &local_join) ==
            TAVRN_MENTORSHIP_OK &&
        local_join.type == TAVRN_WIRE_TC_UPDATE && local_join.pdu_len == 24u &&
        local_join.pdu[6] == 0xf0u &&
        tavrn_mentorship_counters(&fixture.mentorship)->join_received == 1u &&
        tavrn_mentorship_counters(&fixture.mentorship)->join_duplicate == 0u &&
        tavrn_mentorship_counters(&fixture.mentorship)->join_relayed == 1u;
    join_counters_before_conflict = *tavrn_mentorship_counters(&fixture.mentorship);
    memset(&trace, 0, sizeof(trace));
    ok &= deliver_control(&fixture, &join_conflict, adva_c,
                          TAVRN_IDENTITY_SID16, 61u, &trace) ==
            TAVRN_MENTORSHIP_COLLISION &&
        tavrn_mentorship_counters(&fixture.mentorship)->join_relayed ==
            join_counters_before_conflict.join_relayed;

    /* A selected mentor gets one initial pull plus at most two retries.  The
     * third page timeout restarts full-identity HELLO without activation. */
    memset(&retry_offer, 0, sizeof(retry_offer));
    ok &= setup_fixture(&retrying, 0u);
    retry_offer.mentor = make_adva(adva_b);
    retry_offer.mentee = make_adva(adva_a);
    retry_offer.snapshot_id = 0x7703u;
    retry_offer.boot_nonce = 0x5a01u;
    retry_offer.snapshot_count = 1u;
    retry_offer.rssi_magnitude_db = 40u;
    ok &= tavrn_mentorship_collect_offer(&retrying.mentorship, &retry_offer, 1u) ==
            TAVRN_MENTORSHIP_OFFER_ACCEPTED &&
        tavrn_mentorship_tick(&retrying.mentorship, 21u) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&retrying.mentorship, 22u) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&retrying.mentorship, 52u) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&retrying.mentorship, 53u) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&retrying.mentorship, 83u) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&retrying.mentorship, 84u) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&retrying.mentorship, 114u) ==
            TAVRN_MENTORSHIP_RESTARTED &&
        tavrn_mentorship_state_snapshot(&retrying.mentorship, &retry_state) ==
            TAVRN_MENTORSHIP_OK && retry_state.restart_count == 1u &&
        retry_state.ordinary_traffic_gated != 0u;

    /* JOIN has the same bounded full-identity origin rule after a complete
     * selected mentorship session, not only after the no-offer path above. */
    memset(&mentored_snapshot, 0, sizeof(mentored_snapshot));
    memset(&mentored_offer, 0, sizeof(mentored_offer));
    ok &= setup_fixture(&mentored, 0u) && seed_retained_records(&mentored);
    ok &= tavrn_mentorship_freeze_snapshot(&mentored.mentorship, 0x8801u, 150u,
                                            &mentored_snapshot) ==
        TAVRN_MENTORSHIP_OK;
    mentored_offer.mentor = make_adva(adva_b);
    mentored_offer.mentee = make_adva(adva_a);
    mentored_offer.snapshot_id = mentored_snapshot.snapshot_id;
    mentored_offer.boot_nonce = 0x5a01u;
    mentored_offer.snapshot_count = mentored_snapshot.count;
    mentored_offer.rssi_magnitude_db = 40u;
    ok &= begin_sync_page_wait(&mentored, &mentored_offer, 151u) &&
        adva_equal(&mentored.mentorship.state.selected_mentor, adva_b);
    for (page_index = 0u; page_index < mentored_snapshot.count; page_index++) {
        uint32_t page_at_ms = 173u + (uint32_t)page_index * 2u;

        memset(&mentored_page, 0, sizeof(mentored_page));
        ok &= tavrn_mentorship_build_sync_data(&mentored_snapshot,
                                               &mentored_offer.mentor,
                                               &mentored_offer.mentee, page_index,
                                               &mentored_page) == TAVRN_MENTORSHIP_OK;
        if (page_index + 1u < mentored_snapshot.count) {
            ok &= deliver_control(&mentored, &mentored_page, adva_b,
                                  TAVRN_IDENTITY_SID16, page_at_ms, NULL) ==
                    TAVRN_MENTORSHIP_OK &&
                tavrn_mentorship_tick(&mentored.mentorship, page_at_ms + 1u) ==
                    TAVRN_MENTORSHIP_OK;
        } else {
            ok &= tavrn_mentorship_ingest_sync_data(&mentored.mentorship,
                                                    &mentored_offer.mentor,
                                                    &mentored_page, page_at_ms) ==
                TAVRN_MENTORSHIP_OK;
        }
    }
    ok &= tavrn_mentorship_activate_sid8(&mentored.mentorship, 180u) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_state_snapshot(&mentored.mentorship, &mentored_state) ==
            TAVRN_MENTORSHIP_OK &&
        mentored_state.state == TAVRN_MENTORSHIP_SID8_ACTIVE &&
        mentored_state.join_originated == 0u &&
        pending_join_obligation_count(&mentored.mentorship) == 1u;
    return ok;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "--esc") != 0 &&
                      strcmp(argv[1], "--bootstrap") != 0)) {
        fprintf(stderr, "usage: %s --esc|--bootstrap\n", argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "--esc") == 0) {
        CHECK("ESC-01", test_esc_01_fixed_k_and_retained_snapshot());
        CHECK("ESC-02", test_esc_02_context_fail_closed());
        CHECK("ESC-03", test_esc_03_atomic_width_transitions());
    } else {
        CHECK("BOOT-01", test_boot_01_full_identity_and_real_composed_admission());
        CHECK("BOOT-02", test_boot_02_deterministic_offer_timing_and_suppression());
        CHECK("BOOT-03", test_boot_03_total_offer_selection_order() &&
              test_boot_03_offer_capacity_retention());
        CHECK("BOOT-04", test_boot_04_immutable_canonical_snapshot());
        CHECK("BOOT-05", test_boot_05_one_entry_pages_and_fresh_merge() &&
              test_boot_05_broadcast_production_exchange() &&
              test_boot_05_deadlines_ttl_and_sync_dedupe());
        CHECK("BOOT-06", test_boot_06_gates_recovery_and_join_convergence() &&
               test_boot_06_known_peer_reboot_after_sid8_activation() &&
               test_boot_06_single_serving_session() &&
               test_boot_06_revalidate_colliding_pending_joins() &&
               test_boot_06_join_dedupe_expiry_and_replacement());
    }
    if (failures != 0u) {
        printf("tavrn_phase4 %s RED tests failed: %u assertion(s)\n",
               strcmp(argv[1], "--esc") == 0 ? "ESC" : "BOOTSTRAP", failures);
        return 1;
    }
    printf("tavrn_phase4 %s tests passed\n",
           strcmp(argv[1], "--esc") == 0 ? "ESC" : "BOOTSTRAP");
    return 0;
}
