/*
 * tx_adapter.c - transmit-side policy (see tx_adapter.h).
 */

#include "tx_adapter.h"
#include "schema.h"

/*
 * Wrap-safe "has now reached target?".
 *
 * The clock is a 32-bit millisecond counter that wraps roughly every 49 days,
 * and a plain (now >= target) breaks across that wrap: a target just past the
 * wrap is numerically tiny while now is huge. Comparing the unsigned
 * difference against half the range instead treats anything within ~24 days
 * ahead as "not yet" and anything within ~24 days behind as "reached", which
 * is the same idiom fusion.c uses for its coincidence window. Every deadline
 * here is at most a few seconds out, so the ambiguous half-range is never
 * approached.
 */
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

void tx_adapter_init(tx_adapter_t *a,
                     UW budget_ms, UW interval_ms, UB min_tx,
                     UW hb_interval_ms, UW now_ms)
{
    UB i;

    if (a == 0) {
        return;
    }

    for (i = 0; i < TX_ADAPTER_MAX_EVENTS; i++) {
        a->slots[i].used = 0;
        a->slots[i].tx_count = 0;
        a->slots[i].deadline_ms = 0;
        a->slots[i].hard_deadline_ms = 0;
        a->slots[i].next_tx_ms = 0;
    }

    a->budget_ms = budget_ms;
    a->interval_ms = interval_ms;
    a->hb_interval_ms = hb_interval_ms;
    a->min_tx = min_tx;

    a->next_hb_ms = now_ms + hb_interval_ms;
    a->rr = 0;

    a->admitted = 0;
    a->dropped = 0;
    a->evicted = 0;
    a->event_tx = 0;
    a->hb_tx = 0;
}

/* Retire slots that have served their budget. The min_tx floor keeps a slot
 * alive past its deadline when a coarse tick or heavy contention starved it of
 * copies; hard_deadline_ms stops that floor extending the slot forever. */
static void retire_expired(tx_adapter_t *a, UW now_ms)
{
    UB i;

    for (i = 0; i < TX_ADAPTER_MAX_EVENTS; i++) {
        tx_slot_t *s = &a->slots[i];

        if (!s->used) {
            continue;
        }
        if (time_reached(now_ms, s->hard_deadline_ms) ||
            (time_reached(now_ms, s->deadline_ms) && s->tx_count >= a->min_tx)) {
            s->used = 0;
        }
    }
}

static void slot_start(tx_adapter_t *a, tx_slot_t *s,
                       const incident_state_t *inc, UW now_ms)
{
    s->inc = *inc;
    s->deadline_ms = now_ms + a->budget_ms;
    s->hard_deadline_ms = now_ms + (a->budget_ms * 2u);
    s->next_tx_ms = now_ms;     /* first copy goes out immediately */
    s->tx_count = 0;
    s->used = 1;
}

BOOL tx_adapter_admit(tx_adapter_t *a, const incident_state_t *inc, UW now_ms)
{
    UB i;
    UB new_sev;
    UB victim = TX_ADAPTER_MAX_EVENTS;
    UB victim_sev = 0xFFu;

    if (a == 0 || inc == 0) {
        return FALSE;
    }

    retire_expired(a, now_ms);

    for (i = 0; i < TX_ADAPTER_MAX_EVENTS; i++) {
        if (!a->slots[i].used) {
            slot_start(a, &a->slots[i], inc, now_ms);
            a->admitted++;
            return TRUE;
        }
    }

    /* Full: displace the least severe resident, but only for a strictly more
     * severe newcomer. Equal severity does not evict, so a run of same-type
     * events cannot thrash the table and lose every one of them. */
    new_sev = tx_adapter_severity(inc->event_type);

    for (i = 0; i < TX_ADAPTER_MAX_EVENTS; i++) {
        UB sev = tx_adapter_severity(a->slots[i].inc.event_type);

        if (sev < victim_sev) {
            victim_sev = sev;
            victim = i;
        }
    }

    if (victim < TX_ADAPTER_MAX_EVENTS && new_sev > victim_sev) {
        slot_start(a, &a->slots[victim], inc, now_ms);
        a->admitted++;
        a->evicted++;
        return TRUE;
    }

    a->dropped++;
    return FALSE;
}

tx_adapter_send_t tx_adapter_next(tx_adapter_t *a, UW now_ms,
                                  incident_state_t *out)
{
    UB i;

    if (a == 0 || out == 0) {
        return TX_ADAPTER_SEND_NONE;
    }

    retire_expired(a, now_ms);

    /* Heartbeat wins when it is due. It costs one emit slot every
     * hb_interval_ms against events pacing at interval_ms, so events lose at
     * most one opportunity per heartbeat - whereas letting events win would
     * let a busy incident silence the device for the whole burst, which is
     * the starvation this adapter exists to fix. */
    if (time_reached(now_ms, a->next_hb_ms)) {
        a->next_hb_ms = now_ms + a->hb_interval_ms;
        a->hb_tx++;
        return TX_ADAPTER_SEND_HEARTBEAT;
    }

    /* Round-robin so concurrent events interleave instead of the lowest slot
     * index monopolising the air. */
    for (i = 0; i < TX_ADAPTER_MAX_EVENTS; i++) {
        UB idx = (UB)((a->rr + i) % TX_ADAPTER_MAX_EVENTS);
        tx_slot_t *s = &a->slots[idx];

        if (!s->used || !time_reached(now_ms, s->next_tx_ms)) {
            continue;
        }

        *out = s->inc;              /* same identity on every copy */
        s->tx_count++;
        s->next_tx_ms = now_ms + a->interval_ms;
        a->rr = (UB)((idx + 1u) % TX_ADAPTER_MAX_EVENTS);
        a->event_tx++;
        return TX_ADAPTER_SEND_EVENT;
    }

    return TX_ADAPTER_SEND_NONE;
}

UB tx_adapter_active(const tx_adapter_t *a)
{
    UB i;
    UB n = 0;

    if (a == 0) {
        return 0;
    }
    for (i = 0; i < TX_ADAPTER_MAX_EVENTS; i++) {
        if (a->slots[i].used) {
            n++;
        }
    }
    return n;
}
