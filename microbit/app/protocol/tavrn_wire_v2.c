#include "tavrn_wire_v2.h"

#include <string.h>

#define TAVRN_ADV_MAX_LEN 31u
#define TAVRN_ADV_WRAPPER_LEN 7u
#define TAVRN_PDU_MIN_LEN 5u
#define TAVRN_WIRE_VERSION 0x02u

#define TAVRN_FLAGS_I 0x80u
#define TAVRN_FLAGS_U 0x20u
#define TAVRN_FLAGS_M 0x08u
#define TAVRN_RERR_FLAGS_M 0x10u

#define TAVRN_WIRE_SID8_LEN  1u
#define TAVRN_WIRE_SID16_LEN 2u
#define TAVRN_WIRE_META_HEADER_LEN 1u
#define TAVRN_WIRE_META_ENTRY_LEN  2u

#define TAVRN_WIRE_DATA_BASE_LEN(width_len) (11u + 3u * (width_len))
#define TAVRN_WIRE_DATA16_BASE_LEN \
    TAVRN_WIRE_DATA_BASE_LEN(TAVRN_WIRE_SID16_LEN)
#define TAVRN_WIRE_DATA8_BASE_LEN \
    TAVRN_WIRE_DATA_BASE_LEN(TAVRN_WIRE_SID8_LEN)

#define TAVRN_WIRE_HACK_STATUS_OFFSET(width_len) (10u + 3u * (width_len))
#define TAVRN_WIRE_HACK_LEN(width_len) \
    (TAVRN_WIRE_HACK_STATUS_OFFSET(width_len) + 1u)
#define TAVRN_WIRE_RREP_ACK_LEN(width_len) (10u + 3u * (width_len))
#define TAVRN_WIRE_RREP_ACK16_LEN \
    TAVRN_WIRE_RREP_ACK_LEN(TAVRN_WIRE_SID16_LEN)
#define TAVRN_WIRE_RREP_ACK8_LEN \
    TAVRN_WIRE_RREP_ACK_LEN(TAVRN_WIRE_SID8_LEN)

#define TAVRN_WIRE_FLOOD_BODY_OFFSET(width_len) (10u + (width_len))
#define TAVRN_WIRE_FLOOD_BASE_LEN(width_len) \
    (TAVRN_WIRE_FLOOD_BODY_OFFSET(width_len) + 1u)
#define TAVRN_WIRE_FLOOD8_BASE_LEN \
    TAVRN_WIRE_FLOOD_BASE_LEN(TAVRN_WIRE_SID8_LEN)
#define TAVRN_WIRE_FLOOD16_BASE_LEN \
    TAVRN_WIRE_FLOOD_BASE_LEN(TAVRN_WIRE_SID16_LEN)
#define TAVRN_WIRE_FLOOD8_BODY_MAX 12u
#define TAVRN_WIRE_FLOOD16_BODY_MAX 11u

#define TAVRN_WIRE_RREQ_BASE_LEN(width_len) (13u + 2u * (width_len))
#define TAVRN_WIRE_RREQ8_BASE_LEN \
    TAVRN_WIRE_RREQ_BASE_LEN(TAVRN_WIRE_SID8_LEN)
#define TAVRN_WIRE_RREP_BASE_LEN(width_len) (13u + 3u * (width_len))
#define TAVRN_WIRE_RREP8_BASE_LEN \
    TAVRN_WIRE_RREP_BASE_LEN(TAVRN_WIRE_SID8_LEN)

#define TAVRN_WIRE_RERR_ENTRIES_OFFSET(width_len) (10u + (width_len))
#define TAVRN_WIRE_RERR_ENTRY_LEN(width_len) ((width_len) + 2u)
#define TAVRN_WIRE_RERR_PDU_LEN(width_len, count) \
    (TAVRN_WIRE_RERR_ENTRIES_OFFSET(width_len) + \
     (count) * TAVRN_WIRE_RERR_ENTRY_LEN(width_len))
#define TAVRN_WIRE_RERR16_BASE_LEN \
    TAVRN_WIRE_RERR_ENTRIES_OFFSET(TAVRN_WIRE_SID16_LEN)
#define TAVRN_WIRE_RERR16_ENTRY_LEN \
    TAVRN_WIRE_RERR_ENTRY_LEN(TAVRN_WIRE_SID16_LEN)
#define TAVRN_WIRE_RERR8_D1_META_LEN \
    (TAVRN_WIRE_RERR_PDU_LEN(TAVRN_WIRE_SID8_LEN, 1u) + \
     TAVRN_WIRE_META_HEADER_LEN + 4u * TAVRN_WIRE_META_ENTRY_LEN)
#define TAVRN_WIRE_RERR8_D2_META_LEN \
    (TAVRN_WIRE_RERR_PDU_LEN(TAVRN_WIRE_SID8_LEN, 2u) + \
     TAVRN_WIRE_META_HEADER_LEN + 3u * TAVRN_WIRE_META_ENTRY_LEN)
#define TAVRN_WIRE_RERR8_D3_META_LEN \
    (TAVRN_WIRE_RERR_PDU_LEN(TAVRN_WIRE_SID8_LEN, 3u) + \
     TAVRN_WIRE_META_HEADER_LEN + TAVRN_WIRE_META_ENTRY_LEN)
#define TAVRN_WIRE_RERR8_D4_LEN \
    TAVRN_WIRE_RERR_PDU_LEN(TAVRN_WIRE_SID8_LEN, 4u)

#define TAVRN_WIRE_HELLO_BASE_LEN(width_len) (15u + 2u * (width_len))
#define TAVRN_WIRE_HELLO16_BASE_LEN \
    TAVRN_WIRE_HELLO_BASE_LEN(TAVRN_WIRE_SID16_LEN)
#define TAVRN_WIRE_HELLO8_BASE_LEN \
    TAVRN_WIRE_HELLO_BASE_LEN(TAVRN_WIRE_SID8_LEN)
#define TAVRN_WIRE_HELLO8_VERIFICATION_LEN \
    (TAVRN_WIRE_HELLO8_BASE_LEN + TAVRN_WIRE_META_HEADER_LEN + \
     TAVRN_WIRE_META_ENTRY_LEN)

#define TAVRN_WIRE_SYNC_OFFER_LEN      24u
#define TAVRN_WIRE_SYNC_PULL_LEN       22u
#define TAVRN_WIRE_SYNC_DATA_EMPTY_LEN 15u
#define TAVRN_WIRE_SYNC_DATA_ENTRY_LEN 9u
#define TAVRN_WIRE_SYNC_DATA_PRESENT_LEN \
    (TAVRN_WIRE_SYNC_DATA_EMPTY_LEN + TAVRN_WIRE_SYNC_DATA_ENTRY_LEN)
#define TAVRN_WIRE_TC_UPDATE_LEN 24u

typedef char tavrn_wire_v2_pdu_max_guard[
    (TAVRN_LINK_CONTROL_PDU_MAX == 24u) ? 1 : -1];
typedef char tavrn_wire_v2_adv_max_guard[
    (TAVRN_ADV_WRAPPER_LEN + TAVRN_LINK_CONTROL_PDU_MAX == TAVRN_ADV_MAX_LEN) ?
        1 : -1];
typedef char tavrn_wire_v2_data16_patient_guard[
    (TAVRN_WIRE_DATA16_BASE_LEN + TAVRN_LINK_SID16_APP_BYTES ==
     TAVRN_LINK_CONTROL_PDU_MAX) ? 1 : -1];
