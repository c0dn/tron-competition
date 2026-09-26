/* RED fallback: real Stage-0/GTT/AODV/link owners are present, but this backend
 * deliberately adds no retained-hop/full-diameter verification behavior. */
#include "tavrn_phase5_rreq_verification_contract.h"

#include <string.h>

#if defined(TAVRN_PHASE5_RREQ_VERIFICATION_RED_MODE)

tavrn_rreq_verification_status_t phase5_rreq_red_owner_tick(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, uint32_t now_ms,
    tavrn_rreq_verification_action_t *action_out)
{
    (void)maintenance; (void)router; (void)link; (void)gtt; (void)now_ms;
    if (action_out != NULL) {
        memset(action_out, 0, sizeof(*action_out));
    }
    return TAVRN_RREQ_VERIFICATION_OK;
}

tavrn_rreq_verification_status_t phase5_rreq_red_terminal(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link,
    const tavrn_rreq_verification_completion_t *completion, uint32_t now_ms)
{
    (void)maintenance; (void)router; (void)link; (void)completion; (void)now_ms;
    return TAVRN_RREQ_VERIFICATION_OK;
}

tavrn_rreq_verification_status_t phase5_rreq_red_snapshot(
    const tavrn_maintenance_t *maintenance, const tavrn_router_t *router,
    const tavrn_link_v2_t *link, const tavrn_gtt_t *gtt,
    tavrn_rreq_verification_snapshot_t *snapshot_out)
{
    (void)maintenance; (void)router; (void)link; (void)gtt;
    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    return TAVRN_RREQ_VERIFICATION_OK;
}

#endif /* TAVRN_PHASE5_RREQ_VERIFICATION_RED_MODE */
