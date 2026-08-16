/* RED fallback: preserve the real owner objects but implement no policy. */
#include "tavrn_phase5_targeted_freshness_contract.h"

#include <string.h>

#if defined(TAVRN_PHASE5_TARGETED_RED_MODE)

static void inert_action(tavrn_targeted_freshness_action_t *action_out)
{
    if (action_out != NULL) {
        memset(action_out, 0, sizeof(*action_out));
        action_out->context_index = 0u;
    }
}

tavrn_targeted_freshness_status_t phase5_targeted_red_begin_stage0(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, const tavrn_adva_t *subject,
    uint32_t now_ms, tavrn_targeted_freshness_action_t *action_out)
{
    (void)maintenance; (void)router; (void)link; (void)gtt; (void)subject; (void)now_ms;
    inert_action(action_out);
    return TAVRN_TARGETED_FRESHNESS_OK;
}

tavrn_targeted_freshness_status_t phase5_targeted_red_receive(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt,
    const tavrn_targeted_freshness_rx_input_t *input, uint32_t now_ms,
    tavrn_targeted_freshness_action_t *action_out)
{
    (void)maintenance; (void)router; (void)link; (void)gtt; (void)input; (void)now_ms;
    inert_action(action_out);
    return TAVRN_TARGETED_FRESHNESS_OK;
}

tavrn_targeted_freshness_status_t phase5_targeted_red_owner_tick(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, uint32_t now_ms,
    tavrn_targeted_freshness_action_t *action_out)
{
    (void)maintenance; (void)router; (void)link; (void)gtt; (void)now_ms;
    inert_action(action_out);
    return TAVRN_TARGETED_FRESHNESS_OK;
}

tavrn_targeted_freshness_status_t phase5_targeted_red_revalidate(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, const tavrn_adva_t *subject,
    uint32_t now_ms)
{
    (void)maintenance; (void)router; (void)link; (void)gtt; (void)subject; (void)now_ms;
    return TAVRN_TARGETED_FRESHNESS_OK;
}

tavrn_targeted_freshness_status_t phase5_targeted_red_terminal(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, const ble_mesh_sched_event_t *event, uint32_t now_ms)
{
    (void)maintenance; (void)router; (void)link; (void)event; (void)now_ms;
    return TAVRN_TARGETED_FRESHNESS_OK;
}

tavrn_targeted_freshness_status_t phase5_targeted_red_cancel_subject(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, const tavrn_adva_t *subject,
    uint32_t now_ms)
{
    (void)maintenance; (void)router; (void)link; (void)gtt; (void)subject; (void)now_ms;
    return TAVRN_TARGETED_FRESHNESS_OK;
}

tavrn_targeted_freshness_status_t phase5_targeted_red_evicted(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, uint16_t token, uint32_t now_ms)
{
    (void)maintenance; (void)router; (void)link; (void)token; (void)now_ms;
    return TAVRN_TARGETED_FRESHNESS_OK;
}

tavrn_targeted_freshness_status_t phase5_targeted_red_seed_high_token(
    tavrn_maintenance_t *maintenance, uint16_t next_token)
{
    (void)maintenance; (void)next_token;
    return TAVRN_TARGETED_FRESHNESS_OK;
}

tavrn_targeted_freshness_status_t phase5_targeted_red_snapshot(
    const tavrn_maintenance_t *maintenance, const tavrn_router_t *router,
    const tavrn_link_v2_t *link, const tavrn_gtt_t *gtt,
    tavrn_targeted_freshness_snapshot_t *snapshot_out)
{
    (void)maintenance; (void)router; (void)link; (void)gtt;
    if (snapshot_out == NULL) {
        return TAVRN_TARGETED_FRESHNESS_INVALID;
    }
    memset(snapshot_out, 0, sizeof(*snapshot_out));
    return TAVRN_TARGETED_FRESHNESS_OK;
}

tavrn_targeted_freshness_status_t phase5_targeted_red_reserve_low_token(
    tavrn_link_v2_t *link, tavrn_targeted_freshness_low_work_t work,
    uint16_t *token_out)
{
    uint16_t candidate;

    (void)work;
    if (link == NULL || token_out == NULL) {
        return TAVRN_TARGETED_FRESHNESS_INVALID;
    }
    candidate = (uint16_t)(link->next_scheduler_token + 1u);
    if (candidate == 0u || candidate > 0x7fffu) {
        candidate = 1u;
    }
    link->next_scheduler_token = candidate;
    *token_out = candidate;
    return TAVRN_TARGETED_FRESHNESS_OK;
}

#endif /* TAVRN_PHASE5_TARGETED_RED_MODE */
