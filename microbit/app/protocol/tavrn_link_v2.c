#include "tavrn_link_v2.h"

#include <string.h>

#if defined(TAVRN_LINK_V2_HOST_TEST_IMMEDIATE_HACK) && \
    !defined(BLE_RADIO_HOST_TEST)
#error "immediate-HACK RED mode is host-test-only"
#endif

#define TAVRN_LINK_HALF_RANGE 0x80000000u
#define TAVRN_LINK_PROFILE_MAX_ATTEMPTS 3u
#define TAVRN_LINK_PROFILE_BUSY_RESPONSES 3u

static int time_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static int time_value_valid(uint32_t value)
{
    return value < TAVRN_LINK_HALF_RANGE;
}

static int admission_before(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) < 0;
}

static void clear_event(tavrn_link_event_t *event)
{
    if (event != NULL) {
        memset(event, 0, sizeof(*event));
        event->type = TAVRN_LINK_EVENT_NONE;
    }
}

static int logical_id_valid(const tavrn_logical_id_t *id, int allow_broadcast)
{
    if (id == NULL) {
        return 0;
    }
    if (id->width == TAVRN_IDENTITY_SID16) {
        if (id->value == 0u) {
            return 0;
        }
        return allow_broadcast || id->value != 0xffffu;
    }
    if (id->width == TAVRN_IDENTITY_SID8) {
        if ((id->value & 0xff00u) != 0u || id->value == 0u) {
            return 0;
        }
        return allow_broadcast || id->value != 0xffu;
    }
    return 0;
}

static int logical_id_equal(const tavrn_logical_id_t *left,
                            const tavrn_logical_id_t *right)
{
    return left != NULL && right != NULL && left->width == right->width &&
           left->value == right->value;
}

static int bytes_equal(const uint8_t *left, const uint8_t *right, uint8_t length)
{
    uint8_t i;

    if (left == NULL || right == NULL) {
        return 0;
    }
    for (i = 0u; i < length; i++) {
        if (left[i] != right[i]) {
            return 0;
        }
    }
    return 1;
}

static int direct_peer_equal(const tavrn_direct_peer_t *left,
                             const tavrn_direct_peer_t *right)
{
    return left != NULL && right != NULL &&
           logical_id_equal(&left->logical_id, &right->logical_id) &&
           bytes_equal(left->adva.bytes, right->adva.bytes, TAVRN_ADVA_LEN);
}

static int link_data_equal(const tavrn_link_data_t *left,
                           const tavrn_link_data_t *right)
{
    return left != NULL && right != NULL &&
           logical_id_equal(&left->origin, &right->origin) &&
           logical_id_equal(&left->final_destination, &right->final_destination) &&
           left->data_seq == right->data_seq && left->ttl == right->ttl &&
           left->hops == right->hops && left->app_kind == right->app_kind &&
           left->app_source == right->app_source && left->urgent == right->urgent &&
           left->app_len == right->app_len &&
           bytes_equal(left->app_bytes, right->app_bytes,
                       TAVRN_LINK_APP_BYTES) &&
           left->ownership == right->ownership;
}

/* DATA belongs to a resetting logical identity when that identity was the
 * direct hop for this copy, its origin, or its final destination. */
static int data_uses_direct_peer(const tavrn_logical_id_t *direct_peer,
                                 const tavrn_link_data_t *data,
                                 const tavrn_direct_peer_t *peer)
{
    return direct_peer != NULL && data != NULL && peer != NULL &&
        (logical_id_equal(direct_peer, &peer->logical_id) ||
         logical_id_equal(&data->origin, &peer->logical_id) ||
         logical_id_equal(&data->final_destination, &peer->logical_id));
}

static int custody_uses_direct_peer(const tavrn_custody_slot_t *slot,
                                    const tavrn_direct_peer_t *peer)
{
    return slot != NULL && slot->phase != TAVRN_CUSTODY_FREE &&
        data_uses_direct_peer(&slot->next_hop.logical_id, &slot->data, peer);
}

static int candidate_uses_direct_peer(const tavrn_rx_data_candidate_t *candidate,
                                      const tavrn_direct_peer_t *peer)
{
    return candidate != NULL && data_uses_direct_peer(
        &candidate->transmitter.logical_id, &candidate->data, peer);
}

static int candidate_equal(const tavrn_rx_data_candidate_t *left,
                           const tavrn_rx_data_candidate_t *right)
{
    return left != NULL && right != NULL && left->token == right->token &&
           direct_peer_equal(&left->transmitter, &right->transmitter) &&
           left->rssi_magnitude_db == right->rssi_magnitude_db &&
           link_data_equal(&left->data, &right->data) &&
           left->deadline_ms == right->deadline_ms;
}

static int adva_valid(const tavrn_adva_t *adva)
{
    uint8_t all_zero = 1u;
    uint8_t all_one = 1u;
    uint8_t random_all_zero = 1u;
    uint8_t random_all_one = 1u;
    uint8_t i;

    if (adva == NULL || (adva->bytes[5] & 0xc0u) != 0xc0u) {
        return 0;
    }
    for (i = 0u; i < TAVRN_ADVA_LEN; i++) {
        if (adva->bytes[i] != 0u) {
            all_zero = 0u;
        }
        if (adva->bytes[i] != 0xffu) {
            all_one = 0u;
        }
        if (i < TAVRN_ADVA_LEN - 1u && adva->bytes[i] != 0u) {
            random_all_zero = 0u;
        }
        if (i < TAVRN_ADVA_LEN - 1u && adva->bytes[i] != 0xffu) {
            random_all_one = 0u;
        }
    }
    if ((adva->bytes[5] & 0x3fu) != 0u) {
        random_all_zero = 0u;
    }
    if ((adva->bytes[5] & 0x3fu) != 0x3fu) {
        random_all_one = 0u;
    }
    return all_zero == 0u && all_one == 0u && random_all_zero == 0u &&
           random_all_one == 0u;
}

static int direct_peer_valid(const tavrn_direct_peer_t *peer)
{
    uint16_t expected;

    if (peer == NULL || !adva_valid(&peer->adva) ||
        !logical_id_valid(&peer->logical_id, 0)) {
        return 0;
    }
    expected = peer->logical_id.width == TAVRN_IDENTITY_SID8 ?
        peer->adva.bytes[0] :
        (uint16_t)peer->adva.bytes[0] | ((uint16_t)peer->adva.bytes[1] << 8);
    return peer->logical_id.value == expected;
}

static int data_valid(const tavrn_link_data_t *data)
{
    if (data == NULL || !logical_id_valid(&data->origin, 0) ||
        !logical_id_valid(&data->final_destination, 0) ||
        data->origin.width != data->final_destination.width || data->ttl > 15u ||
        data->hops > 15u || data->urgent > 1u ||
        data->app_len > (data->origin.width == TAVRN_IDENTITY_SID16 ?
                         TAVRN_LINK_SID16_APP_BYTES : TAVRN_LINK_APP_BYTES) ||
        (data->ownership != TAVRN_DATA_ORIGINATED &&
         data->ownership != TAVRN_DATA_TRANSIT)) {
        return 0;
    }
    return 1;
}

static int flood_valid(const tavrn_codec_flood_t *flood)
{
    return flood != NULL && logical_id_valid(&flood->origin, 0) &&
           flood->ttl <= 15u && flood->hops <= 15u && flood->urgent <= 1u &&
           flood->body_len <= TAVRN_FLOOD_BODY_MAX;
}

static uint16_t control_pdu_id(const tavrn_validated_control_t *control,
                               uint8_t offset)
{
    if ((control->pdu[5] & 0x80u) != 0u) {
        return control->pdu[offset];
    }
    return (uint16_t)control->pdu[offset] |
        ((uint16_t)control->pdu[(uint8_t)(offset + 1u)] << 8);
}

static int control_send_shape_valid(const tavrn_link_v2_t *link,
                                     const tavrn_validated_control_t *control,
                                     const tavrn_direct_peer_t *next_hop_or_null,
                                     uint8_t controlled_flood)
{
    uint8_t receiver_offset;

    if (link == NULL || control == NULL || controlled_flood > 1u ||
        control->pdu_len < 7u || control->type != (tavrn_wire_type_t)control->pdu[4] ||
        control->pdu[0] != 0x54u || control->pdu[1] != 0x52u ||
        control->pdu[2] != 0x02u || control->pdu[3] != link->config.network_id) {
        return 0;
    }
    if (control->type == TAVRN_WIRE_SYNC_OFFER ||
        control->type == TAVRN_WIRE_SYNC_PULL ||
        control->type == TAVRN_WIRE_SYNC_DATA) {
        return controlled_flood == 0u && next_hop_or_null == NULL;
    }
    if (control->type == TAVRN_WIRE_TC_UPDATE) {
        return controlled_flood != 0u && next_hop_or_null == NULL;
    }
    if ((control->pdu[5] & 0x80u) != 0u &&
        link->config.local_peer.logical_id.width != TAVRN_IDENTITY_SID8) {
        return 0;
    }
    if ((control->pdu[5] & 0x80u) == 0u &&
        link->config.local_peer.logical_id.width != TAVRN_IDENTITY_SID16 &&
        control->type != TAVRN_WIRE_HELLO) {
        return 0;
    }
    if (control->type == TAVRN_WIRE_E_RREQ || control->type == TAVRN_WIRE_E_RERR) {
        return controlled_flood != 0u && next_hop_or_null == NULL;
    }
    if (control->type == TAVRN_WIRE_HELLO) {
        return controlled_flood == 0u && next_hop_or_null == NULL;
    }
    if (control->type != TAVRN_WIRE_E_RREP &&
        control->type != TAVRN_WIRE_E_RREP_ACK) {
        return 0;
    }
    if (controlled_flood != 0u || !direct_peer_valid(next_hop_or_null) ||
        next_hop_or_null->logical_id.width != link->config.local_peer.logical_id.width ||
        logical_id_equal(&next_hop_or_null->logical_id,
                         &link->config.local_peer.logical_id)) {
        return 0;
    }
    receiver_offset = control->type == TAVRN_WIRE_E_RREP ? 7u : 6u;
    return control_pdu_id(control, receiver_offset) ==
        next_hop_or_null->logical_id.value;
}

static int targeted_control_send_shape_valid(
    const tavrn_link_v2_t *link, const tavrn_validated_control_t *control,
    const tavrn_direct_peer_t *next_hop, ble_mesh_tx_token_t token)
{
    if (link != NULL && control != NULL && token >= 0x8000u &&
        control->type == TAVRN_WIRE_E_RREQ) {
        return next_hop == NULL &&
            link->config.local_peer.logical_id.width == TAVRN_IDENTITY_SID8 &&
            control_send_shape_valid(link, control, NULL, 1u);
    }
    if (link == NULL || control == NULL || !direct_peer_valid(next_hop) ||
        token < 0x8000u || control->type != TAVRN_WIRE_HELLO ||
        control->pdu_len != 20u || control->pdu[0] != 0x54u ||
        control->pdu[1] != 0x52u || control->pdu[2] != 0x02u ||
        control->pdu[3] != link->config.network_id ||
        control->pdu[4] != TAVRN_WIRE_HELLO ||
        (control->pdu[5] != 0xb8u && control->pdu[5] != 0xa8u) ||
        control->pdu[17] != 1u ||
        next_hop->logical_id.width != TAVRN_IDENTITY_SID8 ||
        next_hop->logical_id.value != control->pdu[7]) {
        return 0;
    }
    if (control->pdu[6] == 0u || control->pdu[7] == 0u ||
        control->pdu[7] == 0xffu || control->pdu[8] == 0u ||
        control->pdu[8] == 0xffu || control->pdu[18] == 0u ||
        control->pdu[18] == 0xffu ||
        (control->pdu[5] == 0xb8u &&
         (control->pdu[18] != control->pdu[8] || control->pdu[19] != 0x01u)) ||
        (control->pdu[5] == 0xa8u && (control->pdu[19] & 0x0fu) != 0u)) {
        return 0;
    }
    return (control->pdu[6] & 0x0fu) != 0u ||
        memcmp(&control->pdu[9], link->config.local_peer.adva.bytes,
               TAVRN_ADVA_LEN) == 0;
}

