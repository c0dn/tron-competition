/*
 * MIND ESP32-C3 sidecar — application entry point.
 *
 * Pipeline role: passively observe micro:bit wearable BLE beacons (schema v1),
 * de-duplicate them, and forward each observation over an ESP-MESH-LITE network
 * to the root node, which republishes over MQTT to the dashboard.
 *
 *   BLE observe (mind_ble) -> obs queue -> uplink (mind_uplink):
 *       leaf  -> mesh raw msg -> root
 *       root  -> MQTT (mind/ingest/*)
 *
 * Wire contracts: shared/schema.h (BLE, Tier A) and shared/uplink_schema.h
 * (mesh + MQTT, Tier B). Per-unit config in Kconfig (MIND_* menu).
 */

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "mind_ble.h"
#include "mind_uplink.h"

static const char *TAG = "mind";

#define OBS_QUEUE_LEN 16

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    ESP_LOGI(TAG, "MIND sidecar boot (node_id=%d)", CONFIG_MIND_NODE_ID);

    init_nvs();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Observation queue: mind_ble is the producer, mind_uplink the consumer. */
    QueueHandle_t obs_q = xQueueCreate(OBS_QUEUE_LEN, sizeof(mind_observation_t));
    if (obs_q == NULL) {
        ESP_LOGE(TAG, "failed to create observation queue");
        return;
    }

    ESP_ERROR_CHECK(mind_ble_observer_start(obs_q));
    ESP_ERROR_CHECK(mind_uplink_start(obs_q));

    ESP_LOGI(TAG, "MIND sidecar running");
}
