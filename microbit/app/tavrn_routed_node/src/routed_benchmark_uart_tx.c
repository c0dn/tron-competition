#include "routed_benchmark_uart_tx.h"

#include <limits.h>
#include <stddef.h>

#ifdef ROUTED_BENCHMARK_UART_TX_HOST_TEST
typedef uint32_t UINT;
typedef int ER;
typedef void (*FP)(uint32_t);
#define E_OK 0
#define TA_HLNG 1u
#define UART0_BASE 0x40002000u
#define INTNO(base) ((uint32_t)(base))
#define in_w(address) routed_benchmark_uart_tx_host_in_w((address))
#define out_w(address, value) routed_benchmark_uart_tx_host_out_w((address), (value))
#define DisableInt(intno) routed_benchmark_uart_tx_host_disable_int((intno))
#define ClearInt(intno) routed_benchmark_uart_tx_host_clear_int((intno))
#define EnableInt(intno, priority) \
    routed_benchmark_uart_tx_host_enable_int((intno), (priority))
#else
#include <tk/tkernel.h>
#endif

#define ROUTED_UART0_BASE 0x40002000u
#define ROUTED_UART_TASKS_STARTRX 0x000u
#define ROUTED_UART_TASKS_STOPRX 0x004u
#define ROUTED_UART_TASKS_STARTTX 0x008u
#define ROUTED_UART_TASKS_STOPTX 0x00cu
#define ROUTED_UART_EVENTS_ENDTX 0x120u
#define ROUTED_UART_EVENTS_ERROR 0x124u
#define ROUTED_UART_EVENTS_TXSTOPPED 0x158u
#define ROUTED_UART_INTENSET 0x304u
#define ROUTED_UART_INTENCLR 0x308u
#define ROUTED_UART_ENABLE 0x500u
#define ROUTED_UART_TXD_PTR 0x544u
#define ROUTED_UART_TXD_MAXCNT 0x548u
#define ROUTED_UART_TXD_AMOUNT 0x54cu
#define ROUTED_UART_INT_ENDTX 0x00000100u
#define ROUTED_UART_ENABLE_DISABLED 0u
#define ROUTED_UART_ENABLE_UARTE 8u
#define ROUTED_UART_INTERRUPT_PRIORITY 6

#if defined(__GNUC__)
#define ROUTED_UART_TX_ALIGNMENT __attribute__((aligned(4)))
#else
#define ROUTED_UART_TX_ALIGNMENT
#endif

typedef struct routed_benchmark_uart_tx_state {
    uint16_t head;
    uint16_t tail;
    uint16_t count;
    uint16_t ready_count;
    uint16_t open_record_count;
    uint16_t active_length;
    uint16_t high_water_bytes;
    uint8_t dropping_record;
    uint8_t ready;
    uint32_t dropped_bytes;
    uint32_t dropped_records;
    uint32_t transport_faults;
} routed_benchmark_uart_tx_state_t;

static uint8_t routed_benchmark_uart_tx_ring[ROUTED_BENCHMARK_UART_TX_CAPACITY]
    ROUTED_UART_TX_ALIGNMENT;
static volatile routed_benchmark_uart_tx_state_t routed_benchmark_uart_tx_state;

static uint32_t routed_benchmark_uart_tx_irq_save(void)
{
#ifdef ROUTED_BENCHMARK_UART_TX_HOST_TEST
    return routed_benchmark_uart_tx_host_irq_save();
#else
    UW state;

    DI(state);
    return (uint32_t)state;
#endif
}

static void routed_benchmark_uart_tx_irq_restore(uint32_t state)
{
#ifdef ROUTED_BENCHMARK_UART_TX_HOST_TEST
    routed_benchmark_uart_tx_host_irq_restore(state);
#else
    EI((UW)state);
#endif
}

static void routed_benchmark_uart_tx_compiler_barrier(void)
{
#if defined(__GNUC__)
    __asm__ volatile ("" ::: "memory");
#endif
}

static void routed_benchmark_uart_tx_publish_barrier(void)
{
    routed_benchmark_uart_tx_compiler_barrier();
#if defined(__arm__) || defined(__thumb__)
    __asm__ volatile ("dmb" ::: "memory");
#endif
    routed_benchmark_uart_tx_compiler_barrier();
}

static void routed_benchmark_uart_tx_increment(volatile uint32_t *value)
{
    if (value != NULL && *value != UINT32_MAX) {
        (*value)++;
    }
}

