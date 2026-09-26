#ifndef ROUTED_BENCHMARK_FULL_H
#define ROUTED_BENCHMARK_FULL_H

#include <stdint.h>

#include "tavrn_full.h"

typedef enum routed_benchmark_destination_status {
    ROUTED_BENCHMARK_DESTINATION_READY = 0,
    ROUTED_BENCHMARK_DESTINATION_UNKNOWN,
    ROUTED_BENCHMARK_DESTINATION_COLLIDING,
    ROUTED_BENCHMARK_DESTINATION_RESERVED,
    ROUTED_BENCHMARK_DESTINATION_NOT_ACTIVE,
    ROUTED_BENCHMARK_DESTINATION_INVALID,
} routed_benchmark_destination_status_t;

/* A resolved FULL destination remains admissible for AODV while it is a unique
 * non-departed GTT identity.  The caller passes this request directly to the
 * normal mentorship/router submission path; the router's existing FULL hook
 * selects a soft-stale Smart-TTL scope or no hint for hard expiry/zero hop. */
typedef struct routed_benchmark_destination_submission {
    tavrn_logical_id_t logical_destination;
    tavrn_gtt_freshness_t freshness;
    uint8_t hop_count;
} routed_benchmark_destination_submission_t;

/* A FULL benchmark may use SID8 only after the configured full AdvA has a
 * unique, non-departed production ESC context and a retained non-departed GTT
 * snapshot.  The resolver is intentionally the production one. */
routed_benchmark_destination_status_t routed_benchmark_destination_ready(
    const tavrn_full_t *full, const tavrn_adva_t *configured_destination,
    uint32_t now_ms, tavrn_logical_id_t *logical_destination_out);

routed_benchmark_destination_status_t routed_benchmark_destination_prepare(
    const tavrn_full_t *full, const tavrn_adva_t *configured_destination,
    uint32_t now_ms, routed_benchmark_destination_submission_t *submission_out);

#endif /* ROUTED_BENCHMARK_FULL_H */
