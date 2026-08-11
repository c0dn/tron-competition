#include "tavrn_full_repair_binding.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PATH_DISCOVERY_MS 600u
#define REPAIR_TIMEOUT_MS 1700u
#define REPAIR_COOLDOWN_MS 1000u

static unsigned int failures;

#define CHECK(expression) \
    do { \
        if (!(expression)) { \
            printf("FAIL full-repair-binding: %s:%d: %s\n", __FILE__, __LINE__, \
                   #expression); \
            failures++; \
        } \
    } while (0)

static const uint8_t adva_a[TAVRN_ADVA_LEN] =
    { 0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu };
static const uint8_t adva_b[TAVRN_ADVA_LEN] =
    { 0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u };
static const uint8_t adva_c[TAVRN_ADVA_LEN] =
    { 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0xc1u };
static const uint8_t adva_d[TAVRN_ADVA_LEN] =
    { 0x12u, 0x23u, 0x34u, 0x45u, 0x56u, 0xc2u };

typedef struct repair_fixture {
    ble_mesh_scheduler_t scheduler;
    tavrn_link_v2_t link;
    aodv_core_t aodv;
    tavrn_gtt_storage_t gtt_storage;
    tavrn_gtt_t gtt;
    tavrn_router_t router;
    tavrn_maintenance_t maintenance;
    tavrn_repair_t repair;
    tavrn_full_repair_binding_t binding;
} repair_fixture_t;

static tavrn_direct_peer_t peer(const uint8_t bytes[TAVRN_ADVA_LEN])
{
    tavrn_direct_peer_t result;

    memset(&result, 0, sizeof(result));
    result.logical_id.width = TAVRN_IDENTITY_SID8;
    result.logical_id.value = bytes[0];
    memcpy(result.adva.bytes, bytes, TAVRN_ADVA_LEN);
    return result;
}

static int no_identity_conflict(void *context, const tavrn_logical_id_t *logical_id,
                                const tavrn_adva_t *direct_adva_or_null)
{
    (void)context;
    (void)logical_id;
    (void)direct_adva_or_null;
    return 0;
}

static tavrn_link_config_t link_config(void)
{
    tavrn_link_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = peer(adva_a);
    config.network_id = 0x2au;
    config.hack_max_attempts = 3u;
    config.busy_max_responses = 3u;
    config.hack_response_ms = 250u;
    config.hack_turnaround_ms = 8u;
    config.retry_backoff_ms = 1u;
    config.busy_backoff_ms = 500u;
    config.data_forward_deadline_ms = 5000u;
    config.candidate_resolve_ms = 10u;
    config.data_dedupe_ms = 10000u;
    config.flood_dedupe_ms = 10000u;
    config.flood_jitter_min_ms = 20u;
    config.flood_jitter_max_ms = 120u;
    config.identity_conflict = no_identity_conflict;
    return config;
}

static aodv_core_config_t aodv_config(void)
{
    aodv_core_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = peer(adva_a);
    config.network_id = 0x2au;
    config.net_diameter = 15u;
    config.rreq_retries = 2u;
    config.rreq_rate_per_second = 10u;
    config.rerr_rate_per_second = 10u;
    config.request_rrep_ack = 0u;
    config.initial_origin_sequence = 1u;
    config.initial_request_id = 1u;
    config.initial_rerr_sequence = 1u;
    config.node_traversal_ms = 10u;
    config.path_discovery_ms = PATH_DISCOVERY_MS;
    config.rreq_seen_ms = 10000u;
    config.active_route_ms = 36000u;
    config.pending_data_ms = 5000u;
    config.blacklist_ms = 600u;
    config.rrep_dedupe_ms = 10000u;
    config.rerr_dedupe_ms = 10000u;
    config.rrep_ack_wait_ms = 250u;
    return config;
}

static tavrn_maintenance_config_t maintenance_config(void)
{
    tavrn_maintenance_config_t config;

    memset(&config, 0, sizeof(config));
    config.hello_change_ms = 10u;
    config.hello_dedupe_ms = 100u;
    config.initial_node_sequence = 1u;
    config.aodv_net_traversal_ms = 30u;
    config.freshness_response_min_ms = 10u;
    config.freshness_response_max_ms = 100u;
    config.metadata_cooldown_ms = 5u;
    config.tc_uuid_ms = 30u;
    config.tc_subject_ms = 5u;
    return config;
}

static int fixture_init(repair_fixture_t *fixture)
{
    tavrn_gtt_config_t gtt_config;
    tavrn_router_incarnation_config_t incarnation;
    tavrn_full_repair_binding_config_t repair_config;
    tavrn_link_config_t configured_link = link_config();
    aodv_core_config_t configured_aodv = aodv_config();
    tavrn_maintenance_config_t configured_maintenance = maintenance_config();

    memset(fixture, 0, sizeof(*fixture));
    memset(&gtt_config, 0, sizeof(gtt_config));
    gtt_config.local_identity.bytes[0] = adva_a[0];
    gtt_config.local_identity.bytes[1] = adva_a[1];
    gtt_config.local_identity.bytes[2] = adva_a[2];
    gtt_config.local_identity.bytes[3] = adva_a[3];
    gtt_config.local_identity.bytes[4] = adva_a[4];
    gtt_config.local_identity.bytes[5] = adva_a[5];
    gtt_config.soft_expiry_ms = 10u;
    gtt_config.hard_expiry_ms = 300000u;
    gtt_config.departed_retention_ms = 600000u;
    memset(&incarnation, 0, sizeof(incarnation));
    incarnation.feature_level = TAVRN_ROUTER_FEATURE_FULL_TAVRN;
    incarnation.boot_nonce = 0x5a01u;
    incarnation.reboot_announce_ms = 30u;
    ble_mesh_scheduler_init(&fixture->scheduler, 0u, adva_a);
    if (tavrn_link_v2_init(&fixture->link, &fixture->scheduler, &configured_link, 0u) !=
            TAVRN_LINK_INIT_OK ||
        aodv_core_init(&fixture->aodv, &configured_aodv, 0u) != AODV_INIT_OK ||
        tavrn_gtt_init(&fixture->gtt, &fixture->gtt_storage, &gtt_config, 0u) !=
            TAVRN_GTT_INIT_OK ||
        tavrn_router_init_with_incarnation(&fixture->router, &fixture->link,
                                            &fixture->aodv, NULL, &incarnation, 0u) !=
            TAVRN_ROUTER_INCARNATION_OK ||
        tavrn_router_complete_full_bootstrap(&fixture->router, 1u) !=
            TAVRN_ROUTER_INCARNATION_OK ||
        tavrn_maintenance_init(&fixture->maintenance, &fixture->router, &fixture->gtt,
                               &configured_maintenance) != TAVRN_MAINTENANCE_OK) {
        return 0;
    }
    memset(&repair_config, 0, sizeof(repair_config));
    repair_config.repair = &fixture->repair;
    repair_config.router = &fixture->router;
    repair_config.link = &fixture->link;
    repair_config.aodv = &fixture->aodv;
    repair_config.maintenance = &fixture->maintenance;
    repair_config.gtt = &fixture->gtt;
    repair_config.net_diameter = 15u;
    repair_config.path_discovery_ms = PATH_DISCOVERY_MS;
    repair_config.repair_timeout_ms = REPAIR_TIMEOUT_MS;
    repair_config.repair_cooldown_ms = REPAIR_COOLDOWN_MS;
    return tavrn_full_repair_binding_init(&fixture->binding, &repair_config) ==
        TAVRN_FULL_REPAIR_BINDING_OK;
}

static tavrn_link_data_t transit_data(uint16_t sequence)
{
    tavrn_link_data_t data;

    memset(&data, 0, sizeof(data));
    data.origin = peer(adva_b).logical_id;
    data.final_destination = peer(adva_c).logical_id;
    data.data_seq = sequence;
    data.ttl = 7u;
    data.hops = 2u;
    data.app_kind = 0x71u;
    data.app_source = 0x31u;
    data.app_len = 3u;
    data.app_bytes[0] = 0xa1u;
    data.app_bytes[1] = 0xb2u;
    data.app_bytes[2] = 0xc3u;
    data.ownership = TAVRN_DATA_TRANSIT;
    return data;
}

static tavrn_adva_t generated_adva(uint8_t suffix)
{
    tavrn_adva_t value;

    memset(&value, 0, sizeof(value));
    value.bytes[0] = suffix;
    value.bytes[1] = 0xa5u;
    value.bytes[2] = 0x5au;
    value.bytes[3] = 0x3cu;
    value.bytes[4] = 0xc3u;
    value.bytes[5] = 0xc5u;
    return value;
}

static void pin_transit_data(tavrn_link_v2_t *link, const tavrn_link_data_t *data)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_LINK_DATA_DEDUPE_CAPACITY; index++) {
        tavrn_data_dedupe_entry_t *entry = &link->data_dedupe[index];

        if (entry->valid == 0u) {
            memset(entry, 0, sizeof(*entry));
            entry->valid = 1u;
            entry->custody_pinned = 1u;
            entry->origin = data->origin;
            entry->final_destination = data->final_destination;
            entry->data_seq = data->data_seq;
            entry->app_kind = data->app_kind;
            entry->app_source = data->app_source;
            entry->expires_at_ms = 10000u;
            return;
        }
    }
}

