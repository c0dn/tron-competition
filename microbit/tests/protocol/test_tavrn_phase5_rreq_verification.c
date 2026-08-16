#include "aodv_core.h"
#include "ble_mesh_scheduler.h"
#include "tavrn_full.h"
#include "tavrn_gtt.h"
#include "tavrn_link_v2.h"
#include "tavrn_maintenance.h"
#include "tavrn_mentorship.h"
#include "tavrn_phase5_rreq_verification_contract.h"
#include "tavrn_router.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NETWORK_ID 0x2au
#define HARD_EXPIRY_MS 300u
#define NET_TRAVERSAL_MS 30u
#define PATH_DISCOVERY_MS 60u
#define VERIFICATION_WINDOW_MS 150u

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
static const uint8_t adva_g[TAVRN_ADVA_LEN] = { 0x77u, 0x88u, 0x99u, 0xaau, 0xbbu, 0xc5u };
static const uint8_t adva_e_alias[TAVRN_ADVA_LEN] = { 0x55u, 0x77u, 0x28u, 0x4au, 0x6cu, 0xc6u };

typedef struct verification_fixture {
    ble_mesh_scheduler_t scheduler;
    tavrn_link_v2_t link;
    aodv_core_t aodv;
    tavrn_gtt_t gtt;
    tavrn_gtt_storage_t gtt_storage;
    tavrn_full_t full;
    tavrn_router_t router;
    tavrn_mentorship_t mentorship;
    tavrn_maintenance_t maintenance;
} verification_fixture_t;

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
    config.initial_request_id = 0xfffeu;
    config.initial_rerr_sequence = 1u;
    config.node_traversal_ms = 1u;
    config.path_discovery_ms = PATH_DISCOVERY_MS;
    config.rreq_seen_ms = 10000u;
    config.active_route_ms = 36000u;
    config.pending_data_ms = 5000u;
    config.blacklist_ms = NET_TRAVERSAL_MS;
    config.rrep_dedupe_ms = 10000u;
    config.rerr_dedupe_ms = 10000u;
    config.rrep_ack_wait_ms = 250u;
    return config;
}

static int observe(verification_fixture_t *fixture,
                   const uint8_t identity[TAVRN_ADVA_LEN], uint8_t hop,
                   uint16_t serial, tavrn_gtt_provenance_t provenance,
                   uint32_t now_ms, tavrn_gtt_expiry_snapshot_t *snapshot_out)
{
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_expiry_observe_status_t status;

    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = adva(identity);
    evidence.serial = serial;
    evidence.serial_present = 1u;
    evidence.hop_count = hop;
    evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    status = tavrn_gtt_observe_with_provenance(&fixture->gtt, &evidence,
                                                provenance, now_ms, snapshot_out);
    return status == TAVRN_GTT_EXPIRY_OBSERVE_ADDED ||
        status == TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED;
}

static int fixture_init(verification_fixture_t *fixture)
{
    tavrn_gtt_config_t gtt_config;
    tavrn_router_incarnation_config_t incarnation;
    tavrn_mentorship_config_t mentorship_config;
    tavrn_maintenance_config_t maintenance_config;
    tavrn_router_augmentation_hooks_t hooks;
    tavrn_mentorship_state_snapshot_t state;
    tavrn_link_config_t configured_link = link_config();
    aodv_core_config_t configured_aodv = aodv_config();

    memset(fixture, 0, sizeof(*fixture));
    memset(&gtt_config, 0, sizeof(gtt_config));
    gtt_config.local_identity = adva(adva_a);
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
    maintenance_config.hello_change_ms = 2000u;
    maintenance_config.hello_stable_ms = 4000u;
    maintenance_config.hello_alpha_permille = 800u;
    maintenance_config.hello_snap_permille = 950u;
    maintenance_config.topology_sample_ms = 1000u;
    maintenance_config.hello_dedupe_ms = 10000u;
    maintenance_config.initial_node_sequence = 1u;
    maintenance_config.aodv_net_traversal_ms = NET_TRAVERSAL_MS;
    maintenance_config.freshness_response_min_ms = 10u;
    maintenance_config.freshness_response_max_ms = 100u;

    ble_mesh_scheduler_init(&fixture->scheduler, 0u, adva_a);
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
        !observe(fixture, adva_b, 1u, 1u, TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 1u, NULL) ||
        tavrn_mentorship_tick(&fixture->mentorship, 20u) !=
            TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED ||
        tavrn_mentorship_state_snapshot(&fixture->mentorship, &state) !=
            TAVRN_MENTORSHIP_OK || state.state != TAVRN_MENTORSHIP_SID8_ACTIVE ||
        tavrn_maintenance_activate(&fixture->maintenance, 20u) != TAVRN_MAINTENANCE_OK) {
        return 0;
    }
    return 1;
}

static int start_stage1_at(verification_fixture_t *fixture,
                           const uint8_t subject_bytes[TAVRN_ADVA_LEN], uint8_t hop,
                           tavrn_gtt_provenance_t provenance, uint32_t observed_at_ms,
                           uint8_t *index_out, uint32_t *expiry_out)
{
    tavrn_adva_t subject = adva(subject_bytes);
    tavrn_gtt_expiry_snapshot_t expiry;
    tavrn_targeted_freshness_action_t action;
    tavrn_targeted_freshness_snapshot_t targeted;
    uint8_t index;

    if (!observe(fixture, subject_bytes, hop, 1u, provenance, observed_at_ms, &expiry) ||
        tavrn_gtt_application_request(&fixture->gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                                      &subject, observed_at_ms + 1u, NULL) !=
            TAVRN_GTT_APPLICATION_CHANGED ||
        tavrn_maintenance_targeted_begin_stage0(&fixture->maintenance, &fixture->router,
                                                &fixture->link, &fixture->gtt, &subject,
                                                expiry.hard_deadline_ms, &action) !=
            TAVRN_TARGETED_FRESHNESS_DEFERRED ||
        tavrn_maintenance_targeted_snapshot(&fixture->maintenance, &fixture->router,
                                            &fixture->link, &fixture->gtt, &targeted) !=
            TAVRN_TARGETED_FRESHNESS_OK) {
        return 0;
    }
    for (index = 0u; index < TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY; index++) {
        if (targeted.contexts[index].valid != 0u &&
            memcmp(targeted.contexts[index].subject.bytes, subject.bytes,
                   TAVRN_ADVA_LEN) == 0 &&
            targeted.contexts[index].stage == TAVRN_TARGETED_STAGE1_READY) {
            *index_out = index;
            *expiry_out = expiry.hard_deadline_ms;
            return 1;
        }
    }
    return 0;
}

static int start_stage1(verification_fixture_t *fixture,
                        const uint8_t subject_bytes[TAVRN_ADVA_LEN], uint8_t hop,
                        tavrn_gtt_provenance_t provenance, uint8_t *index_out,
                        uint32_t *expiry_out)
{
    return start_stage1_at(fixture, subject_bytes, hop, provenance, 21u,
                           index_out, expiry_out);
}

