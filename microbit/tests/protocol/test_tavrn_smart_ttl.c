#include "aodv_core.h"
#include "tavrn_smart_ttl.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;
static const char *reported[8];
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
static const uint8_t adva_c[TAVRN_ADVA_LEN] = {
    0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0xc1u,
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

static aodv_core_config_t make_core_config(void)
{
    aodv_core_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_peer(adva_a);
    config.network_id = 0x2au;
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

static tron_application_data_t make_application(void)
{
    tron_application_data_t data;

    memset(&data, 0, sizeof(data));
    data.final_destination = make_peer(adva_c).logical_id;
    data.app_kind = 0x7fu;
    data.app_len = 1u;
    data.app_bytes[0] = 0xa5u;
    return data;
}

static uint16_t pdu_u16(const tavrn_validated_control_t *control, uint8_t offset)
{
    return (uint16_t)control->pdu[offset] |
        ((uint16_t)control->pdu[(uint8_t)(offset + 1u)] << 8);
}

static tavrn_gtt_snapshot_t make_snapshot(tavrn_gtt_freshness_t freshness,
                                          uint8_t hop_count)
{
    tavrn_gtt_snapshot_t snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.identity.bytes[0] = 0x11u;
    snapshot.freshness = freshness;
    snapshot.hop_count = hop_count;
    snapshot.serial_present = 1u;
    snapshot.serial = 1u;
    return snapshot;
}

static void test_gtt_06_pure_decisions(void)
{
    tavrn_gtt_snapshot_t snapshot = make_snapshot(TAVRN_GTT_FRESHNESS_ACTIVE, 2u);
    tavrn_gtt_snapshot_t before;
    tavrn_smart_ttl_decision_t decision;

    before = snapshot;
    decision = tavrn_smart_ttl_decide(&snapshot, 15u);
    CHECK("GTT-06", decision.has_hint != 0u && decision.initial_scope == 4u &&
                        memcmp(&snapshot, &before, sizeof(snapshot)) == 0);
    snapshot.freshness = TAVRN_GTT_FRESHNESS_SOFT_STALE;
    decision = tavrn_smart_ttl_decide(&snapshot, 15u);
    CHECK("GTT-06", decision.has_hint != 0u && decision.initial_scope == 4u);
    snapshot.freshness = TAVRN_GTT_FRESHNESS_HARD_EXPIRED;
    decision = tavrn_smart_ttl_decide(&snapshot, 15u);
    CHECK("GTT-06", decision.has_hint == 0u);
    snapshot.freshness = TAVRN_GTT_FRESHNESS_DEPARTED;
    decision = tavrn_smart_ttl_decide(&snapshot, 15u);
    CHECK("GTT-06", decision.has_hint == 0u);
    snapshot = make_snapshot(TAVRN_GTT_FRESHNESS_ACTIVE, 0u);
    decision = tavrn_smart_ttl_decide(&snapshot, 15u);
    CHECK("GTT-06", decision.has_hint == 0u);
    decision = tavrn_smart_ttl_decide(NULL, 15u);
    CHECK("GTT-06", decision.has_hint == 0u);
    snapshot = make_snapshot(TAVRN_GTT_FRESHNESS_ACTIVE, 14u);
    decision = tavrn_smart_ttl_decide(&snapshot, 15u);
    CHECK("GTT-06", decision.has_hint != 0u && decision.initial_scope == 15u);
}

static void test_gtt_06_scoped_core_fallback(void)
{
    aodv_core_t core;
    aodv_core_config_t config = make_core_config();
    tron_application_data_t application = make_application();
    aodv_action_t action;
    uint16_t first_request = 0u;

    memset(&action, 0, sizeof(action));
    CHECK("GTT-06", aodv_core_init(&core, &config, 0u) == AODV_INIT_OK &&
                        aodv_core_submit_application_scoped(
                            &core, &application, 5u, 0u) == AODV_STATUS_QUEUED);
    CHECK("GTT-06", aodv_core_poll_action(&core, &action) ==
                        AODV_ACTION_POLL_OK &&
                        action.type == AODV_ACTION_SEND_RREQ &&
                        (action.detail.control.control.pdu[6] >> 4) == 5u);
    first_request = pdu_u16(&action.detail.control.control, 9u);
    CHECK("GTT-06", aodv_core_tick(&core, 600u) == AODV_STATUS_OK &&
                        aodv_core_poll_action(&core, &action) ==
                            AODV_ACTION_POLL_OK &&
                        action.type == AODV_ACTION_SEND_RREQ &&
                        (action.detail.control.control.pdu[6] >> 4) == 15u &&
                        pdu_u16(&action.detail.control.control, 9u) != first_request);

    memset(&action, 0, sizeof(action));
    first_request = 0u;
    CHECK("GTT-06", aodv_core_init(&core, &config, 0u) == AODV_INIT_OK &&
                        aodv_core_submit_application_scoped(
                            &core, &application, 15u, 0u) == AODV_STATUS_QUEUED);
    CHECK("GTT-06", aodv_core_poll_action(&core, &action) ==
                        AODV_ACTION_POLL_OK &&
                        action.type == AODV_ACTION_SEND_RREQ &&
                        (action.detail.control.control.pdu[6] >> 4) == 15u);
    first_request = pdu_u16(&action.detail.control.control, 9u);
    CHECK("GTT-06", aodv_core_tick(&core, 600u) == AODV_STATUS_OK &&
                        aodv_core_poll_action(&core, &action) ==
                            AODV_ACTION_POLL_OK &&
                        action.type == AODV_ACTION_SEND_RREQ &&
                        (action.detail.control.control.pdu[6] >> 4) == 15u &&
                        pdu_u16(&action.detail.control.control, 9u) != first_request);
}

static void test_aodv_only_no_hint_characterization(void)
{
    aodv_core_t core;
    aodv_core_config_t config = make_core_config();
    tron_application_data_t application = make_application();
    aodv_action_t action;
    uint8_t expected_ttl[] = { 1u, 3u, 5u, 7u, 15u };
    uint16_t previous_request = 0u;
    uint8_t i;

    CHECK("AODV-04", aodv_core_init(&core, &config, 0u) == AODV_INIT_OK &&
                        aodv_core_submit_application(&core, &application, 0u) ==
                            AODV_STATUS_QUEUED);
    for (i = 0u; i < sizeof(expected_ttl); i++) {
        CHECK("AODV-04", aodv_core_poll_action(&core, &action) ==
                            AODV_ACTION_POLL_OK &&
                        action.type == AODV_ACTION_SEND_RREQ &&
                        (action.detail.control.control.pdu[6] >> 4) == expected_ttl[i] &&
                        (i == 0u || pdu_u16(&action.detail.control.control, 9u) !=
                             previous_request));
        previous_request = pdu_u16(&action.detail.control.control, 9u);
        CHECK("AODV-04", aodv_core_tick(&core, (uint32_t)(i + 1u) * 600u) ==
                            AODV_STATUS_OK);
    }
}

int main(void)
{
    test_gtt_06_pure_decisions();
    test_gtt_06_scoped_core_fallback();
    test_aodv_only_no_hint_characterization();
    if (failures != 0u) {
        printf("tavrn_smart_ttl RED tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_smart_ttl tests passed\n");
    return 0;
}