static int transit_data_is_pinned(const tavrn_link_v2_t *link,
                                  const tavrn_link_data_t *data)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_LINK_DATA_DEDUPE_CAPACITY; index++) {
        const tavrn_data_dedupe_entry_t *entry = &link->data_dedupe[index];

        if (entry->valid != 0u && entry->origin.width == data->origin.width &&
            entry->origin.value == data->origin.value &&
            entry->final_destination.width == data->final_destination.width &&
            entry->final_destination.value == data->final_destination.value &&
            entry->data_seq == data->data_seq && entry->app_kind == data->app_kind &&
            entry->app_source == data->app_source) {
            return entry->custody_pinned != 0u;
        }
    }
    return 0;
}

static int transit_data_has_external_custody_owner(const tavrn_link_v2_t *link,
                                                   const tavrn_link_data_t *data)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_LINK_DATA_DEDUPE_CAPACITY; index++) {
        const tavrn_data_dedupe_entry_t *entry = &link->data_dedupe[index];

        if (entry->valid != 0u && entry->origin.width == data->origin.width &&
            entry->origin.value == data->origin.value &&
            entry->final_destination.width == data->final_destination.width &&
            entry->final_destination.value == data->final_destination.value &&
            entry->data_seq == data->data_seq && entry->app_kind == data->app_kind &&
            entry->app_source == data->app_source) {
            return entry->external_custody_owner != 0u;
        }
    }
    return 0;
}

