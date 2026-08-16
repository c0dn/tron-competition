#include "mind_application_ingress.h"

#include <limits.h>
#include <string.h>

#include "tron_mesh_packet.h"

#define MIND_INGRESS_FLAGS_LENGTH_OFFSET 0u
#define MIND_INGRESS_FLAGS_TYPE_OFFSET 1u
#define MIND_INGRESS_FLAGS_VALUE_OFFSET 2u
#define MIND_INGRESS_MANUFACTURER_LENGTH_OFFSET 3u
#define MIND_INGRESS_MANUFACTURER_TYPE_OFFSET 4u
#define MIND_INGRESS_COMPANY_LOW_OFFSET 5u
#define MIND_INGRESS_COMPANY_HIGH_OFFSET 6u
#define MIND_INGRESS_MAGIC0_OFFSET 7u
#define MIND_INGRESS_MAGIC1_OFFSET 8u
#define MIND_INGRESS_VERSION_OFFSET 9u
#define MIND_INGRESS_TYPE_OFFSET 10u
#define MIND_INGRESS_NETWORK_OFFSET 11u
#define MIND_INGRESS_TTL_OFFSET 12u
#define MIND_INGRESS_SOURCE_LOW_OFFSET 13u
#define MIND_INGRESS_SOURCE_HIGH_OFFSET 14u
#define MIND_INGRESS_PACKET_ID0_OFFSET 15u
#define MIND_INGRESS_PACKET_ID1_OFFSET 16u
#define MIND_INGRESS_PACKET_ID2_OFFSET 17u
#define MIND_INGRESS_PAYLOAD_LENGTH_OFFSET 18u
#define MIND_INGRESS_PAYLOAD_OFFSET 19u

static void increment_saturating(uint32_t *value)
{
    if (value != NULL && *value != UINT32_MAX) {
        (*value)++;
    }
}

static uint16_t get_u16_le(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t get_u24_le(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16);
}

static int is_expired(uint32_t committed_at_ms, uint32_t now_ms)
{
    return (uint32_t)(now_ms - committed_at_ms) >=
        MIND_APPLICATION_INGRESS_RETENTION_MS;
}

static int queue_is_valid(const mind_application_ingress_t *ingress)
{
    uint8_t expected_tail;

    if (ingress == NULL ||
        ingress->queue_head >= MIND_APPLICATION_INGRESS_QUEUE_CAPACITY ||
        ingress->queue_tail >= MIND_APPLICATION_INGRESS_QUEUE_CAPACITY ||
        ingress->queue_count > MIND_APPLICATION_INGRESS_QUEUE_CAPACITY) {
        return 0;
    }
    expected_tail = (uint8_t)((ingress->queue_head + ingress->queue_count) %
                              MIND_APPLICATION_INGRESS_QUEUE_CAPACITY);
    return ingress->queue_tail == expected_tail;
}

static void purge_expired(mind_application_ingress_t *ingress, uint32_t now_ms)
{
    uint8_t index;

    for (index = 0u; index < MIND_APPLICATION_INGRESS_SEEN_CAPACITY; index++) {
        mind_application_ingress_seen_t *entry = &ingress->seen[index];

        if (entry->occupied != 0u && is_expired(entry->committed_at_ms, now_ms)) {
            memset(entry, 0, sizeof(*entry));
            increment_saturating(&ingress->counters.expired_purged);
        }
    }
}

static int seen_matches(const mind_application_ingress_seen_t *entry,
                        uint8_t network_id, uint16_t wearable_src16,
                        uint32_t packet_id24)
{
    return entry->occupied != 0u && entry->network_id == network_id &&
        entry->wearable_src16 == wearable_src16 &&
        entry->packet_id24 == packet_id24;
}

static int find_seen_slot(const mind_application_ingress_t *ingress,
                          uint8_t network_id, uint16_t wearable_src16,
                          uint32_t packet_id24, uint8_t *slot_out)
{
    uint8_t index;

    for (index = 0u; index < MIND_APPLICATION_INGRESS_SEEN_CAPACITY; index++) {
        if (seen_matches(&ingress->seen[index], network_id, wearable_src16,
                         packet_id24)) {
            return 1;
        }
        if (ingress->seen[index].occupied == 0u && slot_out != NULL &&
            *slot_out == MIND_APPLICATION_INGRESS_SEEN_CAPACITY) {
            *slot_out = index;
        }
    }
    return 0;
}

