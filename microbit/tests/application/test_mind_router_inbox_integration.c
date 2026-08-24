#include "mind_root_inbox.h"

#include <stdio.h>
#include <string.h>

#include "tavrn_link_v2.h"
#include "tavrn_router.h"

static unsigned failures;

static const uint8_t sender_adva[TAVRN_ADVA_LEN] = {
    0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu,
};
static const uint8_t receiver_adva[TAVRN_ADVA_LEN] = {
    0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u,
};

static void check(int condition, const char *message)
{
    if (!condition) {
        failures++;
        printf("FAIL: %s\n", message);
    }
}

static tavrn_direct_peer_t peer(const uint8_t adva[TAVRN_ADVA_LEN])
{
    tavrn_direct_peer_t result;

    memset(&result, 0, sizeof(result));
    result.logical_id.width = TAVRN_IDENTITY_SID16;
    result.logical_id.value = (uint16_t)adva[0] | ((uint16_t)adva[1] << 8);
    memcpy(result.adva.bytes, adva, TAVRN_ADVA_LEN);
    return result;
}

static tavrn_direct_peer_t peer8(const uint8_t adva[TAVRN_ADVA_LEN])
{
    tavrn_direct_peer_t result = peer(adva);

    result.logical_id.width = TAVRN_IDENTITY_SID8;
    result.logical_id.value = adva[0];
    return result;
}

static tavrn_link_config_t link_config(const uint8_t local_adva[TAVRN_ADVA_LEN])
{
    tavrn_link_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = peer(local_adva);
    config.network_id = 0x2au;
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

static int no_sid8_conflict(void *context, const tavrn_logical_id_t *logical_id,
                            const tavrn_adva_t *direct_adva_or_null)
{
    (void)context;
    (void)logical_id;
    (void)direct_adva_or_null;
    return 0;
}

static aodv_core_config_t core_config(void)
{
    aodv_core_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = peer(receiver_adva);
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

static tavrn_router_delivery_status_t inbox_reserve(
    void *context, const tavrn_direct_peer_t *transmitter,
    const tavrn_link_data_t *data, tavrn_router_delivery_token_t *token_out,
    uint32_t now_ms)
{
    (void)now_ms;
    return mind_root_inbox_reserve((mind_root_inbox_t *)context, transmitter, data,
                                   token_out);
}

static tavrn_router_delivery_status_t inbox_commit(
    void *context, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t now_ms)
{
    return mind_root_inbox_commit((mind_root_inbox_t *)context, token, transmitter,
                                  data, now_ms);
}

static tavrn_router_delivery_status_t inbox_cancel(
    void *context, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t now_ms)
{
    (void)now_ms;
    return mind_root_inbox_cancel((mind_root_inbox_t *)context, token, transmitter,
                                  data);
}

static const ble_mesh_tx_item_t *queued_data(const ble_mesh_scheduler_t *scheduler)
{
    uint8_t index;

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry = &scheduler->routed_tx_queue.entries[index];

        if (entry->occupied != 0u && entry->item.adv_len > 11u &&
            entry->item.adv_data[11] == TAVRN_WIRE_DATA) {
            return &entry->item;
        }
    }
    return NULL;
}

static uint8_t queued_hack_count(const ble_mesh_scheduler_t *scheduler,
                                 tavrn_hack_status_t status)
{
    uint8_t index;
    uint8_t count = 0u;

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry = &scheduler->routed_tx_queue.entries[index];

        if (entry->occupied != 0u && entry->item.adv_len > 12u &&
            entry->item.adv_data[11] == TAVRN_WIRE_HACK &&
            entry->item.adv_data[entry->item.adv_len - 1u] == (uint8_t)status) {
            count++;
        }
    }
    return count;
}

