#include "ble_mesh_tx_queue.h"

#define BLE_MESH_TX_ENTRY_OCCUPIED       0x01u
#define BLE_MESH_TX_ENTRY_BYPASS_PENDING 0x80u

static void clear_bytes(void *dst, uint32_t len)
{
    uint8_t *bytes = (uint8_t *)dst;

    while (len > 0u) {
        *bytes++ = 0u;
        len--;
    }
}

static int time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (uint32_t)(now_ms - deadline_ms) < 0x80000000UL;
}

static int ordinal_is_older(ble_mesh_tx_ordinal_t left,
                            ble_mesh_tx_ordinal_t right)
{
    return left != right && (uint32_t)(left - right) >= 0x80000000UL;
}

static int time_is_earlier(uint32_t left_ms, uint32_t right_ms)
{
    return left_ms != right_ms &&
        (uint32_t)(left_ms - right_ms) >= 0x80000000UL;
}

static int item_is_valid(const ble_mesh_tx_item_t *item);
static int queue_remove_unchecked(ble_mesh_tx_queue_t *queue, uint8_t index,
                                  ble_mesh_tx_item_t *removed_out);

static int queue_is_consistent(const ble_mesh_tx_queue_t *queue)
{
    uint8_t index;
    uint8_t other_index;
    uint8_t occupied = 0u;

    if (queue == NULL || !tron_timer_config_is_valid(&tron_timer_config) ||
        queue->count > BLE_MESH_TX_QUEUE_CAPACITY) {
        return 0;
    }

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (queue->entries[index].occupied != 0u) {
            if ((queue->entries[index].occupied & BLE_MESH_TX_ENTRY_OCCUPIED) == 0u ||
                (queue->entries[index].occupied &
                 (uint8_t)~(BLE_MESH_TX_ENTRY_OCCUPIED |
                             BLE_MESH_TX_ENTRY_BYPASS_PENDING)) != 0u) {
                return 0;
            }
            if (!item_is_valid(&queue->entries[index].item)) {
                return 0;
            }
            for (other_index = (uint8_t)(index + 1u);
                 other_index < BLE_MESH_TX_QUEUE_CAPACITY; other_index++) {
                if (queue->entries[other_index].occupied != 0u &&
                    queue->entries[index].item.token != BLE_MESH_TX_TOKEN_NONE &&
                    queue->entries[index].item.token ==
                        queue->entries[other_index].item.token) {
                    return 0;
                }
            }
            occupied++;
        }
    }
    return occupied == queue->count;
}

static int item_is_valid(const ble_mesh_tx_item_t *item)
{
    if (item == NULL || item->channel_mask == 0u ||
        (item->channel_mask & (uint8_t)~BLE_RADIO_ADV_CH_ALL) != 0u ||
        item->priority < BLE_MESH_TX_PRIORITY_RELAY ||
        item->priority > BLE_MESH_TX_PRIORITY_HACK ||
        item->service_class < BLE_MESH_TX_SERVICE_BEST_EFFORT ||
        item->service_class > BLE_MESH_TX_SERVICE_CUSTODY_DATA ||
        item->sweep_count < BLE_MESH_TX_SWEEP_COUNT_ONE ||
        item->sweep_count > BLE_MESH_TX_SWEEP_COUNT_TWO ||
        item->budget_class < BLE_MESH_TX_BUDGET_GENERAL ||
        item->budget_class > BLE_MESH_TX_BUDGET_CRITICAL) {
        return 0;
    }

    if (item->service_class == BLE_MESH_TX_SERVICE_CUSTODY_DATA &&
        (item->sweep_count != BLE_MESH_TX_SWEEP_COUNT_TWO ||
         item->token == BLE_MESH_TX_TOKEN_NONE)) {
        return 0;
    }
    return 1;
}

static int item_is_anonymous(const ble_mesh_tx_item_t *item)
{
    return item->token == BLE_MESH_TX_TOKEN_NONE;
}

static int terminal_reason_is_valid(ble_mesh_tx_terminal_reason_t reason)
{
    return reason >= BLE_MESH_TX_TERMINAL_TX_DONE &&
        reason <= BLE_MESH_TX_TERMINAL_IDENTITY_RESET;
}