static void routed_benchmark_uart_tx_add_saturating(volatile uint32_t *value,
                                                    uint16_t addend)
{
    if (value != NULL && *value != UINT32_MAX) {
        if (UINT32_MAX - *value < addend) {
            *value = UINT32_MAX;
        } else {
            *value += addend;
        }
    }
}

static int routed_benchmark_uart_tx_invariants_hold(void)
{
    const volatile routed_benchmark_uart_tx_state_t *state =
        &routed_benchmark_uart_tx_state;
    uint32_t expected_tail;

    if (state->head >= ROUTED_BENCHMARK_UART_TX_CAPACITY ||
        state->tail >= ROUTED_BENCHMARK_UART_TX_CAPACITY ||
        state->count > ROUTED_BENCHMARK_UART_TX_CAPACITY ||
        state->ready > 1u || state->dropping_record > 1u) {
        return 0;
    }
    expected_tail = (uint32_t)state->head + state->count;
    if (expected_tail >= ROUTED_BENCHMARK_UART_TX_CAPACITY) {
        expected_tail -= ROUTED_BENCHMARK_UART_TX_CAPACITY;
    }

    return state->tail == expected_tail &&
        state->active_length <= state->ready_count &&
        state->ready_count <= state->count &&
        state->open_record_count == state->count - state->ready_count &&
        (state->active_length == 0u ||
         state->active_length <=
             ROUTED_BENCHMARK_UART_TX_CAPACITY - state->head) &&
        (state->count != 0u ||
         (state->ready_count == 0u && state->open_record_count == 0u &&
          state->active_length == 0u && state->head == state->tail));
}

static void routed_benchmark_uart_tx_latch_fault(void)
{
    out_w(ROUTED_UART0_BASE + ROUTED_UART_EVENTS_ENDTX, 0u);
    out_w(ROUTED_UART0_BASE + ROUTED_UART_INTENCLR, ROUTED_UART_INT_ENDTX);
    if (routed_benchmark_uart_tx_state.transport_faults == 0u) {
        routed_benchmark_uart_tx_increment(
            &routed_benchmark_uart_tx_state.transport_faults);
    }
}

/* The current active segment starts at head and is physically contiguous.  It
 * contains only committed LF-terminated records; an open record is always
 * after ready_count and can therefore never alter EasyDMA-visible memory. */
static void routed_benchmark_uart_tx_start_contiguous(void)
{
    uint16_t contiguous;

    if (!routed_benchmark_uart_tx_invariants_hold() ||
        routed_benchmark_uart_tx_state.ready_count == 0u ||
        routed_benchmark_uart_tx_state.active_length != 0u) {
        routed_benchmark_uart_tx_latch_fault();
        return;
    }
    contiguous = routed_benchmark_uart_tx_state.ready_count;
    if (contiguous > ROUTED_BENCHMARK_UART_TX_CAPACITY -
        routed_benchmark_uart_tx_state.head) {
        contiguous = (uint16_t)(ROUTED_BENCHMARK_UART_TX_CAPACITY -
                                routed_benchmark_uart_tx_state.head);
    }
    routed_benchmark_uart_tx_state.active_length = contiguous;
    routed_benchmark_uart_tx_publish_barrier();
#ifdef ROUTED_BENCHMARK_UART_TX_HOST_TEST
    routed_benchmark_uart_tx_host_capture(
        &routed_benchmark_uart_tx_ring[routed_benchmark_uart_tx_state.head],
        contiguous);
#endif
    out_w(ROUTED_UART0_BASE + ROUTED_UART_TXD_PTR,
          (uint32_t)(uintptr_t)&routed_benchmark_uart_tx_ring[
              routed_benchmark_uart_tx_state.head]);
    out_w(ROUTED_UART0_BASE + ROUTED_UART_TXD_MAXCNT, contiguous);
    /* Publish the immutable RAM contents and both EasyDMA descriptor words
     * before STARTTX is observable to the peripheral. */
    routed_benchmark_uart_tx_publish_barrier();
    out_w(ROUTED_UART0_BASE + ROUTED_UART_TASKS_STARTTX, 1u);
}

