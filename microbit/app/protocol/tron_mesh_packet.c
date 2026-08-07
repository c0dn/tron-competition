#include "tron_mesh_packet.h"

#include <stdbool.h>
#include <string.h>

static void put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static uint16_t get_u16_le(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t get_u24_le(const uint8_t *src)
{
    return (uint32_t)src[0] |
           ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16);
}

size_t tron_mesh_packet_encoded_len(uint8_t payload_len)
{
    if (payload_len > TRON_MESH_PAYLOAD_MAX) {
        return 0u;
    }

    return (size_t)TRON_MESH_ADV_BASE_LEN + (size_t)payload_len;
}

tron_mesh_packet_result_t tron_mesh_packet_encode(
    const tron_mesh_packet_t *packet,
    uint8_t *adv,
    size_t adv_cap,
    size_t *adv_len)
{
    size_t needed;
    size_t i;
    size_t pos = 0u;
    uint32_t seq24;

    if (packet == NULL || adv == NULL || adv_len == NULL) {
        return TRON_MESH_PACKET_ERR_NULL;
    }

    *adv_len = 0u;

    needed = tron_mesh_packet_encoded_len(packet->payload_len);
    if (needed == 0u) {
        return TRON_MESH_PACKET_ERR_PAYLOAD_LEN;
    }

    if (packet->ttl > TRON_MESH_TTL_MAX) {
        return TRON_MESH_PACKET_ERR_TTL;
    }

    if (needed > TRON_MESH_ADV_MAX_LEN || adv_cap < needed) {
        *adv_len = needed;
        return TRON_MESH_PACKET_ERR_LENGTH;
    }

    adv[pos++] = TRON_MESH_FLAGS_AD_LEN;
    adv[pos++] = TRON_MESH_FLAGS_AD_TYPE;
    adv[pos++] = TRON_MESH_FLAGS_VALUE;

    adv[pos++] = (uint8_t)(TRON_MESH_MANUF_BASE_LEN + packet->payload_len);
    adv[pos++] = TRON_MESH_MANUF_AD_TYPE;
    put_u16_le(&adv[pos], TRON_MESH_COMPANY_ID);
    pos += 2u;

    adv[pos++] = (uint8_t)TRON_MESH_MAGIC0;
    adv[pos++] = (uint8_t)TRON_MESH_MAGIC1;
    adv[pos++] = TRON_MESH_VERSION;
    adv[pos++] = packet->msg_type;
    adv[pos++] = packet->net_id;
    adv[pos++] = packet->ttl;
    put_u16_le(&adv[pos], packet->src);
    pos += 2u;

    seq24 = packet->seq24 & TRON_MESH_SEQ24_MAX;
    adv[pos++] = (uint8_t)(seq24 & 0xFFu);
    adv[pos++] = (uint8_t)((seq24 >> 8) & 0xFFu);
    adv[pos++] = (uint8_t)((seq24 >> 16) & 0xFFu);

    adv[pos++] = packet->payload_len;
    for (i = 0u; i < packet->payload_len; i++) {
        adv[pos++] = packet->payload[i];
    }

    *adv_len = pos;
    return TRON_MESH_PACKET_OK;
}

static tron_mesh_packet_result_t decode_manufacturer_ad(
    const uint8_t *ad,
    uint8_t ad_len,
    tron_mesh_packet_t *packet)
{
    uint8_t payload_len;

    if (ad_len < 3u) {
        return TRON_MESH_PACKET_ERR_LENGTH;
    }

    if (get_u16_le(&ad[1]) != TRON_MESH_COMPANY_ID) {
        return TRON_MESH_PACKET_ERR_COMPANY;
    }

    if (ad_len < TRON_MESH_MANUF_BASE_LEN) {
        return TRON_MESH_PACKET_ERR_LENGTH;
    }

    if (ad[3] != (uint8_t)TRON_MESH_MAGIC0 ||
        ad[4] != (uint8_t)TRON_MESH_MAGIC1) {
        return TRON_MESH_PACKET_ERR_MAGIC;
    }

    if (ad[5] != TRON_MESH_VERSION) {
        return TRON_MESH_PACKET_ERR_VERSION;
    }

    payload_len = ad[14];
    if (payload_len > TRON_MESH_PAYLOAD_MAX ||
        ad_len != (uint8_t)(TRON_MESH_MANUF_BASE_LEN + payload_len)) {
        return TRON_MESH_PACKET_ERR_PAYLOAD_LEN;
    }

    if (ad[8] > TRON_MESH_TTL_MAX) {
        return TRON_MESH_PACKET_ERR_TTL;
    }

    packet->msg_type = ad[6];
    packet->net_id = ad[7];
    packet->ttl = ad[8];
    packet->src = get_u16_le(&ad[9]);
    packet->seq24 = get_u24_le(&ad[11]);
    packet->payload_len = payload_len;

    if (payload_len > 0u) {
        memcpy(packet->payload, &ad[15], payload_len);
    }

    return TRON_MESH_PACKET_OK;
}

