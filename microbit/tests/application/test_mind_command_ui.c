#include "mind_command.h"
#include "mind_log.h"
#include "mind_ui.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;
static uint32_t display_priority;
static uint32_t task_priority;
static int display_start_result = 1;
static int task_start_result = 1;
static uint32_t pwm_writes[8];

static void check(int condition, const char *message)
{
    if (!condition) {
        failures++;
        printf("FAIL: %s\n", message);
    }
}

int mind_ui_host_display_start(uint32_t priority)
{
    display_priority = priority;
    return display_start_result;
}

int mind_ui_host_task_start(uint32_t priority)
{
    task_priority = priority;
    return task_start_result;
}

void mind_audio_host_out_w(uint32_t address, uint32_t value)
{
    if (address == 0x50000700u && value == 3u) {
        pwm_writes[0]++;
    } else if (address == 0x4001c560u && value == 0u) {
        pwm_writes[1]++;
    } else if (address == 0x4001c504u && value == 0u) {
        pwm_writes[2]++;
    } else if (address == 0x4001c508u && value == 16000u) {
        pwm_writes[3]++;
    } else if (address == 0x4001c508u && value == 12800u) {
        pwm_writes[4]++;
    } else if (address == 0x4001c008u && value == 1u) {
        pwm_writes[5]++;
    } else if (address == 0x4001c004u && value == 1u) {
        pwm_writes[6]++;
    }
}

static int feed_line(mind_command_parser_t *parser, const char *line,
                     mind_command_attempt_t *attempt)
{
    size_t index;
    int got_attempt = 0;

    for (index = 0u; line[index] != '\0'; index++) {
        got_attempt |= mind_command_parser_feed(parser, (uint8_t)line[index], attempt);
    }
    return got_attempt;
}

static void test_commands(void)
{
    mind_command_parser_t parser;
    mind_command_attempt_t attempt;
    mind_command_mailbox_t mailbox;
    uint8_t index;

    mind_command_parser_init(&parser);
    check(feed_line(&parser, "ROOT ON\r\n", &attempt) &&
          attempt.command == MIND_COMMAND_ON &&
          attempt.status == MIND_COMMAND_ACCEPTED,
          "ROOT ON CRLF parses once");
    check(feed_line(&parser, "ROOT STATUS\n", &attempt) &&
          attempt.command == MIND_COMMAND_STATUS,
          "ROOT STATUS parses");
    check(feed_line(&parser, "GTT\r", &attempt) &&
          attempt.command == MIND_COMMAND_GTT &&
          attempt.status == MIND_COMMAND_ACCEPTED &&
          strcmp(mind_command_kind_name(attempt.command), "gtt") == 0,
          "exact uppercase GTT parses with its production command name");
    check(feed_line(&parser, "gtt\n", &attempt) &&
          attempt.command == MIND_COMMAND_INVALID &&
          attempt.status == MIND_COMMAND_MALFORMED,
          "GTT parser rejects non-exact lowercase input");
    check(feed_line(&parser, "ROOT MAYBE\r", &attempt) &&
          attempt.command == MIND_COMMAND_INVALID &&
          attempt.status == MIND_COMMAND_MALFORMED,
          "invalid command is explicitly malformed");
    check(feed_line(&parser, "1234567890123456\n", &attempt) &&
          attempt.status == MIND_COMMAND_OVERFLOW,
          "overlong command is explicitly overflow");

    mind_command_mailbox_init(&mailbox);
    attempt.command = MIND_COMMAND_ON;
    attempt.status = MIND_COMMAND_ACCEPTED;
    for (index = 0u; index < MIND_COMMAND_MAILBOX_CAPACITY - 1u; index++) {
        check(mind_command_mailbox_offer(&mailbox, &attempt),
              "SPSC mailbox accepts copied attempts through its physical bound");
    }
    check(!mind_command_mailbox_offer(&mailbox, &attempt),
          "SPSC mailbox full leaves producer/consumer indices intact");
    check(mind_command_mailbox_peek(&mailbox, &attempt) &&
          mind_command_mailbox_consume(&mailbox) &&
          mind_command_mailbox_offer(&mailbox, &attempt),
          "consumer interleave frees exactly one mailbox publication slot");
}

