/* Test-only RED link port. Never link this file into firmware. */
#include "tavrn_link_v2.h"

#include <string.h>

static void clear_event(tavrn_link_event_t *event)
{
    if (event != NULL) {
        memset(event, 0, sizeof(*event));
        event->type = TAVRN_LINK_EVENT_NONE;
    }
}

void ble_mesh_scheduler_init(ble_mesh_scheduler_t *sched, uint32_t now_ms,
                             const uint8_t local_adva[6])
{
    (void)now_ms;
    if (sched != NULL && local_adva != NULL) {
        memset(sched, 0, sizeof(*sched));
        memcpy(sched->local_adva, local_adva, TAVRN_ADVA_LEN);
        sched->routed_started = 1u;
    }
}

int ble_mesh_scheduler_copy_local_adva(const ble_mesh_scheduler_t *sched,
                                       uint8_t out_adva[6])
{
    if (sched == NULL || out_adva == NULL || sched->routed_started == 0u) {
        return 0;
    }
    memcpy(out_adva, sched->local_adva, TAVRN_ADVA_LEN);
    return 1;
}

ble_mesh_sched_enqueue_result_t ble_mesh_scheduler_enqueue_ex(
    ble_mesh_scheduler_t *sched, const ble_mesh_tx_item_t *item)
{
    ble_mesh_sched_enqueue_result_t result;

    (void)sched;
    (void)item;
    memset(&result, 0, sizeof(result));
    result.status = BLE_MESH_SCHED_ENQUEUE_INVALID;
    return result;
}

tavrn_link_init_status_t tavrn_link_v2_init(
    tavrn_link_v2_t *link, ble_mesh_scheduler_t *sched,
    const tavrn_link_config_t *config, uint32_t now_ms)
{
    (void)now_ms;
    if (link == NULL || sched == NULL || config == NULL) {
        return TAVRN_LINK_INIT_INVALID_ARGUMENT;
    }
    memset(link, 0, sizeof(*link));
    link->scheduler = sched;
    link->config = *config;
    link->active_custody_index = TAVRN_CUSTODY_SLOT_NONE;
    return TAVRN_LINK_INIT_OK;
}

tavrn_link_send_status_t tavrn_link_v2_send_unicast(
    tavrn_link_v2_t *link, const tavrn_direct_peer_t *next_hop,
    const tavrn_link_data_t *data, uint32_t now_ms,
    tavrn_link_event_t *local_outcome)
{
    (void)link;
    (void)next_hop;
    (void)data;
    (void)now_ms;
    clear_event(local_outcome);
    return TAVRN_LINK_SEND_INVALID;
}

tavrn_link_send_status_t tavrn_link_v2_send_flood(
    tavrn_link_v2_t *link, const tavrn_codec_flood_t *flood,
    uint32_t now_ms, tavrn_link_event_t *local_outcome)
{
    (void)link;
    (void)flood;
    (void)now_ms;
    clear_event(local_outcome);
    return TAVRN_LINK_SEND_INVALID;
}

tavrn_link_resolve_status_t tavrn_link_v2_resolve_rx(
    tavrn_link_v2_t *link, tavrn_rx_candidate_token_t token,
    tavrn_rx_decision_t decision, uint32_t now_ms,
    tavrn_link_event_t *local_outcome)
{
    (void)link;
    (void)token;
    (void)decision;
    (void)now_ms;
    clear_event(local_outcome);
    return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
}

tavrn_link_resolve_status_t tavrn_link_v2_release_rx_custody(
    tavrn_link_v2_t *link, const tavrn_link_data_t *data, uint32_t now_ms)
{
    (void)link;
    (void)data;
    (void)now_ms;
    return TAVRN_LINK_RESOLVE_TOKEN_INVALID;
}

tavrn_link_step_status_t tavrn_link_v2_on_scheduler_event(
    tavrn_link_v2_t *link, const ble_mesh_sched_event_t *input,
    uint32_t now_ms, tavrn_link_event_t *output)
{
    (void)link;
    (void)input;
    (void)now_ms;
    clear_event(output);
    return TAVRN_LINK_STEP_INVALID;
}

tavrn_link_step_status_t tavrn_link_v2_tick(
    tavrn_link_v2_t *link, uint32_t now_ms, tavrn_link_event_t *output)
{
    (void)link;
    (void)now_ms;
    clear_event(output);
    return TAVRN_LINK_STEP_INVALID;
}

tavrn_link_step_status_t tavrn_link_v2_dispatch(
    tavrn_link_v2_t *link, uint32_t now_ms, tavrn_link_event_t *local_outcome)
{
    (void)link;
    (void)now_ms;
    clear_event(local_outcome);
    return TAVRN_LINK_STEP_INVALID;
}

const tavrn_link_counters_t *tavrn_link_v2_counters(const tavrn_link_v2_t *link)
{
    return link == NULL ? NULL : &link->counters;
}
