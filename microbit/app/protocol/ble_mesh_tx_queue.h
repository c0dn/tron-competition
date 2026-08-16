#ifndef BLE_MESH_TX_QUEUE_H
#define BLE_MESH_TX_QUEUE_H

#include <stdint.h>

#include "ble_radio.h"
#include "tron_timer_config.h"

/* Firmware targets force-include their generated build configuration before
 * this header. Direct host tests deliberately retain the routed control size. */
#ifndef TRON_BUILD_ROUTED_TX_QUEUE_CAPACITY
#define TRON_BUILD_ROUTED_TX_QUEUE_CAPACITY 4u
#endif

#if TRON_BUILD_ROUTED_TX_QUEUE_CAPACITY != 4u && \
    TRON_BUILD_ROUTED_TX_QUEUE_CAPACITY != 8u && \
    TRON_BUILD_ROUTED_TX_QUEUE_CAPACITY != 16u && \
    TRON_BUILD_ROUTED_TX_QUEUE_CAPACITY != 40u
#error "TRON_BUILD_ROUTED_TX_QUEUE_CAPACITY must be 4, 8, 16, or 40"
#endif

#define BLE_MESH_TX_QUEUE_CAPACITY TRON_BUILD_ROUTED_TX_QUEUE_CAPACITY
#define BLE_MESH_TX_TOKEN_NONE     0u
#define BLE_MESH_TX_QUEUE_FUTURE_BEARER_API 1

typedef uint16_t ble_mesh_tx_token_t;

typedef enum ble_mesh_tx_priority {
    BLE_MESH_TX_PRIORITY_RELAY = 0,
    BLE_MESH_TX_PRIORITY_DATA = 1,
    BLE_MESH_TX_PRIORITY_CONTROL = 2,
    BLE_MESH_TX_PRIORITY_RETRY = 3,
    BLE_MESH_TX_PRIORITY_HACK = 4,
} ble_mesh_tx_priority_t;

typedef enum ble_mesh_tx_service_class {
    BLE_MESH_TX_SERVICE_BEST_EFFORT = 0,
    BLE_MESH_TX_SERVICE_CUSTODY_DATA,
} ble_mesh_tx_service_class_t;

typedef enum ble_mesh_tx_sweep_count {
    BLE_MESH_TX_SWEEP_COUNT_ONE = 1,
    BLE_MESH_TX_SWEEP_COUNT_TWO = 2,
} ble_mesh_tx_sweep_count_t;

typedef enum ble_mesh_tx_budget_class {
    BLE_MESH_TX_BUDGET_GENERAL = 0,
    BLE_MESH_TX_BUDGET_CRITICAL,
} ble_mesh_tx_budget_class_t;

typedef struct ble_mesh_tx_item {
    uint8_t adv_len;
    uint8_t channel_mask;
    ble_mesh_tx_priority_t priority;
    ble_mesh_tx_service_class_t service_class;
    uint32_t not_before_ms;
    uint32_t expiry_ms;
    ble_mesh_tx_sweep_count_t sweep_count;
    ble_mesh_tx_budget_class_t budget_class;
    ble_mesh_tx_token_t token;
    uint8_t adv_data[BLE_ADV_MAX_DATA];
} ble_mesh_tx_item_t;

typedef uint32_t ble_mesh_tx_ordinal_t;

typedef enum ble_mesh_tx_enqueue_status {
    BLE_MESH_TX_ENQUEUE_OK = 0,
    BLE_MESH_TX_ENQUEUE_FULL,
    BLE_MESH_TX_ENQUEUE_INVALID,
    BLE_MESH_TX_ENQUEUE_TOO_LONG,
    BLE_MESH_TX_ENQUEUE_REJECTED,
} ble_mesh_tx_enqueue_status_t;

typedef struct ble_mesh_tx_enqueue_result {
    ble_mesh_tx_enqueue_status_t status;
    ble_mesh_tx_token_t accepted_token;
    ble_mesh_tx_token_t evicted_token;
} ble_mesh_tx_enqueue_result_t;

