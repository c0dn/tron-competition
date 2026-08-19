#include "routed_benchmark_uart_tx.h"

#include <stdint.h>
#include <string.h>

#define UART0_BASE 0x40002000u
#define UART_TASKS_STOPRX 0x004u
#define UART_TASKS_STARTTX 0x008u
#define UART_TASKS_STOPTX 0x00cu
#define UART_EVENTS_ENDTX 0x120u
#define UART_INTENSET 0x304u
#define UART_INTENCLR 0x308u
#define UART_ENABLE 0x500u
#define UART_TXD_PTR 0x544u
#define UART_TXD_MAXCNT 0x548u
#define UART_TXD_AMOUNT 0x54cu
#define UART_INT_ENDTX 0x00000100u

static uint32_t registers[0x600u / sizeof(uint32_t)];
static uint32_t writes[64];
static uint32_t write_values[64];
static uint32_t write_count;
static uint32_t irq_depth;
static uint32_t disabled_int;
static uint32_t cleared_int;
static uint32_t enabled_int;
static int enabled_priority;
static int def_result;
static routed_benchmark_uart_tx_host_dint_t installed_definition;
static uint8_t captured[ROUTED_BENCHMARK_UART_TX_CAPACITY * 2u];
static uint32_t captured_count;
static int failures;

static void expect(int condition)
{
    if (!condition) {
        failures++;
    }
}

static void reset_host(void)
{
    memset(registers, 0, sizeof(registers));
    memset(writes, 0, sizeof(writes));
    memset(write_values, 0, sizeof(write_values));
    write_count = 0u;
    irq_depth = 0u;
    disabled_int = UINT32_MAX;
    cleared_int = UINT32_MAX;
    enabled_int = UINT32_MAX;
    enabled_priority = -1;
    def_result = 0;
    memset(&installed_definition, 0, sizeof(installed_definition));
    memset(captured, 0, sizeof(captured));
    captured_count = 0u;
    routed_benchmark_uart_tx_host_reset();
}

uint32_t routed_benchmark_uart_tx_host_in_w(uint32_t address)
{
    return registers[(address - UART0_BASE) / sizeof(uint32_t)];
}

void routed_benchmark_uart_tx_host_out_w(uint32_t address, uint32_t value)
{
    uint32_t index = (address - UART0_BASE) / sizeof(uint32_t);

    if (write_count < sizeof(writes) / sizeof(writes[0])) {
        writes[write_count] = address;
        write_values[write_count] = value;
        write_count++;
    }
    registers[index] = value;
}

int routed_benchmark_uart_tx_host_def_int(
    uint32_t intno, const routed_benchmark_uart_tx_host_dint_t *definition)
{
    expect(intno == UART0_BASE);
    if (definition != NULL) {
        installed_definition = *definition;
    }
    return def_result;
}

void routed_benchmark_uart_tx_host_enable_int(uint32_t intno, int priority)
{
    enabled_int = intno;
    enabled_priority = priority;
}

void routed_benchmark_uart_tx_host_disable_int(uint32_t intno)
{
    disabled_int = intno;
}

void routed_benchmark_uart_tx_host_clear_int(uint32_t intno)
{
    cleared_int = intno;
}

uint32_t routed_benchmark_uart_tx_host_irq_save(void)
{
    irq_depth++;
    return irq_depth;
}

void routed_benchmark_uart_tx_host_irq_restore(uint32_t state)
{
    expect(state == irq_depth);
    expect(irq_depth != 0u);
    irq_depth--;
}

void routed_benchmark_uart_tx_host_capture(const uint8_t *bytes, uint16_t length)
{
    expect(bytes != NULL || length == 0u);
    expect(captured_count + length <= sizeof(captured));
    if (bytes != NULL && captured_count + length <= sizeof(captured)) {
        memcpy(&captured[captured_count], bytes, length);
        captured_count += length;
    }
}

static void complete_active(void)
{
    registers[UART_EVENTS_ENDTX / sizeof(uint32_t)] = 1u;
    registers[UART_TXD_AMOUNT / sizeof(uint32_t)] =
        registers[UART_TXD_MAXCNT / sizeof(uint32_t)];
    routed_benchmark_uart_tx_interrupt(UART0_BASE);
}

