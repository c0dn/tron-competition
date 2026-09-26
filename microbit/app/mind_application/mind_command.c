#include "mind_command.h"

#include <string.h>

static int line_equals(const uint8_t *line, uint8_t length, const char *text)
{
    size_t text_length = strlen(text);

    return length == text_length && memcmp(line, text, text_length) == 0;
}

void mind_command_parser_init(mind_command_parser_t *parser)
{
    if (parser != NULL) {
        memset(parser, 0, sizeof(*parser));
    }
}

int mind_command_parser_feed(mind_command_parser_t *parser, uint8_t byte,
                             mind_command_attempt_t *attempt_out)
{
    mind_command_attempt_t attempt;

    if (parser == NULL || attempt_out == NULL) {
        return 0;
    }
    if (byte != '\r' && byte != '\n') {
        if (parser->overlong == 0u) {
            if (parser->length + 1u < MIND_COMMAND_LINE_CAPACITY) {
                parser->line[parser->length++] = byte;
            } else {
                parser->overlong = 1u;
            }
        }
        return 0;
    }
    if (parser->length == 0u && parser->overlong == 0u) {
        return 0;
    }
    memset(&attempt, 0, sizeof(attempt));
    if (parser->overlong != 0u) {
        attempt.command = MIND_COMMAND_INVALID;
        attempt.status = MIND_COMMAND_OVERFLOW;
    } else if (line_equals(parser->line, parser->length, "ROOT ON")) {
        attempt.command = MIND_COMMAND_ON;
        attempt.status = MIND_COMMAND_ACCEPTED;
    } else if (line_equals(parser->line, parser->length, "ROOT OFF")) {
        attempt.command = MIND_COMMAND_OFF;
        attempt.status = MIND_COMMAND_ACCEPTED;
    } else if (line_equals(parser->line, parser->length, "ROOT STATUS")) {
        attempt.command = MIND_COMMAND_STATUS;
        attempt.status = MIND_COMMAND_ACCEPTED;
    } else if (line_equals(parser->line, parser->length, "GTT")) {
        attempt.command = MIND_COMMAND_GTT;
        attempt.status = MIND_COMMAND_ACCEPTED;
    } else {
        attempt.command = MIND_COMMAND_INVALID;
        attempt.status = MIND_COMMAND_MALFORMED;
    }
    parser->length = 0u;
    parser->overlong = 0u;
    *attempt_out = attempt;
    return 1;
}

void mind_command_mailbox_init(mind_command_mailbox_t *mailbox)
{
    if (mailbox != NULL) {
        memset(mailbox, 0, sizeof(*mailbox));
    }
}

int mind_command_mailbox_offer(mind_command_mailbox_t *mailbox,
                               const mind_command_attempt_t *attempt)
{
    uint8_t tail;
    uint8_t next_tail;

    if (mailbox == NULL || attempt == NULL) {
        return 0;
    }
    tail = mailbox->tail;
    if (tail >= MIND_COMMAND_MAILBOX_CAPACITY ||
        mailbox->head >= MIND_COMMAND_MAILBOX_CAPACITY) {
        return 0;
    }
    next_tail = (uint8_t)((tail + 1u) % MIND_COMMAND_MAILBOX_CAPACITY);
    if (next_tail == mailbox->head) {
        return 0;
    }
    mailbox->attempts[tail] = *attempt;
    /* Publish only after the copied attempt is complete. */
    mailbox->tail = next_tail;
    return 1;
}

int mind_command_mailbox_peek(const mind_command_mailbox_t *mailbox,
                              mind_command_attempt_t *attempt_out)
{
    uint8_t head;

    if (mailbox == NULL || attempt_out == NULL) {
        return 0;
    }
    head = mailbox->head;
    if (head >= MIND_COMMAND_MAILBOX_CAPACITY ||
        mailbox->tail >= MIND_COMMAND_MAILBOX_CAPACITY || head == mailbox->tail) {
        return 0;
    }
    *attempt_out = mailbox->attempts[head];
    return 1;
}

int mind_command_mailbox_consume(mind_command_mailbox_t *mailbox)
{
    uint8_t head;

    if (mailbox == NULL) {
        return 0;
    }
    head = mailbox->head;
    if (head >= MIND_COMMAND_MAILBOX_CAPACITY ||
        mailbox->tail >= MIND_COMMAND_MAILBOX_CAPACITY || head == mailbox->tail) {
        return 0;
    }
    mailbox->head = (uint8_t)((head + 1u) % MIND_COMMAND_MAILBOX_CAPACITY);
    return 1;
}

uint8_t mind_command_mailbox_depth(const mind_command_mailbox_t *mailbox)
{
    uint8_t head;
    uint8_t tail;

    if (mailbox == NULL) {
        return 0u;
    }
    head = mailbox->head;
    tail = mailbox->tail;
    if (head >= MIND_COMMAND_MAILBOX_CAPACITY ||
        tail >= MIND_COMMAND_MAILBOX_CAPACITY) {
        return 0u;
    }
    return tail >= head ? (uint8_t)(tail - head) :
        (uint8_t)(MIND_COMMAND_MAILBOX_CAPACITY - head + tail);
}

const char *mind_command_kind_name(mind_command_kind_t command)
{
    switch (command) {
    case MIND_COMMAND_ON:
        return "on";
    case MIND_COMMAND_OFF:
        return "off";
    case MIND_COMMAND_STATUS:
        return "status";
    case MIND_COMMAND_GTT:
        return "gtt";
    default:
        return "invalid";
    }
}

const char *mind_command_status_name(mind_command_status_t status)
{
    switch (status) {
    case MIND_COMMAND_ACCEPTED:
        return "accepted";
    case MIND_COMMAND_DUPLICATE:
        return "duplicate";
    case MIND_COMMAND_BUSY:
        return "busy";
    case MIND_COMMAND_MALFORMED:
        return "malformed";
    case MIND_COMMAND_OVERFLOW:
        return "overflow";
    default:
        return "rejected";
    }
}