static tavrn_gtt_entry_t *entry_for(verification_fixture_t *fixture,
                                    const uint8_t subject[TAVRN_ADVA_LEN])
{
    uint8_t index;

    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        tavrn_gtt_entry_t *entry = &fixture->gtt_storage.entries[index];

        if (entry->occupied != 0u &&
            memcmp(entry->identity.bytes, subject, TAVRN_ADVA_LEN) == 0) {
            return entry;
        }
    }
    return NULL;
}

static tavrn_rreq_verification_completion_t completion_for(
    const tavrn_rreq_verification_action_t *action,
    tavrn_rreq_verification_terminal_kind_t kind, uint8_t mask)
{
    tavrn_rreq_verification_completion_t completion;

    memset(&completion, 0, sizeof(completion));
    completion.attempt = action->attempt;
    completion.token = action->token;
    completion.purpose = action->purpose;
    completion.kind = kind;
    completion.completed_channel_mask = mask;
    return completion;
}

#if !defined(TAVRN_PHASE5_RREQ_VERIFICATION_RED_MODE)
static tavrn_rx_control_event_t rrep_for(
    const tavrn_rreq_verification_action_t *action,
    const uint8_t transmitter[TAVRN_ADVA_LEN])
{
    tavrn_rx_control_event_t event;

    memset(&event, 0, sizeof(event));
    event.transmitter = peer(transmitter, TAVRN_IDENTITY_SID8);
    event.control.type = TAVRN_WIRE_E_RREP;
    event.control.pdu_len = 16u;
    event.control.pdu[0] = 0x54u;
    event.control.pdu[1] = 0x52u;
    event.control.pdu[2] = 0x02u;
    event.control.pdu[3] = NETWORK_ID;
    event.control.pdu[4] = TAVRN_WIRE_E_RREP;
    event.control.pdu[5] = 0x80u;
    event.control.pdu[6] = 0x10u;
    event.control.pdu[7] = adva_a[0];
    event.control.pdu[8] = (uint8_t)action->attempt.destination.value;
    event.control.pdu[9] = 2u;
    event.control.pdu[10] = 0u;
    event.control.pdu[11] = adva_a[0];
    event.control.pdu[12] = (uint8_t)action->attempt.request_id;
    event.control.pdu[13] = (uint8_t)(action->attempt.request_id >> 8);
    event.control.pdu[14] = 36u;
    event.control.pdu[15] = 0u;
    return event;
}

#endif

static int action_is(const tavrn_rreq_verification_action_t *action,
                     tavrn_rreq_verification_purpose_t purpose,
                     uint8_t scope, const uint8_t subject[TAVRN_ADVA_LEN])
{
    return action != NULL && action->enqueued != 0u && action->purpose == purpose &&
        action->token >= 0x8000u && action->attempt.request_id != 0u &&
        action->aodv_action.type == AODV_ACTION_SEND_RREQ &&
        action->aodv_action.detail.control.rreq_attempt_present ==
            AODV_RREQ_ATTEMPT_PRESENT &&
        action->aodv_action.detail.control.rreq_attempt.request_id ==
            action->attempt.request_id &&
        action->attempt.current_scope == scope &&
        memcmp(action->subject.bytes, subject, TAVRN_ADVA_LEN) == 0;
}

static int fill_control_queue(verification_fixture_t *fixture)
{
    ble_mesh_tx_item_t item;
    uint8_t index;

    memset(&item, 0, sizeof(item));
    item.adv_len = 1u;
    item.adv_data[0] = 0x5au;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = BLE_MESH_TX_PRIORITY_CONTROL;
    item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (ble_mesh_scheduler_enqueue_ex(&fixture->scheduler, &item).status !=
            BLE_MESH_SCHED_ENQUEUE_OK) {
            return 0;
        }
    }
    return 1;
}

static void clear_queue(verification_fixture_t *fixture)
{
    uint8_t index;

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (fixture->scheduler.routed_tx_queue.entries[index].occupied != 0u) {
            (void)ble_mesh_tx_queue_remove(&fixture->scheduler.routed_tx_queue,
                                           index, NULL);
        }
    }
}