tron_mesh_packet_result_t tron_mesh_packet_decode(
    const uint8_t *adv,
    size_t adv_len,
    tron_mesh_packet_t *packet)
{
    size_t pos = 0u;
    bool found_flags = false;
    bool saw_manufacturer = false;
    bool decoded_manufacturer = false;
    tron_mesh_packet_result_t first_manufacturer_error = TRON_MESH_PACKET_OK;
    tron_mesh_packet_t decoded_packet;

    if (adv == NULL || packet == NULL) {
        return TRON_MESH_PACKET_ERR_NULL;
    }

    if (adv_len > TRON_MESH_ADV_MAX_LEN) {
        return TRON_MESH_PACKET_ERR_LENGTH;
    }

    while (pos < adv_len) {
        uint8_t ad_len = adv[pos];
        const uint8_t *ad;

        if (ad_len == 0u || (size_t)ad_len > (adv_len - pos - 1u)) {
            return TRON_MESH_PACKET_ERR_LENGTH;
        }

        ad = &adv[pos + 1u];

        if (ad_len == TRON_MESH_FLAGS_AD_LEN &&
            ad[0] == TRON_MESH_FLAGS_AD_TYPE &&
            ad[1] == TRON_MESH_FLAGS_VALUE) {
            found_flags = true;
        }

        if (ad[0] == TRON_MESH_MANUF_AD_TYPE) {
            tron_mesh_packet_t candidate;
            tron_mesh_packet_result_t result;

            saw_manufacturer = true;
            result = decode_manufacturer_ad(ad, ad_len, &candidate);
            if (result == TRON_MESH_PACKET_OK) {
                decoded_packet = candidate;
                decoded_manufacturer = true;
            } else if (first_manufacturer_error == TRON_MESH_PACKET_OK) {
                first_manufacturer_error = result;
            }
        }

        pos += (size_t)ad_len + 1u;
    }

    if (!found_flags) {
        return TRON_MESH_PACKET_ERR_MISSING_FLAGS;
    }

    if (!saw_manufacturer) {
        return TRON_MESH_PACKET_ERR_MISSING_MANUFACTURER;
    }

    if (!decoded_manufacturer) {
        return first_manufacturer_error;
    }

    *packet = decoded_packet;
    return TRON_MESH_PACKET_OK;
}

int tron_mesh_packet_is_for_network(const tron_mesh_packet_t *packet,
                                    uint8_t expected_net_id)
{
    return packet != NULL && packet->net_id == expected_net_id;
}

const char *tron_mesh_packet_result_name(tron_mesh_packet_result_t result)
{
    switch (result) {
    case TRON_MESH_PACKET_OK:
        return "OK";
    case TRON_MESH_PACKET_ERR_NULL:
        return "ERR_NULL";
    case TRON_MESH_PACKET_ERR_LENGTH:
        return "ERR_LENGTH";
    case TRON_MESH_PACKET_ERR_MISSING_FLAGS:
        return "ERR_MISSING_FLAGS";
    case TRON_MESH_PACKET_ERR_MISSING_MANUFACTURER:
        return "ERR_MISSING_MANUFACTURER";
    case TRON_MESH_PACKET_ERR_COMPANY:
        return "ERR_COMPANY";
    case TRON_MESH_PACKET_ERR_MAGIC:
        return "ERR_MAGIC";
    case TRON_MESH_PACKET_ERR_VERSION:
        return "ERR_VERSION";
    case TRON_MESH_PACKET_ERR_PAYLOAD_LEN:
        return "ERR_PAYLOAD_LEN";
    case TRON_MESH_PACKET_ERR_TTL:
        return "ERR_TTL";
    default:
        return "ERR_UNKNOWN";
    }
}
