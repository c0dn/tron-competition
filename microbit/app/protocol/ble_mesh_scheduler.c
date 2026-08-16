#include "ble_mesh_scheduler.h"

#if defined(__GNUC__)
/* Legacy firmware still links this scheduler before the routed queue is added
 * to its source manifest. Undefined weak routed seams keep that wire-v1 build
 * runnable; a routed initializer refuses to start until every seam is linked. */
#pragma weak ble_radio_try_listen_once
#pragma weak ble_radio_try_poll_snapshot
#pragma weak ble_radio_try_advertise_channels
#pragma weak ble_radio_try_advertise_sweeps
#pragma weak ble_mesh_tx_queue_init
#pragma weak ble_mesh_tx_queue_enqueue
#pragma weak ble_mesh_tx_queue_select_due
#pragma weak ble_mesh_tx_queue_collect_due_expiry
#pragma weak ble_mesh_tx_queue_retire
#endif

static const uint8_t ble_mesh_sched_channels[3] = { 37u, 38u, 39u };

#define BLE_MESH_SCHED_ROUTED_LEGACY          0u
#define BLE_MESH_SCHED_ROUTED_HEALTHY         1u
#define BLE_MESH_SCHED_ROUTED_FAULT_PENDING   2u
#define BLE_MESH_SCHED_ROUTED_FAULT_REPORTED  3u
#define BLE_MESH_SCHED_ROUTED_INVALID         4u

#define BLE_MESH_SCHED_BUDGET_WINDOW_MS       1000u
#define BLE_MESH_SCHED_BUDGET_GENERAL_MAX_BU  32u
#define BLE_MESH_SCHED_BUDGET_CRITICAL_MAX_BU 8u

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
static void routed_latch_fault(ble_mesh_scheduler_t *sched,
                               ble_mesh_sched_fault_t fault,
                               ble_mesh_tx_token_t token,
                               uint8_t requested_mask,
                               uint8_t completed_mask,
                               int has_tx_diagnostic);
static void routed_clear_custody_hold(ble_mesh_scheduler_t *sched);

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
        event->tx_requested_sweep_count = tx_result.requested_sweep_count;
        event->tx_attempted_sweep_count = tx_result.attempted_sweep_count;
        copy_bytes(event->tx_completed_channel_masks,
                   tx_result.completed_channel_masks,
                   sizeof(event->tx_completed_channel_masks));
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
           ble_radio_try_advertise_sweeps != NULL;
}