static void test_stage1_scope_stage2_fresh_id(void)
{
    verification_fixture_t fixture;
    verification_fixture_t zero_hop_fixture;
    verification_fixture_t wrap_fixture;
    tavrn_rreq_verification_action_t stage1;
    tavrn_rreq_verification_action_t stage2;
    tavrn_rreq_verification_action_t zero_hop_action;
    tavrn_rreq_verification_completion_t completion;
    tavrn_rreq_verification_snapshot_t snapshot;
    aodv_action_t ordinary_action;
    uint8_t index;
    uint8_t wrap_index;
    uint32_t now_ms;

    REACHED("stage1-retained-cap-stage2-fresh");
    STRUCTURAL("stage-scope-fixture", fixture_init(&fixture));
    STRUCTURAL("stage-scope-stage1", start_stage1(&fixture, adva_c, 14u,
                                                     TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                                     &index, &now_ms));
    if (structural_failures != 0u) return;
    memset(&stage1, 0, sizeof(stage1));
    CHECK("MAINT-06", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt, now_ms,
                                               &stage1) == TAVRN_RREQ_VERIFICATION_OK);
    CHECK("MAINT-06", stage1.purpose ==
                          TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE1_RETAINED_HOP);
    CHECK("MAINT-07", action_is(&stage1,
                                 TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE1_RETAINED_HOP,
                                 15u, adva_c));
    CHECK("GTT-06", stage1.attempt.initial_scope == 15u &&
                    stage1.attempt.current_scope == 15u);
    CHECK("SERIAL-01", stage1.attempt.request_id == 0xfffeu);
    CHECK("AODV-02", stage1.attempt.discovery_correlation != 0u);
    CHECK("AODV-02", aodv_core_poll_action(&fixture.aodv, &ordinary_action) ==
                      AODV_ACTION_POLL_EMPTY);
    CHECK("AODV-04", phase5_rreq_snapshot(&fixture.maintenance, &fixture.router,
                                            &fixture.link, &fixture.gtt, &snapshot) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[index].stage ==
                          TAVRN_RREQ_VERIFICATION_STAGE1_READY &&
                      snapshot.ordinary_pending_data_created == 0u &&
                      snapshot.ordinary_inner_retry_created == 0u &&
                      snapshot.ordinary_smart_ttl_fallback_created == 0u);
    completion = completion_for(&stage1, TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE, 0x01u);
    CHECK("MAINT-06", phase5_rreq_terminal(&fixture.maintenance, &fixture.router,
                                             &fixture.link, &completion, now_ms) ==
                          TAVRN_RREQ_VERIFICATION_OK);
    CHECK("MAINT-07", phase5_rreq_snapshot(&fixture.maintenance, &fixture.router,
                                             &fixture.link, &fixture.gtt, &snapshot) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[index].stage ==
                          TAVRN_RREQ_VERIFICATION_STAGE1_WAIT_RESPONSE);
    memset(&stage2, 0, sizeof(stage2));
    CHECK("MAINT-06", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt,
                                               now_ms + PATH_DISCOVERY_MS, &stage2) ==
                          TAVRN_RREQ_VERIFICATION_OK);
    CHECK("AODV-04", action_is(&stage2,
                                TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE2_FULL_DIAMETER,
                                15u, adva_c) &&
                      stage2.attempt.request_id != stage1.attempt.request_id);

    STRUCTURAL("zero-hop-fixture", fixture_init(&zero_hop_fixture));
    STRUCTURAL("zero-hop-stage1", start_stage1(&zero_hop_fixture, adva_d, 1u,
                                                 TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                                 &index, &now_ms));
    STRUCTURAL("zero-hop-entry", entry_for(&zero_hop_fixture, adva_d) != NULL);
    if (structural_failures != 0u) return;
    entry_for(&zero_hop_fixture, adva_d)->hop_count = 0u;
    memset(&zero_hop_action, 0, sizeof(zero_hop_action));
    CHECK("GTT-06", phase5_rreq_owner_tick(&zero_hop_fixture.maintenance,
                                             &zero_hop_fixture.router,
                                             &zero_hop_fixture.link,
                                             &zero_hop_fixture.gtt, now_ms,
                                             &zero_hop_action) == TAVRN_RREQ_VERIFICATION_OK &&
                    action_is(&zero_hop_action,
                              TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE2_FULL_DIAMETER,
                              15u, adva_d));
    CHECK("MAINT-06", phase5_rreq_snapshot(&zero_hop_fixture.maintenance,
                                             &zero_hop_fixture.router,
                                             &zero_hop_fixture.link,
                                             &zero_hop_fixture.gtt, &snapshot) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[index].stage1_request_id == 0u &&
                      snapshot.contexts[index].stage2_request_id != 0u);

    STRUCTURAL("wrap-fixture", fixture_init(&wrap_fixture));
    STRUCTURAL("wrap-stage1", start_stage1_at(&wrap_fixture, adva_e, 2u,
                                                TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                                0xfffffe80u, &wrap_index, &now_ms));
    STRUCTURAL("wrap-entry", entry_for(&wrap_fixture, adva_e) != NULL);
    if (structural_failures != 0u) return;
    entry_for(&wrap_fixture, adva_e)->hard_deadline_ms = 0xffffffe0u;
    memset(&stage1, 0, sizeof(stage1));
    CHECK("MAINT-06", phase5_rreq_owner_tick(&wrap_fixture.maintenance,
                                               &wrap_fixture.router, &wrap_fixture.link,
                                               &wrap_fixture.gtt, 0xffffffe0u,
                                               &stage1) == TAVRN_RREQ_VERIFICATION_OK);
    completion = completion_for(&stage1, TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE, 0x01u);
    CHECK("MAINT-07", phase5_rreq_terminal(&wrap_fixture.maintenance,
                                             &wrap_fixture.router, &wrap_fixture.link,
                                             &completion, 0xffffffe0u) ==
                          TAVRN_RREQ_VERIFICATION_OK);
    CHECK("AODV-04", phase5_rreq_owner_tick(&wrap_fixture.maintenance,
                                               &wrap_fixture.router, &wrap_fixture.link,
                                               &wrap_fixture.gtt, 0x0000001bu,
                                               &stage2) == TAVRN_RREQ_VERIFICATION_OK &&
                      phase5_rreq_snapshot(&wrap_fixture.maintenance,
                                           &wrap_fixture.router, &wrap_fixture.link,
                                           &wrap_fixture.gtt, &snapshot) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[wrap_index].stage ==
                          TAVRN_RREQ_VERIFICATION_STAGE1_WAIT_RESPONSE);
    CHECK("AODV-04", phase5_rreq_owner_tick(&wrap_fixture.maintenance,
                                               &wrap_fixture.router, &wrap_fixture.link,
                                               &wrap_fixture.gtt, 0x0000001cu,
                                               &stage2) == TAVRN_RREQ_VERIFICATION_OK &&
                      action_is(&stage2,
                                TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE2_FULL_DIAMETER,
                                15u, adva_e) &&
                      phase5_rreq_snapshot(&wrap_fixture.maintenance,
                                           &wrap_fixture.router, &wrap_fixture.link,
                                           &wrap_fixture.gtt, &snapshot) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[wrap_index].verification_deadline_ms == 0x00000076u);
}

