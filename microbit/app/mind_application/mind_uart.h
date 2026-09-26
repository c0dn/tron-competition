#ifndef MIND_UART_H
#define MIND_UART_H

/* UART0 RX-only replacement vector.  The T-monitor remains the only TX
 * owner: no part of this interface opens sera or writes UART TX registers. */

#include <stdint.h>

#include "mind_command.h"

#define MIND_UART_RX_RING_CAPACITY 32u
#define MIND_UART_TASK_PRIORITY 12u
#define MIND_UART_INTERRUPT_PRIORITY 5u

/* A copied observation only: it never services the RX ring or advances either
 * mailbox endpoint.  The depth values are bounded by their fixed rings. */
typedef struct mind_uart_snapshot {
    uint32_t bytes_received;
    uint32_t commands_parsed;
    uint32_t mailbox_publications;
    uint32_t production_peeks;
    uint32_t production_consumes;
    uint32_t error_count;
    uint32_t overflow_count;
    uint32_t mailbox_full_count;
    uint8_t ring_depth;
    uint8_t mailbox_depth;
    uint8_t pending_valid;
} mind_uart_snapshot_t;

typedef struct mind_uart {
    volatile uint8_t bytes[MIND_UART_RX_RING_CAPACITY];
    volatile uint32_t error_count;
    volatile uint32_t overflow_count;
    uint32_t overflow_reported;
    volatile uint32_t bytes_received;
    volatile uint32_t commands_parsed;
    volatile uint32_t mailbox_publications;
    volatile uint32_t mailbox_full_count;
    uint32_t production_peeks;
    uint32_t production_consumes;
    /* SPSC RX ring: ISR writes tail; parser task writes head. */
    volatile uint8_t head;
    volatile uint8_t tail;
    volatile uint8_t pending_valid;
    mind_command_parser_t parser;
    mind_command_mailbox_t mailbox;
    mind_command_attempt_t pending_attempt;
} mind_uart_t;

void mind_uart_init_state(mind_uart_t *uart);
/* Installs the application UART0 vector after inherited device startup. */
int mind_uart_install(mind_uart_t *uart);
/* Starts the lower-priority parser task. */
int mind_uart_start_task(mind_uart_t *uart);

/* Exposed for host verification; production ISR calls this after RXDRDY. */
void mind_uart_isr_push(mind_uart_t *uart, uint8_t byte);
/* Parser-task service; it owns RX head and mailbox tail. */
void mind_uart_service(mind_uart_t *uart);
/* Production coordinator seam.  These preserve the mailbox transaction while
 * recording only successful mesh-owner observations; neither services RX. */
int mind_uart_command_peek(mind_uart_t *uart,
                           mind_command_attempt_t *attempt_out);
int mind_uart_command_consume(mind_uart_t *uart);
void mind_uart_snapshot(const mind_uart_t *uart,
                        mind_uart_snapshot_t *snapshot_out);
/* Legacy host convenience only.  Production bindings must use the two
 * non-servicing wrappers above so parser ownership remains explicit. */
int mind_uart_take_attempt(mind_uart_t *uart, mind_command_attempt_t *attempt_out);
void mind_uart_interrupt(uint32_t intno);

#endif /* MIND_UART_H */
