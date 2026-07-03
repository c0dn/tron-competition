/*
 * mind_ble — passive BLE observer for MIND wearable beacons.
 *
 * Runs the NimBLE host in observer role, decodes schema-v1 Manufacturer
 * Specific Data adverts from micro:bit wearables, de-duplicates the wearable's
 * repeated incident bursts, and pushes each fresh observation to a queue for
 * the uplink layer to forward over the mesh.
 *
 * Wire contract: shared/schema.h (single source of truth, shared with the
 * micro:bit encoder). Both sides are little-endian, so the MSD payload maps
 * byte-for-byte onto mind_adv_payload_t.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "schema.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One decoded, de-duplicated wearable observation. */
typedef struct {
    uint8_t            device_id;  /* micro:bit DEVICE_ID (from AdvA[4])        */
    int8_t             rssi;       /* receive strength, dBm (negative)          */
    int64_t            rx_us;      /* esp_timer_get_time() at reception         */
    mind_adv_payload_t payload;    /* 7-byte schema-v1 payload, copied out      */
} mind_observation_t;

/*
 * Start the NimBLE observer. Fresh observations (type mind_observation_t) are
 * sent to `out_q` from the NimBLE host task. The queue must already exist and
 * hold items of sizeof(mind_observation_t). Non-blocking sends: if the queue is
 * full the observation is dropped (liveness over backlog).
 *
 * Returns ESP_OK once the host task is started, or an esp_err_t from NimBLE
 * bring-up on failure.
 */
esp_err_t mind_ble_observer_start(QueueHandle_t out_q);

#ifdef __cplusplus
}
#endif