static void test_rate_and_terminal_nonconsumption(void)
{
    verification_fixture_t fixture;
    tavrn_rreq_verification_action_t action;
    tavrn_rreq_verification_completion_t completion;
    tavrn_rreq_verification_snapshot_t snapshot;
    tron_application_data_t ordinary;
    aodv_action_t ordinary_action;
    uint8_t index;
    uint32_t now_ms;
    uint16_t retained_id;

    REACHED("rate-and-nonconsuming-terminal");
    STRUCTURAL("rate-fixture", fixture_init(&fixture));
    STRUCTURAL("rate-stage1", start_stage1(&fixture, adva_d, 3u,
                                             TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                             &index, &now_ms));
    if (structural_failures != 0u) return;
    fixture.aodv.config.rreq_rate_per_second = 1u;
    memset(&ordinary, 0, sizeof(ordinary));
    ordinary.final_destination.width = TAVRN_IDENTITY_SID8;
    ordinary.final_destination.value = adva_g[0];
    ordinary.app_kind = 0x7fu;
    CHECK("AODV-04", aodv_core_submit_application(&fixture.aodv, &ordinary, now_ms) ==
                          AODV_STATUS_QUEUED &&
                      aodv_core_poll_action(&fixture.aodv, &ordinary_action) ==
                          AODV_ACTION_POLL_OK && ordinary_action.type == AODV_ACTION_SEND_RREQ);
    memset(&action, 0, sizeof(action));
    CHECK("MAINT-06", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt, now_ms,
                                               &action) == TAVRN_RREQ_VERIFICATION_RATE_DEFERRED);
    CHECK("MAINT-07", phase5_rreq_snapshot(&fixture.maintenance, &fixture.router,
                                            &fixture.link, &fixture.gtt, &snapshot) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[index].stage ==
                          TAVRN_RREQ_VERIFICATION_STAGE1_READY &&
                      snapshot.contexts[index].stage1_request_id == 0u &&
                      snapshot.rreq_rate_deferrals == 1u);
    CHECK("AODV-04", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt, now_ms + 1u,
                                               &action) == TAVRN_RREQ_VERIFICATION_RATE_DEFERRED);
    memset(&action, 0, sizeof(action));
    CHECK("MAINT-06", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt, now_ms + 1000u,
                                               &action) == TAVRN_RREQ_VERIFICATION_OK &&
                      action_is(&action,
                                TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE1_RETAINED_HOP,
                                5u, adva_d));
    retained_id = action.attempt.request_id;
    completion = completion_for(&action, TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE, 0x01u);
    completion.token++;
    CHECK("MAINT-07", phase5_rreq_terminal(&fixture.maintenance, &fixture.router,
                                             &fixture.link, &completion, now_ms + 1000u) ==
                          TAVRN_RREQ_VERIFICATION_IGNORED);
    completion = completion_for(&action, TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE, 0x01u);
    completion.attempt.request_id++;
    CHECK("MAINT-07", phase5_rreq_terminal(&fixture.maintenance, &fixture.router,
                                             &fixture.link, &completion, now_ms + 1000u) ==
                          TAVRN_RREQ_VERIFICATION_IGNORED);
    completion = completion_for(&action, TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE, 0x01u);
    completion.purpose = TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE2_FULL_DIAMETER;
    CHECK("MAINT-07", phase5_rreq_terminal(&fixture.maintenance, &fixture.router,
                                             &fixture.link, &completion, now_ms + 1000u) ==
                          TAVRN_RREQ_VERIFICATION_IGNORED);
    completion = completion_for(&action, TAVRN_RREQ_VERIFICATION_TERMINAL_TX_FAILED, 0u);
    CHECK("MAINT-07", phase5_rreq_terminal(&fixture.maintenance, &fixture.router,
                                             &fixture.link, &completion, now_ms + 1000u) ==
                          TAVRN_RREQ_VERIFICATION_OK);
    memset(&action, 0, sizeof(action));
    CHECK("SERIAL-01", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                                &fixture.link, &fixture.gtt, now_ms + 1001u,
                                                &action) == TAVRN_RREQ_VERIFICATION_OK &&
                         action.attempt.request_id == retained_id);
    completion = completion_for(&action, TAVRN_RREQ_VERIFICATION_TERMINAL_EVICTED, 0u);
    CHECK("MAINT-07", phase5_rreq_terminal(&fixture.maintenance, &fixture.router,
                                             &fixture.link, &completion, now_ms + 1001u) ==
                          TAVRN_RREQ_VERIFICATION_OK);
    memset(&action, 0, sizeof(action));
    CHECK("SERIAL-01", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                                &fixture.link, &fixture.gtt, now_ms + 1002u,
                                                &action) == TAVRN_RREQ_VERIFICATION_OK &&
                         action.attempt.request_id == retained_id);
    completion = completion_for(&action,
                                TAVRN_RREQ_VERIFICATION_TERMINAL_LOCAL_NOT_ATTEMPTED, 0u);
    CHECK("MAINT-07", phase5_rreq_terminal(&fixture.maintenance, &fixture.router,
                                             &fixture.link, &completion, now_ms + 1002u) ==
                          TAVRN_RREQ_VERIFICATION_OK);
    CHECK("MAINT-07", phase5_rreq_snapshot(&fixture.maintenance, &fixture.router,
                                            &fixture.link, &fixture.gtt, &snapshot) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[index].stage ==
                          TAVRN_RREQ_VERIFICATION_STAGE1_READY &&
                      snapshot.contexts[index].stage1_request_id == retained_id);
}

