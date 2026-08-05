#include "ble_mesh_scheduler.h"

#if defined(__GNUC__)
/* Legacy firmware still links this scheduler before the routed queue is added
 * to its source manifest. Undefined weak routed seams keep that wire-v1 build
 * runnable; a routed initializer refuses to start until every seam is linked. */
#pragma weak ble_radio_try_listen_once
#pragma weak ble_radio_try_poll_snapshot
#pragma weak ble_radio_try_advertise_channels
#pragma weak ble_mesh_tx_queue_init
#pragma weak ble_mesh_tx_queue_enqueue
#pragma weak ble_mesh_tx_queue_select_due
#pragma weak ble_mesh_tx_queue_complete
#endif

static const uint8_t ble_mesh_sched_channels[3] = { 37u, 38u, 39u };

#define BLE_MESH_SCHED_ROUTED_LEGACY          0u
#define BLE_MESH_SCHED_ROUTED_HEALTHY         1u
#define BLE_MESH_SCHED_ROUTED_FAULT_PENDING   2u
#define BLE_MESH_SCHED_ROUTED_FAULT_REPORTED  3u
#define BLE_MESH_SCHED_ROUTED_INVALID         4u

static void routed_start_rx(ble_mesh_scheduler_t *sched, uint32_t now_ms);
static int routed_emit_fault(ble_mesh_scheduler_t *sched,
                             ble_mesh_sched_event_t *event);
static void routed_pack_fault_diagnostic(ble_mesh_scheduler_t *sched,
                                         ble_mesh_tx_token_t token,
                                         uint8_t requested_mask,
                                         uint8_t completed_mask,
                                         int has_tx_diagnostic);
static void legacy_latch_fault(ble_mesh_scheduler_t *sched,
                               ble_mesh_sched_fault_t fault,
                               uint8_t requested_mask,
                               uint8_t completed_mask,
                               int has_tx_diagnostic);
static int local_adva_is_valid(const uint8_t adva[6]);
static ble_mesh_sched_fault_t routed_fault_from_radio(ble_radio_op_result_t result);

static void clear_bytes(void *dst, size_t len)
{
    uint8_t *p = (uint8_t *)dst;

    while (len > 0) {
        *p++ = 0u;
        len--;
    }
}

static void copy_bytes(void *dst, const void *src, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (len > 0) {
        *d++ = *s++;
        len--;
    }
}

static int time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (uint32_t)(now_ms - deadline_ms) < 0x80000000UL;
}

static int elapsed_less_than(uint32_t now_ms, uint32_t then_ms, uint32_t interval_ms)
{
    return (uint32_t)(now_ms - then_ms) < interval_ms;
}

static uint8_t current_channel(const ble_mesh_scheduler_t *sched)
{
    return ble_mesh_sched_channels[sched->rx_channel_index % 3u];
}

static uint8_t sanitize_channel_mask(uint8_t channel_mask)
{
    channel_mask &= (uint8_t)BLE_MESH_SCHED_CH_ALL;
    if (channel_mask == 0u) {
        channel_mask = (uint8_t)BLE_MESH_SCHED_CH_ALL;
    }
    return channel_mask;
}

static uint8_t queue_index(const ble_mesh_scheduler_t *sched, uint8_t pos)
{
    return (uint8_t)((sched->legacy_tx_head + pos) %
                     BLE_MESH_SCHED_LEGACY_TX_QUEUE_CAPACITY);
}

static void queue_remove_pos(ble_mesh_scheduler_t *sched, uint8_t pos)
{
    uint8_t i;

    if (pos >= sched->legacy_tx_count) {
        return;
    }

    for (i = pos; i + 1u < sched->legacy_tx_count; i++) {
        sched->legacy_tx_queue[queue_index(sched, i)] =
            sched->legacy_tx_queue[queue_index(sched, (uint8_t)(i + 1u))];
    }

    sched->legacy_tx_count--;
    if (sched->legacy_tx_count == 0u) {
        sched->legacy_tx_head = 0u;
    }
}

static int find_oldest_relay(const ble_mesh_scheduler_t *sched)
{
    uint8_t pos;

    for (pos = 0u; pos < sched->legacy_tx_count; pos++) {
        if (sched->legacy_tx_queue[queue_index(sched, pos)].kind ==
            BLE_MESH_SCHED_TX_RELAY) {
            return (int)pos;
        }
    }
    return -1;
}

