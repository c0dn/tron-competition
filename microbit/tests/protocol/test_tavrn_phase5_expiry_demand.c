#include "aodv_core.h"
#include "ble_mesh_scheduler.h"
#include "tavrn_full.h"
#include "tavrn_gtt.h"
#include "tavrn_link_v2.h"
#include "tavrn_maintenance.h"
#include "tavrn_mentorship.h"
#include "tavrn_phase5_expiry_demand_contract.h"
#include "tavrn_router.h"
#include "routed_full_telemetry.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Temporary source compatibility for the remaining helper bodies: RED must
 * resolve no future production symbol even when one later appears. */
#if defined(TAVRN_PHASE5_EXPIRY_RED_MODE)
#define tavrn_gtt_observe_with_provenance phase5_gtt_observe
#define tavrn_gtt_expiry_snapshot phase5_gtt_snapshot
#define tavrn_gtt_enumerate_known phase5_gtt_enumerate_known
#define tavrn_gtt_application_request phase5_gtt_request
#define tavrn_gtt_application_cancel phase5_gtt_cancel
#define tavrn_gtt_apply_application_command phase5_gtt_apply_application_command
#define tavrn_gtt_sync_merge phase5_gtt_sync_merge
#define aodv_core_subject_demand_snapshot phase5_aodv_demand
#define tavrn_link_v2_subject_demand_snapshot phase5_link_demand
#define tavrn_router_subject_demand_snapshot phase5_router_demand
#define tavrn_full_resolve_unique_sid8 phase5_full_resolve_sid8
#define tavrn_maintenance_demand_snapshot phase5_maintenance_demand
#define tavrn_maintenance_checked_local_departure phase5_maintenance_checked_departure
#define tavrn_maintenance_sweep_expiry phase5_maintenance_sweep
#endif

static unsigned int failures;
static const char *reported[7];
static unsigned int reported_count;

#define NETWORK_ID 0x2au

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
    0x34u, 0x56u, 0x78u, 0x9au, 0xbcu, 0xc2u,
};
/* Shares B's compressed SID8 while remaining a distinct canonical AdvA. */
static const uint8_t adva_b_alias[TAVRN_ADVA_LEN] = {
    0xdcu, 0x57u, 0x28u, 0x4au, 0x6cu, 0xc3u,
};

typedef struct expiry_fixture {
    ble_mesh_scheduler_t scheduler;
    tavrn_link_v2_t link;
    aodv_core_t aodv;
    tavrn_gtt_t gtt;
    tavrn_gtt_storage_t gtt_storage;
    tavrn_full_t full;
    tavrn_router_t router;
    tavrn_mentorship_t mentorship;
    tavrn_maintenance_t maintenance;
} expiry_fixture_t;

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

static tavrn_adva_t adva(const uint8_t bytes[TAVRN_ADVA_LEN])
{
    tavrn_adva_t result;

    memcpy(result.bytes, bytes, TAVRN_ADVA_LEN);
    return result;
}

static tavrn_adva_t generated_adva(uint8_t low)
{
    tavrn_adva_t result = { .bytes = { low, (uint8_t)(0x40u + low), 0x21u,
                                       0x43u, 0x65u, 0xc0u } };
    return result;
}