static int adva_matches_wearable(const uint8_t adv_addr[6], uint8_t wearable_id)
{
    return adv_addr[0] == 0u && adv_addr[1] == 0u && adv_addr[2] == 0u &&
        adv_addr[3] == 0u && adv_addr[4] == wearable_id &&
        adv_addr[5] == 0xc0u;
}

static mind_application_ingress_validation_t validate_direct_frame(
    const mind_application_ingress_t *ingress,
    const ble_mesh_sched_event_t *scheduler_event, uint8_t *network_id_out,
    uint16_t *wearable_src16_out, uint32_t *packet_id24_out,
    mind_application_wire_record_t *report_out)
{
    const uint8_t *adv;
    uint16_t source;
    uint32_t packet_id24;
    uint8_t wearable_id;

    if (ingress == NULL || scheduler_event == NULL || network_id_out == NULL ||
        wearable_src16_out == NULL || packet_id24_out == NULL || report_out == NULL) {
        return MIND_APPLICATION_INGRESS_INVALID_ARGUMENT;
    }
    if (scheduler_event->adv_len != MIND_APPLICATION_INGRESS_FRAME_BYTES) {
        return MIND_APPLICATION_INGRESS_INVALID_FRAME_LENGTH;
    }
    adv = scheduler_event->adv_data;
    if (adv[MIND_INGRESS_FLAGS_LENGTH_OFFSET] != TRON_MESH_FLAGS_AD_LEN ||
        adv[MIND_INGRESS_FLAGS_TYPE_OFFSET] != TRON_MESH_FLAGS_AD_TYPE ||
        adv[MIND_INGRESS_FLAGS_VALUE_OFFSET] != TRON_MESH_FLAGS_VALUE) {
        return MIND_APPLICATION_INGRESS_INVALID_FLAGS;
    }
    if (adv[MIND_INGRESS_MANUFACTURER_LENGTH_OFFSET] !=
            (TRON_MESH_MANUF_BASE_LEN + MIND_PAYLOAD_SIZE) ||
        adv[MIND_INGRESS_MANUFACTURER_TYPE_OFFSET] !=
            TRON_MESH_MANUF_AD_TYPE) {
        return MIND_APPLICATION_INGRESS_INVALID_MANUFACTURER;
    }
    if (adv[MIND_INGRESS_COMPANY_LOW_OFFSET] != 0xffu ||
        adv[MIND_INGRESS_COMPANY_HIGH_OFFSET] != 0xffu) {
        return MIND_APPLICATION_INGRESS_INVALID_COMPANY;
    }
    if (adv[MIND_INGRESS_MAGIC0_OFFSET] != (uint8_t)TRON_MESH_MAGIC0 ||
        adv[MIND_INGRESS_MAGIC1_OFFSET] != (uint8_t)TRON_MESH_MAGIC1) {
        return MIND_APPLICATION_INGRESS_INVALID_MAGIC;
    }
    if (adv[MIND_INGRESS_VERSION_OFFSET] != TRON_MESH_VERSION) {
        return MIND_APPLICATION_INGRESS_INVALID_VERSION;
    }
    if (adv[MIND_INGRESS_TYPE_OFFSET] != TRON_MESH_MSG_TYPE_MIND_EVENT) {
        return MIND_APPLICATION_INGRESS_INVALID_TYPE;
    }
    if (adv[MIND_INGRESS_NETWORK_OFFSET] != ingress->expected_network_id) {
        return MIND_APPLICATION_INGRESS_INVALID_NETWORK;
    }
    if (adv[MIND_INGRESS_TTL_OFFSET] != 0u) {
        return MIND_APPLICATION_INGRESS_INVALID_TTL;
    }
    source = get_u16_le(&adv[MIND_INGRESS_SOURCE_LOW_OFFSET]);
    wearable_id = (uint8_t)(source & 0xffu);
    if ((source & 0xff00u) != 0x0100u || wearable_id == 0u ||
        wearable_id == 0xffu) {
        return MIND_APPLICATION_INGRESS_INVALID_SOURCE;
    }
    if (!adva_matches_wearable(scheduler_event->adv_addr, wearable_id)) {
        return MIND_APPLICATION_INGRESS_INVALID_ADVA;
    }
    if (adv[MIND_INGRESS_PAYLOAD_LENGTH_OFFSET] != MIND_PAYLOAD_SIZE) {
        return MIND_APPLICATION_INGRESS_INVALID_PAYLOAD_LENGTH;
    }
    packet_id24 = get_u24_le(&adv[MIND_INGRESS_PACKET_ID0_OFFSET]);
    if (mind_application_wire_pack_report(
            report_out, wearable_id, packet_id24,
            &adv[MIND_INGRESS_PAYLOAD_OFFSET]) != MIND_APPLICATION_WIRE_OK) {
        return MIND_APPLICATION_INGRESS_INVALID_SCHEMA;
    }
    *network_id_out = adv[MIND_INGRESS_NETWORK_OFFSET];
    *wearable_src16_out = source;
    *packet_id24_out = packet_id24;
    return MIND_APPLICATION_INGRESS_VALID;
}

