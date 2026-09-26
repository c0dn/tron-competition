#include "routed_benchmark_observer.h"

#include <string.h>

static void increment_saturating(uint32_t *value)
{
    if (value != NULL && *value != UINT32_MAX) {
        (*value)++;
    }
}

static void add_saturating(uint32_t *value, uint32_t addend)
{
    if (value == NULL || *value == UINT32_MAX) {
        return;
    }
    if (UINT32_MAX - *value < addend) {
        *value = UINT32_MAX;
    } else {
        *value += addend;
    }
}

static int wire_type_is_control(tavrn_wire_type_t type)
{
    return type >= TAVRN_WIRE_E_RREQ && type <= TAVRN_WIRE_E_RREP_ACK;
}

static int action_type_has_control(aodv_action_type_t type)
{
    return type == AODV_ACTION_SEND_RREQ || type == AODV_ACTION_SEND_RREP ||
        type == AODV_ACTION_FORWARD_RREP || type == AODV_ACTION_SEND_RERR ||
        type == AODV_ACTION_SEND_RREP_ACK;
}

static void observe_control(uint32_t *count, uint32_t *bytes,
                            tavrn_wire_type_t type, uint32_t byte_count)
{
    if (!wire_type_is_control(type)) {
        return;
    }
    increment_saturating(&count[(uint8_t)type]);
    add_saturating(&bytes[(uint8_t)type], byte_count);
}

static void observe_channels(uint32_t channels[3], uint8_t channel_mask)
{
    uint8_t index;

    if (channels == NULL) {
        return;
    }
    for (index = 0u; index < 3u; index++) {
        if ((channel_mask & (uint8_t)(1u << index)) != 0u) {
            increment_saturating(&channels[index]);
        }
    }
}

void routed_benchmark_observer_init(routed_benchmark_observer_t *observer)
{
    if (observer != NULL) {
        memset(observer, 0, sizeof(*observer));
    }
}

void routed_benchmark_observer_observe_trace(
    routed_benchmark_observer_t *observer, const routed_cycle_trace_t *trace)
{
    const tavrn_router_scheduler_trace_t *scheduler_trace;
    const tavrn_router_dispatch_trace_t *dispatch_trace;

    if (observer == NULL || trace == NULL) {
        return;
    }
    if (trace->timing.over_budget == ROUTED_CYCLE_BOOLEAN_TRUE) {
        increment_saturating(&observer->trace_over_budget);
    }
    if (trace->fault_latched == ROUTED_CYCLE_BOOLEAN_TRUE) {
        increment_saturating(&observer->trace_fault_latched);
    }
    if (trace->phase == ROUTED_CYCLE_PHASE_SCHEDULER_EVENT) {
        scheduler_trace = &trace->detail.router.detail.scheduler_event;
        if (scheduler_trace->input_event_type == BLE_MESH_SCHED_EVENT_RX_ADV) {
            increment_saturating(&observer->scheduler_rx_adv_proxy);
            observe_channels(observer->scheduler_rx_channel_proxy,
                             scheduler_trace->input_channel);
        } else if (scheduler_trace->input_event_type == BLE_MESH_SCHED_EVENT_TX_DONE) {
            increment_saturating(&observer->scheduler_tx_done_proxy);
        } else if (scheduler_trace->input_event_type == BLE_MESH_SCHED_EVENT_TX_FAILED) {
            increment_saturating(&observer->scheduler_tx_failed_proxy);
        } else if (scheduler_trace->input_event_type == BLE_MESH_SCHED_EVENT_RADIO_FAULT ||
                   scheduler_trace->input_event_type == BLE_MESH_SCHED_EVENT_SERVICE_FAULT) {
            increment_saturating(&observer->scheduler_fault_proxy);
        }
        if (scheduler_trace->rx_control_present == TAVRN_ROUTER_TRACE_PRESENT) {
            observe_control(observer->rx_control_proxy_count,
                            observer->rx_control_proxy_bytes,
                            scheduler_trace->rx_control.control.type,
                            scheduler_trace->rx_control.control.pdu_len);
        }
    } else if (trace->phase == ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH) {
        dispatch_trace = &trace->detail.router.detail.dispatch;
        if (dispatch_trace->action_present == TAVRN_ROUTER_TRACE_PRESENT &&
            dispatch_trace->link_send_present == TAVRN_ROUTER_TRACE_PRESENT &&
            dispatch_trace->link_send_status == TAVRN_LINK_SEND_OK &&
            action_type_has_control(dispatch_trace->action.type)) {
            observe_control(observer->aodv_dispatch_control_enqueue_proxy_count,
                            observer->aodv_dispatch_control_enqueue_proxy_bytes,
                            dispatch_trace->action.detail.control.control.type,
                            dispatch_trace->action.detail.control.control.pdu_len);
        }
    }
}
