#include "mind_uart.h"

#include <limits.h>
#include <string.h>

#ifdef MIND_APPLICATION_HOST_TEST
typedef unsigned int UINT;
typedef int INT;
typedef int ER;
typedef void (*FP)(UINT);
typedef struct mind_uart_host_dint {
    uint32_t intatr;
    void (*inthdr)(UINT);
} T_DINT;
#define TA_HLNG 1u
#define TA_RNG3 2u
#define E_OK 0
#define UART0_BASE 0x40002000u
#define UART_EVENTS_RXDRDY 0x108u
#define UART_EVENTS_TXDRDY 0x11cu
#define UART_EVENTS_ERROR 0x124u
#define UART_INTENSET 0x304u
#define UART_INTENCLR 0x308u
#define UART_ERRORSRC 0x480u
#define UART_RXD 0x518u
#define INTNO(base) ((UINT)(base))
#define UART(unit, register_name) (UART0_BASE + UART_##register_name)
extern uint32_t mind_uart_host_in_w(uint32_t address);
extern void mind_uart_host_out_w(uint32_t address, uint32_t value);
extern ER mind_uart_host_def_int(UINT intno, const T_DINT *definition);
extern void mind_uart_host_enable_int(UINT intno, INT priority);
#define in_w(address) mind_uart_host_in_w((address))
#define out_w(address, value) mind_uart_host_out_w((address), (value))
#define tk_def_int(intno, definition) mind_uart_host_def_int((intno), (definition))
#define EnableInt(intno, priority) mind_uart_host_enable_int((intno), (priority))
#else
#include <tk/tkernel.h>

#include "tron_build_config.h"
#endif

#define MIND_UART_INT_RXDRDY 0x00000004u
#define MIND_UART_INT_TXDRDY 0x00000080u
#define MIND_UART_INT_ERROR 0x00000200u

static mind_uart_t *mind_uart_active;

#ifndef MIND_APPLICATION_HOST_TEST
#if defined(__GNUC__)
#define MIND_UART_TASK_STACK_ALIGNMENT \
    __attribute__((aligned(TRON_BUILD_MIND_TASK_STACK_ALIGNMENT_BYTES)))
#else
#define MIND_UART_TASK_STACK_ALIGNMENT
#endif
typedef char mind_uart_task_stack_size_guard[
    (TRON_BUILD_MIND_UART_TASK_STATIC_BUFFER_BYTES ==
     TRON_BUILD_MIND_UART_TASK_STACK_BYTES +
     TRON_BUILD_MIND_TASK_SYSTEM_STACK_BYTES) ? 1 : -1];
typedef char mind_uart_task_stack_alignment_guard[
    (TRON_BUILD_MIND_UART_TASK_STATIC_BUFFER_BYTES %
     TRON_BUILD_MIND_TASK_STACK_ALIGNMENT_BYTES == 0u) ? 1 : -1];
static uint8_t mind_uart_task_stack[TRON_BUILD_MIND_UART_TASK_STATIC_BUFFER_BYTES]
    MIND_UART_TASK_STACK_ALIGNMENT;
#endif

static void increment_saturating(volatile uint32_t *value)
{
    if (value != NULL && *value != UINT32_MAX) {
        (*value)++;
    }
}

void mind_uart_init_state(mind_uart_t *uart)
{
    if (uart != NULL) {
        memset(uart, 0, sizeof(*uart));
        mind_command_parser_init(&uart->parser);
        mind_command_mailbox_init(&uart->mailbox);
    }
}

void mind_uart_isr_push(mind_uart_t *uart, uint8_t byte)
{
    uint8_t tail;
    uint8_t next_tail;

    if (uart == NULL) {
        return;
    }
    increment_saturating(&uart->bytes_received);
    tail = uart->tail;
    if (tail >= MIND_UART_RX_RING_CAPACITY ||
        uart->head >= MIND_UART_RX_RING_CAPACITY) {
        increment_saturating(&uart->overflow_count);
        return;
    }
    next_tail = (uint8_t)((tail + 1u) % MIND_UART_RX_RING_CAPACITY);
    if (next_tail == uart->head) {
        increment_saturating(&uart->overflow_count);
        return;
    }
    uart->bytes[tail] = byte;
    /* The volatile tail store publishes the fully copied byte to the parser. */
    uart->tail = next_tail;
}

