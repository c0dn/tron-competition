/* Host-only routed scheduler port for future production-mode link tests. */
#include "ble_mesh_scheduler.h"

#include <string.h>

void ble_mesh_scheduler_init(ble_mesh_scheduler_t *sched, uint32_t now_ms,
                             const uint8_t local_adva[6])
{
    (void)now_ms;
    if (sched != NULL && local_adva != NULL) {
        memset(sched, 0, sizeof(*sched));
        memcpy(sched->local_adva, local_adva, 6u);
        sched->routed_started = 1u;
        ble_mesh_tx_queue_init(&sched->routed_tx_queue);
    }
}

int ble_mesh_scheduler_copy_local_adva(const ble_mesh_scheduler_t *sched,
                                       uint8_t out_adva[6])
{
    if (sched == NULL || out_adva == NULL || sched->routed_started == 0u) {
        return 0;
    }
    memcpy(out_adva, sched->local_adva, 6u);
    return 1;
}

ble_mesh_sched_enqueue_result_t ble_mesh_scheduler_enqueue_ex(
    ble_mesh_scheduler_t *sched, const ble_mesh_tx_item_t *item)
{
    ble_mesh_sched_enqueue_result_t result;
    ble_mesh_tx_enqueue_result_t queue_result;

    memset(&result, 0, sizeof(result));
    if (sched == NULL || item == NULL || sched->routed_started == 0u) {
        result.status = BLE_MESH_SCHED_ENQUEUE_INVALID;
        return result;
    }
    queue_result = ble_mesh_tx_queue_enqueue(&sched->routed_tx_queue, item);
    result.status = (ble_mesh_sched_enqueue_status_t)queue_result.status;
    result.accepted_token = queue_result.accepted_token;
    result.evicted_token = queue_result.evicted_token;
    return result;
}