static int terminal_reason_is_physical(ble_mesh_tx_terminal_reason_t reason)
{
    return reason == BLE_MESH_TX_TERMINAL_TX_DONE ||
        reason == BLE_MESH_TX_TERMINAL_TX_FAILED;
}

static int terminal_evidence_is_valid(const ble_mesh_tx_item_t *item,
                                       ble_mesh_tx_terminal_reason_t reason,
                                       const ble_radio_tx_result_t *tx_result)
{
    uint8_t index;
    uint8_t completed_channel_mask = 0u;

    if (!terminal_reason_is_valid(reason)) {
        return 0;
    }
    if (!terminal_reason_is_physical(reason)) {
        return tx_result == NULL;
    }
    if (tx_result == NULL ||
        tx_result->requested_channel_mask != item->channel_mask ||
        tx_result->requested_sweep_count != item->sweep_count ||
        tx_result->requested_sweep_count == 0u ||
        tx_result->requested_sweep_count > BLE_RADIO_MAX_ADV_SWEEPS ||
        tx_result->attempted_sweep_count > tx_result->requested_sweep_count ||
        tx_result->attempted_sweep_count > BLE_RADIO_MAX_ADV_SWEEPS ||
        (tx_result->completed_channel_mask &
          (uint8_t)~item->channel_mask) != 0u) {
        return 0;
    }
    for (index = 0u; index < BLE_RADIO_MAX_ADV_SWEEPS; index++) {
        uint8_t completed = tx_result->completed_channel_masks[index];

        if ((completed & (uint8_t)~item->channel_mask) != 0u ||
            (index >= tx_result->attempted_sweep_count && completed != 0u)) {
            return 0;
        }
        completed_channel_mask |= completed;
    }
    if (tx_result->completed_channel_mask != completed_channel_mask) {
        return 0;
    }
    if (tx_result->fault == BLE_RADIO_OP_OK) {
        if (tx_result->attempted_sweep_count != tx_result->requested_sweep_count) {
            return 0;
        }
        for (index = 0u; index < tx_result->requested_sweep_count; index++) {
            if (tx_result->completed_channel_masks[index] != item->channel_mask) {
                return 0;
            }
        }
    }
    if (reason == BLE_MESH_TX_TERMINAL_TX_DONE) {
        return tx_result->completed_channel_mask != 0u;
    }
    return tx_result->completed_channel_mask == 0u;
}

static void record_terminal_custody_bypass(
    ble_mesh_tx_queue_t *queue, const ble_mesh_tx_queue_entry_t *entry,
    ble_mesh_tx_terminal_reason_t reason,
    const ble_radio_tx_result_t *tx_result)
{
    if (entry->item.service_class == BLE_MESH_TX_SERVICE_CUSTODY_DATA) {
        queue->custody_bypass_count = 0u;
    } else if (reason == BLE_MESH_TX_TERMINAL_TX_DONE && tx_result != NULL &&
               tx_result->completed_channel_mask != 0u &&
               (entry->occupied & BLE_MESH_TX_ENTRY_BYPASS_PENDING) != 0u &&
               queue->custody_bypass_count <
                   tron_timer_config.scheduler_custody_bypass_max) {
        queue->custody_bypass_count++;
    }
}

static int entry_has_earlier_expiry(const ble_mesh_tx_queue_entry_t *left,
                                    const ble_mesh_tx_queue_entry_t *right)
{
    return time_is_earlier(left->item.expiry_ms, right->item.expiry_ms) ||
        (left->item.expiry_ms == right->item.expiry_ms &&
         ordinal_is_older(left->ordinal, right->ordinal));
}

static int token_is_queued(const ble_mesh_tx_queue_t *queue,
                           ble_mesh_tx_token_t token)
{
    uint8_t index;

    if (token == BLE_MESH_TX_TOKEN_NONE) {
        return 0;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (queue->entries[index].occupied != 0u &&
            queue->entries[index].item.token == token) {
            return 1;
        }
    }
    return 0;
}