static void test_evidence_direct_deadline_and_departure(void)
{
    verification_fixture_t fixture;
    verification_fixture_t evidence_fixture;
    tavrn_rreq_verification_action_t stage1;
    tavrn_rreq_verification_action_t stage2;
    tavrn_rreq_verification_action_t evidence_action;
    tavrn_rreq_verification_completion_t completion;
    tavrn_rreq_verification_snapshot_t snapshot;
    tavrn_adva_t subject = adva(adva_e);
    tavrn_gtt_evidence_t malformed;
    uint8_t index;
    uint8_t evidence_index;
    uint32_t now_ms;
    uint32_t evidence_now;
    uint32_t direct_deadline_ms;

    REACHED("evidence-direct-departure");
    STRUCTURAL("evidence-fixture", fixture_init(&evidence_fixture));
    STRUCTURAL("evidence-stage1", start_stage1(&evidence_fixture, adva_e, 1u,
                                                 TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                                                 &evidence_index, &evidence_now));
    if (structural_failures != 0u) return;
    memset(&evidence_action, 0, sizeof(evidence_action));
    CHECK("MAINT-06", phase5_rreq_owner_tick(&evidence_fixture.maintenance,
                                               &evidence_fixture.router,
                                               &evidence_fixture.link,
                                               &evidence_fixture.gtt, evidence_now,
                                               &evidence_action) == TAVRN_RREQ_VERIFICATION_OK);
    CHECK("MAINT-07", observe(&evidence_fixture, adva_f, 1u, 2u,
                                TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                evidence_now + 1u, NULL) &&
                      observe(&evidence_fixture, adva_e_alias, 1u, 2u,
                              TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                              evidence_now + 2u, NULL));
    CHECK("MAINT-07", !observe(&evidence_fixture, adva_e, 1u, 1u,
                                 TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                                 evidence_now + 3u, NULL));
    memset(&malformed, 0, sizeof(malformed));
    malformed.identity = adva(adva_e);
    malformed.serial = 3u;
    malformed.serial_present = 1u;
    malformed.hop_count = 0u;
    malformed.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    CHECK("MAINT-07", tavrn_gtt_observe_with_provenance(
                          &evidence_fixture.gtt, &malformed,
                          TAVRN_GTT_PROVENANCE_DIRECT_HELLO, evidence_now + 4u,
                          NULL) == TAVRN_GTT_EXPIRY_OBSERVE_INVALID);
    CHECK("MAINT-07", phase5_rreq_owner_tick(&evidence_fixture.maintenance,
                                               &evidence_fixture.router,
                                               &evidence_fixture.link,
                                               &evidence_fixture.gtt, evidence_now + 4u,
                                               &evidence_action) == TAVRN_RREQ_VERIFICATION_OK &&
                      phase5_rreq_snapshot(&evidence_fixture.maintenance,
                                           &evidence_fixture.router,
                                           &evidence_fixture.link, &evidence_fixture.gtt,
                                           &snapshot) == TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[evidence_index].valid != 0u);
    CHECK("MAINT-07", observe(&evidence_fixture, adva_e, 1u, 2u,
                                TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                                evidence_now + 5u, NULL));
    CHECK("MAINT-07", phase5_rreq_owner_tick(&evidence_fixture.maintenance,
                                               &evidence_fixture.router,
                                               &evidence_fixture.link,
                                               &evidence_fixture.gtt, evidence_now + 5u,
                                               &evidence_action) == TAVRN_RREQ_VERIFICATION_CANCELED &&
                      phase5_rreq_snapshot(&evidence_fixture.maintenance,
                                           &evidence_fixture.router,
                                           &evidence_fixture.link, &evidence_fixture.gtt,
                                           &snapshot) == TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[evidence_index].valid == 0u);

    STRUCTURAL("direct-fixture", fixture_init(&fixture));
    STRUCTURAL("direct-stage1", start_stage1(&fixture, adva_e, 1u,
                                               TAVRN_GTT_PROVENANCE_DIRECT_HELLO,
                                               &index, &now_ms));
    if (structural_failures != 0u) return;
    memset(&stage1, 0, sizeof(stage1));
    CHECK("MAINT-06", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt, now_ms,
                                               &stage1) == TAVRN_RREQ_VERIFICATION_OK);
    completion = completion_for(&stage1, TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE, 0x01u);
    CHECK("MAINT-07", phase5_rreq_terminal(&fixture.maintenance, &fixture.router,
                                             &fixture.link, &completion, now_ms) ==
                          TAVRN_RREQ_VERIFICATION_OK);
    memset(&stage2, 0, sizeof(stage2));
    CHECK("MAINT-06", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt,
                                               now_ms + PATH_DISCOVERY_MS, &stage2) ==
                          TAVRN_RREQ_VERIFICATION_OK);
    completion = completion_for(&stage2, TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE, 0x01u);
    CHECK("MAINT-07", phase5_rreq_terminal(&fixture.maintenance, &fixture.router,
                                             &fixture.link, &completion,
                                             now_ms + PATH_DISCOVERY_MS) ==
                          TAVRN_RREQ_VERIFICATION_OK);
    CHECK("AODV-04", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt,
                                               now_ms + 2u * PATH_DISCOVERY_MS, &stage2) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      phase5_rreq_snapshot(&fixture.maintenance, &fixture.router,
                                           &fixture.link, &fixture.gtt, &snapshot) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[index].stage ==
                          TAVRN_RREQ_VERIFICATION_WAIT_DIRECT_DEADLINE &&
                       snapshot.contexts[index].verification_deadline_ms ==
                           now_ms + VERIFICATION_WINDOW_MS &&
                       snapshot.contexts[index].direct_evidence_deadline_ms >
                           now_ms + 2u * PATH_DISCOVERY_MS);
    direct_deadline_ms = snapshot.contexts[index].direct_evidence_deadline_ms;
    CHECK("MAINT-07", observe(&fixture, adva_f, 1u, 2u,
                                TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                now_ms + 2u * PATH_DISCOVERY_MS + 1u, NULL));
    CHECK("MAINT-07", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt,
                                               now_ms + 2u * PATH_DISCOVERY_MS + 1u,
                                               &stage2) == TAVRN_RREQ_VERIFICATION_OK &&
                      phase5_rreq_snapshot(&fixture.maintenance, &fixture.router,
                                           &fixture.link, &fixture.gtt, &snapshot) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[index].valid != 0u);
    CHECK("MAINT-06", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt,
                                               direct_deadline_ms, &stage2) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      phase5_rreq_snapshot(&fixture.maintenance, &fixture.router,
                                           &fixture.link, &fixture.gtt, &snapshot) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[index].stage ==
                          TAVRN_RREQ_VERIFICATION_DEPARTURE_DEFERRED &&
                      snapshot.checked_departures == 0u);
    CHECK("MAINT-07", tavrn_gtt_application_cancel(&fixture.gtt,
                                                      TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                                                      &subject,
                                                      direct_deadline_ms + 1u,
                                                      NULL) == TAVRN_GTT_APPLICATION_CANCELED);
    CHECK("MAINT-06", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt,
                                               direct_deadline_ms + 1u,
                                               &stage2) == TAVRN_RREQ_VERIFICATION_OK &&
                      phase5_rreq_snapshot(&fixture.maintenance, &fixture.router,
                                           &fixture.link, &fixture.gtt, &snapshot) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                       snapshot.contexts[index].valid == 0u &&
                      snapshot.checked_departures == 1u &&
                      snapshot.expiry_tc_or_leave_created == 0u);
}

#if !defined(TAVRN_PHASE5_RREQ_VERIFICATION_RED_MODE)
static void test_failed_hop_adopts_cancels_and_reuses_context(void)
{
    verification_fixture_t fixture;
    tavrn_adva_t subject = adva(adva_d);
    tavrn_rreq_verification_action_t action;
    tavrn_rreq_verification_completion_t completion;
    tavrn_rreq_verification_snapshot_t snapshot;
    tavrn_maintenance_failed_hop_verification_snapshot_t pending;
    uint8_t index;
    uint32_t now_ms;

    STRUCTURAL("failed-hop-adopt-fixture", fixture_init(&fixture));
    STRUCTURAL("failed-hop-adopt-stage1", start_stage1(&fixture, adva_d, 2u,
                                                         TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                                         &index, &now_ms));
    if (structural_failures != 0u) return;
    CHECK("MAINT-04", tavrn_maintenance_schedule_failed_hop_verification(
                          &fixture.maintenance, &subject, now_ms) ==
                          TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED &&
                      tavrn_maintenance_failed_hop_verification_owner_tick(
                          &fixture.maintenance, now_ms) ==
                          TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_COALESCED &&
                       fixture.maintenance.targeted[index].verification_failed_hop != 0u &&
                      tavrn_maintenance_failed_hop_verification_snapshot(
                          &fixture.maintenance, &pending) ==
                          TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED &&
                      pending.pending != 0u);
    memset(&action, 0, sizeof(action));
    CHECK("MAINT-06", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                                &fixture.link, &fixture.gtt, now_ms,
                                                &action) == TAVRN_RREQ_VERIFICATION_OK);
    completion = completion_for(&action, TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE, 0x01u);
    CHECK("MAINT-07", phase5_rreq_terminal(&fixture.maintenance, &fixture.router,
                                              &fixture.link, &completion, now_ms) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      observe(&fixture, adva_d, 1u, 2u,
                              TAVRN_GTT_PROVENANCE_DIRECT_HELLO, now_ms + 1u, NULL) &&
                      phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt, now_ms + 1u,
                                               &action) == TAVRN_RREQ_VERIFICATION_CANCELED &&
                      tavrn_maintenance_failed_hop_verification_snapshot(
                          &fixture.maintenance, &pending) ==
                          TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_RETIRED_NOT_FOUND &&
                      pending.pending == 0u &&
                      phase5_rreq_snapshot(&fixture.maintenance, &fixture.router,
                                            &fixture.link, &fixture.gtt, &snapshot) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[index].valid == 0u &&
                      tavrn_maintenance_schedule_failed_hop_verification(
                          &fixture.maintenance, &subject, now_ms + 2u) ==
                          TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED);
}

/* An active failed-hop context owns H1 after it consumes the one pending slot.
 * If H2 then occupies that slot, every terminal path for H1 must leave H2's
 * demand untouched. */
