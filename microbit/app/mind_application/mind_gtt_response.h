#ifndef MIND_GTT_RESPONSE_H
#define MIND_GTT_RESPONSE_H

/* Fixed UI-owned read-only GTT response state.  The routed binding supplies
 * the sole copied snapshot storage; this helper only tracks its lifecycle and
 * formats one raw response record per call. */

#include <stdint.h>

#include "routed_cycle.h"

#define MIND_GTT_RESPONSE_LINE_BYTES 256u

typedef enum mind_gtt_response_state {
    MIND_GTT_RESPONSE_IDLE = 0,
    MIND_GTT_RESPONSE_CLAIMED,
    MIND_GTT_RESPONSE_PENDING,
    MIND_GTT_RESPONSE_READY,
    MIND_GTT_RESPONSE_EMITTING,
} mind_gtt_response_state_t;

typedef struct mind_gtt_response {
    mind_gtt_response_state_t state;
    uint8_t entry_cursor;
} mind_gtt_response_t;

void mind_gtt_response_init(mind_gtt_response_t *response);
int mind_gtt_response_claim(mind_gtt_response_t *response);
void mind_gtt_response_commit(mind_gtt_response_t *response);
void mind_gtt_response_cancel(mind_gtt_response_t *response);
void mind_gtt_response_snapshot_ready(mind_gtt_response_t *response);
void mind_gtt_response_snapshot_failed(mind_gtt_response_t *response);
int mind_gtt_response_is_busy(const mind_gtt_response_t *response);

/* Formats at most one LF-terminated GTT begin, entry, or end record.  A
 * malformed copied snapshot is discarded by releasing the UI request, rather
 * than reading outside its fixed 16-entry bound. */
int mind_gtt_response_format_next(mind_gtt_response_t *response,
                                  const routed_cycle_gtt_snapshot_t *snapshot,
                                  char *line_out, uint16_t line_capacity);

#endif /* MIND_GTT_RESPONSE_H */