typedef char tavrn_wire_v2_data8_capacity_guard[
    (TAVRN_WIRE_DATA8_BASE_LEN + TAVRN_LINK_APP_BYTES ==
     TAVRN_LINK_CONTROL_PDU_MAX) ? 1 : -1];
typedef char tavrn_wire_v2_data8_patient_guard[
    (TAVRN_WIRE_DATA8_BASE_LEN + TAVRN_LINK_SID16_APP_BYTES == 21u) ? 1 : -1];
typedef char tavrn_wire_v2_flood_capacity_guard[
    (TAVRN_WIRE_FLOOD16_BASE_LEN + TAVRN_WIRE_FLOOD16_BODY_MAX ==
     TAVRN_LINK_CONTROL_PDU_MAX &&
     TAVRN_WIRE_FLOOD8_BASE_LEN + TAVRN_WIRE_FLOOD8_BODY_MAX ==
     TAVRN_LINK_CONTROL_PDU_MAX) ? 1 : -1];
typedef char tavrn_wire_v2_rreq8_metadata_guard[
    (TAVRN_WIRE_RREQ8_BASE_LEN + TAVRN_WIRE_META_HEADER_LEN +
     4u * TAVRN_WIRE_META_ENTRY_LEN == TAVRN_LINK_CONTROL_PDU_MAX) ? 1 : -1];
typedef char tavrn_wire_v2_rerr16_capacity_guard[
    (TAVRN_WIRE_RERR16_BASE_LEN + 3u * TAVRN_WIRE_RERR16_ENTRY_LEN ==
     TAVRN_LINK_CONTROL_PDU_MAX) ? 1 : -1];
typedef char tavrn_wire_v2_rerr8_capacity_guard[
    (TAVRN_WIRE_RERR8_D1_META_LEN == TAVRN_LINK_CONTROL_PDU_MAX - 1u &&
     TAVRN_WIRE_RERR8_D2_META_LEN == TAVRN_LINK_CONTROL_PDU_MAX &&
     TAVRN_WIRE_RERR8_D3_META_LEN == TAVRN_LINK_CONTROL_PDU_MAX - 1u &&
     TAVRN_WIRE_RERR8_D4_LEN == TAVRN_LINK_CONTROL_PDU_MAX - 1u) ? 1 : -1];
typedef char tavrn_wire_v2_rrep8_metadata_guard[
    (TAVRN_WIRE_RREP8_BASE_LEN + TAVRN_WIRE_META_HEADER_LEN +
     3u * TAVRN_WIRE_META_ENTRY_LEN == TAVRN_LINK_CONTROL_PDU_MAX - 1u) ?
        1 : -1];
typedef char tavrn_wire_v2_rrep_ack_guard[
    (TAVRN_WIRE_RREP_ACK16_LEN == 16u && TAVRN_WIRE_RREP_ACK8_LEN == 13u) ?
        1 : -1];
typedef char tavrn_wire_v2_hello8_verification_guard[
    (TAVRN_WIRE_HELLO8_VERIFICATION_LEN == 20u) ? 1 : -1];
typedef char tavrn_wire_v2_hello16_base_guard[
    (TAVRN_WIRE_HELLO16_BASE_LEN == 19u) ? 1 : -1];
typedef char tavrn_wire_v2_sync_data_entry_guard[
    (TAVRN_WIRE_SYNC_DATA_PRESENT_LEN == TAVRN_LINK_CONTROL_PDU_MAX) ? 1 : -1];
typedef char tavrn_wire_v2_sync_data_empty_guard[
    (TAVRN_WIRE_SYNC_DATA_EMPTY_LEN == 15u) ? 1 : -1];
typedef char tavrn_wire_v2_sync_offer_guard[
    (TAVRN_WIRE_SYNC_OFFER_LEN == TAVRN_LINK_CONTROL_PDU_MAX) ? 1 : -1];
typedef char tavrn_wire_v2_sync_pull_guard[
    (TAVRN_WIRE_SYNC_PULL_LEN == 22u) ? 1 : -1];
typedef char tavrn_wire_v2_tc_update_guard[
    (TAVRN_WIRE_TC_UPDATE_LEN == TAVRN_LINK_CONTROL_PDU_MAX) ? 1 : -1];

static uint16_t get_u16_le(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static void put_u16_le(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xffu);
    dst[1] = (uint8_t)(value >> 8);
}

static int is_supported_type(uint8_t type)
{
    switch ((tavrn_wire_type_t)type) {
    case TAVRN_WIRE_E_RREQ:
    case TAVRN_WIRE_E_RREP:
    case TAVRN_WIRE_E_RERR:
    case TAVRN_WIRE_HELLO:
    case TAVRN_WIRE_SYNC_OFFER:
    case TAVRN_WIRE_SYNC_PULL:
    case TAVRN_WIRE_SYNC_DATA:
    case TAVRN_WIRE_TC_UPDATE:
    case TAVRN_WIRE_E_RREP_ACK:
    case TAVRN_WIRE_DATA:
    case TAVRN_WIRE_HACK:
    case TAVRN_WIRE_FLOOD:
        return 1;
    default:
        return 0;
    }
}

static int type_has_identity_flag(tavrn_wire_type_t type)
{
    switch (type) {
    case TAVRN_WIRE_E_RREQ:
    case TAVRN_WIRE_E_RREP:
    case TAVRN_WIRE_E_RERR:
    case TAVRN_WIRE_HELLO:
    case TAVRN_WIRE_E_RREP_ACK:
    case TAVRN_WIRE_DATA:
    case TAVRN_WIRE_HACK:
    case TAVRN_WIRE_FLOOD:
        return 1;
    default:
        return 0;
    }
}

static tavrn_identity_width_t width_from_flag(uint8_t flags)
{
    return (flags & TAVRN_FLAGS_I) != 0u ? TAVRN_IDENTITY_SID8 :
        TAVRN_IDENTITY_SID16;
}

static uint8_t width_bytes(tavrn_identity_width_t width)
{
    return width == TAVRN_IDENTITY_SID8 ? TAVRN_WIRE_SID8_LEN :
        TAVRN_WIRE_SID16_LEN;
}

static int is_valid_width(tavrn_identity_width_t width)
{
    return width == TAVRN_IDENTITY_SID8 || width == TAVRN_IDENTITY_SID16;
}

static void read_id(const uint8_t *src, tavrn_identity_width_t width,
                    tavrn_logical_id_t *id)
{
    id->width = width;
    id->value = width == TAVRN_IDENTITY_SID8 ? (uint16_t)src[0] :
        get_u16_le(src);
}

static void write_id(uint8_t *dst, const tavrn_logical_id_t *id)
{
    if (id->width == TAVRN_IDENTITY_SID8) {
        dst[0] = (uint8_t)id->value;
    } else {
        put_u16_le(dst, id->value);
    }
}

static void derive_id(const uint8_t adva[TAVRN_ADVA_LEN],
                      tavrn_identity_width_t width, tavrn_logical_id_t *id)
{
    id->width = width;
    id->value = width == TAVRN_IDENTITY_SID8 ? (uint16_t)adva[0] :
        get_u16_le(adva);
}

static int is_unicast_id(const tavrn_logical_id_t *id)
{
    if (id->width == TAVRN_IDENTITY_SID8) {
        return id->value > 0u && id->value < 0xffu;
    }
    if (id->width == TAVRN_IDENTITY_SID16) {
        return id->value != 0u && id->value != 0xffffu;
    }
    return 0;
}

