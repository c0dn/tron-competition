#ifndef LINK_TESTBED_STATE_H
#define LINK_TESTBED_STATE_H

/*
 * Pure, fixed-capacity state used by the Phase 1 harness.  Task dispatch is
 * disabled by main.c around each caller-visible mutation; these routines stay
 * kernel-free so their reserve/commit and exact-HACK rules are host-testable.
 */

#include <stdint.h>
#include <string.h>

#include "ble_mesh_tx_queue.h"
#include "tavrn_link_v2.h"

#define LINK_TESTBED_DELIVERY_CAPACITY   8u
#define LINK_TESTBED_DIAGNOSTIC_CAPACITY 8u
#define LINK_TESTBED_MESH_TASK_PRIORITY  10u
#define LINK_TESTBED_LOGGER_TASK_PRIORITY 11u

typedef enum link_testbed_rf_row {
    LINK_TESTBED_RF_ROW_E_RREQ = 0,
    LINK_TESTBED_RF_ROW_E_RREP,
    LINK_TESTBED_RF_ROW_E_RERR,
    LINK_TESTBED_RF_ROW_HELLO,
    LINK_TESTBED_RF_ROW_SYNC_OFFER,
    LINK_TESTBED_RF_ROW_SYNC_PULL,
    LINK_TESTBED_RF_ROW_SYNC_DATA,
    LINK_TESTBED_RF_ROW_TC_UPDATE,
    LINK_TESTBED_RF_ROW_E_RREP_ACK,
    LINK_TESTBED_RF_ROW_DATA,
    LINK_TESTBED_RF_ROW_HACK,
    LINK_TESTBED_RF_ROW_FLOOD,
    LINK_TESTBED_RF_ROW_COUNT,
} link_testbed_rf_row_t;

typedef enum link_testbed_rf_channel {
    LINK_TESTBED_RF_CHANNEL_37 = 0,
    LINK_TESTBED_RF_CHANNEL_38,
    LINK_TESTBED_RF_CHANNEL_39,
    LINK_TESTBED_RF_CHANNEL_COUNT,
} link_testbed_rf_channel_t;

#define LINK_TESTBED_RF_WIRE_TYPE_COUNT  LINK_TESTBED_RF_ROW_COUNT

typedef char link_testbed_delivery_capacity_must_be_eight[
    (LINK_TESTBED_DELIVERY_CAPACITY == 8u) ? 1 : -1];
typedef char link_testbed_diagnostic_capacity_must_be_eight[
    (LINK_TESTBED_DIAGNOSTIC_CAPACITY == 8u) ? 1 : -1];
typedef char link_testbed_rf_wire_type_count_must_be_twelve[
    (LINK_TESTBED_RF_WIRE_TYPE_COUNT == 12u) ? 1 : -1];
typedef char link_testbed_rf_channel_count_must_be_three[
    (LINK_TESTBED_RF_CHANNEL_COUNT == 3u) ? 1 : -1];

typedef struct link_testbed_ring_state {
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} link_testbed_ring_state_t;

typedef struct link_testbed_delivery_state {
    link_testbed_ring_state_t published;
    uint8_t provisional;
} link_testbed_delivery_state_t;

typedef struct link_testbed_poll_gate {
    uint32_t last_poll_return_ms;
    uint8_t have_poll_return;
    uint8_t fault_latched;
} link_testbed_poll_gate_t;

typedef struct link_testbed_rf_telemetry {
    uint32_t valid_wire_channel[LINK_TESTBED_RF_WIRE_TYPE_COUNT]
                                 [LINK_TESTBED_RF_CHANNEL_COUNT];
} link_testbed_rf_telemetry_t;

typedef struct link_testbed_hack_capture {
    uint8_t armed;
    uint8_t network_id;
    ble_mesh_tx_ordinal_t ordinal;
    uint8_t occupied[BLE_MESH_TX_QUEUE_CAPACITY];
    ble_mesh_tx_ordinal_t prior_ordinal[BLE_MESH_TX_QUEUE_CAPACITY];
    tavrn_direct_peer_t peer;
    tavrn_link_data_t data;
    tavrn_hack_status_t status;
} link_testbed_hack_capture_t;

static inline int link_testbed_id_equal(const tavrn_logical_id_t *left,
                                        const tavrn_logical_id_t *right)
{
    return left != NULL && right != NULL && left->width == right->width &&
           left->value == right->value;
}

