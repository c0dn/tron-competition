/*
 * mind_mesh — ESP-MESH-LITE bring-up and node<->root transport.
 *
 * Brings up WiFi (AP+STA) and the self-organizing mesh, exposes the node's
 * role/health, forwards raw binary records up to the root, and (on the root)
 * delivers received leaf records to a callback for MQTT republish.
 *
 * The root election, tree forming, and NAT-to-router are handled by mesh-lite;
 * this component is the thin MIND-specific wrapper around it.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Snapshot of this node's mesh position, for the status record. */
typedef struct {
    bool    is_root;      /* mesh level == 1                        */
    uint8_t level;        /* mesh level (root = 1, 0 = not joined)  */
    int8_t  parent_rssi;  /* RSSI to parent/router, dBm (0 if n/a)  */
    uint8_t child_count;  /* direct SoftAP children                 */
} mind_mesh_status_t;

/*
 * Called on the ROOT node for each raw record received from a descendant.
 * `data`/`len` are valid only for the duration of the call — copy out what you
 * need. Runs on the mesh receive context, so keep it short (enqueue + return).
 */
typedef void (*mind_mesh_raw_rx_cb_t)(const uint8_t *data, uint32_t len);

/*
 * Start WiFi + mesh-lite. `on_root_rx` may be NULL if this build never acts as
 * root; otherwise it is invoked when this node is (or becomes) the root and a
 * leaf record arrives. Returns ESP_OK once the mesh is starting.
 */
esp_err_t mind_mesh_start(mind_mesh_raw_rx_cb_t on_root_rx);

/* True if this node is currently the mesh root. */
bool mind_mesh_is_root(void);

/* Fill `out` with the current mesh position/health. */
void mind_mesh_get_status(mind_mesh_status_t *out);

/*
 * Forward a raw binary record toward the root over the mesh. Safe to call from
 * any node; on the root itself the caller should publish locally instead (see
 * mind_mesh_is_root). Returns the underlying mesh-lite send result.
 */
esp_err_t mind_mesh_send_to_root(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
