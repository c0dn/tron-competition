#include "mind_application_wire.h"
#include "mind_root_inbox.h"
#include "mind_uart.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;
static uint32_t installed_intno;
static uint32_t installed_priority;
static uint32_t writes[8];
static uint8_t rx_bytes[64];
static uint8_t rx_head;
static uint8_t rx_tail;
static uint32_t error_event;
static uint32_t error_mask;

static void check(int condition, const char *message)
{
    if (!condition) {
        failures++;
        printf("FAIL: %s\n", message);
    }
}

static void host_rx_feed(const char *text)
{
    size_t index;

    for (index = 0u; text[index] != '\0'; index++) {
        rx_bytes[rx_tail++] = (uint8_t)text[index];
    }
}

uint32_t mind_uart_host_in_w(uint32_t address)
{
    if (address == 0x40002108u) {
        return rx_head != rx_tail ? 1u : 0u;
    }
    if (address == 0x40002518u) {
        return rx_head != rx_tail ? rx_bytes[rx_head++] : 0u;
    }
    if (address == 0x40002124u) {
        return error_event;
    }
    if (address == 0x40002480u) {
        return error_mask;
    }
    return 0u;
}

void mind_uart_host_out_w(uint32_t address, uint32_t value)
{
    if (address == 0x40002108u && value == 0u) {
        writes[0]++;
    } else if (address == 0x40002124u && value == 0u) {
        writes[1]++;
        error_event = 0u;
    } else if (address == 0x40002480u) {
        writes[2]++;
        writes[7] = value;
        error_mask &= ~value;
    } else if (address == 0x40002308u && value == 0x80u) {
        writes[3]++;
    } else if (address == 0x40002304u && value == 0x204u) {
        writes[4]++;
    } else if (address == 0x4000211cu) {
        writes[5]++;
    } else if (address == 0x4000251cu) {
        writes[6]++;
    }
}

int mind_uart_host_def_int(unsigned int intno, const void *definition)
{
    (void)definition;
    installed_intno = intno;
    return 0;
}

void mind_uart_host_enable_int(unsigned int intno, int priority)
{
    installed_intno = intno;
    installed_priority = (uint32_t)priority;
}

static void test_uart_isr_spsc_and_w1c(void)
{
    mind_uart_t uart;
    mind_command_attempt_t attempt;
    mind_uart_snapshot_t snapshot;

    memset(writes, 0, sizeof(writes));
    rx_head = 0u;
    rx_tail = 0u;
    error_mask = 0x0au;
    error_event = 0u;
    mind_uart_init_state(&uart);
    check(mind_uart_install(&uart) && installed_intno == 0x40002000u &&
          installed_priority == MIND_UART_INTERRUPT_PRIORITY,
          "UART vector installs at inherited UART0 interrupt with priority five");
    check(writes[0] == 1u && writes[1] == 1u && writes[2] == 1u &&
          writes[7] == 0x0au && writes[3] == 1u && writes[4] == 1u &&
          writes[5] == 0u && writes[6] == 0u,
          "install clears RX/error, W1C-clears observed ERRORSRC, and never TXs");

    host_rx_feed("ROOT OFF\r");
    error_mask = 0x05u;
    error_event = 1u;
    mind_uart_interrupt(0x40002000u);
    check(writes[7] == 0x05u && error_mask == 0u && uart.error_count == 1u,
          "ISR reads and writes back ERRORSRC mask exactly once");
    mind_uart_service(&uart);
    check(mind_uart_command_peek(&uart, &attempt) &&
          attempt.command == MIND_COMMAND_OFF && mind_uart_command_consume(&uart),
          "ISR producer, parser task, and production mailbox transaction preserve bytes");
    mind_uart_snapshot(&uart, &snapshot);
    check(snapshot.bytes_received == 9u && snapshot.commands_parsed == 1u &&
          snapshot.mailbox_publications == 1u && snapshot.production_peeks == 1u &&
          snapshot.production_consumes == 1u && snapshot.error_count == 1u &&
          snapshot.overflow_count == 0u && snapshot.mailbox_full_count == 0u &&
          snapshot.ring_depth == 0u && snapshot.mailbox_depth == 0u &&
          snapshot.pending_valid == 0u,
          "non-servicing snapshot records the complete successful UART lifecycle");
}