static void apply_control_bearer_policy(
    const tavrn_validated_control_t *control, ble_mesh_tx_item_t *item)
{
    int bootstrap;

    bootstrap = control->type == TAVRN_WIRE_HELLO &&
        control->pdu_len > 5u && control->pdu[5] == 0x40u;
    item->sweep_count = control->type == TAVRN_WIRE_E_RREQ ||
            control->type == TAVRN_WIRE_E_RREP || bootstrap ?
        BLE_MESH_TX_SWEEP_COUNT_TWO : BLE_MESH_TX_SWEEP_COUNT_ONE;
    item->budget_class = control->type == TAVRN_WIRE_E_RREP ||
            control->type == TAVRN_WIRE_E_RREP_ACK ||
            control->type == TAVRN_WIRE_SYNC_OFFER ||
            control->type == TAVRN_WIRE_SYNC_DATA ?
        BLE_MESH_TX_BUDGET_CRITICAL : BLE_MESH_TX_BUDGET_GENERAL;
}

static tavrn_codec_config_t codec_config(const tavrn_link_v2_t *link)
{
    tavrn_codec_config_t config;

    memset(&config, 0, sizeof(config));
    config.network_id = link->config.network_id;
    config.local_peer = link->config.local_peer;
    config.identity_conflict = link->config.identity_conflict;
    config.identity_context = link->config.identity_context;
    return config;
}

static void reset_slot(tavrn_link_v2_t *link, uint8_t index)
{
    if (index >= TAVRN_LINK_CUSTODY_CAPACITY) {
        return;
    }
    memset(&link->custody[index], 0, sizeof(link->custody[index]));
    link->custody[index].phase = TAVRN_CUSTODY_FREE;
    if (link->active_custody_index == index) {
        link->active_custody_index = TAVRN_CUSTODY_SLOT_NONE;
    }
}

static void fill_owned_event(tavrn_link_event_t *event,
                             tavrn_link_event_type_t type,
                             const tavrn_custody_slot_t *slot,
                             tavrn_local_tx_reason_t local_reason,
                             tavrn_mesh_fault_reason_t mesh_fault_reason)
{
    event->type = type;
    event->detail.owned_data.next_hop = slot->next_hop;
    event->detail.owned_data.data = slot->data;
    event->detail.owned_data.requested_channel_mask =
        slot->attempt_requested_channel_mask;
    event->detail.owned_data.completed_channel_mask =
        slot->attempt_completed_channel_mask;
    event->detail.owned_data.attempt_count = slot->attempt_count;
    event->detail.owned_data.busy_response_count = slot->busy_response_count;
    event->detail.owned_data.first_tx_ms = slot->first_tx_ms;
    event->detail.owned_data.last_tx_ms = slot->last_tx_ms;
    event->detail.owned_data.final_deadline_ms =
        type == TAVRN_LINK_EVENT_RETRY_EXHAUSTED ? slot->response_deadline_ms :
        slot->transaction_deadline_ms;
    event->detail.owned_data.local_reason = local_reason;
    event->detail.owned_data.mesh_fault_reason = mesh_fault_reason;
}

static void terminal_owned(tavrn_link_v2_t *link, uint8_t index,
                           tavrn_link_event_t *event,
                           tavrn_link_event_type_t type,
                           tavrn_local_tx_reason_t local_reason,
                           tavrn_mesh_fault_reason_t mesh_fault_reason)
{
    tavrn_custody_slot_t slot;

    slot = link->custody[index];
    fill_owned_event(event, type, &slot, local_reason, mesh_fault_reason);
    reset_slot(link, index);
    if (type == TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED) {
        link->counters.local_tx_not_attempted++;
    } else if (type == TAVRN_LINK_EVENT_CUSTODY_BUSY_EXPIRED) {
        link->counters.custody_busy_expired++;
    } else if (type == TAVRN_LINK_EVENT_CUSTODY_REJECTED) {
        link->counters.custody_rejected++;
    } else if (type == TAVRN_LINK_EVENT_RETRY_EXHAUSTED) {
        link->counters.retry_exhausted++;
    } else if (type == TAVRN_LINK_EVENT_RADIO_FAULT_TERMINAL) {
        link->counters.radio_fault_terminal++;
    } else if (type == TAVRN_LINK_EVENT_SERVICE_FAULT_TERMINAL) {
        link->counters.service_fault_terminal++;
    }
}

static void transferred(tavrn_link_v2_t *link, uint8_t index,
                        tavrn_hack_status_t status, tavrn_link_event_t *event)
{
    tavrn_custody_slot_t slot = link->custody[index];

    event->type = TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED;
    event->detail.transferred_data.next_hop = slot.next_hop;
    event->detail.transferred_data.data = slot.data;
    event->detail.transferred_data.status = status;
    event->detail.transferred_data.requested_channel_mask =
        slot.attempt_requested_channel_mask;
    event->detail.transferred_data.completed_channel_mask =
        slot.attempt_completed_channel_mask;
    event->detail.transferred_data.attempt_count = slot.attempt_count;
    event->detail.transferred_data.first_tx_ms = slot.first_tx_ms;
    event->detail.transferred_data.last_tx_ms = slot.last_tx_ms;
    reset_slot(link, index);
    link->counters.custody_transferred++;
}

static int token_in_use(const tavrn_link_v2_t *link, ble_mesh_tx_token_t token)
{
    uint8_t i;

    if (token == BLE_MESH_TX_TOKEN_NONE) {
        return 1;
    }
    for (i = 0u; i < TAVRN_LINK_CUSTODY_CAPACITY; i++) {
        if (link->custody[i].phase != TAVRN_CUSTODY_FREE &&
            link->custody[i].scheduler_token == token) {
            return 1;
        }
    }
    if (link->scheduler != NULL) {
        for (i = 0u; i < BLE_MESH_TX_QUEUE_CAPACITY; i++) {
            const ble_mesh_tx_queue_entry_t *entry =
                &link->scheduler->routed_tx_queue.entries[i];

            if (entry->occupied != 0u && entry->item.token == token) {
                return 1;
            }
        }
    }
    return 0;
}

static ble_mesh_tx_token_t allocate_scheduler_token(tavrn_link_v2_t *link)
{
    uint32_t attempts;

    for (attempts = 0u; attempts < 0x7fffu; attempts++) {
        link->next_scheduler_token++;
        if (link->next_scheduler_token == BLE_MESH_TX_TOKEN_NONE ||
            link->next_scheduler_token > 0x7fffu) {
            link->next_scheduler_token = 1u;
        }
        if (!token_in_use(link, link->next_scheduler_token)) {
            return link->next_scheduler_token;
        }
    }
    return BLE_MESH_TX_TOKEN_NONE;
}

/* Token-bearing work uses checked retirement; anonymous work emits no token
 * terminal and keeps the simple removal path. */
static int retire_queued_entry(ble_mesh_tx_queue_t *queue, uint8_t index,
                               ble_mesh_tx_terminal_reason_t reason)
{
    if (queue == NULL || index >= BLE_MESH_TX_QUEUE_CAPACITY ||
        queue->entries[index].occupied == 0u) {
        return 0;
    }
    if (queue->entries[index].item.token == BLE_MESH_TX_TOKEN_NONE) {
        return ble_mesh_tx_queue_remove(queue, index, NULL);
    }
    return ble_mesh_tx_queue_retire(queue, index, reason, NULL) ==
        BLE_MESH_TX_RETIRE_OK;
}

int tavrn_link_v2_tracked_token_in_use(const tavrn_link_v2_t *link,
                                        ble_mesh_tx_token_t token)
{
    return link != NULL && token_in_use(link, token);
}

int tavrn_link_v2_tracked_control_may_admit(const tavrn_link_v2_t *link)
{
    const ble_mesh_tx_queue_t *queue;
    uint8_t index;
    uint8_t occupied = 0u;
    uint8_t free_slot = 0u;
    uint8_t evictable = 0u;

    if (link == NULL || link->scheduler == NULL || link->mesh_fault_latched != 0u) {
        return 0;
    }
    queue = &link->scheduler->routed_tx_queue;
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry = &queue->entries[index];

        if (entry->occupied == 0u) {
            free_slot = 1u;
            continue;
        }
        occupied++;
        if (entry->item.priority < BLE_MESH_TX_PRIORITY_CONTROL) {
            evictable = 1u;
        }
    }
    return occupied == queue->count &&
        (free_slot != 0u || evictable != 0u);
}

#if defined(BLE_RADIO_HOST_TEST)
tavrn_targeted_freshness_status_t tavrn_link_v2_reserve_targeted_low_token(
    tavrn_link_v2_t *link, tavrn_targeted_freshness_low_work_t work,
    uint16_t *token_out)
{
    ble_mesh_tx_token_t token;

    if (token_out != NULL) {
        *token_out = BLE_MESH_TX_TOKEN_NONE;
    }
    if (link == NULL || token_out == NULL ||
        (work != TAVRN_TARGETED_LOW_CUSTODY &&
         work != TAVRN_TARGETED_LOW_BOOTSTRAP)) {
        return TAVRN_TARGETED_FRESHNESS_INVALID;
    }
    token = allocate_scheduler_token(link);
    if (token == BLE_MESH_TX_TOKEN_NONE) {
        return TAVRN_TARGETED_FRESHNESS_BUSY;
    }
    *token_out = token;
    return TAVRN_TARGETED_FRESHNESS_OK;
}
#endif

static tavrn_rx_candidate_token_t allocate_candidate_token(tavrn_link_v2_t *link)
{
    link->next_candidate_token++;
    if (link->next_candidate_token == TAVRN_RX_CANDIDATE_TOKEN_NONE) {
        link->next_candidate_token++;
    }
    return link->next_candidate_token;
}

static int dedupe_key_equal(const tavrn_data_dedupe_entry_t *entry,
                            const tavrn_link_data_t *data)
{
    return entry->valid != 0u && logical_id_equal(&entry->origin, &data->origin) &&
           entry->data_seq == data->data_seq && entry->app_kind == data->app_kind &&
           entry->app_source == data->app_source;
}

static tavrn_data_dedupe_entry_t *find_data_dedupe_exact(
    tavrn_link_v2_t *link, const tavrn_link_data_t *data)
{
    uint8_t index;

    if (link == NULL || data == NULL) {
        return NULL;
    }
    for (index = 0u; index < TAVRN_LINK_DATA_DEDUPE_CAPACITY; index++) {
        if (dedupe_key_equal(&link->data_dedupe[index], data)) {
            return &link->data_dedupe[index];
        }
    }
    return NULL;
}

static int transit_dedupe_can_release(tavrn_link_v2_t *link,
                                      const tavrn_link_data_t *data)
{
    tavrn_data_dedupe_entry_t *entry;

    if (data == NULL || data->ownership != TAVRN_DATA_TRANSIT) {
        return data != NULL;
    }
    entry = find_data_dedupe_exact(link, data);
    return entry != NULL && entry->custody_pinned != 0u &&
        logical_id_equal(&entry->final_destination, &data->final_destination);
}

static int release_transit_dedupe(tavrn_link_v2_t *link,
                                  const tavrn_link_data_t *data)
{
    tavrn_data_dedupe_entry_t *entry;

    if (!transit_dedupe_can_release(link, data)) {
        return 0;
    }
    if (data->ownership != TAVRN_DATA_TRANSIT) {
        return 1;
    }
    entry = find_data_dedupe_exact(link, data);
    if (entry->incarnation_clear_on_release != 0u) {
        memset(entry, 0, sizeof(*entry));
    } else {
        entry->custody_pinned = 0u;
        entry->external_custody_owner = 0u;
    }
    return 1;
}

static void clear_data_dedupe_exact(tavrn_link_v2_t *link,
                                    const tavrn_link_data_t *data)
{
    tavrn_data_dedupe_entry_t *entry = find_data_dedupe_exact(link, data);

    if (entry != NULL && entry->custody_pinned == 0u) {
        memset(entry, 0, sizeof(*entry));
    }
}

static void prune_data_dedupe(tavrn_link_v2_t *link, uint32_t now_ms)
{
    uint8_t i;

    for (i = 0u; i < TAVRN_LINK_DATA_DEDUPE_CAPACITY; i++) {
        if (link->data_dedupe[i].valid != 0u &&
            link->data_dedupe[i].custody_pinned == 0u &&
            time_due(now_ms, link->data_dedupe[i].expires_at_ms)) {
            memset(&link->data_dedupe[i], 0, sizeof(link->data_dedupe[i]));
        }
    }
}