static tavrn_link_data_t report_data(void)
{
    tavrn_link_data_t data;
    mind_application_wire_record_t report;
    uint8_t payload[MIND_PAYLOAD_SIZE] = { MIND_SCHEMA_VERSION,
        MIND_EVT_FALL_AND_SHOUT, 73u, 0x1fu, 0x1au, 0x7du, 0x34u };

    memset(&data, 0, sizeof(data));
    if (mind_application_wire_pack_observed_report(&report, 7u, 0xa1b234u, payload,
                                                    41u) != MIND_APPLICATION_WIRE_OK) {
        return data;
    }
    data.origin = peer8(sender_adva).logical_id;
    data.final_destination = peer8(receiver_adva).logical_id;
    data.data_seq = 0x1234u;
    data.ttl = 3u;
    data.app_kind = report.app_kind;
    data.app_source = report.app_source;
    data.urgent = report.urgent;
    data.app_len = report.app_len;
    memcpy(data.app_bytes, report.app_bytes, sizeof(data.app_bytes));
    data.ownership = TAVRN_DATA_ORIGINATED;
    return data;
}

static mind_application_wire_result_t unpack_data_report(
    const tavrn_link_data_t *data, mind_application_report_t *report_out)
{
    mind_application_wire_record_t record;

    if (data == NULL || report_out == NULL) {
        return MIND_APPLICATION_WIRE_ERR_NULL;
    }
    memset(&record, 0, sizeof(record));
    record.app_kind = data->app_kind;
    record.app_source = data->app_source;
    record.urgent = data->urgent;
    record.app_len = data->app_len;
    memcpy(record.app_bytes, data->app_bytes, sizeof(record.app_bytes));
    return mind_application_wire_unpack_report(&record, report_out);
}

static void test_duplicate_data_reaches_inbox_once(void)
{
    ble_mesh_scheduler_t sender_scheduler;
    ble_mesh_scheduler_t receiver_scheduler;
    tavrn_link_v2_t sender_link;
    tavrn_link_v2_t receiver_link;
    tavrn_router_t receiver_router;
    aodv_core_t receiver_core;
    tavrn_router_application_hooks_t hooks;
    tavrn_link_config_t sender_config;
    tavrn_link_config_t receiver_config;
    aodv_core_config_t receiver_config_core;
    tavrn_link_data_t data = report_data();
    tavrn_link_event_t link_output;
    tavrn_router_phase_trace_t trace;
    tavrn_router_dispatch_event_t dispatch_event;
    ble_mesh_sched_event_t event;
    tavrn_direct_peer_t next_hop = peer8(receiver_adva);
    const ble_mesh_tx_item_t *item;
    mind_root_inbox_t inbox;
    mind_root_inbox_entry_t entry;
    mind_application_report_t unpacked;

    memset(&sender_scheduler, 0, sizeof(sender_scheduler));
    memset(&receiver_scheduler, 0, sizeof(receiver_scheduler));
    ble_mesh_scheduler_init(&sender_scheduler, 0u, sender_adva);
    ble_mesh_scheduler_init(&receiver_scheduler, 0u, receiver_adva);
    mind_root_inbox_init(&inbox);
    sender_config = link_config(sender_adva);
    receiver_config = link_config(receiver_adva);
    receiver_config_core = core_config();
    sender_config.local_peer = peer8(sender_adva);
    receiver_config.local_peer = peer8(receiver_adva);
    sender_config.identity_conflict = no_sid8_conflict;
    receiver_config.identity_conflict = no_sid8_conflict;
    receiver_config_core.local_peer = peer8(receiver_adva);
    memset(&hooks, 0, sizeof(hooks));
    hooks.context = &inbox;
    hooks.reserve = inbox_reserve;
    hooks.commit = inbox_commit;
    hooks.cancel = inbox_cancel;
    check(tavrn_link_v2_init(&sender_link, &sender_scheduler, &sender_config,
                             0u) == TAVRN_LINK_INIT_OK &&
              tavrn_link_v2_init(&receiver_link, &receiver_scheduler,
                                 &receiver_config, 0u) == TAVRN_LINK_INIT_OK &&
              aodv_core_init(&receiver_core, &receiver_config_core, 0u) == AODV_INIT_OK &&
              tavrn_router_init(&receiver_router, &receiver_link, &receiver_core, NULL) ==
                  TAVRN_ROUTER_INIT_OK &&
              tavrn_router_set_application_hooks(&receiver_router, &hooks) ==
                  TAVRN_ROUTER_APPLICATION_HOOK_OK,
          "real production sender/link/router/inbox fixture initializes");
    check(tavrn_link_v2_send_unicast(&sender_link, &next_hop, &data, 1u, &link_output) ==
              TAVRN_LINK_SEND_OK &&
              tavrn_link_v2_dispatch(&sender_link, 1u, &link_output) ==
                  TAVRN_LINK_STEP_NO_EVENT,
          "real sender produces one generic DATA advertisement");
    item = queued_data(&sender_scheduler);
    check(item != NULL, "sender DATA advertisement is queued by the production link");
    if (item == NULL) {
        return;
    }
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
    memcpy(event.adv_addr, sender_adva, TAVRN_ADVA_LEN);
    event.adv_len = item->adv_len;
    memcpy(event.adv_data, item->adv_data, item->adv_len);
    {
        tavrn_router_event_status_t receive_status;
        tavrn_router_event_status_t dispatch_status;

        memset(&dispatch_event, 0, sizeof(dispatch_event));
        receive_status = tavrn_router_handle_scheduler_event_ex(&receiver_router, &event,
                                                                 10u, &trace);
        dispatch_status = tavrn_router_dispatch_ex(&receiver_router, 10u,
                                                    &dispatch_event);
        check(receive_status == TAVRN_ROUTER_EVENT_OK &&
              dispatch_status == TAVRN_ROUTER_EVENT_OK && inbox.count == 1u &&
              queued_hack_count(&receiver_scheduler, TAVRN_HACK_ACCEPTED) == 1u,
          "first DATA reserves/commits final inbox before accepted HACK");
    }
    check(tavrn_router_handle_scheduler_event_ex(&receiver_router, &event, 11u, &trace) ==
               TAVRN_ROUTER_EVENT_OK && inbox.count == 1u &&
               queued_hack_count(&receiver_scheduler, TAVRN_HACK_ACCEPTED) == 1u &&
               queued_hack_count(&receiver_scheduler, TAVRN_HACK_DUPLICATE) == 0u &&
               mind_root_inbox_peek(&inbox, &entry) && entry.data.data_seq == data.data_seq &&
               entry.data.app_kind == MIND_REPORT_OBSERVED &&
               entry.data.app_len == MIND_APPLICATION_REPORT_BYTES &&
               entry.data.app_bytes[0] == 0x34u && entry.data.app_bytes[1] == 0xb2u &&
               entry.data.app_bytes[2] == 0xa1u && entry.data.app_bytes[3] == 1u &&
               entry.data.app_bytes[4] == MIND_EVT_FALL_AND_SHOUT &&
               entry.data.app_bytes[9] == 41u &&
               unpack_data_report(&entry.data, &unpacked) == MIND_APPLICATION_WIRE_OK &&
               unpacked.packet_id24 == 0xa1b234u &&
               unpacked.schema_payload[6] == 0x34u &&
               unpacked.rssi_magnitude_db == 41u,
          "SID8 DATA decode/root inbox preserves the exact observed RSSI record once");
}

