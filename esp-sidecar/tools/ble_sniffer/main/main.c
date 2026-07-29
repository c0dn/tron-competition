/*
 * ble_sniffer - standalone receiver for MIND wearable beacons.
 *
 * A NimBLE passive observer that decodes schema-v1 adverts straight from the
 * shared wire contract (shared/schema.h) and prints them. No WiFi, no mesh, no
 * MQTT: one board, one micro:bit, no router or broker required.
 *
 * Deliberately self-contained. This is the instrument used to check that a
 * wearable is on the air and that its payload matches the contract, so it does
 * not link against the sidecar's own observer - a test tool that shares code
 * with the thing it tests cannot detect a bug in the shared code. Both sides
 * are held against schema.h independently, and the raw advert bytes are logged
 * so the framing can be checked by eye.
 *
 * Expected on-air layout (14 bytes, see schema.h):
 *   02 01 06                Flags AD
 *   0a ff <company LE>      Manufacturer Specific Data, company 0xFFFF
 *   <7-byte mind_adv_payload_t>
 * Identity rides the advertising address: AdvA[4] is DEVICE_ID and AdvA[5] is
 * 0xC0 (random-static).
 *
 *   idf.py -p <PORT> flash monitor | ./watch-observer.sh
 */

#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

#include "schema.h"

static const char *TAG = "sniffer";

/* Scan cadence in 0.625 ms units. Nothing shares the radio here - no WiFi, no
 * mesh - so the window runs close to the interval for a ~90% duty cycle. The
 * sidecar deliberately scans far less to leave the mesh airtime. */
#define SCAN_ITVL_UNITS   160   /* 100 ms */
#define SCAN_WIN_UNITS    144   /*  90 ms */

static uint8_t s_own_addr_type;

/* Collapse the wearable's ~50x incident burst to one line per distinct record,
 * keyed on (device_id, event_type, seq), and report how many copies were heard.
 * Repeat counts are useful here: they show burst delivery and link quality. */
#define DEDUP_N 8
typedef struct {
    bool     used;
    uint8_t  device_id;
    uint8_t  event_type;
    uint8_t  seq;
    uint32_t repeats;
} dedup_slot_t;
static dedup_slot_t s_dedup[DEDUP_N];

static bool is_new_record(uint8_t dev, uint8_t evt, uint8_t seq, uint32_t *prev_repeats)
{
    int free_i = -1;

    for (int i = 0; i < DEDUP_N; i++) {
        if (s_dedup[i].used && s_dedup[i].device_id == dev) {
            if (s_dedup[i].event_type == evt && s_dedup[i].seq == seq) {
                s_dedup[i].repeats++;
                return false;
            }
            *prev_repeats = s_dedup[i].repeats;
            s_dedup[i].event_type = evt;
            s_dedup[i].seq = seq;
            s_dedup[i].repeats = 1;
            return true;
        }
        if (!s_dedup[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (free_i < 0) {
        free_i = dev % DEDUP_N;
    }
    s_dedup[free_i].used = true;
    s_dedup[free_i].device_id = dev;
    s_dedup[free_i].event_type = evt;
    s_dedup[free_i].seq = seq;
    s_dedup[free_i].repeats = 1;
    *prev_repeats = 0;
    return true;
}

static const char *event_name(uint8_t evt)
{
    switch (evt) {
    case MIND_EVT_HEARTBEAT:         return "HEARTBEAT";
    case MIND_EVT_MOTION:            return "MOTION";
    case MIND_EVT_POSSIBLE_FALL:     return "POSSIBLE_FALL";
    case MIND_EVT_CONFIRMED_FALL:    return "CONFIRMED_FALL";
    case MIND_EVT_POSSIBLE_DISTRESS: return "POSSIBLE_DISTRESS";
    case MIND_EVT_FALL_AND_SHOUT:    return "FALL_AND_SHOUT";
    default:                         return "UNKNOWN";
    }
}

static void hex_str(const uint8_t *b, int n, char *out, size_t out_sz)
{
    size_t off = 0;
    for (int i = 0; i < n && off + 3 < out_sz; i++) {
        off += snprintf(out + off, out_sz - off, "%02x ", b[i]);
    }
    out[off ? off - 1 : 0] = '\0';
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_disc_desc *d;
    struct ble_hs_adv_fields fields;
    const mind_adv_payload_t *p;
    uint16_t company;
    uint8_t dev;
    uint32_t prev_repeats = 0;
    char raw[3 * 31 + 1];

    if (event->type != BLE_GAP_EVENT_DISC) {
        return 0;
    }
    d = &event->disc;

    /* Identity gate: MIND wearables use a fixed random-static AdvA whose top
     * byte is 0xC0 (MIND_ADVA). Cheap reject before parsing anything. */
    if (d->addr.type != BLE_ADDR_RANDOM || d->addr.val[5] != 0xC0) {
        return 0;
    }
    if (ble_hs_adv_parse_fields(&fields, d->data, d->length_data) != 0) {
        return 0;
    }
    /* mfg_data = [company id LE (2)][payload]; need company + the 7-byte body. */
    if (fields.mfg_data == NULL || fields.mfg_data_len < 2 + MIND_PAYLOAD_SIZE) {
        return 0;
    }
    company = (uint16_t)fields.mfg_data[0] | ((uint16_t)fields.mfg_data[1] << 8);
    if (company != MIND_COMPANY_ID) {
        return 0;
    }

    p = (const mind_adv_payload_t *)&fields.mfg_data[2];
    if (p->schema_version != MIND_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "schema mismatch: got v%u, expected v%d - ignoring",
                 p->schema_version, MIND_SCHEMA_VERSION);
        return 0;
    }

    dev = d->addr.val[4];               /* DEVICE_ID lives in AdvA[4] */
    if (!is_new_record(dev, p->event_type, p->seq, &prev_repeats)) {
        return 0;
    }

    hex_str(d->data, d->length_data, raw, sizeof(raw));
    ESP_LOGI(TAG,
             "obs dev=%u rssi=%d ver=%u evt=%u(%s) conf=%u svm=%u mic=%u seq=%u prev_rx=%" PRIu32,
             dev, d->rssi, p->schema_version, p->event_type,
             event_name(p->event_type), p->confidence, p->accel_svm,
             p->mic_level, p->seq, prev_repeats);
    ESP_LOGD(TAG, "     raw[%u]: %s", d->length_data, raw);
    return 0;
}

static void start_scan(void)
{
    struct ble_gap_disc_params dp = {
        .itvl = SCAN_ITVL_UNITS,
        .window = SCAN_WIN_UNITS,
        .passive = 1,            /* observer: never send scan requests */
        .filter_duplicates = 0,  /* we de-dup in software and count repeats */
        .limited = 0,
        .filter_policy = 0,
    };
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &dp, gap_event, NULL);

    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: rc=%d", rc);
    } else {
        ESP_LOGI(TAG, "scanning (window=%d/interval=%d units, passive)",
                 SCAN_WIN_UNITS, SCAN_ITVL_UNITS);
        ESP_LOGI(TAG, "waiting for wearable beacons - AdvA xx:..:C0, company 0x%04X",
                 MIND_COMPANY_ID);
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
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    esp_err_t err;

    ESP_LOGI(TAG, "MIND BLE sniffer - schema v%d, company 0x%04X, payload %d B",
             MIND_SCHEMA_VERSION, MIND_COMPANY_ID, MIND_PAYLOAD_SIZE);

    /* NimBLE stores its identity address in NVS. */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    nimble_port_freertos_init(host_task);
}