static tavrn_direct_peer_t peer(const uint8_t bytes[TAVRN_ADVA_LEN],
                                tavrn_identity_width_t width)
{
    tavrn_direct_peer_t result;

    memset(&result, 0, sizeof(result));
    result.logical_id.width = width;
    result.logical_id.value = width == TAVRN_IDENTITY_SID8 ? bytes[0] :
        (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    result.adva = adva(bytes);
    return result;
}

static tavrn_link_config_t link_config(void)
{
    tavrn_link_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = peer(adva_a, TAVRN_IDENTITY_SID16);
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

static aodv_core_config_t aodv_config(void)
{
    aodv_core_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = peer(adva_a, TAVRN_IDENTITY_SID16);
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

static int fixture_init(expiry_fixture_t *fixture, uint32_t now_ms)
{
    tavrn_gtt_config_t gtt_config;
    tavrn_router_incarnation_config_t incarnation;
    tavrn_mentorship_config_t mentorship_config;
    tavrn_maintenance_config_t maintenance_config;
    tavrn_router_augmentation_hooks_t hooks;
    tavrn_link_config_t configured_link;
    aodv_core_config_t configured_aodv;

    if (fixture == NULL) {
        return 0;
    }
    memset(fixture, 0, sizeof(*fixture));
    memset(&gtt_config, 0, sizeof(gtt_config));
    gtt_config.local_identity = adva(adva_a);
    gtt_config.soft_expiry_ms = 150u;
    gtt_config.hard_expiry_ms = 300u;
    gtt_config.departed_retention_ms = 600u;
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
    maintenance_config.hello_dedupe_ms = 10u;
    maintenance_config.initial_node_sequence = 1u;
    configured_link = link_config();
    configured_aodv = aodv_config();
    ble_mesh_scheduler_init(&fixture->scheduler, now_ms, adva_a);
    if (tavrn_link_v2_init(&fixture->link, &fixture->scheduler, &configured_link, now_ms) !=
            TAVRN_LINK_INIT_OK ||
        aodv_core_init(&fixture->aodv, &configured_aodv, now_ms) != AODV_INIT_OK ||
        tavrn_gtt_init(&fixture->gtt, &fixture->gtt_storage, &gtt_config, now_ms) !=
            TAVRN_GTT_INIT_OK ||
        tavrn_full_init(&fixture->full, &fixture->gtt) != TAVRN_FULL_INIT_OK) {
        return 0;
    }
    hooks = tavrn_full_router_hooks(&fixture->full);
    return tavrn_router_init_with_incarnation(&fixture->router, &fixture->link,
                                               &fixture->aodv, &hooks, &incarnation,
                                               now_ms) == TAVRN_ROUTER_INCARNATION_OK &&
        tavrn_mentorship_init(&fixture->mentorship, &fixture->router, &fixture->gtt,
                              &mentorship_config, now_ms) == TAVRN_MENTORSHIP_OK &&
        tavrn_maintenance_init(&fixture->maintenance, &fixture->router, &fixture->gtt,
                                &maintenance_config) == TAVRN_MAINTENANCE_OK;
}

static void clear_queued_controls(expiry_fixture_t *fixture)
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

static int observe_member(expiry_fixture_t *fixture,
                          const uint8_t identity[TAVRN_ADVA_LEN],
                          uint16_t serial, uint32_t now_ms)
{
    tavrn_gtt_evidence_t input;
    tavrn_gtt_observe_status_t status;

    if (fixture == NULL) {
        return 0;
    }
    memset(&input, 0, sizeof(input));
    input.identity = adva(identity);
    input.serial = serial;
    input.serial_present = 1u;
    input.hop_count = 1u;
    input.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    status = tavrn_gtt_observe(&fixture->gtt, &input, now_ms);
    return status == TAVRN_GTT_OBSERVE_ADDED ||
        status == TAVRN_GTT_OBSERVE_REFRESHED;
}

/* This is the real FULL mentorship handover used by Phase 5 maintenance: the
 * SID8 demand matrix must not seed compressed state in a SID16 fixture. */
static int activate_sid8(expiry_fixture_t *fixture, uint32_t start_ms)
{
    tavrn_mentorship_state_snapshot_t state;
    uint32_t activated_at = start_ms + 20u;

    if (fixture == NULL ||
        tavrn_maintenance_activate(&fixture->maintenance, start_ms) !=
            TAVRN_MAINTENANCE_GATED ||
        !observe_member(fixture, adva_b, 0xfffeu, start_ms + 1u) ||
        tavrn_mentorship_tick(&fixture->mentorship, activated_at) !=
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
            TAVRN_ROUTER_INCARNATION_ESTABLISHED &&
        tavrn_maintenance_activate(&fixture->maintenance, activated_at) ==
            TAVRN_MAINTENANCE_OK;
}

static int fixture_init_sid8(expiry_fixture_t *fixture)
{
    return fixture_init(fixture, 0u) && activate_sid8(fixture, 0u);
}

static int fixture_init_sid8_at(expiry_fixture_t *fixture, uint32_t start_ms)
{
    return fixture_init(fixture, start_ms) && activate_sid8(fixture, start_ms);
}

static int fixture_reconfigure_maintenance(
    expiry_fixture_t *fixture, const tavrn_maintenance_config_t *config)
{
    return fixture != NULL && config != NULL &&
        tavrn_maintenance_init(&fixture->maintenance, &fixture->router,
                               &fixture->gtt, config) == TAVRN_MAINTENANCE_OK;
}

/* Owner-order vectors use a short exact hard-expiry interval so one genuine
 * topology cadence can coincide with the expiry boundary. */
static int fixture_reconfigure_expiry(
    expiry_fixture_t *fixture, uint32_t soft_expiry_ms, uint32_t hard_expiry_ms,
    uint32_t departed_retention_ms, uint32_t now_ms)
{
    tavrn_gtt_config_t config;

    if (fixture == NULL) {
        return 0;
    }
    config = fixture->gtt.config;
    config.soft_expiry_ms = soft_expiry_ms;
    config.hard_expiry_ms = hard_expiry_ms;
    config.departed_retention_ms = departed_retention_ms;
    return tavrn_gtt_init(&fixture->gtt, &fixture->gtt_storage, &config, now_ms) ==
        TAVRN_GTT_INIT_OK;
}

static int advance_maintenance_to_stable(expiry_fixture_t *fixture,
                                         uint32_t *now_out)
{
    tavrn_maintenance_snapshot_t snapshot;
    tavrn_maintenance_status_t tick_status;
    uint32_t now_ms = 0u;
    uint8_t iteration;

    if (fixture == NULL ||
        tavrn_maintenance_snapshot(&fixture->maintenance, &snapshot) !=
            TAVRN_MAINTENANCE_OK) {
        return 0;
    }
    for (iteration = 0u;
         snapshot.current_interval_ms != fixture->maintenance.config.hello_stable_ms &&
         iteration < 16u;
         iteration++) {
        now_ms = snapshot.next_hello_due_ms;
        tick_status = tavrn_maintenance_tick(&fixture->maintenance, now_ms);
        if (tick_status != TAVRN_MAINTENANCE_OK) {
            return 0;
        }
        clear_queued_controls(fixture);
        if (tavrn_maintenance_snapshot(&fixture->maintenance, &snapshot) !=
            TAVRN_MAINTENANCE_OK) {
            return 0;
        }
    }
    if (snapshot.current_interval_ms != fixture->maintenance.config.hello_stable_ms) {
        return 0;
    }
    if (now_out != NULL) {
        *now_out = now_ms;
    }
    return 1;
}

static int adva_equal(const tavrn_adva_t *left, const tavrn_adva_t *right)
{
    return left != NULL && right != NULL &&
        memcmp(left->bytes, right->bytes, TAVRN_ADVA_LEN) == 0;
}

static int peer_equal(const tavrn_direct_peer_t *left,
                      const tavrn_direct_peer_t *right)
{
    return left != NULL && right != NULL &&
        left->logical_id.width == right->logical_id.width &&
        left->logical_id.value == right->logical_id.value &&
        adva_equal(&left->adva, &right->adva);
}

static tron_application_data_t application(tavrn_logical_id_t destination)
{
    tron_application_data_t result;

    memset(&result, 0, sizeof(result));
    result.final_destination = destination;
    result.app_kind = 0x7fu;
    result.app_source = 0u;
    result.app_len = 1u;
    result.app_bytes[0] = 0xa5u;
    return result;
}

static tavrn_link_data_t link_data(tavrn_logical_id_t origin,
                                   tavrn_logical_id_t destination,
                                   uint16_t sequence)
{
    tavrn_link_data_t result;

    memset(&result, 0, sizeof(result));
    result.origin = origin;
    result.final_destination = destination;
    result.data_seq = sequence;
    result.ttl = 15u;
    result.app_kind = 0x7fu;
    result.app_source = 0u;
    result.app_len = 1u;
    result.app_bytes[0] = 0xa5u;
    result.ownership = TAVRN_DATA_TRANSIT;
    return result;
}

static int make_committed_rx_event(const tavrn_decoded_frame_t *frame,
                                    const tavrn_direct_peer_t *local,
                                    const uint8_t outer_adva[TAVRN_ADVA_LEN],
                                    ble_mesh_sched_event_t *event_out)
{
    tavrn_codec_config_t codec;
    size_t encoded_len = 0u;

    if (frame == NULL || local == NULL || outer_adva == NULL || event_out == NULL) {
        return 0;
    }
    memset(&codec, 0, sizeof(codec));
    codec.network_id = NETWORK_ID;
    codec.local_peer = *local;
    memset(event_out, 0, sizeof(*event_out));
    event_out->type = BLE_MESH_SCHED_EVENT_RX_ADV;
    event_out->rssi_magnitude_db = 55u;
    memcpy(event_out->adv_addr, outer_adva, TAVRN_ADVA_LEN);
    if (tavrn_wire_v2_encode(&codec, frame, event_out->adv_data,
                             sizeof(event_out->adv_data), &encoded_len) != TAVRN_CODEC_OK ||
        encoded_len > sizeof(event_out->adv_data)) {
        return 0;
    }
    event_out->adv_len = (uint8_t)encoded_len;
    return 1;
}

static tavrn_router_delivery_status_t accept_delivery_reservation(
    void *context, const tavrn_direct_peer_t *transmitter,
    const tavrn_link_data_t *data, tavrn_router_delivery_token_t *token_out,
    uint32_t now_ms)
{
    (void)context;
    (void)now_ms;
    if (transmitter == NULL || data == NULL || token_out == NULL) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    *token_out = 1u;
    return TAVRN_ROUTER_DELIVERY_OK;
}

static tavrn_router_delivery_status_t accept_delivery_completion(
    void *context, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t now_ms)
{
    (void)context;
    (void)now_ms;
    if (token == TAVRN_ROUTER_DELIVERY_TOKEN_NONE || transmitter == NULL ||
        data == NULL) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    return TAVRN_ROUTER_DELIVERY_OK;
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

static tavrn_validated_control_t sync_data_page(const tavrn_adva_t *mentee,
                                                uint16_t snapshot_id,
                                                const tavrn_adva_t *identity,
                                                uint16_t serial,
                                                uint8_t ttl_bucket,
                                                uint8_t hop_count)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = TAVRN_WIRE_SYNC_DATA;
    control.pdu_len = 24u;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = NETWORK_ID;
    control.pdu[4] = TAVRN_WIRE_SYNC_DATA;
    control.pdu[5] = 0xc0u;
    memcpy(&control.pdu[6], mentee->bytes, TAVRN_ADVA_LEN);
    pdu_put_u16(&control, 12u, snapshot_id);
    control.pdu[14] = 0u;
    memcpy(&control.pdu[15], identity->bytes, TAVRN_ADVA_LEN);
    pdu_put_u16(&control, 21u, serial);
    control.pdu[23] = (uint8_t)(ttl_bucket << 4) | (hop_count & 0x0fu);
    return control;
}

static int begin_expiry_sync_page_wait(expiry_fixture_t *fixture,
                                       const tavrn_adva_t *mentor,
                                       uint16_t snapshot_id, uint32_t now_ms)
{
    tavrn_mentorship_offer_t offer;
    uint32_t selection_at_ms;

    if (fixture == NULL || mentor == NULL) {
        return 0;
    }
    memset(&offer, 0, sizeof(offer));
    offer.mentor = *mentor;
    offer.mentee = fixture->gtt.config.local_identity;
    offer.snapshot_id = snapshot_id;
    offer.boot_nonce = 0x5a01u;
    offer.snapshot_count = 1u;
    offer.rssi_magnitude_db = 40u;
    selection_at_ms = now_ms + fixture->mentorship.config.offer_window_ms;
    return tavrn_mentorship_collect_offer(&fixture->mentorship, &offer, now_ms) ==
               TAVRN_MENTORSHIP_OFFER_ACCEPTED &&
        tavrn_mentorship_tick(&fixture->mentorship, selection_at_ms) ==
            TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&fixture->mentorship, selection_at_ms + 1u) ==
            TAVRN_MENTORSHIP_OK &&
        fixture->mentorship.page_deadline_valid != 0u;
}

static int source_file_token_count(const char *source_path, const char *needle,
                                   size_t *first_offset_out,
                                   size_t *count_out)
{
    FILE *file;
    unsigned char chunk[257];
    size_t matched = 0u;
    size_t length;
    size_t index;
    size_t offset = 0u;
    size_t count = 0u;

    if (source_path == NULL || needle == NULL || needle[0] == '\0') {
        return 0;
    }
    if (first_offset_out != NULL) {
        *first_offset_out = 0u;
    }
    if (count_out != NULL) {
        *count_out = 0u;
    }
    file = fopen(source_path, "rb");
    if (file == NULL) {
        return 0;
    }
    while ((length = fread(chunk, 1u, sizeof(chunk), file)) != 0u) {
        for (index = 0u; index < length; index++) {
            if (chunk[index] == (unsigned char)needle[matched]) {
                matched++;
                if (needle[matched] == '\0') {
                    count++;
                    if (count == 1u && first_offset_out != NULL) {
                        *first_offset_out = offset + index + 1u - strlen(needle);
                    }
                    matched = 0u;
                }
            } else {
                matched = chunk[index] == (unsigned char)needle[0] ? 1u : 0u;
            }
        }
        offset += length;
    }
    if (ferror(file) != 0 || fclose(file) != 0) {
        return 0;
    }
    if (count_out != NULL) {
        *count_out = count;
    }
    return 1;
}

static int mentorship_source_contains(const char *needle)
{
#ifndef TAVRN_PHASE5_MENTORSHIP_SOURCE_PATH
    (void)needle;
    return 0;
#else
    size_t count = 0u;

    return source_file_token_count(TAVRN_PHASE5_MENTORSHIP_SOURCE_PATH, needle,
                                   NULL, &count) && count != 0u;
#endif
}

static int mentorship_sync_source_uses_central_merge(void)
{
    return mentorship_source_contains("tavrn_gtt_sync_merge(") &&
        !mentorship_source_contains("entry->soft_deadline_ms =") &&
        !mentorship_source_contains("entry->hard_deadline_ms =") &&
        !mentorship_source_contains("*mentorship->gtt->storage = candidate_storage");
}

static int c_skip_literal_if_zero(FILE *file)
{
    int ch;
    int at_line_start = 1;
    int depth = 1;

    while ((ch = fgetc(file)) != EOF) {
        if (at_line_start != 0 && ch == '#') {
            char directive[16];
            size_t length = 0u;

            while ((ch = fgetc(file)) != EOF && isspace((unsigned char)ch) && ch != '\n') {
            }
            while (ch != EOF && (isalpha((unsigned char)ch) || ch == '_')) {
                if (length + 1u < sizeof(directive)) {
                    directive[length++] = (char)ch;
                }
                ch = fgetc(file);
            }
            directive[length] = '\0';
            if (strcmp(directive, "if") == 0 || strcmp(directive, "ifdef") == 0 ||
                strcmp(directive, "ifndef") == 0) {
                depth++;
            } else if (strcmp(directive, "endif") == 0) {
                depth--;
                if (depth == 0) {
                    while (ch != EOF && ch != '\n') {
                        ch = fgetc(file);
                    }
                    return ch == EOF ? -1 : 1;
                }
            }
            while (ch != EOF && ch != '\n') {
                ch = fgetc(file);
            }
            at_line_start = 1;
            continue;
        }
        at_line_start = ch == '\n';
    }
    return -1;
}

/* Streaming C lexer for source-closure checks. It strips literal #if 0 blocks,
 * comments, and string/character literals; it never buffers a source file. */
static int c_lex_next(FILE *file, char token[128])
{
    int ch;
    int next;
    size_t length;

    if (file == NULL || token == NULL) {
        return -1;
    }
    for (;;) {
        ch = fgetc(file);
        if (ch == EOF) {
            return 0;
        }
        if (isspace((unsigned char)ch)) {
            continue;
        }
        if (ch == '#') {
            char directive[16];
            size_t length = 0u;
            int first_value;

            while ((ch = fgetc(file)) != EOF && isspace((unsigned char)ch) && ch != '\n') {
            }
            while (ch != EOF && (isalpha((unsigned char)ch) || ch == '_')) {
                if (length + 1u < sizeof(directive)) {
                    directive[length++] = (char)ch;
                }
                ch = fgetc(file);
            }
            directive[length] = '\0';
            while (ch != EOF && isspace((unsigned char)ch) && ch != '\n') {
                ch = fgetc(file);
            }
            first_value = ch;
            while (ch != EOF && ch != '\n') {
                ch = fgetc(file);
            }
            if (strcmp(directive, "if") == 0 && first_value == '0' &&
                c_skip_literal_if_zero(file) < 0) {
                return -1;
            }
            continue;
        }
        if (ch == '/') {
            next = fgetc(file);
            if (next == '/') {
                while ((ch = fgetc(file)) != EOF && ch != '\n') {
                }
                continue;
            }
            if (next == '*') {
                int previous = 0;

                while ((ch = fgetc(file)) != EOF) {
                    if (previous == '*' && ch == '/') {
                        break;
                    }
                    previous = ch;
                }
                if (ch == EOF) {
                    return -1;
                }
                continue;
            }
            if (next != EOF) {
                (void)ungetc(next, file);
            }
            token[0] = '/';
            token[1] = '\0';
            return 1;
        }
        if (ch == '"' || ch == '\'') {
            int quote = ch;
            int escaped = 0;

            while ((ch = fgetc(file)) != EOF) {
                if (escaped != 0) {
                    escaped = 0;
                } else if (ch == '\\') {
                    escaped = 1;
                } else if (ch == quote) {
                    break;
                }
            }
            if (ch == EOF) {
                return -1;
            }
            continue;
        }
        if (isalpha((unsigned char)ch) || ch == '_') {
            length = 0u;
            do {
                if (length + 1u >= 128u) {
                    return -1;
                }
                token[length++] = (char)ch;
                ch = fgetc(file);
            } while (ch != EOF && (isalnum((unsigned char)ch) || ch == '_'));
            if (ch != EOF) {
                (void)ungetc(ch, file);
            }
            token[length] = '\0';
            return 1;
        }
        token[0] = (char)ch;
        token[1] = '\0';
        if ((ch == '-' || ch == '=') && (next = fgetc(file)) != EOF) {
            if ((ch == '-' && next == '>') || (ch == '=' && next == '=')) {
                token[1] = (char)next;
                token[2] = '\0';
            } else {
                (void)ungetc(next, file);
            }
        }
        return 1;
    }
}

static int lexical_function_identifier_count(const char *source_path,
                                             const char *function_name,
                                             const char *wanted,
                                             size_t *count_out)
{
    FILE *file;
    char token[128];
    int status;
    int candidate = 0;
    int signature = 0;
    int parentheses = 0;
    int in_body = 0;
    int braces = 0;
    int found = 0;
    size_t count = 0u;

    if (source_path == NULL || function_name == NULL || wanted == NULL ||
        count_out == NULL) {
        return 0;
    }
    *count_out = 0u;
    file = fopen(source_path, "rb");
    if (file == NULL) {
        return 0;
    }
    while ((status = c_lex_next(file, token)) > 0) {
        if (in_body != 0) {
            if (strcmp(token, "{") == 0) {
                braces++;
            } else if (strcmp(token, "}") == 0) {
                braces--;
                if (braces == 0) {
                    in_body = 0;
                }
            } else if (strcmp(token, wanted) == 0) {
                count++;
            }
            continue;
        }
        if (candidate == 0) {
            if (strcmp(token, function_name) == 0) {
                candidate = 1;
            }
            continue;
        }
        if (signature == 0) {
            if (strcmp(token, "(") == 0) {
                signature = 1;
                parentheses = 1;
            } else {
                candidate = strcmp(token, function_name) == 0;
            }
            continue;
        }
        if (strcmp(token, "(") == 0) {
            parentheses++;
        } else if (strcmp(token, ")") == 0) {
            parentheses--;
        } else if (parentheses == 0 && strcmp(token, "{") == 0) {
            in_body = 1;
            braces = 1;
            found = 1;
            candidate = 0;
            signature = 0;
        } else if (parentheses == 0 && strcmp(token, ";") == 0) {
            candidate = 0;
            signature = 0;
        }
    }
    if (fclose(file) != 0 || status < 0 || found == 0) {
        return 0;
    }
    *count_out = count;
    return 1;
}

static int lexical_function_call_count(const char *source_path,
                                       const char *function_name,
                                       const char *wanted,
                                       size_t *count_out)
{
    FILE *file;
    char token[128];
    int status;
    int candidate = 0;
    int signature = 0;
    int parentheses = 0;
    int in_body = 0;
    int braces = 0;
    int found = 0;
    int wanted_pending = 0;
    size_t count = 0u;

    if (source_path == NULL || function_name == NULL || wanted == NULL ||
        count_out == NULL) {
        return 0;
    }
    *count_out = 0u;
    file = fopen(source_path, "rb");
    if (file == NULL) {
        return 0;
    }
    while ((status = c_lex_next(file, token)) > 0) {
        if (in_body != 0) {
            if (wanted_pending != 0) {
                if (strcmp(token, "(") == 0) {
                    count++;
                }
                wanted_pending = 0;
            }
            if (strcmp(token, "{") == 0) {
                braces++;
            } else if (strcmp(token, "}") == 0) {
                braces--;
                if (braces == 0) {
                    in_body = 0;
                }
            }
            if (strcmp(token, wanted) == 0) {
                wanted_pending = 1;
            }
            continue;
        }
        if (candidate == 0) {
            if (strcmp(token, function_name) == 0) {
                candidate = 1;
            }
            continue;
        }
        if (signature == 0) {
            if (strcmp(token, "(") == 0) {
                signature = 1;
                parentheses = 1;
            } else {
                candidate = strcmp(token, function_name) == 0;
            }
            continue;
        }
        if (strcmp(token, "(") == 0) {
            parentheses++;
        } else if (strcmp(token, ")") == 0) {
            parentheses--;
        } else if (parentheses == 0 && strcmp(token, "{") == 0) {
            in_body = 1;
            braces = 1;
            found = 1;
            candidate = 0;
            signature = 0;
        } else if (parentheses == 0 && strcmp(token, ";") == 0) {
            candidate = 0;
            signature = 0;
        }
    }
    if (fclose(file) != 0 || status < 0 || found == 0) {
        return 0;
    }
    *count_out = count;
    return 1;
}

static int lexical_source_token_count(const char *source_path, const char *wanted,
                                      size_t *count_out)
{
    FILE *file;
    char token[128];
    int status;
    size_t count = 0u;

    if (source_path == NULL || wanted == NULL || count_out == NULL) {
        return 0;
    }
    *count_out = 0u;
    file = fopen(source_path, "rb");
    if (file == NULL) {
        return 0;
    }
    while ((status = c_lex_next(file, token)) > 0) {
        if (strcmp(token, wanted) == 0) {
            count++;
        }
    }
    if (fclose(file) != 0 || status < 0) {
        return 0;
    }
    *count_out = count;
    return 1;
}

static int lexical_sequence_in_function(const char *source_path,
                                        const char *function_name,
                                        const char *const *sequence,
                                        size_t sequence_count)
{
    FILE *file;
    char token[128];
    int status;
    int candidate = 0;
    int signature = 0;
    int parentheses = 0;
    int in_body = function_name == NULL ? 1 : 0;
    int braces = 0;
    size_t sequence_index = 0u;

    if (source_path == NULL || sequence == NULL || sequence_count == 0u) {
        return 0;
    }
    file = fopen(source_path, "rb");
    if (file == NULL) {
        return 0;
    }
    while ((status = c_lex_next(file, token)) > 0) {
        if (in_body != 0) {
            if (function_name != NULL) {
                if (strcmp(token, "{") == 0) {
                    braces++;
                } else if (strcmp(token, "}") == 0) {
                    braces--;
                    if (braces == 0) {
                        in_body = 0;
                    }
                }
            }
            if (sequence_index < sequence_count &&
                strcmp(token, sequence[sequence_index]) == 0) {
                sequence_index++;
            }
            if (sequence_index == sequence_count) {
                break;
            }
            continue;
        }
        if (candidate == 0) {
            if (strcmp(token, function_name) == 0) {
                candidate = 1;
            }
            continue;
        }
        if (signature == 0) {
            if (strcmp(token, "(") == 0) {
                signature = 1;
                parentheses = 1;
            } else {
                candidate = strcmp(token, function_name) == 0;
            }
        } else if (strcmp(token, "(") == 0) {
            parentheses++;
        } else if (strcmp(token, ")") == 0) {
            parentheses--;
        } else if (parentheses == 0 && strcmp(token, "{") == 0) {
            in_body = 1;
            braces = 1;
            candidate = 0;
            signature = 0;
        } else if (parentheses == 0 && strcmp(token, ";") == 0) {
            candidate = 0;
            signature = 0;
        }
    }
    if (fclose(file) != 0 || status < 0) {
        return 0;
    }
    return sequence_index == sequence_count;
}

static int lexical_call_sequence_in_function(const char *source_path,
                                             const char *function_name,
                                             const char *const *sequence,
                                             size_t sequence_count)
{
    FILE *file;
    char token[128];
    int status;
    int candidate = 0;
    int signature = 0;
    int parentheses = 0;
    int in_body = function_name == NULL ? 1 : 0;
    int braces = 0;
    int pending_call = 0;
    size_t sequence_index = 0u;

    if (source_path == NULL || sequence == NULL || sequence_count == 0u) {
        return 0;
    }
    file = fopen(source_path, "rb");
    if (file == NULL) {
        return 0;
    }
    while ((status = c_lex_next(file, token)) > 0) {
        if (in_body != 0) {
            if (pending_call != 0) {
                if (strcmp(token, "(") == 0) {
                    sequence_index++;
                }
                pending_call = 0;
            }
            if (function_name != NULL) {
                if (strcmp(token, "{") == 0) {
                    braces++;
                } else if (strcmp(token, "}") == 0) {
                    braces--;
                    if (braces == 0) {
                        in_body = 0;
                    }
                }
            }
            if (sequence_index < sequence_count &&
                strcmp(token, sequence[sequence_index]) == 0) {
                pending_call = 1;
            }
            if (sequence_index == sequence_count) {
                break;
            }
            continue;
        }
        if (candidate == 0) {
            if (strcmp(token, function_name) == 0) {
                candidate = 1;
            }
            continue;
        }
        if (signature == 0) {
            if (strcmp(token, "(") == 0) {
                signature = 1;
                parentheses = 1;
            } else {
                candidate = strcmp(token, function_name) == 0;
            }
        } else if (strcmp(token, "(") == 0) {
            parentheses++;
        } else if (strcmp(token, ")") == 0) {
            parentheses--;
        } else if (parentheses == 0 && strcmp(token, "{") == 0) {
            in_body = 1;
            braces = 1;
            candidate = 0;
            signature = 0;
        } else if (parentheses == 0 && strcmp(token, ";") == 0) {
            candidate = 0;
            signature = 0;
        }
    }
    if (fclose(file) != 0 || status < 0) {
        return 0;
    }
    return sequence_index == sequence_count;
}

static int full_binding_source_is_closed(void)
{
#if !defined(TAVRN_PHASE5_FULL_BINDING_SOURCE_PATH) || \
    !defined(TAVRN_PHASE5_ROUTED_CYCLE_SOURCE_PATH) || \
    !defined(TAVRN_PHASE5_ROUTED_CYCLE_HEADER_PATH)
    return 0;
#else
    static const char *const binding_order[] = {
        "tavrn_maintenance_owner_pre_tick", "tavrn_router_tick_ex",
        "tavrn_mentorship_tick", "tavrn_router_local_broadcast_snapshot",
        "tavrn_maintenance_observe_local_broadcast", "tavrn_maintenance_activate",
        "tavrn_maintenance_owner_post_tick",
    };
    static const char *const cycle_order[] = {
        "router_scheduler_event", "router_tick", "application_prepare",
        "router_submit", "router_dispatch", "router_link_service",
    };
    size_t count;

    if (!lexical_function_call_count(TAVRN_PHASE5_FULL_BINDING_SOURCE_PATH,
                                      "tavrn_full_maintenance_binding_tick",
                                      "tavrn_maintenance_owner_pre_tick", &count) ||
        count != 1u ||
        !lexical_function_call_count(TAVRN_PHASE5_FULL_BINDING_SOURCE_PATH,
                                      "tavrn_full_maintenance_binding_tick",
                                      "tavrn_router_tick_ex", &count) || count != 1u ||
        !lexical_function_call_count(TAVRN_PHASE5_FULL_BINDING_SOURCE_PATH,
                                      "tavrn_full_maintenance_binding_tick",
                                      "tavrn_mentorship_tick", &count) || count != 1u ||
        !lexical_function_call_count(TAVRN_PHASE5_FULL_BINDING_SOURCE_PATH,
                                      "tavrn_full_maintenance_binding_tick",
                                      "tavrn_maintenance_owner_post_tick", &count) ||
        count != 1u ||
        !lexical_function_identifier_count(TAVRN_PHASE5_FULL_BINDING_SOURCE_PATH,
                                      "tavrn_full_maintenance_binding_tick",
                                      "tavrn_full_application_mailbox", &count) ||
        count != 0u ||
        !lexical_call_sequence_in_function(TAVRN_PHASE5_FULL_BINDING_SOURCE_PATH,
                                           "tavrn_full_maintenance_binding_tick", binding_order,
                                           sizeof(binding_order) / sizeof(binding_order[0])) ||
        !lexical_function_identifier_count(TAVRN_PHASE5_ROUTED_CYCLE_SOURCE_PATH,
                                      "routed_cycle_run_once", "tavrn_gtt", &count) ||
        count != 0u ||
        !lexical_function_identifier_count(TAVRN_PHASE5_ROUTED_CYCLE_SOURCE_PATH,
                                      "routed_cycle_run_once", "tavrn_full", &count) ||
        count != 0u ||
        !lexical_sequence_in_function(TAVRN_PHASE5_ROUTED_CYCLE_SOURCE_PATH,
                                      "routed_cycle_run_once", cycle_order,
                                      sizeof(cycle_order) / sizeof(cycle_order[0]))) {
        return 0;
    }
    return lexical_source_token_count(TAVRN_PHASE5_ROUTED_CYCLE_HEADER_PATH,
                                      "tavrn_gtt", &count) && count == 0u &&
        lexical_source_token_count(TAVRN_PHASE5_ROUTED_CYCLE_HEADER_PATH,
                                   "tavrn_full", &count) && count == 0u;
#endif
}

static int full_binding_profile_source_is_closed(void)
{
#ifndef TAVRN_PHASE5_FULL_PROFILE_SOURCE_PATH
    return 0;
#else
    size_t count;

    return source_file_token_count(TAVRN_PHASE5_FULL_PROFILE_SOURCE_PATH,
                                   "FULL_TAVRN_SOURCES", NULL, &count) && count != 0u &&
        source_file_token_count(TAVRN_PHASE5_FULL_PROFILE_SOURCE_PATH,
                                 "tavrn_full_maintenance_binding.c", NULL, &count) &&
        count == 2u;
#endif
}

static int future_resource_gate_is_closed(void)
{
#if !defined(TAVRN_PHASE5_RESOURCE_CHECKER_PATH) || \
    !defined(TAVRN_PHASE5_RESOURCE_BUILD_SCRIPT_PATH) || \
    !defined(TAVRN_PHASE5_BUILD_PROFILE_RUNNER_PATH) || \
    !defined(TAVRN_PHASE5_EXPIRY_SUMMARIZER_PATH) || \
    !defined(TAVRN_PHASE5_ROUTED_MAIN_PATH)
    return 0;
#else
    size_t count;

    return source_file_token_count(TAVRN_PHASE5_RESOURCE_BUILD_SCRIPT_PATH,
                                   "--stack-usage", NULL, &count) && count >= 2u &&
        source_file_token_count(TAVRN_PHASE5_RESOURCE_BUILD_SCRIPT_PATH,
                                "--resource-baseline", NULL, &count) && count >= 2u &&
        source_file_token_count(TAVRN_PHASE5_RESOURCE_CHECKER_PATH,
                                "tron.tavrn.expiry.resources.v1", NULL, &count) &&
        count != 0u &&
        source_file_token_count(TAVRN_PHASE5_RESOURCE_CHECKER_PATH,
                                 "--compile-commands", NULL, &count) && count != 0u &&
        source_file_token_count(TAVRN_PHASE5_RESOURCE_CHECKER_PATH,
                                 "--capture-artifact-manifest", NULL, &count) &&
        count != 0u &&
        source_file_token_count(TAVRN_PHASE5_RESOURCE_CHECKER_PATH,
                                "-E", NULL, &count) && count != 0u &&
        source_file_token_count(TAVRN_PHASE5_RESOURCE_CHECKER_PATH,
                                "-P", NULL, &count) && count != 0u &&
        source_file_token_count(TAVRN_PHASE5_RESOURCE_CHECKER_PATH,
                                 "routed_cycle_router_tick", NULL, &count) && count != 0u &&
        source_file_token_count(TAVRN_PHASE5_RESOURCE_CHECKER_PATH,
                                 "routed_cycle_router_scheduler_event", NULL, &count) &&
        count != 0u &&
        source_file_token_count(TAVRN_PHASE5_BUILD_PROFILE_RUNNER_PATH,
                                "threshold-failing", NULL, &count) && count != 0u &&
        source_file_token_count(TAVRN_PHASE5_BUILD_PROFILE_RUNNER_PATH,
                                 "--stack-usage", NULL, &count) && count != 0u &&
        source_file_token_count(TAVRN_PHASE5_BUILD_PROFILE_RUNNER_PATH,
                                 "--resource-baseline", NULL, &count) && count != 0u &&
        source_file_token_count(TAVRN_PHASE5_EXPIRY_SUMMARIZER_PATH,
                                 "--artifact-manifest-sha256", NULL, &count) && count != 0u &&
        source_file_token_count(TAVRN_PHASE5_EXPIRY_SUMMARIZER_PATH,
                                 "routed expiry_sweep", NULL, &count) && count != 0u &&
        source_file_token_count(TAVRN_PHASE5_ROUTED_MAIN_PATH,
                                 "routed expiry_sweep", NULL, &count) && count != 0u &&
        source_file_token_count(TAVRN_PHASE5_ROUTED_MAIN_PATH,
                                 "TRON_BUILD_TEST_EXPIRY_FULL_TABLE", NULL, &count) &&
        count != 0u;
#endif
}

static int expiry_green_acceptance_is_wired(void)
{
#ifndef TAVRN_PHASE5_EXPIRY_RUNNER_PATH
    return 0;
#else
    size_t count;

    return source_file_token_count(TAVRN_PHASE5_EXPIRY_RUNNER_PATH,
                                    "run_expiry_firmware_acceptance", NULL, &count) &&
        count == 2u &&
        source_file_token_count(TAVRN_PHASE5_EXPIRY_RUNNER_PATH,
                                 "TAVRN_EXPIRY_ARM_CAPTURE_MANIFEST", NULL, &count) &&
        count != 0u &&
        source_file_token_count(TAVRN_PHASE5_EXPIRY_RUNNER_PATH,
                                 "TAVRN_EXPIRY_ARM_CAPTURE_ARTIFACT_MANIFEST", NULL,
                                 &count) && count != 0u &&
        source_file_token_count(TAVRN_PHASE5_EXPIRY_RUNNER_PATH,
                                "TAVRN_EXPIRY_BASELINE_DIR", NULL, &count) &&
        count != 0u &&
        source_file_token_count(TAVRN_PHASE5_EXPIRY_RUNNER_PATH,
                                "fast_before_map", NULL, &count) && count != 0u &&
        source_file_token_count(TAVRN_PHASE5_EXPIRY_RUNNER_PATH,
                                "full_fast_su_glob", NULL, &count) && count != 0u &&
        source_file_token_count(TAVRN_PHASE5_EXPIRY_RUNNER_PATH,
                                "routed_mesh_task", NULL, &count) &&
        count != 0u &&
        source_file_token_count(TAVRN_PHASE5_EXPIRY_RUNNER_PATH,
                                "RESOURCE_REQUIRED_STACK_EDGES", NULL, &count) &&
        count != 0u;
#endif
}

static int scheduler_gap_telemetry_contract(void)
{
#ifndef TAVRN_PHASE5_ROUTED_MAIN_PATH
    return 0;
#else
    size_t count;

    return routed_full_telemetry_max_scheduler_gap(0u, 0u) == 0u &&
        routed_full_telemetry_max_scheduler_gap(0u, 1u) == 1u &&
        routed_full_telemetry_max_scheduler_gap(1u, 0u) == 1u &&
        routed_full_telemetry_max_scheduler_gap(2u, 7u) == 7u &&
        routed_full_telemetry_max_scheduler_gap(7u, 2u) == 7u &&
        lexical_function_call_count(TAVRN_PHASE5_ROUTED_MAIN_PATH,
                                    "routed_cycle_trace_sink",
                                    "routed_full_telemetry_max_scheduler_gap",
                                    &count) && count == 1u &&
        lexical_function_identifier_count(
            TAVRN_PHASE5_ROUTED_MAIN_PATH, "routed_cycle_trace_sink",
            "elapsed_since_scheduler_return_ms", &count) && count == 1u;
#endif
}

static int full_table_hook_seed_contract(void)
{
#ifndef TAVRN_PHASE5_ROUTED_MAIN_PATH
    return 0;
#else
    static const char *const startup_order[] = {
        "tavrn_maintenance_init", "routed_expiry_full_table_seed", "tk_sta_tsk",
    };
    size_t count;

    return lexical_function_call_count(TAVRN_PHASE5_ROUTED_MAIN_PATH,
                                       "routed_expiry_full_table_seed",
                                       "tavrn_esc_resolve_sid8", &count) &&
        count == 1u &&
        lexical_function_call_count(TAVRN_PHASE5_ROUTED_MAIN_PATH,
                                    "routed_expiry_full_table_seed",
                                    "tavrn_full_resolve_unique_sid8", &count) &&
        count == 1u &&
        lexical_function_call_count(TAVRN_PHASE5_ROUTED_MAIN_PATH,
                                    "routed_expiry_full_table_seed",
                                    "tavrn_gtt_application_request", &count) &&
        count == 1u &&
        lexical_source_token_count(TAVRN_PHASE5_ROUTED_MAIN_PATH,
                                   "routed_expiry_full_table_maintain", &count) &&
        count == 0u &&
        lexical_call_sequence_in_function(TAVRN_PHASE5_ROUTED_MAIN_PATH, "usermain",
                                          startup_order,
                                          sizeof(startup_order) /
                                              sizeof(startup_order[0]));
#endif
}

static tavrn_validated_control_t make_sid8_rrep(
    tavrn_logical_id_t receiver, tavrn_logical_id_t destination,
    tavrn_logical_id_t origin, uint16_t request_id, uint16_t lifetime_ms)
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
    pdu_put_u16(&control, 9u, 0x4321u);
    control.pdu[11] = (uint8_t)origin.value;
    pdu_put_u16(&control, 12u, request_id);
    pdu_put_u16(&control, 14u, lifetime_ms);
    return control;
}

/* This follows the real submit -> RREQ -> RREP route-install path.  It drains
 * only the resulting FORWARD_DATA action so the caller starts with route state,
 * not a copied AODV action. */
static int install_route_via_lifetime(expiry_fixture_t *fixture,
                                      tavrn_logical_id_t destination,
                                      const tavrn_direct_peer_t *next_hop,
                                      uint32_t now_ms, uint16_t lifetime_ms,
                                      aodv_route_snapshot_t *route_out)
{
    tron_application_data_t input = application(destination);
    tavrn_validated_control_t reply;
    aodv_control_input_t control_input;
    aodv_action_t action;
    aodv_route_snapshot_t route;

    if (fixture == NULL || next_hop == NULL ||
        tavrn_router_submit_application(&fixture->router, &input, now_ms) !=
            AODV_STATUS_QUEUED ||
        aodv_core_poll_action(&fixture->aodv, &action) != AODV_ACTION_POLL_OK ||
        action.type != AODV_ACTION_SEND_RREQ) {
        return 0;
    }
    reply = make_sid8_rrep(fixture->aodv.config.local_peer.logical_id,
                           destination, fixture->aodv.config.local_peer.logical_id,
                           pdu_u16(&action.detail.control.control, 8u), lifetime_ms);
    memset(&control_input, 0, sizeof(control_input));
    control_input.transmitter = *next_hop;
    control_input.control = reply;
    if (aodv_core_ingest_control(&fixture->aodv, &control_input, now_ms + 1u) !=
            AODV_STATUS_OK ||
        aodv_core_route_snapshot(&fixture->aodv, &destination, &route) !=
            AODV_ROUTE_QUERY_FOUND ||
        route.state != AODV_ROUTE_VALID || !peer_equal(&route.next_hop, next_hop) ||
        aodv_core_poll_action(&fixture->aodv, &action) != AODV_ACTION_POLL_OK ||
        action.type != AODV_ACTION_FORWARD_DATA ||
        action.detail.data.data.final_destination.value != destination.value ||
        !peer_equal(&action.detail.data.next_hop, next_hop)) {
        return 0;
    }
    if (route_out != NULL) {
        *route_out = route;
    }
    return 1;
}

static int install_route_via(expiry_fixture_t *fixture,
                             tavrn_logical_id_t destination,
                             const tavrn_direct_peer_t *next_hop,
                             uint32_t now_ms,
                             aodv_route_snapshot_t *route_out)
{
    return install_route_via_lifetime(fixture, destination, next_hop, now_ms,
                                      40u, route_out);
}

static int seed_precursor(expiry_fixture_t *fixture,
                          const tavrn_direct_peer_t *precursor,
                          uint32_t now_ms,
                          aodv_route_snapshot_t *route_out)
{
    tavrn_direct_peer_t next_hop = peer(adva_d, TAVRN_IDENTITY_SID8);
    tavrn_logical_id_t destination = peer(adva_c, TAVRN_IDENTITY_SID8).logical_id;
    aodv_data_input_t input;
    aodv_action_t action;
    aodv_route_snapshot_t route;

    if (fixture == NULL || precursor == NULL ||
        !install_route_via(fixture, destination, &next_hop, now_ms, &route)) {
        return 0;
    }
    memset(&input, 0, sizeof(input));
    input.transmitter = *precursor;
    input.data = link_data(next_hop.logical_id, destination, 0x7101u);
    if (aodv_core_ingest_data(&fixture->aodv, &input, now_ms + 2u) !=
            AODV_STATUS_OK ||
        aodv_core_poll_action(&fixture->aodv, &action) != AODV_ACTION_POLL_OK ||
        action.type != AODV_ACTION_FORWARD_DATA ||
        aodv_core_route_snapshot(&fixture->aodv, &destination, &route) !=
            AODV_ROUTE_QUERY_FOUND || route.precursor_count != 1u ||
        route.precursors[0].width != precursor->logical_id.width ||
        route.precursors[0].value != precursor->logical_id.value) {
        return 0;
    }
    if (route_out != NULL) {
        *route_out = route;
    }
    return 1;
}

static int fill_unrelated_custody(expiry_fixture_t *fixture, uint32_t now_ms)
{
    tavrn_direct_peer_t next_hop = peer(adva_d, TAVRN_IDENTITY_SID8);
    tavrn_logical_id_t destination = peer(adva_c, TAVRN_IDENTITY_SID8).logical_id;
    uint8_t index;

    if (fixture == NULL) {
        return 0;
    }
    for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
        tavrn_link_data_t data = link_data(
            fixture->link.config.local_peer.logical_id, destination,
            (uint16_t)(0x7200u + index));

        if (tavrn_link_v2_send_unicast(&fixture->link, &next_hop, &data,
                                        now_ms, &(tavrn_link_event_t){0}) !=
            TAVRN_LINK_SEND_OK) {
            return 0;
        }
    }
    return 1;
}

static int retain_forward_data_at_route_expiry(
    expiry_fixture_t *fixture, tavrn_logical_id_t destination,
    const tavrn_direct_peer_t *next_hop, uint32_t now_ms,
    uint32_t *expiry_out)
{
    tron_application_data_t input = application(destination);
    aodv_route_snapshot_t route;
    tavrn_router_dispatch_event_t dispatch_event;

    if (fixture == NULL || next_hop == NULL ||
        !install_route_via(fixture, destination, next_hop, now_ms, &route) ||
        tavrn_router_submit_application(&fixture->router, &input, now_ms + 2u) !=
            AODV_STATUS_OK || !fill_unrelated_custody(fixture, now_ms + 2u)) {
        return 0;
    }
    memset(&dispatch_event, 0, sizeof(dispatch_event));
    if (tavrn_router_dispatch_ex(&fixture->router, route.expires_at_ms,
                                 &dispatch_event) != TAVRN_ROUTER_EVENT_BUSY ||
        dispatch_event.type != TAVRN_ROUTER_DISPATCH_EVENT_NONE ||
        fixture->router.retained_action_valid == 0u ||
        fixture->router.retained_action.type != AODV_ACTION_FORWARD_DATA ||
        fixture->router.retained_action.detail.data.data.final_destination.width !=
            destination.width ||
        fixture->router.retained_action.detail.data.data.final_destination.value !=
            destination.value ||
        !peer_equal(&fixture->router.retained_action.detail.data.next_hop, next_hop)) {
        return 0;
    }
    if (expiry_out != NULL) {
        *expiry_out = route.expires_at_ms;
    }
    return 1;
}

static int note_local_broadcast_after_snapshot(
    tavrn_router_t *router, uint32_t now_ms,
    tavrn_router_local_broadcast_snapshot_t *snapshot_out)
{
    tavrn_router_local_broadcast_snapshot_t before;
    uint32_t expected_generation;

    if (router == NULL || snapshot_out == NULL ||
        tavrn_router_local_broadcast_snapshot(router, &before) !=
            TAVRN_ROUTER_LOCAL_BROADCAST_OK) {
        return 0;
    }
    expected_generation = before.generation + 1u;
    if (expected_generation == 0u) {
        expected_generation = 1u;
    }
    tavrn_router_note_local_broadcast(router, now_ms);
    return tavrn_router_local_broadcast_snapshot(router, snapshot_out) ==
            TAVRN_ROUTER_LOCAL_BROADCAST_OK &&
        snapshot_out->generation == expected_generation &&
        snapshot_out->accepted_at_ms == now_ms;
}

static int aodv_demand_matches(const expiry_fixture_t *fixture,
                                const tavrn_logical_id_t *subject,
                                const tavrn_adva_t *canonical_subject,
                                uint32_t now_ms, uint16_t expected_reason,
                               uint8_t expected_route_to)
{
    tavrn_aodv_subject_demand_snapshot_t snapshot;

    snapshot = aodv_core_subject_demand_snapshot(&fixture->aodv, subject,
                                                   canonical_subject, now_ms);
    return snapshot.snapshot_available != 0u &&
        snapshot.reason_mask == expected_reason &&
        snapshot.valid_route_to_subject == expected_route_to;
}

static int link_demand_matches(const expiry_fixture_t *fixture,
                               const tavrn_logical_id_t *subject,
                               const tavrn_adva_t *canonical_subject,
                               uint32_t now_ms, uint16_t expected_reason)
{
    tavrn_link_subject_demand_snapshot_t snapshot;

    snapshot = tavrn_link_v2_subject_demand_snapshot(&fixture->link, subject,
                                                       canonical_subject, now_ms);
    return snapshot.snapshot_available != 0u &&
        snapshot.reason_mask == expected_reason;
}

static int router_demand_matches(const expiry_fixture_t *fixture,
                                 const tavrn_logical_id_t *subject,
                                 const tavrn_adva_t *canonical_subject,
                                 uint32_t now_ms, uint16_t expected_reason)
{
    tavrn_router_subject_demand_snapshot_t snapshot;

    snapshot = tavrn_router_subject_demand_snapshot(&fixture->router, subject,
                                                      canonical_subject, now_ms);
    return snapshot.snapshot_available != 0u &&
        snapshot.reason_mask == expected_reason;
}

static int maintenance_demand_matches(const expiry_fixture_t *fixture,
                                      const tavrn_adva_t *subject,
                                      uint32_t now_ms, uint16_t expected_reason,
                                      uint8_t expected_route_to)
{
    tavrn_maintenance_demand_snapshot_t snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    return tavrn_maintenance_demand_snapshot(&fixture->maintenance, subject, now_ms,
                                              &snapshot) == TAVRN_MAINTENANCE_DEMAND_OK &&
        snapshot.snapshot_available != 0u && adva_equal(&snapshot.identity, subject) &&
        snapshot.reason_mask == expected_reason &&
        snapshot.valid_route_to_subject == expected_route_to &&
        snapshot.stage_zero_eligible == expected_route_to;
}

static int maintenance_has_no_demand(const expiry_fixture_t *fixture,
                                     const tavrn_adva_t *subject,
                                     uint32_t now_ms)
{
    return maintenance_demand_matches(fixture, subject, now_ms, 0u, 0u);
}

static int maintenance_is_unavailable(const expiry_fixture_t *fixture,
                                      const tavrn_adva_t *subject,
                                      uint32_t now_ms)
{
    tavrn_maintenance_demand_snapshot_t snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    return tavrn_maintenance_demand_snapshot(&fixture->maintenance, subject, now_ms,
                                             &snapshot) ==
        TAVRN_MAINTENANCE_DEMAND_UNAVAILABLE;
}

static tavrn_gtt_evidence_t evidence(tavrn_adva_t identity_in, uint16_t serial,
                                      uint8_t serial_present, uint8_t hop)
{
    tavrn_gtt_evidence_t result;

    memset(&result, 0, sizeof(result));
    result.identity = identity_in;
    result.serial = serial;
    result.serial_present = serial_present;
    result.hop_count = hop;
    result.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    return result;
}

static tavrn_gtt_evidence_t departure_evidence(tavrn_adva_t identity_in,
                                                uint16_t serial, uint8_t hop)
{
    tavrn_gtt_evidence_t result = evidence(identity_in, serial, 1u, hop);

    result.kind = TAVRN_GTT_EVIDENCE_DEPARTED;
    return result;
}

static uint32_t fnv1a_deadline_hash(const tavrn_adva_t *local,
                                    const tavrn_gtt_evidence_t *input,
                                    uint32_t now_ms)
{
    uint32_t hash = 2166136261u;
    uint8_t index;
    uint16_t serial = input->serial_present != 0u ? input->serial : 0u;
    uint8_t bytes[3] = { input->serial_present != 0u ? 1u : 0u,
                         (uint8_t)serial, (uint8_t)(serial >> 8) };

    for (index = 0u; index < TAVRN_ADVA_LEN; index++) {
        hash = (hash ^ local->bytes[index]) * 16777619u;
    }
    for (index = 0u; index < TAVRN_ADVA_LEN; index++) {
        hash = (hash ^ input->identity.bytes[index]) * 16777619u;
    }
    for (index = 0u; index < sizeof(bytes); index++) {
        hash = (hash ^ bytes[index]) * 16777619u;
    }
    for (index = 0u; index < 4u; index++) {
        hash = (hash ^ (uint8_t)(now_ms >> (8u * index))) * 16777619u;
    }
    return hash;
}

static tavrn_gtt_expiry_observe_status_t observe_expiry(
    tavrn_gtt_t *gtt, const tavrn_gtt_evidence_t *input,
    tavrn_gtt_provenance_t provenance, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out)
{
    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    return phase5_gtt_observe(gtt, input, provenance, now_ms, snapshot_out);
}

static tavrn_gtt_expiry_query_status_t expiry_snapshot_query(
    const tavrn_gtt_t *gtt, const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out)
{
    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    return phase5_gtt_snapshot(gtt, identity, now_ms, snapshot_out);
}

static tavrn_gtt_expiry_query_status_t enumerate_known(
    const tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshots_out, uint8_t capacity,
    uint8_t *count_out)
{
    if (snapshots_out != NULL && capacity != 0u) {
        memset(snapshots_out, 0, sizeof(*snapshots_out) * capacity);
    }
    if (count_out != NULL) {
        *count_out = 0u;
    }
    return phase5_gtt_enumerate_known(gtt, lane, now_ms, snapshots_out,
                                      capacity, count_out);
}

static const tavrn_gtt_expiry_snapshot_t *find_known_snapshot(
    const tavrn_gtt_expiry_snapshot_t *snapshots, uint8_t count,
    const tavrn_adva_t *identity)
{
    uint8_t index;

    if (snapshots == NULL || identity == NULL) {
        return NULL;
    }
    for (index = 0u; index < count; index++) {
        if (adva_equal(&snapshots[index].identity, identity)) {
            return &snapshots[index];
        }
    }
    return NULL;
}

static int expiry_snapshots_equal(const tavrn_gtt_expiry_snapshot_t *left,
                                  const tavrn_gtt_expiry_snapshot_t *right)
{
    return left != NULL && right != NULL &&
        adva_equal(&left->identity, &right->identity) &&
        left->last_evidence_ms == right->last_evidence_ms &&
        left->last_direct_evidence_ms == right->last_direct_evidence_ms &&
        left->soft_deadline_ms == right->soft_deadline_ms &&
        left->hard_deadline_ms == right->hard_deadline_ms &&
        left->departed_deadline_ms == right->departed_deadline_ms &&
        left->revision == right->revision &&
        left->storage_generation == right->storage_generation &&
        left->serial == right->serial &&
        left->serial_present == right->serial_present &&
        left->hop_count == right->hop_count && left->direct == right->direct &&
        left->application_requested == right->application_requested &&
        left->freshness == right->freshness;
}

static int known_snapshots_equal(const tavrn_gtt_expiry_snapshot_t *left,
                                 uint8_t left_count,
                                 const tavrn_gtt_expiry_snapshot_t *right,
                                 uint8_t right_count)
{
    uint8_t index;

    if (left == NULL || right == NULL || left_count != right_count) {
        return 0;
    }
    for (index = 0u; index < left_count; index++) {
        const tavrn_gtt_expiry_snapshot_t *matching =
            find_known_snapshot(right, right_count, &left[index].identity);

        if (!expiry_snapshots_equal(&left[index], matching)) {
            return 0;
        }
    }
    return 1;
}

typedef struct expiry_gtt_state {
    tavrn_gtt_t gtt;
    tavrn_gtt_storage_t storage;
    tavrn_gtt_counters_t counters;
    tavrn_gtt_expiry_snapshot_t known[TAVRN_GTT_CAPACITY];
    uint8_t known_count;
} expiry_gtt_state_t;

static int capture_gtt_state(const expiry_fixture_t *fixture, uint32_t now_ms,
                             expiry_gtt_state_t *state_out)
{
    const tavrn_gtt_counters_t *counters;

    if (fixture == NULL || state_out == NULL) {
        return 0;
    }
    counters = tavrn_gtt_counters(&fixture->gtt);
    if (counters == NULL) {
        return 0;
    }
    memset(state_out, 0, sizeof(*state_out));
    state_out->gtt = fixture->gtt;
    state_out->storage = fixture->gtt_storage;
    state_out->counters = *counters;
    return enumerate_known(&fixture->gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                           now_ms, state_out->known, TAVRN_GTT_CAPACITY,
                           &state_out->known_count) == TAVRN_GTT_EXPIRY_QUERY_FOUND;
}

static int gtt_states_equal(const expiry_gtt_state_t *left,
                            const expiry_gtt_state_t *right)
{
    return left != NULL && right != NULL &&
        memcmp(&left->gtt, &right->gtt, sizeof(left->gtt)) == 0 &&
        memcmp(&left->storage, &right->storage, sizeof(left->storage)) == 0 &&
        memcmp(&left->counters, &right->counters, sizeof(left->counters)) == 0 &&
        known_snapshots_equal(left->known, left->known_count,
                              right->known, right->known_count);
}

typedef struct expiry_side_effect_state {
    ble_mesh_scheduler_t scheduler;
    aodv_core_t aodv;
    tavrn_router_t router;
    tavrn_link_v2_t link;
    tavrn_mentorship_t mentorship;
    tavrn_maintenance_counters_t maintenance_counters;
    tavrn_maintenance_snapshot_t maintenance_snapshot;
} expiry_side_effect_state_t;

static int capture_side_effect_state(const expiry_fixture_t *fixture,
                                     expiry_side_effect_state_t *state_out)
{
    const tavrn_maintenance_counters_t *counters;

    if (fixture == NULL || state_out == NULL) {
        return 0;
    }
    counters = tavrn_maintenance_counters(&fixture->maintenance);
    if (counters == NULL || tavrn_maintenance_snapshot(&fixture->maintenance,
                                                        &state_out->maintenance_snapshot) !=
                            TAVRN_MAINTENANCE_OK) {
        return 0;
    }
    state_out->scheduler = fixture->scheduler;
    state_out->aodv = fixture->aodv;
    state_out->router = fixture->router;
    state_out->link = fixture->link;
    state_out->mentorship = fixture->mentorship;
    state_out->maintenance_counters = *counters;
    return 1;
}

static int side_effect_states_equal(const expiry_side_effect_state_t *left,
                                    const expiry_side_effect_state_t *right)
{
    return left != NULL && right != NULL &&
        memcmp(&left->scheduler, &right->scheduler, sizeof(left->scheduler)) == 0 &&
        memcmp(&left->aodv, &right->aodv, sizeof(left->aodv)) == 0 &&
        memcmp(&left->router, &right->router, sizeof(left->router)) == 0 &&
        memcmp(&left->link, &right->link, sizeof(left->link)) == 0 &&
        memcmp(&left->mentorship, &right->mentorship, sizeof(left->mentorship)) == 0 &&
        memcmp(&left->maintenance_counters, &right->maintenance_counters,
               sizeof(left->maintenance_counters)) == 0 &&
        memcmp(&left->maintenance_snapshot, &right->maintenance_snapshot,
               sizeof(left->maintenance_snapshot)) == 0;
}

static int transport_side_effect_states_equal(const expiry_side_effect_state_t *left,
                                              const expiry_side_effect_state_t *right)
{
    return left != NULL && right != NULL &&
        memcmp(&left->scheduler, &right->scheduler, sizeof(left->scheduler)) == 0 &&
        memcmp(&left->aodv, &right->aodv, sizeof(left->aodv)) == 0 &&
        memcmp(&left->router, &right->router, sizeof(left->router)) == 0 &&
        memcmp(&left->link, &right->link, sizeof(left->link)) == 0 &&
        memcmp(&left->mentorship, &right->mentorship, sizeof(left->mentorship)) == 0;
}

static tavrn_gtt_departure_candidate_t candidate_from_snapshot(
    const tavrn_gtt_expiry_snapshot_t *snapshot)
{
    tavrn_gtt_departure_candidate_t candidate;

    memset(&candidate, 0, sizeof(candidate));
    if (snapshot != NULL) {
        candidate.identity = snapshot->identity;
        candidate.revision = snapshot->revision;
        candidate.hard_deadline_ms = snapshot->hard_deadline_ms;
    }
    return candidate;
}

static int maintenance_demand_for(const expiry_fixture_t *fixture,
                                  const tavrn_adva_t *identity, uint32_t now_ms,
                                  tavrn_maintenance_demand_snapshot_t *demand_out)
{
    if (fixture == NULL || identity == NULL || demand_out == NULL) {
        return 0;
    }
    memset(demand_out, 0, sizeof(*demand_out));
    return phase5_maintenance_demand(&fixture->maintenance, identity, now_ms,
                                     demand_out) == TAVRN_MAINTENANCE_DEMAND_OK &&
        demand_out->snapshot_available != 0u &&
        adva_equal(&demand_out->identity, identity);
}

static int direct_deadline_from_maintenance(
    const tavrn_maintenance_snapshot_t *maintenance,
    const tavrn_gtt_expiry_snapshot_t *gtt_snapshot, uint32_t *deadline_out)
{
    uint32_t interval_deadline;
    uint32_t threshold;

    if (maintenance == NULL || gtt_snapshot == NULL || deadline_out == NULL ||
        maintenance->current_interval_ms > UINT32_MAX / 3u) {
        return 0;
    }
    interval_deadline = maintenance->current_interval_ms * 3u;
    threshold = maintenance->liveness_floor_ms > interval_deadline ?
        maintenance->liveness_floor_ms : interval_deadline;
    if (threshold == UINT32_MAX || threshold >= 0x80000000u) {
        return 0;
    }
    *deadline_out = gtt_snapshot->last_direct_evidence_ms + threshold;
    return 1;
}

static int checked_departure_rejection_is_immutable(
    expiry_fixture_t *fixture, const tavrn_gtt_departure_candidate_t *candidate,
    uint32_t now_ms, tavrn_maintenance_checked_departure_status_t expected_status,
    const tavrn_gtt_expiry_snapshot_t *expected_output)
{
    expiry_gtt_state_t gtt_before;
    expiry_gtt_state_t gtt_after;
    expiry_side_effect_state_t effects_before;
    expiry_side_effect_state_t effects_after;
    tavrn_gtt_expiry_snapshot_t output;
    tavrn_gtt_expiry_snapshot_t empty = { 0 };
    tavrn_maintenance_checked_departure_status_t status;

    if (fixture == NULL ||
        !capture_gtt_state(fixture, now_ms, &gtt_before) ||
        !capture_side_effect_state(fixture, &effects_before)) {
        return 0;
    }
    memset(&output, 0xa5, sizeof(output));
    status = phase5_maintenance_checked_departure(&fixture->maintenance, candidate,
                                                  now_ms, &output);
    if (status != expected_status ||
        !expiry_snapshots_equal(&output,
                                expected_output == NULL ? &empty : expected_output) ||
        !capture_gtt_state(fixture, now_ms, &gtt_after) ||
        !capture_side_effect_state(fixture, &effects_after)) {
        return 0;
    }
    return gtt_states_equal(&gtt_before, &gtt_after) &&
        side_effect_states_equal(&effects_before, &effects_after);
}

static int active_snapshots_contain(const tavrn_gtt_snapshot_t *snapshots,
                                    uint8_t count,
                                    const tavrn_adva_t *identity)
{
    uint8_t index;

    if (snapshots == NULL || identity == NULL) {
        return 0;
    }
    for (index = 0u; index < count; index++) {
        if (adva_equal(&snapshots[index].identity, identity)) {
            return 1;
        }
    }
    return 0;
}

static int deadline_precedes(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) < 0;
}