static int queue_push(ble_mesh_scheduler_t *sched,
                      const ble_mesh_sched_legacy_tx_item_t *item)
{
    uint8_t tail;

    if (sched->legacy_tx_count >= BLE_MESH_SCHED_LEGACY_TX_QUEUE_CAPACITY) {
        return 0;
    }

    tail = queue_index(sched, sched->legacy_tx_count);
    sched->legacy_tx_queue[tail] = *item;
    sched->legacy_tx_count++;
    return 1;
}

static int listen_once_on_current_channel(ble_mesh_scheduler_t *sched)
{
    ble_radio_op_result_t result;

    if (ble_radio_try_listen_once == NULL) {
        legacy_latch_fault(sched, BLE_MESH_SCHED_FAULT_INTERNAL_STATE,
                           0u, 0u, 0);
        return 0;
    }
    result = ble_radio_try_listen_once((UINT)current_channel(sched),
                                       (UINT)sched->timers->radio_state_timeout_ms);
    if (result != BLE_RADIO_OP_OK) {
        legacy_latch_fault(sched, routed_fault_from_radio(result), 0u, 0u, 0);
        return 0;
    }
    sched->rx_started = 1u;
    return 1;
}

static int start_rx_on_current_channel(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    if (!listen_once_on_current_channel(sched)) {
        return 0;
    }
    sched->hop_at_ms = now_ms + sched->dwell_ms;
    return 1;
}

static int restore_rx_or_hop(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    if (time_reached(now_ms, sched->hop_at_ms)) {
        sched->rx_channel_index = (uint8_t)((sched->rx_channel_index + 1u) % 3u);
        return start_rx_on_current_channel(sched, now_ms);
    }

    return listen_once_on_current_channel(sched);
}

static int ensure_rx_started(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    if (sched->rx_started == 0u) {
        return start_rx_on_current_channel(sched, now_ms);
    }
    return 1;
}

static int hop_rx_channel(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    sched->rx_channel_index = (uint8_t)((sched->rx_channel_index + 1u) % 3u);
    return start_rx_on_current_channel(sched, now_ms);
}

static int item_not_before_reached(const ble_mesh_sched_legacy_tx_item_t *item,
                                   uint32_t now_ms)
{
    return time_reached(now_ms, item->not_before_ms);
}

static int relay_rate_allows(ble_mesh_scheduler_t *sched,
                             ble_mesh_sched_legacy_tx_item_t *item,
                             uint32_t now_ms)
{
    uint32_t next_ms;

    if (item->kind != BLE_MESH_SCHED_TX_RELAY) {
        return 1;
    }
    if (sched->relay_rate_limit_ms == 0u || sched->relay_last_tx_valid == 0u) {
        return 1;
    }
    if (!elapsed_less_than(now_ms, sched->last_relay_tx_ms, sched->relay_rate_limit_ms)) {
        return 1;
    }

    next_ms = sched->last_relay_tx_ms + sched->relay_rate_limit_ms;
    if (time_reached(next_ms, item->not_before_ms)) {
        item->not_before_ms = next_ms;
    }
    sched->counters.relay_rate_limited++;
    return 0;
}

static int select_tx_pos(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    uint8_t pos;

    /* Own packets are allowed to pass relay packets. Relay packets are lower
       priority and can also be delayed by the relay rate limiter. */
    for (pos = 0u; pos < sched->legacy_tx_count; pos++) {
        ble_mesh_sched_legacy_tx_item_t *item =
            &sched->legacy_tx_queue[queue_index(sched, pos)];
        if (item->kind == BLE_MESH_SCHED_TX_OWN && item_not_before_reached(item, now_ms)) {
            return (int)pos;
        }
    }

    for (pos = 0u; pos < sched->legacy_tx_count; pos++) {
        ble_mesh_sched_legacy_tx_item_t *item =
            &sched->legacy_tx_queue[queue_index(sched, pos)];
        if (item->kind == BLE_MESH_SCHED_TX_RELAY &&
            item_not_before_reached(item, now_ms) &&
            relay_rate_allows(sched, item, now_ms)) {
            return (int)pos;
        }
    }

    return -1;
}

