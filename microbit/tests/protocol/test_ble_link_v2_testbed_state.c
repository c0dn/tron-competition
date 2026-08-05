#include "link_testbed_state.h"

#include <stdio.h>
#include <string.h>

static unsigned int failures;

#define CHECK(expression) \
    do { \
        if (!(expression)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
            failures++; \
        } \
    } while (0)

static const uint8_t local_adva[6] = {
    0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu,
};
static const uint8_t peer_adva[6] = {
    0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u,
};

static tavrn_direct_peer_t peer_from(const uint8_t adva[6])
{
    tavrn_direct_peer_t peer;

    memset(&peer, 0, sizeof(peer));
    peer.logical_id.width = TAVRN_IDENTITY_SID16;
    peer.logical_id.value = (uint16_t)adva[0] | ((uint16_t)adva[1] << 8);
    memcpy(peer.adva.bytes, adva, sizeof(peer.adva.bytes));
    return peer;
}

static tavrn_link_data_t diagnostic_data(uint16_t sequence)
{
    tavrn_link_data_t data;

    memset(&data, 0, sizeof(data));
    data.origin = peer_from(peer_adva).logical_id;
    data.final_destination = peer_from(local_adva).logical_id;
    data.data_seq = sequence;
    data.ttl = 1u;
    data.app_kind = 0x7fu;
    data.app_source = 0u;
    data.app_len = 4u;
    data.app_bytes[0] = (uint8_t)sequence;
    data.ownership = TAVRN_DATA_TRANSIT;
    return data;
}

static int enqueue_hack(ble_mesh_tx_queue_t *queue,
                        const tavrn_direct_peer_t *local,
                        const tavrn_direct_peer_t *peer,
                        const tavrn_link_data_t *data,
                        tavrn_hack_status_t status)
{
    tavrn_codec_config_t config;
    tavrn_decoded_frame_t frame;
    ble_mesh_tx_item_t item;
    size_t length = 0u;

    memset(&config, 0, sizeof(config));
    config.network_id = 0x2au;
    config.local_peer = *local;
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_HACK;
    frame.network_id = config.network_id;
    frame.detail.hack.immediate_receiver = peer->logical_id;
    frame.detail.hack.data_origin = data->origin;
    frame.detail.hack.final_destination = data->final_destination;
    frame.detail.hack.data_seq = data->data_seq;
    frame.detail.hack.app_kind = data->app_kind;
    frame.detail.hack.app_source = data->app_source;
    frame.detail.hack.status = status;
    memset(&item, 0, sizeof(item));
    if (tavrn_wire_v2_encode(&config, &frame, item.adv_data,
                             sizeof(item.adv_data), &length) != TAVRN_CODEC_OK) {
        return 0;
    }
    item.adv_len = (uint8_t)length;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = BLE_MESH_TX_PRIORITY_HACK;
    item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
    return ble_mesh_tx_queue_enqueue(queue, &item).status == BLE_MESH_TX_ENQUEUE_OK;
}

static tavrn_link_config_t link_config(const tavrn_direct_peer_t *local)
{
    tavrn_link_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = *local;
    config.network_id = 0x2au;
    config.hack_max_attempts = 3u;
    config.busy_max_responses = 3u;
    config.hack_response_ms = 250u;
    config.busy_backoff_ms = 500u;
    config.data_forward_deadline_ms = 5000u;
    config.candidate_resolve_ms = 10u;
    config.data_dedupe_ms = 10000u;
    config.flood_dedupe_ms = 10000u;
    config.flood_jitter_min_ms = 20u;
    config.flood_jitter_max_ms = 120u;
    return config;
}

static int make_candidate(tavrn_link_v2_t *link, ble_mesh_scheduler_t *scheduler,
                          const tavrn_direct_peer_t *local,
                          const tavrn_direct_peer_t *peer,
                          const tavrn_link_data_t *data,
                          tavrn_rx_data_candidate_t *candidate_out)
{
    tavrn_link_config_t config = link_config(local);
    tavrn_codec_config_t codec;
    tavrn_decoded_frame_t frame;
    ble_mesh_sched_event_t event;
    tavrn_link_event_t output;
    size_t length = 0u;

    ble_mesh_scheduler_init(scheduler, 0u, local->adva.bytes);
    if (tavrn_link_v2_init(link, scheduler, &config, 0u) != TAVRN_LINK_INIT_OK) {
        return 0;
    }
    memset(&codec, 0, sizeof(codec));
    codec.network_id = config.network_id;
    codec.local_peer = *local;
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = config.network_id;
    frame.detail.data.immediate_receiver = local->logical_id;
    frame.detail.data.data = *data;
    memset(&event, 0, sizeof(event));
    event.type = BLE_MESH_SCHED_EVENT_RX_ADV;
    memcpy(event.adv_addr, peer->adva.bytes, sizeof(event.adv_addr));
    if (tavrn_wire_v2_encode(&codec, &frame, event.adv_data,
                             sizeof(event.adv_data), &length) != TAVRN_CODEC_OK) {
        return 0;
    }
    event.adv_len = (uint8_t)length;
    if (tavrn_link_v2_on_scheduler_event(link, &event, 0u, &output) !=
            TAVRN_LINK_STEP_EVENT ||
        output.type != TAVRN_LINK_EVENT_RX_DATA_CANDIDATE) {
        return 0;
    }
    *candidate_out = output.detail.candidate;
    return 1;
}

