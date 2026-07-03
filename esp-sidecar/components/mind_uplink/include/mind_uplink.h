/*
 * mind_uplink — aggregate-at-root transport.
 *
 * Owns the data path from decoded observations to the broker:
 *   - drains the BLE observation queue, packs uplink event records;
 *   - starts the mesh (via mind_mesh) and, on the ROOT, an MQTT client;
 *   - forward-or-publish: leaf nodes send records to the root over the mesh,
 *     the root republishes every record (its own + descendants') over MQTT;
 *   - periodically emits this node's status (health / mesh position).
 *
 * Records use the shared/uplink_schema.h binary contract (passthrough: the
 * root does not re-encode). MQTT topics: mind/ingest/event, mind/ingest/status,
 * and retained presence on mind/node/<id>/lwt.
 */
#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start the uplink layer. `obs_q` is the observation queue fed by
 * mind_ble_observer_start (items of type mind_observation_t). This call brings
 * up the mesh and spawns the forwarder + status tasks. Returns ESP_OK on
 * success.
 */
esp_err_t mind_uplink_start(QueueHandle_t obs_q);

#ifdef __cplusplus
}
#endif