static void test_failed_hop_retirement_is_subject_scoped(void)
{
    verification_fixture_t fresh_fixture;
    verification_fixture_t missing_fixture;
    verification_fixture_t departed_fixture;
    tavrn_adva_t h1 = adva(adva_d);
    tavrn_adva_t h2 = adva(adva_e);
    tavrn_rreq_verification_action_t action;
    tavrn_maintenance_failed_hop_verification_snapshot_t pending;
    tavrn_gtt_entry_t *entry;
    uint8_t index;
    uint32_t now_ms;

    STRUCTURAL("failed-hop-subject-fresh-fixture", fixture_init(&fresh_fixture));
    STRUCTURAL("failed-hop-subject-missing-fixture", fixture_init(&missing_fixture));
    STRUCTURAL("failed-hop-subject-departed-fixture", fixture_init(&departed_fixture));
    if (structural_failures != 0u) return;

    STRUCTURAL("failed-hop-subject-fresh-stage1",
               start_stage1(&fresh_fixture, adva_d, 2u,
                            TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, &index, &now_ms));
    STRUCTURAL("failed-hop-subject-fresh-h2",
               observe(&fresh_fixture, adva_e, 2u, 1u,
                       TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, now_ms, NULL));
    if (structural_failures != 0u) return;
    {
        tavrn_maintenance_failed_hop_verification_status_t first;
        tavrn_maintenance_failed_hop_verification_status_t adopted;
        tavrn_maintenance_failed_hop_verification_status_t second;
        tavrn_rreq_verification_status_t fresh;
        int evidence;

        first = tavrn_maintenance_schedule_failed_hop_verification(
            &fresh_fixture.maintenance, &h1, now_ms);
        adopted = tavrn_maintenance_failed_hop_verification_owner_tick(
            &fresh_fixture.maintenance, now_ms);
        second = tavrn_maintenance_schedule_failed_hop_verification(
            &fresh_fixture.maintenance, &h2, now_ms + 1u);
        evidence = observe(&fresh_fixture, adva_d, 1u, 2u,
                           TAVRN_GTT_PROVENANCE_DIRECT_HELLO, now_ms + 3u, NULL);
        fresh = phase5_rreq_owner_tick(&fresh_fixture.maintenance,
                                       &fresh_fixture.router, &fresh_fixture.link,
                                       &fresh_fixture.gtt, now_ms + 3u, &action);
        CHECK("MAINT-04", first == TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED &&
                           adopted ==
                               TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_COALESCED &&
                           second == TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED &&
                           evidence != 0 && fresh == TAVRN_RREQ_VERIFICATION_CANCELED &&
                           tavrn_maintenance_failed_hop_verification_snapshot(
                               &fresh_fixture.maintenance, &pending) ==
                               TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED &&
                           pending.pending != 0u &&
                           memcmp(pending.subject.bytes, h2.bytes, TAVRN_ADVA_LEN) == 0);
    }

    STRUCTURAL("failed-hop-subject-missing-stage1",
               start_stage1(&missing_fixture, adva_d, 2u,
                            TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, &index, &now_ms));
    STRUCTURAL("failed-hop-subject-missing-h2",
               observe(&missing_fixture, adva_e, 2u, 1u,
                       TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, now_ms, NULL));
    if (structural_failures != 0u) return;
    CHECK("MAINT-04", tavrn_maintenance_schedule_failed_hop_verification(
                           &missing_fixture.maintenance, &h1, now_ms) ==
                           TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED &&
                       tavrn_maintenance_failed_hop_verification_owner_tick(
                           &missing_fixture.maintenance, now_ms) ==
                           TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_COALESCED &&
                       tavrn_maintenance_schedule_failed_hop_verification(
                           &missing_fixture.maintenance, &h2, now_ms + 1u) ==
                           TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED &&
                       ((entry = entry_for(&missing_fixture, adva_d)) != NULL) &&
                       (memset(entry, 0, sizeof(*entry)), 1) &&
                       phase5_rreq_owner_tick(&missing_fixture.maintenance,
                                              &missing_fixture.router, &missing_fixture.link,
                                              &missing_fixture.gtt, now_ms + 2u, &action) ==
                           TAVRN_RREQ_VERIFICATION_OK &&
                       tavrn_maintenance_failed_hop_verification_snapshot(
                           &missing_fixture.maintenance, &pending) ==
                           TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED &&
                       pending.pending != 0u &&
                       memcmp(pending.subject.bytes, h2.bytes, TAVRN_ADVA_LEN) == 0);

    STRUCTURAL("failed-hop-subject-departed-stage1",
               start_stage1(&departed_fixture, adva_d, 2u,
                            TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, &index, &now_ms));
    STRUCTURAL("failed-hop-subject-departed-h2",
               observe(&departed_fixture, adva_e, 2u, 1u,
                       TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, now_ms, NULL));
    if (structural_failures != 0u) return;
    CHECK("MAINT-04", tavrn_maintenance_schedule_failed_hop_verification(
                           &departed_fixture.maintenance, &h1, now_ms) ==
                           TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED &&
                       tavrn_maintenance_failed_hop_verification_owner_tick(
                           &departed_fixture.maintenance, now_ms) ==
                           TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_COALESCED &&
                       tavrn_maintenance_schedule_failed_hop_verification(
                           &departed_fixture.maintenance, &h2, now_ms + 1u) ==
                           TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED &&
                       ((entry = entry_for(&departed_fixture, adva_d)) != NULL) &&
                       ((entry->departed = 1u) != 0u) &&
                       phase5_rreq_owner_tick(&departed_fixture.maintenance,
                                              &departed_fixture.router, &departed_fixture.link,
                                              &departed_fixture.gtt, now_ms + 2u, &action) ==
                           TAVRN_RREQ_VERIFICATION_OK &&
                       tavrn_maintenance_failed_hop_verification_snapshot(
                           &departed_fixture.maintenance, &pending) ==
                           TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED &&
                       pending.pending != 0u &&
                       memcmp(pending.subject.bytes, h2.bytes, TAVRN_ADVA_LEN) == 0);
}

static void test_failed_hop_adopts_existing_context_behind_distinct_pending(void)
{
    verification_fixture_t fixture;
    tavrn_adva_t h1 = adva(adva_d);
    tavrn_adva_t h2 = adva(adva_e);
    tavrn_maintenance_failed_hop_verification_snapshot_t pending;
    uint8_t index;
    uint32_t now_ms;

    STRUCTURAL("failed-hop-adopt-pending-fixture", fixture_init(&fixture));
    STRUCTURAL("failed-hop-adopt-pending-stage1", start_stage1(
                   &fixture, adva_d, 2u, TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                   &index, &now_ms));
    STRUCTURAL("failed-hop-adopt-pending-h2", observe(
                   &fixture, adva_e, 2u, 1u,
                   TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, now_ms, NULL));
    if (structural_failures != 0u) return;
    CHECK("MAINT-04", tavrn_maintenance_schedule_failed_hop_verification(
                           &fixture.maintenance, &h2, now_ms) ==
                           TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED &&
                       tavrn_maintenance_schedule_failed_hop_verification(
                           &fixture.maintenance, &h1, now_ms + 1u) ==
                           TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_COALESCED &&
                       fixture.maintenance.targeted[index].verification_failed_hop != 0u &&
                       tavrn_maintenance_failed_hop_verification_snapshot(
                           &fixture.maintenance, &pending) ==
                           TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED &&
                       pending.pending != 0u &&
                       memcmp(pending.subject.bytes, h2.bytes, TAVRN_ADVA_LEN) == 0);
}

