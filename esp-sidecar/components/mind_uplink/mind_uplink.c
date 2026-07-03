/*
 * mind_uplink — aggregate-at-root transport implementation.
 *
 * Every record (event or status) is a packed struct from uplink_schema.h. The
 * discriminator byte at offset [1] (rec_type) lets the root pick the MQTT topic
 * without re-decoding the body — true binary passthrough.
 *
 *   leaf:  build record -> mind_mesh_send_to_root(bytes)
 *   root:  build record (own) or receive record (descendant) -> MQTT publish
 *
 * The MQTT client exists only while this node is the root; it is started/stopped
 * on role transitions (polled in the status task).
 */

#include <string.h>
#include <inttypes.h>

#include "mind_uplink.h"
#include "mind_ble.h"
#include "mind_mesh.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "mqtt_client.h"

#include "uplink_schema.h"

static const char *TAG = "mind_uplink";

#define TOPIC_EVENT   "mind/ingest/event"
#define TOPIC_STATUS  "mind/ingest/status"
#define STATUS_PERIOD_MS  5000

static QueueHandle_t         s_obs_q;
static esp_mqtt_client_handle_t s_client;
static bool                  s_mqtt_up;      /* client started (we are root)   */
static bool                  s_mqtt_conn;    /* broker connection established   */
static char                  s_lwt_topic[32];

/* ------------------------------------------------------------------ */
/* MQTT publish helpers (root only)                                    */
/* ------------------------------------------------------------------ */
static void publish_record(const uint8_t *buf, size_t len)
{
    if (!s_mqtt_conn || len < 2) {
        return;                          /* not connected yet: drop */
    }
    const char *topic = (buf[1] == MIND_REC_STATUS) ? TOPIC_STATUS : TOPIC_EVENT;
    esp_mqtt_client_publish(s_client, topic, (const char *)buf, len, 0, 0);
}

/* Root-side ingest: a descendant's record arrived over the mesh. */
static void on_root_rx(const uint8_t *data, uint32_t len)
{
    publish_record(data, (size_t)len);
}

/* Forward-or-publish a locally produced record. */
static void emit_local(const uint8_t *buf, size_t len)
{
    if (mind_mesh_is_root()) {
        publish_record(buf, len);
    } else {
        mind_mesh_send_to_root(buf, len);
    }
}

/* ------------------------------------------------------------------ */
/* Record builders                                                     */
/* ------------------------------------------------------------------ */
static void build_event(mind_uplink_event_t *e, const mind_observation_t *o)
{
    int64_t age_ms = (esp_timer_get_time() - o->rx_us) / 1000;
    if (age_ms < 0) age_ms = 0;
    if (age_ms > 0xFFFF) age_ms = 0xFFFF;

    e->uplink_version = MIND_UPLINK_VERSION;
    e->rec_type       = MIND_REC_EVENT;
    e->node_id        = CONFIG_MIND_NODE_ID;
    e->device_id      = o->device_id;
    e->rssi           = o->rssi;
    e->event_type     = o->payload.event_type;
    e->confidence     = o->payload.confidence;
    e->accel_svm      = o->payload.accel_svm;
    e->mic_level      = o->payload.mic_level;
    e->seq            = o->payload.seq;
    e->age_ms         = (uint16_t)age_ms;
}

static void build_status(mind_uplink_status_t *st)
{
    mind_mesh_status_t m;
    mind_mesh_get_status(&m);

    st->uplink_version = MIND_UPLINK_VERSION;
    st->rec_type       = MIND_REC_STATUS;
    st->node_id        = CONFIG_MIND_NODE_ID;
    st->is_root        = m.is_root ? 1 : 0;
    st->mesh_level     = m.level;
    st->parent_rssi    = m.parent_rssi;
    st->uptime_s       = (uint32_t)(esp_timer_get_time() / 1000000);
    st->free_heap      = esp_get_free_heap_size();
    st->child_count    = m.child_count;
}

/* ------------------------------------------------------------------ */
/* MQTT lifecycle (root only)                                          */
/* ------------------------------------------------------------------ */
static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_conn = true;
        /* Retained presence marker: dashboard sees the root as online. */
        esp_mqtt_client_publish(s_client, s_lwt_topic, "online", 0, 1, 1);
        ESP_LOGI(TAG, "MQTT connected to %s", CONFIG_MIND_BROKER_URI);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt_conn = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    default:
        break;
    }
}

static void mqtt_client_create(void)
{
    snprintf(s_lwt_topic, sizeof(s_lwt_topic), "mind/node/%d/lwt", CONFIG_MIND_NODE_ID);

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_MIND_BROKER_URI,
        .session.last_will = {
            .topic  = s_lwt_topic,
            .msg    = "offline",
            .msg_len = 0,
            .qos    = 1,
            .retain = 1,
        },
    };
    s_client = esp_mqtt_client_init(&cfg);
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
}

/* Start/stop the client as this node gains/loses the root role. */
static void mqtt_track_role(void)
{
    bool root = mind_mesh_is_root();
    if (root && !s_mqtt_up) {
        esp_mqtt_client_start(s_client);
        s_mqtt_up = true;
        ESP_LOGI(TAG, "became root: MQTT client started");
    } else if (!root && s_mqtt_up) {
        esp_mqtt_client_stop(s_client);
        s_mqtt_up = false;
        s_mqtt_conn = false;
        ESP_LOGI(TAG, "no longer root: MQTT client stopped");
    }
}

/* ------------------------------------------------------------------ */
/* Tasks                                                               */
/* ------------------------------------------------------------------ */
static void forwarder_task(void *arg)
{
    mind_observation_t o;
    mind_uplink_event_t e;
    for (;;) {
        if (xQueueReceive(s_obs_q, &o, portMAX_DELAY) == pdTRUE) {
            build_event(&e, &o);
            emit_local((const uint8_t *)&e, sizeof(e));
        }
    }
}

static void status_task(void *arg)
{
    mind_uplink_status_t st;
    for (;;) {
        mqtt_track_role();
        build_status(&st);
        emit_local((const uint8_t *)&st, sizeof(st));
        vTaskDelay(pdMS_TO_TICKS(STATUS_PERIOD_MS));
    }
}

esp_err_t mind_uplink_start(QueueHandle_t obs_q)
{
    s_obs_q = obs_q;

    mqtt_client_create();

    /* Start the mesh; on_root_rx receives descendant records when we are root. */
    esp_err_t err = mind_mesh_start(on_root_rx);
    if (err != ESP_OK) {
        return err;
    }

    xTaskCreate(forwarder_task, "mind_fwd", 3072, NULL, 5, NULL);
    xTaskCreate(status_task, "mind_status", 3072, NULL, 4, NULL);

    ESP_LOGI(TAG, "uplink started (node_id=%d)", CONFIG_MIND_NODE_ID);
    return ESP_OK;
}