static uint32_t later_deadline(uint32_t left, uint32_t right)
{
    return deadline_precedes(left, right) ? right : left;
}

static int jittered_deadlines_match(const expiry_fixture_t *fixture,
                                    const tavrn_gtt_evidence_t *input,
                                    uint32_t now_ms,
                                    const tavrn_gtt_expiry_snapshot_t *snapshot)
{
    uint32_t hash;
    uint32_t jitter;
    uint32_t hard_offset;
    uint32_t soft_offset;

    if (fixture == NULL || input == NULL || snapshot == NULL) {
        return 0;
    }
    hash = fnv1a_deadline_hash(&fixture->gtt.config.local_identity, input, now_ms);
    jitter = hash % (fixture->gtt.config.hard_expiry_ms / 6u + 1u);
    hard_offset = fixture->gtt.config.hard_expiry_ms + jitter;
    soft_offset = (uint32_t)(((uint64_t)hard_offset *
                               fixture->gtt.config.soft_expiry_ms) /
                              fixture->gtt.config.hard_expiry_ms);
    return snapshot->hard_deadline_ms == now_ms + hard_offset &&
        snapshot->soft_deadline_ms == now_ms + soft_offset;
}

static int observe_expected(expiry_fixture_t *fixture,
                             const tavrn_gtt_evidence_t *input,
                             tavrn_gtt_provenance_t provenance, uint32_t now_ms,
                             tavrn_gtt_expiry_observe_status_t status,
                             uint32_t revision, uint8_t direct,
                             tavrn_gtt_expiry_snapshot_t *snapshot_out)
{
    tavrn_gtt_expiry_snapshot_t snapshot;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (fixture == NULL || input == NULL ||
        observe_expiry(&fixture->gtt, input, provenance, now_ms, &snapshot) != status ||
        !adva_equal(&snapshot.identity, &input->identity) ||
        snapshot.revision != revision || snapshot.last_evidence_ms != now_ms ||
        snapshot.direct != direct) {
        return 0;
    }
    if (snapshot_out != NULL) {
        *snapshot_out = snapshot;
    }
    if (provenance == TAVRN_GTT_PROVENANCE_CONFIRMED_DEPARTURE) {
        return input->kind == TAVRN_GTT_EVIDENCE_DEPARTED &&
            snapshot.freshness == TAVRN_GTT_FRESHNESS_DEPARTED &&
            snapshot.departed_deadline_ms == now_ms +
                fixture->gtt.config.departed_retention_ms;
    }
    return jittered_deadlines_match(fixture, input, now_ms, &snapshot);
}

static int test_gtt_02_real_state_contract(void)
{
    int ok = 1;

    /* Exact capacity includes the protected local identity.  A table of only
     * fresh remote records has no legal replacement victim. */
    {
        expiry_fixture_t fixture;
        tavrn_gtt_evidence_t input;
        tavrn_gtt_expiry_snapshot_t snapshot;
        tavrn_gtt_expiry_snapshot_t before[TAVRN_GTT_CAPACITY];
        tavrn_gtt_expiry_snapshot_t after[TAVRN_GTT_CAPACITY];
        tavrn_gtt_storage_t storage_before;
        const tavrn_gtt_counters_t *counters;
        uint32_t capacity_rejected_before;
        uint8_t before_count = 0u;
        uint8_t after_count = 0u;
        uint8_t index;

        ok &= fixture_init(&fixture, 0u) && TAVRN_GTT_CAPACITY == 16u;
        input = evidence(fixture.gtt.config.local_identity, 1u, 1u, 1u);
        ok &= observe_expiry(&fixture.gtt, &input,
                             TAVRN_GTT_PROVENANCE_DIRECT_HELLO, 1u,
                             &snapshot) == TAVRN_GTT_EXPIRY_OBSERVE_SELF_IGNORED;
        ok &= expiry_snapshot_query(&fixture.gtt, &fixture.gtt.config.local_identity,
                                    1u, &snapshot) == TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            snapshot.revision == 1u && snapshot.storage_generation == 1u;
        for (index = 0u; index < TAVRN_GTT_CAPACITY - 1u; index++) {
            input = evidence(generated_adva((uint8_t)(index + 1u)), 1u, 1u, 2u);
            ok &= observe_expected(&fixture, &input,
                                   TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                   (uint32_t)(index + 2u),
                                   TAVRN_GTT_EXPIRY_OBSERVE_ADDED,
                                   (uint32_t)(index + 2u), 0u, NULL);
        }
        ok &= enumerate_known(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                               17u, before, TAVRN_GTT_CAPACITY, &before_count) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            before_count == TAVRN_GTT_CAPACITY;
        counters = tavrn_gtt_counters(&fixture.gtt);
        if (counters == NULL) {
            return 0;
        }
        storage_before = fixture.gtt_storage;
        capacity_rejected_before = counters->capacity_rejected;
        input = evidence(generated_adva(0x51u), 1u, 1u, 2u);
        ok &= observe_expiry(&fixture.gtt, &input,
                             TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 17u,
                             &snapshot) == TAVRN_GTT_EXPIRY_OBSERVE_NO_VICTIM;
        ok &= enumerate_known(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                              17u, after, TAVRN_GTT_CAPACITY, &after_count) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            known_snapshots_equal(before, before_count, after, after_count) &&
            memcmp(&storage_before, &fixture.gtt_storage, sizeof(storage_before)) == 0 &&
            tavrn_gtt_counters(&fixture.gtt) != NULL &&
            tavrn_gtt_counters(&fixture.gtt)->capacity_rejected ==
                capacity_rejected_before + 1u &&
            find_known_snapshot(after, after_count,
                                &fixture.gtt.config.local_identity) != NULL &&
            find_known_snapshot(after, after_count, &input.identity) == NULL;
    }

    /* Duplicates are no-ops, while each distinct storage mutation receives the
     * table's next revision and generation. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input;
        tavrn_gtt_expiry_snapshot_t snapshot;

        if (!fixture_init(&fixture, 0u)) {
            return 0;
        }
        input = evidence(subject, 1u, 1u, 2u);
        if (!observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 10u,
                              TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u,
                              &snapshot) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 10u,
                              TAVRN_GTT_EXPIRY_OBSERVE_DUPLICATE, 2u, 0u,
                              &snapshot)) {
            return 0;
        }
        input.serial = 2u;
        if (!observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 11u,
                              TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 3u, 0u,
                              &snapshot)) {
            return 0;
        }
        input.serial = 3u;
        input.hop_count = 1u;
        if (!observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_DIRECT_HELLO, 12u,
                              TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 4u, 1u,
                              &snapshot)) {
            return 0;
        }
        memset(&snapshot, 0, sizeof(snapshot));
        if (phase5_gtt_request(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                               &subject, 12u, &snapshot) != TAVRN_GTT_APPLICATION_CHANGED) {
            return 0;
        }
        ok &= snapshot.revision == 5u && snapshot.storage_generation == 5u &&
            snapshot.application_requested != 0u;
        memset(&snapshot, 0, sizeof(snapshot));
        if (phase5_gtt_cancel(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                              &subject, 12u, &snapshot) != TAVRN_GTT_APPLICATION_CANCELED) {
            return 0;
        }
        ok &= snapshot.revision == 6u && snapshot.storage_generation == 6u &&
            snapshot.application_requested == 0u;
        ok &= tavrn_gtt_clear_serial(&fixture.gtt, &subject, 13u) ==
            TAVRN_GTT_SERIAL_CLEAR_FOUND;
        if (expiry_snapshot_query(&fixture.gtt, &subject, 13u, &snapshot) !=
            TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= snapshot.revision == 7u && snapshot.storage_generation == 7u &&
            snapshot.serial_present == 0u;
    }

    /* Existing records revise on an admitted equal-serial later liveness
     * update, not only when their serial advances. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 2u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t after;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 10u,
                              TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u, &before) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 11u,
                              TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 3u, 0u, &after)) {
            return 0;
        }
        ok &= after.storage_generation == 3u && after.last_evidence_ms == 11u &&
            after.hard_deadline_ms != before.hard_deadline_ms;
    }

    /* A SYNC adjustment of an existing row preserves the imported lifetime
     * bucket without jitter and still receives a new revision. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 2u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t after;
        tavrn_gtt_sync_record_t record;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 10u,
                              TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u, &before)) {
            return 0;
        }
        memset(&record, 0, sizeof(record));
        record.identity = subject;
        record.remaining_lifetime_ms = 40000u;
        record.serial = input.serial;
        record.serial_present = input.serial_present;
        record.hop_count = input.hop_count;
        if (phase5_gtt_sync_merge(&fixture.gtt, &record, 1u, 10u) !=
                TAVRN_GTT_SYNC_MERGE_COMMITTED ||
            expiry_snapshot_query(&fixture.gtt, &subject, 10u, &after) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= after.revision == 3u && after.storage_generation == 3u &&
            after.last_evidence_ms == before.last_evidence_ms &&
            after.hard_deadline_ms == 10u + record.remaining_lifetime_ms &&
            after.direct == 0u && after.last_direct_evidence_ms == 0u &&
            after.serial == before.serial && after.serial_present == before.serial_present &&
            after.hop_count == before.hop_count;
    }

    /* Direct equal-serial replay is stale even when an imported hop-one row is
     * present at the same timestamp: it cannot upgrade directness or revise. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 1u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t after;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 10u,
                              TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u, &before) ||
            observe_expiry(&fixture.gtt, &input,
                           TAVRN_GTT_PROVENANCE_DIRECT_HELLO, 10u,
                           &after) != TAVRN_GTT_EXPIRY_OBSERVE_STALE ||
            expiry_snapshot_query(&fixture.gtt, &subject, 10u, &after) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= expiry_snapshots_equal(&before, &after);
    }

    /* Request/cancel no-ops and clearing an already-absent serial retain the
     * current storage version. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 2u);
        tavrn_gtt_expiry_snapshot_t requested;
        tavrn_gtt_expiry_snapshot_t canceled;
        tavrn_gtt_expiry_snapshot_t after;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 10u,
                              TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u, &after)) {
            return 0;
        }
        memset(&requested, 0, sizeof(requested));
        memset(&after, 0, sizeof(after));
        if (phase5_gtt_request(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                               &subject, 10u, &requested) != TAVRN_GTT_APPLICATION_CHANGED ||
            phase5_gtt_request(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                               &subject, 10u, &after) !=
                TAVRN_GTT_APPLICATION_UNCHANGED_DUPLICATE) {
            return 0;
        }
        ok &= expiry_snapshots_equal(&requested, &after);
        memset(&canceled, 0, sizeof(canceled));
        memset(&after, 0, sizeof(after));
        if (phase5_gtt_cancel(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                              &subject, 10u, &canceled) != TAVRN_GTT_APPLICATION_CANCELED ||
            phase5_gtt_cancel(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                              &subject, 10u, &after) !=
                TAVRN_GTT_APPLICATION_UNCHANGED_DUPLICATE) {
            return 0;
        }
        ok &= expiry_snapshots_equal(&canceled, &after);
    }
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 0xbeefu, 0u, 2u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t after;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 10u,
                              TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u, &before) ||
            tavrn_gtt_clear_serial(&fixture.gtt, &subject, 11u) !=
                TAVRN_GTT_SERIAL_CLEAR_FOUND ||
            expiry_snapshot_query(&fixture.gtt, &subject, 11u, &after) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= expiry_snapshots_equal(&before, &after);
    }

    /* Departure wins over stale age, and only the oldest departure is evicted. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t members[TAVRN_GTT_CAPACITY - 1u];
        tavrn_gtt_evidence_t input;
        tavrn_gtt_expiry_snapshot_t snapshot;
        tavrn_adva_t replacement = generated_adva(0x52u);
        uint8_t index;

        ok &= fixture_init(&fixture, 0u);
        for (index = 0u; index < TAVRN_GTT_CAPACITY - 1u; index++) {
            members[index] = generated_adva((uint8_t)(index + 1u));
            input = evidence(members[index], 1u, 1u, 2u);
            ok &= observe_expected(&fixture, &input,
                                   TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                   (uint32_t)(index + 1u),
                                   TAVRN_GTT_EXPIRY_OBSERVE_ADDED,
                                   (uint32_t)(index + 2u), 0u, NULL);
        }
        input = departure_evidence(members[0], 2u, 2u);
        ok &= observe_expected(&fixture, &input,
                               TAVRN_GTT_PROVENANCE_CONFIRMED_DEPARTURE, 30u,
                               TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 17u, 0u, NULL);
        input = departure_evidence(members[1], 2u, 2u);
        ok &= observe_expected(&fixture, &input,
                               TAVRN_GTT_PROVENANCE_CONFIRMED_DEPARTURE, 31u,
                               TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 18u, 0u, NULL);
        input = evidence(replacement, 1u, 1u, 2u);
        ok &= observe_expected(&fixture, &input,
                               TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 32u,
                               TAVRN_GTT_EXPIRY_OBSERVE_REPLACED_DEPARTED,
                               19u, 0u, &snapshot);
        ok &= expiry_snapshot_query(&fixture.gtt, &members[0], 32u, &snapshot) ==
            TAVRN_GTT_EXPIRY_QUERY_NOT_FOUND;
        ok &= expiry_snapshot_query(&fixture.gtt, &replacement, 32u, &snapshot) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            adva_equal(&snapshot.identity, &replacement);
        ok &= expiry_snapshot_query(&fixture.gtt, &fixture.gtt.config.local_identity,
                                    32u, &snapshot) == TAVRN_GTT_EXPIRY_QUERY_FOUND;
        for (index = 1u; index < TAVRN_GTT_CAPACITY - 1u; index++) {
            ok &= expiry_snapshot_query(&fixture.gtt, &members[index], 32u,
                                        &snapshot) == TAVRN_GTT_EXPIRY_QUERY_FOUND;
        }
    }

    /* The established GTT policy selects the oldest stale last-evidence time,
     * with canonical AdvA ordering breaking equal-time ties.  Soft deadlines
     * establish stale eligibility only; jitter never picks the victim. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t members[TAVRN_GTT_CAPACITY - 1u];
        tavrn_adva_t oldest = generated_adva(1u);
        tavrn_gtt_evidence_t input;
        tavrn_gtt_expiry_snapshot_t observed;
        tavrn_gtt_expiry_snapshot_t snapshot;
        uint32_t latest_soft_deadline = 0u;
        uint32_t first_evidence_ms = UINT32_MAX - 8u;
        uint32_t replacement_at;
        uint8_t index;

        ok &= fixture_init(&fixture, UINT32_MAX - 20u);
        for (index = 0u; index < TAVRN_GTT_CAPACITY - 1u; index++) {
            uint32_t observed_at = index < 2u ? first_evidence_ms :
                first_evidence_ms + (uint32_t)(index - 1u);

            members[index] = generated_adva(index == 0u ? 2u :
                                            (index == 1u ? 1u :
                                             (uint8_t)(index + 1u)));
            input = evidence(members[index], (uint16_t)(index + 1u), 1u, 2u);
            if (!observe_expected(&fixture, &input,
                                  TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                  observed_at,
                                  TAVRN_GTT_EXPIRY_OBSERVE_ADDED,
                                  (uint32_t)(index + 2u), 0u, &observed)) {
                return 0;
            }
            if (index == 0u ||
                deadline_precedes(latest_soft_deadline, observed.soft_deadline_ms)) {
                latest_soft_deadline = observed.soft_deadline_ms;
            }
        }
        ok &= adva_equal(&members[1], &oldest) && !adva_equal(&members[0], &oldest);
        replacement_at = latest_soft_deadline + 1u;
        input = evidence(generated_adva(0x53u), 1u, 1u, 2u);
        ok &= observe_expected(&fixture, &input,
                               TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, replacement_at,
                               TAVRN_GTT_EXPIRY_OBSERVE_REPLACED_OLDEST_STALE,
                               17u, 0u, &snapshot);
        ok &= expiry_snapshot_query(&fixture.gtt, &oldest, replacement_at,
                                    &snapshot) == TAVRN_GTT_EXPIRY_QUERY_NOT_FOUND;
        ok &= expiry_snapshot_query(&fixture.gtt, &members[0], replacement_at,
                                    &snapshot) == TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            snapshot.last_evidence_ms == first_evidence_ms;
        ok &= expiry_snapshot_query(&fixture.gtt, &input.identity, replacement_at,
                                    &snapshot) == TAVRN_GTT_EXPIRY_QUERY_FOUND;
        ok &= expiry_snapshot_query(&fixture.gtt, &fixture.gtt.config.local_identity,
                                    replacement_at, &snapshot) ==
            TAVRN_GTT_EXPIRY_QUERY_FOUND;
        for (index = 0u; index < TAVRN_GTT_CAPACITY - 1u; index++) {
            if (!adva_equal(&members[index], &oldest)) {
                uint32_t expected_evidence_at = index < 2u ? first_evidence_ms :
                    first_evidence_ms + (uint32_t)(index - 1u);

                ok &= expiry_snapshot_query(&fixture.gtt, &members[index],
                                            replacement_at, &snapshot) ==
                    TAVRN_GTT_EXPIRY_QUERY_FOUND &&
                    snapshot.last_evidence_ms == expected_evidence_at;
            }
        }
    }

    /* SYNC owns generation allocation.  Every changed row receives a fresh
     * nonzero revision/generation; duplicate and invalid bulk merges are
     * atomic no-ops and callers cannot jump the generation. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_adva_t second_subject = adva(adva_c);
        tavrn_gtt_expiry_snapshot_t first;
        tavrn_gtt_expiry_snapshot_t second;
        tavrn_gtt_expiry_snapshot_t after_first;
        tavrn_gtt_expiry_snapshot_t after_second;
        tavrn_gtt_sync_record_t records[2];
        tavrn_gtt_sync_record_t invalid_records[2];
        expiry_gtt_state_t before_duplicate;
        expiry_gtt_state_t after_duplicate;
        expiry_gtt_state_t before_invalid;
        expiry_gtt_state_t after_invalid;

        ok &= fixture_init(&fixture, 0u);
        memset(records, 0, sizeof(records));
        records[0].identity = subject;
        records[0].remaining_lifetime_ms = 73u;
        records[0].serial = 4u;
        records[0].serial_present = 1u;
        records[0].hop_count = 2u;
        records[1] = records[0];
        records[1].identity = second_subject;
        records[1].remaining_lifetime_ms = 91u;
        records[1].serial = 5u;
        records[1].hop_count = 3u;
        ok &= phase5_gtt_sync_merge(&fixture.gtt, records, 2u, 20u) ==
            TAVRN_GTT_SYNC_MERGE_COMMITTED;
        if (expiry_snapshot_query(&fixture.gtt, &subject, 20u, &first) !=
            TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &=
            first.revision != 0u && first.storage_generation != 0u &&
            first.last_evidence_ms == 20u &&
            first.hard_deadline_ms == 20u + records[0].remaining_lifetime_ms &&
            first.direct == 0u && first.last_direct_evidence_ms == 0u;
        if (expiry_snapshot_query(&fixture.gtt, &second_subject, 20u, &second) !=
            TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &=
            second.revision != 0u && second.storage_generation != 0u &&
            second.revision != first.revision &&
            second.storage_generation != first.storage_generation &&
            second.last_evidence_ms == 20u &&
            second.hard_deadline_ms == 20u + records[1].remaining_lifetime_ms &&
            second.direct == 0u && second.last_direct_evidence_ms == 0u;
        if (!capture_gtt_state(&fixture, 20u, &before_duplicate)) {
            return 0;
        }
        ok &= phase5_gtt_sync_merge(&fixture.gtt, records, 2u, 20u) ==
            TAVRN_GTT_SYNC_MERGE_UNCHANGED;
        if (!capture_gtt_state(&fixture, 20u, &after_duplicate)) {
            return 0;
        }
        ok &= gtt_states_equal(&before_duplicate, &after_duplicate);
        memcpy(invalid_records, records, sizeof(invalid_records));
        memset(&invalid_records[1].identity, 0, sizeof(invalid_records[1].identity));
        if (!capture_gtt_state(&fixture, 20u, &before_invalid)) {
            return 0;
        }
        ok &= phase5_gtt_sync_merge(&fixture.gtt, invalid_records, 2u, 21u) ==
            TAVRN_GTT_SYNC_MERGE_INVALID;
        if (!capture_gtt_state(&fixture, 20u, &after_invalid)) {
            return 0;
        }
        ok &= gtt_states_equal(&before_invalid, &after_invalid);
        if (expiry_snapshot_query(&fixture.gtt, &subject, 21u, &after_first) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND ||
            expiry_snapshot_query(&fixture.gtt, &second_subject, 21u, &after_second) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &=
            expiry_snapshots_equal(&first, &after_first) &&
            expiry_snapshots_equal(&second, &after_second) &&
            after_first.storage_generation == first.storage_generation &&
            after_second.storage_generation == second.storage_generation;

        #if defined(TAVRN_MAINTENANCE_EXPIRY_DEMAND_API)
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t demand;
        tavrn_gtt_expiry_snapshot_t departure;

        memset(&candidate, 0, sizeof(candidate));
        candidate.identity = subject;
        candidate.revision = first.revision - 1u;
        candidate.hard_deadline_ms = first.hard_deadline_ms;
        memset(&demand, 0, sizeof(demand));
        if (phase5_maintenance_demand(&fixture.maintenance, &subject, 93u, &demand) !=
            TAVRN_MAINTENANCE_DEMAND_OK) {
            return 0;
        }
        ok &=
            demand.snapshot_available != 0u && adva_equal(&demand.identity, &subject) &&
            demand.gtt_revision == first.revision;
        memset(&departure, 0, sizeof(departure));
        ok &= phase5_maintenance_checked_departure(&fixture.maintenance, &candidate,
                                                    93u, &departure) ==
            TAVRN_GTT_CHECKED_STALE_REVISION;
        candidate.revision = first.revision;
        memset(&demand, 0, sizeof(demand));
        if (phase5_maintenance_demand(&fixture.maintenance, &subject, 93u, &demand) !=
            TAVRN_MAINTENANCE_DEMAND_OK) {
            return 0;
        }
        ok &=
            demand.snapshot_available != 0u && demand.gtt_revision == candidate.revision;
        memset(&departure, 0, sizeof(departure));
        ok &= phase5_maintenance_checked_departure(&fixture.maintenance, &candidate,
                                                    93u, &departure) ==
                TAVRN_GTT_CHECKED_DEPARTED &&
            departure.revision != 0u && departure.storage_generation != 0u &&
            departure.revision != first.revision &&
            departure.departed_deadline_ms == 93u +
                fixture.gtt.config.departed_retention_ms;
        #endif
    }
    {
        expiry_fixture_t fixture;
        tavrn_adva_t mentor = adva(adva_b);
        tavrn_validated_control_t page;
        tavrn_gtt_expiry_snapshot_t imported;
        tavrn_gtt_expiry_snapshot_t repeated;

        /* The real Phase 4 SYNC_DATA ingestion path must end in the future
         * centralized GTT merge.  It is deliberately not a test-side candidate
         * storage/deadline mutation. */
        if (!fixture_init(&fixture, 0u) ||
            !begin_expiry_sync_page_wait(&fixture, &mentor, 0x6811u, 1u)) {
            return 0;
        }
        page = sync_data_page(&fixture.gtt.config.local_identity, 0x6811u,
                              &mentor, 0x4321u, 2u, 1u);
        if (tavrn_mentorship_ingest_sync_data(&fixture.mentorship, &mentor, &page, 23u) !=
                TAVRN_MENTORSHIP_OK ||
            expiry_snapshot_query(&fixture.gtt, &mentor, 23u, &imported) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND ||
            tavrn_mentorship_ingest_sync_data(&fixture.mentorship, &mentor, &page, 23u) !=
                TAVRN_MENTORSHIP_SYNC_PAGE_DUPLICATE ||
            expiry_snapshot_query(&fixture.gtt, &mentor, 23u, &repeated) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= imported.serial == 0x4321u && imported.serial_present != 0u &&
            imported.hop_count == 2u && imported.direct == 0u &&
            imported.soft_deadline_ms == 23u + fixture.gtt.config.soft_expiry_ms &&
            imported.hard_deadline_ms == 40023u && imported.revision != 0u &&
            imported.storage_generation != 0u && expiry_snapshots_equal(&imported, &repeated) &&
            mentorship_sync_source_uses_central_merge();
    }
    return ok;
}

