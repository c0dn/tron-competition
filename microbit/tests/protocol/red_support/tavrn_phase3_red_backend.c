/* Test-only Phase 3 RED port. Never link this file into firmware. */
#include "tavrn_gtt.h"
#include "tavrn_full.h"
#include "tavrn_router.h"
#include "tavrn_smart_ttl.h"

#include <string.h>

int tavrn_gtt_serial_is_newer(uint16_t candidate, uint16_t current)
{
    (void)candidate;
    (void)current;
    return 0;
}

tavrn_gtt_init_status_t tavrn_gtt_init(tavrn_gtt_t *gtt,
                                        tavrn_gtt_storage_t *storage,
                                        const tavrn_gtt_config_t *config,
                                        uint32_t now_ms)
{
    (void)now_ms;
    if (gtt == NULL || storage == NULL || config == NULL) {
        return TAVRN_GTT_INIT_INVALID_ARGUMENT;
    }
    memset(gtt, 0, sizeof(*gtt));
    memset(storage, 0, sizeof(*storage));
    gtt->storage = storage;
    gtt->config = *config;
    return TAVRN_GTT_INIT_OK;
}

tavrn_gtt_observe_status_t tavrn_gtt_observe(
    tavrn_gtt_t *gtt, const tavrn_gtt_evidence_t *evidence, uint32_t now_ms)
{
    (void)gtt;
    (void)evidence;
    (void)now_ms;
    return TAVRN_GTT_OBSERVE_INVALID;
}

tavrn_gtt_query_status_t tavrn_gtt_snapshot(
    const tavrn_gtt_t *gtt, const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_snapshot_t *snapshot_out)
{
    (void)gtt;
    (void)identity;
    (void)now_ms;
    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    return TAVRN_GTT_QUERY_NOT_FOUND;
}

tavrn_gtt_query_status_t tavrn_gtt_enumerate_active(
    const tavrn_gtt_t *gtt, uint32_t now_ms,
    tavrn_gtt_snapshot_t *snapshots_out, uint8_t snapshot_capacity,
    uint8_t *snapshot_count_out)
{
    (void)gtt;
    (void)now_ms;
    (void)snapshots_out;
    (void)snapshot_capacity;
    if (snapshot_count_out != NULL) {
        *snapshot_count_out = 0u;
    }
    return TAVRN_GTT_QUERY_NOT_FOUND;
}

const tavrn_gtt_counters_t *tavrn_gtt_counters(const tavrn_gtt_t *gtt)
{
    return gtt == NULL ? NULL : &gtt->counters;
}

aodv_status_t aodv_core_submit_application_scoped(
    aodv_core_t *core, const tron_application_data_t *data,
    uint8_t initial_scope, uint32_t now_ms)
{
    (void)initial_scope;
    return aodv_core_submit_application(core, data, now_ms);
}

tavrn_smart_ttl_decision_t tavrn_smart_ttl_decide(
    const tavrn_gtt_snapshot_t *snapshot_or_null, uint8_t net_diameter)
{
    tavrn_smart_ttl_decision_t decision;

    (void)snapshot_or_null;
    decision.has_hint = 0u;
    decision.initial_scope = net_diameter;
    return decision;
}

tavrn_full_init_status_t tavrn_full_init(tavrn_full_t *full, tavrn_gtt_t *gtt)
{
    if (full == NULL || gtt == NULL) {
        return TAVRN_FULL_INIT_INVALID_ARGUMENT;
    }
    full->gtt = gtt;
    return TAVRN_FULL_INIT_OK;
}

tavrn_router_augmentation_hooks_t tavrn_full_router_hooks(tavrn_full_t *full)
{
    tavrn_router_augmentation_hooks_t hooks;

    memset(&hooks, 0, sizeof(hooks));
    hooks.context = full;
    return hooks;
}

tavrn_router_init_status_t tavrn_router_init(
    tavrn_router_t *router, tavrn_link_v2_t *link, aodv_core_t *aodv,
    const tavrn_router_augmentation_hooks_t *augmentation_or_null)
{
    if (router == NULL || link == NULL || aodv == NULL) {
        return TAVRN_ROUTER_INIT_INVALID_ARGUMENT;
    }
    memset(router, 0, sizeof(*router));
    router->link = link;
    router->aodv = aodv;
    if (augmentation_or_null != NULL) {
        router->augmentation = *augmentation_or_null;
    }
    return TAVRN_ROUTER_INIT_OK;
}

tavrn_router_event_status_t tavrn_router_handle_link_event(
    tavrn_router_t *router, const tavrn_link_event_t *event, uint32_t now_ms)
{
    (void)router;
    (void)event;
    (void)now_ms;
    return TAVRN_ROUTER_EVENT_IGNORED;
}

tavrn_router_event_status_t tavrn_router_handle_scheduler_event(
    tavrn_router_t *router, const ble_mesh_sched_event_t *event,
    uint32_t now_ms)
{
    (void)router;
    (void)event;
    (void)now_ms;
    return TAVRN_ROUTER_EVENT_IGNORED;
}

aodv_status_t tavrn_router_submit_application(
    tavrn_router_t *router, const tron_application_data_t *data,
    uint32_t now_ms)
{
    if (router == NULL) {
        return AODV_STATUS_INVALID;
    }
    return aodv_core_submit_application(router->aodv, data, now_ms);
}

aodv_status_t tavrn_router_tick(tavrn_router_t *router, uint32_t now_ms)
{
    return router == NULL ? AODV_STATUS_INVALID :
        aodv_core_tick(router->aodv, now_ms);
}

tavrn_router_observe_status_t tavrn_router_observe_frame(
    tavrn_router_t *router,
    const tavrn_router_frame_observation_t *frame_observation,
    uint32_t now_ms)
{
    (void)router;
    (void)frame_observation;
    (void)now_ms;
    return TAVRN_ROUTER_OBSERVE_IGNORED;
}

tavrn_router_scope_status_t tavrn_router_initial_scope(
    const tavrn_router_t *router, const tavrn_logical_id_t *destination,
    uint8_t full_scope, uint32_t now_ms,
    tavrn_router_scope_hint_t *hint_out)
{
    (void)destination;
    (void)now_ms;
    if (router == NULL || hint_out == NULL) {
        return TAVRN_ROUTER_SCOPE_INVALID;
    }
    hint_out->has_hint = 0u;
    hint_out->initial_scope = full_scope;
    return TAVRN_ROUTER_SCOPE_OK;
}