static uint32_t start_count(void)
{
    uint32_t index;
    uint32_t result = 0u;

    for (index = 0u; index < write_count; index++) {
        if (writes[index] == UART0_BASE + UART_TASKS_STARTTX) {
            result++;
        }
    }
    return result;
}

static void test_init_failure_and_success(void)
{
    reset_host();
    def_result = -1;
    expect(!routed_benchmark_uart_tx_init());
    expect(!routed_benchmark_uart_tx_ready());
    expect(disabled_int == UART0_BASE && cleared_int == UART0_BASE);
    expect(enabled_int == UINT32_MAX);
    expect(write_count == 1u);

    reset_host();
    expect(routed_benchmark_uart_tx_init());
    expect(routed_benchmark_uart_tx_ready());
    expect(installed_definition.inthdr == routed_benchmark_uart_tx_interrupt);
    expect(enabled_int == UART0_BASE && enabled_priority == 6);
    expect(registers[UART_ENABLE / sizeof(uint32_t)] == 8u);
    expect(registers[UART_INTENSET / sizeof(uint32_t)] == UART_INT_ENDTX);
    expect(write_count > 7u);
    expect(writes[1] == UART0_BASE + UART_TASKS_STOPTX);
    expect(writes[2] == UART0_BASE + UART_TASKS_STOPRX);
    expect(registers[UART_TXD_PTR / sizeof(uint32_t)] == 0u);
}

static void test_record_commit_active_append_and_canonical_empty(void)
{
    routed_benchmark_uart_tx_snapshot_t snapshot;
    static const uint8_t first[] = "abc\n";
    static const uint8_t second[] = "d\n";
    uint32_t starts = start_count();

    expect(routed_benchmark_uart_tx_settled());
    expect(routed_benchmark_uart_tx_enqueue(first, 1u));
    expect(routed_benchmark_uart_tx_enqueue(first + 1u, 1u));
    expect(routed_benchmark_uart_tx_enqueue(first + 2u, 1u));
    expect(start_count() == starts);
    expect(captured_count == 0u);
    expect(registers[UART_TXD_MAXCNT / sizeof(uint32_t)] == 0u);
    routed_benchmark_uart_tx_snapshot(&snapshot);
    expect(snapshot.pending_bytes == 3u && snapshot.high_water_bytes == 3u);
    expect(!routed_benchmark_uart_tx_settled());
    expect(routed_benchmark_uart_tx_enqueue(first + 3u, 1u));
    expect(start_count() == starts + 1u);
    expect(registers[UART_TXD_MAXCNT / sizeof(uint32_t)] == 4u);
    expect(captured_count == sizeof(first) - 1u &&
           memcmp(captured, first, sizeof(first) - 1u) == 0);
    expect(routed_benchmark_uart_tx_enqueue(second, sizeof(second) - 1u));
    expect(start_count() == starts + 1u);
    routed_benchmark_uart_tx_snapshot(&snapshot);
    expect(snapshot.pending_bytes == 6u && snapshot.high_water_bytes == 6u);
    complete_active();
    expect(start_count() == starts + 2u);
    expect(registers[UART_TXD_MAXCNT / sizeof(uint32_t)] == 2u);
    expect(captured_count == sizeof(first) + sizeof(second) - 2u &&
           memcmp(captured, "abc\nd\n", 6u) == 0);
    complete_active();
    expect(routed_benchmark_uart_tx_settled());
    routed_benchmark_uart_tx_snapshot(&snapshot);
    expect(snapshot.pending_bytes == 0u);
    expect(routed_benchmark_uart_tx_has_headroom());
}

