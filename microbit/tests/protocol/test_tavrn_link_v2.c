#include "tavrn_link_v2.h"
#include "tron_timer_config.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;
static unsigned int emitted;

static int first_for(const char *requirement)
{
    unsigned int bit = requirement[0] == 'B' ? 0u :
        1u + (unsigned int)(requirement[6] - '0');
    unsigned int mask = 1u << bit;

    if ((emitted & mask) != 0u) {
        return 0;
    }
    emitted |= mask;
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

static const uint8_t adva_a[6] = { 0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu };
static const uint8_t adva_b[6] = { 0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u };
static const uint8_t adva_c[6] = { 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0xc1u };
static const uint8_t adva_d[6] = { 0x21u, 0x32u, 0x43u, 0x54u, 0x65u, 0xc2u };

static const uint8_t data16[] = {
    0x02u, 0x01u, 0x06u, 0x1bu, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, 0x2au, 0x10u, 0x20u, 0x30u,
    0xdcu, 0x4bu, 0x18u, 0x42u, 0x11u, 0x22u, 0x34u,
    0x12u, 0x01u, 0x07u, 0x01u, 0x05u, 0x64u, 0x34u,
    0x12u, 0x50u, 0x7eu,
};
static const uint8_t hack16[] = {
    0x02u, 0x01u, 0x06u, 0x14u, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, 0x2au, 0x11u, 0x00u, 0x18u,
    0x42u, 0x18u, 0x42u, 0x11u, 0x22u, 0x34u, 0x12u,
    0x01u, 0x07u, 0x00u,
};
static const uint8_t hack_to_b16[] = {
    0x02u, 0x01u, 0x06u, 0x14u, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, 0x2au, 0x11u, 0x00u, 0xdcu,
    0x4bu, 0x18u, 0x42u, 0x11u, 0x22u, 0x34u, 0x12u,
    0x01u, 0x07u, 0x00u,
};
static const uint8_t flood16[] = {
    0x02u, 0x01u, 0x06u, 0x13u, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, 0x2au, 0x12u, 0x00u, 0x40u,
    0x18u, 0x42u, 0x02u, 0x01u, 0x01u, 0x03u, 0xaau,
    0xbbu, 0xccu,
};
static const uint8_t rrep_ack16[] = {
    0x02u, 0x01u, 0x06u, 0x13u, 0xffu, 0xffu, 0xffu,
    0x54u, 0x52u, 0x02u, 0x2au, 0x09u, 0x00u, 0x18u,
    0x42u, 0x11u, 0x22u, 0x03u, 0x02u, 0x18u, 0x42u,
    0x01u, 0x10u,
};

static tavrn_direct_peer_t make_peer(const uint8_t adva[6])
{
    tavrn_direct_peer_t peer;

    memset(&peer, 0, sizeof(peer));
    peer.logical_id.width = TAVRN_IDENTITY_SID16;
    peer.logical_id.value = (uint16_t)adva[0] | ((uint16_t)adva[1] << 8);
    memcpy(peer.adva.bytes, adva, 6u);
    return peer;
}

static tavrn_link_data_t make_data(uint16_t sequence)
{
    tavrn_link_data_t data;

    memset(&data, 0, sizeof(data));
    data.origin = make_peer(adva_a).logical_id;
    data.final_destination.width = TAVRN_IDENTITY_SID16;
    data.final_destination.value = 0x2211u;
    data.data_seq = sequence;
    data.ttl = 3u;
    data.app_kind = 0x01u;
    data.app_source = 0x07u;
    data.app_len = 7u;
    memcpy(data.app_bytes, &data16[24], 7u);
    data.ownership = TAVRN_DATA_ORIGINATED;
    return data;
}

static tavrn_link_data_t make_transit_data(
    const uint8_t origin[6], const uint8_t final_destination[6],
    uint16_t sequence)
{
    tavrn_link_data_t data = make_data(sequence);

    data.origin = make_peer(origin).logical_id;
    data.final_destination = make_peer(final_destination).logical_id;
    data.ownership = TAVRN_DATA_TRANSIT;
    return data;
}

static tavrn_link_config_t make_config(void)
{
    tavrn_link_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_peer(adva_a);
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

static int setup_link_for(tavrn_link_v2_t *link, ble_mesh_scheduler_t *scheduler,
                          const uint8_t local_adva[6], uint32_t now_ms)
{
    tavrn_link_config_t config = make_config();

    config.local_peer = make_peer(local_adva);
    memset(scheduler, 0, sizeof(*scheduler));
    ble_mesh_scheduler_init(scheduler, now_ms, local_adva);
    if (tavrn_link_v2_init(link, scheduler, &config, now_ms) != TAVRN_LINK_INIT_OK) {
        printf("FATAL link test setup failed\n");
        failures++;
        return 0;
    }
    return 1;
}

static int setup_link(tavrn_link_v2_t *link, ble_mesh_scheduler_t *scheduler,
                      uint32_t now_ms)
{
    return setup_link_for(link, scheduler, adva_a, now_ms);
}

static void seed_active_sender(tavrn_link_v2_t *link, uint16_t sequence,
                               uint32_t now_ms)
{
    tavrn_custody_slot_t *slot = &link->custody[0];

    memset(slot, 0, sizeof(*slot));
    slot->phase = TAVRN_CUSTODY_ACTIVE_WAIT_HACK;
    slot->next_hop = make_peer(adva_b);
    slot->data = make_data(sequence);
    slot->scheduler_token = 0x41u;
    slot->attempt_count = 1u;
    slot->attempt_requested_channel_mask = BLE_RADIO_ADV_CH_ALL;
    slot->attempt_completed_channel_mask = BLE_RADIO_ADV_CH_ALL;
    slot->adv_len = (uint8_t)sizeof(data16);
    memcpy(slot->adv_data, data16, sizeof(data16));
    slot->transaction_deadline_ms = now_ms + 5000u;
    slot->response_deadline_ms = now_ms + 250u;
    slot->first_tx_ms = now_ms;
    slot->last_tx_ms = now_ms;
    link->active_custody_index = 0u;
}

static void seed_active_queued(tavrn_link_v2_t *link, uint16_t sequence,
                               uint32_t now_ms)
{
    tavrn_custody_slot_t *slot = &link->custody[0];

    memset(slot, 0, sizeof(*slot));
    slot->phase = TAVRN_CUSTODY_ACTIVE_QUEUED;
    slot->next_hop = make_peer(adva_b);
    slot->data = make_data(sequence);
    slot->scheduler_token = 0x41u;
    slot->adv_len = (uint8_t)sizeof(data16);
    memcpy(slot->adv_data, data16, sizeof(data16));
    slot->transaction_deadline_ms = now_ms + 5000u;
    link->active_custody_index = 0u;
}

static uint16_t active_token(const tavrn_link_v2_t *link)
{
    if (link->active_custody_index >= TAVRN_LINK_CUSTODY_CAPACITY) {
        return BLE_MESH_TX_TOKEN_NONE;
    }
    return link->custody[link->active_custody_index].scheduler_token;
}

static void seed_candidate_eviction_queue(ble_mesh_scheduler_t *scheduler)
{
    ble_mesh_tx_queue_entry_t *entry;
    uint8_t i;

    memset(&scheduler->routed_tx_queue, 0, sizeof(scheduler->routed_tx_queue));
    scheduler->routed_tx_queue.count = BLE_MESH_TX_QUEUE_CAPACITY;
    for (i = 0u; i < BLE_MESH_TX_QUEUE_CAPACITY; i++) {
        entry = &scheduler->routed_tx_queue.entries[i];
        entry->occupied = 1u;
        entry->ordinal = (uint32_t)i + 1u;
        entry->item.adv_len = 3u;
        entry->item.channel_mask = BLE_RADIO_ADV_CH_ALL;
        /* Every resident item is lower priority than a HACK. Slot zero owns
           the active custody transfer and therefore must be terminalized if
           queue admission evicts its token. */
        entry->item.priority = BLE_MESH_TX_PRIORITY_DATA;
        entry->item.service_class = i == 0u ? BLE_MESH_TX_SERVICE_CUSTODY_DATA :
            BLE_MESH_TX_SERVICE_BEST_EFFORT;
        entry->item.token = i == 0u ? 0x41u : BLE_MESH_TX_TOKEN_NONE;
    }
}

static int queued_hack_status(const ble_mesh_scheduler_t *scheduler, uint8_t status)
{
    uint8_t i;

    for (i = 0u; i < BLE_MESH_TX_QUEUE_CAPACITY; i++) {
        const ble_mesh_tx_queue_entry_t *entry = &scheduler->routed_tx_queue.entries[i];

        if (entry->occupied != 0u && entry->item.adv_len == sizeof(hack16) &&
            entry->item.adv_data[11] == TAVRN_WIRE_HACK &&
            entry->item.adv_data[23] == status) {
            return 1;
        }
    }
    return 0;
}

static const ble_mesh_tx_item_t *queued_hack_item(const ble_mesh_scheduler_t *scheduler,
                                                    uint8_t status)
{
    uint8_t i;

    for (i = 0u; i < BLE_MESH_TX_QUEUE_CAPACITY; i++) {
        const ble_mesh_tx_queue_entry_t *entry = &scheduler->routed_tx_queue.entries[i];

        if (entry->occupied != 0u && entry->item.adv_len == sizeof(hack16) &&
            entry->item.adv_data[11] == TAVRN_WIRE_HACK &&
            entry->item.adv_data[23] == status) {
            return &entry->item;
        }
    }
    return NULL;
}

static const ble_mesh_tx_item_t *queued_wire_item(
    const ble_mesh_scheduler_t *scheduler, tavrn_wire_type_t type,
    ble_mesh_tx_token_t token)
{
    uint8_t i;

    for (i = 0u; i < BLE_MESH_TX_QUEUE_CAPACITY; i++) {
        const ble_mesh_tx_queue_entry_t *entry = &scheduler->routed_tx_queue.entries[i];

        if (entry->occupied != 0u && entry->item.token == token &&
            entry->item.adv_len > 11u &&
            entry->item.adv_data[11] == (uint8_t)type) {
            return &entry->item;
        }
    }
    return NULL;
}

static int queue_has_token(const ble_mesh_scheduler_t *scheduler,
                           ble_mesh_tx_token_t token)
{
    uint8_t i;

    for (i = 0u; i < BLE_MESH_TX_QUEUE_CAPACITY; i++) {
        if (scheduler->routed_tx_queue.entries[i].occupied != 0u &&
            scheduler->routed_tx_queue.entries[i].item.token == token) {
            return 1;
        }
    }
    return 0;
}

static void seed_four_ready(tavrn_link_v2_t *link, uint16_t first_sequence,
                            uint32_t now_ms)
{
    uint8_t i;

    for (i = 0u; i < TAVRN_LINK_CUSTODY_CAPACITY; i++) {
        tavrn_custody_slot_t *slot = &link->custody[i];

        memset(slot, 0, sizeof(*slot));
        slot->phase = TAVRN_CUSTODY_READY_NOT_ELIGIBLE;
        slot->next_hop = make_peer(adva_b);
        slot->data = make_data((uint16_t)(first_sequence + i));
        slot->transaction_deadline_ms = now_ms + 5000u;
        slot->admission_order = (uint32_t)i + 1u;
    }
    link->active_custody_index = TAVRN_CUSTODY_SLOT_NONE;
}

static void make_rx_event(ble_mesh_sched_event_t *event, const uint8_t *adv,
                          uint8_t adv_len, const uint8_t outer_adva[6])
{
    memset(event, 0, sizeof(*event));
    event->type = BLE_MESH_SCHED_EVENT_RX_ADV;
    event->rssi_magnitude_db = 55u;
    memcpy(event->adv_addr, outer_adva, 6u);
    event->adv_len = adv_len;
    memcpy(event->adv_data, adv, adv_len);
}

static void make_tx_event(ble_mesh_sched_event_t *event,
                          ble_mesh_sched_event_type_t type,
                          uint16_t token, uint8_t completed_mask)
{
    memset(event, 0, sizeof(*event));
    event->type = type;
    event->tx_token = token;
    event->tx_requested_channel_mask = BLE_RADIO_ADV_CH_ALL;
    event->tx_completed_channel_mask = completed_mask;
}

static int admit_transit_data(tavrn_link_v2_t *link,
                              const tavrn_link_data_t *data,
                              const uint8_t transmitter[6], uint32_t now_ms)
{
    tavrn_codec_config_t codec;
    tavrn_decoded_frame_t frame;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t output;
    size_t encoded_len = 0u;

    memset(&codec, 0, sizeof(codec));
    codec.network_id = link->config.network_id;
    codec.local_peer = link->config.local_peer;
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = link->config.network_id;
    frame.transmitter = make_peer(transmitter);
    frame.detail.data.immediate_receiver = link->config.local_peer.logical_id;
    frame.detail.data.data = *data;
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
    memcpy(event.adv_addr, transmitter, 6u);
    if (tavrn_wire_v2_encode(&codec, &frame, event.adv_data,
                             sizeof(event.adv_data), &encoded_len) !=
            TAVRN_CODEC_OK ||
        encoded_len > BLE_ADV_MAX_DATA) {
        return 0;
    }
    event.adv_len = (uint8_t)encoded_len;
    if (tavrn_link_v2_on_scheduler_event(link, &event, now_ms, &output) !=
            TAVRN_LINK_STEP_EVENT ||
        output.type != TAVRN_LINK_EVENT_RX_DATA_CANDIDATE) {
        return 0;
    }
    return tavrn_link_v2_resolve_rx(link, output.detail.candidate.token,
                                    TAVRN_RX_ACCEPTED, now_ms, &output) ==
        TAVRN_LINK_RESOLVE_OK;
}

static int data_dedupe_contains(const tavrn_link_v2_t *link,
                                const tavrn_link_data_t *data)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_LINK_DATA_DEDUPE_CAPACITY; index++) {
        const tavrn_data_dedupe_entry_t *entry = &link->data_dedupe[index];

        if (entry->valid != 0u &&
            entry->origin.width == data->origin.width &&
            entry->origin.value == data->origin.value &&
            entry->data_seq == data->data_seq &&
            entry->app_kind == data->app_kind &&
            entry->app_source == data->app_source) {
            return 1;
        }
    }
    return 0;
}