int routed_benchmark_uart_tx_init(void)
{
#ifdef ROUTED_BENCHMARK_UART_TX_HOST_TEST
    routed_benchmark_uart_tx_host_dint_t definition;
#else
    T_DINT definition;
#endif
    uint32_t state;

    state = routed_benchmark_uart_tx_irq_save();
    if (routed_benchmark_uart_tx_state.ready != 0u) {
        routed_benchmark_uart_tx_irq_restore(state);
        return 1;
    }
    routed_benchmark_uart_tx_irq_restore(state);

    /* Legacy tm_snd_dat polls TXDRDY directly, so disabling this NVIC source
     * and UART interrupt masks leaves the polling UART usable while tk_def_int
     * is the only remaining fallible step. */
    DisableInt(INTNO(UART0_BASE));
    ClearInt(INTNO(UART0_BASE));
    out_w(ROUTED_UART0_BASE + ROUTED_UART_INTENCLR, UINT32_MAX);
    definition.intatr = TA_HLNG;
    definition.inthdr = (FP)routed_benchmark_uart_tx_interrupt;
#ifdef ROUTED_BENCHMARK_UART_TX_HOST_TEST
    if (routed_benchmark_uart_tx_host_def_int(INTNO(UART0_BASE), &definition) != E_OK) {
#else
    if (tk_def_int(INTNO(UART0_BASE), &definition) != E_OK) {
#endif
        /* UART0 was never disabled, so __real_tm_snd_dat remains the legacy
         * polling implementation on this fail-closed path. */
        return 0;
    }

    state = routed_benchmark_uart_tx_irq_save();
    routed_benchmark_uart_tx_state.head = 0u;
    routed_benchmark_uart_tx_state.tail = 0u;
    routed_benchmark_uart_tx_state.count = 0u;
    routed_benchmark_uart_tx_state.ready_count = 0u;
    routed_benchmark_uart_tx_state.open_record_count = 0u;
    routed_benchmark_uart_tx_state.active_length = 0u;
    routed_benchmark_uart_tx_state.high_water_bytes = 0u;
    routed_benchmark_uart_tx_state.dropping_record = 0u;
    routed_benchmark_uart_tx_state.dropped_bytes = 0u;
    routed_benchmark_uart_tx_state.dropped_records = 0u;
    routed_benchmark_uart_tx_state.transport_faults = 0u;
    routed_benchmark_uart_tx_irq_restore(state);

    /* tm_snd_dat completes its final byte by waiting for TXDRDY before this
     * initializer runs.  Therefore no UARTE TXSTOPPED wait is required when
     * transitioning the legacy UART peripheral to UARTE mode. */
    out_w(ROUTED_UART0_BASE + ROUTED_UART_TASKS_STOPTX, 1u);
    out_w(ROUTED_UART0_BASE + ROUTED_UART_TASKS_STOPRX, 1u);
    out_w(ROUTED_UART0_BASE + ROUTED_UART_ENABLE, ROUTED_UART_ENABLE_DISABLED);
    out_w(ROUTED_UART0_BASE + ROUTED_UART_EVENTS_ENDTX, 0u);
    out_w(ROUTED_UART0_BASE + ROUTED_UART_EVENTS_ERROR, 0u);
    out_w(ROUTED_UART0_BASE + ROUTED_UART_EVENTS_TXSTOPPED, 0u);
    out_w(ROUTED_UART0_BASE + ROUTED_UART_ENABLE, ROUTED_UART_ENABLE_UARTE);
    out_w(ROUTED_UART0_BASE + ROUTED_UART_INTENCLR, UINT32_MAX);
    ClearInt(INTNO(UART0_BASE));
    out_w(ROUTED_UART0_BASE + ROUTED_UART_INTENSET, ROUTED_UART_INT_ENDTX);
    EnableInt(INTNO(UART0_BASE), ROUTED_UART_INTERRUPT_PRIORITY);

    state = routed_benchmark_uart_tx_irq_save();
    routed_benchmark_uart_tx_state.ready = 1u;
    routed_benchmark_uart_tx_irq_restore(state);
    return 1;
}

int routed_benchmark_uart_tx_ready(void)
{
    return routed_benchmark_uart_tx_state.ready != 0u;
}

int routed_benchmark_uart_tx_enqueue(const uint8_t *bytes, uint16_t length)
{
    uint16_t index;
    uint32_t state;

    if (bytes == NULL && length != 0u) {
        return 0;
    }
    state = routed_benchmark_uart_tx_irq_save();
    if (routed_benchmark_uart_tx_state.ready == 0u) {
        routed_benchmark_uart_tx_irq_restore(state);
        return 0;
    }
    if (!routed_benchmark_uart_tx_invariants_hold()) {
        routed_benchmark_uart_tx_latch_fault();
        routed_benchmark_uart_tx_irq_restore(state);
        return 0;
    }
    for (index = 0u; index < length; index++) {
        uint8_t byte = bytes[index];

        if (routed_benchmark_uart_tx_state.dropping_record != 0u ||
            routed_benchmark_uart_tx_state.count == ROUTED_BENCHMARK_UART_TX_CAPACITY) {
            if (routed_benchmark_uart_tx_state.dropping_record == 0u) {
                uint16_t open = routed_benchmark_uart_tx_state.open_record_count;

                /* An incomplete record was never DMA eligible.  Remove it
                 * before accepting the overflow outcome so no malformed
                 * prefix can survive to be joined with a later valid line. */
                if (routed_benchmark_uart_tx_state.tail >= open) {
                    routed_benchmark_uart_tx_state.tail = (uint16_t)(
                        routed_benchmark_uart_tx_state.tail - open);
                } else {
                    routed_benchmark_uart_tx_state.tail = (uint16_t)(
                        ROUTED_BENCHMARK_UART_TX_CAPACITY -
                        (open - routed_benchmark_uart_tx_state.tail));
                }
                routed_benchmark_uart_tx_state.count = (uint16_t)(
                    routed_benchmark_uart_tx_state.count - open);
                routed_benchmark_uart_tx_state.open_record_count = 0u;
                routed_benchmark_uart_tx_add_saturating(
                    &routed_benchmark_uart_tx_state.dropped_bytes, open);
                routed_benchmark_uart_tx_increment(
                    &routed_benchmark_uart_tx_state.dropped_records);
                routed_benchmark_uart_tx_state.dropping_record = 1u;
            }
            routed_benchmark_uart_tx_increment(
                &routed_benchmark_uart_tx_state.dropped_bytes);
            if (byte == (uint8_t)'\n') {
                routed_benchmark_uart_tx_state.dropping_record = 0u;
            }
            continue;
        }
        routed_benchmark_uart_tx_ring[routed_benchmark_uart_tx_state.tail] = byte;
        routed_benchmark_uart_tx_state.tail++;
        if (routed_benchmark_uart_tx_state.tail ==
            ROUTED_BENCHMARK_UART_TX_CAPACITY) {
            routed_benchmark_uart_tx_state.tail = 0u;
        }
        routed_benchmark_uart_tx_state.count++;
        routed_benchmark_uart_tx_state.open_record_count++;
        if (routed_benchmark_uart_tx_state.count >
            routed_benchmark_uart_tx_state.high_water_bytes) {
            routed_benchmark_uart_tx_state.high_water_bytes =
                routed_benchmark_uart_tx_state.count;
        }
        if (byte == (uint8_t)'\n') {
            routed_benchmark_uart_tx_state.ready_count = (uint16_t)(
                routed_benchmark_uart_tx_state.ready_count +
                routed_benchmark_uart_tx_state.open_record_count);
            routed_benchmark_uart_tx_state.open_record_count = 0u;
        }
    }
    if (routed_benchmark_uart_tx_state.active_length == 0u &&
        routed_benchmark_uart_tx_state.ready_count != 0u &&
        routed_benchmark_uart_tx_state.transport_faults == 0u) {
        routed_benchmark_uart_tx_start_contiguous();
    }
    routed_benchmark_uart_tx_irq_restore(state);
    return 1;
}

int routed_benchmark_uart_tx_has_headroom(void)
{
    int result;
    uint32_t state = routed_benchmark_uart_tx_irq_save();

    result = routed_benchmark_uart_tx_state.ready != 0u &&
        routed_benchmark_uart_tx_state.transport_faults == 0u &&
        routed_benchmark_uart_tx_state.dropping_record == 0u &&
        routed_benchmark_uart_tx_invariants_hold() &&
        ROUTED_BENCHMARK_UART_TX_CAPACITY - routed_benchmark_uart_tx_state.count >=
            ROUTED_BENCHMARK_UART_TX_HEADROOM;
    routed_benchmark_uart_tx_irq_restore(state);
    return result;
}

int routed_benchmark_uart_tx_settled(void)
{
    int result;
    uint32_t state = routed_benchmark_uart_tx_irq_save();

    result = routed_benchmark_uart_tx_state.ready != 0u &&
        routed_benchmark_uart_tx_state.transport_faults == 0u &&
        routed_benchmark_uart_tx_state.dropping_record == 0u &&
        routed_benchmark_uart_tx_invariants_hold() &&
        routed_benchmark_uart_tx_state.count == 0u &&
        routed_benchmark_uart_tx_state.ready_count == 0u &&
        routed_benchmark_uart_tx_state.open_record_count == 0u &&
        routed_benchmark_uart_tx_state.active_length == 0u;
    routed_benchmark_uart_tx_irq_restore(state);
    return result;
}

void routed_benchmark_uart_tx_snapshot(routed_benchmark_uart_tx_snapshot_t *out)
{
    uint32_t state;

    if (out == NULL) {
        return;
    }
    state = routed_benchmark_uart_tx_irq_save();
    out->pending_bytes = routed_benchmark_uart_tx_state.count;
    out->high_water_bytes = routed_benchmark_uart_tx_state.high_water_bytes;
    out->dropped_bytes = routed_benchmark_uart_tx_state.dropped_bytes;
    out->dropped_records = routed_benchmark_uart_tx_state.dropped_records;
    out->transport_faults = routed_benchmark_uart_tx_state.transport_faults;
    routed_benchmark_uart_tx_irq_restore(state);
}

void routed_benchmark_uart_tx_interrupt(uint32_t intno)
{
    uint16_t active;
    uint32_t next_head;

    if (intno != (uint32_t)INTNO(UART0_BASE)) {
        return;
    }
    if (in_w(ROUTED_UART0_BASE + ROUTED_UART_EVENTS_ENDTX) == 0u ||
        !routed_benchmark_uart_tx_invariants_hold() ||
        routed_benchmark_uart_tx_state.active_length == 0u ||
        in_w(ROUTED_UART0_BASE + ROUTED_UART_TXD_AMOUNT) !=
            routed_benchmark_uart_tx_state.active_length) {
        routed_benchmark_uart_tx_latch_fault();
        return;
    }
    out_w(ROUTED_UART0_BASE + ROUTED_UART_EVENTS_ENDTX, 0u);
    active = routed_benchmark_uart_tx_state.active_length;
    next_head = (uint32_t)routed_benchmark_uart_tx_state.head + active;
    if (next_head >= ROUTED_BENCHMARK_UART_TX_CAPACITY) {
        next_head -= ROUTED_BENCHMARK_UART_TX_CAPACITY;
    }
    routed_benchmark_uart_tx_state.head = (uint16_t)next_head;
    routed_benchmark_uart_tx_state.count = (uint16_t)(
        routed_benchmark_uart_tx_state.count - active);
    routed_benchmark_uart_tx_state.ready_count = (uint16_t)(
        routed_benchmark_uart_tx_state.ready_count - active);
    routed_benchmark_uart_tx_state.active_length = 0u;
    if (routed_benchmark_uart_tx_state.count == 0u) {
        routed_benchmark_uart_tx_state.tail = routed_benchmark_uart_tx_state.head;
        return;
    }
    if (routed_benchmark_uart_tx_state.ready_count != 0u) {
        routed_benchmark_uart_tx_start_contiguous();
    }
}

#ifdef ROUTED_BENCHMARK_UART_TX_HOST_TEST
void routed_benchmark_uart_tx_host_reset(void)
{
    routed_benchmark_uart_tx_state.head = 0u;
    routed_benchmark_uart_tx_state.tail = 0u;
    routed_benchmark_uart_tx_state.count = 0u;
    routed_benchmark_uart_tx_state.ready_count = 0u;
    routed_benchmark_uart_tx_state.open_record_count = 0u;
    routed_benchmark_uart_tx_state.active_length = 0u;
    routed_benchmark_uart_tx_state.high_water_bytes = 0u;
    routed_benchmark_uart_tx_state.dropping_record = 0u;
    routed_benchmark_uart_tx_state.ready = 0u;
    routed_benchmark_uart_tx_state.dropped_bytes = 0u;
    routed_benchmark_uart_tx_state.dropped_records = 0u;
    routed_benchmark_uart_tx_state.transport_faults = 0u;
}

void routed_benchmark_uart_tx_host_force_counters(uint32_t dropped_bytes,
                                                  uint32_t dropped_records,
                                                  uint32_t transport_faults)
{
    routed_benchmark_uart_tx_state.dropped_bytes = dropped_bytes;
    routed_benchmark_uart_tx_state.dropped_records = dropped_records;
    routed_benchmark_uart_tx_state.transport_faults = transport_faults;
}
#endif