static int transit_data_is_marked_for_incarnation_clear(
    const tavrn_link_v2_t *link, const tavrn_link_data_t *data)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_LINK_DATA_DEDUPE_CAPACITY; index++) {
        const tavrn_data_dedupe_entry_t *entry = &link->data_dedupe[index];

        if (entry->valid != 0u && entry->origin.width == data->origin.width &&
            entry->origin.value == data->origin.value &&
            entry->final_destination.width == data->final_destination.width &&
            entry->final_destination.value == data->final_destination.value &&
            entry->data_seq == data->data_seq && entry->app_kind == data->app_kind &&
            entry->app_source == data->app_source) {
            return entry->incarnation_clear_on_release != 0u;
        }
    }
    return 0;
}

static tavrn_owned_data_event_t owned(const tavrn_link_data_t *data)
{
    tavrn_owned_data_event_t event;

    memset(&event, 0, sizeof(event));
    event.next_hop = peer(adva_b);
    event.data = *data;
    event.requested_channel_mask = BLE_RADIO_ADV_CH_ALL;
    event.completed_channel_mask = BLE_RADIO_ADV_CH_ALL;
    event.attempt_count = 3u;
    event.first_tx_ms = 1u;
    event.last_tx_ms = 2u;
    event.final_deadline_ms = 3u;
    return event;
}

static int add_same_destination_custody(repair_fixture_t *fixture,
                                         const tavrn_link_data_t *data)
{
    tavrn_link_event_t outcome;
    tavrn_direct_peer_t next_hop = peer(adva_b);

    memset(&outcome, 0, sizeof(outcome));
    pin_transit_data(&fixture->link, data);
    return tavrn_link_v2_send_unicast(&fixture->link, &next_hop, data, 1u,
                                      &outcome) == TAVRN_LINK_SEND_OK &&
        outcome.type == TAVRN_LINK_EVENT_NONE;
}

/* Synthetic terminal events model link-v2 after it has reset the exact
 * physical custody slot. */
static int clear_link_custody_for_terminal(tavrn_link_v2_t *link,
                                           const tavrn_link_data_t *data)
{
    uint8_t index;

    if (link == NULL || data == NULL) {
        return 0;
    }
    for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
        tavrn_custody_slot_t *slot = &link->custody[index];

        if (slot->phase != TAVRN_CUSTODY_FREE &&
            memcmp(&slot->data, data, sizeof(*data)) == 0) {
            memset(slot, 0, sizeof(*slot));
            if (link->active_custody_index == index) {
                link->active_custody_index = TAVRN_CUSTODY_SLOT_NONE;
            }
            return 1;
        }
    }
    return 0;
}

static int start_repair(repair_fixture_t *fixture, const tavrn_link_data_t *primary,
                        const tavrn_link_data_t *reserved_or_null,
                        tavrn_repair_snapshot_t *snapshot_out)
{
    tavrn_link_event_t event;

    if (reserved_or_null != NULL &&
        !add_same_destination_custody(fixture, reserved_or_null)) {
        return 0;
    }
    pin_transit_data(&fixture->link, primary);
    memset(&event, 0, sizeof(event));
    event.type = TAVRN_LINK_EVENT_RETRY_EXHAUSTED;
    event.detail.owned_data = owned(primary);
    return tavrn_router_handle_link_event(&fixture->router, &event, 2u) ==
               TAVRN_ROUTER_EVENT_OK &&
        transit_data_has_external_custody_owner(&fixture->link, primary) &&
        tavrn_repair_snapshot(&fixture->repair, snapshot_out) == TAVRN_REPAIR_OK;
}

static tavrn_link_event_t terminal_event(tavrn_link_event_type_t type,
                                         const tavrn_link_data_t *data)
{
    tavrn_link_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = type;
    if (type == TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED) {
        event.detail.transferred_data.next_hop = peer(adva_b);
        event.detail.transferred_data.data = *data;
        event.detail.transferred_data.status = TAVRN_HACK_ACCEPTED;
    } else {
        event.detail.owned_data = owned(data);
    }
    return event;
}

static int remove_queued_token(ble_mesh_scheduler_t *scheduler, uint16_t token)
{
    uint8_t index;

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_queue_entry_t *entry = &scheduler->routed_tx_queue.entries[index];

        if (entry->occupied != 0u && entry->item.token == token) {
            return ble_mesh_tx_queue_remove(&scheduler->routed_tx_queue, index, NULL);
        }
    }
    return 0;
}