static int transmit_pos(ble_mesh_scheduler_t *sched, uint8_t pos,
                        uint32_t now_ms, ble_mesh_sched_event_t *event)
{
    ble_mesh_sched_legacy_tx_item_t item;
    ble_radio_tx_result_t tx_result;

    item = sched->legacy_tx_queue[queue_index(sched, pos)];
    queue_remove_pos(sched, pos);

    sched->rx_started = 0u;
    if (ble_radio_try_advertise_channels == NULL) {
        legacy_latch_fault(sched, BLE_MESH_SCHED_FAULT_INTERNAL_STATE,
                           item.channel_mask, 0u, 1);
        if (event != NULL) {
            event->type = BLE_MESH_SCHED_EVENT_TX_FAILED;
            event->tx_token = BLE_MESH_TX_TOKEN_NONE;
            event->tx_requested_channel_mask = item.channel_mask;
            event->fault = BLE_MESH_SCHED_FAULT_INTERNAL_STATE;
        }
        return 1;
    }
    tx_result = ble_radio_try_advertise_channels(
        (const UB *)item.adv_data, (UINT)item.adv_len, sched->local_adva,
        (UINT)item.channel_mask, (UINT)sched->timers->radio_state_timeout_ms);
    if (event != NULL) {
        event->type = tx_result.completed_channel_mask == 0u ?
            BLE_MESH_SCHED_EVENT_TX_FAILED : BLE_MESH_SCHED_EVENT_TX_DONE;
        event->tx_token = BLE_MESH_TX_TOKEN_NONE;
        event->tx_requested_channel_mask = tx_result.requested_channel_mask;
        event->tx_completed_channel_mask = tx_result.completed_channel_mask;
        event->fault = tx_result.fault == BLE_RADIO_OP_OK ?
            BLE_MESH_SCHED_FAULT_NONE : routed_fault_from_radio(tx_result.fault);
    }
    if (tx_result.completed_channel_mask != 0u) {
        sched->counters.tx_ok++;
        if (item.kind == BLE_MESH_SCHED_TX_RELAY) {
            sched->last_relay_tx_ms = now_ms;
            sched->relay_last_tx_valid = 1u;
        }
    }
    if (tx_result.fault != BLE_RADIO_OP_OK) {
        legacy_latch_fault(sched, routed_fault_from_radio(tx_result.fault),
                           tx_result.requested_channel_mask,
                           tx_result.completed_channel_mask, 1);
        return 1;
    }
    if (tx_result.completed_channel_mask == 0u) {
        legacy_latch_fault(sched, BLE_MESH_SCHED_FAULT_INTERNAL_STATE,
                           tx_result.requested_channel_mask, 0u, 1);
        if (event != NULL) {
            event->fault = BLE_MESH_SCHED_FAULT_INTERNAL_STATE;
        }
        return 1;
    }
    if (!restore_rx_or_hop(sched, now_ms)) {
        routed_pack_fault_diagnostic(sched, BLE_MESH_TX_TOKEN_NONE,
                                     tx_result.requested_channel_mask,
                                     tx_result.completed_channel_mask, 1);
        if (event != NULL) {
            event->fault = sched->latched_fault;
        }
    }
    return 1;
}

static int validate_rx_pdu(const UB *pdu, UINT pdu_len, UINT *adv_len_out)
{
    UINT adv_len;

    if (pdu == NULL || pdu_len < 8u || pdu[1] < 6u) {
        return 0;
    }

    adv_len = (UINT)pdu[1] - 6u;
    if (adv_len > BLE_MESH_SCHED_ADV_DATA_MAX || pdu_len < adv_len + 8u) {
        return 0;
    }

    if (adv_len_out != NULL) {
        *adv_len_out = adv_len;
    }
    return 1;
}

static int copy_rx_event(ble_mesh_scheduler_t *sched,
                         const UB *pdu,
                         UINT pdu_len,
                         UINT rssi_dbm,
                         ble_mesh_sched_event_t *event)
{
    UINT adv_len;

    if (event == NULL || !validate_rx_pdu(pdu, pdu_len, &adv_len)) {
        return 0;
    }

    event->type = BLE_MESH_SCHED_EVENT_RX_ADV;
    event->channel = current_channel(sched);
    event->rssi_magnitude_db = (uint8_t)(rssi_dbm & 0xFFu);
    event->adv_len = (uint8_t)adv_len;
    if (adv_len > 0u) {
        copy_bytes(event->adv_data, &pdu[8], adv_len);
    }
    return 1;
}

void ble_mesh_scheduler_init_legacy(ble_mesh_scheduler_t *sched, uint32_t now_ms,
                                     const uint8_t local_adva[6])
{
    if (sched == NULL) {
        return;
    }

    clear_bytes(sched, sizeof(*sched));
    sched->routed_started = BLE_MESH_SCHED_ROUTED_INVALID;
    if (!local_adva_is_valid(local_adva) ||
        !tron_timer_config_is_valid(&tron_timer_config)) {
        return;
    }
    sched->timers = &tron_timer_config;
    sched->dwell_ms = tron_timer_config.scheduler_dwell_ms;
    sched->relay_rate_limit_ms = tron_timer_config.scheduler_relay_spacing_ms;
    sched->hop_at_ms = now_ms + sched->dwell_ms;
    copy_bytes(sched->local_adva, local_adva, sizeof(sched->local_adva));
    sched->local_adva_valid = 1u;
    sched->routed_started = BLE_MESH_SCHED_ROUTED_LEGACY;
}

