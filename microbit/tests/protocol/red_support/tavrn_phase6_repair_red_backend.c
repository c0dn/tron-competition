/* Intentional RED fallback: all Phase 6 repair policy is absent. */
#include "tavrn_phase6_repair_contract.h"

#include <string.h>

#if defined(TAVRN_PHASE6_REPAIR_RED_MODE)

static tavrn_repair_status_t unavailable(void)
{
    return TAVRN_REPAIR_UNAVAILABLE;
}

tavrn_repair_status_t phase6_repair_red_init(
    tavrn_repair_t *repair, const tavrn_repair_config_t *config)
{
    (void)config;
    if (repair != NULL) {
        memset(repair, 0, sizeof(*repair));
    }
    return unavailable();
}

tavrn_repair_status_t phase6_repair_red_retry_exhausted(
    tavrn_repair_t *repair, const tavrn_repair_retry_input_t *input,
    uint32_t now_ms, tavrn_repair_setup_result_t *result)
{
    (void)repair; (void)input; (void)now_ms;
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    return unavailable();
}

tavrn_repair_status_t phase6_repair_red_reserve_candidate(
    tavrn_repair_t *repair, const tavrn_link_data_t *data, uint32_t now_ms,
    tavrn_repair_candidate_result_t *result)
{
    (void)repair; (void)data; (void)now_ms;
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    return unavailable();
}

tavrn_repair_status_t phase6_repair_red_commit_candidate(
    tavrn_repair_t *repair, const tavrn_repair_reservation_t *reservation,
    const tavrn_link_data_t *data, uint32_t now_ms)
{
    (void)repair; (void)reservation; (void)data; (void)now_ms;
    return unavailable();
}

tavrn_repair_status_t phase6_repair_red_rollback_candidate(
    tavrn_repair_t *repair, const tavrn_repair_reservation_t *reservation,
    uint32_t now_ms)
{
    (void)repair; (void)reservation; (void)now_ms;
    return unavailable();
}

tavrn_repair_status_t phase6_repair_red_observe_link_custody(
    tavrn_repair_t *repair, const tavrn_repair_link_custody_terminal_input_t *input,
    tavrn_repair_link_custody_terminal_t terminal, uint32_t now_ms,
    tavrn_repair_link_custody_result_t *result)
{
    (void)repair; (void)input; (void)terminal; (void)now_ms;
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    return unavailable();
}

tavrn_repair_status_t phase6_repair_red_reconcile_link_custody(
    tavrn_repair_t *repair,
    const tavrn_repair_link_custody_snapshot_t *current_link_custody,
    uint32_t now_ms,
    tavrn_repair_link_custody_reconcile_result_t *result)
{
    (void)repair; (void)current_link_custody; (void)now_ms;
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
    }
    return unavailable();
}

tavrn_repair_status_t phase6_repair_red_owner_tick(
    tavrn_repair_t *repair, uint32_t now_ms, tavrn_repair_action_t *action)
{
    (void)repair; (void)now_ms;
    if (action != NULL) {
        memset(action, 0, sizeof(*action));
    }
    return unavailable();
}

tavrn_repair_status_t phase6_repair_red_rreq_enqueue(
    tavrn_repair_t *repair, const tavrn_repair_rreq_enqueue_t *enqueue,
    uint32_t now_ms)
{
    (void)repair; (void)enqueue; (void)now_ms;
    return unavailable();
}

tavrn_repair_status_t phase6_repair_red_data_action_not_admitted(
    tavrn_repair_t *repair, const tavrn_repair_action_t *action, uint32_t now_ms)
{
    (void)repair; (void)action; (void)now_ms;
    return unavailable();
}

tavrn_repair_status_t phase6_repair_red_complete(
    tavrn_repair_t *repair, const tavrn_repair_completion_t *completion,
    uint32_t now_ms, tavrn_repair_terminal_t *terminal)
{
    (void)repair; (void)completion; (void)now_ms;
    if (terminal != NULL) {
        memset(terminal, 0, sizeof(*terminal));
    }
    return unavailable();
}

tavrn_repair_status_t phase6_repair_red_receive_rrep(
    tavrn_repair_t *repair, const tavrn_repair_rrep_completion_t *completion,
    uint32_t now_ms, tavrn_repair_terminal_t *terminal)
{
    (void)repair; (void)completion; (void)now_ms;
    if (terminal != NULL) {
        memset(terminal, 0, sizeof(*terminal));
    }
    return unavailable();
}

tavrn_repair_status_t phase6_repair_red_snapshot(
    const tavrn_repair_t *repair, tavrn_repair_snapshot_t *snapshot)
{
    (void)repair;
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return unavailable();
}

tavrn_repair_status_t phase6_repair_red_active_rreq_snapshot(
    const tavrn_repair_t *repair, tavrn_repair_active_rreq_snapshot_t *snapshot)
{
    (void)repair;
    if (snapshot != NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
    }
    return unavailable();
}

#endif /* TAVRN_PHASE6_REPAIR_RED_MODE */