static void test_uart_interleaving_and_overflow(void)
{
    mind_uart_t uart;
    mind_command_attempt_t attempt;
    uint8_t index;

    mind_uart_init_state(&uart);
    rx_head = 0u;
    rx_tail = 0u;
    error_mask = 0u;
    error_event = 0u;
    check(mind_uart_install(&uart), "interleave fixture installs its active UART state");
    host_rx_feed("ROOT ON\r");
    mind_uart_interrupt(0x40002000u);
    mind_uart_service(&uart);
    check(mind_command_mailbox_peek(&uart.mailbox, &attempt) &&
          attempt.command == MIND_COMMAND_ON,
          "parser publishes first SPSC mailbox record before consumer advances head");
    host_rx_feed("ROOT STATUS\r");
    mind_uart_interrupt(0x40002000u);
    check(mind_command_mailbox_consume(&uart.mailbox),
          "mesh consumer advances only mailbox head during ISR interleave");
    mind_uart_service(&uart);
    check(mind_uart_command_peek(&uart, &attempt) &&
          attempt.command == MIND_COMMAND_STATUS && mind_uart_command_consume(&uart),
          "interleaved ISR/task publications lose no command");

    mind_uart_init_state(&uart);
    for (index = 0u; index < MIND_UART_RX_RING_CAPACITY; index++) {
        mind_uart_isr_push(&uart, (uint8_t)index);
    }
    check(uart.overflow_count != 0u, "physical ring reserves one SPSC sentinel slot");
    mind_uart_service(&uart);
    check(mind_uart_command_peek(&uart, &attempt) &&
          attempt.status == MIND_COMMAND_OVERFLOW && mind_uart_command_consume(&uart),
          "ring overflow resynchronizes parser and emits one bounded outcome");
    host_rx_feed("ROOT ON\r");
    mind_uart_interrupt(0x40002000u);
    mind_uart_service(&uart);
    check(mind_uart_command_peek(&uart, &attempt) &&
          attempt.command == MIND_COMMAND_ON && mind_uart_command_consume(&uart),
          "parser resumes cleanly after overflow resynchronization");
}

static void test_uart_mailbox_backpressure(void)
{
    mind_uart_t uart;
    mind_command_attempt_t attempt;
    mind_uart_snapshot_t snapshot;
    uint8_t index;
    uint8_t off_seen = 0u;

    mind_uart_init_state(&uart);
    rx_head = 0u;
    rx_tail = 0u;
    error_mask = 0u;
    error_event = 0u;
    check(mind_uart_install(&uart), "backpressure fixture installs its active UART state");
    attempt.command = MIND_COMMAND_STATUS;
    attempt.status = MIND_COMMAND_ACCEPTED;
    for (index = 0u; index < MIND_COMMAND_MAILBOX_CAPACITY - 1u; index++) {
        check(mind_command_mailbox_offer(&uart.mailbox, &attempt),
              "fixture fills mailbox without producer count RMW");
    }
    host_rx_feed("ROOT OFF\r");
    mind_uart_interrupt(0x40002000u);
    mind_uart_service(&uart);
    check(uart.pending_valid != 0u && uart.mailbox_full_count != 0u,
          "parser retains a complete command while mailbox is full");
    mind_uart_snapshot(&uart, &snapshot);
    check(snapshot.bytes_received == 9u && snapshot.commands_parsed == 1u &&
          snapshot.mailbox_publications == 0u && snapshot.mailbox_full_count == 1u &&
          snapshot.ring_depth == 0u &&
          snapshot.mailbox_depth == MIND_COMMAND_MAILBOX_CAPACITY - 1u &&
          snapshot.pending_valid != 0u,
          "snapshot preserves parser-pending evidence while the mailbox is full");
    check(mind_uart_command_consume(&uart),
          "production consumer recovers one mailbox slot without servicing RX");
    mind_uart_service(&uart);
    while (mind_uart_command_peek(&uart, &attempt)) {
        if (attempt.command == MIND_COMMAND_OFF) {
            off_seen = 1u;
        }
        check(mind_uart_command_consume(&uart),
              "successful production peek has a matching production consume");
    }
    mind_uart_snapshot(&uart, &snapshot);
    check(off_seen != 0u && snapshot.mailbox_publications == 1u &&
          snapshot.production_peeks == MIND_COMMAND_MAILBOX_CAPACITY - 1u &&
          snapshot.production_consumes == MIND_COMMAND_MAILBOX_CAPACITY &&
          snapshot.mailbox_depth == 0u && snapshot.pending_valid == 0u,
          "retained backpressured command eventually publishes exactly once");
}

static void test_root_inbox(void)
{
    mind_root_inbox_t inbox;
    tavrn_direct_peer_t transmitter;
    tavrn_link_data_t data;
    tavrn_router_delivery_token_t token;
    mind_root_inbox_entry_t entry;

    memset(&transmitter, 0, sizeof(transmitter));
    memset(&data, 0, sizeof(data));
    data.app_kind = ROOT_STATE;
    mind_root_inbox_init(&inbox);
    check(mind_root_inbox_reserve(&inbox, &transmitter, &data, &token) ==
              TAVRN_ROUTER_DELIVERY_OK && token != 0u,
          "root control reserves final inbox before transport acceptance");
    check(mind_root_inbox_commit(&inbox, token, &transmitter, &data, 7u) ==
              TAVRN_ROUTER_DELIVERY_OK && mind_root_inbox_take(&inbox, &entry) &&
          entry.delivered_at_ms == 7u,
          "root control commit copies delivery for later mesh mutation");
}

int main(void)
{
    test_uart_isr_spsc_and_w1c();
    test_uart_interleaving_and_overflow();
    test_uart_mailbox_backpressure();
    test_root_inbox();
    if (failures != 0u) {
        printf("mind_uart failures=%u\n", failures);
        return 1;
    }
    printf("mind_uart tests passed\n");
    return 0;
}