void ble_mesh_scheduler_start_rx(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    if (sched == NULL) {
        return;
    }
    if (sched->routed_started != BLE_MESH_SCHED_ROUTED_LEGACY) {
        if (sched->routed_started == BLE_MESH_SCHED_ROUTED_HEALTHY) {
            routed_start_rx(sched, now_ms);
        }
        return;
    }
    (void)start_rx_on_current_channel(sched, now_ms);
}

int ble_mesh_scheduler_enqueue(ble_mesh_scheduler_t *sched,
                               const uint8_t *adv,
                               uint8_t adv_len,
                               uint8_t channel_mask,
                               ble_mesh_sched_tx_kind_t kind,
                               uint32_t not_before_ms)
{
    ble_mesh_sched_legacy_tx_item_t item;
    int relay_pos;

    if (sched == NULL || (adv == NULL && adv_len != 0u)) {
        return 0;
    }
    if (sched->routed_started != BLE_MESH_SCHED_ROUTED_LEGACY) {
        return 0;
    }
    if (adv_len > BLE_MESH_SCHED_ADV_DATA_MAX) {
        sched->counters.tx_len_drop++;
        return 0;
    }

    if (kind != BLE_MESH_SCHED_TX_OWN) {
        kind = BLE_MESH_SCHED_TX_RELAY;
    }

    if (sched->legacy_tx_count >= BLE_MESH_SCHED_LEGACY_TX_QUEUE_CAPACITY) {
        if (kind == BLE_MESH_SCHED_TX_RELAY) {
            sched->counters.queue_drop++;
            sched->counters.relay_drop++;
            return 0;
        }

        relay_pos = find_oldest_relay(sched);
        if (relay_pos < 0) {
            sched->counters.queue_drop++;
            return 0;
        }
        queue_remove_pos(sched, (uint8_t)relay_pos);
        sched->counters.queue_drop++;
        sched->counters.relay_drop++;
    }

    clear_bytes(&item, sizeof(item));
    item.adv_len = adv_len;
    item.channel_mask = sanitize_channel_mask(channel_mask);
    item.kind = kind;
    item.not_before_ms = not_before_ms;
    if (adv_len > 0u) {
        copy_bytes(item.adv_data, adv, adv_len);
    }

    return queue_push(sched, &item);
}

static int ble_mesh_scheduler_poll_legacy(ble_mesh_scheduler_t *sched,
                                           uint32_t now_ms,
                                           ble_mesh_sched_event_t *event)
{
    UB pdu[BLE_RX_MAX];
    UINT pdu_len = 0u;
    UINT rssi_dbm = 0u;
    ble_radio_op_result_t snapshot;
    int tx_pos;

    if (event != NULL) {
        clear_bytes(event, sizeof(*event));
        event->type = BLE_MESH_SCHED_EVENT_NONE;
    }
    if (sched == NULL) {
        return 0;
    }

    if (!ensure_rx_started(sched, now_ms)) {
        return 0;
    }

    /* Service one due TX before consuming another RX snapshot. Otherwise a
       continuously busy advertising channel can indefinitely starve queued
       own and relay traffic. Each TX restores RX before returning. */
    tx_pos = select_tx_pos(sched, now_ms);
    if (tx_pos >= 0) {
        return transmit_pos(sched, (uint8_t)tx_pos, now_ms, event);
    }

    if (ble_radio_try_poll_snapshot == NULL) {
        legacy_latch_fault(sched, BLE_MESH_SCHED_FAULT_INTERNAL_STATE,
                           0u, 0u, 0);
        return 0;
    }
    snapshot = ble_radio_try_poll_snapshot(
        pdu, &pdu_len, &rssi_dbm, (UINT)sched->timers->radio_state_timeout_ms);
    if (snapshot != BLE_RADIO_OP_NO_EVENT) {
        sched->rx_started = 0u;
        if (snapshot == BLE_RADIO_OP_OK && event == NULL &&
            validate_rx_pdu(pdu, pdu_len, NULL)) {
            sched->counters.rx_ok++;
            (void)restore_rx_or_hop(sched, now_ms);
            return 1;
        }
        if (snapshot == BLE_RADIO_OP_OK &&
            copy_rx_event(sched, pdu, pdu_len, rssi_dbm, event)) {
            sched->counters.rx_ok++;
            (void)restore_rx_or_hop(sched, now_ms);
            return 1;
        }

        if (snapshot == BLE_RADIO_OP_CRC_DROP || snapshot == BLE_RADIO_OP_OK) {
            sched->counters.rx_crc_or_empty++;
            (void)restore_rx_or_hop(sched, now_ms);
            return 0;
        }
        legacy_latch_fault(sched, routed_fault_from_radio(snapshot),
                           0u, 0u, 0);
        return 0;
    }

    if (time_reached(now_ms, sched->hop_at_ms)) {
        (void)hop_rx_channel(sched, now_ms);
    }

    return 0;
}