static void test_admitted_rrep_cancels_verification_context(void)
{
    verification_fixture_t fixture;
    tavrn_rreq_verification_action_t action;
    tavrn_rreq_verification_completion_t completion;
    tavrn_rreq_verification_snapshot_t snapshot;
    tavrn_rx_control_event_t rrep;
    aodv_route_snapshot_t route;
    tavrn_gtt_entry_t *entry;
    tavrn_adva_t subject = adva(adva_c);
    tavrn_maintenance_failed_hop_verification_snapshot_t pending;
    uint8_t index;
    uint32_t now_ms;

    STRUCTURAL("rrep-fixture", fixture_init(&fixture));
    STRUCTURAL("rrep-stage1", start_stage1(&fixture, adva_c, 2u,
                                               TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                               &index, &now_ms));
    if (structural_failures != 0u) return;
    CHECK("MAINT-04", tavrn_maintenance_schedule_failed_hop_verification(
                          &fixture.maintenance, &subject, now_ms) ==
                          TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_SCHEDULED &&
                      fixture.maintenance.targeted[index].verification_failed_hop != 0u);
    memset(&action, 0, sizeof(action));
    CHECK("MAINT-06", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt, now_ms,
                                               &action) == TAVRN_RREQ_VERIFICATION_OK);
    completion = completion_for(&action, TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE, 0x01u);
    CHECK("MAINT-07", phase5_rreq_terminal(&fixture.maintenance, &fixture.router,
                                             &fixture.link, &completion, now_ms) ==
                          TAVRN_RREQ_VERIFICATION_OK);
    rrep = rrep_for(&action, adva_b);
    rrep.control.pdu[12]++;
    CHECK("MAINT-07", tavrn_maintenance_verification_receive_rrep(
                          &fixture.maintenance, &fixture.router, &fixture.link,
                          &fixture.gtt, &rrep, now_ms + 1u) ==
                          TAVRN_RREQ_VERIFICATION_IGNORED);
    rrep = rrep_for(&action, adva_b);
    rrep.control.pdu[8] = adva_d[0];
    CHECK("MAINT-07", tavrn_maintenance_verification_receive_rrep(
                          &fixture.maintenance, &fixture.router, &fixture.link,
                          &fixture.gtt, &rrep, now_ms + 1u) ==
                          TAVRN_RREQ_VERIFICATION_IGNORED);
    rrep = rrep_for(&action, adva_b);
    rrep.control.pdu[11] = adva_b[0];
    CHECK("MAINT-07", tavrn_maintenance_verification_receive_rrep(
                          &fixture.maintenance, &fixture.router, &fixture.link,
                          &fixture.gtt, &rrep, now_ms + 1u) ==
                          TAVRN_RREQ_VERIFICATION_IGNORED);
    rrep = rrep_for(&action, adva_b);
    CHECK("MAINT-07", tavrn_maintenance_verification_receive_rrep(
                          &fixture.maintenance, &fixture.router, &fixture.link,
                          &fixture.gtt, &rrep, now_ms + 1u) ==
                          TAVRN_RREQ_VERIFICATION_CANCELED);
    entry = entry_for(&fixture, adva_c);
    CHECK("AODV-04", tavrn_router_route_to_subject(&fixture.router,
                                                      &action.attempt.destination,
                                                      now_ms + 1u, &route) ==
                          TAVRN_ROUTER_ROUTE_OK &&
                      route.next_hop.logical_id.value == adva_b[0] && entry != NULL &&
                      entry->hard_deadline_ms != now_ms);
    CHECK("MAINT-06", phase5_rreq_snapshot(&fixture.maintenance, &fixture.router,
                                               &fixture.link, &fixture.gtt, &snapshot) ==
                           TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[index].valid == 0u &&
                      tavrn_maintenance_failed_hop_verification_snapshot(
                          &fixture.maintenance, &pending) ==
                          TAVRN_MAINTENANCE_FAILED_HOP_VERIFICATION_RETIRED_NOT_FOUND &&
                      pending.pending == 0u);
}

static void test_completed_contexts_retire_and_reuse_capacity(void)
{
    verification_fixture_t fixture;
    const uint8_t *subjects[] = { adva_b, adva_c, adva_d, adva_e };
    tavrn_rreq_verification_action_t action;
    tavrn_rreq_verification_snapshot_t snapshot;
    uint8_t index;
    uint8_t context_index;
    uint32_t now_ms;

    STRUCTURAL("completed-context-reuse-fixture", fixture_init(&fixture));
    if (structural_failures != 0u) return;
    for (index = 0u; index < 4u; index++) {
        STRUCTURAL("completed-context-reuse-stage1",
                   start_stage1(&fixture, subjects[index], 2u,
                                TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                &context_index, &now_ms));
        if (structural_failures != 0u) return;
        memset(&action, 0, sizeof(action));
        CHECK("MAINT-06", phase5_rreq_owner_tick(
                               &fixture.maintenance, &fixture.router, &fixture.link,
                               &fixture.gtt, now_ms, &action) ==
                               TAVRN_RREQ_VERIFICATION_OK &&
                           action.enqueued != 0u &&
                           phase5_rreq_terminal(&fixture.maintenance,
                                                &fixture.router, &fixture.link,
                                                &(tavrn_rreq_verification_completion_t){
                                                    action.attempt, action.token,
                                                    action.purpose,
                                                    TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE,
                                                    0x01u
                                                }, now_ms) == TAVRN_RREQ_VERIFICATION_OK);
        memset(&action, 0, sizeof(action));
        CHECK("MAINT-06", phase5_rreq_owner_tick(
                               &fixture.maintenance, &fixture.router, &fixture.link,
                               &fixture.gtt, now_ms + PATH_DISCOVERY_MS, &action) ==
                               TAVRN_RREQ_VERIFICATION_OK &&
                           action.enqueued != 0u &&
                           phase5_rreq_terminal(&fixture.maintenance,
                                                &fixture.router, &fixture.link,
                                                &(tavrn_rreq_verification_completion_t){
                                                    action.attempt, action.token,
                                                    action.purpose,
                                                    TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE,
                                                    0x01u
                                                }, now_ms + PATH_DISCOVERY_MS) ==
                               TAVRN_RREQ_VERIFICATION_OK &&
                           phase5_rreq_owner_tick(
                               &fixture.maintenance, &fixture.router, &fixture.link,
                               &fixture.gtt, now_ms + 2u * PATH_DISCOVERY_MS,
                               &action) == TAVRN_RREQ_VERIFICATION_OK &&
                           tavrn_gtt_application_cancel(
                               &fixture.gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
                               &(tavrn_adva_t){ { subjects[index][0], subjects[index][1],
                                                  subjects[index][2], subjects[index][3],
                                                  subjects[index][4], subjects[index][5] } },
                               now_ms + 2u * PATH_DISCOVERY_MS + 1u, NULL) ==
                               TAVRN_GTT_APPLICATION_CANCELED &&
                           phase5_rreq_owner_tick(
                               &fixture.maintenance, &fixture.router, &fixture.link,
                               &fixture.gtt, now_ms + 2u * PATH_DISCOVERY_MS + 1u,
                               &action) == TAVRN_RREQ_VERIFICATION_OK);
    }
    CHECK("MAINT-07", phase5_rreq_snapshot(&fixture.maintenance, &fixture.router,
                                             &fixture.link, &fixture.gtt, &snapshot) ==
                           TAVRN_RREQ_VERIFICATION_OK &&
                       snapshot.active_contexts == 0u &&
                       start_stage1(&fixture, adva_f, 2u,
                                    TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                    &context_index, &now_ms));
}