static void test_delivery_reserve_commit_cancel_and_policy(void)
{
    link_testbed_delivery_state_t state;
    tavrn_link_data_t data = diagnostic_data(1u);
    tavrn_link_data_t unsupported = data;
    tavrn_link_data_t wrong_source = data;
    tavrn_link_data_t wrong_shape = data;
    tavrn_link_data_t nonlocal = data;
    tavrn_direct_peer_t local = peer_from(local_adva);
    uint8_t index;
    uint32_t forced_busy;
    uint8_t i;

    link_testbed_delivery_init(&state);
    CHECK(link_testbed_delivery_reserve(&state, &index));
    CHECK(index == 0u && state.published.count == 0u && state.provisional == 1u);
    CHECK(!link_testbed_delivery_pop(&state, &index));
    link_testbed_delivery_cancel(&state);
    CHECK(state.published.count == 0u && state.provisional == 0u);
    CHECK(link_testbed_delivery_reserve(&state, &index));
    CHECK(link_testbed_delivery_commit(&state));
    CHECK(state.published.count == 1u && state.published.tail == 1u);
    CHECK(link_testbed_delivery_pop(&state, &index) && index == 0u);

    forced_busy = 1u;
    unsupported.app_kind = 0x01u;
    CHECK(link_testbed_admit_final(&unsupported, &local.logical_id,
                                   &state, &forced_busy, &index) == TAVRN_RX_REJECTED);
    CHECK(forced_busy == 1u && state.published.count == 0u);
    wrong_source.app_source = 1u;
    CHECK(link_testbed_admit_final(&wrong_source, &local.logical_id,
                                   &state, &forced_busy, &index) == TAVRN_RX_REJECTED);
    wrong_shape.app_len = 3u;
    CHECK(link_testbed_admit_final(&wrong_shape, &local.logical_id,
                                   &state, &forced_busy, &index) == TAVRN_RX_REJECTED);
    CHECK(forced_busy == 1u && state.published.count == 0u);
    nonlocal.final_destination = peer_from(peer_adva).logical_id;
    CHECK(link_testbed_admit_final(&nonlocal, &local.logical_id,
                                   &state, &forced_busy, &index) == TAVRN_RX_REJECTED);
    CHECK(forced_busy == 1u && state.published.count == 0u);
    CHECK(link_testbed_admit_final(&data, &local.logical_id,
                                   &state, &forced_busy, &index) == TAVRN_RX_BUSY);
    CHECK(forced_busy == 0u && state.published.count == 0u);

    for (i = 0u; i < LINK_TESTBED_DELIVERY_CAPACITY; i++) {
        CHECK(link_testbed_delivery_reserve(&state, &index));
        CHECK(link_testbed_delivery_commit(&state));
    }
    CHECK(state.published.count == LINK_TESTBED_DELIVERY_CAPACITY);
    CHECK(link_testbed_admit_final(&data, &local.logical_id,
                                   &state, &forced_busy, &index) == TAVRN_RX_BUSY);
    CHECK(state.published.count == LINK_TESTBED_DELIVERY_CAPACITY &&
          state.provisional == 0u);
}

static void test_exact_hack_suppression(void)
{
    ble_mesh_tx_queue_t queue;
    tavrn_direct_peer_t local = peer_from(local_adva);
    tavrn_direct_peer_t peer = peer_from(peer_adva);
    tavrn_link_data_t data = diagnostic_data(9u);
    tavrn_link_data_t wrong_data = diagnostic_data(10u);
    link_testbed_hack_capture_t capture;
    uint8_t i;

    ble_mesh_tx_queue_init(&queue);
    link_testbed_capture_hack(&capture, &queue, peer_adva, &local, &data,
                               TAVRN_HACK_ACCEPTED, 0x2au);
    CHECK(capture.armed == 0u);
    CHECK(enqueue_hack(&queue, &local, &peer, &data, TAVRN_HACK_ACCEPTED));
    link_testbed_capture_hack(&capture, &queue, peer_adva, &peer, &data,
                               TAVRN_HACK_ACCEPTED, 0x2au);
    CHECK(enqueue_hack(&queue, &local, &peer, &data, TAVRN_HACK_ACCEPTED));
    CHECK(link_testbed_remove_captured_hack(&queue, &local, &capture));
    CHECK(queue.count == 1u && queue.entries[0].occupied != 0u &&
          queue.entries[0].ordinal == 0u);

    link_testbed_capture_hack(&capture, &queue, peer_adva, &peer, &data,
                               TAVRN_HACK_ACCEPTED, 0x2au);
    CHECK(enqueue_hack(&queue, &local, &peer, &wrong_data, TAVRN_HACK_ACCEPTED));
    CHECK(!link_testbed_remove_captured_hack(&queue, &local, &capture));
    CHECK(queue.count == 2u);

    link_testbed_capture_hack(&capture, &queue, peer_adva, &peer, &data,
                               TAVRN_HACK_DUPLICATE, 0x2au);
    CHECK(enqueue_hack(&queue, &local, &peer, &data, TAVRN_HACK_ACCEPTED));
    CHECK(!link_testbed_remove_captured_hack(&queue, &local, &capture));
    CHECK(queue.count == 3u);

    ble_mesh_tx_queue_init(&queue);
    for (i = 0u; i < BLE_MESH_TX_QUEUE_CAPACITY; i++) {
        CHECK(enqueue_hack(&queue, &local, &peer, &data, TAVRN_HACK_ACCEPTED));
    }
    link_testbed_capture_hack(&capture, &queue, peer_adva, &peer, &data,
                               TAVRN_HACK_ACCEPTED, 0x2au);
    CHECK(!enqueue_hack(&queue, &local, &peer, &data, TAVRN_HACK_ACCEPTED));
    CHECK(!link_testbed_remove_captured_hack(&queue, &local, &capture));
    CHECK(queue.count == BLE_MESH_TX_QUEUE_CAPACITY);
}

