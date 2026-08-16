/*
 * RED-only incarnation binding.  It links the actual wire/link/router/AODV
 * implementation and intentionally does not add an incarnation owner, reset
 * state, or routing substitute.  It merely exposes the frozen future API as
 * the old router's current no-incarnation behavior.
 */
#include "tavrn_phase4_incarnation_contract.h"

#ifndef TAVRN_ROUTER_INCARNATION_API

#include <string.h>

static tavrn_router_incarnation_counters_t red_counters;

tavrn_router_incarnation_status_t tavrn_router_init_with_incarnation(
    tavrn_router_t *router, tavrn_link_v2_t *link, aodv_core_t *aodv,
    const tavrn_router_augmentation_hooks_t *augmentation_or_null,
    const tavrn_router_incarnation_config_t *config, uint32_t now_ms)
{
    (void)config;
    (void)now_ms;
    memset(&red_counters, 0, sizeof(red_counters));
    return tavrn_router_init(router, link, aodv, augmentation_or_null) ==
            TAVRN_ROUTER_INIT_OK ? TAVRN_ROUTER_INCARNATION_OK :
            TAVRN_ROUTER_INCARNATION_INVALID;
}

tavrn_router_incarnation_status_t tavrn_router_incarnation_snapshot(
    const tavrn_router_t *router,
    tavrn_router_incarnation_snapshot_t *snapshot_out)
{
    if (router == NULL || snapshot_out == NULL) {
        return TAVRN_ROUTER_INCARNATION_INVALID;
    }
    memset(snapshot_out, 0, sizeof(*snapshot_out));
    /* Existing routed-common behavior has no boot barrier, so it is observably
     * established with no bootstrap record. */
    snapshot_out->state = TAVRN_ROUTER_INCARNATION_ESTABLISHED;
    return TAVRN_ROUTER_INCARNATION_OK;
}

tavrn_router_incarnation_status_t tavrn_router_incarnation_peer_snapshot(
    const tavrn_router_t *router, const tavrn_adva_t *peer_adva,
    tavrn_router_incarnation_peer_snapshot_t *snapshot_out)
{
    if (router == NULL || peer_adva == NULL || snapshot_out == NULL) {
        return TAVRN_ROUTER_INCARNATION_INVALID;
    }
    memset(snapshot_out, 0, sizeof(*snapshot_out));
    return TAVRN_ROUTER_INCARNATION_NOT_FOUND;
}

const tavrn_router_incarnation_counters_t *tavrn_router_incarnation_counters(
    const tavrn_router_t *router)
{
    return router == NULL ? NULL : &red_counters;
}

#endif /* TAVRN_ROUTER_INCARNATION_API */