void mind_application_ingress_init(mind_application_ingress_t *ingress,
                                   uint8_t expected_network_id)
{
    if (ingress != NULL) {
        memset(ingress, 0, sizeof(*ingress));
        ingress->expected_network_id = expected_network_id;
    }
}

int mind_application_ingress_is_mind_family(
    const ble_mesh_sched_event_t *scheduler_event)
{
    uint8_t position = 0u;

    if (scheduler_event == NULL ||
        scheduler_event->type != BLE_MESH_SCHED_EVENT_RX_ADV) {
        return 0;
    }
    /* The fixed direct frame offsets let a damaged AD wrapper still identify
     * itself as MIND_EVENT.  It must be consumed and rejected locally rather
     * than handed to mentorship merely because its length or flags are bad. */
    if (scheduler_event->adv_len > MIND_INGRESS_TYPE_OFFSET &&
        scheduler_event->adv_data[MIND_INGRESS_MAGIC0_OFFSET] ==
            (uint8_t)TRON_MESH_MAGIC0 &&
        scheduler_event->adv_data[MIND_INGRESS_MAGIC1_OFFSET] ==
            (uint8_t)TRON_MESH_MAGIC1 &&
        scheduler_event->adv_data[MIND_INGRESS_TYPE_OFFSET] ==
            TRON_MESH_MSG_TYPE_MIND_EVENT) {
        return 1;
    }
    while (position < scheduler_event->adv_len) {
        uint8_t ad_length = scheduler_event->adv_data[position];
        const uint8_t *ad;

        if (ad_length == 0u ||
            ad_length > (uint8_t)(scheduler_event->adv_len - position - 1u)) {
            return 0;
        }
        ad = &scheduler_event->adv_data[position + 1u];
        if (ad_length >= 7u && ad[0] == TRON_MESH_MANUF_AD_TYPE &&
            ad[3] == (uint8_t)TRON_MESH_MAGIC0 &&
            ad[4] == (uint8_t)TRON_MESH_MAGIC1 &&
            ad[6] == TRON_MESH_MSG_TYPE_MIND_EVENT) {
            return 1;
        }
        position = (uint8_t)(position + ad_length + 1u);
    }
    return 0;
}