static inline void link_testbed_ring_init(link_testbed_ring_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

/* The mesh task voluntarily waits for one millisecond less than the immutable
 * poll budget. Any kernel wakeup or logger delay that exceeds the remaining
 * slack is caught before another radio operation begins. */
static inline uint32_t link_testbed_mesh_yield_delay_ms(uint32_t poll_max_ms)
{
    return poll_max_ms > 1u ? poll_max_ms - 1u : 0u;
}

static inline void link_testbed_poll_gate_init(link_testbed_poll_gate_t *gate)
{
    if (gate != NULL) {
        memset(gate, 0, sizeof(*gate));
    }
}

/* Returns zero before the caller polls when the prior operation-return gap is
 * over budget. Latching here prevents a later caller from issuing one extra
 * scheduler/radio operation after the service fault. */
static inline int link_testbed_poll_gate_begin(link_testbed_poll_gate_t *gate,
                                               uint32_t poll_start_ms,
                                               uint32_t poll_max_ms)
{
    if (gate == NULL || poll_max_ms == 0u || gate->fault_latched != 0u) {
        if (gate != NULL) {
            gate->fault_latched = 1u;
        }
        return 0;
    }
    if (gate->have_poll_return != 0u &&
        (uint32_t)(poll_start_ms - gate->last_poll_return_ms) > poll_max_ms) {
        gate->fault_latched = 1u;
        return 0;
    }
    return 1;
}

static inline void link_testbed_poll_gate_complete(link_testbed_poll_gate_t *gate,
                                                   uint32_t poll_return_ms)
{
    if (gate != NULL && gate->fault_latched == 0u) {
        gate->last_poll_return_ms = poll_return_ms;
        gate->have_poll_return = 1u;
    }
}

static inline void link_testbed_rf_telemetry_init(
    link_testbed_rf_telemetry_t *telemetry)
{
    if (telemetry != NULL) {
        memset(telemetry, 0, sizeof(*telemetry));
    }
}

static inline int link_testbed_rf_wire_type_index(tavrn_wire_type_t type,
                                                   link_testbed_rf_row_t *row_out)
{
    link_testbed_rf_row_t row;

    if (row_out == NULL) {
        return 0;
    }
    switch (type) {
    case TAVRN_WIRE_E_RREQ: row = LINK_TESTBED_RF_ROW_E_RREQ; break;
    case TAVRN_WIRE_E_RREP: row = LINK_TESTBED_RF_ROW_E_RREP; break;
    case TAVRN_WIRE_E_RERR: row = LINK_TESTBED_RF_ROW_E_RERR; break;
    case TAVRN_WIRE_HELLO: row = LINK_TESTBED_RF_ROW_HELLO; break;
    case TAVRN_WIRE_SYNC_OFFER: row = LINK_TESTBED_RF_ROW_SYNC_OFFER; break;
    case TAVRN_WIRE_SYNC_PULL: row = LINK_TESTBED_RF_ROW_SYNC_PULL; break;
    case TAVRN_WIRE_SYNC_DATA: row = LINK_TESTBED_RF_ROW_SYNC_DATA; break;
    case TAVRN_WIRE_TC_UPDATE: row = LINK_TESTBED_RF_ROW_TC_UPDATE; break;
    case TAVRN_WIRE_E_RREP_ACK: row = LINK_TESTBED_RF_ROW_E_RREP_ACK; break;
    case TAVRN_WIRE_DATA: row = LINK_TESTBED_RF_ROW_DATA; break;
    case TAVRN_WIRE_HACK: row = LINK_TESTBED_RF_ROW_HACK; break;
    case TAVRN_WIRE_FLOOD: row = LINK_TESTBED_RF_ROW_FLOOD; break;
    default:
        return 0;
    }
    *row_out = row;
    return 1;
}

static inline int link_testbed_rf_channel_index(uint8_t channel,
                                                 link_testbed_rf_channel_t *index_out)
{
    if (index_out == NULL) {
        return 0;
    }
    if (channel == 37u) {
        *index_out = LINK_TESTBED_RF_CHANNEL_37;
    } else if (channel == 38u) {
        *index_out = LINK_TESTBED_RF_CHANNEL_38;
    } else if (channel == 39u) {
        *index_out = LINK_TESTBED_RF_CHANNEL_39;
    } else {
        return 0;
    }
    return 1;
}

static inline void link_testbed_rf_telemetry_record(
    link_testbed_rf_telemetry_t *telemetry, uint8_t channel,
    tavrn_codec_result_t decode_result, const tavrn_decoded_frame_t *frame)
{
    link_testbed_rf_row_t row;
    link_testbed_rf_channel_t channel_index;

    if (telemetry == NULL || frame == NULL || decode_result != TAVRN_CODEC_OK ||
        !link_testbed_rf_wire_type_index(frame->type, &row) ||
        !link_testbed_rf_channel_index(channel, &channel_index)) {
        return;
    }
    telemetry->valid_wire_channel[row][channel_index]++;
}

/* This is the pre-hook RX accounting boundary. It owns raw event decode and
 * excludes a remote AdvA that aliases the local logical SID before telemetry
 * can count it as a valid observation. */
static inline void link_testbed_rf_telemetry_record_rx_event(
    link_testbed_rf_telemetry_t *telemetry, const tavrn_codec_config_t *config,
    const ble_mesh_sched_event_t *event)
{
    tavrn_decoded_frame_t frame;
    tavrn_codec_result_t decoded;

    if (telemetry == NULL || config == NULL || event == NULL ||
        event->type != BLE_MESH_SCHED_EVENT_RX_ADV) {
        return;
    }
    memset(&frame, 0, sizeof(frame));
    decoded = tavrn_wire_v2_decode(config, event->adv_addr, event->adv_data,
                                   event->adv_len, &frame);
    if (decoded == TAVRN_CODEC_OK &&
        link_testbed_id_equal(&frame.transmitter.logical_id,
                              &config->local_peer.logical_id) &&
        memcmp(frame.transmitter.adva.bytes, config->local_peer.adva.bytes,
               TAVRN_ADVA_LEN) != 0) {
        decoded = TAVRN_CODEC_IDENTITY_CONFLICT;
    }
    link_testbed_rf_telemetry_record(telemetry, event->channel, decoded, &frame);
}

static inline uint32_t link_testbed_rf_telemetry_count(
    const link_testbed_rf_telemetry_t *telemetry, tavrn_wire_type_t type,
    uint8_t channel)
{
    link_testbed_rf_row_t row;
    link_testbed_rf_channel_t channel_index;

    if (telemetry == NULL || !link_testbed_rf_wire_type_index(type, &row) ||
        !link_testbed_rf_channel_index(channel, &channel_index)) {
        return 0u;
    }
    return telemetry->valid_wire_channel[row][channel_index];
}

static inline int link_testbed_ring_push(link_testbed_ring_state_t *state,
                                         uint8_t capacity, uint8_t *index_out)
{
    if (state == NULL || index_out == NULL || capacity == 0u ||
        state->count >= capacity) {
        return 0;
    }
    *index_out = state->tail;
    state->tail = (uint8_t)((state->tail + 1u) % capacity);
    state->count++;
    return 1;
}

static inline int link_testbed_ring_pop(link_testbed_ring_state_t *state,
                                        uint8_t capacity, uint8_t *index_out)
{
    if (state == NULL || index_out == NULL || capacity == 0u ||
        state->count == 0u) {
        return 0;
    }
    *index_out = state->head;
    state->head = (uint8_t)((state->head + 1u) % capacity);
    state->count--;
    return 1;
}

static inline void link_testbed_delivery_init(link_testbed_delivery_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

/* A reservation consumes capacity but is intentionally invisible to the
 * logger until commit.  There is only one link-v2 RX candidate at a time. */
static inline int link_testbed_delivery_reserve(link_testbed_delivery_state_t *state,
                                                uint8_t *index_out)
{
    if (state == NULL || index_out == NULL || state->provisional != 0u ||
        state->published.count >= LINK_TESTBED_DELIVERY_CAPACITY) {
        return 0;
    }
    state->provisional = 1u;
    *index_out = state->published.tail;
    return 1;
}

static inline int link_testbed_delivery_commit(link_testbed_delivery_state_t *state)
{
    uint8_t ignored;

    if (state == NULL || state->provisional == 0u ||
        !link_testbed_ring_push(&state->published,
                                LINK_TESTBED_DELIVERY_CAPACITY, &ignored)) {
        return 0;
    }
    state->provisional = 0u;
    return 1;
}

static inline void link_testbed_delivery_cancel(link_testbed_delivery_state_t *state)
{
    if (state != NULL) {
        state->provisional = 0u;
    }
}

static inline int link_testbed_delivery_pop(link_testbed_delivery_state_t *state,
                                            uint8_t *index_out)
{
    if (state == NULL || state->provisional > 1u) {
        return 0;
    }
    return link_testbed_ring_pop(&state->published,
                                 LINK_TESTBED_DELIVERY_CAPACITY, index_out);
}

static inline int link_testbed_supported_final_data(
    const tavrn_link_data_t *data, const tavrn_logical_id_t *local_id)
{
    return data != NULL && local_id != NULL &&
           data->final_destination.width == TAVRN_IDENTITY_SID16 &&
           link_testbed_id_equal(&data->final_destination, local_id) &&
           data->app_kind == 0x7fu && data->app_source == 0u &&
           data->app_len == 4u;
}

/* Permanent policy is evaluated before a configured transient BUSY hook.
 * ACCEPTED reserves an unpublished final-delivery slot; the caller must commit
 * only after tavrn_link_v2_resolve_rx() succeeds. */
static inline tavrn_rx_decision_t link_testbed_admit_final(
    const tavrn_link_data_t *data, const tavrn_logical_id_t *local_id,
    link_testbed_delivery_state_t *deliveries, uint32_t *forced_busy_remaining,
    uint8_t *reserved_index_out)
{
    if (!link_testbed_supported_final_data(data, local_id)) {
        return TAVRN_RX_REJECTED;
    }
    if (forced_busy_remaining != NULL && *forced_busy_remaining != 0u) {
        (*forced_busy_remaining)--;
        return TAVRN_RX_BUSY;
    }
    if (!link_testbed_delivery_reserve(deliveries, reserved_index_out)) {
        return TAVRN_RX_BUSY;
    }
    return TAVRN_RX_ACCEPTED;
}

static inline void link_testbed_capture_hack(
    link_testbed_hack_capture_t *capture, const ble_mesh_tx_queue_t *queue,
    const uint8_t configured_peer_adva[TAVRN_ADVA_LEN],
    const tavrn_direct_peer_t *peer, const tavrn_link_data_t *data,
    tavrn_hack_status_t status, uint8_t network_id)
{
    uint8_t index;

    if (capture == NULL) {
        return;
    }
    memset(capture, 0, sizeof(*capture));
    if (queue == NULL || configured_peer_adva == NULL || peer == NULL ||
        data == NULL || memcmp(peer->adva.bytes, configured_peer_adva,
                               TAVRN_ADVA_LEN) != 0) {
        return;
    }
    capture->ordinal = queue->next_ordinal;
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        capture->occupied[index] = queue->entries[index].occupied;
        capture->prior_ordinal[index] = queue->entries[index].ordinal;
    }
    capture->peer = *peer;
    capture->data = *data;
    capture->status = status;
    capture->network_id = network_id;
    capture->armed = 1u;
}

/* Remove only the exact item inserted by the just-completed link call.  An
 * ordinal snapshot excludes an older equivalent HACK, while full decode checks
 * the receiver, DATA correlation tuple, and status rather than packet offsets. */
static inline int link_testbed_remove_captured_hack(
    ble_mesh_tx_queue_t *queue, const tavrn_direct_peer_t *local_peer,
    const link_testbed_hack_capture_t *capture)
{
    tavrn_codec_config_t config;
    uint8_t index;

    if (queue == NULL || local_peer == NULL || capture == NULL ||
        capture->armed == 0u) {
        return 0;
    }
    memset(&config, 0, sizeof(config));
    config.local_peer = *local_peer;
    /* The queue stores a locally emitted HACK, so the local AdvA is the only
     * valid outer identity for decoding its wire bytes. */
    config.network_id = capture->network_id;
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        tavrn_decoded_frame_t frame;
        const ble_mesh_tx_queue_entry_t *entry = &queue->entries[index];

        if (entry->occupied == 0u || entry->ordinal != capture->ordinal ||
            (capture->occupied[index] != 0u &&
             capture->prior_ordinal[index] == entry->ordinal)) {
            continue;
        }
        if (tavrn_wire_v2_decode(&config, local_peer->adva.bytes,
                                 entry->item.adv_data, entry->item.adv_len,
                                 &frame) != TAVRN_CODEC_OK ||
            frame.type != TAVRN_WIRE_HACK ||
            !link_testbed_id_equal(&frame.detail.hack.immediate_receiver,
                                   &capture->peer.logical_id) ||
            !link_testbed_id_equal(&frame.detail.hack.data_origin,
                                   &capture->data.origin) ||
            !link_testbed_id_equal(&frame.detail.hack.final_destination,
                                   &capture->data.final_destination) ||
            frame.detail.hack.data_seq != capture->data.data_seq ||
            frame.detail.hack.app_kind != capture->data.app_kind ||
            frame.detail.hack.app_source != capture->data.app_source ||
            frame.detail.hack.status != capture->status) {
            continue;
        }
        return ble_mesh_tx_queue_remove(queue, index, NULL);
    }
    return 0;
}

#endif /* LINK_TESTBED_STATE_H */