static int mind_uart_ring_take(mind_uart_t *uart, uint8_t *byte_out)
{
    uint8_t head;

    if (uart == NULL || byte_out == NULL) {
        return 0;
    }
    head = uart->head;
    if (head >= MIND_UART_RX_RING_CAPACITY ||
        uart->tail >= MIND_UART_RX_RING_CAPACITY || head == uart->tail) {
        return 0;
    }
    *byte_out = uart->bytes[head];
    uart->head = (uint8_t)((head + 1u) % MIND_UART_RX_RING_CAPACITY);
    return 1;
}

static int mind_uart_publish_pending(mind_uart_t *uart)
{
    if (uart == NULL || uart->pending_valid == 0u) {
        return 1;
    }
    if (!mind_command_mailbox_offer(&uart->mailbox, &uart->pending_attempt)) {
        increment_saturating(&uart->mailbox_full_count);
        return 0;
    }
    increment_saturating(&uart->mailbox_publications);
    uart->pending_valid = 0u;
    return 1;
}

void mind_uart_service(mind_uart_t *uart)
{
    uint8_t byte;

    if (uart == NULL || !mind_uart_publish_pending(uart)) {
        return;
    }
    /* An ISR-only monotonic counter prevents a task-side clear racing a new
     * overflow.  The parser discards the uncertain partial line and emits a
     * bounded overflow outcome before resuming byte parsing. */
    if (uart->overflow_reported != uart->overflow_count) {
        uart->head = uart->tail;
        mind_command_parser_init(&uart->parser);
        uart->pending_attempt.command = MIND_COMMAND_INVALID;
        uart->pending_attempt.status = MIND_COMMAND_OVERFLOW;
        uart->pending_valid = 1u;
        uart->overflow_reported = uart->overflow_count;
        (void)mind_uart_publish_pending(uart);
        return;
    }
    while (mind_uart_ring_take(uart, &byte)) {
        mind_command_attempt_t attempt;

        if (mind_command_parser_feed(&uart->parser, byte, &attempt)) {
            increment_saturating(&uart->commands_parsed);
            uart->pending_attempt = attempt;
            uart->pending_valid = 1u;
            if (!mind_uart_publish_pending(uart)) {
                return;
            }
        }
    }
}

int mind_uart_command_peek(mind_uart_t *uart,
                           mind_command_attempt_t *attempt_out)
{
    if (uart == NULL || attempt_out == NULL ||
        !mind_command_mailbox_peek(&uart->mailbox, attempt_out)) {
        return 0;
    }
    increment_saturating(&uart->production_peeks);
    return 1;
}

int mind_uart_command_consume(mind_uart_t *uart)
{
    if (uart == NULL || !mind_command_mailbox_consume(&uart->mailbox)) {
        return 0;
    }
    increment_saturating(&uart->production_consumes);
    return 1;
}

static uint8_t mind_uart_ring_depth(const mind_uart_t *uart)
{
    uint8_t head;
    uint8_t tail;

    if (uart == NULL) {
        return 0u;
    }
    head = uart->head;
    tail = uart->tail;
    if (head >= MIND_UART_RX_RING_CAPACITY || tail >= MIND_UART_RX_RING_CAPACITY) {
        return 0u;
    }
    return tail >= head ? (uint8_t)(tail - head) :
        (uint8_t)(MIND_UART_RX_RING_CAPACITY - head + tail);
}

void mind_uart_snapshot(const mind_uart_t *uart,
                        mind_uart_snapshot_t *snapshot_out)
{
    if (snapshot_out == NULL) {
        return;
    }
    memset(snapshot_out, 0, sizeof(*snapshot_out));
    if (uart == NULL) {
        return;
    }
    snapshot_out->bytes_received = uart->bytes_received;
    snapshot_out->commands_parsed = uart->commands_parsed;
    snapshot_out->mailbox_publications = uart->mailbox_publications;
    snapshot_out->production_peeks = uart->production_peeks;
    snapshot_out->production_consumes = uart->production_consumes;
    snapshot_out->error_count = uart->error_count;
    snapshot_out->overflow_count = uart->overflow_count;
    snapshot_out->mailbox_full_count = uart->mailbox_full_count;
    snapshot_out->ring_depth = mind_uart_ring_depth(uart);
    snapshot_out->mailbox_depth = mind_command_mailbox_depth(&uart->mailbox);
    snapshot_out->pending_valid = uart->pending_valid != 0u ? 1u : 0u;
}