typedef enum ble_mesh_tx_select_status {
    BLE_MESH_TX_SELECT_NONE = 0,
    BLE_MESH_TX_SELECT_OK,
    BLE_MESH_TX_SELECT_INVALID,
} ble_mesh_tx_select_status_t;

typedef struct ble_mesh_tx_selection {
    ble_mesh_tx_select_status_t status;
    uint8_t index;
    ble_mesh_tx_item_t item;
} ble_mesh_tx_selection_t;

typedef enum ble_mesh_tx_expiry_status {
    BLE_MESH_TX_EXPIRY_OK = 0,
    BLE_MESH_TX_EXPIRY_INVALID,
} ble_mesh_tx_expiry_status_t;

typedef struct ble_mesh_tx_expiry_result {
    ble_mesh_tx_expiry_status_t status;
    uint8_t anonymous_purged_count;
    uint8_t tracked_due;
    uint8_t tracked_index;
    ble_mesh_tx_item_t tracked_item;
} ble_mesh_tx_expiry_result_t;

typedef enum ble_mesh_tx_terminal_reason {
    BLE_MESH_TX_TERMINAL_TX_DONE = 0,
    BLE_MESH_TX_TERMINAL_TX_FAILED,
    BLE_MESH_TX_TERMINAL_EXPIRED,
    BLE_MESH_TX_TERMINAL_CANCELED,
    BLE_MESH_TX_TERMINAL_EVICTED,
    BLE_MESH_TX_TERMINAL_PEER_RESET,
    BLE_MESH_TX_TERMINAL_IDENTITY_RESET,
} ble_mesh_tx_terminal_reason_t;

typedef enum ble_mesh_tx_retire_status {
    BLE_MESH_TX_RETIRE_OK = 0,
    BLE_MESH_TX_RETIRE_INVALID,
} ble_mesh_tx_retire_status_t;

typedef struct ble_mesh_tx_queue_entry {
    uint8_t occupied;
    ble_mesh_tx_ordinal_t ordinal;
    ble_mesh_tx_item_t item;
} ble_mesh_tx_queue_entry_t;

typedef struct ble_mesh_tx_queue {
    ble_mesh_tx_queue_entry_t entries[BLE_MESH_TX_QUEUE_CAPACITY];
    uint8_t count;
    uint8_t custody_bypass_count;
    ble_mesh_tx_ordinal_t next_ordinal;
} ble_mesh_tx_queue_t;

void ble_mesh_tx_queue_init(ble_mesh_tx_queue_t *queue);
ble_mesh_tx_enqueue_result_t ble_mesh_tx_queue_enqueue(
    ble_mesh_tx_queue_t *queue, const ble_mesh_tx_item_t *item);
ble_mesh_tx_select_status_t ble_mesh_tx_queue_select_due(
    ble_mesh_tx_queue_t *queue, uint32_t now_ms,
    ble_mesh_tx_token_t promoted_custody_token,
    ble_mesh_tx_selection_t *selection_out);
ble_mesh_tx_expiry_status_t ble_mesh_tx_queue_collect_due_expiry(
    ble_mesh_tx_queue_t *queue, uint32_t now_ms, uint8_t anonymous_limit,
    ble_mesh_tx_expiry_result_t *result_out);
ble_mesh_tx_retire_status_t ble_mesh_tx_queue_retire(
    ble_mesh_tx_queue_t *queue, uint8_t index,
    ble_mesh_tx_terminal_reason_t reason,
    const ble_radio_tx_result_t *tx_result);
ble_mesh_tx_retire_status_t ble_mesh_tx_queue_cancel(
    ble_mesh_tx_queue_t *queue, ble_mesh_tx_token_t token);
int ble_mesh_tx_queue_remove(ble_mesh_tx_queue_t *queue, uint8_t index,
                              ble_mesh_tx_item_t *removed_out);
int ble_mesh_tx_queue_complete(ble_mesh_tx_queue_t *queue, uint8_t index,
                               uint8_t completed_channel_mask,
                               ble_mesh_tx_token_t promoted_custody_token);
uint8_t ble_mesh_tx_queue_count(const ble_mesh_tx_queue_t *queue);

#endif /* BLE_MESH_TX_QUEUE_H */
