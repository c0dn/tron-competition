#include "routed_benchmark_observer.h"

#include <stdio.h>
#include <string.h>

static unsigned int failures;

#define CHECK(expression) \
    do { \
        if (!(expression)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
            failures++; \
        } \
    } while (0)

int main(void)
{
    routed_benchmark_observer_t observer;
    routed_cycle_trace_t trace;

    routed_benchmark_observer_init(&observer);
    memset(&trace, 0, sizeof(trace));
    trace.phase = ROUTED_CYCLE_PHASE_SCHEDULER_EVENT;
    trace.detail.router.detail.scheduler_event.input_event_type =
        BLE_MESH_SCHED_EVENT_RX_ADV;
    trace.detail.router.detail.scheduler_event.input_channel = BLE_MESH_SCHED_CH38;
    trace.detail.router.detail.scheduler_event.rx_control_present =
        TAVRN_ROUTER_TRACE_PRESENT;
    trace.detail.router.detail.scheduler_event.rx_control.control.type =
        TAVRN_WIRE_E_RREQ;
    trace.detail.router.detail.scheduler_event.rx_control.control.pdu_len = 7u;
    routed_benchmark_observer_observe_trace(&observer, &trace);
    CHECK(observer.scheduler_rx_adv_proxy == 1u);
    CHECK(observer.scheduler_rx_channel_proxy[1] == 1u);
    CHECK(observer.rx_control_proxy_count[TAVRN_WIRE_E_RREQ] == 1u);
    CHECK(observer.rx_control_proxy_bytes[TAVRN_WIRE_E_RREQ] == 7u);

    memset(&trace, 0, sizeof(trace));
    trace.phase = ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH;
    trace.detail.router.detail.dispatch.action_present = TAVRN_ROUTER_TRACE_PRESENT;
    trace.detail.router.detail.dispatch.link_send_present = TAVRN_ROUTER_TRACE_PRESENT;
    trace.detail.router.detail.dispatch.link_send_status = TAVRN_LINK_SEND_OK;
    trace.detail.router.detail.dispatch.action.type = AODV_ACTION_SEND_RREP;
    trace.detail.router.detail.dispatch.action.detail.control.control.type =
        TAVRN_WIRE_E_RREP;
    trace.detail.router.detail.dispatch.action.detail.control.control.pdu_len = 9u;
    routed_benchmark_observer_observe_trace(&observer, &trace);
    CHECK(observer.aodv_dispatch_control_enqueue_proxy_count[
              TAVRN_WIRE_E_RREP] == 1u);
    CHECK(observer.aodv_dispatch_control_enqueue_proxy_bytes[
              TAVRN_WIRE_E_RREP] == 9u);

    trace.detail.router.detail.dispatch.action.type = AODV_ACTION_FORWARD_DATA;
    routed_benchmark_observer_observe_trace(&observer, &trace);
    CHECK(observer.aodv_dispatch_control_enqueue_proxy_count[
              TAVRN_WIRE_E_RREP] == 1u);

    trace.phase = ROUTED_CYCLE_PHASE_SCHEDULER_EVENT;
    trace.detail.router.detail.scheduler_event.input_event_type =
        BLE_MESH_SCHED_EVENT_TX_DONE;
    trace.detail.router.detail.scheduler_event.input_channel = BLE_MESH_SCHED_CH37 |
        BLE_MESH_SCHED_CH39;
    trace.timing.over_budget = ROUTED_CYCLE_BOOLEAN_TRUE;
    trace.fault_latched = ROUTED_CYCLE_BOOLEAN_TRUE;
    routed_benchmark_observer_observe_trace(&observer, &trace);
    CHECK(observer.scheduler_tx_done_proxy == 1u);
    CHECK(observer.trace_over_budget == 1u && observer.trace_fault_latched == 1u);

    return failures == 0u ? 0 : 1;
}