static void test_sid16_rejects_ten_byte_observed_report(void)
{
    ble_mesh_scheduler_t scheduler;
    tavrn_link_v2_t link;
    tavrn_link_config_t config = link_config(sender_adva);
    tavrn_link_data_t data = report_data();
    tavrn_link_event_t output;
    tavrn_direct_peer_t next_hop = peer(receiver_adva);

    memset(&scheduler, 0, sizeof(scheduler));
    ble_mesh_scheduler_init(&scheduler, 0u, sender_adva);
    data.origin = peer(sender_adva).logical_id;
    data.final_destination = peer(receiver_adva).logical_id;
    check(tavrn_link_v2_init(&link, &scheduler, &config, 0u) == TAVRN_LINK_INIT_OK &&
              tavrn_link_v2_send_unicast(&link, &next_hop, &data, 1u, &output) ==
                  TAVRN_LINK_SEND_INVALID,
          "SID16 submission rejects the ten-byte observed report before advertising");
}

int main(void)
{
    test_duplicate_data_reaches_inbox_once();
    test_sid16_rejects_ten_byte_observed_report();
    if (failures != 0u) {
        printf("mind_router_inbox_integration failures=%u\n", failures);
        return 1;
    }
    printf("mind_router_inbox_integration tests passed\n");
    return 0;
}
