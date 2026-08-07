/*
 * tx_adapter.h - transmit-side policy for the wearable.
 *
 * WHAT THIS REPLACES
 * ------------------
 * The old advertise_task held one incident and a countdown:
 *
 *     if (burst_remaining > 0) { send incident; burst_remaining--; }
 *     else                     { send heartbeat; }
 *
 * That has three defects. A second event overwrote the first and reset the
 * countdown, so the "possible fall -> confirmed fall" upgrade silently
 * truncated the first event's retransmissions. Heartbeats were the else
 * branch, so the device went quiet for the whole burst. And the pacing came
 * from a chain of tk_dly_tsk() calls, which quantise to CNF_TIMER_PERIOD
 * (10 ms in this app) and may overshoot by a full tick, so the error
 * accumulated across a burst.
 *
 * WHY RETRANSMIT AT ALL
 * ---------------------
 * The mesh receiver time-slices a single radio: ble_mesh_scheduler dwells
 * ~50 ms per advertising channel and drops RX entirely during its TX windows.
 * A one-shot advertisement can land wholly inside a deaf period. Re-sending
 * the same event across a time budget gives the receiver several independent
 * chances to be listening.
 *
 * INVARIANT
 * ---------
 * Every copy of one event carries the same identity. Relays suppress the
 * copies they have already seen, so the spray costs one relay per event, not
 * one per copy, while a node that missed the first copy can still catch a
 * later one. Retransmission and duplicate suppression only compose because
 * the identity is stable - if a copy were given a fresh id it would be
 * relayed again as a new event.
 *
 * All scheduling is against absolute deadlines read from the caller's clock,
 * never cumulative sleeps, so tick overshoot cannot accumulate. The module
 * has no kernel dependency: the caller supplies now_ms and performs the emit.
 */

#ifndef TX_ADAPTER_H
#define TX_ADAPTER_H

#include "incident.h"

/* Concurrent events held live at once. Four covers the realistic worst case
 * (a fall upgrading while a distress event is still spraying) and bounds both
 * airtime and the struct. */
#define TX_ADAPTER_MAX_EVENTS   4

typedef struct {
    incident_state_t inc;
    UW  deadline_ms;        /* absolute: admitted_at + budget            */
    UW  hard_deadline_ms;   /* absolute: cap when min_tx is unreachable  */
    UW  next_tx_ms;         /* absolute: earliest next copy              */
    UB  tx_count;
    UB  used;
} tx_slot_t;

typedef struct {
    tx_slot_t slots[TX_ADAPTER_MAX_EVENTS];

    UW  budget_ms;          /* how long one event keeps being re-sent    */
    UW  interval_ms;        /* spacing between copies of the same event  */
    UW  hb_interval_ms;
    UB  min_tx;             /* floor on copies before a slot may retire  */

    UW  next_hb_ms;
    UB  rr;                 /* round-robin cursor, so slots share airtime */

    /* Diagnostics, surfaced over serial. */
    UW  admitted;
    UW  dropped;
    UW  evicted;
    UW  event_tx;
    UW  hb_tx;
} tx_adapter_t;

typedef enum {
    TX_ADAPTER_SEND_NONE = 0,
    TX_ADAPTER_SEND_EVENT,
    TX_ADAPTER_SEND_HEARTBEAT
} tx_adapter_send_t;

void tx_adapter_init(tx_adapter_t *a,
                     UW budget_ms, UW interval_ms, UB min_tx,
                     UW hb_interval_ms, UW now_ms);

/* Take ownership of a freshly fused incident. When every slot is busy the
 * least severe one is evicted, but only if the newcomer outranks it; a
 * distress event cannot displace a confirmed fall. Returns TRUE if admitted. */
BOOL tx_adapter_admit(tx_adapter_t *a, const incident_state_t *inc, UW now_ms);

/* Decide what to put on air now. On TX_ADAPTER_SEND_EVENT, *out holds the copy
 * to send. On TX_ADAPTER_SEND_HEARTBEAT the caller fills in the live SVM and
 * its own sequence; only the timing decision belongs here. */
tx_adapter_send_t tx_adapter_next(tx_adapter_t *a, UW now_ms,
                                  incident_state_t *out);

/* Live slots, for logging and tests. */
UB tx_adapter_active(const tx_adapter_t *a);

/* Severity ranking used for eviction. Exposed for tests. */
UB tx_adapter_severity(UB event_type);

#endif /* TX_ADAPTER_H */