static int routed_queue_api_available(void)
{
    return ble_mesh_tx_queue_init != NULL &&
           ble_mesh_tx_queue_enqueue != NULL &&
           ble_mesh_tx_queue_select_due != NULL &&
           ble_mesh_tx_queue_collect_due_expiry != NULL &&
           ble_mesh_tx_queue_retire != NULL;
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

    /* A selected TX can fault while RX is restored. Its event evidence is
     * staged before the restore attempt, so retain that selected diagnostic
     * rather than replacing it with the follow-up RX failure's empty masks. */
    if (has_tx_diagnostic || sched->relay_last_tx_valid == 0u) {
        routed_pack_fault_diagnostic(sched, token, requested_mask, completed_mask,
                                      has_tx_diagnostic);
    }
    sched->latched_fault = fault;
    sched->routed_started = BLE_MESH_SCHED_ROUTED_FAULT_PENDING;
    sched->rx_started = 0u;
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

static uint8_t routed_budget_ring_index(const ble_mesh_scheduler_t *sched,
                                        uint8_t position)
{
    return (uint8_t)((sched->budget_head + position) %
                     BLE_MESH_SCHED_BUDGET_RING_CAPACITY);
}

static int routed_budget_state_is_consistent(const ble_mesh_scheduler_t *sched)
{
    uint8_t position;
    uint8_t general_bu = 0u;
    uint8_t critical_bu = 0u;

    if (sched == NULL || sched->budget_head >= BLE_MESH_SCHED_BUDGET_RING_CAPACITY ||
        sched->budget_count > BLE_MESH_SCHED_BUDGET_RING_CAPACITY ||
        sched->budget_general_bu > BLE_MESH_SCHED_BUDGET_GENERAL_MAX_BU ||
        sched->budget_critical_bu > BLE_MESH_SCHED_BUDGET_RING_CAPACITY ||
        sched->custody_hold > 1u ||
        (sched->custody_hold == 0u &&
         sched->custody_hold_token != BLE_MESH_TX_TOKEN_NONE) ||
        (sched->custody_hold != 0u &&
         sched->custody_hold_token == BLE_MESH_TX_TOKEN_NONE)) {
        return 0;
    }

    for (position = 0u; position < sched->budget_count; position++) {
        uint8_t budget_class =
            sched->budget_classes[routed_budget_ring_index(sched, position)];

        if (budget_class == BLE_MESH_TX_BUDGET_GENERAL) {
            general_bu++;
        } else if (budget_class == BLE_MESH_TX_BUDGET_CRITICAL) {
            critical_bu++;
        } else {
            return 0;
        }
    }
    return general_bu == sched->budget_general_bu &&
        critical_bu == sched->budget_critical_bu &&
        (uint8_t)(general_bu + critical_bu) == sched->budget_count;
}

static int routed_budget_charge_is_live(uint32_t now_ms, uint32_t charged_at_ms)
{
    return !time_reached(now_ms, charged_at_ms + BLE_MESH_SCHED_BUDGET_WINDOW_MS);
}

static int routed_budget_purge(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    if (!routed_budget_state_is_consistent(sched)) {
        return 0;
    }

    while (sched->budget_count != 0u &&
           !routed_budget_charge_is_live(
               now_ms, sched->budget_timestamps[sched->budget_head])) {
        uint8_t budget_class = sched->budget_classes[sched->budget_head];

        if (budget_class == BLE_MESH_TX_BUDGET_GENERAL) {
            if (sched->budget_general_bu == 0u) {
                return 0;
            }
            sched->budget_general_bu--;
        } else if (budget_class == BLE_MESH_TX_BUDGET_CRITICAL) {
            if (sched->budget_critical_bu == 0u) {
                return 0;
            }
            sched->budget_critical_bu--;
        } else {
            return 0;
        }
        sched->budget_head = (uint8_t)((sched->budget_head + 1u) %
                                       BLE_MESH_SCHED_BUDGET_RING_CAPACITY);
        sched->budget_count--;
    }
    if (sched->budget_count == 0u) {
        sched->budget_head = 0u;
    }
    return routed_budget_state_is_consistent(sched);
}

/* Returns one when the complete operation was reserved, zero when it does not
 * fit, and -1 only for an internal fixed-state inconsistency. */
static int routed_budget_reserve(ble_mesh_scheduler_t *sched, uint32_t now_ms,
                                 const ble_mesh_tx_item_t *item,
                                 int reserve_critical_lane)
{
    uint8_t cost;
    uint8_t index;

    if (sched == NULL || item == NULL || !routed_budget_purge(sched, now_ms)) {
        return -1;
    }
    cost = (uint8_t)item->sweep_count;
    if (cost != BLE_MESH_TX_SWEEP_COUNT_ONE &&
        cost != BLE_MESH_TX_SWEEP_COUNT_TWO) {
        return -1;
    }
    if (sched->budget_count >
        (uint8_t)(BLE_MESH_SCHED_BUDGET_RING_CAPACITY - cost)) {
        return 0;
    }
    if (item->budget_class == BLE_MESH_TX_BUDGET_GENERAL) {
        if (sched->budget_general_bu >
            (uint8_t)(BLE_MESH_SCHED_BUDGET_GENERAL_MAX_BU - cost)) {
            return 0;
        }
    } else if (item->budget_class == BLE_MESH_TX_BUDGET_CRITICAL) {
        if (reserve_critical_lane != 0 && sched->budget_critical_bu >
            (uint8_t)(BLE_MESH_SCHED_BUDGET_CRITICAL_MAX_BU - cost)) {
            return 0;
        }
    } else {
        return -1;
    }

    for (index = 0u; index < cost; index++) {
        uint8_t ring_index = routed_budget_ring_index(sched, sched->budget_count);

        sched->budget_timestamps[ring_index] = now_ms;
        sched->budget_classes[ring_index] = (uint8_t)item->budget_class;
        sched->budget_count++;
    }
    if (item->budget_class == BLE_MESH_TX_BUDGET_GENERAL) {
        sched->budget_general_bu = (uint8_t)(sched->budget_general_bu + cost);
    } else {
        sched->budget_critical_bu = (uint8_t)(sched->budget_critical_bu + cost);
    }
    return routed_budget_state_is_consistent(sched) ? 1 : -1;
}

static void routed_clear_custody_hold(ble_mesh_scheduler_t *sched)
{
    sched->custody_hold = 0u;
    sched->custody_hold_token = BLE_MESH_TX_TOKEN_NONE;
}

static void routed_enter_custody_hold(ble_mesh_scheduler_t *sched,
                                      ble_mesh_tx_token_t token)
{
    if (token != BLE_MESH_TX_TOKEN_NONE) {
        sched->custody_hold = 1u;
        sched->custody_hold_token = token;
    }
}

/* A hold is established only from a due item. A waiting entry cannot normally
 * occur afterwards, but keeping it distinct avoids treating a malformed
 * external mutation as a terminal condition. */
static int routed_held_custody_status(const ble_mesh_scheduler_t *sched,
                                      uint32_t now_ms,
                                      ble_mesh_tx_selection_t *selection_out)
{
    uint8_t index;

    if (sched == NULL || selection_out == NULL || sched->custody_hold == 0u ||
        sched->custody_hold_token == BLE_MESH_TX_TOKEN_NONE) {
        return -1;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry =
            &sched->routed_tx_queue.entries[index];

        if (entry->occupied == 0u ||
            entry->item.token != sched->custody_hold_token) {
            continue;
        }
        if (entry->item.service_class != BLE_MESH_TX_SERVICE_CUSTODY_DATA) {
            return -1;
        }
        if (time_reached(now_ms, entry->item.expiry_ms)) {
            return 0;
        }
        if (!time_reached(now_ms, entry->item.not_before_ms)) {
            return 2;
        }
        clear_bytes(selection_out, sizeof(*selection_out));
        selection_out->status = BLE_MESH_TX_SELECT_OK;
        selection_out->index = index;
        selection_out->item = entry->item;
        return 1;
    }
    return 0;
}

static int routed_select_held_critical(
    const ble_mesh_scheduler_t *sched, uint32_t now_ms,
    const ble_mesh_tx_item_t *held_custody,
    ble_mesh_tx_selection_t *selection_out)
{
    uint8_t index;
    uint8_t selected_index = BLE_MESH_TX_QUEUE_CAPACITY;

    if (sched == NULL || held_custody == NULL || selection_out == NULL) {
        return -1;
    }
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry =
            &sched->routed_tx_queue.entries[index];
        const ble_mesh_tx_queue_entry_t *selected;

        if (entry->occupied == 0u ||
            entry->item.budget_class != BLE_MESH_TX_BUDGET_CRITICAL ||
            entry->item.priority <= held_custody->priority ||
            time_reached(now_ms, entry->item.expiry_ms) ||
            !time_reached(now_ms, entry->item.not_before_ms)) {
            continue;
        }
        if (selected_index == BLE_MESH_TX_QUEUE_CAPACITY) {
            selected_index = index;
            continue;
        }
        selected = &sched->routed_tx_queue.entries[selected_index];
        if (entry->item.priority > selected->item.priority ||
            (entry->item.priority == selected->item.priority &&
             (time_is_earlier(entry->item.expiry_ms, selected->item.expiry_ms) ||
              (entry->item.expiry_ms == selected->item.expiry_ms &&
               ordinal_is_older(entry->ordinal, selected->ordinal))))) {
            selected_index = index;
        }
    }
    if (selected_index == BLE_MESH_TX_QUEUE_CAPACITY) {
        return 0;
    }
    clear_bytes(selection_out, sizeof(*selection_out));
    selection_out->status = BLE_MESH_TX_SELECT_OK;
    selection_out->index = selected_index;
    selection_out->item = sched->routed_tx_queue.entries[selected_index].item;
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

static int routed_send_reserved_selection(ble_mesh_scheduler_t *sched,
                                          const ble_mesh_tx_selection_t *selection,
                                          uint32_t now_ms,
                                          ble_mesh_sched_event_t *event)
{
    ble_radio_tx_result_t tx_result;
    uint8_t requested_channel_mask;
    ble_mesh_sched_fault_t fault;

    tx_result = ble_radio_try_advertise_sweeps(
        selection->item.adv_data, selection->item.adv_len, sched->local_adva,
        selection->item.channel_mask, (uint8_t)selection->item.sweep_count,
        (UINT)sched->timers->radio_state_timeout_ms);
    sched->rx_started = 0u;
    if (ble_mesh_tx_queue_retire(
            &sched->routed_tx_queue, selection->index,
            tx_result.completed_channel_mask == 0u ?
                BLE_MESH_TX_TERMINAL_TX_FAILED : BLE_MESH_TX_TERMINAL_TX_DONE,
            &tx_result) != BLE_MESH_TX_RETIRE_OK) {
        routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_QUEUE_CORRUPT,
                            BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
        return routed_emit_fault(sched, event);
    }
    sched->custody_bypass_count = sched->routed_tx_queue.custody_bypass_count;

    requested_channel_mask = tx_result.requested_channel_mask;
    fault = tx_result.fault == BLE_RADIO_OP_OK ? BLE_MESH_SCHED_FAULT_NONE :
        routed_fault_from_radio(tx_result.fault);
    if (event != NULL) {
        event->type = tx_result.completed_channel_mask == 0u ?
            BLE_MESH_SCHED_EVENT_TX_FAILED : BLE_MESH_SCHED_EVENT_TX_DONE;
        event->tx_token = selection->item.token;
        event->tx_requested_channel_mask = requested_channel_mask;
        event->tx_completed_channel_mask = tx_result.completed_channel_mask;
        event->tx_requested_sweep_count = tx_result.requested_sweep_count;
        event->tx_attempted_sweep_count = tx_result.attempted_sweep_count;
        copy_bytes(event->tx_completed_channel_masks,
                   tx_result.completed_channel_masks,
                   sizeof(event->tx_completed_channel_masks));
        event->fault = fault;
    }
    if (tx_result.completed_channel_mask != 0u) {
        sched->counters.tx_ok++;
    }
    if (tx_result.fault != BLE_RADIO_OP_OK) {
        routed_latch_fault(sched, fault, selection->item.token,
                            requested_channel_mask,
                            tx_result.completed_channel_mask, 1);
        return 1;
    }
    /* Preserve this selected TX if restoring RX is what fails next. */
    routed_pack_fault_diagnostic(sched, selection->item.token,
                                 requested_channel_mask,
                                 tx_result.completed_channel_mask, 1);
    if (!routed_restore_rx_or_hop(sched, now_ms)) {
        if (event != NULL) {
            event->fault = sched->latched_fault;
        }
        return 1;
    }
    routed_pack_fault_diagnostic(sched, BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
    return 1;
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
    ble_mesh_tx_expiry_result_t expiry;
    ble_mesh_tx_token_t promoted_token;
    ble_mesh_tx_select_status_t select_status;
    int budget_result;
    int held_status;

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

    clear_bytes(&expiry, sizeof(expiry));
    if (ble_mesh_tx_queue_collect_due_expiry(
            &sched->routed_tx_queue, now_ms, BLE_MESH_TX_QUEUE_CAPACITY,
            &expiry) != BLE_MESH_TX_EXPIRY_OK) {
        routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_QUEUE_CORRUPT,
                            BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
        return routed_emit_fault(sched, event);
    }
    sched->counters.tx_expired_anonymous += expiry.anonymous_purged_count;
    sched->custody_bypass_count = sched->routed_tx_queue.custody_bypass_count;
    if (expiry.tracked_due != 0u) {
        if (event == NULL) {
            goto poll_rx;
        }
        if (ble_mesh_tx_queue_retire(
                &sched->routed_tx_queue, expiry.tracked_index,
                BLE_MESH_TX_TERMINAL_EXPIRED, NULL) != BLE_MESH_TX_RETIRE_OK) {
            routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_QUEUE_CORRUPT,
                                BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
            return routed_emit_fault(sched, event);
        }
        if (sched->custody_hold != 0u &&
            sched->custody_hold_token == expiry.tracked_item.token) {
            routed_clear_custody_hold(sched);
        }
        sched->custody_bypass_count = sched->routed_tx_queue.custody_bypass_count;
        sched->counters.tx_expired_tracked++;
        event->type = BLE_MESH_SCHED_EVENT_TX_EXPIRED;
        event->tx_token = expiry.tracked_item.token;
        event->tx_requested_channel_mask = expiry.tracked_item.channel_mask;
        event->tx_requested_sweep_count = (uint8_t)expiry.tracked_item.sweep_count;
        return 1;
    }

    if (!routed_budget_purge(sched, now_ms)) {
        routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_INTERNAL_STATE,
                            BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
        return routed_emit_fault(sched, event);
    }

    if (sched->custody_hold != 0u) {
        if (routed_find_promoted_token(sched, &promoted_token) != 0) {
            routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_QUEUE_CORRUPT,
                                BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
            return routed_emit_fault(sched, event);
        }
        if (promoted_token != sched->custody_hold_token) {
            routed_clear_custody_hold(sched);
        } else {
            held_status = routed_held_custody_status(sched, now_ms, &selection);
            if (held_status < 0) {
                routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_QUEUE_CORRUPT,
                                    BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
                return routed_emit_fault(sched, event);
            }
            if (held_status == 0) {
                /* The exact custody token expired or was terminally removed. */
                routed_clear_custody_hold(sched);
            } else if (held_status == 2) {
                goto poll_rx;
            } else {
                budget_result = routed_budget_reserve(sched, now_ms,
                                                       &selection.item, 0);
                if (budget_result < 0) {
                    routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_INTERNAL_STATE,
                                        BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
                    return routed_emit_fault(sched, event);
                }
                if (budget_result > 0) {
                    routed_clear_custody_hold(sched);
                    return routed_send_reserved_selection(sched, &selection,
                                                          now_ms, event);
                }

                held_status = routed_select_held_critical(
                    sched, now_ms, &selection.item, &selection);
                if (held_status < 0) {
                    routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_INTERNAL_STATE,
                                        BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
                    return routed_emit_fault(sched, event);
                }
                if (held_status == 0) {
                    goto poll_rx;
                }
                budget_result = routed_budget_reserve(sched, now_ms,
                                                       &selection.item, 1);
                if (budget_result < 0) {
                    routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_INTERNAL_STATE,
                                        BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
                    return routed_emit_fault(sched, event);
                }
                if (budget_result == 0) {
                    /* A blocked critical selection is never bypassed. */
                    goto poll_rx;
                }
                return routed_send_reserved_selection(sched, &selection,
                                                      now_ms, event);
            }
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
    sched->custody_bypass_count = sched->routed_tx_queue.custody_bypass_count;
    if (select_status == BLE_MESH_TX_SELECT_OK) {
        budget_result = routed_budget_reserve(sched, now_ms, &selection.item, 0);
        if (budget_result < 0) {
            routed_latch_fault(sched, BLE_MESH_SCHED_FAULT_INTERNAL_STATE,
                                BLE_MESH_TX_TOKEN_NONE, 0u, 0u, 0);
            return routed_emit_fault(sched, event);
        }
        if (budget_result > 0) {
            return routed_send_reserved_selection(sched, &selection, now_ms, event);
        }
        if (selection.item.service_class == BLE_MESH_TX_SERVICE_CUSTODY_DATA &&
            selection.item.budget_class == BLE_MESH_TX_BUDGET_GENERAL &&
            selection.item.token == promoted_token &&
            sched->routed_tx_queue.custody_bypass_count >=
                tron_timer_config.scheduler_custody_bypass_max) {
            routed_enter_custody_hold(sched, selection.item.token);
        }
    }

poll_rx:
    if (sched->rx_started == 0u) {
        routed_start_rx(sched, now_ms);
        if (sched->routed_started != BLE_MESH_SCHED_ROUTED_HEALTHY) {
            return routed_emit_fault(sched, event);
        }
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

int ble_mesh_scheduler_get_budget_snapshot(
    const ble_mesh_scheduler_t *sched, uint32_t now_ms,
    ble_mesh_sched_budget_snapshot_t *snapshot_out)
{
    uint8_t position;

    if (snapshot_out == NULL || !routed_budget_state_is_consistent(sched)) {
        return 0;
    }
    clear_bytes(snapshot_out, sizeof(*snapshot_out));
    for (position = 0u; position < sched->budget_count; position++) {
        uint8_t ring_index = routed_budget_ring_index(sched, position);

        if (!routed_budget_charge_is_live(now_ms,
                                          sched->budget_timestamps[ring_index])) {
            continue;
        }
        snapshot_out->live_total_bu++;
        if (sched->budget_classes[ring_index] == BLE_MESH_TX_BUDGET_GENERAL) {
            snapshot_out->live_general_bu++;
        } else {
            snapshot_out->live_critical_bu++;
        }
    }
    snapshot_out->custody_hold = sched->custody_hold;
    return 1;
}

const ble_mesh_sched_counters_t *ble_mesh_scheduler_counters(const ble_mesh_scheduler_t *sched)
{
    if (sched == NULL) {
        return NULL;
    }
    return &sched->counters;
}
