/*
 * tx_adapter.c - bounded interleaved wearable event transmitter.
 */

#include "tx_adapter.h"
#include "schema.h"

/* Valid for deadlines less than half the 32-bit millisecond range away. */
static BOOL time_reached(UW now, UW target)
{
    return (BOOL)((UW)(now - target) < 0x80000000u);
}

UB tx_adapter_severity(UB event_type)
{
    switch (event_type) {
    case MIND_EVT_FALL_AND_SHOUT:    return 4;
    case MIND_EVT_CONFIRMED_FALL:    return 3;
    case MIND_EVT_POSSIBLE_FALL:     return 2;
    case MIND_EVT_POSSIBLE_DISTRESS: return 1;
    default:                         return 0;
    }
}

void tx_adapter_init(tx_adapter_t *adapter,
                     UW budget_ms, UW interval_ms, UB min_tx,
                     UW hb_interval_ms, UW now_ms)
{
    UB index;

    if (adapter == 0) {
        return;
    }

    for (index = 0; index < TX_ADAPTER_MAX_EVENTS; index++) {
        adapter->slots[index].used = 0;
        adapter->slots[index].tx_count = 0;
        adapter->slots[index].deadline_ms = 0;
        adapter->slots[index].hard_deadline_ms = 0;
        adapter->slots[index].next_tx_ms = 0;
    }

    adapter->budget_ms = budget_ms;
    adapter->interval_ms = interval_ms;
    adapter->hb_interval_ms = hb_interval_ms;
    adapter->min_tx = min_tx;
    adapter->next_hb_ms = now_ms + hb_interval_ms;
    adapter->rr = 0;
    adapter->admitted = 0;
    adapter->dropped = 0;
    adapter->evicted = 0;
    adapter->event_tx = 0;
    adapter->hb_tx = 0;
}

static void retire_expired(tx_adapter_t *adapter, UW now_ms)
{
    UB index;

    for (index = 0; index < TX_ADAPTER_MAX_EVENTS; index++) {
        tx_slot_t *slot = &adapter->slots[index];

        if (!slot->used) {
            continue;
        }
        if (time_reached(now_ms, slot->hard_deadline_ms) ||
            (time_reached(now_ms, slot->deadline_ms) &&
             slot->tx_count >= adapter->min_tx)) {
            slot->used = 0;
        }
    }
}

static void slot_start(tx_adapter_t *adapter, tx_slot_t *slot,
                       const incident_state_t *incident, UW now_ms)
{
    slot->inc = *incident;
    slot->deadline_ms = now_ms + adapter->budget_ms;
    slot->hard_deadline_ms = now_ms + (adapter->budget_ms * 2u);
    slot->next_tx_ms = now_ms;
    slot->tx_count = 0;
    slot->used = 1;
}

BOOL tx_adapter_admit(tx_adapter_t *adapter, const incident_state_t *incident,
                      UW now_ms)
{
    UB index;
    UB new_severity;
    UB victim = TX_ADAPTER_MAX_EVENTS;
    UB victim_severity = 0xFFu;

    if (adapter == 0 || incident == 0) {
        return FALSE;
    }

    retire_expired(adapter, now_ms);

    for (index = 0; index < TX_ADAPTER_MAX_EVENTS; index++) {
        if (!adapter->slots[index].used) {
            slot_start(adapter, &adapter->slots[index], incident, now_ms);
            adapter->admitted++;
            return TRUE;
        }
    }

    new_severity = tx_adapter_severity(incident->event_type);
    for (index = 0; index < TX_ADAPTER_MAX_EVENTS; index++) {
        UB severity = tx_adapter_severity(adapter->slots[index].inc.event_type);

        if (severity < victim_severity) {
            victim_severity = severity;
            victim = index;
        }
    }

    if (victim < TX_ADAPTER_MAX_EVENTS && new_severity > victim_severity) {
        slot_start(adapter, &adapter->slots[victim], incident, now_ms);
        adapter->admitted++;
        adapter->evicted++;
        return TRUE;
    }

    adapter->dropped++;
    return FALSE;
}

tx_adapter_send_t tx_adapter_next(tx_adapter_t *adapter, UW now_ms,
                                  incident_state_t *out)
{
    UB offset;

    if (adapter == 0 || out == 0) {
        return TX_ADAPTER_SEND_NONE;
    }

    retire_expired(adapter, now_ms);

    /* A due heartbeat has priority so an event spray cannot starve liveness. */
    if (time_reached(now_ms, adapter->next_hb_ms)) {
        adapter->next_hb_ms = now_ms + adapter->hb_interval_ms;
        adapter->hb_tx++;
        return TX_ADAPTER_SEND_HEARTBEAT;
    }

    for (offset = 0; offset < TX_ADAPTER_MAX_EVENTS; offset++) {
        UB index = (UB)((adapter->rr + offset) % TX_ADAPTER_MAX_EVENTS);
        tx_slot_t *slot = &adapter->slots[index];

        if (!slot->used || !time_reached(now_ms, slot->next_tx_ms)) {
            continue;
        }

        *out = slot->inc;
        slot->tx_count++;
        slot->next_tx_ms = now_ms + adapter->interval_ms;
        adapter->rr = (UB)((index + 1u) % TX_ADAPTER_MAX_EVENTS);
        adapter->event_tx++;
        return TX_ADAPTER_SEND_EVENT;
    }

    return TX_ADAPTER_SEND_NONE;
}

UB tx_adapter_active(const tx_adapter_t *adapter)
{
    UB index;
    UB count = 0;

    if (adapter == 0) {
        return 0;
    }
    for (index = 0; index < TX_ADAPTER_MAX_EVENTS; index++) {
        if (adapter->slots[index].used) {
            count++;
        }
    }
    return count;
}