static int test_gtt_03_provenance_contract(void)
{
    static const tavrn_gtt_provenance_t imported[] = {
        TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
        TAVRN_GTT_PROVENANCE_IMPORTED_METADATA,
        TAVRN_GTT_PROVENANCE_IMPORTED_TC_SUBJECT,
    };
    tavrn_gtt_evidence_t input;
    uint8_t index;
    int ok = 1;

    for (index = 0u; index < sizeof(imported) / sizeof(imported[0]); index++) {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = generated_adva((uint8_t)(0x61u + index));
        tavrn_gtt_expiry_snapshot_t snapshot;

        ok &= fixture_init(&fixture, 0u);
        input = evidence(subject, 1u, 1u, 1u);
        ok &= observe_expected(&fixture, &input, imported[index], 10u,
                               TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u, &snapshot) &&
            expiry_snapshot_query(&fixture.gtt, &subject, 10u, &snapshot) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND && snapshot.direct == 0u &&
            snapshot.last_direct_evidence_ms == 0u;
    }
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 1u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t after;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_DIRECT_OUTER_TRANSMITTER,
                              20u, TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 1u,
                              &before)) {
            return 0;
        }
        ok &= before.last_direct_evidence_ms == 20u;
        input.serial = 2u;
        input.hop_count = 2u;
        ok &= observe_expiry(&fixture.gtt, &input,
                             TAVRN_GTT_PROVENANCE_DIRECT_OUTER_TRANSMITTER,
                             21u, &after) == TAVRN_GTT_EXPIRY_OBSERVE_INVALID;
        if (expiry_snapshot_query(&fixture.gtt, &subject, 21u, &after) !=
            TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= expiry_snapshots_equal(&before, &after);
        input.hop_count = 1u;
        input.serial = 1u;
        ok &= observe_expiry(&fixture.gtt, &input,
                             TAVRN_GTT_PROVENANCE_DIRECT_OUTER_TRANSMITTER,
                             22u, &after) == TAVRN_GTT_EXPIRY_OBSERVE_STALE;
        if (expiry_snapshot_query(&fixture.gtt, &subject, 22u, &after) !=
            TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= expiry_snapshots_equal(&before, &after);
        input.serial_present = 0u;
        input.serial = 0xbeefu;
        ok &= observe_expected(&fixture, &input,
                               TAVRN_GTT_PROVENANCE_DIRECT_OUTER_TRANSMITTER,
                               23u, TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 3u, 1u,
                               &after);
        if (expiry_snapshot_query(&fixture.gtt, &subject, 23u, &after) !=
            TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        /* Serial-absent directness hashes canonical zero serial bytes but never
         * erases an already known latest serial. */
        ok &= after.last_direct_evidence_ms == 23u && after.serial_present != 0u &&
            after.serial == 1u;
    }
    {
        expiry_fixture_t fixture;
        tavrn_direct_peer_t outer = peer(adva_c, TAVRN_IDENTITY_SID8);
        tavrn_direct_peer_t payload_origin = peer(adva_d, TAVRN_IDENTITY_SID8);
        tavrn_direct_peer_t overheard_outer;
        tavrn_decoded_frame_t frame;
        ble_mesh_sched_event_t event;
        aodv_action_t action;
        tavrn_router_application_hooks_t delivery_hooks;
        tavrn_gtt_evidence_t outer_evidence;
        tavrn_gtt_evidence_t outer_seed_evidence;
        tavrn_gtt_evidence_t payload_evidence;
        tavrn_gtt_evidence_t overheard_evidence;
        tavrn_gtt_expiry_snapshot_t outer_snapshot;
        tavrn_gtt_expiry_snapshot_t before_duplicate;
        tavrn_gtt_expiry_snapshot_t after_duplicate;
        tavrn_gtt_expiry_snapshot_t payload_snapshot;
        tavrn_gtt_expiry_snapshot_t local_snapshot;
        tavrn_router_event_status_t router_status;
        aodv_action_poll_status_t action_status;

        /* This is the real router -> FULL observation path.  The test never
         * chooses provenance: a committed non-HELLO DATA RX must cause the
         * future FULL hook to classify only its exact outer AdvA as generic
         * direct evidence. */
        if (!fixture_init_sid8(&fixture)) {
            return 0;
        }
        memset(&delivery_hooks, 0, sizeof(delivery_hooks));
        delivery_hooks.reserve = accept_delivery_reservation;
        delivery_hooks.commit = accept_delivery_completion;
        delivery_hooks.cancel = accept_delivery_completion;
        if (tavrn_router_set_application_hooks(&fixture.router, &delivery_hooks) !=
            TAVRN_ROUTER_APPLICATION_HOOK_OK) {
            return 0;
        }
        memset(&outer_evidence, 0, sizeof(outer_evidence));
        outer_evidence.identity = outer.adva;
        outer_evidence.hop_count = 1u;
        outer_evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
        outer_seed_evidence = outer_evidence;
        outer_seed_evidence.serial_present = 1u;
        payload_evidence = outer_seed_evidence;
        payload_evidence.identity = payload_origin.adva;
        memset(&overheard_outer, 0, sizeof(overheard_outer));
        overheard_outer.adva = generated_adva(0x62u);
        overheard_outer.logical_id.width = TAVRN_IDENTITY_SID8;
        overheard_outer.logical_id.value = overheard_outer.adva.bytes[0];
        overheard_evidence = outer_seed_evidence;
        overheard_evidence.identity = overheard_outer.adva;
        if (observe_expiry(&fixture.gtt, &outer_seed_evidence,
                           TAVRN_GTT_PROVENANCE_IMPORTED_METADATA, 29u, NULL) !=
                TAVRN_GTT_EXPIRY_OBSERVE_ADDED ||
            observe_expiry(&fixture.gtt, &payload_evidence,
                           TAVRN_GTT_PROVENANCE_IMPORTED_METADATA, 29u, NULL) !=
                TAVRN_GTT_EXPIRY_OBSERVE_ADDED ||
            observe_expiry(&fixture.gtt, &overheard_evidence,
                           TAVRN_GTT_PROVENANCE_IMPORTED_METADATA, 29u, NULL) !=
                TAVRN_GTT_EXPIRY_OBSERVE_ADDED) {
            return 0;
        }
        memset(&frame, 0, sizeof(frame));
        frame.type = TAVRN_WIRE_DATA;
        frame.network_id = NETWORK_ID;
        frame.transmitter = outer;
        frame.detail.data.immediate_receiver = fixture.link.config.local_peer.logical_id;
        frame.detail.data.data = link_data(payload_origin.logical_id,
                                           fixture.link.config.local_peer.logical_id,
                                           0x5a01u);
        if (!make_committed_rx_event(&frame, &fixture.link.config.local_peer, adva_c,
                                      &event)) {
            return 0;
        }
        router_status = tavrn_router_handle_scheduler_event(&fixture.router, &event, 30u);
        action_status = aodv_core_poll_action(&fixture.aodv, &action);
        ok &= router_status == TAVRN_ROUTER_EVENT_OK &&
            action_status == AODV_ACTION_POLL_OK &&
            action.type == AODV_ACTION_DELIVER_DATA;
        memset(&outer_snapshot, 0, sizeof(outer_snapshot));
        ok &= expiry_snapshot_query(&fixture.gtt, &outer.adva, 30u, &outer_snapshot) ==
                 TAVRN_GTT_EXPIRY_QUERY_FOUND && outer_snapshot.direct != 0u &&
            outer_snapshot.last_direct_evidence_ms == 30u &&
            outer_snapshot.serial_present != 0u &&
            jittered_deadlines_match(&fixture, &outer_evidence, 30u, &outer_snapshot) &&
            expiry_snapshot_query(&fixture.gtt, &payload_origin.adva, 30u,
                                  &payload_snapshot) == TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            payload_snapshot.direct == 0u &&
            expiry_snapshot_query(&fixture.gtt, &fixture.gtt.config.local_identity, 30u,
                                  &local_snapshot) == TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            local_snapshot.direct == 0u;

        before_duplicate = outer_snapshot;
        (void)tavrn_router_handle_scheduler_event(&fixture.router, &event, 31u);
        ok &= expiry_snapshot_query(&fixture.gtt, &outer.adva, 31u, &outer_snapshot) ==
                 TAVRN_GTT_EXPIRY_QUERY_FOUND && outer_snapshot.direct != 0u &&
            outer_snapshot.last_direct_evidence_ms == 31u &&
            !expiry_snapshots_equal(&outer_snapshot, &before_duplicate) &&
            jittered_deadlines_match(&fixture, &outer_evidence, 31u, &outer_snapshot);
        after_duplicate = outer_snapshot;

        event.adv_data[0] ^= 0x01u;
        (void)tavrn_router_handle_scheduler_event(&fixture.router, &event, 32u);
        ok &= expiry_snapshot_query(&fixture.gtt, &outer.adva, 32u, &outer_snapshot) ==
                 TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            expiry_snapshots_equal(&outer_snapshot, &after_duplicate);

        frame.transmitter = overheard_outer;
        frame.detail.data.immediate_receiver = outer.logical_id;
        if (!make_committed_rx_event(&frame, &fixture.link.config.local_peer,
                                     overheard_outer.adva.bytes,
                                     &event)) {
            return 0;
        }
        (void)tavrn_router_handle_scheduler_event(&fixture.router, &event, 33u);
        ok &= expiry_snapshot_query(&fixture.gtt, &overheard_outer.adva, 33u,
                                  &payload_snapshot) == TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            payload_snapshot.direct == 0u &&
            payload_snapshot.last_direct_evidence_ms == 0u;
    }
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 1u);
        tavrn_gtt_expiry_snapshot_t snapshot;
        tavrn_gtt_sync_record_t record;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                              20u, TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 1u,
                              &snapshot)) {
            return 0;
        }
        input.serial = 2u;
        if (!observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_METADATA, 21u,
                              TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 3u, 1u,
                              &snapshot) ||
            expiry_snapshot_query(&fixture.gtt, &subject, 21u, &snapshot) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= snapshot.last_direct_evidence_ms == 20u;
        input.serial = 3u;
        if (!observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_TC_SUBJECT, 22u,
                              TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 4u, 1u,
                              &snapshot) ||
            expiry_snapshot_query(&fixture.gtt, &subject, 22u, &snapshot) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= snapshot.last_direct_evidence_ms == 20u;
        memset(&record, 0, sizeof(record));
        record.identity = subject;
        record.remaining_lifetime_ms = 40000u;
        record.serial = 4u;
        record.serial_present = 1u;
        record.hop_count = 1u;
        if (phase5_gtt_sync_merge(&fixture.gtt, &record, 1u, 23u) !=
                TAVRN_GTT_SYNC_MERGE_COMMITTED ||
            expiry_snapshot_query(&fixture.gtt, &subject, 23u, &snapshot) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= snapshot.revision == 5u && snapshot.storage_generation == 5u &&
            snapshot.last_evidence_ms == 23u &&
            snapshot.hard_deadline_ms == 23u + record.remaining_lifetime_ms &&
            snapshot.direct != 0u && snapshot.last_direct_evidence_ms == 20u;
    }
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 2u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t after;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 10u,
                              TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u, &before)) {
            return 0;
        }
        input.serial = 2u;
        input.hop_count = 1u;
        ok &= observe_expiry(&fixture.gtt, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                             11u, &after) == TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED;
        if (expiry_snapshot_query(&fixture.gtt, &subject, 11u, &after) !=
            TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= after.revision == before.revision + 1u && after.direct != 0u &&
            after.serial == 2u && after.last_direct_evidence_ms == 11u;
    }
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 2u, 1u, 1u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t after;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                              20u, TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 1u,
                              &before)) {
            return 0;
        }
        input.serial = 1u;
        ok &= observe_expiry(&fixture.gtt, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                             21u, &after) == TAVRN_GTT_EXPIRY_OBSERVE_STALE;
        if (expiry_snapshot_query(&fixture.gtt, &subject, 21u, &after) !=
            TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= expiry_snapshots_equal(&before, &after);
        input.serial = 0x8002u;
        ok &= observe_expiry(&fixture.gtt, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                             22u, &after) == TAVRN_GTT_EXPIRY_OBSERVE_STALE;
        if (expiry_snapshot_query(&fixture.gtt, &subject, 22u, &after) !=
            TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= expiry_snapshots_equal(&before, &after);
        input.serial_present = 0u;
        input.serial = 0xbeefu;
        ok &= observe_expiry(&fixture.gtt, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                             23u, &after) == TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED;
        if (expiry_snapshot_query(&fixture.gtt, &subject, 23u, &after) !=
            TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= after.serial_present != 0u && after.serial == before.serial &&
            after.last_direct_evidence_ms == 23u &&
            after.revision == before.revision + 1u;
    }
    {
        expiry_fixture_t fixture;
        tavrn_gtt_expiry_snapshot_t snapshot;

        ok &= fixture_init(&fixture, 0u);
        input = evidence(adva(adva_b), 1u, 1u, 1u);
        ok &= observe_expected(&fixture, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                               20u, TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 1u,
                               &snapshot) &&
            expiry_snapshot_query(&fixture.gtt, &input.identity, 20u, &snapshot) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND && snapshot.last_direct_evidence_ms == 20u;
        input.serial = 2u;
        ok &= observe_expected(&fixture, &input,
                               TAVRN_GTT_PROVENANCE_DIRECT_BOOTSTRAP, 21u,
                               TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 3u, 1u,
                               &snapshot) &&
            expiry_snapshot_query(&fixture.gtt, &input.identity, 21u, &snapshot) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND && snapshot.last_direct_evidence_ms == 21u;
        input = departure_evidence(input.identity, 3u, 1u);
        ok &= observe_expected(&fixture, &input,
                               TAVRN_GTT_PROVENANCE_CONFIRMED_DEPARTURE, 22u,
                               TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 4u, 0u,
                               &snapshot) &&
            expiry_snapshot_query(&fixture.gtt, &input.identity, 22u, &snapshot) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND && snapshot.last_direct_evidence_ms == 0u;
        input = evidence(input.identity, 4u, 1u, 1u);
        ok &= observe_expected(&fixture, &input,
                               TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 23u,
                               TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 5u, 0u,
                               &snapshot) &&
            expiry_snapshot_query(&fixture.gtt, &input.identity, 23u, &snapshot) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND && snapshot.direct == 0u &&
            snapshot.last_direct_evidence_ms == 0u;
        input = departure_evidence(input.identity, 5u, 1u);
        ok &= observe_expected(&fixture, &input,
                               TAVRN_GTT_PROVENANCE_CONFIRMED_DEPARTURE, 24u,
                               TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 6u, 0u,
                               &snapshot) &&
            expiry_snapshot_query(&fixture.gtt, &input.identity, 24u, &snapshot) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND && snapshot.last_direct_evidence_ms == 0u;
        input = evidence(input.identity, 6u, 1u, 1u);
        ok &= observe_expected(&fixture, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                               25u, TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 7u, 1u,
                               &snapshot) &&
            expiry_snapshot_query(&fixture.gtt, &input.identity, 25u, &snapshot) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND && snapshot.last_direct_evidence_ms == 25u;
        input.serial = 7u;
        ok &= observe_expected(&fixture, &input,
                               TAVRN_GTT_PROVENANCE_DIRECT_INCARNATION, 26u,
                               TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 8u, 1u,
                               &snapshot) &&
            expiry_snapshot_query(&fixture.gtt, &input.identity, 26u, &snapshot) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND && snapshot.last_direct_evidence_ms == 26u;
    }
    return ok;
}

static int test_gtt_05_visibility_contract(void)
{
    expiry_fixture_t fixture;
    tavrn_gtt_evidence_t input;
    tavrn_gtt_expiry_snapshot_t snapshots[TAVRN_GTT_CAPACITY];
    tavrn_gtt_snapshot_t active_snapshots[TAVRN_GTT_CAPACITY];
    tavrn_gtt_expiry_snapshot_t snapshot;
    tavrn_gtt_expiry_snapshot_t demanded_observation;
    tavrn_gtt_expiry_snapshot_t undemanded_observation;
    tavrn_gtt_expiry_snapshot_t demanded_query;
    tavrn_gtt_expiry_snapshot_t undemanded_query;
    tavrn_gtt_expiry_snapshot_t self_query;
    tavrn_gtt_expiry_snapshot_t departed_query;
    tavrn_gtt_application_command_t command;
    tavrn_gtt_application_command_result_t command_result;
    expiry_gtt_state_t state_before;
    expiry_gtt_state_t state_after;
    tavrn_adva_t demanded = adva(adva_b);
    tavrn_adva_t undemanded = adva(adva_c);
    tavrn_adva_t departed = generated_adva(0x71u);
    tavrn_adva_t unknown = generated_adva(0x72u);
    tavrn_adva_t invalid = { { 0u } };
    const tavrn_gtt_expiry_snapshot_t empty_snapshot = { 0 };
    const tavrn_gtt_expiry_snapshot_t *demanded_known;
    const tavrn_gtt_expiry_snapshot_t *undemanded_known;
    const tavrn_gtt_expiry_snapshot_t *self_known;
    uint32_t hard_expired_at;
    uint8_t count = 0u;
    tavrn_gtt_query_status_t active_status;
    int ok = 1;

    if (!fixture_init(&fixture, 0u)) {
        return 0;
    }
    input = evidence(demanded, 1u, 1u, 2u);
    if (!observe_expected(&fixture, &input,
                          TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 10u,
                          TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u,
                          &demanded_observation)) {
        return 0;
    }
    input.identity = undemanded;
    if (!observe_expected(&fixture, &input,
                          TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 11u,
                          TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 3u, 0u,
                          &undemanded_observation)) {
        return 0;
    }
    input = departure_evidence(departed, 1u, 2u);
    if (!observe_expected(&fixture, &input,
                          TAVRN_GTT_PROVENANCE_CONFIRMED_DEPARTURE, 12u,
                          TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 4u, 0u, &snapshot)) {
        return 0;
    }
    hard_expired_at = later_deadline(demanded_observation.hard_deadline_ms,
                                     undemanded_observation.hard_deadline_ms) + 1u;
    memset(&snapshot, 0, sizeof(snapshot));
    ok &= phase5_gtt_request(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_OTHER_TASK,
                               &demanded, 12u, &snapshot) ==
        TAVRN_GTT_APPLICATION_MARSHAL_REQUIRED;
    memset(&snapshot, 0, sizeof(snapshot));
    ok &= phase5_gtt_cancel(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_OTHER_TASK,
                              &demanded, 12u, &snapshot) ==
        TAVRN_GTT_APPLICATION_MARSHAL_REQUIRED;
    memset(&command, 0, sizeof(command));
    command.identity = demanded;
    command.now_ms = 12u;
    command.kind = TAVRN_GTT_APPLICATION_COMMAND_REQUEST;
    command_result = phase5_gtt_apply_application_command(
        &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_OTHER_TASK, command);
    ok &= command_result.status == TAVRN_GTT_APPLICATION_MARSHAL_REQUIRED;
    command_result = phase5_gtt_apply_application_command(
        &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, command);
    ok &= command_result.status == TAVRN_GTT_APPLICATION_CHANGED &&
        adva_equal(&command_result.snapshot.identity, &demanded) &&
        command_result.snapshot.hop_count == 2u &&
        command_result.snapshot.application_requested != 0u;
    command_result = phase5_gtt_apply_application_command(
        &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, command);
    ok &= command_result.status == TAVRN_GTT_APPLICATION_UNCHANGED_DUPLICATE &&
        adva_equal(&command_result.snapshot.identity, &demanded) &&
        command_result.snapshot.application_requested != 0u;
    command.kind = TAVRN_GTT_APPLICATION_COMMAND_QUERY;
    command.now_ms = hard_expired_at;
    command.query_time_ms = hard_expired_at;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_before)) {
        return 0;
    }
    command_result = phase5_gtt_apply_application_command(
        &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_OTHER_TASK, command);
    ok &= command_result.query_status == TAVRN_GTT_EXPIRY_QUERY_MARSHAL_REQUIRED;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_after)) {
        return 0;
    }
    ok &= gtt_states_equal(&state_before, &state_after);
    command_result = phase5_gtt_apply_application_command(
        &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, command);
    ok &= command_result.query_status == TAVRN_GTT_EXPIRY_QUERY_FOUND &&
        adva_equal(&command_result.snapshot.identity, &demanded) &&
        command_result.snapshot.freshness == TAVRN_GTT_FRESHNESS_HARD_EXPIRED &&
        command_result.snapshot.application_requested != 0u;
    demanded_query = command_result.snapshot;
    ok &= enumerate_known(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_OTHER_TASK,
                          hard_expired_at, snapshots, TAVRN_GTT_CAPACITY, &count) ==
        TAVRN_GTT_EXPIRY_QUERY_MARSHAL_REQUIRED;
    ok &= enumerate_known(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                          hard_expired_at, snapshots, 2u, &count) ==
        TAVRN_GTT_EXPIRY_QUERY_OUTPUT_TOO_SMALL;
    if (enumerate_known(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                        hard_expired_at, snapshots, TAVRN_GTT_CAPACITY, &count) !=
        TAVRN_GTT_EXPIRY_QUERY_FOUND) {
        return 0;
    }
    ok &= count == 3u;
    demanded_known = find_known_snapshot(snapshots, count, &demanded);
    undemanded_known = find_known_snapshot(snapshots, count, &undemanded);
    self_known = find_known_snapshot(snapshots, count,
                                     &fixture.gtt.config.local_identity);
    if (expiry_snapshot_query(&fixture.gtt, &undemanded, hard_expired_at,
                              &undemanded_query) != TAVRN_GTT_EXPIRY_QUERY_FOUND ||
        expiry_snapshot_query(&fixture.gtt, &fixture.gtt.config.local_identity,
                              hard_expired_at, &self_query) !=
            TAVRN_GTT_EXPIRY_QUERY_FOUND ||
        expiry_snapshot_query(&fixture.gtt, &departed, hard_expired_at,
                              &departed_query) !=
            TAVRN_GTT_EXPIRY_QUERY_FOUND) {
        return 0;
    }
    ok &= demanded_known != NULL && undemanded_known != NULL && self_known != NULL &&
        find_known_snapshot(snapshots, count, &departed) == NULL &&
        expiry_snapshots_equal(demanded_known, &demanded_query) &&
        expiry_snapshots_equal(undemanded_known, &undemanded_query) &&
        expiry_snapshots_equal(self_known, &self_query) &&
        demanded_known->application_requested != 0u &&
        undemanded_known->application_requested == 0u &&
        self_known->application_requested == 0u;
    memset(active_snapshots, 0, sizeof(active_snapshots));
    count = 0u;
    active_status = tavrn_gtt_enumerate_active(&fixture.gtt, hard_expired_at,
                                               active_snapshots,
                                               TAVRN_GTT_CAPACITY, &count);
    ok &= active_status == TAVRN_GTT_QUERY_FOUND && count == 1u &&
        active_snapshots_contain(active_snapshots, count,
                                 &fixture.gtt.config.local_identity) &&
        !active_snapshots_contain(active_snapshots, count, &demanded) &&
        !active_snapshots_contain(active_snapshots, count, &undemanded);
    ok &= expiry_snapshot_query(&fixture.gtt, &unknown, hard_expired_at,
                                &snapshot) == TAVRN_GTT_EXPIRY_QUERY_NOT_FOUND;
    ok &= expiry_snapshot_query(&fixture.gtt, &invalid, hard_expired_at,
                                &snapshot) == TAVRN_GTT_EXPIRY_QUERY_INVALID;
    ok &= expiry_snapshot_query(&fixture.gtt, &demanded, hard_expired_at, NULL) ==
        TAVRN_GTT_EXPIRY_QUERY_INVALID;
    ok &= expiry_snapshot_query(NULL, &demanded, hard_expired_at, &snapshot) ==
        TAVRN_GTT_EXPIRY_QUERY_INVALID;
    ok &= enumerate_known(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                          hard_expired_at, NULL, 1u, &count) ==
        TAVRN_GTT_EXPIRY_QUERY_INVALID;
    ok &= enumerate_known(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                          hard_expired_at, snapshots, TAVRN_GTT_CAPACITY, NULL) ==
        TAVRN_GTT_EXPIRY_QUERY_INVALID;
    command.kind = TAVRN_GTT_APPLICATION_COMMAND_CANCEL;
    command_result = phase5_gtt_apply_application_command(
        &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, command);
    ok &= command_result.status == TAVRN_GTT_APPLICATION_CANCELED &&
        adva_equal(&command_result.snapshot.identity, &demanded) &&
        command_result.snapshot.hop_count == 2u &&
        command_result.snapshot.application_requested == 0u;
    command_result = phase5_gtt_apply_application_command(
        &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, command);
    ok &= command_result.status == TAVRN_GTT_APPLICATION_UNCHANGED_DUPLICATE &&
        command_result.snapshot.application_requested == 0u;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_before)) {
        return 0;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    ok &= phase5_gtt_request(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                               &fixture.gtt.config.local_identity, hard_expired_at,
                               &snapshot) ==
        TAVRN_GTT_APPLICATION_SELF;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_after)) {
        return 0;
    }
    ok &= gtt_states_equal(&state_before, &state_after);

    if (!capture_gtt_state(&fixture, hard_expired_at, &state_before)) {
        return 0;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    ok &= phase5_gtt_request(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                               &unknown, hard_expired_at, &snapshot) ==
        TAVRN_GTT_APPLICATION_NOT_FOUND;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_after)) {
        return 0;
    }
    ok &= gtt_states_equal(&state_before, &state_after);

    if (!capture_gtt_state(&fixture, hard_expired_at, &state_before)) {
        return 0;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    ok &= phase5_gtt_request(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                               &departed, hard_expired_at, &snapshot) ==
        TAVRN_GTT_APPLICATION_DEPARTED;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_after)) {
        return 0;
    }
    ok &= gtt_states_equal(&state_before, &state_after);

    if (!capture_gtt_state(&fixture, hard_expired_at, &state_before)) {
        return 0;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    ok &= phase5_gtt_request(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                               NULL, hard_expired_at, &snapshot) ==
        TAVRN_GTT_APPLICATION_INVALID;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_after)) {
        return 0;
    }
    ok &= gtt_states_equal(&state_before, &state_after);

    if (!capture_gtt_state(&fixture, hard_expired_at, &state_before)) {
        return 0;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    ok &= phase5_gtt_cancel(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                              NULL, hard_expired_at, &snapshot) ==
        TAVRN_GTT_APPLICATION_INVALID;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_after)) {
        return 0;
    }
    ok &= gtt_states_equal(&state_before, &state_after);

    command.identity = invalid;
    command.now_ms = hard_expired_at;
    command.query_time_ms = hard_expired_at;
    command.kind = TAVRN_GTT_APPLICATION_COMMAND_REQUEST;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_before)) {
        return 0;
    }
    command_result = phase5_gtt_apply_application_command(
        &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, command);
    ok &= command_result.status == TAVRN_GTT_APPLICATION_INVALID;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_after)) {
        return 0;
    }
    ok &= gtt_states_equal(&state_before, &state_after);

    command.kind = TAVRN_GTT_APPLICATION_COMMAND_CANCEL;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_before)) {
        return 0;
    }
    command_result = phase5_gtt_apply_application_command(
        &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, command);
    ok &= command_result.status == TAVRN_GTT_APPLICATION_INVALID;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_after)) {
        return 0;
    }
    ok &= gtt_states_equal(&state_before, &state_after);

    command.kind = TAVRN_GTT_APPLICATION_COMMAND_QUERY;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_before)) {
        return 0;
    }
    command_result = phase5_gtt_apply_application_command(
        &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, command);
    ok &= command_result.query_status == TAVRN_GTT_EXPIRY_QUERY_INVALID;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_after)) {
        return 0;
    }
    ok &= gtt_states_equal(&state_before, &state_after);

    command.identity = fixture.gtt.config.local_identity;
    command.now_ms = hard_expired_at;
    command.query_time_ms = hard_expired_at;
    command.kind = TAVRN_GTT_APPLICATION_COMMAND_REQUEST;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_before)) {
        return 0;
    }
    memset(&command_result, 0xa5, sizeof(command_result));
    command_result = phase5_gtt_apply_application_command(
        &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, command);
    ok &= command_result.status == TAVRN_GTT_APPLICATION_SELF &&
        expiry_snapshots_equal(&command_result.snapshot, &self_query);
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_after)) {
        return 0;
    }
    ok &= gtt_states_equal(&state_before, &state_after);

    command.identity = unknown;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_before)) {
        return 0;
    }
    memset(&command_result, 0xa5, sizeof(command_result));
    command_result = phase5_gtt_apply_application_command(
        &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, command);
    ok &= command_result.status == TAVRN_GTT_APPLICATION_NOT_FOUND &&
        expiry_snapshots_equal(&command_result.snapshot, &empty_snapshot);
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_after)) {
        return 0;
    }
    ok &= gtt_states_equal(&state_before, &state_after);

    command.identity = departed;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_before)) {
        return 0;
    }
    memset(&command_result, 0xa5, sizeof(command_result));
    command_result = phase5_gtt_apply_application_command(
        &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, command);
    ok &= command_result.status == TAVRN_GTT_APPLICATION_DEPARTED &&
        expiry_snapshots_equal(&command_result.snapshot, &departed_query);
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_after)) {
        return 0;
    }
    ok &= gtt_states_equal(&state_before, &state_after);

    command.identity = unknown;
    command.kind = TAVRN_GTT_APPLICATION_COMMAND_QUERY;
    command.query_time_ms = hard_expired_at;
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_before)) {
        return 0;
    }
    memset(&command_result, 0xa5, sizeof(command_result));
    command_result = phase5_gtt_apply_application_command(
        &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, command);
    ok &= command_result.query_status == TAVRN_GTT_EXPIRY_QUERY_NOT_FOUND &&
        expiry_snapshots_equal(&command_result.snapshot, &empty_snapshot);
    if (!capture_gtt_state(&fixture, hard_expired_at, &state_after)) {
        return 0;
    }
    ok &= gtt_states_equal(&state_before, &state_after);

    #if defined(TAVRN_FULL_EXPIRY_API)
    {
        tavrn_full_application_mailbox_t mailbox;
        tavrn_gtt_application_command_t submitted;
        tavrn_gtt_application_command_t taken;
        tavrn_gtt_application_command_result_t published;
        tavrn_gtt_application_command_result_t invalid_published;
        tavrn_gtt_application_command_result_t consumed;
        tavrn_full_application_mailbox_t mailbox_before;

        memset(&mailbox, 0, sizeof(mailbox));
        memset(&submitted, 0, sizeof(submitted));
        submitted.identity = demanded;
        submitted.kind = TAVRN_GTT_APPLICATION_COMMAND_REQUEST;
        ok &= phase5_full_application_mailbox_init(NULL) ==
                TAVRN_FULL_APPLICATION_MAILBOX_INVALID &&
            phase5_full_application_mailbox_init(&mailbox) ==
                TAVRN_FULL_APPLICATION_MAILBOX_EMPTY &&
            mailbox.command_pending == 0u && mailbox.owner_processing == 0u &&
            mailbox.result_ready == 0u;
        mailbox_before = mailbox;
        ok &= phase5_full_application_mailbox_submit(NULL, submitted) ==
                TAVRN_FULL_APPLICATION_MAILBOX_INVALID &&
            memcmp(&mailbox, &mailbox_before, sizeof(mailbox)) == 0 &&
            phase5_full_application_mailbox_submit(&mailbox, submitted) ==
                TAVRN_FULL_APPLICATION_MAILBOX_QUEUED &&
            mailbox.command_pending != 0u && mailbox.owner_processing == 0u &&
            mailbox.result_ready == 0u &&
            phase5_full_application_mailbox_submit(&mailbox, submitted) ==
                TAVRN_FULL_APPLICATION_MAILBOX_BUSY;
        mailbox_before = mailbox;
        submitted.identity = unknown;
        memset(&taken, 0, sizeof(taken));
        memset(&consumed, 0xa5, sizeof(consumed));
        ok &= phase5_full_application_mailbox_consumer_take(&mailbox, &consumed) ==
                TAVRN_FULL_APPLICATION_MAILBOX_EMPTY &&
            memcmp(&mailbox, &mailbox_before, sizeof(mailbox)) == 0 &&
            phase5_full_application_mailbox_owner_take(NULL, &taken) ==
                TAVRN_FULL_APPLICATION_MAILBOX_INVALID &&
            memcmp(&mailbox, &mailbox_before, sizeof(mailbox)) == 0 &&
            phase5_full_application_mailbox_owner_take(&mailbox, NULL) ==
                TAVRN_FULL_APPLICATION_MAILBOX_INVALID &&
            memcmp(&mailbox, &mailbox_before, sizeof(mailbox)) == 0 &&
            phase5_full_application_mailbox_owner_take(&mailbox, &taken) ==
                TAVRN_FULL_APPLICATION_MAILBOX_PROCESSING &&
            adva_equal(&taken.identity, &demanded) &&
            mailbox.command_pending == 0u && mailbox.owner_processing != 0u &&
            mailbox.result_ready == 0u &&
            phase5_full_application_mailbox_submit(&mailbox, submitted) ==
                TAVRN_FULL_APPLICATION_MAILBOX_BUSY &&
            phase5_full_application_mailbox_owner_take(&mailbox, &taken) ==
                TAVRN_FULL_APPLICATION_MAILBOX_BUSY;
        memset(&published, 0, sizeof(published));
        published.status = TAVRN_GTT_APPLICATION_CHANGED;
        published.snapshot = demanded_query;
        invalid_published = published;
        invalid_published.status = (tavrn_gtt_application_request_status_t)0xffu;
        mailbox_before = mailbox;
        ok &= phase5_full_application_mailbox_owner_publish(NULL, published) ==
                TAVRN_FULL_APPLICATION_MAILBOX_INVALID &&
            memcmp(&mailbox, &mailbox_before, sizeof(mailbox)) == 0 &&
            phase5_full_application_mailbox_consumer_take(NULL, &consumed) ==
                TAVRN_FULL_APPLICATION_MAILBOX_INVALID &&
            memcmp(&mailbox, &mailbox_before, sizeof(mailbox)) == 0 &&
            phase5_full_application_mailbox_owner_publish(&mailbox, invalid_published) ==
                TAVRN_FULL_APPLICATION_MAILBOX_INVALID &&
            memcmp(&mailbox, &mailbox_before, sizeof(mailbox)) == 0 &&
            phase5_full_application_mailbox_consumer_take(&mailbox, NULL) ==
                TAVRN_FULL_APPLICATION_MAILBOX_INVALID &&
            memcmp(&mailbox, &mailbox_before, sizeof(mailbox)) == 0 &&
            phase5_full_application_mailbox_owner_publish(&mailbox, published) ==
                TAVRN_FULL_APPLICATION_MAILBOX_READY && mailbox.command_pending == 0u &&
            mailbox.owner_processing == 0u && mailbox.result_ready != 0u;
        mailbox_before = mailbox;
        ok &= phase5_full_application_mailbox_submit(&mailbox, submitted) ==
                TAVRN_FULL_APPLICATION_MAILBOX_BUSY &&
            phase5_full_application_mailbox_owner_take(&mailbox, &taken) ==
                TAVRN_FULL_APPLICATION_MAILBOX_BUSY &&
            phase5_full_application_mailbox_owner_publish(&mailbox, published) ==
                TAVRN_FULL_APPLICATION_MAILBOX_INVALID &&
            memcmp(&mailbox, &mailbox_before, sizeof(mailbox)) == 0;
        ok &= phase5_full_application_mailbox_consumer_take(&mailbox, NULL) ==
                TAVRN_FULL_APPLICATION_MAILBOX_INVALID &&
            memcmp(&mailbox, &mailbox_before, sizeof(mailbox)) == 0;
        published.snapshot.identity = unknown;
        memset(&consumed, 0, sizeof(consumed));
        ok &= phase5_full_application_mailbox_consumer_take(&mailbox, &consumed) ==
                TAVRN_FULL_APPLICATION_MAILBOX_READY &&
            adva_equal(&consumed.snapshot.identity, &demanded) &&
            mailbox.command_pending == 0u && mailbox.owner_processing == 0u &&
            mailbox.result_ready == 0u &&
            phase5_full_application_mailbox_consumer_take(&mailbox, &consumed) ==
                TAVRN_FULL_APPLICATION_MAILBOX_EMPTY &&
            phase5_full_application_mailbox_submit(&mailbox, submitted) ==
                TAVRN_FULL_APPLICATION_MAILBOX_QUEUED &&
            phase5_full_application_mailbox_owner_take(&mailbox, &taken) ==
                TAVRN_FULL_APPLICATION_MAILBOX_PROCESSING &&
            phase5_full_application_mailbox_owner_publish(&mailbox, published) ==
                TAVRN_FULL_APPLICATION_MAILBOX_READY &&
            phase5_full_application_mailbox_consumer_take(&mailbox, &consumed) ==
                TAVRN_FULL_APPLICATION_MAILBOX_READY;
    }
    #endif

    /* A route is reachability state, not GTT membership.  Use the existing
     * public RREQ/RREP helper and expire the installed route in a fresh fixture. */
    {
        expiry_fixture_t route_fixture;
        tavrn_direct_peer_t next_hop = peer(adva_c, TAVRN_IDENTITY_SID8);
        tavrn_logical_id_t destination = peer(adva_b, TAVRN_IDENTITY_SID8).logical_id;
        aodv_route_snapshot_t route;
        tavrn_gtt_expiry_snapshot_t known[TAVRN_GTT_CAPACITY];
        tavrn_gtt_expiry_snapshot_t before_route_expiry;
        tavrn_gtt_expiry_snapshot_t after_route_expiry;
        uint8_t known_count = 0u;

        if (!fixture_init_sid8(&route_fixture) ||
            !install_route_via(&route_fixture, destination, &next_hop, 100u, &route)) {
            ok = 0;
        } else {
            if (expiry_snapshot_query(&route_fixture.gtt, &demanded,
                                      route.expires_at_ms - 1u,
                                      &before_route_expiry) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
                return 0;
            }
            ok &= tavrn_router_tick(&route_fixture.router, route.expires_at_ms + 1u) ==
                AODV_STATUS_OK;
            ok &= aodv_core_route_snapshot(&route_fixture.aodv, &destination, &route) ==
                    AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_INVALID;
            if (expiry_snapshot_query(&route_fixture.gtt, &demanded,
                                      route.expires_at_ms + 1u,
                                      &after_route_expiry) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
                return 0;
            }
            ok &= expiry_snapshots_equal(&before_route_expiry, &after_route_expiry);
            if (enumerate_known(&route_fixture.gtt,
                                TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                                route.expires_at_ms + 1u, known,
                                TAVRN_GTT_CAPACITY, &known_count) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
                return 0;
            }
            ok &= find_known_snapshot(known, known_count, &demanded) != NULL &&
                expiry_snapshots_equal(find_known_snapshot(known, known_count, &demanded),
                                       &after_route_expiry);
        }
    }

    return ok;
}