static tavrn_maintenance_high_token_status_t unused_completion(
    void *context, const tavrn_maintenance_high_token_completion_t *completion)
{
    (void)context;
    (void)completion;
    return TAVRN_MAINTENANCE_HIGH_TOKEN_OK;
}

static int send_repair_rreq(repair_fixture_t *fixture,
                            tavrn_full_repair_binding_tick_result_t *tick)
{
    ble_mesh_sched_event_t scheduler_event;
    tavrn_maintenance_high_token_dispatch_result_t dispatch;

    if (tavrn_full_repair_binding_tick(&fixture->binding, 10u, tick) !=
            TAVRN_FULL_REPAIR_BINDING_OK ||
        tick->action_present == 0u ||
        tick->action.type != TAVRN_REPAIR_ACTION_RREQ_CREATED ||
        tick->action.token < 0x8000u ||
        fixture->maintenance.external_high_token.valid == 0u ||
        fixture->maintenance.external_high_token.token != tick->action.token ||
        tavrn_maintenance_high_token_reserve(
            &fixture->maintenance, &fixture->link, 0x1234u, 0x0001u,
            unused_completion, NULL, &(uint16_t){ 0u }) != TAVRN_MAINTENANCE_HIGH_TOKEN_BUSY ||
        !remove_queued_token(&fixture->scheduler, tick->action.token)) {
        return 0;
    }
    memset(&scheduler_event, 0, sizeof(scheduler_event));
    scheduler_event.type = BLE_MESH_SCHED_EVENT_TX_DONE;
    scheduler_event.tx_token = tick->action.token;
    scheduler_event.tx_requested_channel_mask = BLE_RADIO_ADV_CH_ALL;
    scheduler_event.tx_completed_channel_mask = BLE_RADIO_ADV_CH_ALL;
    memset(&dispatch, 0, sizeof(dispatch));
    return tavrn_maintenance_high_token_scheduler_event(
               &fixture->maintenance, &fixture->router, &fixture->link,
               &scheduler_event, 11u, &dispatch) == TAVRN_MAINTENANCE_HIGH_TOKEN_OK &&
        dispatch.external_handled != 0u &&
               dispatch.external_status == TAVRN_MAINTENANCE_HIGH_TOKEN_OK;
}

static int queue_repair_rreq(repair_fixture_t *fixture,
                             tavrn_full_repair_binding_tick_result_t *tick)
{
    return tavrn_full_repair_binding_tick(&fixture->binding, 10u, tick) ==
               TAVRN_FULL_REPAIR_BINDING_OK &&
        tick->action_present != 0u &&
        tick->action.type == TAVRN_REPAIR_ACTION_RREQ_CREATED &&
        fixture->maintenance.external_high_token.valid != 0u &&
        fixture->maintenance.external_high_token.token == tick->action.token &&
        tavrn_link_v2_tracked_token_in_use(&fixture->link, tick->action.token);
}

static int adopt_alternate_rrep(repair_fixture_t *fixture,
                                const tavrn_full_repair_binding_tick_result_t *tick,
                                uint16_t lifetime_ms, uint32_t now_ms)
{
    tavrn_rx_control_event_t rrep;
    tavrn_repair_snapshot_t snapshot;
    aodv_route_snapshot_t route;
    tavrn_logical_id_t destination = peer(adva_c).logical_id;

    memset(&rrep, 0, sizeof(rrep));
    rrep.transmitter = peer(adva_d);
    rrep.control.type = TAVRN_WIRE_E_RREP;
    rrep.control.pdu_len = 16u;
    rrep.control.pdu[0] = 0x54u;
    rrep.control.pdu[1] = 0x52u;
    rrep.control.pdu[2] = 0x02u;
    rrep.control.pdu[3] = 0x2au;
    rrep.control.pdu[4] = TAVRN_WIRE_E_RREP;
    rrep.control.pdu[5] = 0x80u;
    rrep.control.pdu[6] = 0x10u;
    rrep.control.pdu[7] = adva_a[0];
    rrep.control.pdu[8] = adva_c[0];
    rrep.control.pdu[9] = 1u;
    rrep.control.pdu[10] = 0u;
    rrep.control.pdu[11] = adva_a[0];
    rrep.control.pdu[12] = (uint8_t)tick->action.rreq_attempt.request_id;
    rrep.control.pdu[13] = (uint8_t)(tick->action.rreq_attempt.request_id >> 8);
    rrep.control.pdu[14] = (uint8_t)lifetime_ms;
    rrep.control.pdu[15] = (uint8_t)(lifetime_ms >> 8);
    return tavrn_full_repair_binding_receive_rrep(&fixture->binding, &rrep, now_ms) ==
               TAVRN_ROUTER_CONTROL_INTERCEPT_CONSUMED &&
        tavrn_repair_snapshot(&fixture->repair, &snapshot) == TAVRN_REPAIR_OK &&
        snapshot.state == TAVRN_REPAIR_STATE_FLUSHING &&
        tavrn_router_route_to_subject(&fixture->router, &destination,
                                      now_ms, &route) == TAVRN_ROUTER_ROUTE_OK &&
        memcmp(route.next_hop.adva.bytes, adva_d, TAVRN_ADVA_LEN) == 0;
}

