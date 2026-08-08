/* RED-only lifecycle bridge.  It links real wire/link/router/GTT production
 * objects but intentionally implements none of the Step 5a maintenance
 * contract. */
#include "tavrn_phase5_maintenance_contract.h"

#ifndef TAVRN_MAINTENANCE_API

#include <string.h>

static int config_is_valid(const tavrn_maintenance_config_t *config)
{
    if (config == NULL || config->hello_change_ms == 0u ||
        config->hello_change_ms >= 0x80000000u ||
        config->hello_dedupe_ms == 0u || config->hello_dedupe_ms >= 0x80000000u) {
        return 0;
    }
#ifdef TAVRN_MAINTENANCE_ADAPTIVE_CONTRACT
    return config->hello_stable_ms >= config->hello_change_ms &&
        config->hello_stable_ms < 0x80000000u &&
        config->hello_alpha_permille >= 1u &&
        config->hello_alpha_permille <= 1000u &&
        config->hello_snap_permille >= 1u &&
        config->hello_snap_permille <= 1000u &&
        config->topology_sample_ms != 0u &&
        config->topology_sample_ms < 0x80000000u;
#else
    return config->hello_change_ms != 0u &&
        config->hello_change_ms < 0x80000000u &&
        config->hello_dedupe_ms != 0u && config->hello_dedupe_ms < 0x80000000u;
#endif
}

tavrn_maintenance_status_t tavrn_maintenance_init(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router, tavrn_gtt_t *gtt,
    const tavrn_maintenance_config_t *config)
{
    if (maintenance == NULL || router == NULL || gtt == NULL || !config_is_valid(config)) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    memset(maintenance, 0, sizeof(*maintenance));
    maintenance->router = router;
    maintenance->gtt = gtt;
    maintenance->config = *config;
    maintenance->snapshot.next_node_sequence = config->initial_node_sequence;
    return TAVRN_MAINTENANCE_OK;
}

tavrn_maintenance_status_t tavrn_maintenance_activate(
    tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    (void)now_ms;
    if (maintenance == NULL || maintenance->router == NULL || maintenance->gtt == NULL) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (maintenance->router->link == NULL ||
        maintenance->router->link->config.local_peer.logical_id.width !=
            TAVRN_IDENTITY_SID8) {
        return TAVRN_MAINTENANCE_GATED;
    }
    /* Intentionally no arming or cadence behavior. */
    return TAVRN_MAINTENANCE_OK;
}

tavrn_maintenance_status_t tavrn_maintenance_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    (void)now_ms;
    return maintenance != NULL ? TAVRN_MAINTENANCE_OK :
                                 TAVRN_MAINTENANCE_INVALID;
}

tavrn_maintenance_status_t tavrn_maintenance_handle_rx_control(
    tavrn_maintenance_t *maintenance,
    const tavrn_rx_control_event_t *control_event, uint32_t now_ms)
{
    (void)control_event;
    (void)now_ms;
    return maintenance != NULL ? TAVRN_MAINTENANCE_IGNORED :
                                 TAVRN_MAINTENANCE_INVALID;
}

tavrn_maintenance_status_t tavrn_maintenance_observe_local_broadcast(
    tavrn_maintenance_t *maintenance, uint32_t accepted_at_ms)
{
    (void)accepted_at_ms;
    return maintenance != NULL ? TAVRN_MAINTENANCE_OK :
                                 TAVRN_MAINTENANCE_INVALID;
}

tavrn_maintenance_status_t tavrn_maintenance_snapshot(
    const tavrn_maintenance_t *maintenance,
    tavrn_maintenance_snapshot_t *snapshot_out)
{
    if (maintenance == NULL || snapshot_out == NULL) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    *snapshot_out = maintenance->snapshot;
    return TAVRN_MAINTENANCE_OK;
}

const tavrn_maintenance_counters_t *tavrn_maintenance_counters(
    const tavrn_maintenance_t *maintenance)
{
    return maintenance == NULL ? NULL : &maintenance->counters;
}

#endif /* TAVRN_MAINTENANCE_API */