static int is_broadcast_id(const tavrn_logical_id_t *id)
{
    if (id->width == TAVRN_IDENTITY_SID8) {
        return id->value == 0xffu;
    }
    if (id->width == TAVRN_IDENTITY_SID16) {
        return id->value == 0xffffu;
    }
    return 0;
}

static int pdu_id_is_unicast(const uint8_t *pdu, size_t offset,
                             tavrn_identity_width_t width)
{
    tavrn_logical_id_t id;

    read_id(&pdu[offset], width, &id);
    return is_unicast_id(&id);
}

static int pdu_id_is_broadcast(const uint8_t *pdu, size_t offset,
                               tavrn_identity_width_t width)
{
    tavrn_logical_id_t id;

    read_id(&pdu[offset], width, &id);
    return is_broadcast_id(&id);
}

static int is_valid_adva(const uint8_t adva[TAVRN_ADVA_LEN])
{
    uint8_t i;
    int random_all_zero = 1;
    int random_all_one = 1;

    if ((adva[5] & 0xc0u) != 0xc0u) {
        return 0;
    }
    for (i = 0u; i < 5u; i++) {
        if (adva[i] != 0u) {
            random_all_zero = 0;
        }
        if (adva[i] != 0xffu) {
            random_all_one = 0;
        }
    }
    if ((adva[5] & 0x3fu) != 0u) {
        random_all_zero = 0;
    }
    if ((adva[5] & 0x3fu) != 0x3fu) {
        random_all_one = 0;
    }
    return !random_all_zero && !random_all_one;
}

static int ids_equal(const uint8_t *left, const uint8_t *right)
{
    return memcmp(left, right, TAVRN_ADVA_LEN) == 0;
}

static int config_is_valid(const tavrn_codec_config_t *config)
{
    tavrn_logical_id_t derived;

    if (config == NULL || config->network_id == 0u || config->network_id == 0xffu ||
        !is_valid_width(config->local_peer.logical_id.width) ||
        !is_valid_adva(config->local_peer.adva.bytes)) {
        return 0;
    }
    derive_id(config->local_peer.adva.bytes, config->local_peer.logical_id.width,
              &derived);
    return is_unicast_id(&derived) &&
        derived.value == config->local_peer.logical_id.value;
}

