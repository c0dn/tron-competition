#ifndef BLE_MESH_SCHEDULER_H
#define BLE_MESH_SCHEDULER_H

/*
 * Polling-first single-radio scheduler for the experimental TRON BLE
 * advertising mesh-like transport.
 *
 * The nRF52833 has one 2.4 GHz radio. This scheduler must never claim true
 * simultaneous TX/RX. Mesh-node operation is passive RX by default, with
 * bounded TX windows interleaved by explicitly idling/disabling RX first and
 * restoring RX afterwards.
 */

#include <stdint.h>

#include "ble_radio.h"
#include "ble_mesh_tx_queue.h"

#define BLE_MESH_SCHED_DWELL_MS_DEFAULT       50u
#define BLE_MESH_SCHED_RELAY_TX_INTERVAL_MS   200u
#define BLE_MESH_SCHED_LEGACY_TX_QUEUE_CAPACITY 4u
#define BLE_MESH_SCHED_ADV_DATA_MAX           BLE_ADV_MAX_DATA

#define BLE_MESH_SCHED_CH37                   BLE_RADIO_ADV_CH37
#define BLE_MESH_SCHED_CH38                   BLE_RADIO_ADV_CH38
#define BLE_MESH_SCHED_CH39                   BLE_RADIO_ADV_CH39
#define BLE_MESH_SCHED_CH_ALL                 (BLE_MESH_SCHED_CH37 | BLE_MESH_SCHED_CH38 | BLE_MESH_SCHED_CH39)

typedef enum ble_mesh_sched_tx_kind {
    BLE_MESH_SCHED_TX_RELAY = 0,
    BLE_MESH_SCHED_TX_OWN = 1,
} ble_mesh_sched_tx_kind_t;

typedef enum ble_mesh_sched_event_type {
    BLE_MESH_SCHED_EVENT_NONE = 0,
    BLE_MESH_SCHED_EVENT_RX_ADV,
    BLE_MESH_SCHED_EVENT_TX_DONE,
    BLE_MESH_SCHED_EVENT_TX_FAILED,
    BLE_MESH_SCHED_EVENT_RADIO_FAULT,
    BLE_MESH_SCHED_EVENT_SERVICE_FAULT,
} ble_mesh_sched_event_type_t;

typedef enum ble_mesh_sched_fault {
    BLE_MESH_SCHED_FAULT_NONE = 0,
    BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT,
    BLE_MESH_SCHED_FAULT_RADIO_INVALID_ARGUMENT,
    BLE_MESH_SCHED_FAULT_POLL_OVERRUN,
    BLE_MESH_SCHED_FAULT_QUEUE_CORRUPT,
    BLE_MESH_SCHED_FAULT_INTERNAL_STATE,
} ble_mesh_sched_fault_t;

typedef struct ble_mesh_sched_event {
    ble_mesh_sched_event_type_t type;
    uint8_t channel;
    uint8_t rssi_magnitude_db;
    uint8_t adv_addr[6];
    uint8_t adv_len;
    uint8_t adv_data[BLE_MESH_SCHED_ADV_DATA_MAX];
    ble_mesh_tx_token_t tx_token;
    uint8_t tx_requested_channel_mask;
    uint8_t tx_completed_channel_mask;
    ble_mesh_sched_fault_t fault;
} ble_mesh_sched_event_t;

typedef enum ble_mesh_sched_enqueue_status {
    BLE_MESH_SCHED_ENQUEUE_OK = 0,
    BLE_MESH_SCHED_ENQUEUE_FULL,
    BLE_MESH_SCHED_ENQUEUE_INVALID,
    BLE_MESH_SCHED_ENQUEUE_TOO_LONG,
} ble_mesh_sched_enqueue_status_t;

typedef struct ble_mesh_sched_enqueue_result {
    ble_mesh_sched_enqueue_status_t status;
    ble_mesh_tx_token_t accepted_token;
    ble_mesh_tx_token_t evicted_token;
} ble_mesh_sched_enqueue_result_t;

