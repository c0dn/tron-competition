#include "routed_benchmark_full.h"

#include <string.h>

routed_benchmark_destination_status_t routed_benchmark_destination_ready(
    const tavrn_full_t *full, const tavrn_adva_t *configured_destination,
    uint32_t now_ms, tavrn_logical_id_t *logical_destination_out)
{
    tavrn_esc_context_status_t context_status;
    tavrn_gtt_snapshot_t snapshot;

    if (logical_destination_out != NULL) {
        memset(logical_destination_out, 0, sizeof(*logical_destination_out));
    }
    if (full == NULL || full->gtt == NULL || configured_destination == NULL ||
        logical_destination_out == NULL) {
        return ROUTED_BENCHMARK_DESTINATION_INVALID;
    }
    context_status = tavrn_full_resolve_unique_sid8(
        full, configured_destination, logical_destination_out);
    if (context_status == TAVRN_ESC_CONTEXT_UNKNOWN) {
        return ROUTED_BENCHMARK_DESTINATION_UNKNOWN;
    }
    if (context_status == TAVRN_ESC_CONTEXT_COLLIDING) {
        return ROUTED_BENCHMARK_DESTINATION_COLLIDING;
    }
    if (context_status == TAVRN_ESC_CONTEXT_RESERVED) {
        return ROUTED_BENCHMARK_DESTINATION_RESERVED;
    }
    if (context_status != TAVRN_ESC_CONTEXT_UNIQUE ||
        tavrn_gtt_snapshot(full->gtt, configured_destination, now_ms, &snapshot) !=
            TAVRN_GTT_QUERY_FOUND ||
        snapshot.freshness != TAVRN_GTT_FRESHNESS_ACTIVE) {
        memset(logical_destination_out, 0, sizeof(*logical_destination_out));
        return ROUTED_BENCHMARK_DESTINATION_NOT_ACTIVE;
    }
    return ROUTED_BENCHMARK_DESTINATION_READY;
}