static void remove_queued_hacks_for_origin(ble_mesh_scheduler_t *scheduler,
                                           uint16_t origin)
{
    uint8_t index;

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_queue_entry_t *entry = &scheduler->routed_tx_queue.entries[index];

        if (entry->occupied != 0u && entry->item.adv_len >= 17u &&
            entry->item.adv_data[11] == TAVRN_WIRE_HACK &&
            ((uint16_t)entry->item.adv_data[15] |
             ((uint16_t)entry->item.adv_data[16] << 8)) == origin) {
            (void)ble_mesh_tx_queue_remove(&scheduler->routed_tx_queue, index, NULL);
        }
    }
}

static tavrn_link_step_status_t start_two_peer_data(
    tavrn_link_v2_t *sender, ble_mesh_scheduler_t *sender_scheduler,
    tavrn_link_v2_t *receiver, const tavrn_link_data_t *data, uint32_t now_ms,
    tavrn_link_event_t *receiver_output)
{
    tavrn_link_event_t sender_output;
    ble_mesh_sched_event_t event;
    const ble_mesh_tx_item_t *item;
    uint8_t adv_data[BLE_ADV_MAX_DATA];
    uint8_t adv_len;
    tavrn_direct_peer_t next_hop = make_peer(adva_b);

    if (tavrn_link_v2_send_unicast(sender, &next_hop, data, now_ms,
                                   &sender_output) != TAVRN_LINK_SEND_OK ||
        tavrn_link_v2_dispatch(sender, now_ms, &sender_output) !=
            TAVRN_LINK_STEP_NO_EVENT) {
        return TAVRN_LINK_STEP_INVALID;
    }
    item = queued_wire_item(sender_scheduler, TAVRN_WIRE_DATA, active_token(sender));
    if (item == NULL) {
        return TAVRN_LINK_STEP_INVALID;
    }
    adv_len = item->adv_len;
    memcpy(adv_data, item->adv_data, adv_len);
    make_tx_event(&event, BLE_MESH_SCHED_EVENT_TX_DONE, active_token(sender),
                  BLE_RADIO_ADV_CH_ALL);
    if (tavrn_link_v2_on_scheduler_event(sender, &event, now_ms,
                                         &sender_output) != TAVRN_LINK_STEP_NO_EVENT) {
        return TAVRN_LINK_STEP_INVALID;
    }
    make_rx_event(&event, adv_data, adv_len, adva_a);
    return tavrn_link_v2_on_scheduler_event(receiver, &event, now_ms,
                                             receiver_output);
}

static tavrn_link_step_status_t finish_two_peer_hack(
    tavrn_link_v2_t *sender, const ble_mesh_scheduler_t *receiver_scheduler,
    tavrn_hack_status_t status, uint32_t now_ms, tavrn_link_event_t *output)
{
    ble_mesh_sched_event_t event;
    const ble_mesh_tx_item_t *item = queued_hack_item(receiver_scheduler, status);

    if (item == NULL) {
        return TAVRN_LINK_STEP_INVALID;
    }
    make_rx_event(&event, item->adv_data, item->adv_len, adva_b);
    return tavrn_link_v2_on_scheduler_event(sender, &event, now_ms, output);
}

static void test_bearer_04_outer_adva_before_mutation(void)
{
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t output;
    uint8_t directed[sizeof(data16)];
    uint8_t foreign[sizeof(data16)];

    if (!setup_link(&link, &scheduler, 0u)) {
        return;
    }
    memcpy(directed, data16, sizeof(directed));
    directed[14] = adva_a[0];
    directed[15] = adva_a[1];
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    CHECK("BEARER-04", tavrn_link_v2_on_scheduler_event(&link, &event, 1u,
                                                           &output) ==
                           TAVRN_LINK_STEP_EVENT);
    CHECK("BEARER-04", output.type == TAVRN_LINK_EVENT_RX_DATA_CANDIDATE);
    CHECK("BEARER-04", memcmp(output.detail.candidate.transmitter.adva.bytes,
                               adva_b, 6u) == 0);
    CHECK("BEARER-04", output.detail.candidate.transmitter.logical_id.value == 0x4bdcu);

    memcpy(foreign, directed, sizeof(foreign));
    foreign[10] = 0x2bu;
    make_rx_event(&event, foreign, (uint8_t)sizeof(foreign), adva_b);
    (void)tavrn_link_v2_on_scheduler_event(&link, &event, 2u, &output);
    CHECK("BEARER-04", output.type == TAVRN_LINK_EVENT_NONE);
    CHECK("BEARER-04", link.counters.rx_candidate == 1u);
    CHECK("BEARER-04", link.data_dedupe[0].valid == 0u);
}