static void test_ui_and_audio(void)
{
    mind_ui_t ui;
    uint8_t rows[5];
    uint8_t node;

    mind_ui_init_state(&ui, 1u);
    mind_ui_publish(&ui, 0u, 0u);
    mind_ui_service(&ui, 0u);
    for (node = 1u; node <= 6u; node++) {
        mind_ui_init_state(&ui, node);
        mind_ui_publish(&ui, 0u, 0u);
        mind_ui_service(&ui, 0u);
        mind_ui_render(&ui, 0u, rows);
        check((rows[4] & 0x18u) == 0u &&
              (rows[0] | rows[1] | rows[2] | rows[3] | rows[4]) != 0u,
              "all six leaf glyphs reserve root indicators");
    }
    mind_ui_init_state(&ui, 1u);
    mind_ui_publish(&ui, 0u, 0u);
    mind_ui_service(&ui, 0u);
    mind_ui_render(&ui, 0u, rows);
    check((rows[4] & 0x18u) == 0u, "zero-root leaf hides both indicators");
    mind_ui_publish(&ui, 0u, 1u);
    mind_ui_service(&ui, 1u);
    mind_ui_render(&ui, 1u, rows);
    check((rows[4] & 0x10u) != 0u && (rows[4] & 0x08u) == 0u,
          "one root uses one indicator pixel");
    mind_ui_publish(&ui, 0u, 16u);
    mind_ui_service(&ui, 2u);
    mind_ui_render(&ui, 2u, rows);
    check((rows[4] & 0x18u) == 0x18u, "two-or-more roots saturate two indicators");
    mind_ui_render(&ui, 500u, rows);
    check((rows[4] & 0x18u) == 0x18u,
          "two root indicators remain steady across the former flash phase");
    mind_ui_render(&ui, 5000u, rows);
    check((rows[4] & 0x18u) == 0x18u,
          "steady root indicators are independent of render time");

    mind_audio_hardware_init();
    mind_ui_publish(&ui, 1u, 16u);
    mind_ui_service(&ui, 600u);
    check(ui.audio.phase == 1u && pwm_writes[3] != 0u && pwm_writes[5] != 0u,
          "UI consumer alone starts the first PWM tone");
    mind_ui_service(&ui, 680u);
    check(ui.audio.phase == 2u && pwm_writes[4] != 0u,
          "UI real-time service advances to the second PWM tone");
    mind_ui_publish(&ui, 1u, 16u);
    mind_ui_service(&ui, 681u);
    check(ui.audio.queued_cues == 0u,
          "duplicate ON does not create another cue");
    mind_ui_publish(&ui, 0u, 16u);
    mind_ui_service(&ui, 682u);
    mind_ui_publish(&ui, 1u, 16u);
    mind_ui_service(&ui, 683u);
    check(ui.audio.phase == 1u && ui.audio.queued_cues != 0u,
          "rapid OFF->ON restarts and queues a genuine root cue");
    mind_ui_render(&ui, 683u, rows);
    check(memcmp(rows, (uint8_t[]){ 0x0eu, 0x11u, 0x0eu, 0x0au, 0x11u }, 5u) == 0,
          "local root persistently displays R without leaf overlay");
    check(pwm_writes[0] != 0u && pwm_writes[1] != 0u && pwm_writes[6] != 0u,
          "PWM setup/configuration/stop register sequence is explicit");
}

static void test_ui_start_failure(void)
{
    mind_ui_t ui;

    mind_ui_init_state(&ui, 1u);
    display_start_result = 0;
    task_start_result = 1;
    check(!mind_ui_start(&ui), "display-start failure is reported truthfully");
    display_start_result = 1;
    task_start_result = 0;
    check(!mind_ui_start(&ui), "UI-task-start failure is reported truthfully");
    task_start_result = 1;
    check(mind_ui_start(&ui) && display_priority == MIND_UI_TASK_PRIORITY &&
          task_priority == MIND_UI_TASK_PRIORITY,
          "both visual tasks run at priority twelve");
}

static void test_root_command_logger_capacity_gate(void)
{
    mind_command_mailbox_t mailbox;
    mind_log_queue_t log_queue;
    mind_log_reservation_t reservation;
    mind_log_record_t records[MIND_LOG_CAPACITY];
    mind_log_record_t discarded;
    mind_root_plane_t plane;
    mind_ui_t ui;
    mind_command_attempt_t attempt;
    tavrn_adva_t local = { { 0x80u, 0x81u, 0x54u, 0x56u, 0x78u, 0xc0u } };

    memset(records, 0, sizeof(records));
    mind_command_mailbox_init(&mailbox);
    mind_log_queue_init(&log_queue);
    mind_root_plane_init(&plane, &local, 1u);
    mind_ui_init_state(&ui, 1u);
    mind_audio_hardware_init();
    attempt.command = MIND_COMMAND_ON;
    attempt.status = MIND_COMMAND_ACCEPTED;
    check(mind_command_mailbox_offer(&mailbox, &attempt) &&
          mind_log_queue_reserve(&log_queue, MIND_LOG_CAPACITY - 1u, &reservation) &&
          mind_log_queue_commit(&log_queue, &reservation, records),
          "logger-full fixture retains an accepted ROOT ON at mailbox head");

    /* This is the exact pre-mutation gate used by mind_process_commands(): a
     * root command needs both its command and resulting-root records. */
    check(!mind_log_queue_reserve(&log_queue, 2u, &reservation) &&
          mind_command_mailbox_peek(&mailbox, &attempt) &&
          !mind_root_plane_local_active(&plane) && ui.audio.phase == 0u,
          "full logger prevents command state transition and root cue before two slots exist");
    check(mind_log_queue_take(&log_queue, &discarded) &&
          mind_log_queue_take(&log_queue, &discarded) &&
          mind_log_queue_reserve(&log_queue, 2u, &reservation),
          "logger recovery makes both root-command records available atomically");
    check(mind_root_plane_set_local(&plane, 1u, 1u) == MIND_ROOT_APPLY_ACCEPTED &&
          mind_log_queue_commit(&log_queue, &reservation, records) &&
          mind_command_mailbox_consume(&mailbox),
          "accepted ROOT ON mutates only after the reserved records commit");
    mind_ui_publish(&ui, mind_root_plane_local_active(&plane),
                    mind_root_plane_active_count(&plane));
    mind_ui_service(&ui, 1u);
    check(ui.audio.phase == 1u,
          "the deferred command produces its root cue only after logger recovery");
}

int main(void)
{
    test_commands();
    test_ui_and_audio();
    test_ui_start_failure();
    test_root_command_logger_capacity_gate();
    if (failures != 0u) {
        printf("mind_command_ui failures=%u\n", failures);
        return 1;
    }
    printf("mind_command_ui tests passed\n");
    return 0;
}