static void test_exhaustive_reserved_terminals(void)
{
    static const tavrn_link_event_type_t terminals[] = {
        TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED,
        TAVRN_LINK_EVENT_CUSTODY_REJECTED,
        TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED,
        TAVRN_LINK_EVENT_CUSTODY_BUSY_EXPIRED,
        TAVRN_LINK_EVENT_RETRY_EXHAUSTED,
        TAVRN_LINK_EVENT_RADIO_FAULT_TERMINAL,
        TAVRN_LINK_EVENT_SERVICE_FAULT_TERMINAL,
    };
    uint8_t index;

    for (index = 0u; index < sizeof(terminals) / sizeof(terminals[0]); index++) {
        repair_fixture_t fixture;
        tavrn_link_data_t primary = transit_data((uint16_t)(0x9100u + index * 2u));
        tavrn_link_data_t reserved = transit_data((uint16_t)(primary.data_seq + 1u));
        tavrn_repair_snapshot_t before;
        tavrn_repair_snapshot_t after;
        tavrn_link_event_t event;

        if (!fixture_init(&fixture) ||
            !start_repair(&fixture, &primary, &reserved, &before)) {
            CHECK(0);
            continue;
        }
        event = terminal_event(terminals[index], &reserved);
        CHECK(before.reserved_link_slots == 1u && before.buffered_count == 1u &&
              clear_link_custody_for_terminal(&fixture.link, &reserved) &&
              tavrn_router_handle_link_event(&fixture.router, &event, 3u) ==
                  TAVRN_ROUTER_EVENT_OK &&
              tavrn_repair_snapshot(&fixture.repair, &after) == TAVRN_REPAIR_OK &&
              after.reserved_link_slots == 0u &&
              (terminals[index] == TAVRN_LINK_EVENT_RETRY_EXHAUSTED ?
                   after.buffered_count == 2u && transit_data_is_pinned(&fixture.link,
                                                                          &reserved) :
                   after.buffered_count == 1u && !transit_data_is_pinned(&fixture.link,
                                                                            &reserved)));
    }
}

static void test_unrelated_terminal_declines(void)
{
    repair_fixture_t fixture;
    tavrn_link_data_t primary = transit_data(0x9200u);
    tavrn_link_data_t reserved = transit_data(0x9201u);
    tavrn_link_data_t unrelated = transit_data(0x9202u);
    tavrn_repair_snapshot_t snapshot;
    tavrn_link_event_t event;

    if (!fixture_init(&fixture) ||
        !start_repair(&fixture, &primary, &reserved, &snapshot)) {
        CHECK(0);
        return;
    }
    pin_transit_data(&fixture.link, &unrelated);
    event = terminal_event(TAVRN_LINK_EVENT_CUSTODY_REJECTED, &unrelated);
    CHECK(tavrn_router_handle_link_event(&fixture.router, &event, 3u) ==
              TAVRN_ROUTER_EVENT_OK &&
          tavrn_repair_snapshot(&fixture.repair, &snapshot) == TAVRN_REPAIR_OK &&
          snapshot.reserved_link_slots == 1u && snapshot.buffered_count == 1u &&
          !transit_data_is_pinned(&fixture.link, &unrelated) &&
          transit_data_is_pinned(&fixture.link, &reserved));
}

static void test_declined_retry_retains_one_baseline_leave(void)
{
    repair_fixture_t fixture;
    tavrn_link_data_t primary = transit_data(0x9300u);
    tavrn_repair_snapshot_t snapshot;
    tavrn_link_event_t event;
    uint8_t index;

    if (!fixture_init(&fixture)) {
        CHECK(0);
        return;
    }
    for (index = 0u; index < TAVRN_REPAIR_BUFFER_CAPACITY; index++) {
        tavrn_link_data_t existing = transit_data((uint16_t)(0x9301u + index));

        if (!add_same_destination_custody(&fixture, &existing)) {
            CHECK(0);
            return;
        }
    }
    pin_transit_data(&fixture.link, &primary);
    event = terminal_event(TAVRN_LINK_EVENT_RETRY_EXHAUSTED, &primary);
    CHECK(tavrn_router_handle_link_event(&fixture.router, &event, 4u) ==
              TAVRN_ROUTER_EVENT_OK &&
          tavrn_repair_snapshot(&fixture.repair, &snapshot) == TAVRN_REPAIR_OK &&
          snapshot.state == TAVRN_REPAIR_STATE_IDLE &&
          fixture.maintenance.tc_metadata.origin_fact_count == 1u &&
          fixture.maintenance.tc_metadata.origin_facts[0].event == TAVRN_TC_EVENT_LEAVE &&
          memcmp(fixture.maintenance.tc_metadata.origin_facts[0].subject.bytes, adva_b,
                 TAVRN_ADVA_LEN) == 0 &&
          !transit_data_is_pinned(&fixture.link, &primary));
}

