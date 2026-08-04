#include "ble_mesh_scheduler.h"

static const uint8_t ble_mesh_sched_channels[3] = { 37u, 38u, 39u };

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

static void listen_once_on_current_channel(ble_mesh_scheduler_t *sched)
{
    ble_radio_listen_once((UINT)current_channel(sched));
    sched->rx_started = 1u;
}

static void start_rx_on_current_channel(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    listen_once_on_current_channel(sched);
    sched->hop_at_ms = now_ms + sched->dwell_ms;
}

static void restore_rx_or_hop(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    if (time_reached(now_ms, sched->hop_at_ms)) {
        sched->rx_channel_index = (uint8_t)((sched->rx_channel_index + 1u) % 3u);
        start_rx_on_current_channel(sched, now_ms);
        return;
    }

    listen_once_on_current_channel(sched);
}

static void ensure_rx_started(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    if (sched->rx_started == 0u) {
        start_rx_on_current_channel(sched, now_ms);
    }
}

static void hop_rx_channel(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    sched->rx_channel_index = (uint8_t)((sched->rx_channel_index + 1u) % 3u);
    start_rx_on_current_channel(sched, now_ms);
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

static void transmit_pos(ble_mesh_scheduler_t *sched, uint8_t pos, uint32_t now_ms)
{
    ble_mesh_sched_legacy_tx_item_t item;

    item = sched->legacy_tx_queue[queue_index(sched, pos)];
    queue_remove_pos(sched, pos);

    ble_radio_idle();
    sched->rx_started = 0u;

    ble_radio_advertise_channels((const UB *)item.adv_data,
                                 (UINT)item.adv_len,
                                 NULL,
                                 (UINT)item.channel_mask);

    sched->counters.tx_ok++;
    if (item.kind == BLE_MESH_SCHED_TX_RELAY) {
        sched->last_relay_tx_ms = now_ms;
        sched->relay_last_tx_valid = 1u;
    }

    restore_rx_or_hop(sched, now_ms);
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

void ble_mesh_scheduler_init_legacy(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    if (sched == NULL) {
        return;
    }

    clear_bytes(sched, sizeof(*sched));
    sched->dwell_ms = BLE_MESH_SCHED_DWELL_MS_DEFAULT;
    sched->relay_rate_limit_ms = BLE_MESH_SCHED_RELAY_TX_INTERVAL_MS;
    sched->hop_at_ms = now_ms + sched->dwell_ms;
}

void ble_mesh_scheduler_start_rx(ble_mesh_scheduler_t *sched, uint32_t now_ms)
{
    if (sched == NULL) {
        return;
    }
    start_rx_on_current_channel(sched, now_ms);
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

int ble_mesh_scheduler_poll(ble_mesh_scheduler_t *sched,
                            uint32_t now_ms,
                            ble_mesh_sched_event_t *event)
{
    UB pdu[BLE_RX_MAX];
    UINT pdu_len = 0u;
    UINT rssi_dbm = 0u;
    int snapshot;
    int tx_pos;

    if (event != NULL) {
        clear_bytes(event, sizeof(*event));
        event->type = BLE_MESH_SCHED_EVENT_NONE;
    }
    if (sched == NULL) {
        return 0;
    }

    ensure_rx_started(sched, now_ms);

    /* Service one due TX before consuming another RX snapshot. Otherwise a
       continuously busy advertising channel can indefinitely starve queued
       own and relay traffic. Each TX restores RX before returning. */
    tx_pos = select_tx_pos(sched, now_ms);
    if (tx_pos >= 0) {
        transmit_pos(sched, (uint8_t)tx_pos, now_ms);
        return 0;
    }

    snapshot = ble_radio_poll_snapshot(pdu, &pdu_len, &rssi_dbm);
    if (snapshot != 0) {
        sched->rx_started = 0u;
        if (snapshot > 0 && event == NULL && validate_rx_pdu(pdu, pdu_len, NULL)) {
            sched->counters.rx_ok++;
            restore_rx_or_hop(sched, now_ms);
            return 1;
        }
        if (snapshot > 0 && copy_rx_event(sched, pdu, pdu_len, rssi_dbm, event)) {
            sched->counters.rx_ok++;
            restore_rx_or_hop(sched, now_ms);
            return 1;
        }

        sched->counters.rx_crc_or_empty++;
        restore_rx_or_hop(sched, now_ms);
        return 0;
    }

    if (time_reached(now_ms, sched->hop_at_ms)) {
        hop_rx_channel(sched, now_ms);
    }

    return 0;
}

const ble_mesh_sched_counters_t *ble_mesh_scheduler_counters(const ble_mesh_scheduler_t *sched)
{
    if (sched == NULL) {
        return NULL;
    }
    return &sched->counters;
}