static int routed_radio_api_available(void)
{
    return ble_radio_try_listen_once != NULL &&
           ble_radio_try_poll_snapshot != NULL &&
           ble_radio_try_advertise_channels != NULL;
}

static int routed_queue_api_available(void)
{
    return ble_mesh_tx_queue_init != NULL &&
           ble_mesh_tx_queue_enqueue != NULL &&
           ble_mesh_tx_queue_select_due != NULL &&
           ble_mesh_tx_queue_complete != NULL;
}

static int local_adva_is_valid(const uint8_t adva[6])
{
    uint8_t index;
    int random_all_zero = 1;
    int random_all_one = 1;

    if (adva == NULL || (adva[5] & 0xC0u) != 0xC0u) {
        return 0;
    }
    for (index = 0u; index < 5u; index++) {
        if (adva[index] != 0u) {
            random_all_zero = 0;
        }
        if (adva[index] != 0xFFu) {
            random_all_one = 0;
        }
    }
    if ((adva[5] & 0x3Fu) != 0u) {
        random_all_zero = 0;
    }
    if ((adva[5] & 0x3Fu) != 0x3Fu) {
        random_all_one = 0;
    }
    return !random_all_zero && !random_all_one &&
           !(adva[0] == 0u && adva[1] == 0u) &&
           !(adva[0] == 0xFFu && adva[1] == 0xFFu);
}

static ble_mesh_sched_fault_t routed_fault_from_radio(ble_radio_op_result_t result)
{
    if (result == BLE_RADIO_OP_STATE_TIMEOUT) {
        return BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT;
    }
    return BLE_MESH_SCHED_FAULT_RADIO_INVALID_ARGUMENT;
}

static int routed_fault_is_radio(ble_mesh_sched_fault_t fault)
{
    return fault == BLE_MESH_SCHED_FAULT_RADIO_TIMEOUT ||
           fault == BLE_MESH_SCHED_FAULT_RADIO_INVALID_ARGUMENT;
}

static void routed_pack_fault_diagnostic(ble_mesh_scheduler_t *sched,
                                         ble_mesh_tx_token_t token,
                                         uint8_t requested_mask,
                                         uint8_t completed_mask,
                                         int has_tx_diagnostic)
{
    sched->last_relay_tx_ms = (uint32_t)token |
        ((uint32_t)requested_mask << 16) |
        ((uint32_t)completed_mask << 24);
    sched->relay_last_tx_valid = has_tx_diagnostic ? 1u : 0u;
}

static void routed_latch_fault(ble_mesh_scheduler_t *sched,
                               ble_mesh_sched_fault_t fault,
                               ble_mesh_tx_token_t token,
                               uint8_t requested_mask,
                               uint8_t completed_mask,
                               int has_tx_diagnostic)
{
    if (sched->routed_started != BLE_MESH_SCHED_ROUTED_HEALTHY) {
        return;
    }

    routed_pack_fault_diagnostic(sched, token, requested_mask, completed_mask,
                                 has_tx_diagnostic);
    sched->latched_fault = fault;
    sched->routed_started = BLE_MESH_SCHED_ROUTED_FAULT_PENDING;
    sched->rx_started = 0u;
    clear_bytes(&sched->routed_tx_queue, sizeof(sched->routed_tx_queue));
    sched->custody_bypass_count = 0u;
}

static void legacy_latch_fault(ble_mesh_scheduler_t *sched,
                               ble_mesh_sched_fault_t fault,
                               uint8_t requested_mask,
                               uint8_t completed_mask,
                               int has_tx_diagnostic)
{
    if (sched->routed_started != BLE_MESH_SCHED_ROUTED_LEGACY) {
        return;
    }

    routed_pack_fault_diagnostic(sched, BLE_MESH_TX_TOKEN_NONE,
                                 requested_mask, completed_mask,
                                 has_tx_diagnostic);
    sched->latched_fault = fault;
    sched->routed_started = BLE_MESH_SCHED_ROUTED_FAULT_PENDING;
    sched->rx_started = 0u;
    sched->legacy_tx_head = 0u;
    sched->legacy_tx_count = 0u;
}