static int test_maint_01_real_deadline_and_sweep_contract(void)
{
    int ok = 1;

    /* Ordinary evidence has deterministic FNV jitter, while imported SYNC and
     * departures retain their transferred/fixed unjittered lifetimes. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t first = evidence(subject, 0x1234u, 1u, 2u);
        tavrn_gtt_evidence_t absent = first;
        tavrn_gtt_expiry_snapshot_t snapshot;
        tavrn_gtt_sync_record_t record;

        absent.serial_present = 0u;
        absent.serial = 0xbeefu;
        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &first,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                              0x10203040u, TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u,
                              &snapshot) ||
            !observe_expected(&fixture, &first,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                              0x10203040u, TAVRN_GTT_EXPIRY_OBSERVE_DUPLICATE, 2u, 0u,
                              NULL) ||
            !observe_expected(&fixture, &absent,
                               TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                               0x01020304u, TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 3u, 0u,
                               NULL)) {
            return 0;
        }
        absent.serial = 1u;
        if (!observe_expected(&fixture, &absent,
                               TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                               0x01020304u, TAVRN_GTT_EXPIRY_OBSERVE_DUPLICATE, 3u, 0u,
                               NULL)) {
            return 0;
        }
        memset(&record, 0, sizeof(record));
        record.identity = subject;
        record.remaining_lifetime_ms = 40000u;
        record.serial = 0x1234u;
        record.serial_present = 1u;
        record.hop_count = 2u;
        if (phase5_gtt_sync_merge(&fixture.gtt, &record, 1u, 0x10203050u) !=
                TAVRN_GTT_SYNC_MERGE_COMMITTED ||
            expiry_snapshot_query(&fixture.gtt, &subject, 0x10203050u, &snapshot) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= snapshot.hard_deadline_ms == 0x10203050u + record.remaining_lifetime_ms &&
            snapshot.soft_deadline_ms == 0x10203050u + fixture.gtt.config.soft_expiry_ms &&
            snapshot.direct == 0u;
        first = departure_evidence(subject, 0x1235u, 2u);
        ok &= observe_expected(&fixture, &first,
                               TAVRN_GTT_PROVENANCE_CONFIRMED_DEPARTURE,
                               0x10203051u, TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED,
                               5u, 0u, &snapshot);
    }
    {
        expiry_fixture_t fixture;
        tavrn_adva_t active = adva(adva_b);
        tavrn_adva_t departed_identity = adva(adva_c);
        tavrn_gtt_sync_record_t record;
        tavrn_gtt_expiry_snapshot_t below_soft;
        tavrn_gtt_expiry_snapshot_t repeated;
        tavrn_gtt_expiry_snapshot_t above_soft;
        tavrn_gtt_expiry_snapshot_t departed;

        if (!fixture_init(&fixture, 0u)) {
            return 0;
        }
        memset(&record, 0, sizeof(record));
        record.identity = active;
        record.remaining_lifetime_ms = 100u;
        record.serial = 1u;
        record.serial_present = 1u;
        record.hop_count = 2u;
        if (phase5_gtt_sync_merge(&fixture.gtt, &record, 1u, 100u) !=
                TAVRN_GTT_SYNC_MERGE_COMMITTED ||
            expiry_snapshot_query(&fixture.gtt, &active, 100u, &below_soft) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND ||
            phase5_gtt_sync_merge(&fixture.gtt, &record, 1u, 100u) !=
                TAVRN_GTT_SYNC_MERGE_UNCHANGED ||
            expiry_snapshot_query(&fixture.gtt, &active, 100u, &repeated) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= below_soft.soft_deadline_ms == 200u &&
            below_soft.hard_deadline_ms == 200u &&
            below_soft.departed_deadline_ms == 0u && below_soft.direct == 0u &&
            expiry_snapshots_equal(&below_soft, &repeated);

        record.remaining_lifetime_ms = 400u;
        record.serial = 2u;
        if (phase5_gtt_sync_merge(&fixture.gtt, &record, 1u, 120u) !=
                TAVRN_GTT_SYNC_MERGE_COMMITTED ||
            expiry_snapshot_query(&fixture.gtt, &active, 120u, &above_soft) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= above_soft.soft_deadline_ms == 120u + fixture.gtt.config.soft_expiry_ms &&
            above_soft.hard_deadline_ms == 520u &&
            above_soft.departed_deadline_ms == 0u && above_soft.direct == 0u;

        memset(&record, 0, sizeof(record));
        record.identity = departed_identity;
        record.serial = 3u;
        record.serial_present = 1u;
        record.hop_count = 2u;
        record.departed = 1u;
        if (phase5_gtt_sync_merge(&fixture.gtt, &record, 1u, 130u) !=
                TAVRN_GTT_SYNC_MERGE_COMMITTED ||
            expiry_snapshot_query(&fixture.gtt, &departed_identity, 130u, &departed) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= departed.freshness == TAVRN_GTT_FRESHNESS_DEPARTED &&
            departed.soft_deadline_ms == 130u && departed.hard_deadline_ms == 130u &&
            departed.departed_deadline_ms == 130u + fixture.gtt.config.departed_retention_ms &&
            departed.direct == 0u && departed.last_direct_evidence_ms == 0u;
    }
    {
        expiry_fixture_t fixture;
        tavrn_gtt_evidence_t input = evidence(adva(adva_b), 1u, 1u, 2u);
        tavrn_gtt_expiry_snapshot_t snapshot;

        if (!fixture_init(&fixture, 0xfffffff0u) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                              0xfffffff0u, TAVRN_GTT_EXPIRY_OBSERVE_ADDED,
                              2u, 0u, &snapshot) ||
            !observe_expected(&fixture, &input,
                               TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                               0xfffffff0u, TAVRN_GTT_EXPIRY_OBSERVE_DUPLICATE,
                               2u, 0u, NULL) ||
            expiry_snapshot_query(&fixture.gtt, &input.identity, 0x0000000au,
                                  &snapshot) != TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= snapshot.freshness == TAVRN_GTT_FRESHNESS_ACTIVE;
    }
    {
        expiry_fixture_t fixture;
        tavrn_gtt_config_t large_config;
        tavrn_gtt_evidence_t input = evidence(adva(adva_b), 1u, 1u, 2u);
        tavrn_gtt_expiry_snapshot_t snapshot;

        if (!fixture_init(&fixture, 0u)) {
            return 0;
        }
        large_config = fixture.gtt.config;
        large_config.soft_expiry_ms = 0x40000000u;
        large_config.hard_expiry_ms = 0x50000000u;
        if (tavrn_gtt_init(&fixture.gtt, &fixture.gtt_storage, &large_config, 0u) !=
                TAVRN_GTT_INIT_OK ||
            !observe_expected(&fixture, &input,
                               TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 0u,
                               TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u, &snapshot)) {
            return 0;
        }
        ok &= snapshot.soft_deadline_ms != 0u &&
            snapshot.soft_deadline_ms < snapshot.hard_deadline_ms;
    }

    /* The sweep is cadence-gated.  It reaches a real soft-stale row and two
     * hard-expired imported rows in one bounded pass without starving the
     * later undemanded candidate behind the earlier demanded one. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t soft_subject = adva(adva_b);
        tavrn_adva_t demanded_subject = adva(adva_c);
        tavrn_adva_t undemanded_subject = adva(adva_d);
        tavrn_gtt_evidence_t soft_input = evidence(soft_subject, 1u, 1u, 2u);
        tavrn_gtt_expiry_snapshot_t soft_before;
        tavrn_gtt_expiry_snapshot_t soft_after;
        tavrn_gtt_expiry_snapshot_t demanded_after;
        tavrn_gtt_expiry_snapshot_t undemanded_after;
        tavrn_gtt_expiry_snapshot_t generation_source;
        tavrn_gtt_expiry_snapshot_t request_snapshot;
        tavrn_gtt_sync_record_t records[2];
        tavrn_maintenance_expiry_sweep_snapshot_t sweep;
        tavrn_maintenance_expiry_sweep_snapshot_t due_sweep;
        uint32_t due;
        uint32_t previous_due = 0u;
        uint8_t index;
        uint8_t pass_count = 0u;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &soft_input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 0u,
                              TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u,
                              &soft_before) ||
            phase5_maintenance_sweep(&fixture.maintenance, 0u, &sweep) !=
                TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK) {
            return 0;
        }
        ok &= sweep.pass_performed == 0u && sweep.trace_count == 0u &&
            sweep.next_due_ms == 25u;
        due = sweep.next_due_ms;
        while (deadline_precedes(due, soft_before.soft_deadline_ms) &&
               pass_count < TAVRN_GTT_CAPACITY * 2u) {
            if (phase5_maintenance_sweep(&fixture.maintenance, due, &sweep) !=
                    TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK ||
                sweep.pass_performed == 0u || sweep.trace_count > TAVRN_GTT_CAPACITY ||
                sweep.maximum_copied_candidates > 1u ||
                sweep.next_due_ms != due + 25u) {
                return 0;
            }
            previous_due = due;
            due = sweep.next_due_ms;
            pass_count++;
        }
        if (pass_count == TAVRN_GTT_CAPACITY * 2u ||
            !deadline_precedes(due, soft_before.hard_deadline_ms) ||
            expiry_snapshot_query(&fixture.gtt, &soft_subject, previous_due,
                                  &generation_source) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        memset(records, 0, sizeof(records));
        records[0].identity = demanded_subject;
        records[0].remaining_lifetime_ms = 1u;
        records[0].serial = 1u;
        records[0].serial_present = 1u;
        records[0].hop_count = 2u;
        records[1] = records[0];
        records[1].identity = undemanded_subject;
        records[1].serial = 2u;
        if (phase5_gtt_sync_merge(&fixture.gtt, records, 2u, previous_due) !=
                TAVRN_GTT_SYNC_MERGE_COMMITTED ||
            phase5_gtt_request(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                                &demanded_subject, previous_due, &request_snapshot) !=
                TAVRN_GTT_APPLICATION_CHANGED ||
            expiry_snapshot_query(&fixture.gtt, &soft_subject, due, &soft_before) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND ||
            phase5_maintenance_sweep(&fixture.maintenance, due, &due_sweep) !=
                TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK) {
            return 0;
        }
        ok &= due_sweep.pass_performed != 0u &&
            due_sweep.trace_count <= TAVRN_GTT_CAPACITY &&
            due_sweep.next_due_ms == due + 25u &&
            due_sweep.maximum_copied_candidates == 1u &&
            due_sweep.soft_selected_count >= 1u && due_sweep.hard_selected_count >= 2u &&
            due_sweep.demand_deferred_count >= 1u && due_sweep.departed_count >= 1u &&
            due_sweep.unavailable_count == 0u &&
            due_sweep.soft_stale_control_count == 0u &&
            due_sweep.targeted_control_count == 0u && due_sweep.rreq_control_count == 0u &&
            due_sweep.expiry_tc_control_count == 0u && due_sweep.received_evidence_count == 0u;
        for (index = 0u; index < due_sweep.trace_count; index++) {
            uint8_t other;

            ok &= due_sweep.trace_slots[index] < TAVRN_GTT_CAPACITY;
            for (other = (uint8_t)(index + 1u); other < due_sweep.trace_count; other++) {
                ok &= due_sweep.trace_slots[index] != due_sweep.trace_slots[other];
            }
        }
        if (due_sweep.trace_count != 0u) {
            ok &= due_sweep.next_cursor ==
                (uint8_t)((due_sweep.trace_slots[due_sweep.trace_count - 1u] + 1u) %
                          TAVRN_GTT_CAPACITY);
        }
        if (expiry_snapshot_query(&fixture.gtt, &soft_subject, due, &soft_after) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND ||
            expiry_snapshot_query(&fixture.gtt, &demanded_subject, due,
                                  &demanded_after) != TAVRN_GTT_EXPIRY_QUERY_FOUND ||
            expiry_snapshot_query(&fixture.gtt, &undemanded_subject, due,
                                  &undemanded_after) != TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= soft_after.last_evidence_ms == soft_before.last_evidence_ms &&
            soft_after.revision == soft_before.revision &&
            soft_after.freshness == TAVRN_GTT_FRESHNESS_SOFT_STALE &&
            demanded_after.freshness == TAVRN_GTT_FRESHNESS_HARD_EXPIRED &&
            demanded_after.application_requested != 0u &&
            undemanded_after.freshness == TAVRN_GTT_FRESHNESS_DEPARTED &&
            undemanded_after.departed_deadline_ms == due +
                fixture.gtt.config.departed_retention_ms;
        if (deadline_precedes(due + 1u, due_sweep.next_due_ms) &&
            phase5_maintenance_sweep(&fixture.maintenance, due + 1u, &sweep) ==
                TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK) {
            ok &= sweep.pass_performed == 0u && sweep.trace_count == 0u &&
                sweep.next_cursor == due_sweep.next_cursor &&
                sweep.next_due_ms == due_sweep.next_due_ms;
        } else {
            return 0;
        }
    }

    /* The documented initial cursor is slot zero.  A full table must therefore
     * yield the exact 0..15 cyclic visit once, even when early demanded work is
     * deferred and late undemanded work is departed in that same pass. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t demanded_subject = adva(adva_b);
        tavrn_adva_t undemanded_subject = generated_adva(0xa0u);
        tavrn_gtt_sync_record_t record;
        tavrn_gtt_evidence_t filler;
        tavrn_gtt_expiry_snapshot_t generation_source;
        tavrn_gtt_expiry_snapshot_t demanded_after;
        tavrn_gtt_expiry_snapshot_t undemanded_after;
        tavrn_gtt_expiry_snapshot_t request_snapshot;
        tavrn_maintenance_expiry_sweep_snapshot_t pre;
        tavrn_maintenance_expiry_sweep_snapshot_t due_sweep;
        expiry_side_effect_state_t effects_before;
        expiry_side_effect_state_t effects_after;
        tavrn_gtt_counters_t counters_before;
        uint8_t index;

        if (!fixture_init(&fixture, 0u)) {
            return 0;
        }
        memset(&record, 0, sizeof(record));
        record.identity = demanded_subject;
        record.remaining_lifetime_ms = 1u;
        record.serial = 1u;
        record.serial_present = 1u;
        record.hop_count = 2u;
        if (phase5_gtt_sync_merge(&fixture.gtt, &record, 1u, 0u) !=
            TAVRN_GTT_SYNC_MERGE_COMMITTED ||
            phase5_gtt_request(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                                &demanded_subject, 0u, &request_snapshot) !=
                TAVRN_GTT_APPLICATION_CHANGED) {
            return 0;
        }
        for (index = 0u; index < TAVRN_GTT_CAPACITY - 3u; index++) {
            filler = evidence(generated_adva((uint8_t)(0x80u + index)), 1u, 1u, 2u);
            if (observe_expiry(&fixture.gtt, &filler,
                               TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 0u,
                               &generation_source) != TAVRN_GTT_EXPIRY_OBSERVE_ADDED) {
                return 0;
            }
        }
        if (expiry_snapshot_query(&fixture.gtt, &demanded_subject, 0u,
                                  &generation_source) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        record.identity = undemanded_subject;
        record.serial = 2u;
        if (phase5_gtt_sync_merge(&fixture.gtt, &record, 1u, 0u) !=
                TAVRN_GTT_SYNC_MERGE_COMMITTED ||
            phase5_maintenance_sweep(&fixture.maintenance, 0u, &pre) !=
                TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK ||
            pre.pass_performed != 0u || pre.next_due_ms != 25u || pre.next_cursor != 0u ||
            !capture_side_effect_state(&fixture, &effects_before)) {
            return 0;
        }
        counters_before = *tavrn_gtt_counters(&fixture.gtt);
        if (phase5_maintenance_sweep(&fixture.maintenance, 25u, &due_sweep) !=
                TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK ||
            !capture_side_effect_state(&fixture, &effects_after)) {
            return 0;
        }
        ok &= due_sweep.pass_performed != 0u && due_sweep.trace_count == TAVRN_GTT_CAPACITY &&
            due_sweep.maximum_copied_candidates == 1u && due_sweep.next_cursor == 0u &&
            due_sweep.next_due_ms == 50u && due_sweep.demand_deferred_count >= 1u &&
            due_sweep.unavailable_count == 0u && due_sweep.departed_count >= 1u &&
            due_sweep.scheduler_controls == 0u &&
            due_sweep.aodv_actions == 0u && due_sweep.router_broadcast_generation == 0u &&
            due_sweep.mentorship_tc_generation == 0u &&
            due_sweep.targeted_control_count == 0u && due_sweep.rreq_control_count == 0u &&
            due_sweep.expiry_tc_control_count == 0u && due_sweep.received_evidence_count == 0u &&
            transport_side_effect_states_equal(&effects_before, &effects_after) &&
            tavrn_gtt_counters(&fixture.gtt)->departed == counters_before.departed + 1u &&
            tavrn_gtt_counters(&fixture.gtt)->observed == counters_before.observed;
        for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
            ok &= due_sweep.trace_slots[index] == index;
        }
        if (expiry_snapshot_query(&fixture.gtt, &demanded_subject, 25u,
                                  &demanded_after) != TAVRN_GTT_EXPIRY_QUERY_FOUND ||
            expiry_snapshot_query(&fixture.gtt, &undemanded_subject, 25u,
                                  &undemanded_after) != TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= demanded_after.freshness == TAVRN_GTT_FRESHNESS_HARD_EXPIRED &&
            demanded_after.application_requested != 0u &&
            undemanded_after.freshness == TAVRN_GTT_FRESHNESS_DEPARTED;
    }

    /* A departed candidate and its retention purge cross the uint32 clock
     * boundary without resetting the deterministic bounded cursor. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_sync_record_t record;
        tavrn_gtt_expiry_snapshot_t departed;
        tavrn_maintenance_expiry_sweep_snapshot_t pre;
        tavrn_maintenance_expiry_sweep_snapshot_t departure_sweep;
        tavrn_maintenance_expiry_sweep_snapshot_t purge_sweep;
        expiry_side_effect_state_t effects_before;
        expiry_side_effect_state_t effects_after;
        tavrn_gtt_counters_t counters_before;
        uint32_t start = UINT32_MAX - 20u;
        uint8_t index;

        if (!fixture_init(&fixture, start)) {
            return 0;
        }
        memset(&record, 0, sizeof(record));
        record.identity = subject;
        record.remaining_lifetime_ms = 1u;
        record.serial = 1u;
        record.serial_present = 1u;
        record.hop_count = 2u;
        if (phase5_gtt_sync_merge(&fixture.gtt, &record, 1u, start) !=
                TAVRN_GTT_SYNC_MERGE_COMMITTED ||
            phase5_maintenance_sweep(&fixture.maintenance, start, &pre) !=
                TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK ||
            pre.pass_performed != 0u || pre.next_due_ms != 4u ||
            !capture_side_effect_state(&fixture, &effects_before)) {
            return 0;
        }
        counters_before = *tavrn_gtt_counters(&fixture.gtt);
        if (phase5_maintenance_sweep(&fixture.maintenance, pre.next_due_ms,
                                     &departure_sweep) !=
                TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK ||
            !capture_side_effect_state(&fixture, &effects_after) ||
            departure_sweep.pass_performed == 0u ||
            departure_sweep.next_due_ms != 29u ||
            expiry_snapshot_query(&fixture.gtt, &subject, pre.next_due_ms,
                                  &departed) != TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= departed.freshness == TAVRN_GTT_FRESHNESS_DEPARTED &&
            departed.departed_deadline_ms == 604u &&
            departure_sweep.trace_count == 2u && departure_sweep.trace_slots[0] == 0u &&
            departure_sweep.trace_slots[1] == 1u && departure_sweep.next_cursor == 2u &&
            departure_sweep.scheduler_controls == 0u && departure_sweep.aodv_actions == 0u &&
            departure_sweep.router_broadcast_generation == 0u &&
            departure_sweep.mentorship_tc_generation == 0u &&
            departure_sweep.targeted_control_count == 0u &&
            departure_sweep.rreq_control_count == 0u &&
            departure_sweep.expiry_tc_control_count == 0u &&
            departure_sweep.received_evidence_count == 0u &&
            transport_side_effect_states_equal(&effects_before, &effects_after) &&
            tavrn_gtt_counters(&fixture.gtt)->departed == counters_before.departed + 1u &&
            tavrn_gtt_counters(&fixture.gtt)->observed == counters_before.observed;
        if (!capture_side_effect_state(&fixture, &effects_before)) {
            return 0;
        }
        counters_before = *tavrn_gtt_counters(&fixture.gtt);
        if (phase5_maintenance_sweep(&fixture.maintenance, 604u, &purge_sweep) !=
                TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK ||
            !capture_side_effect_state(&fixture, &effects_after)) {
            return 0;
        }
        ok &= purge_sweep.pass_performed != 0u && purge_sweep.purged_count >= 1u &&
            purge_sweep.next_due_ms == 629u && purge_sweep.trace_count == 1u &&
            purge_sweep.trace_slots[0] == 0u && purge_sweep.next_cursor == 1u &&
            purge_sweep.scheduler_controls == 0u && purge_sweep.aodv_actions == 0u &&
            purge_sweep.router_broadcast_generation == 0u &&
            purge_sweep.mentorship_tc_generation == 0u &&
            purge_sweep.targeted_control_count == 0u && purge_sweep.rreq_control_count == 0u &&
            purge_sweep.expiry_tc_control_count == 0u &&
            purge_sweep.received_evidence_count == 0u &&
            transport_side_effect_states_equal(&effects_before, &effects_after) &&
            tavrn_gtt_counters(&fixture.gtt)->departed == counters_before.departed &&
            tavrn_gtt_counters(&fixture.gtt)->observed == counters_before.observed;
        for (index = 0u; index < purge_sweep.trace_count; index++) {
            ok &= purge_sweep.trace_slots[index] < TAVRN_GTT_CAPACITY;
        }
    }
    return ok;
}

static int test_maint_02_direct_gate_contract(void)
{
    int ok = 1;

    /* The ordinary lifecycle supplies the interval-dominated direct deadline.
     * Candidate and demand are both copied from real future production APIs. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 1u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t output;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t demand;
        tavrn_maintenance_snapshot_t maintenance;
        expiry_gtt_state_t gtt_before;
        expiry_gtt_state_t gtt_after;
        expiry_side_effect_state_t effects_before;
        expiry_side_effect_state_t effects_after;
        uint32_t direct_deadline;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                              100u, TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 1u,
                              &before) ||
            tavrn_maintenance_snapshot(&fixture.maintenance, &maintenance) !=
                TAVRN_MAINTENANCE_OK ||
            !direct_deadline_from_maintenance(&maintenance, &before, &direct_deadline) ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms - 1u,
                                    &demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        ok &= direct_deadline == before.last_direct_evidence_ms +
            3u * maintenance.current_interval_ms &&
            deadline_precedes(direct_deadline, candidate.hard_deadline_ms);
        if (!capture_gtt_state(&fixture, candidate.hard_deadline_ms - 1u, &gtt_before) ||
            !capture_side_effect_state(&fixture, &effects_before)) {
            return 0;
        }
        memset(&output, 0xa5, sizeof(output));
        ok &= phase5_maintenance_checked_departure(&fixture.maintenance, &candidate,
                                                    candidate.hard_deadline_ms - 1u,
                                                    &output) ==
            TAVRN_GTT_CHECKED_NOT_HARD_EXPIRED &&
            expiry_snapshots_equal(&output, &before);
        if (!capture_gtt_state(&fixture, candidate.hard_deadline_ms - 1u, &gtt_after) ||
            !capture_side_effect_state(&fixture, &effects_after)) {
            return 0;
        }
        ok &= gtt_states_equal(&gtt_before, &gtt_after) &&
            side_effect_states_equal(&effects_before, &effects_after);
    }

    /* Public adaptive lifecycle: advance to stable, add a new direct peer, and
     * let the public topology sample reset interval/floor.  No owner state is
     * seeded by this test. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_c);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 1u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t output;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t demand;
        tavrn_maintenance_snapshot_t maintenance;
        uint32_t direct_deadline;
        uint32_t stable_at;
        int initialized;
        int stabilized;
        int observed;
        int snapshot_ok;
        int tick_ok;

        initialized = fixture_init_sid8(&fixture);
        stabilized = initialized != 0 && advance_maintenance_to_stable(&fixture, &stable_at);
        observed = stabilized != 0 && observe_expected(
            &fixture, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO, stable_at + 1u,
            TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 3u, 1u, &before);
        snapshot_ok = observed != 0 &&
            tavrn_maintenance_snapshot(&fixture.maintenance, &maintenance) ==
                TAVRN_MAINTENANCE_OK;
        tick_ok = snapshot_ok != 0 && tavrn_maintenance_tick(
            &fixture.maintenance, maintenance.next_topology_sample_ms) ==
                TAVRN_MAINTENANCE_OK;
        if (initialized == 0 || stabilized == 0 || observed == 0 ||
            snapshot_ok == 0 || tick_ok == 0) {
            return 0;
        }
        clear_queued_controls(&fixture);
        if (tavrn_maintenance_snapshot(&fixture.maintenance, &maintenance) !=
                TAVRN_MAINTENANCE_OK ||
            expiry_snapshot_query(&fixture.gtt, &subject,
                                  maintenance.next_topology_sample_ms, &before) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND ||
            !direct_deadline_from_maintenance(&maintenance, &before, &direct_deadline) ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                    &demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        ok &= maintenance.current_interval_ms == fixture.maintenance.config.hello_change_ms &&
            maintenance.liveness_floor_ms >
                3u * maintenance.current_interval_ms &&
            deadline_precedes(candidate.hard_deadline_ms, direct_deadline);
        ok &= checked_departure_rejection_is_immutable(
            &fixture, &candidate, candidate.hard_deadline_ms,
            TAVRN_GTT_CHECKED_DIRECT_DEADLINE_DEFERRED, &before) &&
            checked_departure_rejection_is_immutable(
                &fixture, &candidate, direct_deadline - 1u,
                TAVRN_GTT_CHECKED_DIRECT_DEADLINE_DEFERRED, &before);
        memset(&output, 0xa5, sizeof(output));
        ok &= phase5_maintenance_checked_departure(&fixture.maintenance, &candidate,
                                                    direct_deadline, &output) ==
                TAVRN_GTT_CHECKED_DEPARTED &&
            output.departed_deadline_ms == direct_deadline +
                fixture.gtt.config.departed_retention_ms;
    }

    /* Direct deadline arithmetic remains wrap-safe after the public adaptive
     * lifecycle has advanced the interval; no maintenance snapshot is edited. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_c);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 1u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t output;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t demand;
        tavrn_maintenance_snapshot_t maintenance;
        uint32_t direct_deadline;
        uint32_t observed_at = UINT32_MAX - 2000u;
        uint32_t stable_at;

        if (!fixture_init_sid8_at(&fixture, observed_at) ||
            !advance_maintenance_to_stable(&fixture, &stable_at) ||
            !observe_expected(&fixture, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                              stable_at + 1u, TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 3u, 1u,
                              &before)) {
            return 0;
        }
        if (tavrn_maintenance_snapshot(&fixture.maintenance, &maintenance) !=
                TAVRN_MAINTENANCE_OK ||
            !direct_deadline_from_maintenance(&maintenance, &before, &direct_deadline) ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                    &demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        ok &= direct_deadline < before.last_direct_evidence_ms;
        ok &= checked_departure_rejection_is_immutable(
            &fixture, &candidate, candidate.hard_deadline_ms,
            TAVRN_GTT_CHECKED_DIRECT_DEADLINE_DEFERRED, &before);
        ok &= checked_departure_rejection_is_immutable(
            &fixture, &candidate, direct_deadline - 1u,
            TAVRN_GTT_CHECKED_DIRECT_DEADLINE_DEFERRED, &before);
        memset(&output, 0xa5, sizeof(output));
        ok &= phase5_maintenance_checked_departure(&fixture.maintenance, &candidate,
                                                    direct_deadline, &output) ==
                TAVRN_GTT_CHECKED_DEPARTED;
    }

    /* Imported evidence has no direct deadline gate. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 2u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t output;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t demand;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input, TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                              100u, TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u,
                              &before) ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                    &demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        memset(&output, 0xa5, sizeof(output));
        ok &= phase5_maintenance_checked_departure(&fixture.maintenance, &candidate,
                                                    candidate.hard_deadline_ms,
                                                    &output) == TAVRN_GTT_CHECKED_DEPARTED &&
            output.direct == 0u;
    }

    /* A same-timestamp accepted direct RX wins before the copied candidate is
     * checked; the stale revision prevents a departure. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 1u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t output;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t demand;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                              100u, TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 1u,
                              &before) ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                    &demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        input.serial = 2u;
        if (!observe_expected(&fixture, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                              candidate.hard_deadline_ms,
                              TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 3u, 1u,
                              &output)) {
            return 0;
        }
        ok &= checked_departure_rejection_is_immutable(
            &fixture, &candidate, candidate.hard_deadline_ms,
            TAVRN_GTT_CHECKED_STALE_REVISION, &output);
    }

    /* Public configuration reaches the signed-half boundary: the largest
     * representable triple remains a direct defer, while the first value at
     * or above half fails closed without a GTT mutation. */
    {
        expiry_fixture_t fixture;
        tavrn_maintenance_config_t config;
        tavrn_adva_t subject = adva(adva_c);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 1u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t demand;
        tavrn_maintenance_snapshot_t maintenance;
        uint32_t direct_deadline;
        const uint32_t below_half = 0x7fffffffu / 3u;

        if (!fixture_init(&fixture, 0u)) {
            return 0;
        }
        config = fixture.maintenance.config;
        config.hello_change_ms = below_half;
        config.hello_stable_ms = below_half;
        if (!fixture_reconfigure_maintenance(&fixture, &config) ||
            !activate_sid8(&fixture, 0u) ||
            !observe_expected(&fixture, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                              100u, TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 3u, 1u,
                              &before) ||
            tavrn_maintenance_snapshot(&fixture.maintenance, &maintenance) !=
                TAVRN_MAINTENANCE_OK ||
            !direct_deadline_from_maintenance(&maintenance, &before, &direct_deadline) ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                    &demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        ok &= direct_deadline_from_maintenance(&maintenance, &before, &direct_deadline) &&
            checked_departure_rejection_is_immutable(
                &fixture, &candidate, candidate.hard_deadline_ms,
                TAVRN_GTT_CHECKED_DIRECT_DEADLINE_DEFERRED, &before);
    }
    {
        expiry_fixture_t fixture;
        tavrn_maintenance_config_t config;
        tavrn_adva_t subject = adva(adva_c);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 1u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t demand;
        const uint32_t below_half = 0x7fffffffu / 3u;

        if (!fixture_init(&fixture, 0u)) {
            return 0;
        }
        config = fixture.maintenance.config;
        config.hello_change_ms = below_half + 1u;
        config.hello_stable_ms = below_half + 1u;
        if (!fixture_reconfigure_maintenance(&fixture, &config) ||
            !activate_sid8(&fixture, 0u) ||
            !observe_expected(&fixture, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                              100u, TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 3u, 1u,
                              &before) ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                    &demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        ok &= checked_departure_rejection_is_immutable(
            &fixture, &candidate, candidate.hard_deadline_ms,
            TAVRN_GTT_CHECKED_INVALID, NULL);
    }
    return ok;
}

/* This intentionally composes the future owner boundaries around today's real
 * router, mentorship, local-broadcast, and activation calls.  It is not a
 * model of those calls: the future owner APIs alone are RED-backed. */
static int test_maint_04_split_owner_order_contract(void)
{
    tavrn_adva_t subject = adva(adva_b);
    tavrn_logical_id_t subject_sid8 = peer(adva_b, TAVRN_IDENTITY_SID8).logical_id;
    tavrn_direct_peer_t peer_c = peer(adva_c, TAVRN_IDENTITY_SID8);
    int ok = 1;

    /* Direct evidence at the exact old hard deadline is admitted before the
     * sweep, revises the copied entry, and prevents a departure.  This block
     * also spells out the real activation and local-broadcast steps which the
     * routed FULL binding keeps between the two owner boundaries. */
    {
        expiry_fixture_t fixture;
        tavrn_gtt_evidence_t imported = evidence(subject, 1u, 1u, 1u);
        tavrn_gtt_evidence_t direct = evidence(subject, 2u, 1u, 1u);
        tavrn_maintenance_owner_pre_tick_input_t pre_input;
        tavrn_maintenance_owner_pre_tick_result_t pre;
        tavrn_maintenance_owner_post_tick_result_t priming_post;
        tavrn_maintenance_owner_post_tick_result_t post;
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t refreshed;
        tavrn_router_local_broadcast_snapshot_t broadcast;
        const tavrn_maintenance_counters_t *counters;
        uint32_t due;

        if (!fixture_init(&fixture, 0u) ||
            tavrn_maintenance_activate(&fixture.maintenance, 0u) !=
                TAVRN_MAINTENANCE_GATED ||
            !observe_member(&fixture, adva_b, 0xfffeu, 1u) ||
            tavrn_mentorship_tick(&fixture.mentorship, 20u) !=
                TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED) {
            return 0;
        }
        clear_queued_controls(&fixture);
        if (tavrn_maintenance_activate(&fixture.maintenance, 20u) !=
                TAVRN_MAINTENANCE_OK ||
            !fixture_reconfigure_expiry(&fixture, 24u, 25u, 60u, 20u) ||
            tavrn_gtt_observe(&fixture.gtt, &imported, 20u) !=
                TAVRN_GTT_OBSERVE_ADDED) {
            return 0;
        }
        memset(&before, 0, sizeof(before));
        ok &= expiry_snapshot_query(&fixture.gtt, &subject, 20u, &before) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            before.hard_deadline_ms == 45u;

        /* Prime the armed cadence without doing a sweep; the first due owner
         * post-tick is exactly one topology interval after activation. */
        priming_post = phase5_maintenance_owner_post_tick(&fixture.maintenance, 20u);
        due = priming_post.sweep.next_due_ms;
        ok &= priming_post.sweep_status == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
            priming_post.maintenance_status == TAVRN_MAINTENANCE_OK &&
            priming_post.sweep.pass_performed == 0u && due == 45u &&
            priming_post.maintenance.armed != 0u;

        memset(&pre_input, 0, sizeof(pre_input));
        pre_input.evidence = direct;
        pre_input.provenance = TAVRN_GTT_PROVENANCE_DIRECT_OUTER_TRANSMITTER;
        pre_input.evidence_present = 1u;
        pre = phase5_maintenance_owner_pre_tick(&fixture.maintenance, pre_input, due);
        ok &= tavrn_router_tick(&fixture.router, due) == AODV_STATUS_OK &&
            tavrn_mentorship_tick(&fixture.mentorship, due) == TAVRN_MENTORSHIP_OK;
        memset(&broadcast, 0, sizeof(broadcast));
        ok &= note_local_broadcast_after_snapshot(&fixture.router, due, &broadcast) &&
            tavrn_maintenance_observe_local_broadcast(&fixture.maintenance, due) ==
                TAVRN_MAINTENANCE_OK;
        post = phase5_maintenance_owner_post_tick(&fixture.maintenance, due);
        memset(&refreshed, 0, sizeof(refreshed));
        counters = tavrn_maintenance_counters(&fixture.maintenance);
        ok &= pre.observe_present != 0u && pre.application_present == 0u &&
            pre.observe_status == TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED &&
            expiry_snapshot_query(&fixture.gtt, &subject, due, &refreshed) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            refreshed.revision != before.revision && refreshed.direct != 0u &&
            deadline_precedes(due, refreshed.hard_deadline_ms) &&
            post.sweep_status == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
            post.maintenance_status == TAVRN_MAINTENANCE_OK &&
            post.sweep.pass_performed != 0u && post.sweep.next_due_ms == due + 25u &&
            post.sweep.hard_selected_count == 0u && post.sweep.departed_count == 0u &&
            post.sweep.received_evidence_count == 1u && counters != NULL &&
            counters->local_broadcast_suppressed == 1u;
        if (post.sweep.trace_count != 0u) {
            ok &= post.sweep.next_cursor ==
                (uint8_t)((post.sweep.trace_slots[post.sweep.trace_count - 1u] + 1u) %
                          TAVRN_GTT_CAPACITY);
        }
    }

    /* A copied request wins at the same timestamp and causes the due sweep to
     * defer.  The later copied cancel must revise the same record before the
     * following due sweep restores departure eligibility. */
    {
        expiry_fixture_t fixture;
        tavrn_gtt_evidence_t imported = evidence(subject, 3u, 1u, 1u);
        tavrn_maintenance_owner_pre_tick_input_t pre_input;
        tavrn_maintenance_owner_pre_tick_result_t request_pre;
        tavrn_maintenance_owner_pre_tick_result_t cancel_pre;
        tavrn_maintenance_owner_post_tick_result_t priming_post;
        tavrn_maintenance_owner_post_tick_result_t request_post;
        tavrn_maintenance_owner_post_tick_result_t cancel_post;
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t requested;
        tavrn_gtt_expiry_snapshot_t canceled;
        tavrn_gtt_expiry_snapshot_t departed;
        tavrn_router_local_broadcast_snapshot_t broadcast;
        const tavrn_maintenance_counters_t *counters;
        uint32_t request_due;
        uint32_t cancel_due;

        if (!fixture_init_sid8(&fixture) ||
            !fixture_reconfigure_expiry(&fixture, 24u, 25u, 60u, 20u) ||
            tavrn_gtt_observe(&fixture.gtt, &imported, 20u) !=
                TAVRN_GTT_OBSERVE_ADDED) {
            return 0;
        }
        memset(&before, 0, sizeof(before));
        ok &= expiry_snapshot_query(&fixture.gtt, &subject, 20u, &before) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND && before.hard_deadline_ms == 45u;
        priming_post = phase5_maintenance_owner_post_tick(&fixture.maintenance, 20u);
        request_due = priming_post.sweep.next_due_ms;
        ok &= priming_post.sweep_status == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
            priming_post.sweep.pass_performed == 0u && request_due == 45u;

        memset(&pre_input, 0, sizeof(pre_input));
        pre_input.application_command.identity = subject;
        pre_input.application_command.kind = TAVRN_GTT_APPLICATION_COMMAND_REQUEST;
        pre_input.application_present = 1u;
        request_pre = phase5_maintenance_owner_pre_tick(&fixture.maintenance,
                                                         pre_input, request_due);
        ok &= tavrn_router_tick(&fixture.router, request_due) == AODV_STATUS_OK &&
            tavrn_mentorship_tick(&fixture.mentorship, request_due) ==
                TAVRN_MENTORSHIP_OK;
        memset(&broadcast, 0, sizeof(broadcast));
        ok &= note_local_broadcast_after_snapshot(&fixture.router, request_due, &broadcast) &&
            tavrn_maintenance_observe_local_broadcast(&fixture.maintenance,
                                                       request_due) == TAVRN_MAINTENANCE_OK;
        request_post = phase5_maintenance_owner_post_tick(&fixture.maintenance,
                                                           request_due);
        memset(&requested, 0, sizeof(requested));
        ok &= request_pre.observe_present == 0u && request_pre.application_present != 0u &&
            request_pre.application_result.status == TAVRN_GTT_APPLICATION_CHANGED &&
            expiry_snapshot_query(&fixture.gtt, &subject, request_due, &requested) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND && requested.application_requested != 0u &&
            requested.revision != before.revision &&
            request_post.sweep_status == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
            request_post.maintenance_status == TAVRN_MAINTENANCE_OK &&
            request_post.sweep.pass_performed != 0u &&
            request_post.sweep.next_due_ms == request_due + 25u &&
            request_post.sweep.demand_deferred_count == 1u &&
            request_post.sweep.departed_count == 0u;

        cancel_due = request_post.sweep.next_due_ms;
        pre_input.application_command.kind = TAVRN_GTT_APPLICATION_COMMAND_CANCEL;
        cancel_pre = phase5_maintenance_owner_pre_tick(&fixture.maintenance,
                                                        pre_input, cancel_due);
        ok &= tavrn_router_tick(&fixture.router, cancel_due) == AODV_STATUS_OK &&
            tavrn_mentorship_tick(&fixture.mentorship, cancel_due) ==
                TAVRN_MENTORSHIP_OK;
        memset(&broadcast, 0, sizeof(broadcast));
        ok &= note_local_broadcast_after_snapshot(&fixture.router, cancel_due, &broadcast) &&
            tavrn_maintenance_observe_local_broadcast(&fixture.maintenance,
                                                       cancel_due) == TAVRN_MAINTENANCE_OK;
        cancel_post = phase5_maintenance_owner_post_tick(&fixture.maintenance, cancel_due);
        memset(&canceled, 0, sizeof(canceled));
        memset(&departed, 0, sizeof(departed));
        counters = tavrn_maintenance_counters(&fixture.maintenance);
        ok &= cancel_pre.observe_present == 0u && cancel_pre.application_present != 0u &&
            cancel_pre.application_result.status == TAVRN_GTT_APPLICATION_CANCELED &&
            expiry_snapshot_query(&fixture.gtt, &subject, cancel_due, &canceled) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND && canceled.revision != requested.revision &&
            canceled.application_requested == 0u &&
            cancel_post.sweep_status == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
            cancel_post.maintenance_status == TAVRN_MAINTENANCE_OK &&
            cancel_post.sweep.pass_performed != 0u &&
            cancel_post.sweep.next_due_ms == cancel_due + 25u &&
            cancel_post.sweep.hard_selected_count >= 1u &&
            cancel_post.sweep.demand_deferred_count == 0u &&
            cancel_post.sweep.departed_count == 1u && counters != NULL &&
            counters->local_broadcast_suppressed == 2u &&
            expiry_snapshot_query(&fixture.gtt, &subject, cancel_due, &departed) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            departed.freshness == TAVRN_GTT_FRESHNESS_DEPARTED;
        if (cancel_post.sweep.trace_count != 0u) {
            ok &= cancel_post.sweep.next_cursor ==
                (uint8_t)((cancel_post.sweep.trace_slots[cancel_post.sweep.trace_count - 1u] +
                           1u) % TAVRN_GTT_CAPACITY);
        }
    }

    /* Main alone owns the capacity-one mailbox. Host RED calls the future
     * binding with the copied command and exact presence bit, never mailbox
     * storage; the binding owns only the phase order. */
    {
        expiry_fixture_t fixture;
        tavrn_gtt_evidence_t imported = evidence(subject, 0x6101u, 1u, 1u);
        tavrn_full_application_mailbox_t mailbox;
        tavrn_full_maintenance_binding_input_t binding_input;
        tavrn_full_maintenance_binding_result_t request_result;
        tavrn_full_maintenance_binding_result_t cancel_result;
        tavrn_gtt_application_command_t command;
        tavrn_gtt_application_command_t taken;
        tavrn_gtt_application_command_result_t command_result;
        tavrn_maintenance_owner_post_tick_result_t priming_post;
        tavrn_gtt_expiry_snapshot_t requested;
        tavrn_gtt_expiry_snapshot_t departed;
        tavrn_full_application_mailbox_status_t mailbox_init_status;
        tavrn_full_application_mailbox_status_t request_submit_status;
        tavrn_full_application_mailbox_status_t request_take_status;
        tavrn_full_application_mailbox_status_t request_publish_status;
        tavrn_full_application_mailbox_status_t cancel_submit_status;
        tavrn_full_application_mailbox_status_t cancel_take_status;
        tavrn_full_application_mailbox_status_t cancel_publish_status;
        tavrn_full_maintenance_binding_status_t request_binding_status;
        tavrn_full_maintenance_binding_status_t cancel_binding_status;
        uint32_t request_due;

        if (!fixture_init_sid8(&fixture) ||
            !fixture_reconfigure_expiry(&fixture, 24u, 25u, 60u, 20u) ||
            tavrn_gtt_observe(&fixture.gtt, &imported, 20u) !=
                TAVRN_GTT_OBSERVE_ADDED) {
            return 0;
        }
        priming_post = phase5_maintenance_owner_post_tick(&fixture.maintenance, 20u);
        request_due = priming_post.sweep.next_due_ms;
        memset(&mailbox, 0, sizeof(mailbox));
        memset(&command, 0, sizeof(command));
        command.identity = subject;
        command.kind = TAVRN_GTT_APPLICATION_COMMAND_REQUEST;
        mailbox_init_status = phase5_full_application_mailbox_init(&mailbox);
        request_submit_status = phase5_full_application_mailbox_submit(&mailbox, command);
        memset(&taken, 0, sizeof(taken));
        request_take_status = phase5_full_application_mailbox_owner_take(&mailbox, &taken);
        binding_input.router = &fixture.router;
        binding_input.mentorship = &fixture.mentorship;
        binding_input.maintenance = &fixture.maintenance;
        binding_input.application_command = taken;
        binding_input.application_present = 1u;
        request_binding_status = phase5_full_maintenance_binding_tick(
            binding_input, request_due, &request_result);
        request_publish_status = phase5_full_application_mailbox_owner_publish(
            &mailbox, request_result.application_result);
        taken.identity = generated_adva(0x6au);
        memset(&command_result, 0, sizeof(command_result));
        ok &= mailbox_init_status ==
                TAVRN_FULL_APPLICATION_MAILBOX_EMPTY &&
            request_submit_status == TAVRN_FULL_APPLICATION_MAILBOX_QUEUED &&
            request_take_status == TAVRN_FULL_APPLICATION_MAILBOX_PROCESSING &&
            request_binding_status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            request_publish_status == TAVRN_FULL_APPLICATION_MAILBOX_READY &&
            phase5_full_application_mailbox_consumer_take(&mailbox, &command_result) ==
                TAVRN_FULL_APPLICATION_MAILBOX_READY &&
            request_result.status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            request_result.pre_tick.observe_present == 0u &&
            request_result.pre_tick.application_present != 0u &&
            request_result.pre_tick.application_result.status == TAVRN_GTT_APPLICATION_CHANGED &&
            request_result.application_present == 1u &&
            request_result.application_result.status == TAVRN_GTT_APPLICATION_CHANGED &&
            adva_equal(&request_result.application_result.snapshot.identity, &subject) &&
            command_result.status == TAVRN_GTT_APPLICATION_CHANGED &&
            request_result.router_status == AODV_STATUS_OK &&
            request_result.router_trace.phase == TAVRN_ROUTER_TRACE_TICK &&
            request_result.router_trace.detail.tick.status == request_result.router_status &&
            request_result.mentorship_status == TAVRN_MENTORSHIP_OK &&
            request_result.broadcast_snapshot_status == TAVRN_ROUTER_LOCAL_BROADCAST_OK &&
            request_result.broadcast_observation_status == TAVRN_MAINTENANCE_OK &&
            request_result.activation_status == TAVRN_MAINTENANCE_OK &&
            request_result.post_tick.sweep_status == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
            request_result.post_tick.maintenance_status == TAVRN_MAINTENANCE_OK &&
            request_result.post_tick.sweep.demand_deferred_count == 1u &&
            request_result.post_tick.sweep.departed_count == 0u &&
            expiry_snapshot_query(&fixture.gtt, &subject, request_due, &requested) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND && requested.application_requested != 0u;

        command.kind = TAVRN_GTT_APPLICATION_COMMAND_CANCEL;
        cancel_submit_status = phase5_full_application_mailbox_submit(&mailbox, command);
        memset(&taken, 0, sizeof(taken));
        cancel_take_status = phase5_full_application_mailbox_owner_take(&mailbox, &taken);
        binding_input.application_command = taken;
        binding_input.application_present = 1u;
        cancel_binding_status = phase5_full_maintenance_binding_tick(
            binding_input, request_result.post_tick.sweep.next_due_ms, &cancel_result);
        cancel_publish_status = phase5_full_application_mailbox_owner_publish(
            &mailbox, cancel_result.application_result);
        memset(&command_result, 0, sizeof(command_result));
        memset(&departed, 0, sizeof(departed));
        ok &= cancel_submit_status == TAVRN_FULL_APPLICATION_MAILBOX_QUEUED &&
            cancel_take_status == TAVRN_FULL_APPLICATION_MAILBOX_PROCESSING &&
            cancel_binding_status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            cancel_publish_status == TAVRN_FULL_APPLICATION_MAILBOX_READY &&
            phase5_full_application_mailbox_consumer_take(&mailbox, &command_result) ==
                TAVRN_FULL_APPLICATION_MAILBOX_READY &&
            cancel_result.status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            cancel_result.pre_tick.observe_present == 0u &&
            cancel_result.pre_tick.application_present != 0u &&
            cancel_result.pre_tick.application_result.status == TAVRN_GTT_APPLICATION_CANCELED &&
            cancel_result.application_present == 1u &&
            cancel_result.application_result.status == TAVRN_GTT_APPLICATION_CANCELED &&
            command_result.status == TAVRN_GTT_APPLICATION_CANCELED &&
            cancel_result.router_status == AODV_STATUS_OK &&
            cancel_result.router_trace.phase == TAVRN_ROUTER_TRACE_TICK &&
            cancel_result.mentorship_status == TAVRN_MENTORSHIP_OK &&
            cancel_result.post_tick.sweep_status == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
            cancel_result.post_tick.sweep.demand_deferred_count == 0u &&
            cancel_result.post_tick.sweep.departed_count == 1u &&
            expiry_snapshot_query(&fixture.gtt, &subject,
                                  cancel_result.post_tick.sweep.next_due_ms - 25u,
                                  &departed) == TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            departed.freshness == TAVRN_GTT_FRESHNESS_DEPARTED &&
            full_binding_source_is_closed() && full_binding_profile_source_is_closed() &&
            future_resource_gate_is_closed() && expiry_green_acceptance_is_wired();
    }

    {
        expiry_fixture_t fixture;
        tavrn_full_maintenance_binding_input_t binding_input;
        tavrn_full_maintenance_binding_result_t invalid_result;
        tavrn_full_maintenance_binding_result_t absent_result;
        tavrn_full_maintenance_binding_result_t malformed_result;
        tavrn_full_maintenance_binding_result_t null_result;
        tavrn_full_maintenance_binding_result_t null_before;
        tavrn_full_maintenance_binding_status_t binding_status;
        expiry_side_effect_state_t before;
        expiry_side_effect_state_t after;

        if (!fixture_init_sid8(&fixture)) {
            return 0;
        }
        memset(&binding_input, 0, sizeof(binding_input));
        binding_input.router = &fixture.router;
        binding_input.mentorship = &fixture.mentorship;
        binding_input.maintenance = &fixture.maintenance;
        binding_input.application_present = 2u;
        if (!capture_side_effect_state(&fixture, &before)) {
            return 0;
        }
        binding_status = phase5_full_maintenance_binding_tick(binding_input, 20u,
                                                               &invalid_result);
        if (!capture_side_effect_state(&fixture, &after)) {
            return 0;
        }
        ok &= binding_status == TAVRN_FULL_MAINTENANCE_BINDING_INVALID &&
            invalid_result.status == TAVRN_FULL_MAINTENANCE_BINDING_INVALID &&
            invalid_result.application_present == 0u &&
            memcmp(&before, &after, sizeof(before)) == 0;

        /* An absent command ignores even malformed copied bytes. */
        binding_input.application_command.kind =
            (tavrn_gtt_application_command_kind_t)0xffu;
        binding_input.application_present = 0u;
        binding_status = phase5_full_maintenance_binding_tick(binding_input, 21u,
                                                               &absent_result);
        ok &= binding_status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            absent_result.status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            absent_result.application_present == 0u &&
            absent_result.pre_tick.application_present == 0u;

        binding_input.application_present = 1u;
        binding_status = phase5_full_maintenance_binding_tick(binding_input, 22u,
                                                               &malformed_result);
        ok &= binding_status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            malformed_result.status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            malformed_result.application_present == 1u &&
            malformed_result.pre_tick.application_present != 0u &&
            malformed_result.application_result.status == TAVRN_GTT_APPLICATION_INVALID;

        memset(&null_result, 0xa5, sizeof(null_result));
        null_before = null_result;
        ok &= phase5_full_maintenance_binding_tick(binding_input, 23u, NULL) ==
                TAVRN_FULL_MAINTENANCE_BINDING_INVALID &&
            memcmp(&null_result, &null_before, sizeof(null_result)) == 0;
    }

    {
        expiry_fixture_t fixture;
        tavrn_gtt_evidence_t imported = evidence(subject, 0x6102u, 1u, 1u);
        tavrn_full_maintenance_binding_input_t binding_input;
        tavrn_full_maintenance_binding_result_t result;
        tavrn_full_maintenance_binding_result_t repeated;
        tavrn_maintenance_owner_post_tick_result_t priming_post;
        tavrn_gtt_expiry_snapshot_t departed;
        tavrn_direct_peer_t next_hop = peer(adva_c, TAVRN_IDENTITY_SID8);
        tavrn_logical_id_t subject_sid8 = peer(adva_b, TAVRN_IDENTITY_SID8).logical_id;
        aodv_route_snapshot_t route;
        tavrn_maintenance_counters_t counters_after_result;
        tavrn_full_maintenance_binding_status_t result_status;
        tavrn_full_maintenance_binding_status_t repeated_status;
        uint32_t due;

        /* Binding's real router _ex tick invalidates an equality-expired route
         * before its post sweep; repeating that exact due time cannot run a
         * second cadence pass or depart a second time. */
        if (!fixture_init_sid8(&fixture)) {
            return 0;
        }
        if (!fixture_reconfigure_expiry(&fixture, 24u, 25u, 60u, 20u)) {
            return 0;
        }
        if (tavrn_gtt_observe(&fixture.gtt, &imported, 20u) !=
            TAVRN_GTT_OBSERVE_ADDED) {
            return 0;
        }
        if (!install_route_via_lifetime(&fixture, subject_sid8, &next_hop, 20u, 24u,
                                        &route)) {
            return 0;
        }
        priming_post = phase5_maintenance_owner_post_tick(&fixture.maintenance, 20u);
        due = priming_post.sweep.next_due_ms;
        memset(&binding_input, 0, sizeof(binding_input));
        binding_input.router = &fixture.router;
        binding_input.mentorship = &fixture.mentorship;
        binding_input.maintenance = &fixture.maintenance;
        binding_input.application_present = 0u;
        result_status = phase5_full_maintenance_binding_tick(binding_input, due, &result);
        counters_after_result = *tavrn_maintenance_counters(&fixture.maintenance);
        repeated_status = phase5_full_maintenance_binding_tick(binding_input, due,
                                                                 &repeated);
        memset(&departed, 0, sizeof(departed));
        ok &= due == route.expires_at_ms &&
            result_status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            result.status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            result.application_present == 0u &&
            result.pre_tick.application_present == 0u &&
            result.router_status == AODV_STATUS_OK &&
            result.broadcast_snapshot_status == TAVRN_ROUTER_LOCAL_BROADCAST_OK &&
            result.broadcast.generation != 0u &&
            fixture.maintenance.local_broadcast_generation_seen == result.broadcast.generation &&
            aodv_core_route_snapshot(&fixture.aodv, &subject_sid8, &route) ==
                AODV_ROUTE_QUERY_FOUND &&
            route.state == AODV_ROUTE_INVALID &&
            result.post_tick.sweep.pass_performed != 0u &&
            result.post_tick.sweep.demand_deferred_count == 0u &&
            result.post_tick.sweep.departed_count == 1u &&
            repeated_status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            repeated.status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            repeated.post_tick.sweep.pass_performed == 0u &&
            repeated.post_tick.sweep.next_cursor == result.post_tick.sweep.next_cursor &&
            repeated.post_tick.sweep.departed_count == 0u &&
            repeated.broadcast_observation_status == TAVRN_MAINTENANCE_OK &&
            tavrn_maintenance_counters(&fixture.maintenance)->local_broadcast_suppressed ==
                counters_after_result.local_broadcast_suppressed &&
            expiry_snapshot_query(&fixture.gtt, &subject, due, &departed) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            departed.freshness == TAVRN_GTT_FRESHNESS_DEPARTED;
    }

    {
        expiry_fixture_t fixture;
        tavrn_gtt_evidence_t bootstrap_peer = evidence(subject, 0x6103u, 1u, 1u);
        tavrn_full_maintenance_binding_input_t binding_input;
        tavrn_full_maintenance_binding_result_t activation_result;
        tavrn_full_maintenance_binding_status_t activation_status;

        /* Pre-SID8, the binding's mentorship tick and activation occur before
         * post processing.  The first eligible call arms but performs no sweep. */
        if (!fixture_init(&fixture, 0u) ||
            tavrn_gtt_observe(&fixture.gtt, &bootstrap_peer, 1u) !=
                TAVRN_GTT_OBSERVE_ADDED) {
            return 0;
        }
        memset(&binding_input, 0, sizeof(binding_input));
        binding_input.router = &fixture.router;
        binding_input.mentorship = &fixture.mentorship;
        binding_input.maintenance = &fixture.maintenance;
        activation_status = phase5_full_maintenance_binding_tick(binding_input, 20u,
                                                                   &activation_result);
        ok &= activation_status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            activation_result.status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            activation_result.mentorship_status == TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED &&
            activation_result.activation_status == TAVRN_MAINTENANCE_OK &&
            activation_result.post_tick.maintenance.armed != 0u &&
            activation_result.post_tick.sweep.pass_performed == 0u;
    }

    {
        expiry_fixture_t fixture;
        tavrn_maintenance_config_t config;
        tavrn_gtt_evidence_t direct = evidence(subject, 0u, 0u, 1u);
        tavrn_gtt_expiry_snapshot_t active;
        tavrn_gtt_expiry_snapshot_t departed;
        tavrn_full_maintenance_binding_input_t binding_input;
        tavrn_full_maintenance_binding_result_t first;
        tavrn_full_maintenance_binding_result_t middle;
        tavrn_full_maintenance_binding_result_t expiry;
        tavrn_maintenance_snapshot_t before_topology;
        tavrn_maintenance_counters_t counters_before;
        const tavrn_maintenance_counters_t *counters_after;
        tavrn_full_maintenance_binding_status_t first_status;
        tavrn_full_maintenance_binding_status_t middle_status;
        tavrn_full_maintenance_binding_status_t expiry_status;
        uint32_t first_at;
        uint32_t middle_at;
        uint32_t expiry_at;

        /* Direct evidence is committed before the boundary. At hard equality,
         * this binding's sweep departs first and its following topology work
         * observes the direct-count loss/reset. */
        if (!fixture_init_sid8(&fixture) ||
            !fixture_reconfigure_expiry(&fixture, 50u, 75u, 60u, 20u)) {
            return 0;
        }
        config = fixture.maintenance.config;
        config.hello_change_ms = 1u;
        config.hello_stable_ms = 1u;
        config.topology_sample_ms = 25u;
        if (!fixture_reconfigure_maintenance(&fixture, &config) ||
            tavrn_maintenance_activate(&fixture.maintenance, 20u) !=
                TAVRN_MAINTENANCE_OK ||
            !observe_expected(&fixture, &direct,
                              TAVRN_GTT_PROVENANCE_DIRECT_OUTER_TRANSMITTER, 20u,
                              TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 1u, &active)) {
            return 0;
        }
        expiry_at = active.hard_deadline_ms;
        first_at = expiry_at - 50u;
        middle_at = expiry_at - 25u;
        memset(&binding_input, 0, sizeof(binding_input));
        binding_input.router = &fixture.router;
        binding_input.mentorship = &fixture.mentorship;
        binding_input.maintenance = &fixture.maintenance;
        first_status = phase5_full_maintenance_binding_tick(binding_input, first_at,
                                                             &first);
        middle_status = phase5_full_maintenance_binding_tick(binding_input, middle_at,
                                                              &middle);
        if (tavrn_maintenance_snapshot(&fixture.maintenance, &before_topology) !=
                TAVRN_MAINTENANCE_OK ||
            tavrn_maintenance_counters(&fixture.maintenance) == NULL) {
            return 0;
        }
        counters_before = *tavrn_maintenance_counters(&fixture.maintenance);
        expiry_status = phase5_full_maintenance_binding_tick(binding_input, expiry_at,
                                                              &expiry);
        counters_after = tavrn_maintenance_counters(&fixture.maintenance);
        memset(&departed, 0, sizeof(departed));
        ok &= first_status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            middle_status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            expiry_status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            first.status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            first.post_tick.sweep.pass_performed == 0u &&
            first.post_tick.sweep.next_due_ms == middle_at &&
            middle.status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            middle.post_tick.sweep.pass_performed != 0u &&
            middle.post_tick.sweep.next_due_ms == expiry_at &&
            before_topology.direct_one_hop_count == 1u &&
            expiry.status == TAVRN_FULL_MAINTENANCE_BINDING_OK &&
            expiry.post_tick.sweep.departed_count == 1u &&
            expiry.post_tick.maintenance.direct_one_hop_count == 0u &&
            expiry.post_tick.maintenance.current_interval_ms == config.hello_change_ms &&
            counters_after != NULL &&
            counters_after->topology_reset == counters_before.topology_reset + 1u &&
            expiry_snapshot_query(&fixture.gtt, &subject, expiry_at, &departed) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            departed.freshness == TAVRN_GTT_FRESHNESS_DEPARTED;
    }

    /* The ordinary router tick runs before the post-tick sweep.  Equality is
     * significant: the route expires at this timestamp, so it cannot keep the
     * hard-expired subject demanded when post-tick evaluates it. */
    {
        expiry_fixture_t fixture;
        tavrn_gtt_evidence_t imported = evidence(subject, 4u, 1u, 1u);
        tavrn_maintenance_owner_pre_tick_input_t pre_input;
        tavrn_maintenance_owner_post_tick_result_t priming_post;
        tavrn_maintenance_owner_post_tick_result_t post;
        tavrn_gtt_expiry_snapshot_t departed;
        tavrn_router_local_broadcast_snapshot_t broadcast;
        aodv_route_snapshot_t route;
        uint32_t due;

        if (!fixture_init_sid8(&fixture) ||
            !fixture_reconfigure_expiry(&fixture, 24u, 25u, 60u, 20u) ||
            tavrn_gtt_observe(&fixture.gtt, &imported, 20u) !=
                TAVRN_GTT_OBSERVE_ADDED ||
            !install_route_via_lifetime(&fixture, subject_sid8, &peer_c, 20u, 24u,
                                        &route)) {
            return 0;
        }
        priming_post = phase5_maintenance_owner_post_tick(&fixture.maintenance, 20u);
        due = priming_post.sweep.next_due_ms;
        ok &= priming_post.sweep_status == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
            priming_post.sweep.pass_performed == 0u && due == route.expires_at_ms &&
            due == 45u;

        memset(&pre_input, 0, sizeof(pre_input));
        (void)phase5_maintenance_owner_pre_tick(&fixture.maintenance, pre_input, due);
        ok &= tavrn_router_tick(&fixture.router, due) == AODV_STATUS_OK &&
            aodv_core_route_snapshot(&fixture.aodv, &subject_sid8, &route) ==
                AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_INVALID &&
            tavrn_mentorship_tick(&fixture.mentorship, due) == TAVRN_MENTORSHIP_OK;
        memset(&broadcast, 0, sizeof(broadcast));
        ok &= note_local_broadcast_after_snapshot(&fixture.router, due, &broadcast) &&
            tavrn_maintenance_observe_local_broadcast(&fixture.maintenance, due) ==
                TAVRN_MAINTENANCE_OK;
        post = phase5_maintenance_owner_post_tick(&fixture.maintenance, due);
        memset(&departed, 0, sizeof(departed));
        ok &= post.sweep_status == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
            post.maintenance_status == TAVRN_MAINTENANCE_OK &&
            post.sweep.pass_performed != 0u && post.sweep.next_due_ms == due + 25u &&
            post.sweep.hard_selected_count >= 1u &&
            post.sweep.demand_deferred_count == 0u && post.sweep.departed_count == 1u &&
            expiry_snapshot_query(&fixture.gtt, &subject, due, &departed) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            departed.freshness == TAVRN_GTT_FRESHNESS_DEPARTED;
    }

    /* Sweep is first inside an armed owner post-tick.  The middle cadence
     * samples a live direct peer; the hard-expiry cadence must depart it before
     * topology samples again, so public maintenance observes the loss and
     * resets its cadence rather than retaining the old direct count. */
    {
        expiry_fixture_t fixture;
        tavrn_maintenance_config_t config;
        tavrn_gtt_evidence_t direct = evidence(subject, 0u, 0u, 1u);
        tavrn_maintenance_owner_pre_tick_input_t pre_input;
        tavrn_maintenance_owner_pre_tick_result_t pre;
        tavrn_maintenance_owner_post_tick_result_t first_post;
        tavrn_maintenance_owner_post_tick_result_t middle_post;
        tavrn_maintenance_owner_post_tick_result_t expiry_post;
        tavrn_gtt_expiry_snapshot_t active;
        tavrn_gtt_expiry_snapshot_t departed;
        tavrn_maintenance_snapshot_t before_topology;
        tavrn_maintenance_snapshot_t after_topology;
        tavrn_maintenance_counters_t counters_before;
        const tavrn_maintenance_counters_t *counters_after;
        uint32_t first_at;
        uint32_t middle_at;
        uint32_t expiry_at;

        if (!fixture_init_sid8(&fixture) ||
            !fixture_reconfigure_expiry(&fixture, 50u, 75u, 60u, 20u)) {
            return 0;
        }
        config = fixture.maintenance.config;
        config.hello_change_ms = 1u;
        config.hello_stable_ms = 1u;
        config.topology_sample_ms = 25u;
        if (!fixture_reconfigure_maintenance(&fixture, &config) ||
            tavrn_maintenance_activate(&fixture.maintenance, 20u) !=
                TAVRN_MAINTENANCE_OK) {
            return 0;
        }
        memset(&pre_input, 0, sizeof(pre_input));
        pre_input.evidence = direct;
        pre_input.provenance = TAVRN_GTT_PROVENANCE_DIRECT_OUTER_TRANSMITTER;
        pre_input.evidence_present = 1u;
        pre = phase5_maintenance_owner_pre_tick(&fixture.maintenance, pre_input, 20u);
        if (expiry_snapshot_query(&fixture.gtt, &subject, 20u, &active) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND ||
            !deadline_precedes(20u, active.hard_deadline_ms)) {
            return 0;
        }
        expiry_at = active.hard_deadline_ms;
        first_at = expiry_at - 50u;
        middle_at = expiry_at - 25u;
        first_post = phase5_maintenance_owner_post_tick(&fixture.maintenance, first_at);
        middle_post = phase5_maintenance_owner_post_tick(&fixture.maintenance, middle_at);
        if (tavrn_maintenance_snapshot(&fixture.maintenance, &before_topology) !=
                TAVRN_MAINTENANCE_OK ||
            tavrn_maintenance_counters(&fixture.maintenance) == NULL) {
            return 0;
        }
        counters_before = *tavrn_maintenance_counters(&fixture.maintenance);
        expiry_post = phase5_maintenance_owner_post_tick(&fixture.maintenance, expiry_at);
        counters_after = tavrn_maintenance_counters(&fixture.maintenance);
        memset(&departed, 0, sizeof(departed));
        ok &= pre.observe_present != 0u && pre.application_present == 0u &&
            pre.observe_status == TAVRN_GTT_EXPIRY_OBSERVE_ADDED &&
            active.direct != 0u && active.serial_present == 0u &&
            first_post.sweep_status == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
            first_post.sweep.pass_performed == 0u &&
            first_post.sweep.next_due_ms == middle_at &&
            middle_post.sweep_status == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
            middle_post.sweep.pass_performed != 0u &&
            middle_post.sweep.next_due_ms == expiry_at &&
            before_topology.armed != 0u && before_topology.direct_one_hop_count == 1u &&
            expiry_post.sweep_status == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
            expiry_post.maintenance_status == TAVRN_MAINTENANCE_OK &&
            expiry_post.sweep.pass_performed != 0u &&
            expiry_post.sweep.departed_count == 1u &&
            expiry_post.sweep.demand_deferred_count == 0u &&
            expiry_post.sweep.next_due_ms == expiry_at + 25u &&
            expiry_post.maintenance.direct_one_hop_count == 0u &&
            expiry_post.maintenance.current_interval_ms == config.hello_change_ms &&
            counters_after != NULL &&
            counters_after->topology_reset == counters_before.topology_reset + 1u &&
            tavrn_maintenance_snapshot(&fixture.maintenance, &after_topology) ==
                TAVRN_MAINTENANCE_OK &&
            after_topology.direct_one_hop_count == 0u &&
            expiry_snapshot_query(&fixture.gtt, &subject, expiry_at, &departed) ==
                TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            departed.freshness == TAVRN_GTT_FRESHNESS_DEPARTED;
    }

    /* Before SID8 activation there is no armed expiry owner.  The post-tick
     * result is copied but must report no pass and leave GTT/maintenance state
     * untouched even though the pre/router/mentorship boundaries ran. */
    {
        expiry_fixture_t fixture;
        tavrn_gtt_evidence_t imported = evidence(subject, 5u, 1u, 1u);
        tavrn_maintenance_owner_pre_tick_input_t pre_input;
        tavrn_maintenance_owner_pre_tick_result_t pre;
        tavrn_maintenance_owner_post_tick_result_t post;
        tavrn_gtt_t gtt_before;
        tavrn_gtt_storage_t storage_before;
        tavrn_gtt_counters_t counters_before;
        tavrn_maintenance_snapshot_t maintenance_before;
        tavrn_maintenance_counters_t maintenance_counters_before;
        const tavrn_gtt_counters_t *gtt_counters;
        const tavrn_maintenance_counters_t *maintenance_counters;

        if (!fixture_init(&fixture, 0u) ||
            !fixture_reconfigure_expiry(&fixture, 24u, 25u, 60u, 0u) ||
            tavrn_gtt_observe(&fixture.gtt, &imported, 0u) != TAVRN_GTT_OBSERVE_ADDED ||
            tavrn_maintenance_snapshot(&fixture.maintenance, &maintenance_before) !=
                TAVRN_MAINTENANCE_OK) {
            return 0;
        }
        gtt_counters = tavrn_gtt_counters(&fixture.gtt);
        maintenance_counters = tavrn_maintenance_counters(&fixture.maintenance);
        if (gtt_counters == NULL || maintenance_counters == NULL) {
            return 0;
        }
        gtt_before = fixture.gtt;
        storage_before = fixture.gtt_storage;
        counters_before = *gtt_counters;
        maintenance_counters_before = *maintenance_counters;
        memset(&pre_input, 0, sizeof(pre_input));
        pre = phase5_maintenance_owner_pre_tick(&fixture.maintenance, pre_input, 15u);
        {
            aodv_status_t router_status = tavrn_router_tick(&fixture.router, 15u);
            tavrn_mentorship_status_t mentorship_status =
                tavrn_mentorship_tick(&fixture.mentorship, 15u);

            ok &= router_status == AODV_STATUS_REJOINING &&
                mentorship_status == TAVRN_MENTORSHIP_OK;
        }
        post = phase5_maintenance_owner_post_tick(&fixture.maintenance, 15u);
        gtt_counters = tavrn_gtt_counters(&fixture.gtt);
        maintenance_counters = tavrn_maintenance_counters(&fixture.maintenance);
        /* Presence bits make all absent status/snapshot payload fields ignored. */
        ok &= pre.observe_present == 0u && pre.application_present == 0u &&
            post.sweep_status == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
            post.maintenance_status == TAVRN_MAINTENANCE_GATED &&
            post.sweep.pass_performed == 0u && post.sweep.trace_count == 0u &&
            post.sweep.hard_selected_count == 0u && post.sweep.demand_deferred_count == 0u &&
            post.sweep.departed_count == 0u && post.maintenance.armed == 0u &&
            memcmp(&fixture.gtt, &gtt_before, sizeof(gtt_before)) == 0 &&
            memcmp(&fixture.gtt_storage, &storage_before, sizeof(storage_before)) == 0 &&
            gtt_counters != NULL &&
            memcmp(gtt_counters, &counters_before, sizeof(counters_before)) == 0 &&
            maintenance_counters != NULL &&
            memcmp(maintenance_counters, &maintenance_counters_before,
                   sizeof(maintenance_counters_before)) == 0;
    }
    return ok;
}

static int test_maint_04_real_owner_contract(void)
{
    tavrn_adva_t subject = adva(adva_b);
    tavrn_logical_id_t subject_sid8 = peer(adva_b, TAVRN_IDENTITY_SID8).logical_id;
    tavrn_logical_id_t other_sid8 = peer(adva_c, TAVRN_IDENTITY_SID8).logical_id;
    tavrn_direct_peer_t peer_b = peer(adva_b, TAVRN_IDENTITY_SID8);
    tavrn_direct_peer_t peer_c = peer(adva_c, TAVRN_IDENTITY_SID8);
    tavrn_direct_peer_t peer_d = peer(adva_d, TAVRN_IDENTITY_SID8);
    const uint32_t now_ms = 100u;
    int ok = 1;

    /* Every owner gets a newly initialized, real SID8 fixture. */
    {
        expiry_fixture_t fixture;
        tron_application_data_t input = application(subject_sid8);

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            ok &= aodv_core_submit_application(&fixture.aodv, &input, now_ms) ==
                AODV_STATUS_QUEUED;
            ok &= aodv_demand_matches(&fixture, &subject_sid8, &subject, now_ms,
                                       TAVRN_MAINT_DEMAND_PENDING_DATA, 0u);
            ok &= maintenance_demand_matches(&fixture, &subject, now_ms,
                                              TAVRN_MAINT_DEMAND_PENDING_DATA, 0u);
        }
    }
    {
        expiry_fixture_t fixture;
        tron_application_data_t input = application(subject_sid8);
        aodv_route_snapshot_t route;

        if (!fixture_init_sid8(&fixture) ||
            !install_route_via(&fixture, subject_sid8, &peer_c, now_ms, &route)) {
            ok = 0;
        } else {
            ok &= aodv_core_submit_application(&fixture.aodv, &input, now_ms + 2u) ==
                AODV_STATUS_OK;
            ok &= aodv_demand_matches(&fixture, &subject_sid8, &subject, route.expires_at_ms,
                                       TAVRN_MAINT_DEMAND_QUEUED_DATA, 0u);
            ok &= maintenance_demand_matches(&fixture, &subject, route.expires_at_ms,
                                              TAVRN_MAINT_DEMAND_QUEUED_DATA, 0u);
        }
    }
    {
        expiry_fixture_t fixture;

        if (!fixture_init_sid8(&fixture) ||
            !install_route_via(&fixture, subject_sid8, &peer_c, now_ms, NULL)) {
            ok = 0;
        } else {
            ok &= aodv_demand_matches(&fixture, &subject_sid8, &subject, now_ms + 2u,
                                       TAVRN_MAINT_DEMAND_VALID_ROUTE_TO_SUBJECT, 1u);
            ok &= maintenance_demand_matches(
                &fixture, &subject, now_ms + 2u,
                TAVRN_MAINT_DEMAND_VALID_ROUTE_TO_SUBJECT, 1u);
        }
    }
    {
        expiry_fixture_t fixture;

        if (!fixture_init_sid8(&fixture) ||
            !install_route_via(&fixture, other_sid8, &peer_b, now_ms, NULL)) {
            ok = 0;
        } else {
            ok &= aodv_demand_matches(&fixture, &subject_sid8, &subject, now_ms + 2u,
                                       TAVRN_MAINT_DEMAND_VALID_ROUTE_VIA_SUBJECT, 0u);
            ok &= maintenance_demand_matches(
                &fixture, &subject, now_ms + 2u,
                                               TAVRN_MAINT_DEMAND_VALID_ROUTE_VIA_SUBJECT, 0u);
        }
    }
    {
        expiry_fixture_t fixture;
        tavrn_direct_peer_t sid8_alias = peer(adva_b_alias, TAVRN_IDENTITY_SID8);

        /* SID8 is a logical subject selector only.  A route whose next hop
         * merely collides with B's SID8 must not create an AdvA-specific
         * VALID_ROUTE_VIA_SUBJECT demand for B. */
        sid8_alias.logical_id = subject_sid8;
        if (!fixture_init_sid8(&fixture) ||
            !install_route_via(&fixture, other_sid8, &sid8_alias, now_ms, NULL)) {
            ok = 0;
        } else {
            ok &= aodv_demand_matches(&fixture, &subject_sid8, &subject, now_ms + 2u,
                                       0u, 0u);
            ok &= maintenance_demand_matches(
                &fixture, &subject, now_ms + 2u, 0u, 0u);
        }
    }
    {
        expiry_fixture_t fixture;

        if (!fixture_init_sid8(&fixture) ||
            !seed_precursor(&fixture, &peer_b, now_ms, NULL)) {
            ok = 0;
        } else {
            ok &= aodv_demand_matches(&fixture, &subject_sid8, &subject, now_ms + 2u,
                                       TAVRN_MAINT_DEMAND_PRECURSOR, 0u);
            ok &= maintenance_demand_matches(&fixture, &subject, now_ms + 2u,
                                              TAVRN_MAINT_DEMAND_PRECURSOR, 0u);
        }
    }
    {
        expiry_fixture_t fixture;

        if (!fixture_init_sid8(&fixture) ||
            !install_route_via(&fixture, subject_sid8, &peer_c, now_ms, NULL)) {
            ok = 0;
        } else {
            ok &= aodv_core_report_link_failure(
                &fixture.aodv, &peer_c, &subject_sid8,
                AODV_LINK_FAILURE_DEFER_REPAIR_DESTINATION_RERR, now_ms + 2u) ==
                AODV_FAILURE_OK;
            ok &= aodv_demand_matches(&fixture, &subject_sid8, &subject, now_ms + 2u,
                                       TAVRN_MAINT_DEMAND_DEFERRED_REPAIR, 0u);
            ok &= maintenance_demand_matches(&fixture, &subject, now_ms + 2u,
                                              TAVRN_MAINT_DEMAND_DEFERRED_REPAIR, 0u);
        }
    }
    {
        expiry_fixture_t fixture;
        tavrn_link_data_t data;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            data = link_data(fixture.link.config.local_peer.logical_id,
                             subject_sid8, 0x7301u);
            ok &= tavrn_link_v2_send_unicast(&fixture.link, &peer_c, &data, now_ms,
                                              &(tavrn_link_event_t){0}) == TAVRN_LINK_SEND_OK;
            ok &= link_demand_matches(&fixture, &subject_sid8, &subject, now_ms,
                                       TAVRN_MAINT_DEMAND_CUSTODY_FINAL);
            ok &= maintenance_demand_matches(&fixture, &subject, now_ms,
                                              TAVRN_MAINT_DEMAND_CUSTODY_FINAL, 0u);
        }
    }
    {
        expiry_fixture_t fixture;
        tavrn_link_data_t data;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            data = link_data(fixture.link.config.local_peer.logical_id,
                             other_sid8, 0x7302u);
            ok &= tavrn_link_v2_send_unicast(&fixture.link, &peer_b, &data, now_ms,
                                              &(tavrn_link_event_t){0}) == TAVRN_LINK_SEND_OK;
            ok &= link_demand_matches(&fixture, &subject_sid8, &subject, now_ms,
                                       TAVRN_MAINT_DEMAND_CUSTODY_NEXT_HOP);
            ok &= maintenance_demand_matches(&fixture, &subject, now_ms,
                                              TAVRN_MAINT_DEMAND_CUSTODY_NEXT_HOP, 0u);
        }
    }
    {
        expiry_fixture_t fixture;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            /* Router's public candidate path immediately consumes an accepted
             * ingest.  Seed the exposed retained slot to isolate the pending
             * state without also manufacturing a queue owner. */
            fixture.router.pending_ingest.input.transmitter = peer_c;
            fixture.router.pending_ingest.input.data = link_data(
                peer(adva_d, TAVRN_IDENTITY_SID8).logical_id, subject_sid8, 0x7401u);
            fixture.router.pending_ingest.valid = 1u;
            ok &= router_demand_matches(&fixture, &subject_sid8, &subject, now_ms,
                                        TAVRN_MAINT_DEMAND_PENDING_INGEST);
            ok &= maintenance_demand_matches(&fixture, &subject, now_ms,
                                              TAVRN_MAINT_DEMAND_PENDING_INGEST, 0u);
        }
    }
    {
        expiry_fixture_t fixture;
        uint32_t expiry_at;

        if (!fixture_init_sid8(&fixture) ||
            !retain_forward_data_at_route_expiry(&fixture, subject_sid8, &peer_c,
                                                  now_ms, &expiry_at)) {
            ok = 0;
        } else {
            ok &= router_demand_matches(&fixture, &subject_sid8, &subject, expiry_at,
                                        TAVRN_MAINT_DEMAND_RETAINED_FINAL);
            ok &= maintenance_demand_matches(&fixture, &subject, expiry_at,
                                              TAVRN_MAINT_DEMAND_RETAINED_FINAL, 0u);
        }
    }
    {
        expiry_fixture_t fixture;
        uint32_t expiry_at;

        if (!fixture_init_sid8(&fixture) ||
            !retain_forward_data_at_route_expiry(&fixture, other_sid8, &peer_b,
                                                  now_ms, &expiry_at)) {
            ok = 0;
        } else {
            ok &= router_demand_matches(&fixture, &subject_sid8, &subject, expiry_at,
                                        TAVRN_MAINT_DEMAND_RETAINED_NEXT_HOP);
            ok &= maintenance_demand_matches(&fixture, &subject, expiry_at,
                                              TAVRN_MAINT_DEMAND_RETAINED_NEXT_HOP, 0u);
        }
    }
    {
        expiry_fixture_t fixture;
        tavrn_gtt_expiry_snapshot_t gtt_snapshot;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            ok &= tavrn_gtt_application_request(
                &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, &subject,
                now_ms, &gtt_snapshot) == TAVRN_GTT_APPLICATION_CHANGED;
            ok &= tavrn_gtt_expiry_snapshot(&fixture.gtt, &subject, now_ms,
                                             &gtt_snapshot) == TAVRN_GTT_EXPIRY_QUERY_FOUND &&
                gtt_snapshot.application_requested != 0u;
            ok &= maintenance_demand_matches(
                &fixture, &subject, now_ms, TAVRN_MAINT_DEMAND_APPLICATION_REQUEST, 0u);
        }
    }

    /* Fresh-fixture negatives: state that is merely nearby, expired, copied,
     * historical, or unresolved must fail closed without a demand reason. */
    {
        expiry_fixture_t fixture;
        tavrn_link_data_t data;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            data = link_data(fixture.link.config.local_peer.logical_id,
                             other_sid8, 0x7500u);
            ok &= tavrn_link_v2_send_unicast(&fixture.link, &peer_d, &data, now_ms,
                                              &(tavrn_link_event_t){0}) == TAVRN_LINK_SEND_OK;
            ok &= link_demand_matches(&fixture, &subject_sid8, &subject, now_ms, 0u);
            ok &= maintenance_has_no_demand(&fixture, &subject, now_ms);
        }
    }
    {
        expiry_fixture_t fixture;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            /* Permitted white-box pending-ingest seed: the retained input is
             * addressed to C.  B as immediate transmitter alone must not
             * become a final-data demand reason. */
            fixture.router.pending_ingest.input.transmitter = peer_b;
            fixture.router.pending_ingest.input.data = link_data(
                peer_d.logical_id, other_sid8, 0x7503u);
            fixture.router.pending_ingest.valid = 1u;
            ok &= router_demand_matches(&fixture, &subject_sid8, &subject, now_ms,
                                        0u);
            ok &= maintenance_has_no_demand(&fixture, &subject, now_ms);
        }
    }
    {
        expiry_fixture_t fixture;
        tavrn_direct_peer_t alias = peer(adva_b_alias, TAVRN_IDENTITY_SID8);
        tavrn_link_data_t data;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            data = link_data(fixture.link.config.local_peer.logical_id,
                             other_sid8, 0x7501u);
            ok &= tavrn_link_v2_send_unicast(&fixture.link, &alias, &data, now_ms,
                                              &(tavrn_link_event_t){0}) == TAVRN_LINK_SEND_OK;
            ok &= link_demand_matches(&fixture, &subject_sid8, &subject, now_ms, 0u);
            ok &= maintenance_has_no_demand(&fixture, &subject, now_ms);
        }
    }
    {
        expiry_fixture_t fixture;
        aodv_route_snapshot_t route;

        if (!fixture_init_sid8(&fixture) ||
            !install_route_via(&fixture, subject_sid8, &peer_c, now_ms, &route)) {
            ok = 0;
        } else {
            ok &= aodv_demand_matches(&fixture, &subject_sid8, &subject, route.expires_at_ms,
                                       0u, 0u);
            ok &= maintenance_has_no_demand(&fixture, &subject, route.expires_at_ms);
        }
    }
    {
        expiry_fixture_t fixture;
        aodv_route_snapshot_t route;

        if (!fixture_init_sid8(&fixture) ||
            !seed_precursor(&fixture, &peer_b, now_ms, &route)) {
            ok = 0;
        } else {
            ok &= aodv_demand_matches(&fixture, &subject_sid8, &subject, route.expires_at_ms,
                                       0u, 0u);
            ok &= maintenance_has_no_demand(&fixture, &subject, route.expires_at_ms);
        }
    }
    {
        expiry_fixture_t fixture;
        tron_application_data_t input = application(other_sid8);

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            /* The public API atomically pairs C's discovery with its pending
             * C DATA.  For the queried B, the future per-subject snapshot can
             * distinguish neither as an owner: no B-final DATA, route, or
             * action is present. */
            ok &= aodv_core_submit_application(&fixture.aodv, &input, now_ms) ==
                AODV_STATUS_QUEUED;
            ok &= aodv_demand_matches(&fixture, &subject_sid8, &subject, now_ms, 0u, 0u);
            ok &= maintenance_has_no_demand(&fixture, &subject, now_ms);
        }
    }
    {
        expiry_fixture_t fixture;
        tron_application_data_t input = application(other_sid8);

        if (!fixture_init_sid8(&fixture) ||
            !install_route_via(&fixture, other_sid8, &peer_d, now_ms, NULL)) {
            ok = 0;
        } else {
            ok &= aodv_core_submit_application(&fixture.aodv, &input, now_ms + 2u) ==
                AODV_STATUS_OK;
            ok &= aodv_demand_matches(&fixture, &subject_sid8, &subject, now_ms + 2u, 0u, 0u);
            ok &= maintenance_has_no_demand(&fixture, &subject, now_ms + 2u);
        }
    }
    {
        expiry_fixture_t fixture;

        if (!fixture_init_sid8(&fixture) ||
            !install_route_via(&fixture, other_sid8, &peer_d, now_ms, NULL)) {
            ok = 0;
        } else {
            ok &= aodv_core_report_link_failure(
                &fixture.aodv, &peer_d, &other_sid8,
                AODV_LINK_FAILURE_DEFER_REPAIR_DESTINATION_RERR, now_ms + 2u) ==
                AODV_FAILURE_OK;
            ok &= aodv_demand_matches(&fixture, &subject_sid8, &subject, now_ms + 2u, 0u, 0u);
            ok &= maintenance_has_no_demand(&fixture, &subject, now_ms + 2u);
        }
    }
    {
        expiry_fixture_t fixture;

        if (!fixture_init_sid8(&fixture) ||
            !seed_precursor(&fixture, &peer_c, now_ms, NULL)) {
            ok = 0;
        } else {
            ok &= aodv_demand_matches(&fixture, &subject_sid8, &subject, now_ms + 2u, 0u, 0u);
            ok &= maintenance_has_no_demand(&fixture, &subject, now_ms + 2u);
        }
    }
    {
        expiry_fixture_t fixture;
        uint32_t expiry_at;

        if (!fixture_init_sid8(&fixture) ||
            !retain_forward_data_at_route_expiry(&fixture, other_sid8, &peer_d,
                                                  now_ms, &expiry_at)) {
            ok = 0;
        } else {
            ok &= router_demand_matches(&fixture, &subject_sid8, &subject, expiry_at,
                                        0u);
            ok &= maintenance_has_no_demand(&fixture, &subject, expiry_at);
        }
    }
    {
        expiry_fixture_t fixture;
        tavrn_direct_peer_t alias = peer(adva_b_alias, TAVRN_IDENTITY_SID8);
        uint32_t expiry_at;

        if (!fixture_init_sid8(&fixture) ||
            !retain_forward_data_at_route_expiry(&fixture, other_sid8, &alias,
                                                  now_ms, &expiry_at)) {
            ok = 0;
        } else {
            ok &= router_demand_matches(&fixture, &subject_sid8, &subject, expiry_at,
                                        0u);
            ok &= maintenance_has_no_demand(&fixture, &subject, expiry_at);
        }
    }
    {
        expiry_fixture_t fixture;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            /* The post-ACK public path consumes an accepted pending ingest
             * immediately.  This permitted exposed-slot seed isolates C's
             * retained input while proving it cannot demand B. */
            fixture.router.pending_ingest.input.transmitter = peer_d;
            fixture.router.pending_ingest.input.data = link_data(
                peer_d.logical_id, other_sid8, 0x7502u);
            fixture.router.pending_ingest.valid = 1u;
            ok &= router_demand_matches(&fixture, &subject_sid8, &subject, now_ms,
                                        0u);
            ok &= maintenance_has_no_demand(&fixture, &subject, now_ms);
        }
    }
    {
        expiry_fixture_t fixture;
        ble_mesh_tx_item_t copied;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            memset(&copied, 0, sizeof(copied));
            copied.adv_len = 1u;
            copied.channel_mask = BLE_RADIO_ADV_CH_ALL;
            copied.priority = BLE_MESH_TX_PRIORITY_DATA;
            copied.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
            copied.not_before_ms = now_ms;
            copied.adv_data[0] = (uint8_t)subject_sid8.value;
            ok &= ble_mesh_scheduler_enqueue_ex(&fixture.scheduler, &copied).status ==
                BLE_MESH_SCHED_ENQUEUE_OK;
            ok &= maintenance_has_no_demand(&fixture, &subject, now_ms);
        }
    }
    {
        expiry_fixture_t fixture;
        tavrn_data_dedupe_entry_t *dedupe;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            /* A matching non-local unpinned entry has no public producer: the
             * real receive producer pins it.  This exposed cache-only state is
             * deliberately white-boxed to prove it is not an owner. */
            dedupe = &fixture.link.data_dedupe[0];
            dedupe->valid = 1u;
            dedupe->custody_pinned = 0u;
            dedupe->origin = peer(adva_d, TAVRN_IDENTITY_SID8).logical_id;
            dedupe->final_destination = subject_sid8;
            dedupe->data_seq = 0x7601u;
            dedupe->app_kind = 0x7fu;
            dedupe->expires_at_ms = now_ms + 1u;
            ok &= link_demand_matches(&fixture, &subject_sid8, &subject, now_ms, 0u);
            ok &= maintenance_has_no_demand(&fixture, &subject, now_ms);
        }
    }
    {
        expiry_fixture_t fixture;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            /* Completion normally clears delivery; no public producer leaves
             * historical bytes in a NONE reservation, so this is an exclusion
             * seed rather than a fabricated router owner. */
            fixture.router.delivery.transmitter = peer_c;
            fixture.router.delivery.data = link_data(
                peer(adva_d, TAVRN_IDENTITY_SID8).logical_id, subject_sid8, 0x7701u);
            fixture.router.delivery.token = 1u;
            fixture.router.delivery.state = TAVRN_ROUTER_DELIVERY_NONE;
            ok &= router_demand_matches(&fixture, &subject_sid8, &subject, now_ms, 0u);
            ok &= maintenance_has_no_demand(&fixture, &subject, now_ms);
        }
    }
    {
        expiry_fixture_t fixture;
        tavrn_gtt_expiry_snapshot_t gtt_snapshot;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            ok &= tavrn_gtt_expiry_snapshot(&fixture.gtt, &subject, now_ms,
                                             &gtt_snapshot) == TAVRN_GTT_EXPIRY_QUERY_FOUND &&
                gtt_snapshot.application_requested == 0u;
            ok &= maintenance_has_no_demand(&fixture, &subject, now_ms);
        }
    }
    {
        expiry_fixture_t fixture;
        tavrn_adva_t other = adva(adva_c);
        tavrn_gtt_evidence_t known_c = evidence(other, 0x6002u, 1u, 1u);
        tavrn_gtt_expiry_snapshot_t gtt_snapshot;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            ok &= tavrn_gtt_observe(&fixture.gtt, &known_c, now_ms) ==
                TAVRN_GTT_OBSERVE_ADDED;
            ok &= tavrn_gtt_application_request(
                &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, &other,
                now_ms, &gtt_snapshot) == TAVRN_GTT_APPLICATION_CHANGED;
            ok &= tavrn_gtt_expiry_snapshot(&fixture.gtt, &subject, now_ms,
                                             &gtt_snapshot) == TAVRN_GTT_EXPIRY_QUERY_FOUND &&
                gtt_snapshot.application_requested == 0u;
            ok &= maintenance_has_no_demand(&fixture, &subject, now_ms);
        }
    }
    {
        expiry_fixture_t fixture;
        tavrn_adva_t missing = adva(adva_c);
        tavrn_logical_id_t resolved;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            ok &= tavrn_full_resolve_unique_sid8(&fixture.full, &missing, &resolved) ==
                TAVRN_ESC_CONTEXT_UNKNOWN;
            ok &= maintenance_has_no_demand(&fixture, &missing, now_ms);
        }
    }
    {
        expiry_fixture_t fixture;
        tavrn_gtt_evidence_t collision;
        tavrn_logical_id_t resolved;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            collision = evidence(adva(adva_b_alias), 0x6001u, 1u, 1u);
            ok &= tavrn_gtt_observe(&fixture.gtt, &collision, now_ms) ==
                TAVRN_GTT_OBSERVE_ADDED;
            ok &= tavrn_full_resolve_unique_sid8(&fixture.full, &subject, &resolved) ==
                TAVRN_ESC_CONTEXT_COLLIDING;
            ok &= maintenance_is_unavailable(&fixture, &subject, now_ms);
        }
    }
    {
        expiry_fixture_t fixture;
        tavrn_router_subject_demand_snapshot_t router_snapshot;

        if (!fixture_init_sid8(&fixture)) {
            ok = 0;
        } else {
            /* The established post-ACK invalid-ingest precedent is the real
             * fail-stop path.  pending_ingest is the permitted isolated seed. */
            fixture.router.pending_ingest.input.transmitter = peer_c;
            fixture.router.pending_ingest.input.data = link_data(
                peer(adva_d, TAVRN_IDENTITY_SID8).logical_id, subject_sid8, 0x7801u);
            fixture.router.pending_ingest.input.data.app_len = TAVRN_LINK_APP_BYTES + 1u;
            fixture.router.pending_ingest.valid = 1u;
            ok &= tavrn_router_tick(&fixture.router, now_ms) == AODV_STATUS_INVALID;
            ok &= tavrn_router_fault_reason(&fixture.router) ==
                TAVRN_ROUTER_FAULT_POST_ACK_INGEST_INVALID;
            router_snapshot = tavrn_router_subject_demand_snapshot(
                &fixture.router, &subject_sid8, &subject, now_ms);
            ok &= router_snapshot.snapshot_available == 0u;
            ok &= maintenance_is_unavailable(&fixture, &subject, now_ms);
        }
    }
    ok &= test_maint_04_split_owner_order_contract();
    return ok;
}

