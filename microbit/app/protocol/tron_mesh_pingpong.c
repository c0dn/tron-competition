#include "tron_mesh_pingpong.h"

static int time_reached(uint32_t now_ms, uint32_t deadline_ms)
{
    return (uint32_t)(now_ms - deadline_ms) < 0x80000000UL;
}

static uint16_t packet_destination(const tron_mesh_packet_t *packet)
{
    return (uint16_t)((uint16_t)packet->payload[0] |
                      ((uint16_t)packet->payload[1] << 8));
}

static void clear_packet(tron_mesh_packet_t *packet)
{
    tron_mesh_packet_t empty = { 0 };

    *packet = empty;
}

static void set_destination(tron_mesh_packet_t *packet, uint16_t destination)
{
    packet->payload_len = TRON_MESH_PINGPONG_PAYLOAD_LEN;
    packet->payload[0] = (uint8_t)(destination & 0xFFu);
    packet->payload[1] = (uint8_t)((destination >> 8) & 0xFFu);
}

static uint32_t advance_periodic_deadline(uint32_t deadline_ms, uint32_t now_ms,
                                          uint32_t interval_ms)
{
    uint32_t elapsed = now_ms - deadline_ms;
    uint32_t periods = (elapsed / interval_ms) + 1u;

    return deadline_ms + (periods * interval_ms);
}

int tron_mesh_pingpong_is_supported_type(uint8_t msg_type)
{
    return msg_type == TRON_MESH_MSG_TYPE_PING ||
           msg_type == TRON_MESH_MSG_TYPE_PONG;
}

tron_mesh_pingpong_result_t tron_mesh_pingpong_validate(
    const tron_mesh_packet_t *packet)
{
    uint16_t expected_source;
    uint16_t expected_destination;

    if (packet == NULL) {
        return TRON_MESH_PINGPONG_ERR_NULL;
    }
    if (!tron_mesh_pingpong_is_supported_type(packet->msg_type)) {
        return TRON_MESH_PINGPONG_ERR_TYPE;
    }
    if (packet->net_id != TRON_MESH_PINGPONG_NET_ID) {
        return TRON_MESH_PINGPONG_ERR_NETWORK;
    }
    if (packet->ttl > TRON_MESH_TTL_MAX) {
        return TRON_MESH_PINGPONG_ERR_TTL;
    }
    if (packet->payload_len != TRON_MESH_PINGPONG_PAYLOAD_LEN) {
        return TRON_MESH_PINGPONG_ERR_PAYLOAD_LEN;
    }

    if (packet->msg_type == TRON_MESH_MSG_TYPE_PING) {
        expected_source = TRON_MESH_PINGPONG_ROOT_ID;
        expected_destination = TRON_MESH_PINGPONG_LEAF_ID;
    } else {
        expected_source = TRON_MESH_PINGPONG_LEAF_ID;
        expected_destination = TRON_MESH_PINGPONG_ROOT_ID;
    }

    if (packet->src != expected_source) {
        return TRON_MESH_PINGPONG_ERR_SOURCE;
    }
    if (packet_destination(packet) != expected_destination) {
        return TRON_MESH_PINGPONG_ERR_DESTINATION;
    }
    return TRON_MESH_PINGPONG_OK;
}

tron_mesh_pingpong_result_t tron_mesh_pingpong_build_ping(
    tron_mesh_packet_t *packet,
    uint8_t ttl,
    uint32_t seq24)
{
    if (packet == NULL) {
        return TRON_MESH_PINGPONG_ERR_NULL;
    }
    if (ttl > TRON_MESH_TTL_MAX) {
        return TRON_MESH_PINGPONG_ERR_TTL;
    }

    clear_packet(packet);
    packet->net_id = TRON_MESH_PINGPONG_NET_ID;
    packet->ttl = ttl;
    packet->src = TRON_MESH_PINGPONG_ROOT_ID;
    packet->seq24 = seq24 & TRON_MESH_SEQ24_MAX;
    packet->msg_type = TRON_MESH_MSG_TYPE_PING;
    set_destination(packet, TRON_MESH_PINGPONG_LEAF_ID);
    return TRON_MESH_PINGPONG_OK;
}

tron_mesh_pingpong_result_t tron_mesh_pingpong_build_pong(
    tron_mesh_packet_t *packet,
    const tron_mesh_packet_t *ping,
    uint8_t ttl)
{
    tron_mesh_pingpong_result_t result;
    uint32_t ping_seq24;

    if (packet == NULL || ping == NULL) {
        return TRON_MESH_PINGPONG_ERR_NULL;
    }
    result = tron_mesh_pingpong_validate(ping);
    if (result != TRON_MESH_PINGPONG_OK) {
        return result;
    }
    if (ping->msg_type != TRON_MESH_MSG_TYPE_PING) {
        return TRON_MESH_PINGPONG_ERR_TYPE;
    }
    if (ttl > TRON_MESH_TTL_MAX) {
        return TRON_MESH_PINGPONG_ERR_TTL;
    }

    /* Preserve the correlation before clearing so in-place conversion is safe. */
    ping_seq24 = ping->seq24 & TRON_MESH_SEQ24_MAX;
    clear_packet(packet);
    packet->net_id = TRON_MESH_PINGPONG_NET_ID;
    packet->ttl = ttl;
    packet->src = TRON_MESH_PINGPONG_LEAF_ID;
    packet->seq24 = ping_seq24;
    packet->msg_type = TRON_MESH_MSG_TYPE_PONG;
    set_destination(packet, TRON_MESH_PINGPONG_ROOT_ID);
    return TRON_MESH_PINGPONG_OK;
}