static int routed_emit_fault(ble_mesh_scheduler_t *sched,
                             ble_mesh_sched_event_t *event)
{
    if (sched->routed_started != BLE_MESH_SCHED_ROUTED_FAULT_PENDING) {
        return 0;
    }

    if (event != NULL) {
        event->type = routed_fault_is_radio(sched->latched_fault) ?
            BLE_MESH_SCHED_EVENT_RADIO_FAULT :
            BLE_MESH_SCHED_EVENT_SERVICE_FAULT;
        event->fault = sched->latched_fault;
        if (sched->relay_last_tx_valid != 0u) {
            event->tx_token = (ble_mesh_tx_token_t)sched->last_relay_tx_ms;
            event->tx_requested_channel_mask =
                (uint8_t)(sched->last_relay_tx_ms >> 16);
            event->tx_completed_channel_mask =
                (uint8_t)(sched->last_relay_tx_ms >> 24);
        }
    }
    sched->routed_started = BLE_MESH_SCHED_ROUTED_FAULT_REPORTED;
    return 1;
}

static int routed_listen_current(ble_mesh_scheduler_t *sched)
{
    ble_radio_op_result_t result;

    if (!routed_radio_api_available()) {
        routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_INTERNAL_STATE,
                            BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
        return 0;
    }

    result = ble_radio_try_listen_once((UINT)current_channel(sched),
                                       (UINT)sched->timers->radio_state_timeout_ms);
    if (result != BLE_RADIO_OP_OK) {
        routed_latch_fault(sched, routed_fault_from_radio(result),
                            BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
        return 0;
    }
    sched->rx_started = 1u;
    return 1;
}

static void routed_start_rx(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    if (routed_listen_current(sched)) {
        sched->hop_at_ms = now_ms + sched->dwell_ms;
    }
}

static int routed_restore_rx_or_hop(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    if (time_reached(now_ms, sched->hop_at_ms)) {
        sched->rx_channel_index = (uint8_t)((sched->rx_channel_index + 1u) % 3u);
        if (!routed_listen_current(sched)) {
            return 0;
        }
        sched->hop_at_ms = now_ms + sched->dwell_ms;
        return 1;
    }
    return routed_listen_current(sched);
}

static int routed_hop_rx_channel(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    sched->rx_channel_index = (uint8_t)((sched->rx_channel_index + 1u) % 3u);
    if (!routed_listen_current(sched)) {
        return 0;
    }
    sched->hop_at_ms = now_ms + sched->dwell_ms;
    return 1;
}

static int routed_find_promoted_token(const ble_mesh_scheduler_t *sched,
                                      ble_mesh_tx_token_t *token_out)
{
    uint8_t index;
    ble_mesh_tx_token_t token = BLE_MESH_TX_TOKEN_NONE;
    uint8_t custody_count = 0u;

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry =
            &sched->routed_tx_queue.entries[index];

        if (entry->occupied == 0u || entry->item.service_class !=
            BLE_MESH_TX_SERVICE_CUSTODY_DATA) {
            continue;
        }
        if (entry->item.token == BLE_MESH_TX_TOKEN_NONE ||
            custody_count != 0u) {
            return -1;
        }
        token = entry->item.token;
        custody_count++;
    }
    *token_out = token;
    return 0;
}

static int routed_validate_rx_pdu(const UB *pdu, UINT pdu_len, UINT *adv_len_out)
{
    UINT adv_len;

    if (pdu == NULL || pdu_len < 8u || pdu[0] != 0x42u ||
        pdu[1] < 6u || pdu[1] > 37u || pdu_len != (UINT)pdu[1] + 2u) {
        return 0;
    }
    adv_len = (UINT)pdu[1] - 6u;
    if (adv_len > BLE_MESH_SCHED_ADV_DATA_MAX) {
        return 0;
    }
    *adv_len_out = adv_len;
    return 1;
}

static void routed_copy_rx_event(const ble_mesh_scheduler_t *sched,
                                 const UB *pdu, UINT adv_len, UINT rssi_dbm,
                                 ble_mesh_sched_event_t *event)
{
    event->type = BLE_MESH_SCHED_EVENT_RX_ADV;
    event->channel = current_channel(sched);
    event->rssi_magnitude_db = (uint8_t)(rssi_dbm & 0xFFu);
    event->adv_len = (uint8_t)adv_len;
    copy_bytes(event->adv_addr, &pdu[2], sizeof(event->adv_addr));
    if (adv_len > 0u) {
        copy_bytes(event->adv_data, &pdu[8], adv_len);
    }
}

