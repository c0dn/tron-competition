#ifndef ROUTED_BENCHMARK_UART_TX_H
#define ROUTED_BENCHMARK_UART_TX_H

#include <stdint.h>

#define ROUTED_BENCHMARK_UART_TX_CAPACITY 16384u
#define ROUTED_BENCHMARK_UART_TX_HEADROOM 8192u

typedef struct routed_benchmark_uart_tx_snapshot {
    uint16_t pending_bytes;
    uint16_t high_water_bytes;
    uint32_t dropped_bytes;
    uint32_t dropped_records;
    uint32_t transport_faults;
} routed_benchmark_uart_tx_snapshot_t;

/* Takes over UART0 as UARTE0 only after the interrupt handler is installed.
 * A failure leaves the legacy polling T-monitor UART usable. */
int routed_benchmark_uart_tx_init(void);
int routed_benchmark_uart_tx_ready(void);

/* Queue bytes without waiting for physical transmission.  Bytes become DMA
 * eligible only after LF commits their complete record.  Overflow retracts and
 * discards the current incomplete record, then discards through its LF. */
int routed_benchmark_uart_tx_enqueue(const uint8_t *bytes, uint16_t length);
int routed_benchmark_uart_tx_has_headroom(void);
int routed_benchmark_uart_tx_settled(void);
void routed_benchmark_uart_tx_snapshot(routed_benchmark_uart_tx_snapshot_t *out);

void routed_benchmark_uart_tx_interrupt(uint32_t intno);

#ifdef ROUTED_BENCHMARK_UART_TX_HOST_TEST
typedef struct routed_benchmark_uart_tx_host_dint {
    uint32_t intatr;
    void (*inthdr)(uint32_t);
} routed_benchmark_uart_tx_host_dint_t;

uint32_t routed_benchmark_uart_tx_host_in_w(uint32_t address);
void routed_benchmark_uart_tx_host_out_w(uint32_t address, uint32_t value);
int routed_benchmark_uart_tx_host_def_int(
    uint32_t intno, const routed_benchmark_uart_tx_host_dint_t *definition);
void routed_benchmark_uart_tx_host_enable_int(uint32_t intno, int priority);
void routed_benchmark_uart_tx_host_disable_int(uint32_t intno);
void routed_benchmark_uart_tx_host_clear_int(uint32_t intno);
uint32_t routed_benchmark_uart_tx_host_irq_save(void);
void routed_benchmark_uart_tx_host_irq_restore(uint32_t state);
void routed_benchmark_uart_tx_host_capture(const uint8_t *bytes, uint16_t length);
void routed_benchmark_uart_tx_host_reset(void);
void routed_benchmark_uart_tx_host_force_counters(uint32_t dropped_bytes,
                                                  uint32_t dropped_records,
                                                  uint32_t transport_faults);
#endif

#endif