static void test_wrap_and_overflow_retraction(void)
{
    routed_benchmark_uart_tx_snapshot_t snapshot;
    static uint8_t position[ROUTED_BENCHMARK_UART_TX_CAPACITY - 2u];
    static const uint8_t wraps[] = "WXYZ\n";
    static uint8_t ready[ROUTED_BENCHMARK_UART_TX_CAPACITY - 3u];
    static const uint8_t partial[] = "abc";
    static const uint8_t rejected[] = "d\n";
    static const uint8_t valid[] = "ok\n";
    uint32_t starts;

    reset_host();
    expect(routed_benchmark_uart_tx_init());
    position[sizeof(position) - 1u] = (uint8_t)'\n';
    expect(routed_benchmark_uart_tx_enqueue(position, sizeof(position)));
    complete_active();
    captured_count = 0u;
    expect(routed_benchmark_uart_tx_enqueue(wraps, sizeof(wraps) - 1u));
    starts = start_count();
    expect(registers[UART_TXD_MAXCNT / sizeof(uint32_t)] == 2u);
    complete_active();
    expect(start_count() == starts + 1u);
    expect(registers[UART_TXD_MAXCNT / sizeof(uint32_t)] == 3u);
    expect(captured_count == sizeof(wraps) - 1u &&
           memcmp(captured, wraps, sizeof(wraps) - 1u) == 0);
    complete_active();
    expect(start_count() == starts + 1u);
    expect(routed_benchmark_uart_tx_settled());

    reset_host();
    expect(routed_benchmark_uart_tx_init());
    ready[sizeof(ready) - 1u] = (uint8_t)'\n';
    expect(routed_benchmark_uart_tx_enqueue(ready, sizeof(ready)));
    expect(routed_benchmark_uart_tx_enqueue(partial, sizeof(partial) - 1u));
    expect(routed_benchmark_uart_tx_enqueue(rejected, sizeof(rejected) - 1u));
    expect(!routed_benchmark_uart_tx_has_headroom());
    routed_benchmark_uart_tx_snapshot(&snapshot);
    expect(snapshot.pending_bytes == sizeof(ready));
    expect(snapshot.dropped_bytes == 5u && snapshot.dropped_records == 1u);
    complete_active();
    expect(routed_benchmark_uart_tx_settled());
    captured_count = 0u;
    expect(routed_benchmark_uart_tx_enqueue(valid, sizeof(valid) - 1u));
    expect(registers[UART_TXD_MAXCNT / sizeof(uint32_t)] == sizeof(valid) - 1u);
    expect(captured_count == sizeof(valid) - 1u &&
           memcmp(captured, valid, sizeof(valid) - 1u) == 0);
}

static void test_faults_and_saturation(void)
{
    routed_benchmark_uart_tx_snapshot_t snapshot;
    static const uint8_t line[] = "q\n";
    static uint8_t full[ROUTED_BENCHMARK_UART_TX_CAPACITY];
    static const uint8_t overflow[] = "x\n";

    reset_host();
    expect(routed_benchmark_uart_tx_init());
    expect(routed_benchmark_uart_tx_enqueue(line, sizeof(line) - 1u));
    registers[UART_EVENTS_ENDTX / sizeof(uint32_t)] = 1u;
    registers[UART_TXD_AMOUNT / sizeof(uint32_t)] = 0u;
    routed_benchmark_uart_tx_interrupt(UART0_BASE);
    routed_benchmark_uart_tx_snapshot(&snapshot);
    expect(snapshot.pending_bytes == 2u && snapshot.transport_faults == 1u);
    expect(registers[UART_INTENCLR / sizeof(uint32_t)] == UART_INT_ENDTX);
    expect(!routed_benchmark_uart_tx_has_headroom());
    expect(!routed_benchmark_uart_tx_settled());

    reset_host();
    expect(routed_benchmark_uart_tx_init());
    routed_benchmark_uart_tx_interrupt(UART0_BASE);
    routed_benchmark_uart_tx_snapshot(&snapshot);
    expect(snapshot.transport_faults == 1u);

    reset_host();
    expect(routed_benchmark_uart_tx_init());
    routed_benchmark_uart_tx_host_force_counters(UINT32_MAX, UINT32_MAX,
                                                  UINT32_MAX);
    full[sizeof(full) - 1u] = (uint8_t)'\n';
    expect(routed_benchmark_uart_tx_enqueue(full, sizeof(full)));
    expect(routed_benchmark_uart_tx_enqueue(overflow, sizeof(overflow) - 1u));
    routed_benchmark_uart_tx_interrupt(UART0_BASE);
    routed_benchmark_uart_tx_snapshot(&snapshot);
    expect(snapshot.dropped_bytes == UINT32_MAX);
    expect(snapshot.dropped_records == UINT32_MAX);
    expect(snapshot.transport_faults == UINT32_MAX);
}

int main(void)
{
    test_init_failure_and_success();
    reset_host();
    expect(routed_benchmark_uart_tx_init());
    test_record_commit_active_append_and_canonical_empty();
    test_wrap_and_overflow_retraction();
    test_faults_and_saturation();
    return failures == 0 ? 0 : 1;
}
