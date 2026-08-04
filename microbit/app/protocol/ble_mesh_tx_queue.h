#ifndef BLE_MESH_TX_QUEUE_H
#define BLE_MESH_TX_QUEUE_H

#include <stdint.h>

#include "ble_radio.h"

#define BLE_MESH_TX_QUEUE_CAPACITY 4u
#define BLE_MESH_TX_TOKEN_NONE     0u

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

typedef struct ble_mesh_tx_item {
    uint8_t adv_len;
    uint8_t channel_mask;
    ble_mesh_tx_priority_t priority;
    ble_mesh_tx_service_class_t service_class;
    uint32_t not_before_ms;
    ble_mesh_tx_token_t token;
    uint8_t adv_data[BLE_ADV_MAX_DATA];
} ble_mesh_tx_item_t;

typedef uint32_t ble_mesh_tx_ordinal_t;

typedef enum ble_mesh_tx_enqueue_status {
    BLE_MESH_TX_ENQUEUE_OK = 0,
    BLE_MESH_TX_ENQUEUE_FULL,
    BLE_MESH_TX_ENQUEUE_INVALID,
    BLE_MESH_TX_ENQUEUE_TOO_LONG,
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
int ble_mesh_tx_queue_remove(ble_mesh_tx_queue_t *queue, uint8_t index,
                             ble_mesh_tx_item_t *removed_out);
int ble_mesh_tx_queue_complete(ble_mesh_tx_queue_t *queue, uint8_t index,
                               uint8_t completed_channel_mask,
                               ble_mesh_tx_token_t promoted_custody_token);
uint8_t ble_mesh_tx_queue_count(const ble_mesh_tx_queue_t *queue);

#endif /* BLE_MESH_TX_QUEUE_H */