static void test_link_01_only_data_is_hackable(void)
{
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t output;
    tavrn_codec_flood_t flood;

    if (!setup_link(&link, &scheduler, 0u)) {
        return;
    }
    make_rx_event(&event, hack16, (uint8_t)sizeof(hack16), adva_b);
    CHECK("LINK-01", tavrn_link_v2_on_scheduler_event(&link, &event, 1u,
                                                        &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-01", output.type != TAVRN_LINK_EVENT_RX_CONTROL);
    CHECK("LINK-01", link.counters.hack_unmatched == 1u);

    make_rx_event(&event, rrep_ack16, (uint8_t)sizeof(rrep_ack16), adva_b);
    CHECK("LINK-01", tavrn_link_v2_on_scheduler_event(&link, &event, 2u,
                                                        &output) ==
                       TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-01", output.type == TAVRN_LINK_EVENT_RX_CONTROL &&
                       output.detail.control.control.type == TAVRN_WIRE_E_RREP_ACK);

    memset(&flood, 0, sizeof(flood));
    flood.origin = make_peer(adva_a).logical_id;
    flood.ttl = 1u;
    flood.flood_class = 1u;
    CHECK("LINK-01", tavrn_link_v2_send_flood(&link, &flood, 3u, &output) ==
                       TAVRN_LINK_SEND_OK);
    CHECK("LINK-01", link.custody[0].phase == TAVRN_CUSTODY_FREE);
}

static void test_link_02_candidate_resolution_and_containment(void)
{
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t output;
    uint8_t directed[sizeof(data16)];
    uint8_t alternate[sizeof(data16)];
    tavrn_rx_candidate_token_t token;
    const ble_mesh_tx_item_t *queued_hack;

    if (!setup_link(&link, &scheduler, 0u)) {
        return;
    }
    memcpy(directed, data16, sizeof(directed));
    directed[14] = adva_a[0];
    directed[15] = adva_a[1];
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    CHECK("LINK-02", tavrn_link_v2_on_scheduler_event(&link, &event, 0u,
                                                        &output) ==
                       TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-02", output.type == TAVRN_LINK_EVENT_RX_DATA_CANDIDATE);
    CHECK("LINK-02", link.candidate_valid == 1u && link.data_dedupe[0].valid == 0u &&
                       link.counters.hack_accepted == 0u);
    token = output.detail.candidate.token;
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&link, token,
                                                TAVRN_RX_ACCEPTED, 0u, &output) ==
                       TAVRN_LINK_RESOLVE_OK);
    CHECK("LINK-02", link.candidate_valid == 0u && link.data_dedupe[0].valid == 1u &&
                       link.counters.rx_candidate_accepted == 1u);
    CHECK("LINK-02", queued_hack_status(&scheduler, TAVRN_HACK_ACCEPTED));
    queued_hack = queued_hack_item(&scheduler, TAVRN_HACK_ACCEPTED);
    CHECK("LINK-02", queued_hack != NULL &&
                        memcmp(queued_hack->adv_data, hack_to_b16,
                               sizeof(hack_to_b16) - 1u) == 0 &&
                        queued_hack->adv_data[23] == TAVRN_HACK_ACCEPTED);
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&link, token, TAVRN_RX_ACCEPTED,
                                                1u, &output) ==
                       TAVRN_LINK_RESOLVE_ALREADY_RESOLVED);

    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    CHECK("LINK-02", tavrn_link_v2_on_scheduler_event(&link, &event, 1u,
                                                        &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-02", link.counters.rx_committed_duplicate == 1u &&
                       link.counters.rx_candidate == 1u && link.counters.hack_duplicate == 1u);
    CHECK("LINK-02", queued_hack_status(&scheduler, TAVRN_HACK_DUPLICATE));
    queued_hack = queued_hack_item(&scheduler, TAVRN_HACK_DUPLICATE);
    CHECK("LINK-02", queued_hack != NULL && queued_hack->channel_mask ==
                       BLE_RADIO_ADV_CH_ALL && queued_hack->adv_data[23] ==
                       TAVRN_HACK_DUPLICATE);

    if (!setup_link(&link, &scheduler, 20u)) {
        return;
    }
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    (void)tavrn_link_v2_on_scheduler_event(&link, &event, 20u, &output);
    token = output.detail.candidate.token;
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&link, token, TAVRN_RX_BUSY,
                                                20u, &output) == TAVRN_LINK_RESOLVE_OK);
    CHECK("LINK-02", link.candidate_valid == 0u && link.data_dedupe[0].valid == 0u &&
                       link.counters.rx_candidate_busy == 1u);
    CHECK("LINK-02", queued_hack_status(&scheduler, TAVRN_HACK_BUSY));

    if (!setup_link(&link, &scheduler, 30u)) {
        return;
    }
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    (void)tavrn_link_v2_on_scheduler_event(&link, &event, 30u, &output);
    token = output.detail.candidate.token;
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&link, token, TAVRN_RX_REJECTED,
                                                30u, &output) == TAVRN_LINK_RESOLVE_OK);
    CHECK("LINK-02", link.candidate_valid == 0u && link.data_dedupe[0].valid == 0u &&
                       link.counters.rx_candidate_rejected == 1u);
    CHECK("LINK-02", queued_hack_status(&scheduler, TAVRN_HACK_REJECTED));

    memcpy(alternate, directed, sizeof(alternate));
    alternate[20] = 0x35u;
    if (!setup_link(&link, &scheduler, 100u)) {
        return;
    }
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    (void)tavrn_link_v2_on_scheduler_event(&link, &event, 100u, &output);
    make_rx_event(&event, alternate, (uint8_t)sizeof(alternate), adva_b);
    CHECK("LINK-02", tavrn_link_v2_on_scheduler_event(&link, &event, 101u,
                                                        &output) ==
                       TAVRN_LINK_STEP_ADDITIONAL_DATA_BUSY);
    CHECK("LINK-02", link.counters.rx_additional_data_busy == 1u &&
                       link.counters.rx_candidate == 1u);
    make_rx_event(&event, rrep_ack16, (uint8_t)sizeof(rrep_ack16), adva_b);
    CHECK("LINK-02", tavrn_link_v2_on_scheduler_event(&link, &event, 105u,
                                                        &output) ==
                       TAVRN_LINK_STEP_EVENT);
    make_tx_event(&event, BLE_MESH_SCHED_EVENT_TX_DONE, 0x77u, BLE_RADIO_ADV_CH37);
    CHECK("LINK-02", tavrn_link_v2_on_scheduler_event(&link, &event, 106u,
                                                        &output) != TAVRN_LINK_STEP_INVALID);
    CHECK("LINK-02", tavrn_link_v2_tick(&link, 109u, &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-02", tavrn_link_v2_tick(&link, 110u, &output) ==
                       TAVRN_LINK_STEP_CANDIDATE_TIMEOUT_BUSY);
    CHECK("LINK-02", link.candidate_valid == 0u &&
                       link.counters.rx_candidate_timeout_busy == 1u);
    CHECK("LINK-02", queued_hack_status(&scheduler, TAVRN_HACK_BUSY));
    CHECK("LINK-02", tavrn_link_v2_tick(&link, 110u, &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&link, 0x7fffu, TAVRN_RX_ACCEPTED,
                                                111u, &output) ==
                       TAVRN_LINK_RESOLVE_TOKEN_INVALID);
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&link, output.detail.candidate.token,
                                                TAVRN_RX_ACCEPTED, 111u, &output) !=
                       TAVRN_LINK_RESOLVE_OK);

    if (!setup_link(&link, &scheduler, 120u)) {
        return;
    }
    seed_active_queued(&link, 0x1234u, 120u);
    seed_candidate_eviction_queue(&scheduler);
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    (void)tavrn_link_v2_on_scheduler_event(&link, &event, 120u, &output);
    token = output.detail.candidate.token;
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&link, token, TAVRN_RX_ACCEPTED,
                                                120u, &output) ==
                       TAVRN_LINK_RESOLVE_OK);
    CHECK("LINK-02", output.type == TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED &&
                       output.detail.owned_data.data.data_seq == 0x1234u &&
                       output.detail.owned_data.next_hop.logical_id.value == 0x4bdcu);
    CHECK("LINK-02", queued_hack_status(&scheduler, TAVRN_HACK_ACCEPTED) &&
                       scheduler.routed_tx_queue.count == BLE_MESH_TX_QUEUE_CAPACITY &&
                       !queue_has_token(&scheduler, 0x41u));
}

static void test_link_02_candidate_match_seam(void)
{
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t output;
    tavrn_rx_data_candidate_t candidate;
    tavrn_link_counters_t counters_before;
    uint8_t directed[sizeof(data16)];
    uint8_t i;

    if (!setup_link(&link, &scheduler, 100u)) {
        return;
    }
    memcpy(directed, data16, sizeof(directed));
    directed[14] = adva_a[0];
    directed[15] = adva_a[1];
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    CHECK("LINK-02", tavrn_link_v2_on_scheduler_event(&link, &event, 100u,
                                                         &output) ==
                       TAVRN_LINK_STEP_EVENT &&
                       output.type == TAVRN_LINK_EVENT_RX_DATA_CANDIDATE);
    candidate = output.detail.candidate;
    counters_before = link.counters;
    CHECK("LINK-02", tavrn_link_v2_rx_candidate_matches(&link, &candidate) &&
                       link.candidate_valid != 0u &&
                       memcmp(&link.counters, &counters_before,
                              sizeof(link.counters)) == 0);

    candidate.token ^= 0x0001u;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    candidate = output.detail.candidate;
    candidate.transmitter.logical_id.width = TAVRN_IDENTITY_SID8;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    candidate = output.detail.candidate;
    candidate.transmitter.logical_id.value ^= 0x0001u;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    for (i = 0u; i < TAVRN_ADVA_LEN; i++) {
        candidate = output.detail.candidate;
        candidate.transmitter.adva.bytes[i] ^= 0x01u;
        CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    }
    candidate = output.detail.candidate;
    candidate.rssi_magnitude_db++;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));

    candidate = output.detail.candidate;
    candidate.data.origin.width = TAVRN_IDENTITY_SID8;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    candidate = output.detail.candidate;
    candidate.data.origin.value ^= 0x0001u;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    candidate = output.detail.candidate;
    candidate.data.final_destination.width = TAVRN_IDENTITY_SID8;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    candidate = output.detail.candidate;
    candidate.data.final_destination.value ^= 0x0001u;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    candidate = output.detail.candidate;
    candidate.data.data_seq ^= 0x0001u;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    candidate = output.detail.candidate;
    candidate.data.ttl++;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    candidate = output.detail.candidate;
    candidate.data.hops++;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    candidate = output.detail.candidate;
    candidate.data.app_kind ^= 0x01u;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    candidate = output.detail.candidate;
    candidate.data.app_source ^= 0x01u;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    candidate = output.detail.candidate;
    candidate.data.urgent ^= 0x01u;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    candidate = output.detail.candidate;
    candidate.data.app_len--;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    for (i = 0u; i < TAVRN_LINK_APP_BYTES; i++) {
        candidate = output.detail.candidate;
        candidate.data.app_bytes[i] ^= 0x01u;
        CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    }
    candidate = output.detail.candidate;
    candidate.data.ownership = TAVRN_DATA_ORIGINATED;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    candidate = output.detail.candidate;
    candidate.deadline_ms++;
    CHECK("LINK-02", !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
    CHECK("LINK-02", tavrn_link_v2_rx_candidate_matches(&link,
                                                           &output.detail.candidate));
    candidate = output.detail.candidate;
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&link, candidate.token,
                                                 TAVRN_RX_REJECTED, 100u,
                                                 &output) == TAVRN_LINK_RESOLVE_OK &&
                       !tavrn_link_v2_rx_candidate_matches(&link, &candidate));
}

