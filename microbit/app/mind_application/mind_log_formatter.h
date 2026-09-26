#ifndef MIND_LOG_FORMATTER_H
#define MIND_LOG_FORMATTER_H

/* Application-owned UART line formatting.  The logger task injects its sole
 * TX sink; host tests use the same formatter and never duplicate grammar. */

#include <stdint.h>

#include "mind_log.h"

#define MIND_LOG_FORMAT_LINE_BYTES 256u

typedef void (*mind_log_line_sink_fn)(void *context, const char *line);

int mind_log_format_record(const mind_log_record_t *record, char *line_out,
                           uint16_t line_capacity);
void mind_log_emit(const mind_log_record_t *record, mind_log_line_sink_fn sink,
                   void *context);

#endif /* MIND_LOG_FORMATTER_H */
