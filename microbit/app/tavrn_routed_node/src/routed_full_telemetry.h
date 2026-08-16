#ifndef ROUTED_FULL_TELEMETRY_H
#define ROUTED_FULL_TELEMETRY_H

/* FULL-only adapter boundary.  Do not include this header from AODV_ONLY
 * sources; routed_cycle.h deliberately remains GTT/FULL-free. */

#include "routed_cycle.h"
#include "tavrn_gtt.h"

typedef enum routed_full_telemetry_status {
    ROUTED_FULL_TELEMETRY_OK = 0,
    ROUTED_FULL_TELEMETRY_INVALID,
} routed_full_telemetry_status_t;

/* Copies every semantically retained occupied entry at query_at_ms: self,
 * ACTIVE, SOFT_STALE, HARD_EXPIRED, and an unexpired DEPARTED tombstone.  It
 * omits only expired tombstones, sorts canonical AdvA ascending, and counts
 * every nondeparted copied member (including self and HARD_EXPIRED) in
 * nondeparted_count.  It is a read-only adapter:
 * success and every invalid call preserve gtt, its storage, and counters
 * byte-for-byte. */
routed_full_telemetry_status_t routed_full_telemetry_snapshot_gtt(
    const tavrn_gtt_t *gtt, uint32_t query_at_ms,
    routed_cycle_gtt_snapshot_t *snapshot_out);

/* The routed trace sink retains this scalar between emitted sweep records.
 * Keep the monotonic comparison host-testable without moving FULL telemetry
 * state into the generic routed-cycle boundary. */
uint32_t routed_full_telemetry_max_scheduler_gap(uint32_t recorded_gap_ms,
                                                  uint32_t sample_gap_ms);

#endif /* ROUTED_FULL_TELEMETRY_H */