static void test_link_03_busy_rejected_and_exact_correlation(void)
{
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t output;
    uint8_t accepted[sizeof(hack16)];
    uint8_t duplicate[sizeof(hack16)];
    uint8_t busy[sizeof(hack16)];
    uint8_t rejected[sizeof(hack16)];
    uint8_t correlation_mutation[sizeof(hack16)];
    uint8_t wrong_outer[6];
    static const uint8_t correlation_offsets[] = { 12u, 13u, 15u, 17u, 19u, 21u, 22u };
    uint8_t i;

    if (!setup_link(&link, &scheduler, 0u)) {
        return;
    }
    seed_active_sender(&link, 0x1234u, 0u);
    memcpy(accepted, hack16, sizeof(accepted));
    make_rx_event(&event, accepted, (uint8_t)sizeof(accepted), adva_b);
    CHECK("LINK-01", tavrn_link_v2_on_scheduler_event(&link, &event, 0u,
                                                        &output) ==
                       TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-01", output.type == TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED &&
                       output.detail.transferred_data.status == TAVRN_HACK_ACCEPTED &&
                       output.detail.transferred_data.data.data_seq == 0x1234u);

    seed_active_sender(&link, 0x1234u, 0u);
    memcpy(duplicate, hack16, sizeof(duplicate));
    duplicate[23] = TAVRN_HACK_DUPLICATE;
    make_rx_event(&event, duplicate, (uint8_t)sizeof(duplicate), adva_b);
    CHECK("LINK-01", tavrn_link_v2_on_scheduler_event(&link, &event, 0u,
                                                        &output) ==
                       TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-01", output.type == TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED &&
                       output.detail.transferred_data.status == TAVRN_HACK_DUPLICATE);

    seed_active_sender(&link, 0x1234u, 0u);
    memcpy(busy, hack16, sizeof(busy));
    busy[23] = TAVRN_HACK_BUSY;
    make_rx_event(&event, busy, (uint8_t)sizeof(busy), adva_b);
    CHECK("LINK-03", tavrn_link_v2_on_scheduler_event(&link, &event, 1u,
                                                        &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-03", link.custody[0].phase == TAVRN_CUSTODY_BUSY_WAIT &&
                       link.custody[0].retry_not_before_ms == 501u &&
                       link.custody[0].busy_response_count == 1u);
    CHECK("LINK-03", link.counters.retry_exhausted == 0u &&
                       link.counters.hack_busy == 1u);
    CHECK("LINK-03", tavrn_link_v2_dispatch(&link, 500u, &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-03", tavrn_link_v2_dispatch(&link, 501u, &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);

    for (i = 0u; i < sizeof(wrong_outer); i++) {
        seed_active_sender(&link, 0x1234u, (uint32_t)(1000u + i));
        memcpy(wrong_outer, adva_b, sizeof(wrong_outer));
        wrong_outer[i] ^= 0x01u;
        make_rx_event(&event, busy, (uint8_t)sizeof(busy), wrong_outer);
        CHECK("LINK-03", tavrn_link_v2_on_scheduler_event(&link, &event,
                                                            (uint32_t)(1010u + i),
                                                            &output) ==
                           TAVRN_LINK_STEP_NO_EVENT);
        CHECK("LINK-03", link.custody[0].phase == TAVRN_CUSTODY_ACTIVE_WAIT_HACK);
    }

    memcpy(rejected, hack16, sizeof(rejected));
    rejected[23] = TAVRN_HACK_REJECTED;
    make_rx_event(&event, rejected, (uint8_t)sizeof(rejected), adva_b);
    CHECK("LINK-03", tavrn_link_v2_on_scheduler_event(&link, &event, 1020u,
                                                        &output) ==
                       TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-03", output.type == TAVRN_LINK_EVENT_CUSTODY_REJECTED &&
                       output.detail.owned_data.data.data_seq == 0x1234u &&
                       output.detail.owned_data.data.app_len == 7u &&
                       memcmp(output.detail.owned_data.data.app_bytes, &data16[24], 7u) == 0);
    CHECK("LINK-03", link.counters.retry_exhausted == 0u);

    /* Each HACK correlation field, including outer AdvA, is independently exact. */
    for (i = 0u; i < sizeof(correlation_offsets); i++) {
        seed_active_sender(&link, 0x1234u, (uint32_t)(1100u + i));
        memcpy(correlation_mutation, hack16, sizeof(correlation_mutation));
        correlation_mutation[correlation_offsets[i]] ^= 0x01u;
        make_rx_event(&event, correlation_mutation,
                      (uint8_t)sizeof(correlation_mutation), adva_b);
        CHECK("LINK-03", tavrn_link_v2_on_scheduler_event(&link, &event,
                                                            (uint32_t)(1120u + i),
                                                            &output) ==
                           TAVRN_LINK_STEP_NO_EVENT);
        CHECK("LINK-03", link.custody[0].phase == TAVRN_CUSTODY_ACTIVE_WAIT_HACK);
    }
}

static void test_link_02_two_peer_hack_direction_and_statuses(void)
{
    tavrn_link_v2_t sender;
    tavrn_link_v2_t receiver;
    ble_mesh_scheduler_t sender_scheduler;
    ble_mesh_scheduler_t receiver_scheduler;
    tavrn_link_event_t receiver_output;
    tavrn_link_event_t sender_output;
    tavrn_link_data_t data;
    const ble_mesh_tx_item_t *hack;
    tavrn_rx_candidate_token_t token;

    if (!setup_link_for(&sender, &sender_scheduler, adva_a, 0u) ||
        !setup_link_for(&receiver, &receiver_scheduler, adva_b, 0u)) {
        return;
    }

    data = make_data(0x7000u);
    CHECK("LINK-02", start_two_peer_data(&sender, &sender_scheduler, &receiver,
                                            &data, 0u, &receiver_output) ==
                       TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-02", receiver_output.type == TAVRN_LINK_EVENT_RX_DATA_CANDIDATE);
    token = receiver_output.detail.candidate.token;
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&receiver, token, TAVRN_RX_ACCEPTED,
                                                 0u, &receiver_output) ==
                       TAVRN_LINK_RESOLVE_OK);
    hack = queued_hack_item(&receiver_scheduler, TAVRN_HACK_ACCEPTED);
    CHECK("LINK-02", hack != NULL && hack->adv_data[13] == adva_a[0] &&
                       hack->adv_data[14] == adva_a[1] &&
                       hack->adv_data[19] == (uint8_t)data.data_seq &&
                       hack->adv_data[20] == (uint8_t)(data.data_seq >> 8) &&
                       hack->adv_data[23] == TAVRN_HACK_ACCEPTED);
    CHECK("LINK-02", finish_two_peer_hack(&sender, &receiver_scheduler,
                                             TAVRN_HACK_ACCEPTED, 0u,
                                             &sender_output) == TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-02", sender_output.type == TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED &&
                       sender_output.detail.transferred_data.status == TAVRN_HACK_ACCEPTED &&
                       sender_output.detail.transferred_data.data.data_seq == data.data_seq);

    CHECK("LINK-02", start_two_peer_data(&sender, &sender_scheduler, &receiver,
                                            &data, 1u, &receiver_output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-02", receiver.counters.rx_committed_duplicate == 1u &&
                       receiver.counters.rx_candidate_accepted == 1u);
    hack = queued_hack_item(&receiver_scheduler, TAVRN_HACK_DUPLICATE);
    CHECK("LINK-02", hack != NULL && hack->adv_data[13] == adva_a[0] &&
                       hack->adv_data[14] == adva_a[1] &&
                       hack->adv_data[19] == (uint8_t)data.data_seq &&
                       hack->adv_data[20] == (uint8_t)(data.data_seq >> 8) &&
                       hack->adv_data[23] == TAVRN_HACK_DUPLICATE);
    CHECK("LINK-02", finish_two_peer_hack(&sender, &receiver_scheduler,
                                             TAVRN_HACK_DUPLICATE, 1u,
                                             &sender_output) == TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-02", sender_output.type == TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED &&
                       sender_output.detail.transferred_data.status == TAVRN_HACK_DUPLICATE &&
                       sender_output.detail.transferred_data.data.data_seq == data.data_seq);

    data = make_data(0x7001u);
    CHECK("LINK-03", start_two_peer_data(&sender, &sender_scheduler, &receiver,
                                            &data, 2u, &receiver_output) ==
                       TAVRN_LINK_STEP_EVENT);
    token = receiver_output.detail.candidate.token;
    CHECK("LINK-03", tavrn_link_v2_resolve_rx(&receiver, token, TAVRN_RX_BUSY,
                                                 2u, &receiver_output) ==
                       TAVRN_LINK_RESOLVE_OK);
    CHECK("LINK-03", finish_two_peer_hack(&sender, &receiver_scheduler,
                                             TAVRN_HACK_BUSY, 2u,
                                             &sender_output) == TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-03", sender.custody[0].phase == TAVRN_CUSTODY_BUSY_WAIT &&
                       sender.counters.retry_exhausted == 0u);

    data = make_data(0x7002u);
    CHECK("LINK-03", start_two_peer_data(&sender, &sender_scheduler, &receiver,
                                            &data, 3u, &receiver_output) ==
                       TAVRN_LINK_STEP_EVENT);
    token = receiver_output.detail.candidate.token;
    CHECK("LINK-03", tavrn_link_v2_resolve_rx(&receiver, token, TAVRN_RX_REJECTED,
                                                 3u, &receiver_output) ==
                       TAVRN_LINK_RESOLVE_OK);
    CHECK("LINK-03", finish_two_peer_hack(&sender, &receiver_scheduler,
                                             TAVRN_HACK_REJECTED, 3u,
                                             &sender_output) == TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-03", sender_output.type == TAVRN_LINK_EVENT_CUSTODY_REJECTED &&
                       sender_output.detail.owned_data.data.data_seq == data.data_seq &&
                       sender.counters.retry_exhausted == 0u);
}

static void test_link_04_slots_retries_terminal_ownership_and_fault_draining(void)
{
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t output;
    tavrn_direct_peer_t next_hop = make_peer(adva_b);
    tavrn_link_data_t fifth = make_data(0x5000u);
    uint8_t seen[4] = { 0u, 0u, 0u, 0u };
    uint8_t busy[sizeof(hack16)];
    uint8_t rejected[sizeof(hack16)];
    uint8_t i;
    uint16_t drained_sequence;

    if (!setup_link(&link, &scheduler, 0u)) {
        return;
    }
    seed_four_ready(&link, 0x2000u, 0u);
    CHECK("LINK-04", tavrn_link_v2_send_unicast(&link, &next_hop, &fifth, 0u,
                                                  &output) == TAVRN_LINK_SEND_NO_SLOT);
    CHECK("LINK-04", tavrn_link_v2_dispatch(&link, 0u, &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-04", link.active_custody_index != TAVRN_CUSTODY_SLOT_NONE &&
                       link.custody[link.active_custody_index].data.data_seq == 0x2000u);
    CHECK("LINK-04", link.custody[1].phase == TAVRN_CUSTODY_READY_NOT_ELIGIBLE &&
                       link.custody[2].phase == TAVRN_CUSTODY_READY_NOT_ELIGIBLE &&
                       link.custody[3].phase == TAVRN_CUSTODY_READY_NOT_ELIGIBLE);

    seed_active_queued(&link, 0x3000u, 0xffffff00u);
    CHECK("LINK-04", memcmp(link.custody[0].adv_data, data16, sizeof(data16)) == 0);
    make_tx_event(&event, BLE_MESH_SCHED_EVENT_TX_DONE, active_token(&link),
                  BLE_RADIO_ADV_CH_ALL);
    CHECK("LINK-04", tavrn_link_v2_on_scheduler_event(&link, &event, 0xffffff00u,
                                                        &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-04", tavrn_link_v2_tick(&link, 0xfffffffau, &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-04", tavrn_link_v2_dispatch(&link, 0xfffffffau, &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-04", memcmp(link.custody[0].adv_data, data16, sizeof(data16)) == 0);
    make_tx_event(&event, BLE_MESH_SCHED_EVENT_TX_DONE, active_token(&link),
                  BLE_RADIO_ADV_CH_ALL);
    CHECK("LINK-04", tavrn_link_v2_on_scheduler_event(&link, &event, 0x00000018u,
                                                        &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-04", tavrn_link_v2_tick(&link, 0x00000112u, &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-04", tavrn_link_v2_dispatch(&link, 0x00000112u, &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    make_tx_event(&event, BLE_MESH_SCHED_EVENT_TX_DONE, active_token(&link),
                  BLE_RADIO_ADV_CH_ALL);
    CHECK("LINK-04", tavrn_link_v2_on_scheduler_event(&link, &event, 0x00000130u,
                                                        &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-04", tavrn_link_v2_tick(&link, 0x0000022au, &output) ==
                       TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-04", output.type == TAVRN_LINK_EVENT_RETRY_EXHAUSTED &&
                       output.detail.owned_data.attempt_count == 3u &&
                       (uint32_t)(output.detail.owned_data.final_deadline_ms -
                                  0xffffff00u) <= 840u);
    CHECK("LINK-04", output.detail.owned_data.next_hop.logical_id.value == 0x4bdcu &&
                       memcmp(output.detail.owned_data.next_hop.adva.bytes, adva_b, 6u) == 0 &&
                       output.detail.owned_data.data.origin.width == TAVRN_IDENTITY_SID16 &&
                       output.detail.owned_data.data.final_destination.width == TAVRN_IDENTITY_SID16);

    if (!setup_link(&link, &scheduler, 100u)) {
        return;
    }
    seed_four_ready(&link, 0x4000u, 100u);
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_SERVICE_FAULT;
    event.fault = BLE_MESH_SCHED_FAULT_POLL_OVERRUN;
    (void)tavrn_link_v2_on_scheduler_event(&link, &event, 101u, &output);
    for (i = 0u; i < 4u; i++) {
        CHECK("LINK-04", tavrn_link_v2_tick(&link, 101u, &output) ==
                           TAVRN_LINK_STEP_EVENT);
        CHECK("LINK-04", output.type == TAVRN_LINK_EVENT_SERVICE_FAULT_TERMINAL &&
                           output.detail.owned_data.data.data_seq >= 0x4000u &&
                           output.detail.owned_data.data.data_seq <= 0x4003u);
        drained_sequence = output.detail.owned_data.data.data_seq;
        if (drained_sequence >= 0x4000u && drained_sequence <= 0x4003u) {
            seen[(uint8_t)(drained_sequence - 0x4000u)]++;
        }
    }
    CHECK("LINK-04", seen[0] == 1u && seen[1] == 1u && seen[2] == 1u && seen[3] == 1u);
    CHECK("LINK-04", tavrn_link_v2_tick(&link, 102u, &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-04", tavrn_link_v2_send_unicast(&link, &next_hop, &fifth, 103u,
                                                   &output) == TAVRN_LINK_SEND_MESH_FAULTED);

    /* Router/GTT acceptance of SID8 terminal ownership is intentionally Phase 2;
       this Phase 1 link test checks only the by-value logical-ID event contract. */
    if (!setup_link(&link, &scheduler, 200u)) {
        return;
    }
    seed_active_queued(&link, 0x6000u, 200u);
    link.custody[0].attempt_count = 1u;
    make_tx_event(&event, BLE_MESH_SCHED_EVENT_TX_FAILED, 0x41u, 0u);
    CHECK("LINK-04", tavrn_link_v2_on_scheduler_event(&link, &event, 201u,
                                                        &output) ==
                       TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-04", output.type == TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED &&
                       output.detail.owned_data.attempt_count == 1u &&
                       output.detail.owned_data.data.data_seq == 0x6000u &&
                       output.detail.owned_data.next_hop.logical_id.value == 0x4bdcu);
    CHECK("LINK-04", tavrn_link_v2_send_unicast(&link, &next_hop, &fifth, 202u,
                                                  &output) == TAVRN_LINK_SEND_OK);

    if (!setup_link(&link, &scheduler, 210u)) {
        return;
    }
    seed_active_queued(&link, 0x6001u, 210u);
    make_tx_event(&event, BLE_MESH_SCHED_EVENT_TX_DONE, 0x41u, BLE_RADIO_ADV_CH37);
    (void)tavrn_link_v2_on_scheduler_event(&link, &event, 211u, &output);
    make_tx_event(&event, BLE_MESH_SCHED_EVENT_RADIO_FAULT, 0x41u, BLE_RADIO_ADV_CH37);
    (void)tavrn_link_v2_on_scheduler_event(&link, &event, 212u, &output);
    CHECK("LINK-04", tavrn_link_v2_tick(&link, 212u, &output) ==
                       TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-04", output.type == TAVRN_LINK_EVENT_RADIO_FAULT_TERMINAL &&
                       output.detail.owned_data.completed_channel_mask == BLE_RADIO_ADV_CH37 &&
                       output.detail.owned_data.data.data_seq == 0x6001u &&
                       output.type != TAVRN_LINK_EVENT_RETRY_EXHAUSTED);

    if (!setup_link(&link, &scheduler, 220u)) {
        return;
    }
    seed_active_sender(&link, 0x6002u, 220u);
    link.custody[0].busy_response_count = 2u;
    memcpy(busy, hack16, sizeof(busy));
    busy[19] = 0x02u;
    busy[20] = 0x60u;
    busy[23] = TAVRN_HACK_BUSY;
    make_rx_event(&event, busy, (uint8_t)sizeof(busy), adva_b);
    CHECK("LINK-04", tavrn_link_v2_on_scheduler_event(&link, &event, 221u,
                                                        &output) ==
                       TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-04", output.type == TAVRN_LINK_EVENT_CUSTODY_BUSY_EXPIRED &&
                       output.detail.owned_data.data.data_seq == 0x6002u &&
                       output.type != TAVRN_LINK_EVENT_RETRY_EXHAUSTED);

    if (!setup_link(&link, &scheduler, 230u)) {
        return;
    }
    seed_active_sender(&link, 0x6003u, 230u);
    memcpy(rejected, hack16, sizeof(rejected));
    rejected[19] = 0x03u;
    rejected[20] = 0x60u;
    rejected[23] = TAVRN_HACK_REJECTED;
    make_rx_event(&event, rejected, (uint8_t)sizeof(rejected), adva_b);
    CHECK("LINK-04", tavrn_link_v2_on_scheduler_event(&link, &event, 231u,
                                                        &output) ==
                       TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-04", output.type == TAVRN_LINK_EVENT_CUSTODY_REJECTED &&
                       output.detail.owned_data.data.data_seq == 0x6003u &&
                       output.type != TAVRN_LINK_EVENT_RETRY_EXHAUSTED);
}

static void test_link_05_rrep_ack_never_cross_satisfies_data(void)
{
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t output;
    uint8_t wrong_sequence[sizeof(rrep_ack16)];

    if (!setup_link(&link, &scheduler, 0u)) {
        return;
    }
    seed_active_sender(&link, 0x1234u, 0u);
    make_rx_event(&event, rrep_ack16, (uint8_t)sizeof(rrep_ack16), adva_b);
    CHECK("LINK-05", tavrn_link_v2_on_scheduler_event(&link, &event, 1u,
                                                        &output) ==
                       TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-05", output.type == TAVRN_LINK_EVENT_RX_CONTROL &&
                       output.detail.control.control.type == TAVRN_WIRE_E_RREP_ACK &&
                       link.custody[0].phase == TAVRN_CUSTODY_ACTIVE_WAIT_HACK &&
                       link.counters.custody_transferred == 0u);
    memcpy(wrong_sequence, rrep_ack16, sizeof(wrong_sequence));
    wrong_sequence[17] = 0x04u;
    make_rx_event(&event, wrong_sequence, (uint8_t)sizeof(wrong_sequence), adva_b);
    CHECK("LINK-05", tavrn_link_v2_on_scheduler_event(&link, &event, 2u,
                                                        &output) ==
                       TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-05", output.detail.control.control.pdu[10] == 0x04u &&
                       link.counters.custody_transferred == 0u);
}

static void test_link_06_directed_data_and_controlled_flood(void)
{
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t output;
    uint8_t directed[sizeof(data16)];
    uint8_t ttl_zero[sizeof(flood16)];
    uint8_t hops_limit[sizeof(flood16)];
    uint8_t foreign[sizeof(flood16)];
    tavrn_codec_flood_t flood;
    tavrn_direct_peer_t broadcast = make_peer(adva_b);
    tavrn_link_data_t data = make_data(0x7777u);
    uint8_t i;

    if (!setup_link(&link, &scheduler, 0u)) {
        return;
    }
    make_rx_event(&event, flood16, (uint8_t)sizeof(flood16), adva_b);
    CHECK("LINK-06", tavrn_link_v2_on_scheduler_event(&link, &event, 1u,
                                                        &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-06", link.flood_dedupe[0].valid == 1u &&
                       link.flood_dedupe[0].origin.value == 0x4218u &&
                       link.flood_dedupe[0].sequence == 0x0102u);
    CHECK("LINK-06", scheduler.routed_tx_queue.count == 1u &&
                       scheduler.routed_tx_queue.entries[0].item.priority ==
                           BLE_MESH_TX_PRIORITY_RELAY);
    CHECK("LINK-06", scheduler.routed_tx_queue.entries[0].item.channel_mask ==
                       BLE_RADIO_ADV_CH_ALL &&
                       scheduler.routed_tx_queue.entries[0].item.adv_data[13] == 0x31u &&
                       memcmp(&scheduler.routed_tx_queue.entries[0].item.adv_data[20],
                              &flood16[20], 3u) == 0 &&
                       scheduler.routed_tx_queue.entries[0].item.not_before_ms >= 21u &&
                       scheduler.routed_tx_queue.entries[0].item.not_before_ms <= 121u);
    CHECK("LINK-06", link.counters.hack_accepted == 0u &&
                       link.custody[0].phase == TAVRN_CUSTODY_FREE &&
                       link.counters.rx_flood_committed == 1u &&
                       link.counters.rx_flood_duplicate == 0u);
    make_rx_event(&event, flood16, (uint8_t)sizeof(flood16), adva_b);
    CHECK("LINK-06", tavrn_link_v2_on_scheduler_event(&link, &event, 2u,
                                                        &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-06", link.flood_dedupe[0].valid == 1u &&
                       link.counters.rx_flood_committed == 1u &&
                       link.counters.rx_flood_duplicate == 1u &&
                       scheduler.routed_tx_queue.count == 1u);
    memcpy(ttl_zero, flood16, sizeof(ttl_zero));
    ttl_zero[13] = 0x0fu;
    ttl_zero[16] = 0x03u;
    make_rx_event(&event, ttl_zero, (uint8_t)sizeof(ttl_zero), adva_b);
    CHECK("LINK-06", tavrn_link_v2_on_scheduler_event(&link, &event, 3u,
                                                        &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-06", scheduler.routed_tx_queue.count == 1u);

    memcpy(hops_limit, flood16, sizeof(hops_limit));
    hops_limit[13] = 0x1fu;
    hops_limit[16] = 0x04u;
    make_rx_event(&event, hops_limit, (uint8_t)sizeof(hops_limit), adva_b);
    CHECK("LINK-06", tavrn_link_v2_on_scheduler_event(&link, &event, 4u,
                                                        &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-06", scheduler.routed_tx_queue.count == 1u);

    memcpy(foreign, flood16, sizeof(foreign));
    foreign[10] = 0x2bu;
    make_rx_event(&event, foreign, (uint8_t)sizeof(foreign), adva_b);
    CHECK("LINK-06", tavrn_link_v2_on_scheduler_event(&link, &event, 5u,
                                                        &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-06", link.flood_dedupe[0].sequence == 0x0102u &&
                       scheduler.routed_tx_queue.count == 1u);

    for (i = 0u; i < TAVRN_LINK_FLOOD_DEDUPE_CAPACITY; i++) {
        link.flood_dedupe[i].valid = 1u;
        link.flood_dedupe[i].expires_at_ms = 0xfffffff0u;
    }
    memcpy(foreign, flood16, sizeof(foreign));
    foreign[16] = 0x03u;
    make_rx_event(&event, foreign, (uint8_t)sizeof(foreign), adva_b);
    CHECK("LINK-06", tavrn_link_v2_on_scheduler_event(&link, &event, 0x00000010u,
                                                        &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-06", link.flood_dedupe[0].valid == 1u &&
                       link.flood_dedupe[0].expires_at_ms > 0x00000010u);

    make_rx_event(&event, data16, (uint8_t)sizeof(data16), adva_b);
    CHECK("LINK-06", tavrn_link_v2_on_scheduler_event(&link, &event, 6u,
                                                        &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-06", link.counters.rx_wrong_next_hop == 1u &&
                       link.counters.rx_candidate == 0u);
    memcpy(directed, data16, sizeof(directed));
    directed[14] = adva_a[0];
    directed[15] = adva_a[1];
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    CHECK("LINK-06", tavrn_link_v2_on_scheduler_event(&link, &event, 7u,
                                                        &output) ==
                       TAVRN_LINK_STEP_EVENT);
    CHECK("LINK-06", output.type == TAVRN_LINK_EVENT_RX_DATA_CANDIDATE);

    memset(&flood, 0, sizeof(flood));
    flood.origin = make_peer(adva_a).logical_id;
    flood.flood_seq = 0x0102u;
    flood.ttl = 15u;
    flood.flood_class = 1u;
    flood.body_len = 3u;
    memcpy(flood.body, &flood16[20], 3u);
    CHECK("LINK-06", link.config.flood_jitter_min_ms == 20u &&
                       link.config.flood_jitter_max_ms == 120u);
    CHECK("LINK-06", tavrn_link_v2_send_flood(&link, &flood, 8u, &output) ==
                       TAVRN_LINK_SEND_OK);
    CHECK("LINK-06", link.custody[0].phase == TAVRN_CUSTODY_FREE);
    broadcast.logical_id.value = 0xffffu;
    CHECK("LINK-06", tavrn_link_v2_send_unicast(&link, &broadcast, &data, 9u,
                                                  &output) == TAVRN_LINK_SEND_INVALID);
    CHECK("LINK-06", link.custody[0].phase == TAVRN_CUSTODY_FREE &&
                       link.counters.hack_accepted == 0u);
}

static void seed_full_data_dedupe(tavrn_link_v2_t *link, uint32_t now_ms,
                                  uint8_t custody_pinned)
{
    uint8_t i;

    for (i = 0u; i < TAVRN_LINK_DATA_DEDUPE_CAPACITY; i++) {
        tavrn_data_dedupe_entry_t *entry = &link->data_dedupe[i];

        memset(entry, 0, sizeof(*entry));
        entry->valid = 1u;
        entry->custody_pinned = custody_pinned;
        entry->origin = make_peer(adva_a).logical_id;
        entry->data_seq = (uint16_t)(0x8000u + i);
        entry->app_kind = 0x01u;
        entry->app_source = 0x07u;
        entry->expires_at_ms = now_ms + 0x20u + i;
    }
}

static void seed_full_flood_dedupe(tavrn_link_v2_t *link, uint32_t now_ms)
{
    uint8_t i;

    for (i = 0u; i < TAVRN_LINK_FLOOD_DEDUPE_CAPACITY; i++) {
        tavrn_flood_dedupe_entry_t *entry = &link->flood_dedupe[i];

        memset(entry, 0, sizeof(*entry));
        entry->valid = 1u;
        entry->frame_type = TAVRN_WIRE_FLOOD;
        entry->origin = make_peer(adva_a).logical_id;
        entry->sequence = (uint16_t)(0x8000u + i);
        entry->expires_at_ms = now_ms + 0x20u + i;
    }
}

static void test_link_02_dedupe_replacement(void)
{
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t output;
    tavrn_rx_candidate_token_t token;
    uint8_t directed[sizeof(data16)];
    const ble_mesh_tx_item_t *hack;
    const uint32_t now_ms = 0xfffffff0u;

    memcpy(directed, data16, sizeof(directed));
    directed[14] = adva_a[0];
    directed[15] = adva_a[1];

    if (!setup_link(&link, &scheduler, now_ms)) {
        return;
    }
    seed_full_data_dedupe(&link, now_ms, 0u);
    link.data_dedupe[3].expires_at_ms = now_ms + 3u;
    link.data_dedupe[5].expires_at_ms = now_ms + 3u;
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    CHECK("LINK-02", tavrn_link_v2_on_scheduler_event(&link, &event, now_ms,
                                                         &output) ==
                       TAVRN_LINK_STEP_EVENT);
    token = output.detail.candidate.token;
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&link, token, TAVRN_RX_ACCEPTED,
                                                 now_ms, &output) ==
                       TAVRN_LINK_RESOLVE_OK);
    CHECK("LINK-02", link.data_dedupe[3].valid == 1u &&
                       link.data_dedupe[3].data_seq == 0x1234u &&
                       link.data_dedupe[5].data_seq == 0x8005u);

    if (!setup_link(&link, &scheduler, now_ms)) {
        return;
    }
    seed_full_data_dedupe(&link, now_ms, 0u);
    link.data_dedupe[0].expires_at_ms = now_ms;
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    CHECK("LINK-02", tavrn_link_v2_on_scheduler_event(&link, &event, now_ms,
                                                         &output) ==
                       TAVRN_LINK_STEP_EVENT);
    token = output.detail.candidate.token;
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&link, token, TAVRN_RX_ACCEPTED,
                                                 now_ms, &output) ==
                       TAVRN_LINK_RESOLVE_OK);
    CHECK("LINK-02", link.data_dedupe[0].valid == 1u &&
                       link.data_dedupe[0].data_seq == 0x1234u);

    if (!setup_link(&link, &scheduler, now_ms)) {
        return;
    }
    seed_full_data_dedupe(&link, now_ms, 1u);
    link.data_dedupe[0].expires_at_ms = now_ms;
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    CHECK("LINK-02", tavrn_link_v2_on_scheduler_event(&link, &event, now_ms,
                                                          &output) ==
                       TAVRN_LINK_STEP_DATA_DEDUPE_BUSY);
    hack = queued_hack_item(&scheduler, TAVRN_HACK_BUSY);
    CHECK("LINK-02", link.candidate_valid == 0u && hack != NULL &&
                       tavrn_link_v2_counters(&link)->rx_data_dedupe_busy == 1u &&
                       hack->adv_data[13] == adva_b[0] &&
                       hack->adv_data[14] == adva_b[1] &&
                       link.data_dedupe[0].custody_pinned == 1u &&
                       link.data_dedupe[0].data_seq == 0x8000u);

    if (!setup_link(&link, &scheduler, now_ms)) {
        return;
    }
    seed_full_flood_dedupe(&link, now_ms);
    link.flood_dedupe[3].expires_at_ms = now_ms + 3u;
    link.flood_dedupe[5].expires_at_ms = now_ms + 3u;
    make_rx_event(&event, flood16, (uint8_t)sizeof(flood16), adva_b);
    CHECK("LINK-06", tavrn_link_v2_on_scheduler_event(&link, &event, now_ms,
                                                         &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-06", link.flood_dedupe[3].valid == 1u &&
                       link.flood_dedupe[3].sequence == 0x0102u &&
                       link.flood_dedupe[5].sequence == 0x8005u);

    if (!setup_link(&link, &scheduler, now_ms)) {
        return;
    }
    seed_full_flood_dedupe(&link, now_ms);
    link.flood_dedupe[0].expires_at_ms = now_ms;
    make_rx_event(&event, flood16, (uint8_t)sizeof(flood16), adva_b);
    CHECK("LINK-06", tavrn_link_v2_on_scheduler_event(&link, &event, now_ms,
                                                         &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-06", link.flood_dedupe[0].valid == 1u &&
                       link.flood_dedupe[0].sequence == 0x0102u);
}

static void test_link_02_hack_turnaround_config_bounds(void)
{
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    tavrn_link_config_t config = make_config();

    memset(&scheduler, 0, sizeof(scheduler));
    ble_mesh_scheduler_init(&scheduler, 0u, adva_a);
    config.hack_turnaround_ms = 0u;
    CHECK("LINK-02", tavrn_link_v2_init(&link, &scheduler, &config, 0u) ==
                       TAVRN_LINK_INIT_INVALID_CONFIG);

    config = make_config();
    config.hack_turnaround_ms = 0x80000000u;
    CHECK("LINK-02", tavrn_link_v2_init(&link, &scheduler, &config, 0u) ==
                       TAVRN_LINK_INIT_INVALID_CONFIG);

    config = make_config();
    config.hack_turnaround_ms = UINT32_MAX;
    CHECK("LINK-02", tavrn_link_v2_init(&link, &scheduler, &config, 0u) ==
                       TAVRN_LINK_INIT_INVALID_CONFIG);
}

static void check_queued_hack_due(const ble_mesh_scheduler_t *scheduler,
                                  tavrn_hack_status_t status,
                                  uint32_t expected_not_before_ms)
{
    const ble_mesh_tx_item_t *item = queued_hack_item(scheduler, (uint8_t)status);

    CHECK("LINK-02", item != NULL && item->not_before_ms == expected_not_before_ms);
}

static void test_link_02_hack_turnaround_enqueue_paths(void)
{
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t output;
    tavrn_rx_candidate_token_t token;
    uint8_t directed[sizeof(data16)];
    uint8_t alternate[sizeof(data16)];
    const uint32_t turnaround_ms = 8u;

    memcpy(directed, data16, sizeof(directed));
    directed[14] = adva_a[0];
    directed[15] = adva_a[1];

    if (!setup_link(&link, &scheduler, 100u)) {
        return;
    }
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    CHECK("LINK-02", tavrn_link_v2_on_scheduler_event(&link, &event, 100u,
                                                         &output) ==
                       TAVRN_LINK_STEP_EVENT);
    token = output.detail.candidate.token;
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&link, token, TAVRN_RX_ACCEPTED,
                                                 100u, &output) ==
                       TAVRN_LINK_RESOLVE_OK);
    check_queued_hack_due(&scheduler, TAVRN_HACK_ACCEPTED, 100u + turnaround_ms);

    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    CHECK("LINK-02", tavrn_link_v2_on_scheduler_event(&link, &event, 101u,
                                                         &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    check_queued_hack_due(&scheduler, TAVRN_HACK_DUPLICATE, 101u + turnaround_ms);

    if (!setup_link(&link, &scheduler, 200u)) {
        return;
    }
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    (void)tavrn_link_v2_on_scheduler_event(&link, &event, 200u, &output);
    token = output.detail.candidate.token;
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&link, token, TAVRN_RX_BUSY,
                                                 200u, &output) ==
                       TAVRN_LINK_RESOLVE_OK);
    check_queued_hack_due(&scheduler, TAVRN_HACK_BUSY, 200u + turnaround_ms);

    if (!setup_link(&link, &scheduler, 300u)) {
        return;
    }
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    (void)tavrn_link_v2_on_scheduler_event(&link, &event, 300u, &output);
    token = output.detail.candidate.token;
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&link, token, TAVRN_RX_REJECTED,
                                                 300u, &output) ==
                       TAVRN_LINK_RESOLVE_OK);
    check_queued_hack_due(&scheduler, TAVRN_HACK_REJECTED, 300u + turnaround_ms);

    if (!setup_link(&link, &scheduler, 400u)) {
        return;
    }
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    (void)tavrn_link_v2_on_scheduler_event(&link, &event, 400u, &output);
    memcpy(alternate, directed, sizeof(alternate));
    alternate[20] ^= 0x01u;
    make_rx_event(&event, alternate, (uint8_t)sizeof(alternate), adva_b);
    CHECK("LINK-02", tavrn_link_v2_on_scheduler_event(&link, &event, 401u,
                                                         &output) ==
                       TAVRN_LINK_STEP_ADDITIONAL_DATA_BUSY);
    check_queued_hack_due(&scheduler, TAVRN_HACK_BUSY, 401u + turnaround_ms);

    if (!setup_link(&link, &scheduler, 500u)) {
        return;
    }
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    (void)tavrn_link_v2_on_scheduler_event(&link, &event, 500u, &output);
    CHECK("LINK-02", tavrn_link_v2_tick(&link, 510u, &output) ==
                       TAVRN_LINK_STEP_CANDIDATE_TIMEOUT_BUSY);
    check_queued_hack_due(&scheduler, TAVRN_HACK_BUSY, 510u + turnaround_ms);

    if (!setup_link(&link, &scheduler, 600u)) {
        return;
    }
    seed_full_data_dedupe(&link, 600u, 1u);
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    CHECK("LINK-02", tavrn_link_v2_on_scheduler_event(&link, &event, 600u,
                                                          &output) ==
                       TAVRN_LINK_STEP_DATA_DEDUPE_BUSY &&
                       tavrn_link_v2_counters(&link)->rx_data_dedupe_busy == 1u);
    check_queued_hack_due(&scheduler, TAVRN_HACK_BUSY, 600u + turnaround_ms);

    if (!setup_link(&link, &scheduler, UINT32_MAX - 3u)) {
        return;
    }
    make_rx_event(&event, directed, (uint8_t)sizeof(directed), adva_b);
    (void)tavrn_link_v2_on_scheduler_event(&link, &event, UINT32_MAX - 3u, &output);
    token = output.detail.candidate.token;
    CHECK("LINK-02", tavrn_link_v2_resolve_rx(&link, token, TAVRN_RX_ACCEPTED,
                                                 UINT32_MAX - 3u, &output) ==
                       TAVRN_LINK_RESOLVE_OK);
    check_queued_hack_due(&scheduler, TAVRN_HACK_ACCEPTED, 4u);
}

static void test_link_04_tx_done_response_deadline(void)
{
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t output;

    CHECK("LINK-04", tron_timer_config.radio_tx_event_bound_ms == 8u &&
                       tron_timer_config.link_hack_timeout_ms == 250u &&
                       tron_timer_config.link_response_window_sum_ms == 750u &&
                       tron_timer_config.link_no_response_wall_bound_ms == 840u);
    if (!setup_link(&link, &scheduler, 100u)) {
        return;
    }
    seed_active_queued(&link, 0x7100u, 100u);
    make_tx_event(&event, BLE_MESH_SCHED_EVENT_TX_DONE, active_token(&link),
                  BLE_RADIO_ADV_CH_ALL);
    CHECK("LINK-04", tavrn_link_v2_on_scheduler_event(&link, &event, 100u,
                                                         &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-04", link.custody[0].response_deadline_ms == 350u);

    if (!setup_link(&link, &scheduler, 0xfffffff0u)) {
        return;
    }
    seed_active_queued(&link, 0x7101u, 0xfffffff0u);
    make_tx_event(&event, BLE_MESH_SCHED_EVENT_TX_DONE, active_token(&link),
                  BLE_RADIO_ADV_CH_ALL);
    CHECK("LINK-04", tavrn_link_v2_on_scheduler_event(&link, &event, 0xfffffff0u,
                                                         &output) ==
                       TAVRN_LINK_STEP_NO_EVENT);
    CHECK("LINK-04", link.custody[0].response_deadline_ms == 0x000000eau);
}

static tavrn_validated_control_t make_aodv_control(tavrn_wire_type_t type)
{
    tavrn_validated_control_t control;

    memset(&control, 0, sizeof(control));
    control.type = type;
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = 0x2au;
    control.pdu[4] = (uint8_t)type;
    control.pdu[6] = 0xf0u;
    if (type == TAVRN_WIRE_E_RREQ) {
        control.pdu_len = 17u;
        control.pdu[5] = 0x10u;
        control.pdu[7] = adva_a[0]; control.pdu[8] = adva_a[1];
        control.pdu[9] = 1u;
        control.pdu[11] = 0x11u; control.pdu[12] = 0x22u;
        control.pdu[15] = 1u;
    } else if (type == TAVRN_WIRE_E_RREP) {
        control.pdu_len = 19u;
        control.pdu[7] = adva_b[0]; control.pdu[8] = adva_b[1];
        control.pdu[9] = 0x11u; control.pdu[10] = 0x22u;
        control.pdu[11] = 2u;
        control.pdu[13] = adva_a[0]; control.pdu[14] = adva_a[1];
        control.pdu[15] = 1u;
        control.pdu[17] = 0xe8u; control.pdu[18] = 3u;
    } else if (type == TAVRN_WIRE_E_RERR) {
        control.pdu_len = 16u;
        control.pdu[7] = adva_a[0]; control.pdu[8] = adva_a[1];
        control.pdu[9] = 1u;
        control.pdu[11] = 1u;
        control.pdu[12] = 0x11u; control.pdu[13] = 0x22u;
        control.pdu[14] = 2u;
    } else {
        control.pdu_len = 16u;
        control.pdu[6] = adva_b[0]; control.pdu[7] = adva_b[1];
        control.pdu[8] = 0x11u; control.pdu[9] = 0x22u;
        control.pdu[10] = 2u;
        control.pdu[12] = adva_a[0]; control.pdu[13] = adva_a[1];
        control.pdu[14] = 1u;
    }
    return control;
}

static void test_link_07_best_effort_aodv_control_send(void)
{
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    tavrn_link_event_t output;
    tavrn_validated_control_t control;
    const ble_mesh_tx_item_t *item;
    tavrn_direct_peer_t peer_b = make_peer(adva_b);

    if (!setup_link(&link, &scheduler, 100u)) {
        return;
    }
    control = make_aodv_control(TAVRN_WIRE_E_RREQ);
    CHECK("LINK-07", tavrn_link_v2_send_control(&link, &control, NULL, 1u, 100u,
                                                  &output) == TAVRN_LINK_SEND_OK);
    item = queued_wire_item(&scheduler, TAVRN_WIRE_E_RREQ, BLE_MESH_TX_TOKEN_NONE);
    CHECK("LINK-07", item != NULL && item->priority == BLE_MESH_TX_PRIORITY_CONTROL &&
                       item->service_class == BLE_MESH_TX_SERVICE_BEST_EFFORT &&
                       item->token == BLE_MESH_TX_TOKEN_NONE && item->not_before_ms == 100u &&
                       link.custody[0].phase == TAVRN_CUSTODY_FREE);
    CHECK("LINK-07", tavrn_link_v2_send_control(&link, &control, &peer_b,
                                                  1u, 100u, &output) == TAVRN_LINK_SEND_INVALID);

    if (!setup_link(&link, &scheduler, 101u)) {
        return;
    }
    control = make_aodv_control(TAVRN_WIRE_E_RERR);
    CHECK("LINK-07", tavrn_link_v2_send_control(&link, &control, NULL, 1u, 101u,
                                                  &output) == TAVRN_LINK_SEND_OK);
    item = queued_wire_item(&scheduler, TAVRN_WIRE_E_RERR, BLE_MESH_TX_TOKEN_NONE);
    CHECK("LINK-07", item != NULL && item->priority == BLE_MESH_TX_PRIORITY_CONTROL);

    if (!setup_link(&link, &scheduler, 102u)) {
        return;
    }
    control = make_aodv_control(TAVRN_WIRE_E_RREP);
    CHECK("LINK-07", tavrn_link_v2_send_control(&link, &control, &peer_b,
                                                  0u, 102u, &output) == TAVRN_LINK_SEND_OK);
    item = queued_wire_item(&scheduler, TAVRN_WIRE_E_RREP, BLE_MESH_TX_TOKEN_NONE);
    CHECK("LINK-07", item != NULL && item->adv_data[14] == adva_b[0] &&
                       item->adv_data[15] == adva_b[1]);
    control.pdu[7] = adva_a[0];
    CHECK("LINK-07", tavrn_link_v2_send_control(&link, &control, &peer_b,
                                                  0u, 102u, &output) == TAVRN_LINK_SEND_INVALID);

    if (!setup_link(&link, &scheduler, 103u)) {
        return;
    }
    control = make_aodv_control(TAVRN_WIRE_E_RREP_ACK);
    CHECK("LINK-07", tavrn_link_v2_send_control(&link, &control, &peer_b,
                                                  0u, 103u, &output) == TAVRN_LINK_SEND_OK);
    item = queued_wire_item(&scheduler, TAVRN_WIRE_E_RREP_ACK, BLE_MESH_TX_TOKEN_NONE);
    CHECK("LINK-07", item != NULL && item->adv_data[13] == adva_b[0] &&
                       item->adv_data[14] == adva_b[1]);

    if (!setup_link(&link, &scheduler, 104u)) {
        return;
    }
    seed_active_queued(&link, 0x7400u, 104u);
    seed_candidate_eviction_queue(&scheduler);
    control = make_aodv_control(TAVRN_WIRE_E_RREQ);
    CHECK("LINK-07", tavrn_link_v2_send_control(&link, &control, NULL, 1u, 104u,
                                                  &output) == TAVRN_LINK_SEND_OK);
    CHECK("LINK-07", output.type == TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED &&
                       output.detail.owned_data.data.data_seq == 0x7400u);

    if (!setup_link(&link, &scheduler, 105u)) {
        return;
    }
    for (uint8_t i = 0u; i < BLE_MESH_TX_QUEUE_CAPACITY; i++) {
        ble_mesh_tx_queue_entry_t *entry = &scheduler.routed_tx_queue.entries[i];

        entry->occupied = 1u;
        entry->ordinal = (uint32_t)i + 1u;
        entry->item.adv_len = 3u;
        entry->item.channel_mask = BLE_RADIO_ADV_CH_ALL;
        entry->item.priority = BLE_MESH_TX_PRIORITY_HACK;
        entry->item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
    }
    scheduler.routed_tx_queue.count = BLE_MESH_TX_QUEUE_CAPACITY;
    control = make_aodv_control(TAVRN_WIRE_E_RERR);
    CHECK("LINK-07", tavrn_link_v2_send_control(&link, &control, NULL, 1u, 105u,
                                                  &output) == TAVRN_LINK_SEND_BUSY &&
                       output.type == TAVRN_LINK_EVENT_NONE &&
                       link.custody[0].phase == TAVRN_CUSTODY_FREE);
}

static void test_serial_04_peer_cleanup_owns_transit_data_by_endpoints(void)
{
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    tavrn_link_event_t output;
    tavrn_direct_peer_t peer_b = make_peer(adva_b);
    tavrn_direct_peer_t peer_c = make_peer(adva_c);
    tavrn_link_data_t unrelated = make_transit_data(adva_c, adva_d, 0x7100u);
    uint8_t pass;

    if (!setup_link(&link, &scheduler, 0u) ||
        !admit_transit_data(&link, &unrelated, adva_c, 1u)) {
        CHECK("LINK-03", 0);
        return;
    }
    CHECK("LINK-03", data_dedupe_contains(&link, &unrelated) &&
                       link.data_dedupe[0].custody_pinned != 0u);

    for (pass = 0u; pass < 3u; pass++) {
        tavrn_link_data_t b_owned[4];
        tavrn_link_data_t candidate;
        uint8_t index;

        b_owned[0] = make_transit_data(adva_b, adva_d,
                                        (uint16_t)(0x7200u + 4u * pass));
        b_owned[1] = make_transit_data(adva_d, adva_b,
                                        (uint16_t)(0x7201u + 4u * pass));
        b_owned[2] = make_transit_data(adva_b, adva_d,
                                        (uint16_t)(0x7202u + 4u * pass));
        b_owned[3] = make_transit_data(adva_d, adva_b,
                                        (uint16_t)(0x7203u + 4u * pass));
        for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
            CHECK("LINK-03", admit_transit_data(&link, &b_owned[index], adva_c,
                                                   (uint32_t)(10u + 8u * pass + index)) &&
                               tavrn_link_v2_send_unicast(
                                   &link, &peer_c, &b_owned[index],
                                   (uint32_t)(20u + 8u * pass + index),
                                   &output) == TAVRN_LINK_SEND_OK);
            remove_queued_hacks_for_origin(&scheduler,
                                           b_owned[index].origin.value);
        }
        CHECK("LINK-03", tavrn_link_v2_dispatch(&link,
                                                   (uint32_t)(40u + 8u * pass),
                                                   &output) == TAVRN_LINK_STEP_NO_EVENT &&
                           link.active_custody_index == 0u &&
                           link.custody[0].phase == TAVRN_CUSTODY_ACTIVE_QUEUED);
        link.custody[1].phase = TAVRN_CUSTODY_BUSY_WAIT;
        link.custody[1].retry_not_before_ms = 0u;
        candidate = make_transit_data(adva_d, adva_b,
                                      (uint16_t)(0x7300u + pass));

        /* The fifth DATA must remain unresolved rather than consuming another
         * fixed custody slot; it is B-owned only through its final endpoint. */
        {
            tavrn_codec_config_t codec;
            tavrn_decoded_frame_t frame;
            ble_mesh_sched_event_t event;
            size_t encoded_len = 0u;

            memset(&codec, 0, sizeof(codec));
            codec.network_id = link.config.network_id;
            codec.local_peer = link.config.local_peer;
            memset(&frame, 0, sizeof(frame));
            frame.type = TAVRN_WIRE_DATA;
            frame.network_id = link.config.network_id;
            frame.transmitter = peer_c;
            frame.detail.data.immediate_receiver = link.config.local_peer.logical_id;
            frame.detail.data.data = candidate;
            memset(&event, 0, sizeof(event));
            event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
            memcpy(event.adv_addr, adva_c, 6u);
            CHECK("LINK-03", tavrn_wire_v2_encode(&codec, &frame, event.adv_data,
                                                    sizeof(event.adv_data),
                                                    &encoded_len) == TAVRN_CODEC_OK &&
                               encoded_len <= BLE_ADV_MAX_DATA);
            event.adv_len = (uint8_t)encoded_len;
            CHECK("LINK-03", tavrn_link_v2_on_scheduler_event(
                                   &link, &event, (uint32_t)(43u + 8u * pass),
                                   &output) == TAVRN_LINK_STEP_EVENT &&
                               output.type == TAVRN_LINK_EVENT_RX_DATA_CANDIDATE &&
                               link.candidate_valid != 0u);
        }

        CHECK("LINK-03", tavrn_link_v2_clear_peer_incarnation(
                               &link, &peer_b, (uint32_t)(44u + 8u * pass)) ==
                               TAVRN_LINK_RESOLVE_OK &&
                           link.active_custody_index == TAVRN_CUSTODY_SLOT_NONE &&
                           link.candidate_valid == 0u &&
                           data_dedupe_contains(&link, &unrelated) &&
                           scheduler.routed_tx_queue.count == 1u);
        for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
            CHECK("LINK-03", link.custody[index].phase == TAVRN_CUSTODY_FREE &&
                               !data_dedupe_contains(&link, &b_owned[index]));
        }
    }
    CHECK("LINK-03", tavrn_link_v2_release_rx_custody(&link, &unrelated, 100u) ==
                       TAVRN_LINK_RESOLVE_OK);
}

int main(void)
{
    test_bearer_04_outer_adva_before_mutation();
    test_link_01_only_data_is_hackable();
    test_link_02_candidate_resolution_and_containment();
    test_link_02_candidate_match_seam();
    test_link_03_busy_rejected_and_exact_correlation();
    test_link_02_two_peer_hack_direction_and_statuses();
    test_link_04_slots_retries_terminal_ownership_and_fault_draining();
    test_link_05_rrep_ack_never_cross_satisfies_data();
    test_link_06_directed_data_and_controlled_flood();
    test_link_02_dedupe_replacement();
    test_link_02_hack_turnaround_config_bounds();
    test_link_02_hack_turnaround_enqueue_paths();
    test_link_04_tx_done_response_deadline();
    test_link_07_best_effort_aodv_control_send();
    test_serial_04_peer_cleanup_owns_transit_data_by_endpoints();
    if (failures != 0u) {
        printf("tavrn_link_v2 RED tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_link_v2 tests passed\n");
    return 0;
}