void ble_mesh_scheduler_init(ble_mesh_scheduler_t *sched, uint32_t now_ms,
                             const uint8_t local_adva[6])
{
    if (sched == NULL) {
        return;
    }

    clear_bytes(sched, sizeof(*sched));
    sched->routed_started = BLE_MESH_SCHED_ROUTED_INVALID;
    if (!local_adva_is_valid(local_adva) ||
        !tron_timer_config_is_valid(&tron_timer_config) ||
        !routed_queue_api_available()) {
        return;
    }

    sched->timers = &tron_timer_config;
    sched->dwell_ms = tron_timer_config.scheduler_dwell_ms;
    sched->relay_rate_limit_ms = tron_timer_config.scheduler_relay_spacing_ms;
    sched->hop_at_ms = now_ms + sched->dwell_ms;
    copy_bytes(sched->local_adva, local_adva, sizeof(sched->local_adva));
    sched->local_adva_valid = 1u;
    ble_mesh_tx_queue_init(&sched->routed_tx_queue);
    sched->routed_started = BLE_MESH_SCHED_ROUTED_HEALTHY;
}

int ble_mesh_scheduler_copy_local_adva(const ble_mesh_scheduler_t *sched,
                                       uint8_t out_adva[6])
{
    if (sched == NULL || out_adva == NULL || sched->local_adva_valid == 0u) {
        return 0;
    }
    copy_bytes(out_adva, sched->local_adva, sizeof(sched->local_adva));
    return 1;
}

ble_mesh_sched_enqueue_result_t ble_mesh_scheduler_enqueue_ex(
    ble_mesh_scheduler_t *sched, const ble_mesh_tx_item_t *item)
{
    ble_mesh_sched_enqueue_result_t result;
    ble_mesh_tx_enqueue_result_t queue_result;
    ble_mesh_tx_token_t promoted_token;

    clear_bytes(&result, sizeof(result));
    result.status = BLE_MESH_SCHED_ENQUEUE_INVALID;
    if (sched == NULL || item == NULL ||
        sched->routed_started != BLE_MESH_SCHED_ROUTED_HEALTHY ||
        sched->latched_fault != BLE_MESH_SCHED_FAULT_NONE ||
        !routed_queue_api_available()) {
        return result;
    }
    if (routed_find_promoted_token(sched, &promoted_token) != 0) {
        routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_QUEUE_CORRUPT,
                            BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
        return result;
    }
    if (item->service_class == BLE_MESH_TX_SERVICE_CUSTODY_DATA &&
        promoted_token != BLE_MESH_TX_TOKEN_NONE) {
        return result;
    }

    queue_result = ble_mesh_tx_queue_enqueue(&sched->routed_tx_queue, item);
    result.status = (ble_mesh_sched_enqueue_status_t)queue_result.status;
    result.accepted_token = queue_result.accepted_token;
    result.evicted_token = queue_result.evicted_token;
    if (queue_result.status == BLE_MESH_TX_ENQUEUE_FULL) {
        sched->counters.queue_drop++;
    }
    return result;
}

