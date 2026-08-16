/*
 * tx_adapter.h - absolute-deadline transmit policy for wearable events.
 *
 * The caller supplies time and performs the radio emit, keeping this policy
 * host-testable and independent from the kernel.
 */

#ifndef TX_ADAPTER_H
#define TX_ADAPTER_H

#include "incident.h"

#define TX_ADAPTER_MAX_EVENTS   4

typedef struct {
    incident_state_t inc;
    UW deadline_ms;
    UW hard_deadline_ms;
    UW next_tx_ms;
    UB tx_count;
    UB used;
} tx_slot_t;

typedef struct {
    tx_slot_t slots[TX_ADAPTER_MAX_EVENTS];

    UW budget_ms;
    UW interval_ms;
    UW hb_interval_ms;
    UB min_tx;

    UW next_hb_ms;
    UB rr;

    UW admitted;
    UW dropped;
    UW evicted;
    UW event_tx;
    UW hb_tx;
} tx_adapter_t;

typedef enum {
    TX_ADAPTER_SEND_NONE = 0,
    TX_ADAPTER_SEND_EVENT,
    TX_ADAPTER_SEND_HEARTBEAT
} tx_adapter_send_t;

void tx_adapter_init(tx_adapter_t *adapter,
                     UW budget_ms, UW interval_ms, UB min_tx,
                     UW hb_interval_ms, UW now_ms);
BOOL tx_adapter_admit(tx_adapter_t *adapter, const incident_state_t *incident,
                      UW now_ms);
tx_adapter_send_t tx_adapter_next(tx_adapter_t *adapter, UW now_ms,
                                  incident_state_t *out);
UB tx_adapter_active(const tx_adapter_t *adapter);
UB tx_adapter_severity(UB event_type);

#endif /* TX_ADAPTER_H */