static void test_resolve_failure_and_hack_enqueue_commit(void)
{
    tavrn_direct_peer_t local = peer_from(local_adva);
    tavrn_direct_peer_t peer = peer_from(peer_adva);
    tavrn_link_data_t data = diagnostic_data(31u);
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    tavrn_rx_data_candidate_t candidate;
    tavrn_link_event_t outcome;
    link_testbed_delivery_state_t state;
    uint32_t forced_busy = 0u;
    uint8_t index;
    uint8_t i;

    CHECK(make_candidate(&link, &scheduler, &local, &peer, &data, &candidate));
    link_testbed_delivery_init(&state);
    CHECK(link_testbed_admit_final(&candidate.data, &local.logical_id, &state,
                                   &forced_busy, &index) == TAVRN_RX_ACCEPTED);
    CHECK(tavrn_link_v2_resolve_rx(&link, (tavrn_rx_candidate_token_t)
                                   (candidate.token + 1u), TAVRN_RX_ACCEPTED,
                                   0u, &outcome) != TAVRN_LINK_RESOLVE_OK);
    link_testbed_delivery_cancel(&state);
    CHECK(state.published.count == 0u && state.provisional == 0u);

    data.data_seq = 32u;
    CHECK(make_candidate(&link, &scheduler, &local, &peer, &data, &candidate));
    link_testbed_delivery_init(&state);
    CHECK(link_testbed_admit_final(&candidate.data, &local.logical_id, &state,
                                   &forced_busy, &index) == TAVRN_RX_ACCEPTED);
    for (i = 0u; i < BLE_MESH_TX_QUEUE_CAPACITY; i++) {
        CHECK(enqueue_hack(&scheduler.routed_tx_queue, &local, &peer, &data,
                           TAVRN_HACK_ACCEPTED));
    }
    CHECK(tavrn_link_v2_resolve_rx(&link, candidate.token, TAVRN_RX_ACCEPTED,
                                   0u, &outcome) == TAVRN_LINK_RESOLVE_OK);
    CHECK(link.counters.rx_candidate_accepted == 1u &&
          link.counters.hack_enqueue_failed == 1u);
    CHECK(link_testbed_delivery_commit(&state));
    CHECK(state.published.count == 1u && state.provisional == 0u);
}

static void test_poll_gate_wrap_and_no_extra_poll(void)
{
    link_testbed_poll_gate_t gate;

    link_testbed_poll_gate_init(&gate);
    CHECK(LINK_TESTBED_MESH_TASK_PRIORITY < LINK_TESTBED_LOGGER_TASK_PRIORITY);
    CHECK(link_testbed_mesh_yield_delay_ms(2u) == 1u);
    CHECK(link_testbed_mesh_yield_delay_ms(1u) == 0u);
    CHECK(link_testbed_poll_gate_begin(&gate, 0xfffffffcu, 2u));
    link_testbed_poll_gate_complete(&gate, 0xfffffffeu);
    CHECK(link_testbed_poll_gate_begin(&gate, 0u, 2u));
    link_testbed_poll_gate_complete(&gate, 0u);
    CHECK(!link_testbed_poll_gate_begin(&gate, 3u, 2u));
    CHECK(gate.fault_latched == 1u && gate.last_poll_return_ms == 0u);
    CHECK(!link_testbed_poll_gate_begin(&gate, 4u, 2u));
    link_testbed_poll_gate_complete(&gate, 4u);
    CHECK(gate.last_poll_return_ms == 0u);
}

int main(void)
{
    test_delivery_reserve_commit_cancel_and_policy();
    test_exact_hack_suppression();
    test_resolve_failure_and_hack_enqueue_commit();
    test_poll_gate_wrap_and_no_extra_poll();
    if (failures != 0u) {
        return 1;
    }
    printf("ble link-v2 testbed state tests passed\n");
    return 0;
}