static void store_entry(ble_mesh_tx_queue_t *queue, uint8_t index,
                        const ble_mesh_tx_item_t *item)
{
    queue->entries[index].occupied = BLE_MESH_TX_ENTRY_OCCUPIED;
    queue->entries[index].ordinal = queue->next_ordinal;
    queue->entries[index].item = *item;
    queue->next_ordinal++;
}

void ble_mesh_tx_queue_init(ble_mesh_tx_queue_t *queue)
{
    if (queue != NULL && tron_timer_config_is_valid(&tron_timer_config)) {
        clear_bytes(queue, (uint32_t)sizeof(*queue));
    }
}

ble_mesh_tx_enqueue_result_t ble_mesh_tx_queue_enqueue(
    ble_mesh_tx_queue_t *queue, const ble_mesh_tx_item_t *item)
{
    ble_mesh_tx_enqueue_result_t result;
    uint8_t index;
    uint8_t free_index = BLE_MESH_TX_QUEUE_CAPACITY;
    uint8_t eviction_index = BLE_MESH_TX_QUEUE_CAPACITY;

    clear_bytes(&result, (uint32_t)sizeof(result));
    result.status = BLE_MESH_TX_ENQUEUE_INVALID;

    if (!queue_is_consistent(queue) || item == NULL) {
        return result;
    }
    if (item->adv_len > BLE_ADV_MAX_DATA) {
        result.status = BLE_MESH_TX_ENQUEUE_TOO_LONG;
        return result;
    }
    if (!item_is_valid(item) || token_is_queued(queue, item->token)) {
        return result;
    }

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_queue_entry_t *entry = &queue->entries[index];

        if (entry->occupied == 0u) {
            free_index = index;
            break;
        }
    }

    if (free_index < BLE_MESH_TX_QUEUE_CAPACITY) {
        store_entry(queue, free_index, item);
        queue->count++;
        result.status = BLE_MESH_TX_ENQUEUE_OK;
        result.accepted_token = item->token;
        return result;
    }

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_queue_entry_t *entry = &queue->entries[index];

        if (entry->item.priority < item->priority &&
            (eviction_index == BLE_MESH_TX_QUEUE_CAPACITY ||
             ordinal_is_older(entry->ordinal,
                              queue->entries[eviction_index].ordinal))) {
            eviction_index = index;
        }
    }
    if (eviction_index == BLE_MESH_TX_QUEUE_CAPACITY) {
        result.status = BLE_MESH_TX_ENQUEUE_FULL;
        return result;
    }

    result.evicted_token = queue->entries[eviction_index].item.token;
    if (ble_mesh_tx_queue_retire(queue, eviction_index,
                                 BLE_MESH_TX_TERMINAL_EVICTED, NULL) !=
        BLE_MESH_TX_RETIRE_OK) {
        clear_bytes(&result, (uint32_t)sizeof(result));
        result.status = BLE_MESH_TX_ENQUEUE_INVALID;
        return result;
    }
    store_entry(queue, eviction_index, item);
    queue->count++;
    result.status = BLE_MESH_TX_ENQUEUE_OK;
    result.accepted_token = item->token;
    return result;
}