typedef struct ble_mesh_sched_counters {
    uint32_t rx_ok;
    uint32_t tx_ok;
    uint32_t rx_crc_or_empty;
    uint32_t queue_drop;
    uint32_t relay_drop;
    uint32_t relay_rate_limited;
    uint32_t tx_len_drop;
} ble_mesh_sched_counters_t;

/* Compatibility-only wire-v1 queue. Routed callers use ble_mesh_tx_queue_t. */
typedef struct ble_mesh_sched_legacy_tx_item {
    uint8_t adv_len;
    uint8_t channel_mask;
    ble_mesh_sched_tx_kind_t kind;
    uint32_t not_before_ms;
    uint8_t adv_data[BLE_MESH_SCHED_ADV_DATA_MAX];
} ble_mesh_sched_legacy_tx_item_t;

typedef struct ble_mesh_scheduler {
    uint8_t rx_channel_index;
    uint8_t rx_started;
    uint32_t hop_at_ms;
    uint32_t dwell_ms;
    ble_mesh_sched_legacy_tx_item_t
        legacy_tx_queue[BLE_MESH_SCHED_LEGACY_TX_QUEUE_CAPACITY];
    uint8_t legacy_tx_head;
    uint8_t legacy_tx_count;
    uint32_t relay_rate_limit_ms;
    uint32_t last_relay_tx_ms;
    uint8_t relay_last_tx_valid;
    ble_mesh_sched_counters_t counters;
    uint8_t local_adva[6];
    uint8_t routed_started;
    uint8_t custody_bypass_count;
    ble_mesh_sched_fault_t latched_fault;
    ble_mesh_tx_queue_t routed_tx_queue;
} ble_mesh_scheduler_t;

/* Routed typed initialization. It copies the canonical local AdvA by value. */
void ble_mesh_scheduler_init(ble_mesh_scheduler_t *sched, uint32_t now_ms,
                             const uint8_t local_adva[6]);

/* Compatibility-only wire-v1 initializer used by the existing legacy node. */
void ble_mesh_scheduler_init_legacy(ble_mesh_scheduler_t *sched,
                                    uint32_t now_ms);
int ble_mesh_scheduler_copy_local_adva(const ble_mesh_scheduler_t *sched,
                                       uint8_t out_adva[6]);

void ble_mesh_scheduler_start_rx(ble_mesh_scheduler_t *sched, uint32_t now_ms);

int ble_mesh_scheduler_enqueue(
    ble_mesh_scheduler_t *sched,
    const uint8_t *adv,
    uint8_t adv_len,
    uint8_t channel_mask,
    ble_mesh_sched_tx_kind_t kind,
    uint32_t not_before_ms);

ble_mesh_sched_enqueue_result_t ble_mesh_scheduler_enqueue_ex(
    ble_mesh_scheduler_t *sched, const ble_mesh_tx_item_t *item);

int ble_mesh_scheduler_poll(
    ble_mesh_scheduler_t *sched,
    uint32_t now_ms,
    ble_mesh_sched_event_t *event);

const ble_mesh_sched_counters_t *ble_mesh_scheduler_counters(const ble_mesh_scheduler_t *sched);

/*
 * The legacy compatibility implementation below uses the pre-routed radio
 * wrappers. Routed mesh paths must use the typed try_* radio API and the
 * routed queue/event fields above; that behavior is intentionally not present
 * in this legacy source file.
 * One-shot RX disables at packet END before copying raw PDU bytes, so the
 * single rx_pkt buffer is not auto-restarted and overwritten while handed to
 * the mesh node for decode. Existing ble_beacon/ble_observer source
 * compatibility is preserved through ble_radio_listen() and ble_radio_poll().
 */

#endif /* BLE_MESH_SCHEDULER_H */
