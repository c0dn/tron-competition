#ifndef MIND_COMMAND_H
#define MIND_COMMAND_H

/* Fixed copied command parsing.  UART ISR code only writes its byte ring; this
 * module runs below mesh ownership and publishes complete attempts by value. */

#include <stdint.h>

#define MIND_COMMAND_LINE_CAPACITY 16u
#define MIND_COMMAND_MAILBOX_CAPACITY 8u

typedef enum mind_command_kind {
    MIND_COMMAND_ON = 0,
    MIND_COMMAND_OFF,
    MIND_COMMAND_STATUS,
    MIND_COMMAND_GTT,
    MIND_COMMAND_INVALID,
} mind_command_kind_t;

typedef enum mind_command_status {
    MIND_COMMAND_ACCEPTED = 0,
    MIND_COMMAND_DUPLICATE,
    MIND_COMMAND_BUSY,
    MIND_COMMAND_MALFORMED,
    MIND_COMMAND_OVERFLOW,
    MIND_COMMAND_REJECTED,
} mind_command_status_t;

typedef struct mind_command_attempt {
    mind_command_kind_t command;
    mind_command_status_t status;
} mind_command_attempt_t;

typedef struct mind_command_parser {
    uint8_t line[MIND_COMMAND_LINE_CAPACITY];
    uint8_t length;
    uint8_t overlong;
} mind_command_parser_t;

typedef struct mind_command_mailbox {
    mind_command_attempt_t attempts[MIND_COMMAND_MAILBOX_CAPACITY];
    /* SPSC: the parser owns tail; the mesh owner owns head.  One physical
     * entry remains empty, so neither side performs a shared count RMW. */
    volatile uint8_t head;
    volatile uint8_t tail;
} mind_command_mailbox_t;

void mind_command_parser_init(mind_command_parser_t *parser);
/* Returns one complete CR/LF-terminated attempt.  A CRLF pair emits exactly
 * one attempt because an empty line is ignored. */
int mind_command_parser_feed(mind_command_parser_t *parser, uint8_t byte,
                             mind_command_attempt_t *attempt_out);

void mind_command_mailbox_init(mind_command_mailbox_t *mailbox);
int mind_command_mailbox_offer(mind_command_mailbox_t *mailbox,
                               const mind_command_attempt_t *attempt);
int mind_command_mailbox_peek(const mind_command_mailbox_t *mailbox,
                               mind_command_attempt_t *attempt_out);
int mind_command_mailbox_consume(mind_command_mailbox_t *mailbox);
/* Returns the current committed depth without advancing either SPSC owner. */
uint8_t mind_command_mailbox_depth(const mind_command_mailbox_t *mailbox);

const char *mind_command_kind_name(mind_command_kind_t command);
const char *mind_command_status_name(mind_command_status_t status);

#endif /* MIND_COMMAND_H */