ble_mesh_tx_select_status_t ble_mesh_tx_queue_select_due(
    ble_mesh_tx_queue_t *queue, uint32_t now_ms,
    ble_mesh_tx_token_t promoted_custody_token,
    ble_mesh_tx_selection_t *selection_out)
{
    uint8_t index;
    uint8_t selected_index = BLE_MESH_TX_QUEUE_CAPACITY;
    uint8_t promoted_index = BLE_MESH_TX_QUEUE_CAPACITY;

    if (selection_out != NULL) {
        clear_bytes(selection_out, (uint32_t)sizeof(*selection_out));
    }
    if (!queue_is_consistent(queue) || selection_out == NULL) {
        if (selection_out != NULL) {
            selection_out->status = BLE_MESH_TX_SELECT_INVALID;
        }
        return BLE_MESH_TX_SELECT_INVALID;
    }

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        queue->entries[index].occupied &=
            (uint8_t)~BLE_MESH_TX_ENTRY_BYPASS_PENDING;
    }

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_queue_entry_t *entry = &queue->entries[index];

        if (entry->occupied == 0u || time_reached(now_ms, entry->item.expiry_ms) ||
            !time_reached(now_ms, entry->item.not_before_ms)) {
            continue;
        }
        if (promoted_custody_token != BLE_MESH_TX_TOKEN_NONE &&
            entry->item.service_class == BLE_MESH_TX_SERVICE_CUSTODY_DATA &&
            entry->item.token == promoted_custody_token) {
            promoted_index = index;
        }
        if (selected_index == BLE_MESH_TX_QUEUE_CAPACITY ||
            entry->item.priority > queue->entries[selected_index].item.priority ||
            (entry->item.priority == queue->entries[selected_index].item.priority &&
             entry_has_earlier_expiry(entry,
                                      &queue->entries[selected_index]))) {
            selected_index = index;
        }
    }

    if (promoted_index == BLE_MESH_TX_QUEUE_CAPACITY) {
        queue->custody_bypass_count = 0u;
    } else if (queue->custody_bypass_count >=
               tron_timer_config.scheduler_custody_bypass_max) {
        selected_index = promoted_index;
    } else if (selected_index != promoted_index) {
        /* complete() must count only a bypass selected while the promoted
         * custody item was already due; the frozen API has no time argument. */
        queue->entries[selected_index].occupied |=
            BLE_MESH_TX_ENTRY_BYPASS_PENDING;
    }

    if (selected_index == BLE_MESH_TX_QUEUE_CAPACITY) {
        selection_out->status = BLE_MESH_TX_SELECT_NONE;
        return BLE_MESH_TX_SELECT_NONE;
    }

    selection_out->status = BLE_MESH_TX_SELECT_OK;
    selection_out->index = selected_index;
    selection_out->item = queue->entries[selected_index].item;
    return BLE_MESH_TX_SELECT_OK;
}

ble_mesh_tx_expiry_status_t ble_mesh_tx_queue_collect_due_expiry(
    ble_mesh_tx_queue_t *queue, uint32_t now_ms, uint8_t anonymous_limit,
    ble_mesh_tx_expiry_result_t *result_out)
{
    uint8_t index;
    uint8_t selected_index = BLE_MESH_TX_QUEUE_CAPACITY;

    if (result_out != NULL) {
        clear_bytes(result_out, (uint32_t)sizeof(*result_out));
        result_out->status = BLE_MESH_TX_EXPIRY_INVALID;
    }
    if (!queue_is_consistent(queue) || result_out == NULL) {
        return BLE_MESH_TX_EXPIRY_INVALID;
    }

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_queue_entry_t *entry = &queue->entries[index];

        if (entry->occupied == 0u ||
            !time_reached(now_ms, entry->item.expiry_ms)) {
            continue;
        }
        if (item_is_anonymous(&entry->item)) {
            if (result_out->anonymous_purged_count < anonymous_limit &&
                queue_remove_unchecked(queue, index, NULL)) {
                result_out->anonymous_purged_count++;
            }
            continue;
        }
        if (selected_index == BLE_MESH_TX_QUEUE_CAPACITY ||
            entry_has_earlier_expiry(entry, &queue->entries[selected_index])) {
            selected_index = index;
        }
    }
    if (selected_index < BLE_MESH_TX_QUEUE_CAPACITY) {
        result_out->tracked_due = 1u;
        result_out->tracked_index = selected_index;
        result_out->tracked_item = queue->entries[selected_index].item;
    }
    result_out->status = BLE_MESH_TX_EXPIRY_OK;
    return BLE_MESH_TX_EXPIRY_OK;
}

int ble_mesh_tx_queue_remove(ble_mesh_tx_queue_t *queue, uint8_t index,
                              ble_mesh_tx_item_t *removed_out)
{
    if (!queue_is_consistent(queue) || index >= BLE_MESH_TX_QUEUE_CAPACITY ||
        queue->entries[index].occupied == 0u ||
        !item_is_anonymous(&queue->entries[index].item)) {
        return 0;
    }

    return queue_remove_unchecked(queue, index, removed_out);
}