#endif

static void test_capacity_due_order_and_runtime_boundary(void)
{
    verification_fixture_t fixture;
    verification_fixture_t queue_fixture;
    tavrn_rreq_verification_action_t action;
    tavrn_rreq_verification_snapshot_t snapshot;
    tavrn_rreq_verification_completion_t malformed_completion;
    tavrn_router_t detached_router;
    const uint8_t *subjects[] = { adva_b, adva_c, adva_d, adva_e };
    uint8_t index;
    uint8_t context_index;
    uint32_t now_ms = 0u;

    REACHED("capacity-due-order-runtime-boundary");
    STRUCTURAL("capacity-fixture", fixture_init(&fixture));
    for (index = 0u; index < 4u; index++) {
        uint32_t expiry;

        STRUCTURAL("capacity-stage1", start_stage1(&fixture, subjects[index],
                                                      (uint8_t)(index + 1u),
                                                      TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                                      &context_index, &expiry));
        now_ms = expiry;
    }
    if (structural_failures != 0u) return;
    CHECK("MAINT-07", !start_stage1(&fixture, adva_f, 1u,
                                     TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                     &context_index, &now_ms));
    CHECK("MAINT-07", phase5_rreq_snapshot(&fixture.maintenance, &fixture.router,
                                            &fixture.link, &fixture.gtt, &snapshot) ==
                          TAVRN_RREQ_VERIFICATION_OK && snapshot.active_contexts == 4u);
    for (index = 0u; index < 4u; index++) {
        memset(&action, 0, sizeof(action));
        CHECK("MAINT-06", phase5_rreq_owner_tick(&fixture.maintenance, &fixture.router,
                                                   &fixture.link, &fixture.gtt, now_ms,
                                                   &action) == TAVRN_RREQ_VERIFICATION_OK);
        CHECK("AODV-02", memcmp(action.subject.bytes, subjects[index],
                                 TAVRN_ADVA_LEN) == 0);
    }

    STRUCTURAL("queue-fixture", fixture_init(&queue_fixture));
    STRUCTURAL("queue-stage1", start_stage1(&queue_fixture, adva_g, 2u,
                                               TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE,
                                               &context_index, &now_ms));
    STRUCTURAL("queue-fill", fill_control_queue(&queue_fixture));
    if (structural_failures != 0u) return;
    memset(&action, 0, sizeof(action));
    CHECK("MAINT-07", phase5_rreq_owner_tick(&queue_fixture.maintenance,
                                               &queue_fixture.router, &queue_fixture.link,
                                               &queue_fixture.gtt, now_ms, &action) ==
                          TAVRN_RREQ_VERIFICATION_BUSY);
    CHECK("AODV-04", phase5_rreq_snapshot(&queue_fixture.maintenance,
                                            &queue_fixture.router, &queue_fixture.link,
                                            &queue_fixture.gtt, &snapshot) ==
                          TAVRN_RREQ_VERIFICATION_OK &&
                      snapshot.contexts[context_index].stage ==
                          TAVRN_RREQ_VERIFICATION_STAGE1_READY &&
                      snapshot.contexts[context_index].stage1_request_id == 0u);
    clear_queue(&queue_fixture);
    CHECK("MAINT-07", phase5_rreq_owner_tick(NULL, &queue_fixture.router,
                                               &queue_fixture.link, &queue_fixture.gtt,
                                               now_ms, &action) ==
                          TAVRN_RREQ_VERIFICATION_INVALID);
    CHECK("MAINT-07", phase5_rreq_terminal(&queue_fixture.maintenance,
                                             &queue_fixture.router, &queue_fixture.link,
                                             NULL, now_ms) ==
                          TAVRN_RREQ_VERIFICATION_INVALID);
    memset(&action, 0, sizeof(action));
    malformed_completion = completion_for(&action,
                                          TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE, 0u);
    CHECK("MAINT-07", phase5_rreq_terminal(&queue_fixture.maintenance,
                                             &queue_fixture.router, &queue_fixture.link,
                                             &malformed_completion, now_ms) ==
                          TAVRN_RREQ_VERIFICATION_INVALID);
    detached_router = queue_fixture.router;
    queue_fixture.maintenance.router = &detached_router;
    CHECK("MAINT-07", phase5_rreq_owner_tick(&queue_fixture.maintenance,
                                               &queue_fixture.router, &queue_fixture.link,
                                               &queue_fixture.gtt, now_ms, &action) ==
                          TAVRN_RREQ_VERIFICATION_INVALID);
    queue_fixture.maintenance.router = &queue_fixture.router;
}

int main(void)
{
    test_stage1_scope_stage2_fresh_id();
    test_rate_and_terminal_nonconsumption();
    test_evidence_direct_deadline_and_departure();
#if !defined(TAVRN_PHASE5_RREQ_VERIFICATION_RED_MODE)
    test_failed_hop_adopts_cancels_and_reuses_context();
    test_failed_hop_retirement_is_subject_scoped();
    test_failed_hop_adopts_existing_context_behind_distinct_pending();
    test_admitted_rrep_cancels_verification_context();
    test_completed_contexts_retire_and_reuse_capacity();
#endif
    test_capacity_due_order_and_runtime_boundary();
    if (structural_failures != 0u) {
        printf("tavrn_phase5_rreq_verification structural failures: %u\n", structural_failures);
        return 2;
    }
    if (failures != 0u) {
        printf("tavrn_phase5_rreq_verification tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_phase5_rreq_verification tests passed\n");
    return 0;
}