mind_application_ingress_result_t mind_application_ingress_receive(
    mind_application_ingress_t *ingress,
    const ble_mesh_sched_event_t *scheduler_event, uint32_t observed_at_ms)
{
    mind_application_ingress_result_t result;
    mind_application_wire_record_t report;
    mind_application_ingress_event_t event;
    mind_application_ingress_seen_t *seen;
    uint8_t network_id;
    uint16_t wearable_src16;
    uint32_t packet_id24;
    uint8_t reserved_seen = MIND_APPLICATION_INGRESS_SEEN_CAPACITY;
    uint8_t reserved_queue;

    result.outcome = MIND_APPLICATION_INGRESS_INVALID_STATE;
    result.validation = MIND_APPLICATION_INGRESS_INVALID_ARGUMENT;
    if (scheduler_event == NULL) {
        return result;
    }
    if (!mind_application_ingress_is_mind_family(scheduler_event)) {
        if (ingress != NULL) {
            increment_saturating(&ingress->counters.passed_non_mind);
        }
        result.outcome = MIND_APPLICATION_INGRESS_PASSTHROUGH;
        result.validation = MIND_APPLICATION_INGRESS_VALID;
        return result;
    }
    if (!queue_is_valid(ingress)) {
        return result;
    }

    memset(&report, 0, sizeof(report));
    result.validation = validate_direct_frame(ingress, scheduler_event, &network_id,
                                              &wearable_src16, &packet_id24, &report);
    if (result.validation != MIND_APPLICATION_INGRESS_VALID) {
        increment_saturating(&ingress->counters.invalid);
        result.outcome = MIND_APPLICATION_INGRESS_CONSUMED_INVALID;
        return result;
    }

    /* Mesh-owner transaction: validation is complete before any cache change;
     * expired entries are purged before duplicate and reservation checks. */
    purge_expired(ingress, observed_at_ms);
    if (find_seen_slot(ingress, network_id, wearable_src16, packet_id24,
                       &reserved_seen)) {
        increment_saturating(&ingress->counters.duplicate);
        result.outcome = MIND_APPLICATION_INGRESS_CONSUMED_DUPLICATE;
        return result;
    }
    if (reserved_seen == MIND_APPLICATION_INGRESS_SEEN_CAPACITY) {
        increment_saturating(&ingress->counters.cache_full);
        result.outcome = MIND_APPLICATION_INGRESS_CONSUMED_CACHE_FULL;
        return result;
    }
    if (ingress->queue_count == MIND_APPLICATION_INGRESS_QUEUE_CAPACITY) {
        increment_saturating(&ingress->counters.queue_full);
        result.outcome = MIND_APPLICATION_INGRESS_CONSUMED_QUEUE_FULL;
        return result;
    }

    /* Neither reservation mutates shared state.  Copy/publish the complete
     * record first, then commit its seen key at the same observation time. */
    reserved_queue = ingress->queue_tail;
    memset(&event, 0, sizeof(event));
    event.report = report;
    event.observed_at_ms = observed_at_ms;
    event.packet_id24 = packet_id24;
    event.wearable_src16 = wearable_src16;
    event.network_id = network_id;
    memcpy(event.adv_addr, scheduler_event->adv_addr, sizeof(event.adv_addr));
    event.channel = scheduler_event->channel;
    event.rssi_magnitude_db = scheduler_event->rssi_magnitude_db;
    ingress->queue[reserved_queue] = event;
    ingress->queue_tail = (uint8_t)((ingress->queue_tail + 1u) %
                                    MIND_APPLICATION_INGRESS_QUEUE_CAPACITY);
    ingress->queue_count++;

    seen = &ingress->seen[reserved_seen];
    seen->packet_id24 = packet_id24;
    seen->committed_at_ms = observed_at_ms;
    seen->wearable_src16 = wearable_src16;
    seen->network_id = network_id;
    seen->occupied = 1u;
    increment_saturating(&ingress->counters.accepted);
    result.outcome = MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED;
    return result;
}

mind_application_ingress_take_status_t mind_application_ingress_take(
    mind_application_ingress_t *ingress,
    mind_application_ingress_event_t *event_out)
{
    if (!queue_is_valid(ingress) || event_out == NULL) {
        return MIND_APPLICATION_INGRESS_TAKE_INVALID;
    }
    if (ingress->queue_count == 0u) {
        return MIND_APPLICATION_INGRESS_TAKE_EMPTY;
    }
    *event_out = ingress->queue[ingress->queue_head];
    ingress->queue_head = (uint8_t)((ingress->queue_head + 1u) %
                                    MIND_APPLICATION_INGRESS_QUEUE_CAPACITY);
    ingress->queue_count--;
    return MIND_APPLICATION_INGRESS_TAKE_OK;
}