static void test_alternate_rrep_flushes_to_link_custody(void)
{
    repair_fixture_t fixture;
    tavrn_link_data_t primary = transit_data(0x9400u);
    tavrn_repair_snapshot_t snapshot;
    tavrn_full_repair_binding_tick_result_t tick;
    tavrn_link_event_t transferred;

    if (!fixture_init(&fixture) || !start_repair(&fixture, &primary, NULL, &snapshot) ||
        !send_repair_rreq(&fixture, &tick) ||
        !adopt_alternate_rrep(&fixture, &tick, 100u, 12u)) {
        CHECK(0);
        return;
    }
    CHECK(tavrn_full_repair_binding_tick(&fixture.binding, 13u, &tick) ==
              TAVRN_FULL_REPAIR_BINDING_OK &&
          tick.action.type == TAVRN_REPAIR_ACTION_FLUSH_DATA &&
          tick.reforward_status == TAVRN_ROUTER_REFORWARD_OK &&
          tavrn_repair_snapshot(&fixture.repair, &snapshot) == TAVRN_REPAIR_OK &&
          snapshot.buffered_count == 0u && snapshot.flush_count == 1u &&
          fixture.link.custody[0].phase != TAVRN_CUSTODY_FREE &&
          transit_data_is_pinned(&fixture.link, &primary));
    transferred = terminal_event(TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED, &primary);
    CHECK(tavrn_router_handle_link_event(&fixture.router, &transferred, 14u) ==
              TAVRN_ROUTER_EVENT_OK &&
          !transit_data_is_pinned(&fixture.link, &primary));
}

static void test_permanent_reforward_drop_releases_pin(void)
{
    repair_fixture_t fixture;
    tavrn_link_data_t primary = transit_data(0x9500u);
    tavrn_repair_snapshot_t snapshot;
    tavrn_full_repair_binding_tick_result_t tick;

    if (!fixture_init(&fixture) || !start_repair(&fixture, &primary, NULL, &snapshot) ||
        !send_repair_rreq(&fixture, &tick) ||
        !adopt_alternate_rrep(&fixture, &tick, 1u, 12u)) {
        CHECK(0);
        return;
    }
    CHECK(tavrn_full_repair_binding_tick(&fixture.binding, 13u, &tick) ==
              TAVRN_FULL_REPAIR_BINDING_OK &&
          tick.action.type == TAVRN_REPAIR_ACTION_FLUSH_DATA &&
          tick.reforward_status == TAVRN_ROUTER_REFORWARD_NOT_FOUND &&
          tavrn_repair_snapshot(&fixture.repair, &snapshot) == TAVRN_REPAIR_OK &&
          snapshot.buffered_count == 0u && snapshot.flush_count == 0u &&
          snapshot.drop_count == 1u &&
          snapshot.state == TAVRN_REPAIR_STATE_COOLDOWN &&
           !transit_data_is_pinned(&fixture.link, &primary));
}

static void test_invalid_reforward_drop_releases_pin(void)
{
    repair_fixture_t fixture;
    tavrn_link_data_t primary = transit_data(0x9600u);
    tavrn_repair_snapshot_t snapshot;
    tavrn_full_repair_binding_tick_result_t tick;

    if (!fixture_init(&fixture) || !start_repair(&fixture, &primary, NULL, &snapshot) ||
        !send_repair_rreq(&fixture, &tick) ||
        !adopt_alternate_rrep(&fixture, &tick, 100u, 12u)) {
        CHECK(0);
        return;
    }
    fixture.router.fault_reason = TAVRN_ROUTER_FAULT_DATA_TERMINAL_HOOK_INVALID;
    CHECK(tavrn_full_repair_binding_tick(&fixture.binding, 13u, &tick) ==
              TAVRN_FULL_REPAIR_BINDING_OK &&
          tick.action.type == TAVRN_REPAIR_ACTION_FLUSH_DATA &&
          tick.reforward_status == TAVRN_ROUTER_REFORWARD_INVALID &&
          tavrn_repair_snapshot(&fixture.repair, &snapshot) == TAVRN_REPAIR_OK &&
          snapshot.buffered_count == 0u && snapshot.flush_count == 0u &&
          snapshot.drop_count == 1u &&
          snapshot.state == TAVRN_REPAIR_STATE_COOLDOWN &&
          !transit_data_is_pinned(&fixture.link, &primary));
}

