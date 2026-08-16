#include "routed_benchmark_full.h"

#include <string.h>

routed_benchmark_destination_status_t routed_benchmark_destination_prepare(
    const tavrn_full_t *full, const tavrn_adva_t *configured_destination,
    uint32_t now_ms, routed_benchmark_destination_submission_t *submission_out)
{
    tavrn_esc_context_status_t context_status;
    tavrn_gtt_snapshot_t snapshot;

    if (submission_out != NULL) {
        memset(submission_out, 0, sizeof(*submission_out));
    }
    if (full == NULL || full->gtt == NULL || configured_destination == NULL ||
        submission_out == NULL) {
        return ROUTED_BENCHMARK_DESTINATION_INVALID;
    }
    context_status = tavrn_full_resolve_unique_sid8(
        full, configured_destination, &submission_out->logical_destination);
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
        snapshot.freshness == TAVRN_GTT_FRESHNESS_DEPARTED) {
        memset(submission_out, 0, sizeof(*submission_out));
        return ROUTED_BENCHMARK_DESTINATION_NOT_ACTIVE;
    }
    submission_out->freshness = snapshot.freshness;
    submission_out->hop_count = snapshot.hop_count;
    return ROUTED_BENCHMARK_DESTINATION_READY;
}

routed_benchmark_destination_status_t routed_benchmark_destination_ready(
    const tavrn_full_t *full, const tavrn_adva_t *configured_destination,
    uint32_t now_ms, tavrn_logical_id_t *logical_destination_out)
{
    routed_benchmark_destination_submission_t submission;
    routed_benchmark_destination_status_t status;

    if (logical_destination_out != NULL) {
        memset(logical_destination_out, 0, sizeof(*logical_destination_out));
    }
    if (logical_destination_out == NULL) {
        return ROUTED_BENCHMARK_DESTINATION_INVALID;
    }
    status = routed_benchmark_destination_prepare(full, configured_destination,
                                                  now_ms, &submission);
    if (status == ROUTED_BENCHMARK_DESTINATION_READY) {
        *logical_destination_out = submission.logical_destination;
    }
    return status;
}
