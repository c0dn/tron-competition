/*
 * mind_ble — NimBLE passive observer implementation.
 *
 * Coexistence note: the ESP32-C3 has a single 2.4 GHz radio shared between
 * WiFi (mesh) and BLE. The scan window is kept well below the scan interval so
 * the coexistence arbiter leaves the mesh enough airtime. Duplicate filtering
 * is left OFF at the controller — we want the wearable's repeated bursts for
 * RSSI/liveness and de-duplicate in software by (device_id, event_type, seq).
 */

#include <string.h>

#include "mind_ble.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

static const char *TAG = "mind_ble";

/* Scan cadence in 0.625 ms units. Window (30 ms) < interval (100 ms) => ~30%
 * BLE duty cycle, leaving airtime for the WiFi mesh on the shared radio. */
#define SCAN_ITVL_UNITS   160  /* 100 ms */
#define SCAN_WIN_UNITS    48   /* 30 ms  */

static QueueHandle_t s_obs_q;
static uint8_t       s_own_addr_type;

/* ------------------------------------------------------------------ */
/* Software de-dup: collapse the wearable's ~50x incident burst into a */
/* single uplink, while still forwarding each distinct heartbeat.      */
/* Keyed per device on the last (event_type, seq) we forwarded.        */
/* ------------------------------------------------------------------ */
#define DEDUP_N 16
typedef struct {
    bool    used;
    uint8_t device_id;
    uint8_t event_type;
    uint8_t seq;
} dedup_slot_t;
static dedup_slot_t s_dedup[DEDUP_N];

static bool dedup_is_new(uint8_t dev, uint8_t evt, uint8_t seq)
{
    int free_i = -1;
    for (int i = 0; i < DEDUP_N; i++) {
        if (s_dedup[i].used && s_dedup[i].device_id == dev) {
            if (s_dedup[i].event_type == evt && s_dedup[i].seq == seq) {
                return false;                 /* same event still bursting */
            }
            s_dedup[i].event_type = evt;
            s_dedup[i].seq = seq;
            return true;
        }
        if (!s_dedup[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (free_i < 0) {
        free_i = dev % DEDUP_N;               /* table full: evict by hash */
    }
    s_dedup[free_i].used = true;
    s_dedup[free_i].device_id = dev;
    s_dedup[free_i].event_type = evt;
    s_dedup[free_i].seq = seq;
    return true;
}

/* ------------------------------------------------------------------ */
/* GAP discovery callback — one call per received advert.              */
/* ------------------------------------------------------------------ */
static int gap_event(struct ble_gap_event *event, void *arg)
{
    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }
    struct ble_gap_disc_desc *d = &event->disc;

    /* Identity gate: MIND wearables use a fixed random-static AdvA whose top
     * byte is 0xC0 (see MIND_ADVA in schema.h). Cheap reject before parsing. */
    if (d->addr.type != BLE_ADDR_RANDOM || d->addr.val[5] != 0xC0) {
        return 0;
    }

    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) != 0) {
        return 0;
    }
    /* mfg_data = [company_id LE (2)] [payload ...]; need company + 7B payload. */
    if (fields.mfg_data == NULL ||
        fields.mfg_data_len < 2 + MIND_PAYLOAD_SIZE) {
        return 0;
    }
    uint16_t company = (uint16_t)fields.mfg_data[0] |
                       ((uint16_t)fields.mfg_data[1] << 8);
    if (company != MIND_COMPANY_ID) {
        return 0;
    }

    const mind_adv_payload_t *p =
        (const mind_adv_payload_t *)&fields.mfg_data[2];
    if (p->schema_version != MIND_SCHEMA_VERSION) {
        return 0;
    }

    uint8_t dev = d->addr.val[4];             /* DEVICE_ID lives in AdvA[4] */
    if (!dedup_is_new(dev, p->event_type, p->seq)) {
        return 0;
    }

    mind_observation_t obs = {
        .device_id = dev,
        .rssi = (int8_t)d->rssi,
        .rx_us = esp_timer_get_time(),
    };
    memcpy(&obs.payload, p, sizeof(obs.payload));

    if (s_obs_q != NULL) {
        (void)xQueueSend(s_obs_q, &obs, 0);   /* drop if full: liveness > backlog */
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* NimBLE host lifecycle                                               */
/* ------------------------------------------------------------------ */
static void start_scan(void)
{
    struct ble_gap_disc_params dp = {
        .itvl = SCAN_ITVL_UNITS,
        .window = SCAN_WIN_UNITS,
        .passive = 1,             /* observer: no scan requests */
        .filter_duplicates = 0,   /* we de-dup in software */
        .limited = 0,
        .filter_policy = 0,
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &dp, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "observer scanning (win=%d/itvl=%d units, passive)",
                 SCAN_WIN_UNITS, SCAN_ITVL_UNITS);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type rc=%d", rc);
        return;
    }
    start_scan();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();               /* blocks until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

esp_err_t mind_ble_observer_start(QueueHandle_t out_q)
{
    s_obs_q = out_q;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}