static int routed_poll(ble_mesh_scheduler_t *sched, uint32_t now_ms,
                       ble_mesh_sched_event_t *event)
{
    UB pdu[BLE_RX_MAX];
    UINT pdu_len = 0u;
    UINT rssi_dbm = 0u;
    UINT adv_len;
    ble_radio_op_result_t radio_result;
    ble_mesh_tx_selection_t selection;
    ble_mesh_tx_token_t promoted_token;
    ble_mesh_tx_select_status_t select_status;
    ble_radio_tx_result_t tx_result;

    if (sched->routed_started == BLE_MESH_SCHED_ROUTED_FAULT_PENDING) {
        return routed_emit_fault(sched, event);
    }
    if (sched->routed_started != BLE_MESH_SCHED_ROUTED_HEALTHY) {
        return 0;
    }
    if (!routed_radio_api_available() || !routed_queue_api_available()) {
        routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_INTERNAL_STATE,
                            BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
        return routed_emit_fault(sched, event);
    }

    if (sched->rx_started == 0u) {
        routed_start_rx(sched, now_ms);
        if (sched->routed_started != BLE_MESH_SCHED_ROUTED_HEALTHY) {
            return routed_emit_fault(sched, event);
        }
    }

    if (routed_find_promoted_token(sched, &promoted_token) != 0) {
        routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_QUEUE_CORRUPT,
                            BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
        return routed_emit_fault(sched, event);
    }
    clear_bytes(&selection, sizeof(selection));
    select_status = ble_mesh_tx_queue_select_due(&sched->routed_tx_queue,
                                                 now_ms, promoted_token,
                                                 &selection);
    if (select_status == BLE_MESH_TX_SELECT_INVALID) {
        routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_QUEUE_CORRUPT,
                            BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
        return routed_emit_fault(sched, event);
    }
    if (select_status == BLE_MESH_TX_SELECT_OK) {
        tx_result = ble_radio_try_advertise_channels(
            selection.item.adv_data, selection.item.adv_len, sched->local_adva,
            selection.item.channel_mask, (UINT)sched->timers->radio_state_timeout_ms);
        sched->rx_started = 0u;
        if (!ble_mesh_tx_queue_complete(&sched->routed_tx_queue, selection.index,
                                        tx_result.completed_channel_mask,
                                        promoted_token)) {
            routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_QUEUE_CORRUPT,
                                BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
            return routed_emit_fault(sched, event);
        }
        sched->custody_bypass_count =
            sched->routed_tx_queue.custody_bypass_count;

        event->type = tx_result.completed_channel_mask == 0u ?
            BLE_MESH_SCHED_EVENT_TX_FAILED : BLE_MESH_SCHED_EVENT_TX_DONE;
        event->tx_token = selection.item.token;
        event->tx_requested_channel_mask =
            tx_result.requested_channel_mask != 0u ?
            tx_result.requested_channel_mask : selection.item.channel_mask;
        event->tx_completed_channel_mask = tx_result.completed_channel_mask;
        event->fault = tx_result.fault == BLE_RADIO_OP_OK ?
            BLE_MESH_SCHED_FAULT_NONE : routed_fault_from_radio(tx_result.fault);
        if (tx_result.completed_channel_mask != 0u) {
            sched->counters.tx_ok++;
        }
        if (tx_result.fault != BLE_RADIO_OP_OK) {
            routed_latch_fault(sched, event->fault, event->tx_token,
                                event->tx_requested_channel_mask,
                                event->tx_completed_channel_mask, 1);
            return 1;
        }
        if (!routed_restore_rx_or_hop(sched, now_ms)) {
            event->fault = sched->latched_fault;
            routed_pack_fault_diagnostic(sched, event->tx_token,
                                         event->tx_requested_channel_mask,
                                         event->tx_completed_channel_mask, 1);
            return 1;
        }
        return 1;
    }

    radio_result = ble_radio_try_poll_snapshot(
        pdu, &pdu_len, &rssi_dbm, (UINT)sched->timers->radio_state_timeout_ms);
    if (radio_result == BLE_RADIO_OP_NO_EVENT) {
        if (time_reached(now_ms, sched->hop_at_ms) &&
            !routed_hop_rx_channel(sched, now_ms)) {
            return routed_emit_fault(sched, event);
        }
        return 0;
    }
    if (radio_result != BLE_RADIO_OP_OK) {
        sched->rx_started = 0u;
        if (radio_result == BLE_RADIO_OP_CRC_DROP) {
            sched->counters.rx_crc_or_empty++;
            if (!routed_restore_rx_or_hop(sched, now_ms)) {
                return routed_emit_fault(sched, event);
            }
            return 0;
        }
        routed_latch_fault(sched, routed_fault_from_radio(radio_result),
                            BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
        return routed_emit_fault(sched, event);
    }

    sched->rx_started = 0u;
    if (!routed_validate_rx_pdu(pdu, pdu_len, &adv_len)) {
        sched->counters.rx_crc_or_empty++;
        if (!routed_restore_rx_or_hop(sched, now_ms)) {
            return routed_emit_fault(sched, event);
        }
        return 0;
    }

    routed_copy_rx_event(sched, pdu, adv_len, rssi_dbm, event);
    sched->counters.rx_ok++;
    (void)routed_restore_rx_or_hop(sched, now_ms);
    return 1;
}

int ble_mesh_scheduler_poll(ble_mesh_scheduler_t *sched, uint32_t now_ms,
                            ble_mesh_sched_event_t *event)
{
    if (event != NULL) {
        clear_bytes(event, sizeof(*event));
        event->type = BLE_MESH_SCHED_EVENT_NONE;
    }
    if (sched == NULL) {
        return 0;
    }
    if (sched->routed_started == BLE_MESH_SCHED_ROUTED_LEGACY) {
        return ble_mesh_scheduler_poll_legacy(sched, now_ms, event);
    }
    return routed_poll(sched, now_ms, event);
}

const ble_mesh_sched_counters_t *ble_mesh_scheduler_counters(const ble_mesh_scheduler_t *sched)
{
    if (sched == NULL) {
        return NULL;
    }
    return &sched->counters;
}