static void test_quarantined_reservations_reconcile_and_clear_marked_pin(void)
{
    repair_fixture_t fixture;
    tavrn_link_data_t primary = transit_data(0x9700u);
    tavrn_link_data_t reserved = transit_data(0x9701u);
    tavrn_repair_snapshot_t snapshot;
    tavrn_full_repair_binding_tick_result_t tick;
    tavrn_link_event_t transferred;
    tavrn_direct_peer_t failed_hop = peer(adva_b);

    if (!fixture_init(&fixture) || !start_repair(&fixture, &primary, &reserved,
                                                 &snapshot)) {
        CHECK(0);
        return;
    }
    CHECK(snapshot.buffered_count == 1u && snapshot.reserved_link_slots == 1u &&
           tavrn_link_v2_quarantine_peer_incarnation(&fixture.link, &failed_hop) ==
               TAVRN_LINK_RESOLVE_OK &&
           transit_data_is_pinned(&fixture.link, &primary) &&
           transit_data_has_external_custody_owner(&fixture.link, &primary) &&
           transit_data_is_marked_for_incarnation_clear(&fixture.link, &primary) &&
          !transit_data_is_pinned(&fixture.link, &reserved) &&
          send_repair_rreq(&fixture, &tick) &&
          tavrn_repair_snapshot(&fixture.repair, &snapshot) == TAVRN_REPAIR_OK &&
          snapshot.buffered_count == 1u && snapshot.reserved_link_slots == 0u &&
          adopt_alternate_rrep(&fixture, &tick, 100u, 12u) &&
          tavrn_full_repair_binding_tick(&fixture.binding, 13u, &tick) ==
              TAVRN_FULL_REPAIR_BINDING_OK &&
           tick.action.type == TAVRN_REPAIR_ACTION_FLUSH_DATA &&
           transit_data_is_pinned(&fixture.link, &primary) &&
           transit_data_has_external_custody_owner(&fixture.link, &primary) &&
           transit_data_is_marked_for_incarnation_clear(&fixture.link, &primary));
    transferred = terminal_event(TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED, &primary);
    transferred.detail.transferred_data.next_hop = peer(adva_d);
    CHECK(tavrn_router_handle_link_event(&fixture.router, &transferred, 14u) ==
              TAVRN_ROUTER_EVENT_OK &&
          !transit_data_is_pinned(&fixture.link, &primary));
}

static void test_mixed_next_hop_retry_preserves_failed_hop_reservation(void)
{
    repair_fixture_t fixture;
    tavrn_link_data_t primary = transit_data(0x9800u);
    tavrn_link_data_t reserved = transit_data(0x9801u);
    tavrn_repair_snapshot_t snapshot;
    tavrn_link_event_t retry;

    if (!fixture_init(&fixture) || !start_repair(&fixture, &primary, &reserved,
                                                 &snapshot)) {
        CHECK(0);
        return;
    }
    retry = terminal_event(TAVRN_LINK_EVENT_RETRY_EXHAUSTED, &reserved);
    retry.detail.owned_data.next_hop = peer(adva_d);
    CHECK(clear_link_custody_for_terminal(&fixture.link, &reserved) &&
          tavrn_router_handle_link_event(&fixture.router, &retry, 3u) ==
              TAVRN_ROUTER_EVENT_OK &&
          tavrn_repair_snapshot(&fixture.repair, &snapshot) == TAVRN_REPAIR_OK &&
          snapshot.buffered_count == 1u && snapshot.reserved_link_slots == 1u &&
          fixture.maintenance.tc_metadata.origin_fact_count == 1u &&
          fixture.maintenance.tc_metadata.origin_facts[0].event == TAVRN_TC_EVENT_LEAVE &&
          memcmp(fixture.maintenance.tc_metadata.origin_facts[0].subject.bytes, adva_d,
                 TAVRN_ADVA_LEN) == 0);
}

static void test_repair_fault_completion_releases_external_token(void)
{
    repair_fixture_t queued_fixture;
    repair_fixture_t created_fixture;
    tavrn_link_data_t queued_data = transit_data(0x9900u);
    tavrn_link_data_t created_data = transit_data(0x9901u);
    tavrn_repair_snapshot_t snapshot;
    tavrn_full_repair_binding_tick_result_t tick;
    tavrn_repair_action_t created;
    tavrn_maintenance_high_token_dispatch_result_t dispatch;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t link_output;

    if (!fixture_init(&queued_fixture) ||
        !start_repair(&queued_fixture, &queued_data, NULL, &snapshot) ||
        !queue_repair_rreq(&queued_fixture, &tick)) {
        CHECK(0);
        return;
    }
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_RADIO_FAULT;
    memset(&dispatch, 0, sizeof(dispatch));
    CHECK(tavrn_link_v2_on_scheduler_event(&queued_fixture.link, &event, 11u,
                                            &link_output) == TAVRN_LINK_STEP_NO_EVENT &&
          tavrn_maintenance_high_token_scheduler_event(
              &queued_fixture.maintenance, &queued_fixture.router, &queued_fixture.link,
              &event, 11u, &dispatch) == TAVRN_MAINTENANCE_HIGH_TOKEN_OK &&
          dispatch.external_handled != 0u &&
          dispatch.external_status == TAVRN_MAINTENANCE_HIGH_TOKEN_OK &&
          queued_fixture.maintenance.external_high_token.valid == 0u &&
          !tavrn_link_v2_tracked_token_in_use(&queued_fixture.link, tick.action.token) &&
          tavrn_repair_snapshot(&queued_fixture.repair, &snapshot) == TAVRN_REPAIR_OK &&
          snapshot.active_token == 0u);
    CHECK(tavrn_maintenance_high_token_scheduler_event(
              &queued_fixture.maintenance, &queued_fixture.router, &queued_fixture.link,
              &event, 12u, &dispatch) == TAVRN_MAINTENANCE_HIGH_TOKEN_OK &&
          dispatch.external_handled == 0u && snapshot.active_token == 0u);

    if (!fixture_init(&created_fixture) ||
        !start_repair(&created_fixture, &created_data, NULL, &snapshot)) {
        CHECK(0);
        return;
    }
    memset(&created, 0, sizeof(created));
    CHECK(tavrn_repair_owner_tick(&created_fixture.repair, 20u, &created) ==
              TAVRN_REPAIR_OK &&
          created.type == TAVRN_REPAIR_ACTION_RREQ_CREATED &&
          created_fixture.maintenance.external_high_token.valid != 0u &&
          created_fixture.maintenance.external_high_token.token == created.token &&
          !tavrn_link_v2_tracked_token_in_use(&created_fixture.link, created.token));
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_SERVICE_FAULT;
    memset(&dispatch, 0, sizeof(dispatch));
    CHECK(tavrn_link_v2_on_scheduler_event(&created_fixture.link, &event, 21u,
                                            &link_output) == TAVRN_LINK_STEP_NO_EVENT &&
          tavrn_maintenance_high_token_scheduler_event(
              &created_fixture.maintenance, &created_fixture.router,
              &created_fixture.link, &event, 21u, &dispatch) ==
              TAVRN_MAINTENANCE_HIGH_TOKEN_OK &&
          dispatch.external_handled != 0u &&
          dispatch.external_status == TAVRN_MAINTENANCE_HIGH_TOKEN_OK &&
          created_fixture.maintenance.external_high_token.valid == 0u &&
          tavrn_repair_snapshot(&created_fixture.repair, &snapshot) == TAVRN_REPAIR_OK &&
          snapshot.active_token == 0u);
}

