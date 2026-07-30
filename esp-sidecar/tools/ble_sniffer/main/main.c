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
 * A node running the flood mesh appends a 5-byte transport header inside the
 * same manufacturer data (19 bytes total, see shared/mesh_wire.h). Both forms
 * are decoded here; the header is reported when present so a relayed copy can
 * be told from one heard directly. That distinction matters beyond curiosity -
 * a relayed advert carries the ORIGINATOR's address but the LAST HOP's signal
 * strength, so RSSI must not be read as distance to the originator unless the
 * line says "direct".
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
#include "mesh_wire.h"

static const char *TAG = "sniffer";

/* Scan cadence in 0.625 ms units. Nothing shares the radio here - no WiFi, no
 * mesh - so the window runs close to the interval for a ~90% duty cycle. The
 * sidecar deliberately scans far less to leave the mesh airtime. */
#define SCAN_ITVL_UNITS   160   /* 100 ms */
#define SCAN_WIN_UNITS    144   /*  90 ms */

static uint8_t s_own_addr_type;

/* Collapse the wearable's ~50x incident burst to one line per distinct record,
 * keyed on (device_id, event_type, seq), and report how many copies were heard.
 * Repeat counts are useful here: they show burst delivery and link quality.
 *
 * WHY EACH DEVICE NEEDS A RING, NOT ONE SLOT
 * ------------------------------------------
 * This used to hold a single most-recent record per device, which was adequate
 * while every copy arrived in order straight from the wearable. The mesh breaks
 * that assumption: a relayed copy is delayed by a random backoff of up to 50 ms
 * per hop, so a direct copy of record N+1 routinely arrives BEFORE the relayed
 * copy of record N. With one slot, the late arrival of N looks like a brand new
 * record, and the sniffer reports duplicates as fresh events - it would show a
 * fall being detected several times over.
 *
 * A short ring of recent keys per device fixes it: a key is a repeat if it
 * appears anywhere in the device's recent history, not merely if it matches the
 * last one. Depth 8 covers the worst reordering the transport can produce
 * (TTL 6 hops x ~50 ms against the 120 ms burst interval). */
#define DEDUP_DEVICES   8
#define DEDUP_RING      8

typedef struct {
    uint8_t  event_type;
    uint8_t  seq;
    bool     valid;
    uint32_t repeats;
} dedup_rec_t;

typedef struct {
    bool        used;
    uint8_t     device_id;
    uint8_t     next;                   /* write cursor into ring */
    dedup_rec_t ring[DEDUP_RING];
} dedup_dev_t;

static dedup_dev_t s_dedup[DEDUP_DEVICES];

static dedup_dev_t *dev_slot(uint8_t dev)
{
    int free_i = -1;

    for (int i = 0; i < DEDUP_DEVICES; i++) {
        if (s_dedup[i].used && s_dedup[i].device_id == dev) {
            return &s_dedup[i];
        }
        if (!s_dedup[i].used && free_i < 0) {
            free_i = i;
        }
    }
    if (free_i < 0) {
        free_i = dev % DEDUP_DEVICES;   /* more devices than slots: reuse */
        memset(&s_dedup[free_i], 0, sizeof(s_dedup[free_i]));
    }
    s_dedup[free_i].used = true;
    s_dedup[free_i].device_id = dev;
    return &s_dedup[free_i];
}

/* Returns true the first time a (dev, evt, seq) record is seen. On a repeat it
 * bumps that record's counter and returns false. 'prev_repeats' reports how
 * many copies of the record being EVICTED were heard, which is the delivery
 * figure worth reading: 1 means a record arrived exactly once with no
 * redundancy, and on a meshed network that is a link worth looking at. */
static bool is_new_record(uint8_t dev, uint8_t evt, uint8_t seq, uint32_t *prev_repeats)
{
    dedup_dev_t *d = dev_slot(dev);
    dedup_rec_t *slot;

    for (int i = 0; i < DEDUP_RING; i++) {
        if (d->ring[i].valid && d->ring[i].event_type == evt &&
            d->ring[i].seq == seq) {
            d->ring[i].repeats++;
            return false;
        }
    }

    slot = &d->ring[d->next];
    *prev_repeats = slot->valid ? slot->repeats : 0;

    slot->valid = true;
    slot->event_type = evt;
    slot->seq = seq;
    slot->repeats = 1;
    d->next = (uint8_t)((d->next + 1) % DEDUP_RING);
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
    const mesh_wire_hdr_t *mesh = NULL;
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

    /* Transport header, if this copy came through the mesh. Detected by an
     * EXACT manufacturer-data length - see mesh_wire.h for why a >= test would
     * misread a plain advert as a meshed one. Its absence is not an error: a
     * wearable built with the mesh disabled emits the shorter form, and that
     * is precisely the compatibility this layout was designed to keep. */
    if (fields.mfg_data_len == MESH_WIRE_MSD_LEN) {
        mesh = (const mesh_wire_hdr_t *)&fields.mfg_data[MESH_WIRE_HDR_OFFSET];

        if (mesh->version != MESH_WIRE_VERSION) {
            ESP_LOGW(TAG, "mesh transport v%u, expected v%d - ignoring",
                     mesh->version, MESH_WIRE_VERSION);
            return 0;
        }
        /* orig_id restates AdvA[4]; disagreement means the header or the
           address was corrupted in flight, so neither can be trusted. */
        if (mesh->orig_id != d->addr.val[4]) {
            ESP_LOGW(TAG, "mesh id mismatch: hdr=0x%02x adva=0x%02x - dropping",
                     mesh->orig_id, d->addr.val[4]);
            return 0;
        }
    }

    dev = d->addr.val[4];               /* DEVICE_ID lives in AdvA[4] */
    if (!is_new_record(dev, p->event_type, p->seq, &prev_repeats)) {
        return 0;
    }

    if (mesh != NULL && mesh->hops > 0) {
        /* Relayed. The RSSI below belongs to the last hop, NOT to the
           originating device - see mesh_wire.h. */
        ESP_LOGI(TAG,
                 "RELAYED origin=%u via=0x%02x hops=%u ttl=%u seq=%u "
                 "event=%s relay_rssi=%d dBm",
                 dev, mesh->relay_id, mesh->hops, mesh->ttl, p->seq,
                 event_name(p->event_type), d->rssi);
    } else if (mesh != NULL) {
        ESP_LOGI(TAG,
                 "DIRECT  origin=%u seq=%u event=%s ttl=%u rssi=%d dBm",
                 dev, p->seq, event_name(p->event_type), mesh->ttl, d->rssi);
    } else {
        ESP_LOGI(TAG,
                 "LEGACY  origin=%u seq=%u event=%s rssi=%d dBm (no mesh header)",
                 dev, p->seq, event_name(p->event_type), d->rssi);
    }

    /* Keep sensor details visible for real incidents without turning every
       heartbeat in the demo into a wrapped multi-line record. */
    if (p->event_type != MIND_EVT_HEARTBEAT) {
        ESP_LOGI(TAG, "        payload confidence=%u svm=%u mg mic=%u",
                 p->confidence, p->accel_svm, p->mic_level);
    }

    hex_str(d->data, d->length_data, raw, sizeof(raw));
    ESP_LOGD(TAG,
             "        schema=%u event_id=%u conf=%u svm=%u mic=%u previous_record_copies=%" PRIu32,
             p->schema_version, p->event_type, p->confidence, p->accel_svm,
             p->mic_level, prev_repeats);
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
        ESP_LOGI(TAG, "VERIFY: RELAYED origin=<source> via=<relay> hops=1 ttl=5 proves one-hop forwarding");
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