static tavrn_data_dedupe_entry_t *oldest_unpinned_data_dedupe(
    tavrn_link_v2_t *link)
{
    tavrn_data_dedupe_entry_t *oldest = NULL;
    uint8_t i;

    for (i = 0u; i < TAVRN_LINK_DATA_DEDUPE_CAPACITY; i++) {
        tavrn_data_dedupe_entry_t *entry = &link->data_dedupe[i];

        if (entry->valid == 0u || entry->custody_pinned != 0u) {
            continue;
        }
        if (oldest == NULL || admission_before(entry->expires_at_ms,
                                               oldest->expires_at_ms)) {
            oldest = entry;
        }
    }
    return oldest;
}

static tavrn_data_dedupe_entry_t *find_data_dedupe(tavrn_link_v2_t *link,
                                                    const tavrn_link_data_t *data,
                                                    uint32_t now_ms)
{
    uint8_t i;

    prune_data_dedupe(link, now_ms);
    for (i = 0u; i < TAVRN_LINK_DATA_DEDUPE_CAPACITY; i++) {
        if (dedupe_key_equal(&link->data_dedupe[i], data)) {
            return &link->data_dedupe[i];
        }
    }
    return NULL;
}

static tavrn_data_dedupe_entry_t *reserve_data_dedupe(tavrn_link_v2_t *link,
                                                       const tavrn_link_data_t *data,
                                                       uint32_t now_ms)
{
    tavrn_data_dedupe_entry_t *entry;
    uint8_t i;

    entry = find_data_dedupe(link, data, now_ms);
    if (entry != NULL) {
        return entry;
    }
    for (i = 0u; i < TAVRN_LINK_DATA_DEDUPE_CAPACITY; i++) {
        if (link->data_dedupe[i].valid == 0u) {
            return &link->data_dedupe[i];
        }
    }
    return oldest_unpinned_data_dedupe(link);
}

static int data_dedupe_has_capacity(tavrn_link_v2_t *link, uint32_t now_ms)
{
    uint8_t i;

    prune_data_dedupe(link, now_ms);
    for (i = 0u; i < TAVRN_LINK_DATA_DEDUPE_CAPACITY; i++) {
        if (link->data_dedupe[i].valid == 0u) {
            return 1;
        }
    }
    return oldest_unpinned_data_dedupe(link) != NULL;
}

static int commit_data_dedupe(tavrn_link_v2_t *link,
                              const tavrn_link_data_t *data,
                              uint32_t now_ms)
{
    tavrn_data_dedupe_entry_t *entry = reserve_data_dedupe(link, data, now_ms);

    if (entry == NULL) {
        return 0;
    }
    memset(entry, 0, sizeof(*entry));
    entry->valid = 1u;
    entry->custody_pinned = logical_id_equal(&data->final_destination,
                                             &link->config.local_peer.logical_id) ?
        0u : 1u;
    entry->origin = data->origin;
    entry->final_destination = data->final_destination;
    entry->data_seq = data->data_seq;
    entry->app_kind = data->app_kind;
    entry->app_source = data->app_source;
    entry->expires_at_ms = now_ms + link->config.data_dedupe_ms;
    return 1;
}

static int flood_key_equal(const tavrn_flood_dedupe_entry_t *entry,
                           const tavrn_codec_flood_t *flood)
{
    return entry->valid != 0u && entry->frame_type == TAVRN_WIRE_FLOOD &&
           logical_id_equal(&entry->origin, &flood->origin) &&
           entry->sequence == flood->flood_seq;
}

static void prune_flood_dedupe(tavrn_link_v2_t *link, uint32_t now_ms)
{
    uint8_t i;

    for (i = 0u; i < TAVRN_LINK_FLOOD_DEDUPE_CAPACITY; i++) {
        if (link->flood_dedupe[i].valid != 0u &&
            time_due(now_ms, link->flood_dedupe[i].expires_at_ms)) {
            memset(&link->flood_dedupe[i], 0, sizeof(link->flood_dedupe[i]));
        }
    }
}

static tavrn_flood_dedupe_entry_t *oldest_flood_dedupe(tavrn_link_v2_t *link)
{
    tavrn_flood_dedupe_entry_t *oldest = NULL;
    uint8_t i;

    for (i = 0u; i < TAVRN_LINK_FLOOD_DEDUPE_CAPACITY; i++) {
        tavrn_flood_dedupe_entry_t *entry = &link->flood_dedupe[i];

        if (entry->valid == 0u) {
            continue;
        }
        if (oldest == NULL || admission_before(entry->expires_at_ms,
                                               oldest->expires_at_ms)) {
            oldest = entry;
        }
    }
    return oldest;
}

static tavrn_flood_dedupe_entry_t *find_or_reserve_flood_dedupe(
    tavrn_link_v2_t *link, const tavrn_codec_flood_t *flood, uint32_t now_ms,
    int *was_duplicate)
{
    uint8_t i;

    *was_duplicate = 0;
    prune_flood_dedupe(link, now_ms);
    for (i = 0u; i < TAVRN_LINK_FLOOD_DEDUPE_CAPACITY; i++) {
        if (flood_key_equal(&link->flood_dedupe[i], flood)) {
            *was_duplicate = 1;
            return &link->flood_dedupe[i];
        }
    }
    for (i = 0u; i < TAVRN_LINK_FLOOD_DEDUPE_CAPACITY; i++) {
        if (link->flood_dedupe[i].valid == 0u) {
            return &link->flood_dedupe[i];
        }
    }
    return oldest_flood_dedupe(link);
}

static uint32_t flood_jitter(const tavrn_link_v2_t *link,
                             const tavrn_codec_flood_t *flood)
{
    uint32_t key = ((uint32_t)flood->origin.value << 16) | flood->flood_seq;
    uint32_t span = link->config.flood_jitter_max_ms -
                    link->config.flood_jitter_min_ms + 1u;

    key ^= (uint32_t)flood->origin.width << 27;
    key ^= (uint32_t)link->config.network_id << 19;
    key ^= key >> 16;
    key *= 0x45d9f3bu;
    key ^= key >> 16;
    return link->config.flood_jitter_min_ms + key % span;
}

static void count_hack(tavrn_link_v2_t *link, tavrn_hack_status_t status)
{
    if (status == TAVRN_HACK_ACCEPTED) {
        link->counters.hack_accepted++;
    } else if (status == TAVRN_HACK_DUPLICATE) {
        link->counters.hack_duplicate++;
    } else if (status == TAVRN_HACK_BUSY) {
        link->counters.hack_busy++;
    } else if (status == TAVRN_HACK_REJECTED) {
        link->counters.hack_rejected++;
    }
}

static int terminal_evicted_token(tavrn_link_v2_t *link,
                                  ble_mesh_tx_token_t token,
                                  tavrn_link_event_t *output)
{
    uint8_t i;

    if (token == BLE_MESH_TX_TOKEN_NONE) {
        return 0;
    }
    for (i = 0u; i < TAVRN_LINK_CUSTODY_CAPACITY; i++) {
        if (link->custody[i].phase != TAVRN_CUSTODY_FREE &&
            link->custody[i].scheduler_token == token) {
            terminal_owned(link, i, output, TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED,
                           TAVRN_LOCAL_TX_SCHEDULER_EVICTED,
                           TAVRN_MESH_FAULT_NONE);
            return 1;
        }
    }
    return 0;
}

static int pending_hack_exists(const ble_mesh_tx_queue_t *queue,
                               const ble_mesh_tx_item_t *candidate)
{
    uint8_t index;

    if (queue == NULL || candidate == NULL || candidate->adv_len == 0u) {
        return 0;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry = &queue->entries[index];

        /* Canonical HACK encoding places status last. Matching the preceding
         * bytes preserves the first pending reply's status and release time. */
        if (entry->occupied != 0u &&
            entry->item.adv_len == candidate->adv_len &&
            memcmp(entry->item.adv_data, candidate->adv_data,
                   (size_t)candidate->adv_len - 1u) == 0) {
            return 1;
        }
    }
    return 0;
}

static uint32_t repeated_response_due_ms(const tavrn_link_v2_t *link,
                                         uint32_t first_rx_ms)
{
    return first_rx_ms + tron_timer_config.radio_tx_repeated_event_bound_ms +
        link->config.hack_turnaround_ms;
}

static int enqueue_hack(tavrn_link_v2_t *link,
                        const tavrn_direct_peer_t *transmitter,
                        const tavrn_link_data_t *data,
                        tavrn_hack_status_t status, uint32_t first_rx_ms,
                        tavrn_link_event_t *output)
{
    tavrn_codec_config_t config;
    tavrn_decoded_frame_t frame;
    ble_mesh_tx_item_t item;
    ble_mesh_sched_enqueue_result_t result;
    size_t adv_len = 0u;

    count_hack(link, status);
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_HACK;
    frame.network_id = link->config.network_id;
    frame.detail.hack.immediate_receiver = transmitter->logical_id;
    frame.detail.hack.data_origin = data->origin;
    frame.detail.hack.final_destination = data->final_destination;
    frame.detail.hack.data_seq = data->data_seq;
    frame.detail.hack.app_kind = data->app_kind;
    frame.detail.hack.app_source = data->app_source;
    frame.detail.hack.status = status;
    config = codec_config(link);
    memset(&item, 0, sizeof(item));
    if (tavrn_wire_v2_encode(&config, &frame, item.adv_data,
                             sizeof(item.adv_data), &adv_len) != TAVRN_CODEC_OK ||
        adv_len > sizeof(item.adv_data)) {
        link->counters.hack_enqueue_failed++;
        return 0;
    }
    item.adv_len = (uint8_t)adv_len;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = BLE_MESH_TX_PRIORITY_HACK;
    item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
#if defined(TAVRN_LINK_V2_HOST_TEST_IMMEDIATE_HACK)
    /* Host-only turnaround RED mode: restore the superseded immediate due time. */
    item.not_before_ms = first_rx_ms;
#else
    item.not_before_ms = repeated_response_due_ms(link, first_rx_ms);
#endif
    item.expiry_ms = first_rx_ms + link->config.hack_response_ms;
    item.sweep_count = BLE_MESH_TX_SWEEP_COUNT_ONE;
    item.budget_class = BLE_MESH_TX_BUDGET_CRITICAL;
    item.token = BLE_MESH_TX_TOKEN_NONE;
    if (pending_hack_exists(&link->scheduler->routed_tx_queue, &item)) {
        return 0;
    }
    result = ble_mesh_scheduler_enqueue_ex(link->scheduler, &item);
    if (result.status != BLE_MESH_SCHED_ENQUEUE_OK) {
        link->counters.hack_enqueue_failed++;
        return 0;
    }
    return terminal_evicted_token(link, result.evicted_token, output);
}

