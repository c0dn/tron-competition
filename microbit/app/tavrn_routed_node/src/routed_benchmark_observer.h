#ifndef ROUTED_BENCHMARK_OBSERVER_H
#define ROUTED_BENCHMARK_OBSERVER_H

/* This is a benchmark-only, copied observer above the frozen radio and
 * scheduler boundary.  Every control counter is a partial trace proxy: it
 * does not assert energy, PHY airtime, radio wake duration, or all FULL
 * control TX completion for a particular wire type. */

#include "routed_cycle.h"

#define ROUTED_BENCHMARK_WIRE_TYPE_LIMIT 0x13u

typedef struct routed_benchmark_observer {
    uint32_t rx_control_proxy_count[ROUTED_BENCHMARK_WIRE_TYPE_LIMIT];
    uint32_t rx_control_proxy_bytes[ROUTED_BENCHMARK_WIRE_TYPE_LIMIT];
    uint32_t aodv_dispatch_control_enqueue_proxy_count[
        ROUTED_BENCHMARK_WIRE_TYPE_LIMIT];
    uint32_t aodv_dispatch_control_enqueue_proxy_bytes[
        ROUTED_BENCHMARK_WIRE_TYPE_LIMIT];
    uint32_t scheduler_rx_adv_proxy;
    uint32_t scheduler_tx_done_proxy;
    uint32_t scheduler_tx_failed_proxy;
    uint32_t scheduler_fault_proxy;
    uint32_t scheduler_rx_channel_proxy[3];
    uint32_t trace_over_budget;
    uint32_t trace_fault_latched;
} routed_benchmark_observer_t;

void routed_benchmark_observer_init(routed_benchmark_observer_t *observer);
void routed_benchmark_observer_observe_trace(
    routed_benchmark_observer_t *observer, const routed_cycle_trace_t *trace);

#endif /* ROUTED_BENCHMARK_OBSERVER_H */