static void test_declined_retry_overflow_retention_and_fail_stop(void)
{
    repair_fixture_t fixture;
    tavrn_link_data_t first = transit_data(0x9a00u);
    tavrn_link_data_t second = transit_data(0x9a01u);
    tavrn_link_event_t event;
    tavrn_adva_t overflow_before;
    uint8_t index;

    if (!fixture_init(&fixture)) {
        CHECK(0);
        return;
    }
    for (index = 0u; index < TAVRN_TC_METADATA_ORIGIN_CAPACITY; index++) {
        tavrn_adva_t subject = generated_adva((uint8_t)(0x60u + index));

        if (tavrn_maintenance_tc_on_retry_exhausted(&fixture.maintenance, &subject,
                                                    NULL, 1u) !=
            (index == 0u ? TAVRN_TC_METADATA_PREPARED :
                           TAVRN_TC_METADATA_RETAINED)) {
            CHECK(0);
            return;
        }
    }
    for (index = 0u; index < TAVRN_REPAIR_BUFFER_CAPACITY; index++) {
        tavrn_link_data_t existing = transit_data((uint16_t)(0x9a10u + index));

        if (!add_same_destination_custody(&fixture, &existing)) {
            CHECK(0);
            return;
        }
    }
    pin_transit_data(&fixture.link, &first);
    event = terminal_event(TAVRN_LINK_EVENT_RETRY_EXHAUSTED, &first);
    CHECK(tavrn_router_handle_link_event(&fixture.router, &event, 2u) ==
              TAVRN_ROUTER_EVENT_OK &&
          fixture.maintenance.tc_metadata.origin_fact_count ==
              TAVRN_TC_METADATA_ORIGIN_CAPACITY &&
          fixture.maintenance.tc_metadata.retry_exhausted_leave_overflow.valid != 0u &&
          memcmp(fixture.maintenance.tc_metadata.retry_exhausted_leave_overflow
                     .failed_next_hop.bytes,
                 adva_b, TAVRN_ADVA_LEN) == 0 &&
          !transit_data_is_pinned(&fixture.link, &first));
    overflow_before = fixture.maintenance.tc_metadata.retry_exhausted_leave_overflow
        .failed_next_hop;
    pin_transit_data(&fixture.link, &second);
    event = terminal_event(TAVRN_LINK_EVENT_RETRY_EXHAUSTED, &second);
    event.detail.owned_data.next_hop = peer(adva_d);
    CHECK(tavrn_router_handle_link_event(&fixture.router, &event, 3u) ==
              TAVRN_ROUTER_EVENT_INVALID &&
          fixture.maintenance.tc_metadata.retry_exhausted_leave_overflow.valid != 0u &&
          memcmp(fixture.maintenance.tc_metadata.retry_exhausted_leave_overflow
                     .failed_next_hop.bytes,
                 overflow_before.bytes, TAVRN_ADVA_LEN) == 0 &&
          transit_data_is_pinned(&fixture.link, &second));
}

int main(void)
{
    test_exhaustive_reserved_terminals();
    test_unrelated_terminal_declines();
    test_declined_retry_retains_one_baseline_leave();
    test_alternate_rrep_flushes_to_link_custody();
    test_permanent_reforward_drop_releases_pin();
    test_invalid_reforward_drop_releases_pin();
    test_quarantined_reservations_reconcile_and_clear_marked_pin();
    test_mixed_next_hop_retry_preserves_failed_hop_reservation();
    test_repair_fault_completion_releases_external_token();
    test_declined_retry_overflow_retention_and_fail_stop();
    if (failures != 0u) {
        printf("tavrn_full_repair_binding tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_full_repair_binding tests passed\n");
    return 0;
}