int mind_uart_take_attempt(mind_uart_t *uart, mind_command_attempt_t *attempt_out)
{
    if (uart == NULL || attempt_out == NULL) {
        return 0;
    }
    mind_uart_service(uart);
    if (!mind_uart_command_peek(uart, attempt_out)) {
        return 0;
    }
    return mind_uart_command_consume(uart);
}

void mind_uart_interrupt(uint32_t intno)
{
    mind_uart_t *uart = mind_uart_active;

    if (uart == NULL || intno != (uint32_t)INTNO(UART0_BASE)) {
        return;
    }
    while (in_w(UART(0, EVENTS_RXDRDY)) != 0u) {
        uint8_t byte;

        out_w(UART(0, EVENTS_RXDRDY), 0u);
        byte = (uint8_t)in_w(UART(0, RXD));
        mind_uart_isr_push(uart, byte);
    }
    if (in_w(UART(0, EVENTS_ERROR)) != 0u) {
        uint32_t error_mask;

        out_w(UART(0, EVENTS_ERROR), 0u);
        error_mask = in_w(UART(0, ERRORSRC));
        if (error_mask != 0u) {
            /* ERRORSRC is write-one-to-clear. */
            out_w(UART(0, ERRORSRC), error_mask);
        }
        increment_saturating(&uart->error_count);
    }
    /* TXDRDY is deliberately neither read/cleared nor used to write TXD. */
}

int mind_uart_install(mind_uart_t *uart)
{
    T_DINT definition;

    if (uart == NULL) {
        return 0;
    }
    mind_uart_active = uart;
    memset(&definition, 0, sizeof(definition));
    definition.intatr = TA_HLNG;
    definition.inthdr = (FP)mind_uart_interrupt;
    if (tk_def_int(INTNO(UART0_BASE), &definition) != E_OK) {
        mind_uart_active = NULL;
        return 0;
    }
    out_w(UART(0, EVENTS_RXDRDY), 0u);
    out_w(UART(0, EVENTS_ERROR), 0u);
    {
        uint32_t error_mask = in_w(UART(0, ERRORSRC));

        if (error_mask != 0u) {
            out_w(UART(0, ERRORSRC), error_mask);
        }
    }
    out_w(UART(0, INTENCLR), MIND_UART_INT_TXDRDY);
    out_w(UART(0, INTENSET), MIND_UART_INT_RXDRDY | MIND_UART_INT_ERROR);
    EnableInt(INTNO(UART0_BASE), MIND_UART_INTERRUPT_PRIORITY);
    return 1;
}

#ifndef MIND_APPLICATION_HOST_TEST
static void mind_uart_task(INT stacd, void *exinf)
{
    mind_uart_t *uart = (mind_uart_t *)exinf;

    (void)stacd;
    while (1) {
        mind_uart_service(uart);
        (void)tk_dly_tsk(1u);
    }
}

int mind_uart_start_task(mind_uart_t *uart)
{
    T_CTSK task;
    ID id;

    if (uart == NULL) {
        return 0;
    }
    memset(&task, 0, sizeof(task));
    task.exinf = uart;
    task.tskatr = TA_HLNG | TA_RNG3 | TA_USERBUF;
    task.task = (FP)mind_uart_task;
    task.itskpri = MIND_UART_TASK_PRIORITY;
    task.stksz = TRON_BUILD_MIND_UART_TASK_STACK_BYTES;
    task.bufptr = mind_uart_task_stack;
    id = tk_cre_tsk(&task);
    return id > 0 && tk_sta_tsk(id, 0) == E_OK;
}
#else
int mind_uart_start_task(mind_uart_t *uart)
{
    return uart != NULL;
}
#endif