static int test_maint_05_checked_departure_contract(void)
{
    int ok = 1;

    /* Every non-success status is evaluated against real copied GTT/demand
     * state and must leave GTT plus every side-effect owner unchanged. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_adva_t missing = generated_adva(0x79u);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 2u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t self_before;
        tavrn_gtt_expiry_snapshot_t requested;
        tavrn_gtt_expiry_snapshot_t departed;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_gtt_departure_candidate_t malformed_candidate;
        tavrn_gtt_departure_candidate_t missing_candidate;
        tavrn_maintenance_demand_snapshot_t demand;
        tavrn_maintenance_demand_snapshot_t stale_no_demand;
        tavrn_maintenance_demand_snapshot_t missing_demand;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 0u,
                              TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u, &before) ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                     &demand) ||
            expiry_snapshot_query(&fixture.gtt, &fixture.gtt.config.local_identity,
                                   before.hard_deadline_ms, &self_before) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        stale_no_demand = demand;
        ok &= stale_no_demand.reason_mask == 0u;
        candidate = candidate_from_snapshot(&before);
        malformed_candidate = candidate;
        malformed_candidate.hard_deadline_ms++;
        ok &= checked_departure_rejection_is_immutable(
            &fixture, &malformed_candidate, candidate.hard_deadline_ms,
            TAVRN_GTT_CHECKED_INVALID, &before);
        missing_candidate.identity = missing;
        missing_candidate.revision = candidate.revision;
        missing_candidate.hard_deadline_ms = candidate.hard_deadline_ms;
        memset(&missing_demand, 0, sizeof(missing_demand));
        ok &= phase5_maintenance_demand(&fixture.maintenance, &missing,
                                         candidate.hard_deadline_ms,
                                         &missing_demand) == TAVRN_MAINTENANCE_DEMAND_OK &&
            checked_departure_rejection_is_immutable(
                &fixture, &missing_candidate, candidate.hard_deadline_ms,
                TAVRN_GTT_CHECKED_NOT_FOUND, NULL);
        candidate.revision--;
        ok &= checked_departure_rejection_is_immutable(
            &fixture, &candidate, before.hard_deadline_ms,
            TAVRN_GTT_CHECKED_STALE_REVISION, &before);
        candidate = candidate_from_snapshot(&before);
        ok &= checked_departure_rejection_is_immutable(
            &fixture, &candidate, before.hard_deadline_ms - 1u,
            TAVRN_GTT_CHECKED_NOT_HARD_EXPIRED, &before);
        candidate = candidate_from_snapshot(&self_before);
        ok &= checked_departure_rejection_is_immutable(
            &fixture, &candidate, before.hard_deadline_ms,
            TAVRN_GTT_CHECKED_SELF, &self_before);
        if (phase5_gtt_request(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                               &subject, before.hard_deadline_ms, &requested) !=
                TAVRN_GTT_APPLICATION_CHANGED ||
            expiry_snapshot_query(&fixture.gtt, &subject, before.hard_deadline_ms,
                                   &requested) != TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        candidate = candidate_from_snapshot(&requested);
        /* The caller's retained no-demand observation predates the request.
         * Checked departure must recompute demand at the mutation boundary,
         * not trust this stale snapshot. */
        ok &= checked_departure_rejection_is_immutable(
            &fixture, &candidate, candidate.hard_deadline_ms,
            TAVRN_GTT_CHECKED_DEMAND_DEFERRED, &requested);
        if (phase5_gtt_cancel(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                              &subject, before.hard_deadline_ms, &requested) !=
                TAVRN_GTT_APPLICATION_CANCELED ||
            expiry_snapshot_query(&fixture.gtt, &subject, before.hard_deadline_ms,
                                  &requested) != TAVRN_GTT_EXPIRY_QUERY_FOUND ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                     &demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&requested);
        memset(&departed, 0, sizeof(departed));
        if (phase5_maintenance_checked_departure(&fixture.maintenance, &candidate,
                                                 candidate.hard_deadline_ms,
                                                 &departed) != TAVRN_GTT_CHECKED_DEPARTED ||
            !maintenance_demand_for(&fixture, &subject, candidate.hard_deadline_ms,
                                     &demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&departed);
        memset(&demand, 0, sizeof(demand));
        if (phase5_maintenance_demand(&fixture.maintenance, &subject,
                                      departed.departed_deadline_ms - 1u,
                                      &demand) != TAVRN_MAINTENANCE_DEMAND_OK) {
            return 0;
        }
        ok &= checked_departure_rejection_is_immutable(
            &fixture, &candidate, departed.departed_deadline_ms - 1u,
            TAVRN_GTT_CHECKED_ALREADY_DEPARTED, &departed);
        ok &= checked_departure_rejection_is_immutable(
            &fixture, NULL, departed.departed_deadline_ms - 1u,
            TAVRN_GTT_CHECKED_INVALID, NULL);
    }

    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_logical_id_t subject_sid8 = peer(adva_b, TAVRN_IDENTITY_SID8).logical_id;
        tavrn_direct_peer_t next_hop = peer(adva_c, TAVRN_IDENTITY_SID8);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 2u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t no_demand;
        aodv_route_snapshot_t route;

        /* A prior aggregate no-demand read cannot authorize departure.  This
         * creates actual unexpired AODV state afterwards, then invokes checked
         * departure directly so the owner must recompute current demand at its
         * mutation boundary. */
        if (!fixture_init_sid8(&fixture) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 20u,
                              TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 3u, 0u, &before) ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                    &no_demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        ok &= no_demand.reason_mask == 0u;
        if (!install_route_via(&fixture, subject_sid8, &next_hop,
                               candidate.hard_deadline_ms - 2u, &route)) {
            return 0;
        }
        ok &= route.state == AODV_ROUTE_VALID &&
            deadline_precedes(candidate.hard_deadline_ms, route.expires_at_ms) &&
            expiry_snapshot_query(&fixture.gtt, &subject, candidate.hard_deadline_ms,
                                  &before) == TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            checked_departure_rejection_is_immutable(
                &fixture, &candidate, candidate.hard_deadline_ms,
                TAVRN_GTT_CHECKED_DEMAND_DEFERRED, &before) &&
            aodv_core_route_snapshot(&fixture.aodv, &subject_sid8, &route) ==
                AODV_ROUTE_QUERY_FOUND && route.state == AODV_ROUTE_VALID;
    }

    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_logical_id_t subject_sid8 = peer(adva_b, TAVRN_IDENTITY_SID8).logical_id;
        tavrn_direct_peer_t next_hop = peer(adva_c, TAVRN_IDENTITY_SID8);
        tavrn_gtt_evidence_t input = evidence(subject, 2u, 1u, 2u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t no_demand;
        tavrn_link_data_t custody_data;

        /* A real live link custody is created after the no-demand read. */
        if (!fixture_init_sid8(&fixture) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 20u,
                              TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 3u, 0u, &before) ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                    &no_demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        custody_data = link_data(fixture.link.config.local_peer.logical_id, subject_sid8,
                                 0x7c01u);
        ok &= no_demand.reason_mask == 0u &&
            tavrn_link_v2_send_unicast(&fixture.link, &next_hop, &custody_data,
                                       candidate.hard_deadline_ms - 1u,
                                       &(tavrn_link_event_t){0}) == TAVRN_LINK_SEND_OK &&
            expiry_snapshot_query(&fixture.gtt, &subject, candidate.hard_deadline_ms,
                                  &before) == TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            checked_departure_rejection_is_immutable(
                &fixture, &candidate, candidate.hard_deadline_ms,
                TAVRN_GTT_CHECKED_DEMAND_DEFERRED, &before);
    }

    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_logical_id_t subject_sid8 = peer(adva_b, TAVRN_IDENTITY_SID8).logical_id;
        tavrn_direct_peer_t next_hop = peer(adva_c, TAVRN_IDENTITY_SID8);
        tavrn_gtt_evidence_t input = evidence(subject, 3u, 1u, 2u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t no_demand;
        uint32_t retained_at;

        /* Retained router ownership is created through the real route-expiry
         * dispatch path after a no-demand read, never by seeding its fields. */
        if (!fixture_init_sid8(&fixture) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 20u,
                              TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 3u, 0u, &before) ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                    &no_demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        if (!retain_forward_data_at_route_expiry(&fixture, subject_sid8, &next_hop,
                                                 candidate.hard_deadline_ms - 42u,
                                                 &retained_at)) {
            return 0;
        }
        ok &= no_demand.reason_mask == 0u && retained_at + 1u == candidate.hard_deadline_ms &&
            fixture.router.retained_action_valid != 0u &&
            expiry_snapshot_query(&fixture.gtt, &subject, candidate.hard_deadline_ms,
                                  &before) == TAVRN_GTT_EXPIRY_QUERY_FOUND &&
            checked_departure_rejection_is_immutable(
                &fixture, &candidate, candidate.hard_deadline_ms,
                TAVRN_GTT_CHECKED_DEMAND_DEFERRED, &before);
    }

    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 2u);
        tavrn_gtt_evidence_t alias = evidence(adva(adva_b_alias), 1u, 1u, 2u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t unavailable_demand;

        if (!fixture_init_sid8(&fixture) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 20u,
                              TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED, 3u, 0u, &before) ||
            !observe_expected(&fixture, &alias,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 21u,
                              TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 4u, 0u, NULL) ||
            expiry_snapshot_query(&fixture.gtt, &subject, before.hard_deadline_ms,
                                  &before) != TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        memset(&unavailable_demand, 0, sizeof(unavailable_demand));
        ok &= phase5_maintenance_demand(&fixture.maintenance, &subject,
                                         candidate.hard_deadline_ms,
                                         &unavailable_demand) ==
                TAVRN_MAINTENANCE_DEMAND_UNAVAILABLE &&
            checked_departure_rejection_is_immutable(
                &fixture, &candidate, candidate.hard_deadline_ms,
                TAVRN_GTT_CHECKED_UNAVAILABLE, &before);
        {
            tavrn_maintenance_expiry_sweep_snapshot_t pre;
            tavrn_maintenance_expiry_sweep_snapshot_t due;

            ok &= phase5_maintenance_sweep(&fixture.maintenance,
                                            candidate.hard_deadline_ms - 25u,
                                            &pre) == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
                pre.pass_performed == 0u &&
                phase5_maintenance_sweep(&fixture.maintenance,
                                          candidate.hard_deadline_ms,
                                          &due) == TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
                due.pass_performed != 0u && due.hard_selected_count >= 1u &&
                due.unavailable_count >= 1u && due.departed_count == 0u;
        }
    }

    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_c);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 1u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t demand;
        tavrn_maintenance_snapshot_t maintenance;
        uint32_t stable_at;

        if (!fixture_init_sid8(&fixture) ||
            !advance_maintenance_to_stable(&fixture, &stable_at) ||
            !observe_expected(&fixture, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                              stable_at + 1u, TAVRN_GTT_EXPIRY_OBSERVE_ADDED,
                              3u, 1u, &before) ||
            tavrn_maintenance_snapshot(&fixture.maintenance, &maintenance) !=
                TAVRN_MAINTENANCE_OK ||
            tavrn_maintenance_tick(&fixture.maintenance,
                                   maintenance.next_topology_sample_ms) !=
                TAVRN_MAINTENANCE_OK) {
            return 0;
        }
        clear_queued_controls(&fixture);
        if (tavrn_maintenance_snapshot(&fixture.maintenance, &maintenance) !=
                TAVRN_MAINTENANCE_OK ||
            expiry_snapshot_query(&fixture.gtt, &subject,
                                  maintenance.next_topology_sample_ms, &before) !=
                TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        if (!maintenance_demand_for(&fixture, &subject, candidate.hard_deadline_ms,
                                    &demand)) {
            return 0;
        }
        ok &= checked_departure_rejection_is_immutable(
            &fixture, &candidate, candidate.hard_deadline_ms,
            TAVRN_GTT_CHECKED_DIRECT_DEADLINE_DEFERRED, &before);
    }

    /* A genuinely direct, publicly activated member can depart once its real
     * interval-owned direct deadline has elapsed; departure clears direct
     * facts without emitting any subsystem work. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_c);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 1u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t departed;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t demand;
        expiry_side_effect_state_t effects_before;
        expiry_side_effect_state_t effects_after;
        tavrn_gtt_counters_t counters_before;

        if (!fixture_init_sid8(&fixture) ||
            !observe_expected(&fixture, &input, TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                              30u, TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 3u, 1u,
                              &before) ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                    &demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        counters_before = *tavrn_gtt_counters(&fixture.gtt);
        if (!capture_side_effect_state(&fixture, &effects_before)) {
            return 0;
        }
        memset(&departed, 0, sizeof(departed));
        ok &= phase5_maintenance_checked_departure(&fixture.maintenance, &candidate,
                                                    candidate.hard_deadline_ms,
                                                    &departed) == TAVRN_GTT_CHECKED_DEPARTED;
        if (!capture_side_effect_state(&fixture, &effects_after)) {
            return 0;
        }
        ok &= side_effect_states_equal(&effects_before, &effects_after) &&
            departed.direct == 0u && departed.last_direct_evidence_ms == 0u &&
            departed.application_requested == 0u &&
            tavrn_gtt_counters(&fixture.gtt)->departed == counters_before.departed + 1u &&
            tavrn_gtt_counters(&fixture.gtt)->observed == counters_before.observed;
    }

    /* A same-timestamp owner request is ordered before verification.  Cancel
     * yields a new copied revision, after which one checked departure changes
     * only GTT state and preserves the evidence facts. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 0x2345u, 1u, 2u);
        tavrn_gtt_evidence_t resurrection;
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t requested;
        tavrn_gtt_expiry_snapshot_t eligible;
        tavrn_gtt_expiry_snapshot_t departed;
        tavrn_gtt_expiry_snapshot_t resurrected;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t demand;
        expiry_side_effect_state_t effects_before;
        expiry_side_effect_state_t effects_after;
        tavrn_gtt_counters_t counters_before;
        tavrn_gtt_counters_t counters_before_departure;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 0u,
                              TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u, &before) ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                    &demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        if (phase5_gtt_request(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                               &subject, before.hard_deadline_ms, &requested) !=
                TAVRN_GTT_APPLICATION_CHANGED ||
            phase5_maintenance_checked_departure(&fixture.maintenance, &candidate,
                                                 candidate.hard_deadline_ms,
                                                 &departed) !=
                TAVRN_GTT_CHECKED_STALE_REVISION ||
            phase5_gtt_cancel(&fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                               &subject, before.hard_deadline_ms, &eligible) !=
                TAVRN_GTT_APPLICATION_CANCELED ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                    &demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&eligible);
        counters_before_departure = *tavrn_gtt_counters(&fixture.gtt);
        if (!capture_side_effect_state(&fixture, &effects_before)) {
            return 0;
        }
        memset(&departed, 0, sizeof(departed));
        ok &= phase5_maintenance_checked_departure(&fixture.maintenance, &candidate,
                                                    candidate.hard_deadline_ms,
                                                    &departed) == TAVRN_GTT_CHECKED_DEPARTED;
        if (!capture_side_effect_state(&fixture, &effects_after)) {
            return 0;
        }
        ok &= side_effect_states_equal(&effects_before, &effects_after) &&
            departed.last_evidence_ms == eligible.last_evidence_ms &&
            departed.serial_present == eligible.serial_present &&
            departed.serial == eligible.serial && departed.hop_count == eligible.hop_count &&
            departed.hard_deadline_ms == eligible.hard_deadline_ms &&
            departed.direct == 0u && departed.application_requested == 0u &&
            departed.departed_deadline_ms == candidate.hard_deadline_ms +
                fixture.gtt.config.departed_retention_ms && departed.revision != 0u &&
            departed.storage_generation != 0u && departed.revision != eligible.revision &&
            tavrn_gtt_counters(&fixture.gtt)->departed ==
                counters_before_departure.departed + 1u &&
            tavrn_gtt_counters(&fixture.gtt)->observed ==
                counters_before_departure.observed;
        counters_before = *tavrn_gtt_counters(&fixture.gtt);
        resurrection = evidence(subject, (uint16_t)(input.serial + 1u), 1u, 2u);
        if (!observe_expected(&fixture, &resurrection,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                              candidate.hard_deadline_ms + 1u,
                              TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED,
                              departed.revision + 1u, 0u, &resurrected)) {
            return 0;
        }
        ok &= resurrected.freshness == TAVRN_GTT_FRESHNESS_ACTIVE &&
            tavrn_gtt_counters(&fixture.gtt)->resurrected ==
                counters_before.resurrected + 1u;
    }

    /* Retention is inclusive through deadline-1 and purges at exact equality
     * without routing, control, scheduler, or received-evidence side effects. */
    {
        expiry_fixture_t fixture;
        tavrn_adva_t subject = adva(adva_b);
        tavrn_gtt_evidence_t input = evidence(subject, 1u, 1u, 2u);
        tavrn_gtt_expiry_snapshot_t before;
        tavrn_gtt_expiry_snapshot_t departed;
        tavrn_gtt_departure_candidate_t candidate;
        tavrn_maintenance_demand_snapshot_t demand;
        tavrn_maintenance_expiry_sweep_snapshot_t sweep;
        expiry_side_effect_state_t effects_before;
        expiry_side_effect_state_t effects_after;

        if (!fixture_init(&fixture, 0u) ||
            !observe_expected(&fixture, &input,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 0u,
                              TAVRN_GTT_EXPIRY_OBSERVE_ADDED, 2u, 0u, &before) ||
            !maintenance_demand_for(&fixture, &subject, before.hard_deadline_ms,
                                    &demand)) {
            return 0;
        }
        candidate = candidate_from_snapshot(&before);
        memset(&departed, 0, sizeof(departed));
        if (phase5_maintenance_checked_departure(&fixture.maintenance, &candidate,
                                                 candidate.hard_deadline_ms,
                                                 &departed) != TAVRN_GTT_CHECKED_DEPARTED ||
            expiry_snapshot_query(&fixture.gtt, &subject,
                                  departed.departed_deadline_ms - 1u,
                                  &before) != TAVRN_GTT_EXPIRY_QUERY_FOUND) {
            return 0;
        }
        ok &= before.freshness == TAVRN_GTT_FRESHNESS_DEPARTED;
        /* The first owner post establishes expiry cadence.  Prime it after the
         * subject has departed, then verify the retained row is purged at its
         * exact retention deadline. */
        if (phase5_maintenance_sweep(&fixture.maintenance, candidate.hard_deadline_ms,
                                     &sweep) != TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK ||
            sweep.pass_performed != 0u) {
            return 0;
        }
        if (!capture_side_effect_state(&fixture, &effects_before) ||
            phase5_maintenance_sweep(&fixture.maintenance,
                                     departed.departed_deadline_ms, &sweep) !=
                TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK ||
            !capture_side_effect_state(&fixture, &effects_after)) {
            return 0;
        }
        ok &= sweep.pass_performed != 0u && sweep.purged_count >= 1u &&
            sweep.targeted_control_count == 0u && sweep.rreq_control_count == 0u &&
            sweep.expiry_tc_control_count == 0u &&
            transport_side_effect_states_equal(&effects_before, &effects_after) &&
            expiry_snapshot_query(&fixture.gtt, &subject,
                                  departed.departed_deadline_ms, &before) ==
                TAVRN_GTT_EXPIRY_QUERY_NOT_FOUND;
    }
    return ok;
}

int main(void)
{
    CHECK("GTT-02", test_gtt_02_real_state_contract());
    CHECK("GTT-03", test_gtt_03_provenance_contract());
    CHECK("GTT-05", test_gtt_05_visibility_contract());
    CHECK("MAINT-01", test_maint_01_real_deadline_and_sweep_contract());
    CHECK("MAINT-02", test_maint_02_direct_gate_contract());
    CHECK("MAINT-04", test_maint_04_real_owner_contract());
    CHECK("MAINT-04", scheduler_gap_telemetry_contract());
    CHECK("MAINT-04", full_table_hook_seed_contract());
    CHECK("MAINT-05", test_maint_05_checked_departure_contract());
    if (failures != 0u) {
#if defined(TAVRN_PHASE5_EXPIRY_RED_MODE)
        printf("tavrn_phase5_expiry_demand RED tests failed: %u assertion(s)\n", failures);
#else
        printf("tavrn_phase5_expiry_demand GREEN tests failed: %u assertion(s)\n", failures);
#endif
        return 1;
    }
    printf("tavrn_phase5_expiry_demand tests passed\n");
    return 0;
}
