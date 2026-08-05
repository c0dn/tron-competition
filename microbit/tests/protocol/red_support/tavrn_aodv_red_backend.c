/* Test-only RED AODV port.  Never link this file into firmware. */
#include "aodv_core.h"

#include <string.h>

int aodv_serial_is_newer(uint16_t candidate, uint16_t current)
{
    (void)candidate;
    (void)current;
    return 0;
}

aodv_init_status_t aodv_core_init(aodv_core_t *core,
                                  const aodv_core_config_t *config,
                                  uint32_t now_ms)
{
    (void)now_ms;
    if (core == NULL || config == NULL) {
        return AODV_INIT_INVALID_ARGUMENT;
    }
    memset(core, 0, sizeof(*core));
    core->config = *config;
    return AODV_INIT_OK;
}

aodv_status_t aodv_core_submit_application(aodv_core_t *core,
                                           const tron_application_data_t *data,
                                           uint32_t now_ms)
{
    (void)core;
    (void)data;
    (void)now_ms;
    return AODV_STATUS_INVALID;
}

aodv_status_t aodv_core_ingest_control(aodv_core_t *core,
                                        const aodv_control_input_t *input,
                                        uint32_t now_ms)
{
    (void)core;
    (void)input;
    (void)now_ms;
    return AODV_STATUS_INVALID;
}

aodv_status_t aodv_core_ingest_data(aodv_core_t *core,
                                     const aodv_data_input_t *input,
                                     uint32_t now_ms)
{
    (void)core;
    (void)input;
    (void)now_ms;
    return AODV_STATUS_INVALID;
}

aodv_status_t aodv_core_mark_action_sent(aodv_core_t *core,
                                          uint16_t action_token,
                                          uint32_t now_ms)
{
    (void)core;
    (void)action_token;
    (void)now_ms;
    return AODV_STATUS_INVALID;
}

aodv_status_t aodv_core_tick(aodv_core_t *core, uint32_t now_ms)
{
    (void)core;
    (void)now_ms;
    return AODV_STATUS_INVALID;
}

aodv_action_poll_status_t aodv_core_poll_action(aodv_core_t *core,
                                                 aodv_action_t *action_out)
{
    (void)core;
    if (action_out != NULL) {
        memset(action_out, 0, sizeof(*action_out));
        action_out->type = AODV_ACTION_NONE;
    }
    return AODV_ACTION_POLL_EMPTY;
}

aodv_route_query_status_t aodv_core_route_snapshot(
    const aodv_core_t *core, const tavrn_logical_id_t *destination,
    aodv_route_snapshot_t *snapshot_out)
{
    (void)core;
    (void)destination;
    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    return AODV_ROUTE_QUERY_NOT_FOUND;
}

const aodv_counters_t *aodv_core_counters(const aodv_core_t *core)
{
    return core == NULL ? NULL : &core->counters;
}

aodv_failure_status_t aodv_core_report_link_failure(
    aodv_core_t *core, const tavrn_direct_peer_t *failed_next_hop,
    const tavrn_logical_id_t *repair_destination,
    aodv_link_failure_mode_t mode, uint32_t now_ms)
{
    (void)core;
    (void)failed_next_hop;
    (void)repair_destination;
    (void)mode;
    (void)now_ms;
    return AODV_FAILURE_INVALID;
}

aodv_failure_status_t aodv_core_finish_deferred_rerr(
    aodv_core_t *core, const tavrn_logical_id_t *repair_destination,
    aodv_deferred_rerr_decision_t decision, uint32_t now_ms)
{
    (void)core;
    (void)repair_destination;
    (void)decision;
    (void)now_ms;
    return AODV_FAILURE_INVALID;
}
