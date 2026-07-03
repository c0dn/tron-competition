/*
 * mind_mesh — ESP-MESH-LITE bring-up implementation.
 *
 * Sequence (per mesh-lite v1.0.x): netif + event loop already up (main) ->
 * esp_bridge_create_all_netif() -> wifi APSTA -> esp_mesh_lite_init() ->
 * register raw handler -> set router config -> connect -> start.
 *
 * Node -> root forwarding uses esp_mesh_lite_send_msg(ESP_MESH_LITE_RAW_MSG)
 * with a MIND-specific msg_id; the root's registered action fires on match.
 */

#include "mind_mesh.h"

#include <string.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include "esp_bridge.h"
#include "esp_mesh_lite.h"

static const char *TAG = "mind_mesh";

/* Custom raw-message id for MIND uplink records ("MIND"). Chosen well away from
 * mesh-lite's internal small-integer MESH_LITE_MSG_ID_* range. */
#define MIND_MSG_ID_UPLINK  0x4D494E44u

static mind_mesh_raw_rx_cb_t s_root_rx;

/* ------------------------------------------------------------------ */
/* Root-side receive: one call per raw record from a descendant.       */
/* ------------------------------------------------------------------ */
static esp_err_t uplink_raw_handler(uint8_t *data, uint32_t len,
                                    uint8_t **out_data, uint32_t *out_len,
                                    uint32_t seq)
{
    (void)seq;
    if (s_root_rx != NULL) {
        s_root_rx(data, len);
    }
    if (out_data != NULL) *out_data = NULL;   /* no response payload */
    if (out_len != NULL)  *out_len = 0;
    return ESP_OK;
}

static const esp_mesh_lite_raw_msg_action_t s_raw_actions[] = {
    { MIND_MSG_ID_UPLINK, 0, uplink_raw_handler },
    { 0, 0, NULL },
};

/* ------------------------------------------------------------------ */
/* WiFi (AP+STA) — mesh-lite operates on top of this.                  */
/* ------------------------------------------------------------------ */
static void wifi_init(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_start());
}

esp_err_t mind_mesh_start(mind_mesh_raw_rx_cb_t on_root_rx)
{
    s_root_rx = on_root_rx;

    /* Bridge netifs (STA + AP + internal) managed by iot_bridge/mesh-lite. */
    esp_bridge_create_all_netif();

    wifi_init();

    esp_mesh_lite_config_t mesh_cfg = ESP_MESH_LITE_DEFAULT_INIT();
    esp_mesh_lite_init(&mesh_cfg);

    /* SoftAP link between nodes. NOTE(verify): confirm this setter's exact
     * name/signature against the pinned mesh_lite headers; some versions take
     * the SoftAP info via the config struct fields instead. */
    esp_mesh_lite_set_softap_info(CONFIG_MIND_MESH_SOFTAP_SSID,
                                  CONFIG_MIND_MESH_SOFTAP_PASSWORD);

    /* Register the root-side raw record handler. */
    esp_mesh_lite_raw_msg_action_list_register(s_raw_actions);

    /* Router the root uplinks through. */
    mesh_lite_sta_config_t router = { 0 };
    strlcpy((char *)router.ssid, CONFIG_MIND_ROUTER_SSID, sizeof(router.ssid));
    strlcpy((char *)router.password, CONFIG_MIND_ROUTER_PASSWORD, sizeof(router.password));
    esp_mesh_lite_set_router_config(&router);

    esp_mesh_lite_connect();
    esp_mesh_lite_start();

    ESP_LOGI(TAG, "mesh started (node_id=%d, router=\"%s\")",
             CONFIG_MIND_NODE_ID, CONFIG_MIND_ROUTER_SSID);
    return ESP_OK;
}

bool mind_mesh_is_root(void)
{
    return esp_mesh_lite_get_level() == 1;   /* ROOT == 1 */
}

void mind_mesh_get_status(mind_mesh_status_t *out)
{
    if (out == NULL) {
        return;
    }
    out->level = (uint8_t)esp_mesh_lite_get_level();
    out->is_root = (out->level == 1);

    /* RSSI to parent/router = this node's STA-side AP info. */
    wifi_ap_record_t ap = { 0 };
    out->parent_rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? (int8_t)ap.rssi : 0;

    /* Direct children = stations connected to our SoftAP. */
    wifi_sta_list_t sta = { 0 };
    out->child_count = (esp_wifi_ap_get_sta_list(&sta) == ESP_OK) ? (uint8_t)sta.num : 0;
}

esp_err_t mind_mesh_send_to_root(const uint8_t *data, size_t len)
{
    esp_mesh_lite_msg_config_t config = {
        .raw_msg = {
            .msg_id = MIND_MSG_ID_UPLINK,
            .expect_resp_msg_id = 0,      /* fire-and-forget, no ack */
            .max_retry = 2,
            .data = data,
            .size = len,
            .raw_resend = esp_mesh_lite_send_raw_msg_to_root,
        },
    };
    return esp_mesh_lite_send_msg(ESP_MESH_LITE_RAW_MSG, &config);
}