static int queue_remove_unchecked(ble_mesh_tx_queue_t *queue, uint8_t index,
                                  ble_mesh_tx_item_t *removed_out)
{
    if (queue == NULL || index >= BLE_MESH_TX_QUEUE_CAPACITY ||
        queue->entries[index].occupied == 0u || queue->count == 0u) {
        return 0;
    }

    if (removed_out != NULL) {
        *removed_out = queue->entries[index].item;
    }
    clear_bytes(&queue->entries[index],
                (uint32_t)sizeof(queue->entries[index]));
    queue->count--;
    return 1;
}

ble_mesh_tx_retire_status_t ble_mesh_tx_queue_retire(
    ble_mesh_tx_queue_t *queue, uint8_t index,
    ble_mesh_tx_terminal_reason_t reason,
    const ble_radio_tx_result_t *tx_result)
{
    ble_mesh_tx_queue_entry_t *entry;

    if (!queue_is_consistent(queue) || index >= BLE_MESH_TX_QUEUE_CAPACITY ||
        queue->entries[index].occupied == 0u) {
        return BLE_MESH_TX_RETIRE_INVALID;
    }
    entry = &queue->entries[index];
    if (!terminal_evidence_is_valid(&entry->item, reason, tx_result)) {
        return BLE_MESH_TX_RETIRE_INVALID;
    }

    record_terminal_custody_bypass(queue, entry, reason, tx_result);
    if (!queue_remove_unchecked(queue, index, NULL)) {
        return BLE_MESH_TX_RETIRE_INVALID;
    }
    return BLE_MESH_TX_RETIRE_OK;
}

ble_mesh_tx_retire_status_t ble_mesh_tx_queue_cancel(
    ble_mesh_tx_queue_t *queue, ble_mesh_tx_token_t token)
{
    uint8_t index;

    if (!queue_is_consistent(queue) || token == BLE_MESH_TX_TOKEN_NONE) {
        return BLE_MESH_TX_RETIRE_INVALID;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (queue->entries[index].occupied != 0u &&
            queue->entries[index].item.token == token) {
            return ble_mesh_tx_queue_retire(queue, index,
                                             BLE_MESH_TX_TERMINAL_CANCELED,
                                             NULL);
        }
    }
    return BLE_MESH_TX_RETIRE_OK;
}

int ble_mesh_tx_queue_complete(ble_mesh_tx_queue_t *queue, uint8_t index,
                                uint8_t completed_channel_mask,
                                ble_mesh_tx_token_t promoted_custody_token)
{
    ble_radio_tx_result_t tx_result;
    uint8_t sweep_index;

    if ((completed_channel_mask & (uint8_t)~BLE_RADIO_ADV_CH_ALL) != 0u ||
        !queue_is_consistent(queue) || index >= BLE_MESH_TX_QUEUE_CAPACITY ||
        queue->entries[index].occupied == 0u) {
        return 0;
    }

    clear_bytes(&tx_result, (uint32_t)sizeof(tx_result));
    (void)promoted_custody_token;
    tx_result.requested_channel_mask = queue->entries[index].item.channel_mask;
    tx_result.completed_channel_mask = completed_channel_mask;
    tx_result.requested_sweep_count =
        (uint8_t)queue->entries[index].item.sweep_count;
    tx_result.attempted_sweep_count = completed_channel_mask ==
        queue->entries[index].item.channel_mask ?
        tx_result.requested_sweep_count : BLE_MESH_TX_SWEEP_COUNT_ONE;
    for (sweep_index = 0u; sweep_index < tx_result.attempted_sweep_count;
         sweep_index++) {
        tx_result.completed_channel_masks[sweep_index] = completed_channel_mask;
    }
    tx_result.fault = completed_channel_mask == queue->entries[index].item.channel_mask ?
        BLE_RADIO_OP_OK : BLE_RADIO_OP_STATE_TIMEOUT;
    return ble_mesh_tx_queue_retire(
               queue, index, completed_channel_mask == 0u ?
                   BLE_MESH_TX_TERMINAL_TX_FAILED :
                   BLE_MESH_TX_TERMINAL_TX_DONE,
               &tx_result) == BLE_MESH_TX_RETIRE_OK;
}

uint8_t ble_mesh_tx_queue_count(const ble_mesh_tx_queue_t *queue)
{
    return queue == NULL ? 0u : queue->count;
}