void tron_mesh_pingpong_root_init(tron_mesh_pingpong_root_t *state,
                                  uint32_t now_ms)
{
    tron_mesh_pingpong_root_t empty = { 0 };

    if (state == NULL) {
        return;
    }
    *state = empty;
    if (!tron_timer_config_is_valid(&tron_timer_config)) {
        return;
    }
    state->timers = &tron_timer_config;
    state->next_ping_at_ms = now_ms + state->timers->legacy_ping_interval_ms;
}

int tron_mesh_pingpong_root_ping_due(const tron_mesh_pingpong_root_t *state,
                                     uint32_t now_ms)
{
    return state != NULL && tron_timer_config_is_valid(state->timers) &&
           state->pending == 0u &&
           time_reached(now_ms, state->next_ping_at_ms);
}

tron_mesh_pingpong_attempt_result_t tron_mesh_pingpong_root_record_ping_attempt(
    tron_mesh_pingpong_root_t *state,
    uint32_t seq24,
    uint32_t now_ms,
    int queue_succeeded)
{
    if (!tron_mesh_pingpong_root_ping_due(state, now_ms)) {
        return TRON_MESH_PINGPONG_ATTEMPT_NOT_DUE;
    }

    state->next_ping_at_ms = advance_periodic_deadline(
        state->next_ping_at_ms, now_ms, state->timers->legacy_ping_interval_ms);
    if (!queue_succeeded) {
        return TRON_MESH_PINGPONG_ATTEMPT_QUEUE_FAILED;
    }

    state->pending = 1u;
    state->pending_seq24 = seq24 & TRON_MESH_SEQ24_MAX;
    state->pending_queued_at_ms = now_ms;
    state->pending_deadline_ms = now_ms + state->timers->legacy_ping_timeout_ms;
    return TRON_MESH_PINGPONG_ATTEMPT_QUEUED;
}

int tron_mesh_pingpong_root_check_timeout(tron_mesh_pingpong_root_t *state,
                                          uint32_t now_ms)
{
    if (state == NULL || state->pending == 0u ||
        !time_reached(now_ms, state->pending_deadline_ms)) {
        return 0;
    }

    state->pending = 0u;
    return 1;
}

tron_mesh_pingpong_match_result_t tron_mesh_pingpong_root_match_pong(
    tron_mesh_pingpong_root_t *state,
    const tron_mesh_packet_t *pong,
    uint32_t now_ms,
    uint32_t *rtt_ms)
{
    if (state == NULL || pong == NULL ||
        tron_mesh_pingpong_validate(pong) != TRON_MESH_PINGPONG_OK ||
        pong->msg_type != TRON_MESH_MSG_TYPE_PONG ||
        state->pending == 0u ||
        (pong->seq24 & TRON_MESH_SEQ24_MAX) != state->pending_seq24) {
        return TRON_MESH_PINGPONG_PONG_UNMATCHED;
    }

    if (time_reached(now_ms, state->pending_deadline_ms)) {
        state->pending = 0u;
        return TRON_MESH_PINGPONG_PONG_LATE;
    }

    if (rtt_ms != NULL) {
        *rtt_ms = now_ms - state->pending_queued_at_ms;
    }
    state->pending = 0u;
    return TRON_MESH_PINGPONG_PONG_MATCHED;
}

const char *tron_mesh_pingpong_result_name(tron_mesh_pingpong_result_t result)
{
    switch (result) {
    case TRON_MESH_PINGPONG_OK:
        return "OK";
    case TRON_MESH_PINGPONG_ERR_NULL:
        return "ERR_NULL";
    case TRON_MESH_PINGPONG_ERR_TYPE:
        return "ERR_TYPE";
    case TRON_MESH_PINGPONG_ERR_NETWORK:
        return "ERR_NETWORK";
    case TRON_MESH_PINGPONG_ERR_TTL:
        return "ERR_TTL";
    case TRON_MESH_PINGPONG_ERR_PAYLOAD_LEN:
        return "ERR_PAYLOAD_LEN";
    case TRON_MESH_PINGPONG_ERR_SOURCE:
        return "ERR_SOURCE";
    case TRON_MESH_PINGPONG_ERR_DESTINATION:
        return "ERR_DESTINATION";
    default:
        return "ERR_UNKNOWN";
    }
}