static tavrn_codec_result_t validate_metadata(const uint8_t *pdu, size_t pdu_len,
                                              size_t offset, uint8_t max_count)
{
    size_t expected;
    uint8_t count;
    uint8_t i;

    if (pdu_len <= offset) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    count = pdu[offset];
    if (count == 0u || count > max_count) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    expected = offset + TAVRN_WIRE_META_HEADER_LEN +
        (size_t)count * TAVRN_WIRE_META_ENTRY_LEN;
    if (pdu_len != expected) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    for (i = 0u; i < count; i++) {
        tavrn_logical_id_t id;
        size_t entry = offset + TAVRN_WIRE_META_HEADER_LEN +
            (size_t)i * TAVRN_WIRE_META_ENTRY_LEN;

        read_id(&pdu[entry], TAVRN_IDENTITY_SID8, &id);
        if (!is_unicast_id(&id) || (pdu[entry + 1u] & 0x0cu) != 0u) {
            return TAVRN_CODEC_MALFORMED_FIELD;
        }
    }
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t validate_data_pdu(const uint8_t *pdu, size_t pdu_len)
{
    tavrn_identity_width_t width = width_from_flag(pdu[5]);
    uint8_t width_len = width_bytes(width);
    size_t app_offset = TAVRN_WIRE_DATA_BASE_LEN(width_len);
    size_t app_len;
    uint8_t app_kind;
    uint8_t app_source;

    if ((pdu[5] & (uint8_t)~(TAVRN_FLAGS_I | TAVRN_FLAGS_U)) != 0u) {
        return TAVRN_CODEC_MALFORMED_FLAGS;
    }
    if (pdu_len < app_offset) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (!pdu_id_is_unicast(pdu, 7u, width) ||
        !pdu_id_is_unicast(pdu, 7u + width_len, width) ||
        !pdu_id_is_unicast(pdu, 7u + (size_t)2u * width_len, width)) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    if (pdu_len > app_offset +
          (width == TAVRN_IDENTITY_SID8 ? TAVRN_LINK_APP_BYTES :
           TAVRN_LINK_SID16_APP_BYTES)) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    app_len = pdu_len - app_offset;
    app_kind = pdu[9u + (size_t)3u * width_len];
    app_source = pdu[10u + (size_t)3u * width_len];
    if (app_kind == 0x01u && (app_len != TAVRN_LINK_SID16_APP_BYTES || app_source == 0u ||
                              app_source == 0xffu)) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    if (app_kind == 0x7fu && app_source != 0u) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t validate_hack_pdu(const uint8_t *pdu, size_t pdu_len)
{
    tavrn_identity_width_t width = width_from_flag(pdu[5]);
    uint8_t width_len = width_bytes(width);
    size_t status_offset = TAVRN_WIRE_HACK_STATUS_OFFSET(width_len);

    if ((pdu[5] & (uint8_t)~TAVRN_FLAGS_I) != 0u) {
        return TAVRN_CODEC_MALFORMED_FLAGS;
    }
    if (pdu_len != TAVRN_WIRE_HACK_LEN(width_len)) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (!pdu_id_is_unicast(pdu, 6u, width) ||
        !pdu_id_is_unicast(pdu, 6u + width_len, width) ||
        !pdu_id_is_unicast(pdu, 6u + (size_t)2u * width_len, width) ||
        pdu[status_offset] > (uint8_t)TAVRN_HACK_REJECTED) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t validate_flood_pdu(const uint8_t *pdu, size_t pdu_len)
{
    tavrn_identity_width_t width = width_from_flag(pdu[5]);
    uint8_t width_len = width_bytes(width);
    size_t body_len_offset = TAVRN_WIRE_FLOOD_BODY_OFFSET(width_len);
    size_t body_offset = TAVRN_WIRE_FLOOD_BASE_LEN(width_len);
    uint8_t body_len;
    uint8_t max_body = width == TAVRN_IDENTITY_SID8 ? TAVRN_WIRE_FLOOD8_BODY_MAX :
        TAVRN_WIRE_FLOOD16_BODY_MAX;

    if ((pdu[5] & (uint8_t)~(TAVRN_FLAGS_I | 0x40u)) != 0u) {
        return TAVRN_CODEC_MALFORMED_FLAGS;
    }
    if (pdu_len < body_offset) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (!pdu_id_is_unicast(pdu, 7u, width)) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    body_len = pdu[body_len_offset];
    if (body_len > max_body || pdu_len != body_offset + body_len) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (pdu[9u + width_len] != 0x01u && pdu[9u + width_len] != 0x02u) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t validate_rreq_pdu(const uint8_t *pdu, size_t pdu_len)
{
    tavrn_identity_width_t width = width_from_flag(pdu[5]);
    uint8_t width_len = width_bytes(width);
    size_t base_len = TAVRN_WIRE_RREQ_BASE_LEN(width_len);
    size_t dest_sequence_offset = 9u + (size_t)2u * width_len;

    if ((pdu[5] & 0x07u) != 0u) {
        return TAVRN_CODEC_MALFORMED_FLAGS;
    }
    if (pdu_len < base_len) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (!pdu_id_is_unicast(pdu, 7u, width) ||
        !pdu_id_is_unicast(pdu, 9u + width_len, width)) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    if ((pdu[5] & 0x10u) != 0u && get_u16_le(&pdu[dest_sequence_offset]) != 0u) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    if ((pdu[5] & TAVRN_FLAGS_M) == 0u) {
        return pdu_len == base_len ? TAVRN_CODEC_OK :
            TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (width != TAVRN_IDENTITY_SID8) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    return validate_metadata(pdu, pdu_len, base_len, 4u);
}

static tavrn_codec_result_t validate_rrep_pdu(const uint8_t *pdu, size_t pdu_len)
{
    tavrn_identity_width_t width = width_from_flag(pdu[5]);
    uint8_t width_len = width_bytes(width);
    size_t base_len = TAVRN_WIRE_RREP_BASE_LEN(width_len);
    size_t lifetime_offset = 11u + (size_t)3u * width_len;
    uint16_t lifetime;

    if ((pdu[5] & 0x37u) != 0u) {
        return TAVRN_CODEC_MALFORMED_FLAGS;
    }
    if (pdu_len < base_len) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (!pdu_id_is_unicast(pdu, 7u, width) ||
        !pdu_id_is_unicast(pdu, 7u + width_len, width) ||
        !pdu_id_is_unicast(pdu, 9u + (size_t)2u * width_len, width)) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    lifetime = get_u16_le(&pdu[lifetime_offset]);
    if (lifetime >= 0x4000u && lifetime < 0x8000u) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    if ((pdu[5] & TAVRN_FLAGS_M) == 0u) {
        return pdu_len == base_len ? TAVRN_CODEC_OK :
            TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (width != TAVRN_IDENTITY_SID8) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    return validate_metadata(pdu, pdu_len, base_len, 3u);
}

static tavrn_codec_result_t validate_rerr_pdu(const uint8_t *pdu, size_t pdu_len)
{
    tavrn_identity_width_t width = width_from_flag(pdu[5]);
    uint8_t width_len = width_bytes(width);
    uint8_t count;
    uint8_t max_count = width == TAVRN_IDENTITY_SID8 ? 4u : 3u;
    size_t entries_offset = TAVRN_WIRE_RERR_ENTRIES_OFFSET(width_len);
    size_t base_len;
    uint8_t i;
    uint16_t previous = 0u;

    if ((pdu[5] & 0x0fu) != 0u) {
        return TAVRN_CODEC_MALFORMED_FLAGS;
    }
    if (pdu_len < entries_offset) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    count = pdu[9u + width_len];
    if (!pdu_id_is_unicast(pdu, 7u, width) || count == 0u || count > max_count) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    base_len = TAVRN_WIRE_RERR_PDU_LEN(width_len, count);
    if (base_len > pdu_len) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    for (i = 0u; i < count; i++) {
        size_t entry = entries_offset + (size_t)i *
            TAVRN_WIRE_RERR_ENTRY_LEN(width_len);
        tavrn_logical_id_t destination;

        read_id(&pdu[entry], width, &destination);
        if (!is_unicast_id(&destination) || (i != 0u && destination.value <= previous)) {
            return TAVRN_CODEC_MALFORMED_FIELD;
        }
        previous = destination.value;
    }
    if ((pdu[5] & TAVRN_RERR_FLAGS_M) == 0u) {
        return pdu_len == base_len ? TAVRN_CODEC_OK :
            TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (width != TAVRN_IDENTITY_SID8) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    switch (count) {
    case 1u:
        return validate_metadata(pdu, pdu_len, base_len, 4u);
    case 2u:
        return validate_metadata(pdu, pdu_len, base_len, 3u);
    case 3u:
        return validate_metadata(pdu, pdu_len, base_len, 1u);
    default:
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
}

static tavrn_codec_result_t validate_hello_pdu(const uint8_t *pdu, size_t pdu_len,
                                                const uint8_t outer_adva[TAVRN_ADVA_LEN])
{
    tavrn_identity_width_t width = width_from_flag(pdu[5]);
    uint8_t width_len = width_bytes(width);
    size_t base_len = TAVRN_WIRE_HELLO_BASE_LEN(width_len);
    size_t origin_offset = 7u + (size_t)2u * width_len;
    size_t nonce_offset = 13u + (size_t)2u * width_len;
    int targeted = (pdu[5] & 0x20u) != 0u;
    int bootstrap = (pdu[5] & 0x40u) != 0u;
    int metadata = (pdu[5] & TAVRN_FLAGS_M) != 0u;

    if ((pdu[5] & 0x07u) != 0u) {
        return TAVRN_CODEC_MALFORMED_FLAGS;
    }
    if ((pdu[5] & 0x10u) != 0u && pdu[5] != 0xb8u && pdu[5] != 0xa8u) {
        return TAVRN_CODEC_MALFORMED_FLAGS;
    }
    if (pdu_len < base_len) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (targeted) {
        if (!pdu_id_is_unicast(pdu, 7u, width) ||
            !pdu_id_is_unicast(pdu, 7u + width_len, width)) {
            return TAVRN_CODEC_MALFORMED_FIELD;
        }
    } else if (!pdu_id_is_broadcast(pdu, 7u, width) ||
               !pdu_id_is_broadcast(pdu, 7u + width_len, width)) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    if (!is_valid_adva(&pdu[origin_offset])) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    if (bootstrap) {
        if (pdu[5] != 0x40u || pdu_len != base_len || pdu[6] != 0x10u ||
            get_u16_le(&pdu[nonce_offset]) == 0u ||
            !ids_equal(&pdu[origin_offset], outer_adva)) {
            return TAVRN_CODEC_MALFORMED_FIELD;
        }
        return TAVRN_CODEC_OK;
    }
    if (!metadata) {
        return pdu_len == base_len ? TAVRN_CODEC_OK :
            TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if ((pdu[5] != 0xb8u && pdu[5] != 0xa8u) ||
        pdu_len != TAVRN_WIRE_HELLO8_VERIFICATION_LEN ||
        pdu[6] == 0u ||
         pdu[base_len] != 1u ||
         !pdu_id_is_unicast(pdu, base_len + 1u, TAVRN_IDENTITY_SID8) ||
         (pdu[5] == 0xb8u && pdu[base_len + 1u] != pdu[7u + width_len]) ||
         (pdu[5] == 0xb8u && pdu[base_len + 2u] != 0x01u) ||
        (pdu[5] == 0xa8u && (pdu[base_len + 2u] & 0x0fu) != 0u) ||
        ((pdu[6] & 0x0fu) == 0u && !ids_equal(&pdu[origin_offset], outer_adva))) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t validate_sync_offer_pdu(const uint8_t *pdu,
                                                     size_t pdu_len,
                                                     const uint8_t outer_adva[TAVRN_ADVA_LEN])
{
    if (pdu_len != TAVRN_WIRE_SYNC_OFFER_LEN) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (pdu[5] != 0u) {
        return TAVRN_CODEC_MALFORMED_FLAGS;
    }
    if (pdu[6] != 0x10u || !is_valid_adva(&pdu[7]) || !is_valid_adva(&pdu[13]) ||
        !ids_equal(&pdu[7], outer_adva) || pdu[19] > 16u ||
        get_u16_le(&pdu[22]) == 0u) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t validate_sync_pull_pdu(const uint8_t *pdu,
                                                    size_t pdu_len,
                                                    const uint8_t outer_adva[TAVRN_ADVA_LEN])
{
    if (pdu_len != TAVRN_WIRE_SYNC_PULL_LEN) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (pdu[5] != 0u) {
        return TAVRN_CODEC_MALFORMED_FLAGS;
    }
    if (!is_valid_adva(&pdu[6]) || !is_valid_adva(&pdu[12]) ||
        !ids_equal(&pdu[6], outer_adva) || pdu[21] != 1u) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t validate_sync_data_pdu(const uint8_t *pdu,
                                                    size_t pdu_len)
{
    if (pdu_len < TAVRN_WIRE_SYNC_DATA_EMPTY_LEN) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if ((pdu[5] & 0x3fu) != 0u) {
        return TAVRN_CODEC_MALFORMED_FLAGS;
    }
    if (!is_valid_adva(&pdu[6])) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    if ((pdu[5] & 0x80u) == 0u) {
        if (pdu_len != TAVRN_WIRE_SYNC_DATA_EMPTY_LEN) {
            return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
        }
        return pdu[5] == 0x40u && pdu[14] == 0u ? TAVRN_CODEC_OK :
            TAVRN_CODEC_MALFORMED_FIELD;
    }
    if (pdu_len != TAVRN_WIRE_SYNC_DATA_PRESENT_LEN) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    return is_valid_adva(&pdu[15]) ? TAVRN_CODEC_OK :
        TAVRN_CODEC_MALFORMED_FIELD;
}

static tavrn_codec_result_t validate_tc_update_pdu(const uint8_t *pdu,
                                                    size_t pdu_len)
{
    if (pdu_len != TAVRN_WIRE_TC_UPDATE_LEN) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (pdu[5] != 0u) {
        return TAVRN_CODEC_MALFORMED_FLAGS;
    }
    if (!is_valid_adva(&pdu[7]) || !is_valid_adva(&pdu[15]) || pdu[21] > 1u) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t validate_rrep_ack_pdu(const uint8_t *pdu,
                                                   size_t pdu_len)
{
    tavrn_identity_width_t width = width_from_flag(pdu[5]);
    uint8_t width_len = width_bytes(width);
    size_t expected = TAVRN_WIRE_RREP_ACK_LEN(width_len);

    if ((pdu[5] & (uint8_t)~TAVRN_FLAGS_I) != 0u) {
        return TAVRN_CODEC_MALFORMED_FLAGS;
    }
    if (pdu_len != expected) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (!pdu_id_is_unicast(pdu, 6u, width) ||
        !pdu_id_is_unicast(pdu, 6u + width_len, width) ||
        !pdu_id_is_unicast(pdu, 8u + (size_t)2u * width_len, width)) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t validate_pdu(const uint8_t *pdu, size_t pdu_len,
                                         uint8_t network_id,
                                         const uint8_t outer_adva[TAVRN_ADVA_LEN])
{
    if (pdu_len < TAVRN_PDU_MIN_LEN || pdu_len > TAVRN_LINK_CONTROL_PDU_MAX) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (pdu_len < 6u) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if (pdu[0] != 0x54u || pdu[1] != 0x52u || pdu[2] != TAVRN_WIRE_VERSION) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    if (pdu[3] != network_id) {
        return TAVRN_CODEC_FOREIGN_NETWORK;
    }
    if (!is_supported_type(pdu[4])) {
        return TAVRN_CODEC_UNSUPPORTED_TYPE;
    }

    switch ((tavrn_wire_type_t)pdu[4]) {
    case TAVRN_WIRE_DATA:
        return validate_data_pdu(pdu, pdu_len);
    case TAVRN_WIRE_HACK:
        return validate_hack_pdu(pdu, pdu_len);
    case TAVRN_WIRE_FLOOD:
        return validate_flood_pdu(pdu, pdu_len);
    case TAVRN_WIRE_E_RREQ:
        return validate_rreq_pdu(pdu, pdu_len);
    case TAVRN_WIRE_E_RREP:
        return validate_rrep_pdu(pdu, pdu_len);
    case TAVRN_WIRE_E_RERR:
        return validate_rerr_pdu(pdu, pdu_len);
    case TAVRN_WIRE_HELLO:
        return validate_hello_pdu(pdu, pdu_len, outer_adva);
    case TAVRN_WIRE_SYNC_OFFER:
        return validate_sync_offer_pdu(pdu, pdu_len, outer_adva);
    case TAVRN_WIRE_SYNC_PULL:
        return validate_sync_pull_pdu(pdu, pdu_len, outer_adva);
    case TAVRN_WIRE_SYNC_DATA:
        return validate_sync_data_pdu(pdu, pdu_len);
    case TAVRN_WIRE_TC_UPDATE:
        return validate_tc_update_pdu(pdu, pdu_len);
    case TAVRN_WIRE_E_RREP_ACK:
        return validate_rrep_ack_pdu(pdu, pdu_len);
    default:
        return TAVRN_CODEC_UNSUPPORTED_TYPE;
    }
}

static tavrn_codec_result_t check_remote_id(const tavrn_codec_config_t *config,
                                            const tavrn_logical_id_t *id)
{
    if (config->identity_conflict != NULL &&
        config->identity_conflict(config->identity_context, id, NULL) != 0) {
        return TAVRN_CODEC_IDENTITY_CONFLICT;
    }
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t check_pdu_id(const tavrn_codec_config_t *config,
                                         const uint8_t *pdu, size_t offset,
                                         tavrn_identity_width_t width)
{
    tavrn_logical_id_t id;

    read_id(&pdu[offset], width, &id);
    return check_remote_id(config, &id);
}

static tavrn_codec_result_t check_metadata_ids(const tavrn_codec_config_t *config,
                                               const uint8_t *pdu, size_t offset)
{
    uint8_t count = pdu[offset];
    uint8_t i;
    tavrn_codec_result_t result;

    for (i = 0u; i < count; i++) {
        result = check_pdu_id(config, pdu, offset + 1u + (size_t)i * 2u,
                              TAVRN_IDENTITY_SID8);
        if (result != TAVRN_CODEC_OK) {
            return result;
        }
    }
    return TAVRN_CODEC_OK;
}

/* Bootstrap and topology controls carry complete AdvAs, not compressed route
 * identities.  They deliberately remain admissible while a FULL node is in
 * SID8 mode so a late/new incarnation can recover context without guessing a
 * SID8 mapping. */
static int is_full_identity_control(const uint8_t *pdu)
{
    tavrn_wire_type_t type = (tavrn_wire_type_t)pdu[4];

    return type == TAVRN_WIRE_SYNC_OFFER || type == TAVRN_WIRE_SYNC_PULL ||
        type == TAVRN_WIRE_SYNC_DATA || type == TAVRN_WIRE_TC_UPDATE ||
        (type == TAVRN_WIRE_HELLO && pdu[5] == 0x40u);
}

static tavrn_codec_result_t check_pdu_identities(
    const tavrn_codec_config_t *config, const uint8_t *pdu,
    const uint8_t outer_adva[TAVRN_ADVA_LEN])
{
    tavrn_wire_type_t type = (tavrn_wire_type_t)pdu[4];
    tavrn_identity_width_t width = type_has_identity_flag(type) ?
        width_from_flag(pdu[5]) : config->local_peer.logical_id.width;
    uint8_t width_len = width_bytes(width);
    tavrn_logical_id_t direct;
    tavrn_adva_t direct_adva;
    tavrn_codec_result_t result;

    if (config->local_peer.logical_id.width == TAVRN_IDENTITY_SID8 &&
        config->identity_conflict == NULL) {
        return TAVRN_CODEC_IDENTITY_CONFLICT;
    }
    if (is_full_identity_control(pdu)) {
        return TAVRN_CODEC_OK;
    }
    if (width != config->local_peer.logical_id.width) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    derive_id(outer_adva, width, &direct);
    if (!is_unicast_id(&direct)) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    memcpy(direct_adva.bytes, outer_adva, TAVRN_ADVA_LEN);
    if (config->identity_conflict != NULL &&
        config->identity_conflict(config->identity_context, &direct, &direct_adva) != 0) {
        return TAVRN_CODEC_IDENTITY_CONFLICT;
    }

    switch (type) {
    case TAVRN_WIRE_DATA:
        result = check_pdu_id(config, pdu, 7u, width);
        if (result == TAVRN_CODEC_OK) {
            result = check_pdu_id(config, pdu, 7u + width_len, width);
        }
        if (result == TAVRN_CODEC_OK) {
            result = check_pdu_id(config, pdu, 7u + (size_t)2u * width_len, width);
        }
        return result;
    case TAVRN_WIRE_HACK:
        result = check_pdu_id(config, pdu, 6u, width);
        /* validate_hack_pdu() structurally validates the origin/destination
         * custody keys.  Only the immediate receiver is live-resolved here;
         * exact active-custody correlation remains the mutation boundary. */
        return result;
    case TAVRN_WIRE_FLOOD:
        return check_pdu_id(config, pdu, 7u, width);
    case TAVRN_WIRE_E_RREQ:
        result = check_pdu_id(config, pdu, 7u, width);
        if (result == TAVRN_CODEC_OK) {
            result = check_pdu_id(config, pdu, 9u + width_len, width);
        }
        if (result == TAVRN_CODEC_OK && (pdu[5] & TAVRN_FLAGS_M) != 0u) {
            result = check_metadata_ids(config, pdu, 13u + (size_t)2u * width_len);
        }
        return result;
    case TAVRN_WIRE_E_RREP:
        result = check_pdu_id(config, pdu, 7u, width);
        if (result == TAVRN_CODEC_OK) {
            result = check_pdu_id(config, pdu, 7u + width_len, width);
        }
        if (result == TAVRN_CODEC_OK) {
            result = check_pdu_id(config, pdu, 9u + (size_t)2u * width_len, width);
        }
        if (result == TAVRN_CODEC_OK && (pdu[5] & TAVRN_FLAGS_M) != 0u) {
            result = check_metadata_ids(config, pdu, 13u + (size_t)3u * width_len);
        }
        return result;
    case TAVRN_WIRE_E_RERR: {
        uint8_t count = pdu[9u + width_len];
        uint8_t i;
        size_t entries = TAVRN_WIRE_RERR_ENTRIES_OFFSET(width_len);

        result = check_pdu_id(config, pdu, 7u, width);
        for (i = 0u; result == TAVRN_CODEC_OK && i < count; i++) {
            result = check_pdu_id(config, pdu,
                                   entries + (size_t)i *
                                   TAVRN_WIRE_RERR_ENTRY_LEN(width_len),
                                  width);
        }
        if (result == TAVRN_CODEC_OK && (pdu[5] & TAVRN_RERR_FLAGS_M) != 0u) {
            result = check_metadata_ids(config, pdu,
                                        entries + (size_t)count *
                                         TAVRN_WIRE_RERR_ENTRY_LEN(width_len));
        }
        return result;
    }
    case TAVRN_WIRE_HELLO:
        if ((pdu[5] & 0x20u) == 0u) {
            return TAVRN_CODEC_OK;
        }
        result = check_pdu_id(config, pdu, 7u, width);
        if (result == TAVRN_CODEC_OK) {
            result = check_pdu_id(config, pdu, 7u + width_len, width);
        }
        if (result == TAVRN_CODEC_OK && (pdu[5] & TAVRN_FLAGS_M) != 0u) {
            result = check_metadata_ids(config, pdu, 15u + (size_t)2u * width_len);
        }
        return result;
    case TAVRN_WIRE_E_RREP_ACK:
        result = check_pdu_id(config, pdu, 6u, width);
        if (result == TAVRN_CODEC_OK) {
            result = check_pdu_id(config, pdu, 6u + width_len, width);
        }
        if (result == TAVRN_CODEC_OK) {
            result = check_pdu_id(config, pdu, 8u + (size_t)2u * width_len, width);
        }
        return result;
    default:
        return TAVRN_CODEC_OK;
    }
}

static void decode_data(const uint8_t *pdu, tavrn_codec_data_t *data)
{
    tavrn_identity_width_t width = width_from_flag(pdu[5]);
    uint8_t width_len = width_bytes(width);
    read_id(&pdu[7], width, &data->immediate_receiver);
    read_id(&pdu[7u + width_len], width, &data->data.origin);
    read_id(&pdu[7u + (size_t)2u * width_len], width,
            &data->data.final_destination);
    data->data.data_seq = get_u16_le(&pdu[7u + (size_t)3u * width_len]);
    data->data.ttl = (uint8_t)(pdu[6] >> 4);
    data->data.hops = (uint8_t)(pdu[6] & 0x0fu);
    data->data.app_kind = pdu[9u + (size_t)3u * width_len];
    data->data.app_source = pdu[10u + (size_t)3u * width_len];
    data->data.urgent = (pdu[5] & TAVRN_FLAGS_U) != 0u ? 1u : 0u;
    data->data.ownership = TAVRN_DATA_TRANSIT;
}

static void decode_data_with_len(const uint8_t *pdu, size_t pdu_len,
                                 tavrn_codec_data_t *data)
{
    tavrn_identity_width_t width = width_from_flag(pdu[5]);
    uint8_t width_len = width_bytes(width);
    size_t app_offset = TAVRN_WIRE_DATA_BASE_LEN(width_len);

    decode_data(pdu, data);
    data->data.app_len = (uint8_t)(pdu_len - app_offset);
    if (data->data.app_len != 0u) {
        memcpy(data->data.app_bytes, &pdu[app_offset], data->data.app_len);
    }
}

static void decode_hack(const uint8_t *pdu, tavrn_codec_hack_t *hack)
{
    tavrn_identity_width_t width = width_from_flag(pdu[5]);
    uint8_t width_len = width_bytes(width);

    read_id(&pdu[6], width, &hack->immediate_receiver);
    read_id(&pdu[6u + width_len], width, &hack->data_origin);
    read_id(&pdu[6u + (size_t)2u * width_len], width, &hack->final_destination);
    hack->data_seq = get_u16_le(&pdu[6u + (size_t)3u * width_len]);
    hack->app_kind = pdu[8u + (size_t)3u * width_len];
    hack->app_source = pdu[9u + (size_t)3u * width_len];
    hack->status = (tavrn_hack_status_t)pdu[10u + (size_t)3u * width_len];
}

static void decode_flood(const uint8_t *pdu, tavrn_codec_flood_t *flood)
{
    tavrn_identity_width_t width = width_from_flag(pdu[5]);
    uint8_t width_len = width_bytes(width);
    size_t body_len_offset = TAVRN_WIRE_FLOOD_BODY_OFFSET(width_len);

    read_id(&pdu[7], width, &flood->origin);
    flood->flood_seq = get_u16_le(&pdu[7u + width_len]);
    flood->ttl = (uint8_t)(pdu[6] >> 4);
    flood->hops = (uint8_t)(pdu[6] & 0x0fu);
    flood->urgent = (pdu[5] & 0x40u) != 0u ? 1u : 0u;
    flood->flood_class = pdu[9u + width_len];
    flood->body_len = pdu[body_len_offset];
    if (flood->body_len != 0u) {
        memcpy(flood->body, &pdu[body_len_offset + 1u], flood->body_len);
    }
}

static tavrn_codec_result_t validate_wrapper(const uint8_t *adv_data, size_t adv_len,
                                             const uint8_t **pdu_out,
                                             size_t *pdu_len_out)
{
    size_t pdu_len;

    if (adv_len > TAVRN_ADV_MAX_LEN || adv_len < TAVRN_ADV_WRAPPER_LEN) {
        return TAVRN_CODEC_MALFORMED_WRAPPER;
    }
    if (adv_data[0] != 0x02u || adv_data[1] != 0x01u || adv_data[2] != 0x06u ||
        adv_data[4] != 0xffu || adv_data[5] != 0xffu || adv_data[6] != 0xffu) {
        return TAVRN_CODEC_MALFORMED_WRAPPER;
    }
    if (adv_data[3] < 8u || adv_data[3] > 27u ||
        adv_len != 4u + (size_t)adv_data[3]) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    pdu_len = (size_t)adv_data[3] - 3u;
    if (pdu_len < TAVRN_PDU_MIN_LEN || pdu_len > TAVRN_LINK_CONTROL_PDU_MAX) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    *pdu_out = &adv_data[TAVRN_ADV_WRAPPER_LEN];
    *pdu_len_out = pdu_len;
    return TAVRN_CODEC_OK;
}

tavrn_codec_result_t tavrn_wire_v2_decode(
    const tavrn_codec_config_t *config, const uint8_t outer_adva[TAVRN_ADVA_LEN],
    const uint8_t *adv_data, size_t adv_len, tavrn_decoded_frame_t *frame_out)
{
    const uint8_t *pdu;
    size_t pdu_len;
    tavrn_codec_result_t result;
    tavrn_decoded_frame_t decoded;
    tavrn_identity_width_t width;

    if (frame_out == NULL) {
        return TAVRN_CODEC_INVALID_ARGUMENT;
    }
    memset(frame_out, 0, sizeof(*frame_out));
    if (config == NULL || outer_adva == NULL || adv_data == NULL ||
        !config_is_valid(config)) {
        return TAVRN_CODEC_INVALID_ARGUMENT;
    }
    if (!is_valid_adva(outer_adva)) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    result = validate_wrapper(adv_data, adv_len, &pdu, &pdu_len);
    if (result != TAVRN_CODEC_OK) {
        return result;
    }
    result = validate_pdu(pdu, pdu_len, config->network_id, outer_adva);
    if (result != TAVRN_CODEC_OK) {
        return result;
    }
    width = is_full_identity_control(pdu) ? TAVRN_IDENTITY_SID16 :
        (type_has_identity_flag((tavrn_wire_type_t)pdu[4]) ?
             width_from_flag(pdu[5]) : config->local_peer.logical_id.width);
    result = check_pdu_identities(config, pdu, outer_adva);
    if (result != TAVRN_CODEC_OK) {
        return result;
    }

    memset(&decoded, 0, sizeof(decoded));
    decoded.type = (tavrn_wire_type_t)pdu[4];
    decoded.network_id = pdu[3];
    memcpy(decoded.transmitter.adva.bytes, outer_adva, TAVRN_ADVA_LEN);
    derive_id(outer_adva, width, &decoded.transmitter.logical_id);
    switch (decoded.type) {
    case TAVRN_WIRE_DATA:
        decode_data_with_len(pdu, pdu_len, &decoded.detail.data);
        break;
    case TAVRN_WIRE_HACK:
        decode_hack(pdu, &decoded.detail.hack);
        break;
    case TAVRN_WIRE_FLOOD:
        decode_flood(pdu, &decoded.detail.flood);
        break;
    default:
        decoded.detail.control.type = decoded.type;
        decoded.detail.control.pdu_len = (uint8_t)pdu_len;
        memcpy(decoded.detail.control.pdu, pdu, pdu_len);
        break;
    }
    *frame_out = decoded;
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t validate_encode_id(const tavrn_logical_id_t *id,
                                               tavrn_identity_width_t width)
{
    if (id->width != width || !is_unicast_id(id)) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t build_data_pdu(const tavrn_decoded_frame_t *frame,
                                           uint8_t pdu[TAVRN_LINK_CONTROL_PDU_MAX],
                                           size_t *pdu_len)
{
    const tavrn_codec_data_t *data = &frame->detail.data;
    tavrn_identity_width_t width = data->immediate_receiver.width;
    uint8_t width_len;
    size_t app_offset;

    if (!is_valid_width(width) || data->data.urgent > 1u || data->data.ttl > 15u ||
        data->data.hops > 15u ||
        validate_encode_id(&data->immediate_receiver, width) != TAVRN_CODEC_OK ||
        validate_encode_id(&data->data.origin, width) != TAVRN_CODEC_OK ||
        validate_encode_id(&data->data.final_destination, width) != TAVRN_CODEC_OK) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    width_len = width_bytes(width);
    app_offset = TAVRN_WIRE_DATA_BASE_LEN(width_len);
    if (data->data.app_len > TAVRN_LINK_APP_BYTES ||
        data->data.app_len > (width == TAVRN_IDENTITY_SID8 ?
                               TAVRN_LINK_APP_BYTES : TAVRN_LINK_SID16_APP_BYTES)) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    if ((data->data.app_kind == 0x01u &&
          (data->data.app_len != TAVRN_LINK_SID16_APP_BYTES ||
           data->data.app_source == 0u ||
          data->data.app_source == 0xffu)) ||
        (data->data.app_kind == 0x7fu && data->data.app_source != 0u)) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    *pdu_len = app_offset + data->data.app_len;
    pdu[0] = 0x54u;
    pdu[1] = 0x52u;
    pdu[2] = TAVRN_WIRE_VERSION;
    pdu[3] = frame->network_id;
    pdu[4] = (uint8_t)TAVRN_WIRE_DATA;
    pdu[5] = (width == TAVRN_IDENTITY_SID8 ? TAVRN_FLAGS_I : 0u) |
        (data->data.urgent != 0u ? TAVRN_FLAGS_U : 0u);
    pdu[6] = (uint8_t)((data->data.ttl << 4) | data->data.hops);
    write_id(&pdu[7], &data->immediate_receiver);
    write_id(&pdu[7u + width_len], &data->data.origin);
    write_id(&pdu[7u + (size_t)2u * width_len], &data->data.final_destination);
    put_u16_le(&pdu[7u + (size_t)3u * width_len], data->data.data_seq);
    pdu[9u + (size_t)3u * width_len] = data->data.app_kind;
    pdu[10u + (size_t)3u * width_len] = data->data.app_source;
    if (data->data.app_len != 0u) {
        memcpy(&pdu[app_offset], data->data.app_bytes, data->data.app_len);
    }
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t build_hack_pdu(const tavrn_decoded_frame_t *frame,
                                           uint8_t pdu[TAVRN_LINK_CONTROL_PDU_MAX],
                                           size_t *pdu_len)
{
    const tavrn_codec_hack_t *hack = &frame->detail.hack;
    tavrn_identity_width_t width = hack->immediate_receiver.width;
    uint8_t width_len;

    if (!is_valid_width(width) ||
        validate_encode_id(&hack->immediate_receiver, width) != TAVRN_CODEC_OK ||
        validate_encode_id(&hack->data_origin, width) != TAVRN_CODEC_OK ||
        validate_encode_id(&hack->final_destination, width) != TAVRN_CODEC_OK ||
        hack->status > TAVRN_HACK_REJECTED) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    width_len = width_bytes(width);
    *pdu_len = TAVRN_WIRE_HACK_LEN(width_len);
    pdu[0] = 0x54u;
    pdu[1] = 0x52u;
    pdu[2] = TAVRN_WIRE_VERSION;
    pdu[3] = frame->network_id;
    pdu[4] = (uint8_t)TAVRN_WIRE_HACK;
    pdu[5] = width == TAVRN_IDENTITY_SID8 ? TAVRN_FLAGS_I : 0u;
    write_id(&pdu[6], &hack->immediate_receiver);
    write_id(&pdu[6u + width_len], &hack->data_origin);
    write_id(&pdu[6u + (size_t)2u * width_len], &hack->final_destination);
    put_u16_le(&pdu[6u + (size_t)3u * width_len], hack->data_seq);
    pdu[8u + (size_t)3u * width_len] = hack->app_kind;
    pdu[9u + (size_t)3u * width_len] = hack->app_source;
    pdu[10u + (size_t)3u * width_len] = (uint8_t)hack->status;
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t build_flood_pdu(const tavrn_decoded_frame_t *frame,
                                            uint8_t pdu[TAVRN_LINK_CONTROL_PDU_MAX],
                                            size_t *pdu_len)
{
    const tavrn_codec_flood_t *flood = &frame->detail.flood;
    tavrn_identity_width_t width = flood->origin.width;
    uint8_t width_len;
    uint8_t max_body;
    size_t body_offset;

    if (!is_valid_width(width) || flood->ttl > 15u || flood->hops > 15u ||
        flood->urgent > 1u ||
        validate_encode_id(&flood->origin, width) != TAVRN_CODEC_OK ||
        (flood->flood_class != 0x01u && flood->flood_class != 0x02u)) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    width_len = width_bytes(width);
    max_body = width == TAVRN_IDENTITY_SID8 ? TAVRN_WIRE_FLOOD8_BODY_MAX :
        TAVRN_WIRE_FLOOD16_BODY_MAX;
    if (flood->body_len > max_body) {
        return TAVRN_CODEC_MALFORMED_EXACT_LENGTH;
    }
    body_offset = TAVRN_WIRE_FLOOD_BASE_LEN(width_len);
    *pdu_len = body_offset + flood->body_len;
    pdu[0] = 0x54u;
    pdu[1] = 0x52u;
    pdu[2] = TAVRN_WIRE_VERSION;
    pdu[3] = frame->network_id;
    pdu[4] = (uint8_t)TAVRN_WIRE_FLOOD;
    pdu[5] = (width == TAVRN_IDENTITY_SID8 ? TAVRN_FLAGS_I : 0u) |
        (flood->urgent != 0u ? 0x40u : 0u);
    pdu[6] = (uint8_t)((flood->ttl << 4) | flood->hops);
    write_id(&pdu[7], &flood->origin);
    put_u16_le(&pdu[7u + width_len], flood->flood_seq);
    pdu[9u + width_len] = flood->flood_class;
    pdu[10u + width_len] = flood->body_len;
    if (flood->body_len != 0u) {
        memcpy(&pdu[body_offset], flood->body, flood->body_len);
    }
    return TAVRN_CODEC_OK;
}

static tavrn_codec_result_t build_control_pdu(const tavrn_decoded_frame_t *frame,
                                              uint8_t pdu[TAVRN_LINK_CONTROL_PDU_MAX],
                                              size_t *pdu_len)
{
    const tavrn_validated_control_t *control = &frame->detail.control;
    uint8_t i;

    if (!is_supported_type((uint8_t)frame->type) || frame->type == TAVRN_WIRE_DATA ||
        frame->type == TAVRN_WIRE_HACK || frame->type == TAVRN_WIRE_FLOOD ||
        control->type != frame->type || control->pdu_len < TAVRN_PDU_MIN_LEN ||
        control->pdu_len > TAVRN_LINK_CONTROL_PDU_MAX ||
        control->pdu[4] != (uint8_t)frame->type) {
        return TAVRN_CODEC_MALFORMED_FIELD;
    }
    for (i = control->pdu_len; i < TAVRN_LINK_CONTROL_PDU_MAX; i++) {
        if (control->pdu[i] != 0u) {
            return TAVRN_CODEC_MALFORMED_FIELD;
        }
    }
    memcpy(pdu, control->pdu, control->pdu_len);
    *pdu_len = control->pdu_len;
    return TAVRN_CODEC_OK;
}

tavrn_codec_result_t tavrn_wire_v2_encode(
    const tavrn_codec_config_t *config, const tavrn_decoded_frame_t *frame,
    uint8_t *adv_data_out, size_t adv_capacity, size_t *adv_len_out)
{
    uint8_t pdu[TAVRN_LINK_CONTROL_PDU_MAX];
    uint8_t adv[TAVRN_ADV_MAX_LEN];
    size_t pdu_len = 0u;
    size_t adv_len;
    tavrn_codec_result_t result;
    const uint8_t *validation_adva;

    if (config == NULL || frame == NULL || adv_data_out == NULL || adv_len_out == NULL ||
        !config_is_valid(config)) {
        return TAVRN_CODEC_INVALID_ARGUMENT;
    }
    if (frame->network_id != config->network_id) {
        return TAVRN_CODEC_FOREIGN_NETWORK;
    }
    validation_adva = config->local_peer.adva.bytes;
    memset(pdu, 0, sizeof(pdu));
    switch (frame->type) {
    case TAVRN_WIRE_DATA:
        result = build_data_pdu(frame, pdu, &pdu_len);
        break;
    case TAVRN_WIRE_HACK:
        result = build_hack_pdu(frame, pdu, &pdu_len);
        break;
    case TAVRN_WIRE_FLOOD:
        result = build_flood_pdu(frame, pdu, &pdu_len);
        break;
    default:
        result = build_control_pdu(frame, pdu, &pdu_len);
        if (result == TAVRN_CODEC_OK && is_valid_adva(frame->transmitter.adva.bytes)) {
            validation_adva = frame->transmitter.adva.bytes;
        }
        break;
    }
    if (result != TAVRN_CODEC_OK) {
        return result;
    }
    result = validate_pdu(pdu, pdu_len, config->network_id,
                          validation_adva);
    if (result != TAVRN_CODEC_OK) {
        return result;
    }
    /* Context protects incoming SID8 state mutation.  Outbound frames already
     * carry their direct identities and must not require a remote mapping. */
    adv_len = TAVRN_ADV_WRAPPER_LEN + pdu_len;
    if (adv_capacity < adv_len) {
        return TAVRN_CODEC_OUTPUT_TOO_SMALL;
    }
    adv[0] = 0x02u;
    adv[1] = 0x01u;
    adv[2] = 0x06u;
    adv[3] = (uint8_t)(3u + pdu_len);
    adv[4] = 0xffu;
    adv[5] = 0xffu;
    adv[6] = 0xffu;
    memcpy(&adv[TAVRN_ADV_WRAPPER_LEN], pdu, pdu_len);
    memcpy(adv_data_out, adv, adv_len);
    *adv_len_out = adv_len;
    return TAVRN_CODEC_OK;
}