static tavrn_link_step_status_t handle_flood(tavrn_link_v2_t *link,
                                             const tavrn_codec_flood_t *flood,
                                             uint32_t now_ms)
{
    tavrn_flood_dedupe_entry_t *entry;
    tavrn_codec_config_t config;
    tavrn_decoded_frame_t frame;
    ble_mesh_tx_item_t item;
    size_t adv_len = 0u;
    int duplicate;

    entry = find_or_reserve_flood_dedupe(link, flood, now_ms, &duplicate);
    if (duplicate) {
        link->counters.rx_flood_duplicate++;
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    if (entry == NULL) {
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    memset(entry, 0, sizeof(*entry));
    entry->valid = 1u;
    entry->frame_type = TAVRN_WIRE_FLOOD;
    entry->origin = flood->origin;
    entry->sequence = flood->flood_seq;
    entry->expires_at_ms = now_ms + link->config.flood_dedupe_ms;
    link->counters.rx_flood_committed++;
    if (flood->ttl == 0u || flood->hops == 15u || link->mesh_fault_latched != 0u) {
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_FLOOD;
    frame.network_id = link->config.network_id;
    frame.detail.flood = *flood;
    frame.detail.flood.ttl--;
    frame.detail.flood.hops++;
    config = codec_config(link);
    memset(&item, 0, sizeof(item));
    if (tavrn_wire_v2_encode(&config, &frame, item.adv_data,
                             sizeof(item.adv_data), &adv_len) != TAVRN_CODEC_OK ||
        adv_len > sizeof(item.adv_data)) {
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    item.adv_len = (uint8_t)adv_len;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = BLE_MESH_TX_PRIORITY_RELAY;
    item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
    item.not_before_ms = now_ms + flood_jitter(link, flood);
    item.expiry_ms = now_ms + link->config.flood_dedupe_ms;
    item.sweep_count = BLE_MESH_TX_SWEEP_COUNT_ONE;
    item.budget_class = BLE_MESH_TX_BUDGET_GENERAL;
    item.token = BLE_MESH_TX_TOKEN_NONE;
    (void)ble_mesh_scheduler_enqueue_ex(link->scheduler, &item);
    return TAVRN_LINK_STEP_NO_EVENT;
}

static void latch_fault(tavrn_link_v2_t *link,
                         const ble_mesh_sched_event_t *input)
{
    uint8_t i;

    if (link->mesh_fault_latched != 0u) {
        return;
    }
    link->mesh_fault_latched = 1u;
    if (input->type == BLE_MESH_SCHED_EVENT_RADIO_FAULT) {
        link->latched_mesh_fault_reason = input->tx_completed_channel_mask != 0u ?
            TAVRN_MESH_FAULT_RADIO_AFTER_TX : TAVRN_MESH_FAULT_RADIO_UNAVAILABLE;
    } else if (input->fault == BLE_MESH_SCHED_FAULT_POLL_OVERRUN) {
        link->latched_mesh_fault_reason = TAVRN_MESH_FAULT_POLL_OVERRUN;
    } else {
        link->latched_mesh_fault_reason = TAVRN_MESH_FAULT_INTERNAL_STATE;
    }
    memset(&link->candidate, 0, sizeof(link->candidate));
    link->candidate_valid = 0u;
    link->active_custody_index = TAVRN_CUSTODY_SLOT_NONE;
    for (i = 0u; i < TAVRN_LINK_CUSTODY_CAPACITY; i++) {
        if (link->custody[i].phase != TAVRN_CUSTODY_FREE) {
            link->custody[i].phase = TAVRN_CUSTODY_FAULT_PENDING;
        }
    }
}

static int matching_hack(const tavrn_link_v2_t *link,
                         const tavrn_decoded_frame_t *frame)
{
    const tavrn_custody_slot_t *slot;

    if (link->active_custody_index >= TAVRN_LINK_CUSTODY_CAPACITY ||
        frame->type != TAVRN_WIRE_HACK) {
        return 0;
    }
    slot = &link->custody[link->active_custody_index];
    return slot->phase == TAVRN_CUSTODY_ACTIVE_WAIT_HACK &&
           memcmp(frame->transmitter.adva.bytes, slot->next_hop.adva.bytes,
                  TAVRN_ADVA_LEN) == 0 &&
           logical_id_equal(&frame->detail.hack.immediate_receiver,
                            &link->config.local_peer.logical_id) &&
           logical_id_equal(&frame->detail.hack.data_origin, &slot->data.origin) &&
           logical_id_equal(&frame->detail.hack.final_destination,
                            &slot->data.final_destination) &&
           frame->detail.hack.data_seq == slot->data.data_seq &&
           frame->detail.hack.app_kind == slot->data.app_kind &&
           frame->detail.hack.app_source == slot->data.app_source;
}

static tavrn_link_step_status_t handle_hack(tavrn_link_v2_t *link,
                                            const tavrn_decoded_frame_t *frame,
                                            uint32_t now_ms,
                                            tavrn_link_event_t *output)
{
    tavrn_custody_slot_t *slot;
    uint8_t index;

    if (!matching_hack(link, frame)) {
        link->counters.hack_unmatched++;
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    index = link->active_custody_index;
    slot = &link->custody[index];
    count_hack(link, frame->detail.hack.status);
    if (frame->detail.hack.status == TAVRN_HACK_ACCEPTED ||
        frame->detail.hack.status == TAVRN_HACK_DUPLICATE) {
        transferred(link, index, frame->detail.hack.status, output);
        return TAVRN_LINK_STEP_EVENT;
    }
    if (frame->detail.hack.status == TAVRN_HACK_REJECTED) {
        terminal_owned(link, index, output, TAVRN_LINK_EVENT_CUSTODY_REJECTED,
                       TAVRN_LOCAL_TX_ENCODE_FAILED, TAVRN_MESH_FAULT_NONE);
        return TAVRN_LINK_STEP_EVENT;
    }
    slot->busy_response_count++;
    link->active_custody_index = TAVRN_CUSTODY_SLOT_NONE;
    if (slot->busy_response_count >= link->config.busy_max_responses) {
        terminal_owned(link, index, output, TAVRN_LINK_EVENT_CUSTODY_BUSY_EXPIRED,
                       TAVRN_LOCAL_TX_ENCODE_FAILED, TAVRN_MESH_FAULT_NONE);
        return TAVRN_LINK_STEP_EVENT;
    }
    slot->phase = TAVRN_CUSTODY_BUSY_WAIT;
    slot->scheduler_token = BLE_MESH_TX_TOKEN_NONE;
    slot->response_deadline_ms = 0u;
    slot->retry_not_before_ms = now_ms + link->config.busy_backoff_ms;
    slot->attempt_count = 0u;
    slot->attempt_requested_channel_mask = 0u;
    slot->attempt_completed_channel_mask = 0u;
    slot->first_tx_ms = 0u;
    slot->last_tx_ms = 0u;
    return TAVRN_LINK_STEP_NO_EVENT;
}

tavrn_link_init_status_t tavrn_link_v2_init(
    tavrn_link_v2_t *link, ble_mesh_scheduler_t *sched,
    const tavrn_link_config_t *config, uint32_t now_ms)
{
    uint8_t scheduler_adva[TAVRN_ADVA_LEN];

    (void)now_ms;
    if (link == NULL || sched == NULL || config == NULL) {
        return TAVRN_LINK_INIT_INVALID_ARGUMENT;
    }
    memset(link, 0, sizeof(*link));
    link->active_custody_index = TAVRN_CUSTODY_SLOT_NONE;
    link->latched_mesh_fault_reason = TAVRN_MESH_FAULT_NONE;
    if (!direct_peer_valid(&config->local_peer) ||
        (config->local_peer.logical_id.width != TAVRN_IDENTITY_SID16 &&
         config->local_peer.logical_id.width != TAVRN_IDENTITY_SID8) ||
        (config->local_peer.logical_id.width == TAVRN_IDENTITY_SID8 &&
         config->identity_conflict == NULL) ||
        !ble_mesh_scheduler_copy_local_adva(sched, scheduler_adva) ||
        memcmp(scheduler_adva, config->local_peer.adva.bytes,
               TAVRN_ADVA_LEN) != 0) {
        return TAVRN_LINK_INIT_INVALID_LOCAL_IDENTITY;
    }
    if (config->network_id == 0u || config->network_id == 0xffu ||
        config->hack_max_attempts != TAVRN_LINK_PROFILE_MAX_ATTEMPTS ||
        config->busy_max_responses != TAVRN_LINK_PROFILE_BUSY_RESPONSES ||
        config->hack_response_ms == 0u || config->hack_turnaround_ms == 0u ||
        config->data_forward_deadline_ms == 0u ||
        config->candidate_resolve_ms == 0u || config->data_dedupe_ms == 0u ||
        config->flood_dedupe_ms == 0u ||
        !time_value_valid(config->hack_response_ms) ||
        !time_value_valid(config->hack_turnaround_ms) ||
        !time_value_valid(config->retry_backoff_ms) ||
        !time_value_valid(config->busy_backoff_ms) ||
        !time_value_valid(config->data_forward_deadline_ms) ||
        !time_value_valid(config->candidate_resolve_ms) ||
        !time_value_valid(config->data_dedupe_ms) ||
        !time_value_valid(config->flood_dedupe_ms) ||
        !time_value_valid(config->flood_jitter_min_ms) ||
        !time_value_valid(config->flood_jitter_max_ms) ||
        config->flood_jitter_min_ms > config->flood_jitter_max_ms ||
        !tron_timer_config_is_valid(&tron_timer_config) ||
        config->hack_turnaround_ms > UINT32_C(0x7fffffff) -
            tron_timer_config.radio_tx_repeated_event_bound_ms) {
        return TAVRN_LINK_INIT_INVALID_CONFIG;
    }
    link->scheduler = sched;
    link->config = *config;
    return TAVRN_LINK_INIT_OK;
}

tavrn_link_send_status_t tavrn_link_v2_send_unicast(
    tavrn_link_v2_t *link, const tavrn_direct_peer_t *next_hop,
    const tavrn_link_data_t *data, uint32_t now_ms,
    tavrn_link_event_t *local_outcome)
{
    tavrn_codec_config_t config;
    tavrn_decoded_frame_t frame;
    tavrn_custody_slot_t *slot;
    uint8_t index;
    uint8_t encoded[BLE_ADV_MAX_DATA];
    size_t adv_len = 0u;

    if (local_outcome == NULL) {
        return TAVRN_LINK_SEND_INVALID;
    }
    clear_event(local_outcome);
    if (link == NULL || next_hop == NULL || data == NULL ||
        !direct_peer_valid(next_hop) || !data_valid(data) ||
        next_hop->logical_id.width != link->config.local_peer.logical_id.width ||
        data->origin.width != link->config.local_peer.logical_id.width) {
        return TAVRN_LINK_SEND_INVALID;
    }
    if (link->mesh_fault_latched != 0u) {
        return TAVRN_LINK_SEND_MESH_FAULTED;
    }
    if (logical_id_equal(&next_hop->logical_id, &link->config.local_peer.logical_id)) {
        if (memcmp(next_hop->adva.bytes, link->config.local_peer.adva.bytes,
                   TAVRN_ADVA_LEN) != 0) {
            link->counters.rx_identity_conflict++;
            return TAVRN_LINK_SEND_IDENTITY_CONFLICT;
        }
        return TAVRN_LINK_SEND_INVALID;
    }
    for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
        if (link->custody[index].phase == TAVRN_CUSTODY_FREE) {
            break;
        }
    }
    if (index == TAVRN_LINK_CUSTODY_CAPACITY) {
        return TAVRN_LINK_SEND_NO_SLOT;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_DATA;
    frame.network_id = link->config.network_id;
    frame.detail.data.immediate_receiver = next_hop->logical_id;
    frame.detail.data.data = *data;
    config = codec_config(link);
    if (tavrn_wire_v2_encode(&config, &frame, encoded, sizeof(encoded), &adv_len) !=
            TAVRN_CODEC_OK || adv_len > sizeof(link->custody[index].adv_data)) {
        tavrn_custody_slot_t failed;

        memset(&failed, 0, sizeof(failed));
        failed.next_hop = *next_hop;
        failed.data = *data;
        failed.transaction_deadline_ms = now_ms + link->config.data_forward_deadline_ms;
        fill_owned_event(local_outcome, TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED,
                         &failed, TAVRN_LOCAL_TX_ENCODE_FAILED,
                         TAVRN_MESH_FAULT_NONE);
        link->counters.local_tx_not_attempted++;
        return TAVRN_LINK_SEND_LOCAL_NOT_ATTEMPTED;
    }
    slot = &link->custody[index];
    memset(slot, 0, sizeof(*slot));
    slot->phase = TAVRN_CUSTODY_READY_NOT_ELIGIBLE;
    slot->next_hop = *next_hop;
    slot->data = *data;
    slot->adv_len = (uint8_t)adv_len;
    memcpy(slot->adv_data, encoded, adv_len);
    slot->transaction_deadline_ms = now_ms + link->config.data_forward_deadline_ms;
    link->next_custody_order++;
    if (link->next_custody_order == 0u) {
        link->next_custody_order++;
    }
    slot->admission_order = link->next_custody_order;
    return TAVRN_LINK_SEND_OK;
}

tavrn_link_send_status_t tavrn_link_v2_send_flood(
    tavrn_link_v2_t *link, const tavrn_codec_flood_t *flood,
    uint32_t now_ms, tavrn_link_event_t *local_outcome)
{
    tavrn_codec_config_t config;
    tavrn_decoded_frame_t frame;
    ble_mesh_tx_item_t item;
    ble_mesh_sched_enqueue_result_t result;
    size_t adv_len = 0u;

    if (local_outcome == NULL) {
        return TAVRN_LINK_SEND_INVALID;
    }
    clear_event(local_outcome);
    if (link == NULL || !flood_valid(flood) ||
        !logical_id_equal(&flood->origin, &link->config.local_peer.logical_id)) {
        return TAVRN_LINK_SEND_INVALID;
    }
    if (link->mesh_fault_latched != 0u) {
        return TAVRN_LINK_SEND_MESH_FAULTED;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_FLOOD;
    frame.network_id = link->config.network_id;
    frame.detail.flood = *flood;
    config = codec_config(link);
    memset(&item, 0, sizeof(item));
    if (tavrn_wire_v2_encode(&config, &frame, item.adv_data,
                             sizeof(item.adv_data), &adv_len) != TAVRN_CODEC_OK ||
        adv_len > sizeof(item.adv_data)) {
        return TAVRN_LINK_SEND_INVALID;
    }
    item.adv_len = (uint8_t)adv_len;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = BLE_MESH_TX_PRIORITY_RELAY;
    item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
    item.not_before_ms = now_ms;
    item.expiry_ms = now_ms + link->config.flood_dedupe_ms;
    item.sweep_count = BLE_MESH_TX_SWEEP_COUNT_ONE;
    item.budget_class = BLE_MESH_TX_BUDGET_GENERAL;
    item.token = BLE_MESH_TX_TOKEN_NONE;
    result = ble_mesh_scheduler_enqueue_ex(link->scheduler, &item);
    if (result.status != BLE_MESH_SCHED_ENQUEUE_OK) {
        return TAVRN_LINK_SEND_BUSY;
    }
    if (terminal_evicted_token(link, result.evicted_token, local_outcome)) {
        return TAVRN_LINK_SEND_LOCAL_NOT_ATTEMPTED;
    }
    return TAVRN_LINK_SEND_OK;
}

tavrn_link_send_status_t tavrn_link_v2_send_control(
    tavrn_link_v2_t *link, const tavrn_validated_control_t *control,
    const tavrn_direct_peer_t *next_hop_or_null, uint8_t controlled_flood,
    uint32_t now_ms, tavrn_link_event_t *local_outcome)
{
    return tavrn_link_v2_send_control_at(link, control, next_hop_or_null,
                                          controlled_flood, now_ms, now_ms,
                                          local_outcome);
}

tavrn_link_send_status_t tavrn_link_v2_send_control_at(
    tavrn_link_v2_t *link, const tavrn_validated_control_t *control,
    const tavrn_direct_peer_t *next_hop_or_null, uint8_t controlled_flood,
    uint32_t now_ms, uint32_t not_before_ms,
    tavrn_link_event_t *local_outcome)
{
    return tavrn_link_v2_send_control_tracked_at(
        link, control, next_hop_or_null, controlled_flood, now_ms,
        not_before_ms, local_outcome, NULL);
}

tavrn_link_send_status_t tavrn_link_v2_send_control_tracked(
    tavrn_link_v2_t *link, const tavrn_validated_control_t *control,
    const tavrn_direct_peer_t *next_hop_or_null, uint8_t controlled_flood,
    uint32_t now_ms, tavrn_link_event_t *local_outcome,
    ble_mesh_tx_token_t *scheduler_token_out)
{
    return tavrn_link_v2_send_control_tracked_at(
        link, control, next_hop_or_null, controlled_flood, now_ms, now_ms,
        local_outcome, scheduler_token_out);
}

tavrn_link_send_status_t tavrn_link_v2_send_control_tracked_at(
    tavrn_link_v2_t *link, const tavrn_validated_control_t *control,
    const tavrn_direct_peer_t *next_hop_or_null, uint8_t controlled_flood,
    uint32_t now_ms, uint32_t not_before_ms,
    tavrn_link_event_t *local_outcome,
    ble_mesh_tx_token_t *scheduler_token_out)
{
    tavrn_codec_config_t config;
    tavrn_decoded_frame_t frame;
    ble_mesh_tx_item_t item;
    ble_mesh_sched_enqueue_result_t result;
    size_t adv_len = 0u;

    if (scheduler_token_out != NULL) {
        *scheduler_token_out = BLE_MESH_TX_TOKEN_NONE;
    }
    if (local_outcome == NULL) {
        return TAVRN_LINK_SEND_INVALID;
    }
    clear_event(local_outcome);
    if (link == NULL || !control_send_shape_valid(link, control,
                                                   next_hop_or_null,
                                                   controlled_flood)) {
        return TAVRN_LINK_SEND_INVALID;
    }
    if (link->mesh_fault_latched != 0u) {
        return TAVRN_LINK_SEND_MESH_FAULTED;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = control->type;
    frame.network_id = link->config.network_id;
    frame.detail.control = *control;
    config = codec_config(link);
    memset(&item, 0, sizeof(item));
    if (tavrn_wire_v2_encode(&config, &frame, item.adv_data,
                             sizeof(item.adv_data), &adv_len) != TAVRN_CODEC_OK ||
        adv_len > sizeof(item.adv_data)) {
        return TAVRN_LINK_SEND_INVALID;
    }
    item.adv_len = (uint8_t)adv_len;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = BLE_MESH_TX_PRIORITY_CONTROL;
    item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
    item.not_before_ms = not_before_ms;
    item.token = scheduler_token_out == NULL ? BLE_MESH_TX_TOKEN_NONE :
        allocate_scheduler_token(link);
    if (scheduler_token_out != NULL && item.token == BLE_MESH_TX_TOKEN_NONE) {
        return TAVRN_LINK_SEND_BUSY;
    }
    /* TODO(BEARER-06): replace this bounded generic-control deadline with each
     * concrete producer's owner deadline when its typed terminal is wired. */
    item.expiry_ms = now_ms + link->config.data_forward_deadline_ms;
    apply_control_bearer_policy(control, &item);
    result = ble_mesh_scheduler_enqueue_ex(link->scheduler, &item);
    if (result.status != BLE_MESH_SCHED_ENQUEUE_OK) {
        return TAVRN_LINK_SEND_BUSY;
    }
    /* The control is admitted even if it synchronously evicts DATA. Preserve
     * that separate DATA outcome; the router starts a tracked RREP wait only
     * after this token later reports physical TX_DONE. */
    (void)terminal_evicted_token(link, result.evicted_token, local_outcome);
    if (scheduler_token_out != NULL) {
        *scheduler_token_out = result.accepted_token;
    }
    return TAVRN_LINK_SEND_OK;
}

tavrn_link_send_status_t tavrn_link_v2_send_tracked_control(
    tavrn_link_v2_t *link, const tavrn_validated_control_t *control,
    const tavrn_direct_peer_t *next_hop, ble_mesh_tx_token_t token,
    uint32_t now_ms, ble_mesh_tx_token_t *evicted_token_out)
{
    tavrn_codec_config_t config;
    tavrn_decoded_frame_t frame;
    ble_mesh_tx_item_t item;
    ble_mesh_sched_enqueue_result_t result;
    size_t adv_len = 0u;

    (void)now_ms;
    if (evicted_token_out != NULL) {
        *evicted_token_out = BLE_MESH_TX_TOKEN_NONE;
    }
    if (!targeted_control_send_shape_valid(link, control, next_hop, token)) {
        return TAVRN_LINK_SEND_INVALID;
    }
    if (link->mesh_fault_latched != 0u) {
        return TAVRN_LINK_SEND_MESH_FAULTED;
    }
    if (token_in_use(link, token)) {
        return TAVRN_LINK_SEND_BUSY;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = control->type;
    frame.network_id = link->config.network_id;
    frame.detail.control = *control;
    config = codec_config(link);
    memset(&item, 0, sizeof(item));
    if (tavrn_wire_v2_encode(&config, &frame, item.adv_data,
                             sizeof(item.adv_data), &adv_len) != TAVRN_CODEC_OK ||
        adv_len > sizeof(item.adv_data)) {
        return TAVRN_LINK_SEND_INVALID;
    }
    item.adv_len = (uint8_t)adv_len;
    item.channel_mask = BLE_RADIO_ADV_CH_ALL;
    item.priority = BLE_MESH_TX_PRIORITY_CONTROL;
    item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
    item.not_before_ms = now_ms;
    item.token = token;
    /* TODO(BEARER-06): router-owned controls need their concrete producer
     * deadline before their transitional ROUTER/HIGH ownership can retire. */
    item.expiry_ms = now_ms + link->config.data_forward_deadline_ms;
    apply_control_bearer_policy(control, &item);
    result = ble_mesh_scheduler_enqueue_ex(link->scheduler, &item);
    if (result.status != BLE_MESH_SCHED_ENQUEUE_OK) {
        return TAVRN_LINK_SEND_BUSY;
    }
    if (evicted_token_out != NULL) {
        *evicted_token_out = result.evicted_token;
    }
    return TAVRN_LINK_SEND_OK;
}

tavrn_link_resolve_status_t tavrn_link_v2_cancel_queued_control(
    tavrn_link_v2_t *link, const tavrn_validated_control_t *control)
{
    tavrn_decoded_frame_t frame;
    tavrn_codec_config_t config;
    uint8_t encoded[BLE_ADV_MAX_DATA];
    size_t encoded_len = 0u;
    uint8_t index;

    if (link == NULL || link->scheduler == NULL || control == NULL) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = control->type;
    frame.network_id = link->config.network_id;
    frame.detail.control = *control;
    config = codec_config(link);
    if (tavrn_wire_v2_encode(&config, &frame, encoded, sizeof(encoded),
                             &encoded_len) != TAVRN_CODEC_OK ||
        encoded_len > BLE_ADV_MAX_DATA) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_queue_entry_t *entry =
            &link->scheduler->routed_tx_queue.entries[index];

        if (entry->occupied == 0u || entry->item.adv_len != encoded_len ||
            memcmp(entry->item.adv_data, encoded, encoded_len) != 0) {
            continue;
        }
        if (!retire_queued_entry(&link->scheduler->routed_tx_queue, index,
                                  BLE_MESH_TX_TERMINAL_CANCELED)) {
            return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
        }
    }
    return TAVRN_LINK_RESOLVE_OK;
}

tavrn_link_resolve_status_t tavrn_link_v2_cancel_queued_tracked_control(
    tavrn_link_v2_t *link, const tavrn_validated_control_t *control,
    ble_mesh_tx_token_t token, uint8_t *canceled_out)
{
    tavrn_decoded_frame_t frame;
    tavrn_codec_config_t config;
    uint8_t encoded[BLE_ADV_MAX_DATA];
    size_t encoded_len = 0u;
    uint8_t index;

    if (canceled_out != NULL) {
        *canceled_out = 0u;
    }
    if (link == NULL || link->scheduler == NULL || canceled_out == NULL ||
        control == NULL || token < 0x8000u ||
        !((control->type == TAVRN_WIRE_HELLO && control->pdu_len == 20u &&
           (control->pdu[5] == 0xb8u || control->pdu[5] == 0xa8u)) ||
          (control->type == TAVRN_WIRE_E_RREQ &&
           control_send_shape_valid(link, control, NULL, 1u)))) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    memset(&frame, 0, sizeof(frame));
    frame.type = control->type;
    frame.network_id = link->config.network_id;
    frame.detail.control = *control;
    config = codec_config(link);
    if (tavrn_wire_v2_encode(&config, &frame, encoded, sizeof(encoded),
                             &encoded_len) != TAVRN_CODEC_OK) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_queue_entry_t *entry =
            &link->scheduler->routed_tx_queue.entries[index];

        if (entry->occupied == 0u || entry->item.token != token ||
            entry->item.adv_len != encoded_len ||
            memcmp(entry->item.adv_data, encoded, encoded_len) != 0) {
            continue;
        }
        if (!retire_queued_entry(&link->scheduler->routed_tx_queue, index,
                                  BLE_MESH_TX_TERMINAL_CANCELED)) {
            return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
        }
        *canceled_out = 1u;
        return TAVRN_LINK_RESOLVE_OK;
    }
    return TAVRN_LINK_RESOLVE_OK;
}

tavrn_link_resolve_status_t tavrn_link_v2_cancel_queued_tracked_token(
    tavrn_link_v2_t *link, ble_mesh_tx_token_t token, uint8_t *canceled_out)
{
    uint8_t index;
    uint8_t matched_index = BLE_MESH_TX_QUEUE_CAPACITY;

    if (canceled_out != NULL) {
        *canceled_out = 0u;
    }
    if (link == NULL || link->scheduler == NULL || canceled_out == NULL ||
        token == BLE_MESH_TX_TOKEN_NONE) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
        if (link->custody[index].phase != TAVRN_CUSTODY_FREE &&
            link->custody[index].scheduler_token == token) {
            return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
        }
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry =
            &link->scheduler->routed_tx_queue.entries[index];

        if (entry->occupied == 0u || entry->item.token != token) {
            continue;
        }
        if (matched_index != BLE_MESH_TX_QUEUE_CAPACITY) {
            return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
        }
        matched_index = index;
    }
    if (matched_index == BLE_MESH_TX_QUEUE_CAPACITY) {
        return TAVRN_LINK_RESOLVE_OK;
    }
    if (!retire_queued_entry(&link->scheduler->routed_tx_queue, matched_index,
                              BLE_MESH_TX_TERMINAL_CANCELED)) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    *canceled_out = 1u;
    return TAVRN_LINK_RESOLVE_OK;
}

tavrn_link_resolve_status_t tavrn_link_v2_resolve_rx(
    tavrn_link_v2_t *link, tavrn_rx_candidate_token_t token,
    tavrn_rx_decision_t decision, uint32_t now_ms,
    tavrn_link_event_t *local_outcome)
{
    tavrn_rx_data_candidate_t candidate;

    if (local_outcome == NULL || link == NULL) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    clear_event(local_outcome);
    if (link->mesh_fault_latched != 0u) {
        return TAVRN_LINK_RESOLVE_MESH_FAULTED;
    }
    if (decision != TAVRN_RX_ACCEPTED && decision != TAVRN_RX_BUSY &&
        decision != TAVRN_RX_REJECTED) {
        return TAVRN_LINK_RESOLVE_INVALID_DECISION;
    }
    if (token == TAVRN_RX_CANDIDATE_TOKEN_NONE ||
        token != link->candidate.token) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    if (link->candidate_valid == 0u) {
        return TAVRN_LINK_RESOLVE_ALREADY_RESOLVED;
    }
    candidate = link->candidate;
    memset(&link->candidate, 0, sizeof(link->candidate));
    link->candidate.token = token;
    link->candidate_valid = 0u;
    if (decision == TAVRN_RX_ACCEPTED) {
        commit_data_dedupe(link, &candidate.data, now_ms);
        link->counters.rx_candidate_accepted++;
        (void)enqueue_hack(link, &candidate.transmitter, &candidate.data,
                           TAVRN_HACK_ACCEPTED,
                           candidate.deadline_ms - link->config.candidate_resolve_ms,
                           local_outcome);
    } else if (decision == TAVRN_RX_BUSY) {
        link->counters.rx_candidate_busy++;
        (void)enqueue_hack(link, &candidate.transmitter, &candidate.data,
                           TAVRN_HACK_BUSY,
                           candidate.deadline_ms - link->config.candidate_resolve_ms,
                           local_outcome);
    } else {
        link->counters.rx_candidate_rejected++;
        (void)enqueue_hack(link, &candidate.transmitter, &candidate.data,
                           TAVRN_HACK_REJECTED,
                           candidate.deadline_ms - link->config.candidate_resolve_ms,
                           local_outcome);
    }
    return TAVRN_LINK_RESOLVE_OK;
}

int tavrn_link_v2_rx_candidate_matches(
    const tavrn_link_v2_t *link,
    const tavrn_rx_data_candidate_t *candidate)
{
    return link != NULL && candidate != NULL && link->candidate_valid != 0u &&
           candidate_equal(&link->candidate, candidate);
}

tavrn_link_resolve_status_t tavrn_link_v2_release_rx_custody(
    tavrn_link_v2_t *link, const tavrn_link_data_t *data, uint32_t now_ms)
{
    tavrn_data_dedupe_entry_t *entry;

    if (link == NULL || !data_valid(data)) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    entry = find_data_dedupe(link, data, now_ms);
    if (entry == NULL || entry->custody_pinned == 0u ||
        !logical_id_equal(&entry->final_destination, &data->final_destination)) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    if (entry->incarnation_clear_on_release != 0u) {
        memset(entry, 0, sizeof(*entry));
    } else {
        entry->custody_pinned = 0u;
        entry->external_custody_owner = 0u;
    }
    return TAVRN_LINK_RESOLVE_OK;
}

tavrn_link_resolve_status_t tavrn_link_v2_transfer_rx_custody_to_external(
    tavrn_link_v2_t *link, const tavrn_link_data_t *data, uint32_t now_ms)
{
    tavrn_data_dedupe_entry_t *entry;
    uint8_t index;

    (void)now_ms;
    if (link == NULL || !data_valid(data) ||
        data->ownership != TAVRN_DATA_TRANSIT) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    entry = find_data_dedupe_exact(link, data);
    if (entry == NULL || entry->custody_pinned == 0u ||
        !logical_id_equal(&entry->final_destination, &data->final_destination)) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
        if (link->custody[index].phase != TAVRN_CUSTODY_FREE &&
            link_data_equal(&link->custody[index].data, data)) {
            return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
        }
    }
    /* Local repair may reforward an externally owned packet through link
     * custody and reacquire the same exact pin after another retry terminal.
     * With no live internal copy, repeating this single-owner mark is
     * idempotent rather than an ownership split. */
    entry->external_custody_owner = 1u;
    return TAVRN_LINK_RESOLVE_OK;
}

tavrn_link_resolve_status_t tavrn_link_v2_discard_internal_rx_custody(
    tavrn_link_v2_t *link, const tavrn_link_data_t *data)
{
    tavrn_data_dedupe_entry_t *entry;
    uint8_t index;

    if (link == NULL || !data_valid(data) ||
        data->ownership != TAVRN_DATA_TRANSIT) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    entry = find_data_dedupe_exact(link, data);
    if (entry == NULL || entry->custody_pinned == 0u ||
        entry->external_custody_owner != 0u ||
        !logical_id_equal(&entry->final_destination, &data->final_destination)) {
        return TAVRN_LINK_RESOLVE_OK;
    }
    for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
        if (link->custody[index].phase != TAVRN_CUSTODY_FREE &&
            link_data_equal(&link->custody[index].data, data)) {
            return TAVRN_LINK_RESOLVE_OK;
        }
    }
    entry->custody_pinned = 0u;
    return TAVRN_LINK_RESOLVE_OK;
}

tavrn_link_step_status_t tavrn_link_v2_on_scheduler_event(
    tavrn_link_v2_t *link, const ble_mesh_sched_event_t *input,
    uint32_t now_ms, tavrn_link_event_t *output)
{
    tavrn_codec_config_t config;
    tavrn_decoded_frame_t frame;
    tavrn_codec_result_t decoded;
    tavrn_custody_slot_t *slot;
    uint8_t index;

    if (link == NULL || input == NULL || output == NULL) {
        return TAVRN_LINK_STEP_INVALID;
    }
    clear_event(output);
    if (input->type == BLE_MESH_SCHED_EVENT_NONE) {
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    if (input->type == BLE_MESH_SCHED_EVENT_RADIO_FAULT ||
        input->type == BLE_MESH_SCHED_EVENT_SERVICE_FAULT) {
        latch_fault(link, input);
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    if (input->type == BLE_MESH_SCHED_EVENT_TX_DONE ||
        input->type == BLE_MESH_SCHED_EVENT_TX_FAILED ||
        input->type == BLE_MESH_SCHED_EVENT_TX_EXPIRED) {
        if (link->active_custody_index >= TAVRN_LINK_CUSTODY_CAPACITY) {
            return TAVRN_LINK_STEP_NO_EVENT;
        }
        index = link->active_custody_index;
        slot = &link->custody[index];
        if (slot->phase != TAVRN_CUSTODY_ACTIVE_QUEUED ||
            slot->scheduler_token != input->tx_token) {
            return TAVRN_LINK_STEP_NO_EVENT;
        }
        if (input->type == BLE_MESH_SCHED_EVENT_TX_DONE &&
            input->tx_completed_channel_mask != 0u) {
            slot->attempt_requested_channel_mask = input->tx_requested_channel_mask;
            if (slot->attempt_requested_channel_mask == 0u) {
                slot->attempt_requested_channel_mask = BLE_RADIO_ADV_CH_ALL;
            }
            slot->attempt_completed_channel_mask = input->tx_completed_channel_mask;
            if (slot->attempt_count == 0u) {
                slot->first_tx_ms = now_ms;
            }
            slot->attempt_count++;
            slot->last_tx_ms = now_ms;
            slot->phase = TAVRN_CUSTODY_ACTIVE_WAIT_HACK;
            slot->response_deadline_ms = now_ms + link->config.hack_response_ms;
            link->counters.tx_done++;
            if (slot->attempt_completed_channel_mask !=
                slot->attempt_requested_channel_mask) {
                link->counters.tx_partial_done++;
            }
            return TAVRN_LINK_STEP_NO_EVENT;
        }
        if (input->type == BLE_MESH_SCHED_EVENT_TX_EXPIRED) {
            slot->attempt_requested_channel_mask = input->tx_requested_channel_mask;
            slot->attempt_completed_channel_mask = 0u;
            terminal_owned(link, index, output,
                           TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED,
                           TAVRN_LOCAL_TX_DEADLINE_EXPIRED,
                           TAVRN_MESH_FAULT_NONE);
            return TAVRN_LINK_STEP_EVENT;
        }
        link->counters.tx_failed++;
        slot->attempt_requested_channel_mask = input->tx_requested_channel_mask;
        if (slot->attempt_requested_channel_mask == 0u) {
            slot->attempt_requested_channel_mask = BLE_RADIO_ADV_CH_ALL;
        }
        slot->attempt_completed_channel_mask = 0u;
        terminal_owned(link, index, output,
                       TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED,
                       TAVRN_LOCAL_TX_RADIO_FAILED, TAVRN_MESH_FAULT_NONE);
        return TAVRN_LINK_STEP_EVENT;
    }
    if (input->type != BLE_MESH_SCHED_EVENT_RX_ADV) {
        return TAVRN_LINK_STEP_INVALID;
    }
    if (input->adv_len > BLE_ADV_MAX_DATA) {
        link->counters.rx_malformed++;
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    config = codec_config(link);
    memset(&frame, 0, sizeof(frame));
    decoded = tavrn_wire_v2_decode(&config, input->adv_addr, input->adv_data,
                                   input->adv_len, &frame);
    if (decoded != TAVRN_CODEC_OK) {
        if (decoded == TAVRN_CODEC_IDENTITY_CONFLICT) {
            link->counters.rx_identity_conflict++;
        } else if (decoded != TAVRN_CODEC_FOREIGN_NETWORK &&
                   decoded != TAVRN_CODEC_UNSUPPORTED_TYPE) {
            link->counters.rx_malformed++;
        }
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    if (logical_id_equal(&frame.transmitter.logical_id,
                         &link->config.local_peer.logical_id) &&
        memcmp(frame.transmitter.adva.bytes, link->config.local_peer.adva.bytes,
               TAVRN_ADVA_LEN) != 0) {
        link->counters.rx_identity_conflict++;
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    if (frame.type == TAVRN_WIRE_HACK) {
        return handle_hack(link, &frame, now_ms, output);
    }
    if (frame.type == TAVRN_WIRE_DATA) {
        tavrn_data_dedupe_entry_t *entry;

        if (!logical_id_equal(&frame.detail.data.immediate_receiver,
                              &link->config.local_peer.logical_id)) {
            link->counters.rx_wrong_next_hop++;
            return TAVRN_LINK_STEP_NO_EVENT;
        }
        if (link->mesh_fault_latched != 0u) {
            return TAVRN_LINK_STEP_NO_EVENT;
        }
        entry = find_data_dedupe(link, &frame.detail.data.data, now_ms);
        if (entry != NULL) {
            link->counters.rx_committed_duplicate++;
            (void)enqueue_hack(link, &frame.transmitter, &frame.detail.data.data,
                               TAVRN_HACK_DUPLICATE, now_ms, output);
            return output->type == TAVRN_LINK_EVENT_NONE ?
                TAVRN_LINK_STEP_NO_EVENT : TAVRN_LINK_STEP_EVENT;
        }
        if (link->candidate_valid != 0u) {
            /* A repeated physical copy of the unresolved first DATA is not a
             * competing candidate. Keep the original token, decision, and
             * response anchor so a later BUSY HACK cannot suppress it. */
            if (direct_peer_equal(&link->candidate.transmitter,
                                  &frame.transmitter) &&
                link_data_equal(&link->candidate.data,
                                &frame.detail.data.data)) {
                return TAVRN_LINK_STEP_NO_EVENT;
            }
            link->counters.rx_additional_data_busy++;
            (void)enqueue_hack(link, &frame.transmitter, &frame.detail.data.data,
                               TAVRN_HACK_BUSY, now_ms, output);
            return TAVRN_LINK_STEP_ADDITIONAL_DATA_BUSY;
        }
        if (!data_dedupe_has_capacity(link, now_ms)) {
            link->counters.rx_data_dedupe_busy++;
            (void)enqueue_hack(link, &frame.transmitter, &frame.detail.data.data,
                                TAVRN_HACK_BUSY, now_ms, output);
            return TAVRN_LINK_STEP_DATA_DEDUPE_BUSY;
        }
        memset(&link->candidate, 0, sizeof(link->candidate));
        link->candidate.token = allocate_candidate_token(link);
        link->candidate.transmitter = frame.transmitter;
        link->candidate.rssi_magnitude_db = input->rssi_magnitude_db;
        link->candidate.data = frame.detail.data.data;
        link->candidate.deadline_ms = now_ms + link->config.candidate_resolve_ms;
        link->candidate_valid = 1u;
        link->counters.rx_candidate++;
        output->type = TAVRN_LINK_EVENT_RX_DATA_CANDIDATE;
        output->detail.candidate = link->candidate;
        return TAVRN_LINK_STEP_EVENT;
    }
    if (frame.type == TAVRN_WIRE_FLOOD) {
        return handle_flood(link, &frame.detail.flood, now_ms);
    }
    output->type = TAVRN_LINK_EVENT_RX_CONTROL;
    output->detail.control.transmitter = frame.transmitter;
    output->detail.control.rssi_magnitude_db = input->rssi_magnitude_db;
    output->detail.control.control = frame.detail.control;
    link->counters.rx_control++;
    return TAVRN_LINK_STEP_EVENT;
}

tavrn_link_step_status_t tavrn_link_v2_tick(
    tavrn_link_v2_t *link, uint32_t now_ms, tavrn_link_event_t *output)
{
    uint8_t i;
    uint8_t selected = TAVRN_CUSTODY_SLOT_NONE;

    if (link == NULL || output == NULL) {
        return TAVRN_LINK_STEP_INVALID;
    }
    clear_event(output);
    if (link->mesh_fault_latched != 0u) {
        for (i = 0u; i < TAVRN_LINK_CUSTODY_CAPACITY; i++) {
            if (link->custody[i].phase == TAVRN_CUSTODY_FAULT_PENDING) {
                terminal_owned(link, i, output,
                               link->latched_mesh_fault_reason ==
                                   TAVRN_MESH_FAULT_POLL_OVERRUN ||
                                   link->latched_mesh_fault_reason ==
                                   TAVRN_MESH_FAULT_INTERNAL_STATE ?
                                   TAVRN_LINK_EVENT_SERVICE_FAULT_TERMINAL :
                                   TAVRN_LINK_EVENT_RADIO_FAULT_TERMINAL,
                               TAVRN_LOCAL_TX_ENCODE_FAILED,
                               link->latched_mesh_fault_reason);
                return TAVRN_LINK_STEP_EVENT;
            }
        }
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    if (link->candidate_valid != 0u &&
        time_due(now_ms, link->candidate.deadline_ms)) {
        tavrn_rx_data_candidate_t candidate = link->candidate;

        memset(&link->candidate, 0, sizeof(link->candidate));
        link->candidate.token = candidate.token;
        link->candidate_valid = 0u;
        link->counters.rx_candidate_timeout_busy++;
        (void)enqueue_hack(link, &candidate.transmitter, &candidate.data,
                           TAVRN_HACK_BUSY,
                           candidate.deadline_ms - link->config.candidate_resolve_ms,
                           output);
        return TAVRN_LINK_STEP_CANDIDATE_TIMEOUT_BUSY;
    }
    for (i = 0u; i < TAVRN_LINK_CUSTODY_CAPACITY; i++) {
        const tavrn_custody_slot_t *slot = &link->custody[i];

        if (slot->phase != TAVRN_CUSTODY_FREE &&
            time_due(now_ms, slot->transaction_deadline_ms) &&
            (selected == TAVRN_CUSTODY_SLOT_NONE ||
             admission_before(slot->admission_order,
                              link->custody[selected].admission_order))) {
            selected = i;
        }
    }
    if (selected != TAVRN_CUSTODY_SLOT_NONE) {
        if (link->custody[selected].phase == TAVRN_CUSTODY_BUSY_WAIT) {
            terminal_owned(link, selected, output,
                           TAVRN_LINK_EVENT_CUSTODY_BUSY_EXPIRED,
                           TAVRN_LOCAL_TX_ENCODE_FAILED, TAVRN_MESH_FAULT_NONE);
        } else {
            terminal_owned(link, selected, output,
                           TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED,
                           TAVRN_LOCAL_TX_DEADLINE_EXPIRED,
                           TAVRN_MESH_FAULT_NONE);
        }
        return TAVRN_LINK_STEP_EVENT;
    }
    if (link->active_custody_index >= TAVRN_LINK_CUSTODY_CAPACITY ||
        link->custody[link->active_custody_index].phase !=
            TAVRN_CUSTODY_ACTIVE_WAIT_HACK ||
        !time_due(now_ms, link->custody[link->active_custody_index].response_deadline_ms)) {
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    selected = link->active_custody_index;
    if (link->custody[selected].attempt_count >= link->config.hack_max_attempts) {
        terminal_owned(link, selected, output, TAVRN_LINK_EVENT_RETRY_EXHAUSTED,
                       TAVRN_LOCAL_TX_ENCODE_FAILED, TAVRN_MESH_FAULT_NONE);
        return TAVRN_LINK_STEP_EVENT;
    }
    link->custody[selected].phase = TAVRN_CUSTODY_READY_NOT_ELIGIBLE;
    link->custody[selected].scheduler_token = BLE_MESH_TX_TOKEN_NONE;
    link->custody[selected].response_deadline_ms = 0u;
    link->custody[selected].retry_not_before_ms =
        now_ms + link->config.retry_backoff_ms;
    link->active_custody_index = TAVRN_CUSTODY_SLOT_NONE;
    link->counters.retry_due++;
    return TAVRN_LINK_STEP_NO_EVENT;
}

tavrn_link_step_status_t tavrn_link_v2_dispatch(
    tavrn_link_v2_t *link, uint32_t now_ms, tavrn_link_event_t *local_outcome)
{
    tavrn_custody_slot_t *slot;
    ble_mesh_tx_item_t item;
    ble_mesh_sched_enqueue_result_t result;
    uint8_t index = TAVRN_CUSTODY_SLOT_NONE;
    uint8_t i;

    if (link == NULL || local_outcome == NULL) {
        return TAVRN_LINK_STEP_INVALID;
    }
    clear_event(local_outcome);
    if (link->mesh_fault_latched != 0u) {
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    if (link->active_custody_index != TAVRN_CUSTODY_SLOT_NONE) {
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    for (i = 0u; i < TAVRN_LINK_CUSTODY_CAPACITY; i++) {
        tavrn_custody_slot_t *candidate = &link->custody[i];

        if ((candidate->phase != TAVRN_CUSTODY_READY_NOT_ELIGIBLE &&
             candidate->phase != TAVRN_CUSTODY_BUSY_WAIT) ||
            !time_due(now_ms, candidate->retry_not_before_ms)) {
            continue;
        }
        if (time_due(now_ms, candidate->transaction_deadline_ms)) {
            terminal_owned(link, i, local_outcome,
                           candidate->phase == TAVRN_CUSTODY_BUSY_WAIT ?
                               TAVRN_LINK_EVENT_CUSTODY_BUSY_EXPIRED :
                               TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED,
                           TAVRN_LOCAL_TX_DEADLINE_EXPIRED,
                           TAVRN_MESH_FAULT_NONE);
            return TAVRN_LINK_STEP_EVENT;
        }
        if (index == TAVRN_CUSTODY_SLOT_NONE ||
            admission_before(candidate->admission_order,
                             link->custody[index].admission_order)) {
            index = i;
        }
    }
    if (index == TAVRN_CUSTODY_SLOT_NONE) {
        return TAVRN_LINK_STEP_NO_EVENT;
    }
    slot = &link->custody[index];
    slot->scheduler_token = allocate_scheduler_token(link);
    if (slot->scheduler_token == BLE_MESH_TX_TOKEN_NONE) {
        terminal_owned(link, index, local_outcome,
                       TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED,
                       TAVRN_LOCAL_TX_SCHEDULER_FULL, TAVRN_MESH_FAULT_NONE);
        return TAVRN_LINK_STEP_EVENT;
    }
    slot->phase = TAVRN_CUSTODY_ACTIVE_QUEUED;
    slot->attempt_requested_channel_mask = BLE_RADIO_ADV_CH_ALL;
    slot->attempt_completed_channel_mask = 0u;
    link->active_custody_index = index;
    link->counters.custody_promoted++;
    memset(&item, 0, sizeof(item));
    item.adv_len = slot->adv_len;
    item.channel_mask = slot->attempt_requested_channel_mask;
    item.priority = slot->attempt_count == 0u ? BLE_MESH_TX_PRIORITY_DATA :
        BLE_MESH_TX_PRIORITY_RETRY;
    item.service_class = BLE_MESH_TX_SERVICE_CUSTODY_DATA;
    item.not_before_ms = now_ms;
    item.expiry_ms = slot->transaction_deadline_ms;
    item.sweep_count = BLE_MESH_TX_SWEEP_COUNT_TWO;
    item.budget_class = BLE_MESH_TX_BUDGET_GENERAL;
    item.token = slot->scheduler_token;
    memcpy(item.adv_data, slot->adv_data, slot->adv_len);
    result = ble_mesh_scheduler_enqueue_ex(link->scheduler, &item);
    if (result.status != BLE_MESH_SCHED_ENQUEUE_OK) {
        terminal_owned(link, index, local_outcome,
                       TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED,
                       TAVRN_LOCAL_TX_SCHEDULER_FULL, TAVRN_MESH_FAULT_NONE);
        return TAVRN_LINK_STEP_EVENT;
    }
    link->counters.tx_admitted++;
    if (result.evicted_token != BLE_MESH_TX_TOKEN_NONE &&
        terminal_evicted_token(link, result.evicted_token, local_outcome)) {
        return TAVRN_LINK_STEP_EVENT;
    }
    return TAVRN_LINK_STEP_NO_EVENT;
}

const tavrn_link_counters_t *tavrn_link_v2_counters(const tavrn_link_v2_t *link)
{
    return link == NULL ? NULL : &link->counters;
}

tavrn_link_subject_demand_snapshot_t tavrn_link_v2_subject_demand_snapshot(
    const tavrn_link_v2_t *link, const tavrn_logical_id_t *subject,
    const tavrn_adva_t *canonical_subject, uint32_t now_ms)
{
    tavrn_link_subject_demand_snapshot_t snapshot;
    uint8_t index;

    memset(&snapshot, 0, sizeof(snapshot));
    if (link == NULL || subject == NULL || canonical_subject == NULL ||
        subject->width != link->config.local_peer.logical_id.width ||
        !logical_id_valid(subject, 0)) {
        return snapshot;
    }
    if (link->mesh_fault_latched != 0u) {
        return snapshot;
    }
    snapshot.snapshot_available = 1u;
    for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
        const tavrn_custody_slot_t *slot = &link->custody[index];

        if (slot->phase == TAVRN_CUSTODY_FREE ||
            time_due(now_ms, slot->transaction_deadline_ms)) {
            continue;
        }
        if (slot->data.final_destination.width == subject->width &&
            slot->data.final_destination.value == subject->value) {
            snapshot.reason_mask |= TAVRN_MAINT_DEMAND_CUSTODY_FINAL;
        }
        if (slot->next_hop.logical_id.width == subject->width &&
            slot->next_hop.logical_id.value == subject->value &&
            memcmp(slot->next_hop.adva.bytes, canonical_subject->bytes,
                   TAVRN_ADVA_LEN) == 0) {
            snapshot.reason_mask |= TAVRN_MAINT_DEMAND_CUSTODY_NEXT_HOP;
        }
    }
    return snapshot;
}

static int queued_item_has_peer_id(const ble_mesh_tx_item_t *item,
                                   const tavrn_direct_peer_t *peer)
{
    const uint8_t *pdu;
    tavrn_identity_width_t width;
    uint8_t width_len;

    if (item == NULL || peer == NULL || item->adv_len < 12u ||
        item->adv_data[7] != 0x54u || item->adv_data[8] != 0x52u ||
        item->adv_data[9] != 0x02u) {
        return 0;
    }
    pdu = &item->adv_data[7];
    width = (pdu[5] & 0x80u) != 0u ? TAVRN_IDENTITY_SID8 :
                                     TAVRN_IDENTITY_SID16;
    width_len = width == TAVRN_IDENTITY_SID8 ? 1u : 2u;
    if (width != peer->logical_id.width) {
        return 0;
    }
    /* Do not use the validated-control helper above: queued work is already
     * encoded and must be removable even when its later receiver would reject
     * it.  The explicit byte reads below retain queue/link layering. */
#define QUEUED_ID_IS_PEER(at) \
    ((uint32_t)(at) + (uint32_t)width_len <= (uint32_t)item->adv_len - 7u && \
     (width == TAVRN_IDENTITY_SID8 ? (uint16_t)pdu[(at)] : \
      (uint16_t)pdu[(at)] | ((uint16_t)pdu[(uint8_t)((at) + 1u)] << 8)) == \
         peer->logical_id.value)
    switch ((tavrn_wire_type_t)pdu[4]) {
    case TAVRN_WIRE_DATA:
        {
            tavrn_link_data_t data;
            tavrn_logical_id_t receiver;

            if ((uint32_t)(7u + 3u * width_len) >
                (uint32_t)item->adv_len - 7u) {
                return 0;
            }
            memset(&data, 0, sizeof(data));
            receiver.width = width;
            receiver.value = width == TAVRN_IDENTITY_SID8 ? pdu[7] :
                (uint16_t)pdu[7] | ((uint16_t)pdu[8] << 8);
            data.origin.width = width;
            data.origin.value = width == TAVRN_IDENTITY_SID8 ?
                pdu[(uint8_t)(7u + width_len)] :
                (uint16_t)pdu[(uint8_t)(7u + width_len)] |
                    ((uint16_t)pdu[(uint8_t)(8u + width_len)] << 8);
            data.final_destination.width = width;
            data.final_destination.value = width == TAVRN_IDENTITY_SID8 ?
                pdu[(uint8_t)(7u + 2u * width_len)] :
                (uint16_t)pdu[(uint8_t)(7u + 2u * width_len)] |
                    ((uint16_t)pdu[(uint8_t)(8u + 2u * width_len)] << 8);
            return data_uses_direct_peer(&receiver, &data, peer);
        }
    case TAVRN_WIRE_HACK:
        return QUEUED_ID_IS_PEER(6u) || QUEUED_ID_IS_PEER((uint8_t)(6u + width_len)) ||
            QUEUED_ID_IS_PEER((uint8_t)(6u + 2u * width_len));
    case TAVRN_WIRE_FLOOD:
        return QUEUED_ID_IS_PEER(7u);
    case TAVRN_WIRE_E_RREQ:
        return QUEUED_ID_IS_PEER(7u) ||
            QUEUED_ID_IS_PEER((uint8_t)(9u + width_len));
    case TAVRN_WIRE_E_RREP:
        return QUEUED_ID_IS_PEER(7u) ||
            QUEUED_ID_IS_PEER((uint8_t)(7u + width_len)) ||
            QUEUED_ID_IS_PEER((uint8_t)(9u + 2u * width_len));
    case TAVRN_WIRE_E_RREP_ACK:
        return QUEUED_ID_IS_PEER(6u) ||
            QUEUED_ID_IS_PEER((uint8_t)(6u + width_len)) ||
            QUEUED_ID_IS_PEER((uint8_t)(8u + 2u * width_len));
    case TAVRN_WIRE_E_RERR:
        /* A locally-originated RERR remains queued even when one unreachable
         * entry names the rebooted peer.  A stale reporter is peer-owned;
         * unreachable destinations are notification payload, not ownership. */
        return QUEUED_ID_IS_PEER(7u);
    default:
        return 0;
    }
#undef QUEUED_ID_IS_PEER
}

static int remove_queued_peer_work(tavrn_link_v2_t *link,
                                   const tavrn_direct_peer_t *peer)
{
    uint8_t index;

    if (link == NULL || link->scheduler == NULL) {
        return 0;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_queue_entry_t *entry =
            &link->scheduler->routed_tx_queue.entries[index];

        if (entry->occupied != 0u && queued_item_has_peer_id(&entry->item, peer) &&
            !retire_queued_entry(&link->scheduler->routed_tx_queue, index,
                                  BLE_MESH_TX_TERMINAL_PEER_RESET)) {
            return 0;
        }
    }
    return 1;
}

static int peer_custody_can_clear(tavrn_link_v2_t *link,
                                  const tavrn_direct_peer_t *peer)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
        const tavrn_custody_slot_t *slot = &link->custody[index];

        if (custody_uses_direct_peer(slot, peer) &&
            !transit_dedupe_can_release(link, &slot->data)) {
            return 0;
        }
    }
    return 1;
}

static int clear_peer_custody(tavrn_link_v2_t *link,
                              const tavrn_direct_peer_t *peer)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
        tavrn_custody_slot_t *slot = &link->custody[index];

        if (!custody_uses_direct_peer(slot, peer)) {
            continue;
        }
        if (!release_transit_dedupe(link, &slot->data)) {
            return 0;
        }
        if (logical_id_equal(&slot->data.origin, &peer->logical_id) ||
            logical_id_equal(&slot->data.final_destination, &peer->logical_id)) {
            clear_data_dedupe_exact(link, &slot->data);
        }
        reset_slot(link, index);
    }
    return 1;
}

static void clear_peer_data_dedupe(tavrn_link_v2_t *link,
                                   const tavrn_direct_peer_t *peer)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_LINK_DATA_DEDUPE_CAPACITY; index++) {
        tavrn_data_dedupe_entry_t *entry = &link->data_dedupe[index];

        if (entry->valid != 0u &&
            (logical_id_equal(&entry->origin, &peer->logical_id) ||
              logical_id_equal(&entry->final_destination, &peer->logical_id))) {
            /* Router/AODV-owned pins die with this reset.  Preserve only an
             * explicit external copied-data owner through exact release. */
            if (entry->custody_pinned != 0u &&
                entry->external_custody_owner != 0u) {
                entry->incarnation_clear_on_release = 1u;
            } else {
                memset(entry, 0, sizeof(*entry));
            }
        }
    }
}

tavrn_link_resolve_status_t tavrn_link_v2_quarantine_peer_incarnation(
    tavrn_link_v2_t *link, const tavrn_direct_peer_t *peer)
{
    if (link == NULL || !direct_peer_valid(peer) ||
        !peer_custody_can_clear(link, peer) ||
        !remove_queued_peer_work(link, peer)) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    if (!clear_peer_custody(link, peer)) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    clear_peer_data_dedupe(link, peer);
    if (link->candidate_valid != 0u &&
        candidate_uses_direct_peer(&link->candidate, peer)) {
        memset(&link->candidate, 0, sizeof(link->candidate));
        link->candidate_valid = 0u;
    }
    return TAVRN_LINK_RESOLVE_OK;
}

tavrn_link_resolve_status_t tavrn_link_v2_clear_peer_incarnation(
    tavrn_link_v2_t *link, const tavrn_direct_peer_t *peer, uint32_t now_ms)
{
    uint8_t index;

    if (tavrn_link_v2_quarantine_peer_incarnation(link, peer) !=
        TAVRN_LINK_RESOLVE_OK) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    for (index = 0u; index < TAVRN_LINK_FLOOD_DEDUPE_CAPACITY; index++) {
        tavrn_flood_dedupe_entry_t *entry = &link->flood_dedupe[index];

        if (entry->valid != 0u &&
            logical_id_equal(&entry->origin, &peer->logical_id)) {
            memset(entry, 0, sizeof(*entry));
        }
    }
    (void)now_ms;
    return TAVRN_LINK_RESOLVE_OK;
}

tavrn_link_resolve_status_t tavrn_link_v2_peer_incarnation_snapshot(
    const tavrn_link_v2_t *link, const tavrn_direct_peer_t *peer,
    tavrn_link_peer_incarnation_snapshot_t *snapshot_out)
{
    uint8_t index;

    if (link == NULL || !direct_peer_valid(peer) || snapshot_out == NULL) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    memset(snapshot_out, 0, sizeof(*snapshot_out));
    for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
        if (custody_uses_direct_peer(&link->custody[index], peer)) {
            snapshot_out->pending_custody_count++;
        }
    }
    for (index = 0u; index < TAVRN_LINK_DATA_DEDUPE_CAPACITY; index++) {
        if (link->data_dedupe[index].valid != 0u &&
            logical_id_equal(&link->data_dedupe[index].origin,
                              &peer->logical_id)) {
            snapshot_out->data_dedupe_count++;
        }
    }
    return TAVRN_LINK_RESOLVE_OK;
}

tavrn_link_custody_snapshot_status_t
tavrn_link_v2_custody_snapshot_for_destination(
    const tavrn_link_v2_t *link, const tavrn_logical_id_t *destination,
    tavrn_link_custody_data_snapshot_t *snapshot_out)
{
    uint8_t index;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (link == NULL || destination == NULL || snapshot_out == NULL ||
        !logical_id_valid(destination, 0) ||
        destination->width != link->config.local_peer.logical_id.width) {
        return TAVRN_LINK_CUSTODY_SNAPSHOT_INVALID;
    }
    for (index = 0u; index < TAVRN_LINK_CUSTODY_CAPACITY; index++) {
        const tavrn_custody_slot_t *slot = &link->custody[index];

        if (slot->phase == TAVRN_CUSTODY_FREE ||
            !logical_id_equal(&slot->data.final_destination, destination)) {
            continue;
        }
        if (!data_valid(&slot->data) || !direct_peer_valid(&slot->next_hop) ||
            snapshot_out->count >= TAVRN_LINK_CUSTODY_CAPACITY) {
            memset(snapshot_out, 0, sizeof(*snapshot_out));
            return TAVRN_LINK_CUSTODY_SNAPSHOT_INVALID;
        }
        snapshot_out->records[snapshot_out->count].next_hop = slot->next_hop;
        snapshot_out->records[snapshot_out->count].data = slot->data;
        snapshot_out->count++;
    }
    return TAVRN_LINK_CUSTODY_SNAPSHOT_OK;
}

tavrn_link_resolve_status_t tavrn_link_v2_reconfigure_identity(
    tavrn_link_v2_t *link, const tavrn_direct_peer_t *local_peer,
    uint32_t now_ms)
{
    tavrn_link_config_t config;
    ble_mesh_scheduler_t *scheduler;
    uint8_t index;

    if (link == NULL || local_peer == NULL || !direct_peer_valid(local_peer) ||
        link->scheduler == NULL) {
        return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
    }
    config = link->config;
    scheduler = link->scheduler;
    config.local_peer = *local_peer;
    /* No old-width control or DATA may survive to be transmitted after the
     * new logical namespace becomes visible. Keep the scheduler's temporary
     * bridge installed while each tracked entry is terminalized. */
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (scheduler->routed_tx_queue.entries[index].occupied != 0u &&
            !retire_queued_entry(&scheduler->routed_tx_queue, index,
                                  BLE_MESH_TX_TERMINAL_IDENTITY_RESET)) {
            return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
        }
    }
    return tavrn_link_v2_init(link, scheduler, &config, now_ms) ==
            TAVRN_LINK_INIT_OK ? TAVRN_LINK_RESOLVE_OK :
            TAVRN_LINK_RESOLVE_TOKEN_INVALID;
}

void tavrn_link_v2_set_identity_admission(
    tavrn_link_v2_t *link, tavrn_codec_identity_conflict_fn callback,
    void *context)
{
    if (link == NULL) {
        return;
    }
    link->config.identity_conflict = callback;
    link->config.identity_context = context;
}
