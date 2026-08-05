#include "aodv_core.h"

#include <string.h>

#define AODV_RREQ_RING_COUNT 5u
#define AODV_DISCOVERY_REQUEST_CAPACITY \
    (AODV_RREQ_RING_COUNT + 2u)
#define AODV_DISCOVERY_CAPACITY TAVRN_AODV_PENDING_DATA_CAPACITY
#define AODV_RREP_SEEN_CAPACITY 16u
#define AODV_RERR_SEEN_CAPACITY 16u
#define AODV_BLACKLIST_CAPACITY TAVRN_AODV_RREP_ACK_WAIT_CAPACITY

#define AODV_PDU_PREFIX_LEN 5u
#define AODV_PDU_FLAGS_OFFSET 5u
#define AODV_PDU_TTL_HOPS_OFFSET 6u
#define AODV_FLAG_IDENTITY_SID8 0x80u
#define AODV_FLAG_RREP_ACK_REQUIRED 0x40u
#define AODV_FLAG_RREQ_DEST_UNKNOWN 0x10u

typedef struct aodv_route_entry {
    tavrn_direct_peer_t next_hop;
    uint32_t expires_at_ms;
    uint16_t destination;
    uint16_t destination_sequence;
    uint16_t precursors[TAVRN_AODV_PRECURSOR_CAPACITY];
    uint8_t occupied;
    uint8_t hop_count;
    uint8_t sequence_valid;
    uint8_t precursor_count;
    uint8_t precursor_overflow;
    aodv_route_state_t state;
} aodv_route_entry_t;

typedef struct aodv_rreq_seen_entry {
    uint32_t expires_at_ms;
    uint16_t origin;
    uint16_t request_id;
    uint8_t occupied;
} aodv_rreq_seen_entry_t;

typedef struct aodv_rrep_seen_entry {
    uint32_t expires_at_ms;
    uint16_t destination;
    uint16_t destination_sequence;
    uint16_t origin;
    uint16_t request_id;
    uint8_t occupied;
} aodv_rrep_seen_entry_t;

typedef struct aodv_rerr_seen_entry {
    uint32_t expires_at_ms;
    uint16_t reporter;
    uint16_t sequence;
    uint8_t occupied;
} aodv_rerr_seen_entry_t;

typedef struct aodv_discovery_entry {
    uint32_t next_attempt_ms;
    uint16_t destination;
    uint16_t request_ids[AODV_DISCOVERY_REQUEST_CAPACITY];
    uint8_t occupied;
    uint8_t next_ring;
    uint8_t request_count;
} aodv_discovery_entry_t;

typedef struct aodv_pending_entry {
    tavrn_link_data_t data;
    uint32_t expires_at_ms;
    uint8_t occupied;
} aodv_pending_entry_t;

typedef struct aodv_rrep_ack_wait {
    tavrn_direct_peer_t expected_peer;
    uint32_t expires_at_ms;
    uint16_t token;
    uint16_t destination;
    uint16_t destination_sequence;
    uint16_t origin;
    uint16_t request_id;
    uint8_t occupied;
    uint8_t sent;
} aodv_rrep_ack_wait_t;

typedef struct aodv_blacklist_entry {
    tavrn_direct_peer_t peer;
    uint32_t expires_at_ms;
    uint8_t occupied;
} aodv_blacklist_entry_t;

typedef struct aodv_deferred_rerr {
    uint16_t destination;
    uint16_t destination_sequence;
    uint8_t active;
} aodv_deferred_rerr_t;

typedef struct aodv_core_state {
    aodv_route_entry_t routes[TAVRN_AODV_ROUTE_CAPACITY];
    aodv_rreq_seen_entry_t rreq_seen[TAVRN_AODV_RREQ_SEEN_CAPACITY];
    aodv_rrep_seen_entry_t rrep_seen[AODV_RREP_SEEN_CAPACITY];
    aodv_rerr_seen_entry_t rerr_seen[AODV_RERR_SEEN_CAPACITY];
    aodv_discovery_entry_t discoveries[AODV_DISCOVERY_CAPACITY];
    aodv_pending_entry_t pending[TAVRN_AODV_PENDING_DATA_CAPACITY];
    aodv_rrep_ack_wait_t ack_waits[TAVRN_AODV_RREP_ACK_WAIT_CAPACITY];
    aodv_blacklist_entry_t blacklist[AODV_BLACKLIST_CAPACITY];
    aodv_action_t actions[TAVRN_AODV_ACTION_CAPACITY];
    aodv_deferred_rerr_t deferred;
    uint32_t rreq_window_started_ms;
    uint32_t rerr_window_started_ms;
    uint16_t next_request_id;
    uint16_t next_rerr_sequence;
    uint16_t next_action_token;
    uint16_t next_data_sequence;
    uint16_t local_origin_sequence;
    uint8_t rreq_window_count;
    uint8_t rerr_window_count;
    uint8_t action_head;
    uint8_t action_count;
} aodv_core_state_t;

typedef char aodv_core_private_storage_guard[
    (sizeof(aodv_core_state_t) <= TAVRN_AODV_CORE_PRIVATE_STORAGE_BYTES) ? 1 : -1];

static aodv_core_state_t *state_of(aodv_core_t *core)
{
    return (aodv_core_state_t *)(void *)core->private_state.bytes;
}

static const aodv_core_state_t *const_state_of(const aodv_core_t *core)
{
    return (const aodv_core_state_t *)(const void *)core->private_state.bytes;
}

static int time_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static int id_is_unicast(tavrn_identity_width_t width, uint16_t value)
{
    if (width == TAVRN_IDENTITY_SID8) {
        return value > 0u && value < 0xffu;
    }
    return width == TAVRN_IDENTITY_SID16 && value != 0u && value != 0xffffu;
}

static uint8_t width_bytes(tavrn_identity_width_t width)
{
    return width == TAVRN_IDENTITY_SID8 ? 1u : 2u;
}

static int local_width_is_valid(const aodv_core_t *core)
{
    return core->config.local_peer.logical_id.width == TAVRN_IDENTITY_SID8 ||
        core->config.local_peer.logical_id.width == TAVRN_IDENTITY_SID16;
}

static int peer_is_valid_for_core(const aodv_core_t *core,
                                  const tavrn_direct_peer_t *peer)
{
    return peer != NULL && local_width_is_valid(core) &&
        peer->logical_id.width == core->config.local_peer.logical_id.width &&
        id_is_unicast(peer->logical_id.width, peer->logical_id.value);
}

static int peer_equal(const tavrn_direct_peer_t *left,
                      const tavrn_direct_peer_t *right)
{
    return left->logical_id.width == right->logical_id.width &&
        left->logical_id.value == right->logical_id.value &&
        memcmp(left->adva.bytes, right->adva.bytes, TAVRN_ADVA_LEN) == 0;
}

static uint16_t pdu_u16(const uint8_t *pdu, uint8_t offset)
{
    return (uint16_t)((uint16_t)pdu[offset] |
                      ((uint16_t)pdu[(uint8_t)(offset + 1u)] << 8));
}

static void pdu_put_u16(uint8_t *pdu, uint8_t offset, uint16_t value)
{
    pdu[offset] = (uint8_t)value;
    pdu[(uint8_t)(offset + 1u)] = (uint8_t)(value >> 8);
}

static uint16_t pdu_id(const uint8_t *pdu, uint8_t offset,
                       tavrn_identity_width_t width)
{
    return width == TAVRN_IDENTITY_SID8 ? (uint16_t)pdu[offset] :
        pdu_u16(pdu, offset);
}

static void pdu_put_id(uint8_t *pdu, uint8_t offset,
                       tavrn_identity_width_t width, uint16_t value)
{
    pdu[offset] = (uint8_t)value;
    if (width == TAVRN_IDENTITY_SID16) {
        pdu[(uint8_t)(offset + 1u)] = (uint8_t)(value >> 8);
    }
}

static uint32_t lifetime_decode(uint16_t encoded)
{
    if ((encoded & 0x8000u) != 0u) {
        return (uint32_t)(encoded & 0x7fffu) * 100u;
    }
    return encoded;
}

static uint16_t lifetime_encode(uint32_t lifetime_ms)
{
    if (lifetime_ms <= 0x3fffu) {
        return (uint16_t)lifetime_ms;
    }
    lifetime_ms /= 100u;
    if (lifetime_ms > 0x7fffu) {
        lifetime_ms = 0x7fffu;
    }
    return (uint16_t)(0x8000u | lifetime_ms);
}

static void begin_control(tavrn_validated_control_t *control,
                          tavrn_wire_type_t type, uint8_t pdu_len,
                          const aodv_core_t *core)
{
    memset(control, 0, sizeof(*control));
    control->type = type;
    control->pdu_len = pdu_len;
    control->pdu[0] = 0x54u;
    control->pdu[1] = 0x52u;
    control->pdu[2] = 0x02u;
    control->pdu[3] = core->config.network_id;
    control->pdu[4] = (uint8_t)type;
    control->pdu[AODV_PDU_FLAGS_OFFSET] =
        core->config.local_peer.logical_id.width == TAVRN_IDENTITY_SID8 ?
        AODV_FLAG_IDENTITY_SID8 : 0u;
}

static int action_space(const aodv_core_state_t *state, uint8_t needed)
{
    return needed <= (uint8_t)(TAVRN_AODV_ACTION_CAPACITY - state->action_count);
}

static int enqueue_action(aodv_core_t *core, const aodv_action_t *action)
{
    aodv_core_state_t *state = state_of(core);
    uint8_t slot;

    if (!action_space(state, 1u)) {
        core->counters.action_backpressure++;
        return 0;
    }
    slot = (uint8_t)((state->action_head + state->action_count) %
                     TAVRN_AODV_ACTION_CAPACITY);
    state->actions[slot] = *action;
    state->action_count++;
    return 1;
}

static aodv_route_entry_t *route_find(aodv_core_t *core, uint16_t destination)
{
    aodv_core_state_t *state = state_of(core);
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        if (state->routes[i].occupied != 0u &&
            state->routes[i].destination == destination) {
            return &state->routes[i];
        }
    }
    return NULL;
}

static const aodv_route_entry_t *const_route_find(const aodv_core_t *core,
                                                   uint16_t destination)
{
    const aodv_core_state_t *state = const_state_of(core);
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        if (state->routes[i].occupied != 0u &&
            state->routes[i].destination == destination) {
            return &state->routes[i];
        }
    }
    return NULL;
}

static aodv_route_entry_t *route_slot(aodv_core_t *core, uint16_t destination)
{
    aodv_core_state_t *state = state_of(core);
    aodv_route_entry_t *invalid = NULL;
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        if (state->routes[i].occupied == 0u) {
            return &state->routes[i];
        }
        if (state->routes[i].state == AODV_ROUTE_INVALID && invalid == NULL) {
            invalid = &state->routes[i];
        }
    }
    if (invalid != NULL) {
        memset(invalid, 0, sizeof(*invalid));
        invalid->destination = destination;
    }
    return invalid;
}

static int route_has_slot(const aodv_core_t *core)
{
    const aodv_core_state_t *state = const_state_of(core);
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        if (state->routes[i].occupied == 0u ||
            state->routes[i].state == AODV_ROUTE_INVALID) {
            return 1;
        }
    }
    return 0;
}

static int route_is_valid_at(aodv_core_t *core, aodv_route_entry_t *route,
                             uint32_t now_ms)
{
    if (route == NULL || route->state != AODV_ROUTE_VALID) {
        return 0;
    }
    if (time_due(now_ms, route->expires_at_ms)) {
        route->state = AODV_ROUTE_INVALID;
        core->counters.route_expired++;
        return 0;
    }
    return 1;
}

static int route_install(aodv_core_t *core, uint16_t destination,
                         const tavrn_direct_peer_t *next_hop,
                         uint16_t destination_sequence, uint8_t sequence_valid,
                         uint8_t hop_count, uint32_t expires_at_ms)
{
    aodv_route_entry_t *route = route_find(core, destination);
    int replace = 0;

    if (route == NULL) {
        route = route_slot(core, destination);
        if (route == NULL) {
            core->counters.route_capacity_rejected++;
            return 0;
        }
        memset(route, 0, sizeof(*route));
        route->occupied = 1u;
        route->destination = destination;
        replace = 1;
    } else if (route->state == AODV_ROUTE_INVALID) {
        if (sequence_valid != 0u &&
            (route->sequence_valid == 0u ||
             destination_sequence == route->destination_sequence ||
             aodv_serial_is_newer(destination_sequence,
                                  route->destination_sequence))) {
            replace = 1;
        } else if (sequence_valid == 0u && route->sequence_valid == 0u) {
            replace = 1;
        }
    } else if (sequence_valid != 0u && route->sequence_valid == 0u) {
        replace = 1;
    } else if (sequence_valid != 0u && route->sequence_valid != 0u &&
               aodv_serial_is_newer(destination_sequence,
                                    route->destination_sequence)) {
        replace = 1;
    } else if (sequence_valid != 0u && route->sequence_valid != 0u &&
               destination_sequence == route->destination_sequence &&
               hop_count < route->hop_count) {
        replace = 1;
    }

    if (replace == 0) {
        return 1;
    }
    route->next_hop = *next_hop;
    route->destination_sequence = destination_sequence;
    route->sequence_valid = sequence_valid;
    route->hop_count = hop_count;
    route->expires_at_ms = expires_at_ms;
    route->state = AODV_ROUTE_VALID;
    return 1;
}

static void route_add_precursor(aodv_core_t *core, aodv_route_entry_t *route,
                                uint16_t precursor)
{
    uint8_t i;

    if (route == NULL || !id_is_unicast(core->config.local_peer.logical_id.width,
                                        precursor)) {
        return;
    }
    for (i = 0u; i < route->precursor_count; i++) {
        if (route->precursors[i] == precursor) {
            return;
        }
    }
    if (route->precursor_count == TAVRN_AODV_PRECURSOR_CAPACITY) {
        route->precursor_overflow = 1u;
        core->counters.precursor_overflow++;
        return;
    }
    route->precursors[route->precursor_count++] = precursor;
}

static aodv_discovery_entry_t *discovery_find(aodv_core_t *core,
                                               uint16_t destination)
{
    aodv_core_state_t *state = state_of(core);
    uint8_t i;

    for (i = 0u; i < AODV_DISCOVERY_CAPACITY; i++) {
        if (state->discoveries[i].occupied != 0u &&
            state->discoveries[i].destination == destination) {
            return &state->discoveries[i];
        }
    }
    return NULL;
}

static aodv_discovery_entry_t *discovery_slot(aodv_core_t *core)
{
    aodv_core_state_t *state = state_of(core);
    uint8_t i;

    for (i = 0u; i < AODV_DISCOVERY_CAPACITY; i++) {
        if (state->discoveries[i].occupied == 0u) {
            return &state->discoveries[i];
        }
    }
    return NULL;
}

static int discovery_has_request(const aodv_discovery_entry_t *discovery,
                                 uint16_t request_id)
{
    uint8_t i;

    for (i = 0u; i < discovery->request_count; i++) {
        if (discovery->request_ids[i] == request_id) {
            return 1;
        }
    }
    return 0;
}

static int has_pending_destination(const aodv_core_t *core, uint16_t destination)
{
    const aodv_core_state_t *state = const_state_of(core);
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_PENDING_DATA_CAPACITY; i++) {
        if (state->pending[i].occupied != 0u &&
            state->pending[i].data.final_destination.value == destination) {
            return 1;
        }
    }
    return 0;
}

static int rreq_rate_allowed(aodv_core_t *core, uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);

    if ((uint32_t)(now_ms - state->rreq_window_started_ms) >= 1000u) {
        state->rreq_window_started_ms = now_ms;
        state->rreq_window_count = 0u;
    }
    if (state->rreq_window_count >= core->config.rreq_rate_per_second) {
        core->counters.rreq_rate_limited++;
        return 0;
    }
    state->rreq_window_count++;
    return 1;
}

static int rerr_rate_allowed(aodv_core_t *core, uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);

    if ((uint32_t)(now_ms - state->rerr_window_started_ms) >= 1000u) {
        state->rerr_window_started_ms = now_ms;
        state->rerr_window_count = 0u;
    }
    if (state->rerr_window_count >= core->config.rerr_rate_per_second) {
        core->counters.rerr_rate_limited++;
        return 0;
    }
    state->rerr_window_count++;
    return 1;
}

static uint16_t next_nonzero(uint16_t *value)
{
    uint16_t result = *value;

    (*value)++;
    if (*value == 0u) {
        *value = 1u;
    }
    if (result == 0u) {
        result = *value;
        (*value)++;
        if (*value == 0u) {
            *value = 1u;
        }
    }
    return result;
}

static uint8_t ring_ttl(const aodv_core_t *core, uint8_t ring)
{
    static const uint8_t rings[AODV_RREQ_RING_COUNT] = {
        1u, 3u, 5u, 7u, 0u,
    };

    if (ring < AODV_RREQ_RING_COUNT && rings[ring] != 0u) {
        return rings[ring];
    }
    return core->config.net_diameter;
}

static int emit_rreq(aodv_core_t *core, aodv_discovery_entry_t *discovery,
                     uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);
    aodv_action_t action;
    tavrn_validated_control_t *control;
    tavrn_identity_width_t width = core->config.local_peer.logical_id.width;
    uint8_t width_len = width_bytes(width);
    uint8_t destination_offset = (uint8_t)(9u + width_len);
    uint8_t destination_sequence_offset = (uint8_t)(9u + 2u * width_len);
    uint8_t origin_sequence_offset = (uint8_t)(11u + 2u * width_len);
    aodv_route_entry_t *route;
    uint16_t request_id;
    uint8_t ttl;

    if (discovery->next_ring >=
        (uint8_t)(AODV_RREQ_RING_COUNT + core->config.rreq_retries) ||
        discovery->request_count == AODV_DISCOVERY_REQUEST_CAPACITY ||
        !action_space(state, 1u) || !rreq_rate_allowed(core, now_ms)) {
        return 0;
    }
    memset(&action, 0, sizeof(action));
    action.type = AODV_ACTION_SEND_RREQ;
    action.detail.control.controlled_flood = 1u;
    control = &action.detail.control.control;
    begin_control(control, TAVRN_WIRE_E_RREQ,
                  (uint8_t)(13u + 2u * width_len), core);
    ttl = ring_ttl(core, discovery->next_ring);
    control->pdu[AODV_PDU_TTL_HOPS_OFFSET] = (uint8_t)(ttl << 4);
    pdu_put_id(control->pdu, 7u, width, core->config.local_peer.logical_id.value);
    request_id = next_nonzero(&state->next_request_id);
    pdu_put_u16(control->pdu, (uint8_t)(7u + width_len), request_id);
    pdu_put_id(control->pdu, destination_offset, width, discovery->destination);
    route = route_find(core, discovery->destination);
    if (route != NULL && route->sequence_valid != 0u) {
        pdu_put_u16(control->pdu, destination_sequence_offset,
                    route->destination_sequence);
    } else {
        control->pdu[AODV_PDU_FLAGS_OFFSET] |= AODV_FLAG_RREQ_DEST_UNKNOWN;
        pdu_put_u16(control->pdu, destination_sequence_offset, 0u);
    }
    state->local_origin_sequence++;
    pdu_put_u16(control->pdu, origin_sequence_offset,
                state->local_origin_sequence);
    if (!enqueue_action(core, &action)) {
        return 0;
    }
    discovery->request_ids[discovery->request_count++] = request_id;
    discovery->next_ring++;
    discovery->next_attempt_ms = now_ms + core->config.path_discovery_ms;
    return 1;
}

static int queue_pending(aodv_core_t *core, const tavrn_link_data_t *data,
                         uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_PENDING_DATA_CAPACITY; i++) {
        if (state->pending[i].occupied == 0u) {
            state->pending[i].occupied = 1u;
            state->pending[i].data = *data;
            state->pending[i].expires_at_ms = now_ms + core->config.pending_data_ms;
            return 1;
        }
    }
    core->counters.pending_busy++;
    return 0;
}

static int rreq_seen_contains(aodv_core_t *core, uint16_t origin,
                              uint16_t request_id, uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_RREQ_SEEN_CAPACITY; i++) {
        if (state->rreq_seen[i].occupied != 0u &&
            time_due(now_ms, state->rreq_seen[i].expires_at_ms)) {
            state->rreq_seen[i].occupied = 0u;
        }
        if (state->rreq_seen[i].occupied != 0u && state->rreq_seen[i].origin == origin &&
            state->rreq_seen[i].request_id == request_id) {
            return 1;
        }
    }
    return 0;
}

static void rreq_seen_add(aodv_core_t *core, uint16_t origin, uint16_t request_id,
                          uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);
    aodv_rreq_seen_entry_t *victim = &state->rreq_seen[0];
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_RREQ_SEEN_CAPACITY; i++) {
        if (state->rreq_seen[i].occupied == 0u) {
            victim = &state->rreq_seen[i];
            break;
        }
        if (time_due(victim->expires_at_ms, state->rreq_seen[i].expires_at_ms)) {
            victim = &state->rreq_seen[i];
        }
    }
    victim->occupied = 1u;
    victim->origin = origin;
    victim->request_id = request_id;
    victim->expires_at_ms = now_ms + core->config.rreq_seen_ms;
}

static int rrep_seen_contains(aodv_core_t *core, uint16_t destination,
                              uint16_t destination_sequence, uint16_t origin,
                              uint16_t request_id, uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);
    uint8_t i;

    for (i = 0u; i < AODV_RREP_SEEN_CAPACITY; i++) {
        if (state->rrep_seen[i].occupied != 0u &&
            time_due(now_ms, state->rrep_seen[i].expires_at_ms)) {
            state->rrep_seen[i].occupied = 0u;
        }
        if (state->rrep_seen[i].occupied != 0u &&
            state->rrep_seen[i].destination == destination &&
            state->rrep_seen[i].destination_sequence == destination_sequence &&
            state->rrep_seen[i].origin == origin &&
            state->rrep_seen[i].request_id == request_id) {
            return 1;
        }
    }
    return 0;
}

static void rrep_seen_add(aodv_core_t *core, uint16_t destination,
                          uint16_t destination_sequence, uint16_t origin,
                          uint16_t request_id, uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);
    aodv_rrep_seen_entry_t *victim = &state->rrep_seen[0];
    uint8_t i;

    for (i = 0u; i < AODV_RREP_SEEN_CAPACITY; i++) {
        if (state->rrep_seen[i].occupied == 0u) {
            victim = &state->rrep_seen[i];
            break;
        }
        if (time_due(victim->expires_at_ms, state->rrep_seen[i].expires_at_ms)) {
            victim = &state->rrep_seen[i];
        }
    }
    victim->occupied = 1u;
    victim->destination = destination;
    victim->destination_sequence = destination_sequence;
    victim->origin = origin;
    victim->request_id = request_id;
    victim->expires_at_ms = now_ms + core->config.rrep_dedupe_ms;
}

static int rerr_seen_contains(aodv_core_t *core, uint16_t reporter,
                              uint16_t sequence, uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);
    uint8_t i;

    for (i = 0u; i < AODV_RERR_SEEN_CAPACITY; i++) {
        if (state->rerr_seen[i].occupied != 0u &&
            time_due(now_ms, state->rerr_seen[i].expires_at_ms)) {
            state->rerr_seen[i].occupied = 0u;
        }
        if (state->rerr_seen[i].occupied != 0u &&
            state->rerr_seen[i].reporter == reporter &&
            state->rerr_seen[i].sequence == sequence) {
            return 1;
        }
    }
    return 0;
}

static void rerr_seen_add(aodv_core_t *core, uint16_t reporter, uint16_t sequence,
                          uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);
    aodv_rerr_seen_entry_t *victim = &state->rerr_seen[0];
    uint8_t i;

    for (i = 0u; i < AODV_RERR_SEEN_CAPACITY; i++) {
        if (state->rerr_seen[i].occupied == 0u) {
            victim = &state->rerr_seen[i];
            break;
        }
        if (time_due(victim->expires_at_ms, state->rerr_seen[i].expires_at_ms)) {
            victim = &state->rerr_seen[i];
        }
    }
    victim->occupied = 1u;
    victim->reporter = reporter;
    victim->sequence = sequence;
    victim->expires_at_ms = now_ms + core->config.rerr_dedupe_ms;
}

static aodv_rrep_ack_wait_t *ack_wait_reserve(aodv_core_t *core,
                                               const tavrn_direct_peer_t *peer,
                                               uint16_t destination,
                                               uint16_t destination_sequence,
                                               uint16_t origin,
                                               uint16_t request_id)
{
    aodv_core_state_t *state = state_of(core);
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_RREP_ACK_WAIT_CAPACITY; i++) {
        if (state->ack_waits[i].occupied == 0u) {
            aodv_rrep_ack_wait_t *wait = &state->ack_waits[i];

            memset(wait, 0, sizeof(*wait));
            wait->occupied = 1u;
            wait->expected_peer = *peer;
            wait->destination = destination;
            wait->destination_sequence = destination_sequence;
            wait->origin = origin;
            wait->request_id = request_id;
            wait->token = next_nonzero(&state->next_action_token);
            return wait;
        }
    }
    return NULL;
}

static int ack_wait_has_space(const aodv_core_t *core)
{
    const aodv_core_state_t *state = const_state_of(core);
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_RREP_ACK_WAIT_CAPACITY; i++) {
        if (state->ack_waits[i].occupied == 0u) {
            return 1;
        }
    }
    return 0;
}

static void ack_wait_cancel(aodv_rrep_ack_wait_t *wait)
{
    memset(wait, 0, sizeof(*wait));
}

static int enqueue_rrep_ack(aodv_core_t *core,
                            const aodv_control_input_t *input,
                            uint16_t destination,
                            uint16_t destination_sequence,
                            uint16_t origin, uint16_t request_id)
{
    aodv_action_t action;
    tavrn_validated_control_t *control;
    tavrn_identity_width_t width = core->config.local_peer.logical_id.width;
    uint8_t width_len = width_bytes(width);

    memset(&action, 0, sizeof(action));
    action.type = AODV_ACTION_SEND_RREP_ACK;
    action.detail.control.next_hop = input->transmitter;
    control = &action.detail.control.control;
    begin_control(control, TAVRN_WIRE_E_RREP_ACK,
                  (uint8_t)(10u + 3u * width_len), core);
    pdu_put_id(control->pdu, 6u, width, input->transmitter.logical_id.value);
    pdu_put_id(control->pdu, (uint8_t)(6u + width_len), width, destination);
    pdu_put_u16(control->pdu, (uint8_t)(6u + 2u * width_len),
                destination_sequence);
    pdu_put_id(control->pdu, (uint8_t)(8u + 2u * width_len), width, origin);
    pdu_put_u16(control->pdu, (uint8_t)(8u + 3u * width_len), request_id);
    return enqueue_action(core, &action);
}

static int enqueue_rrep(aodv_core_t *core, aodv_action_type_t type,
                        const tavrn_direct_peer_t *next_hop,
                        const tavrn_validated_control_t *control,
                        uint16_t destination, uint16_t destination_sequence,
                        uint16_t origin, uint16_t request_id)
{
    aodv_action_t action;
    aodv_rrep_ack_wait_t *wait = NULL;

    if (!action_space(state_of(core), 1u)) {
        core->counters.action_backpressure++;
        return 0;
    }
    if (core->config.request_rrep_ack != 0u) {
        wait = ack_wait_reserve(core, next_hop, destination, destination_sequence,
                                origin, request_id);
        if (wait == NULL) {
            core->counters.action_backpressure++;
            return 0;
        }
    }
    memset(&action, 0, sizeof(action));
    action.type = type;
    action.detail.control.next_hop = *next_hop;
    action.detail.control.control = *control;
    if (wait != NULL) {
        action.detail.control.token = wait->token;
        action.detail.control.control.pdu[AODV_PDU_FLAGS_OFFSET] |=
            AODV_FLAG_RREP_ACK_REQUIRED;
    } else {
        action.detail.control.control.pdu[AODV_PDU_FLAGS_OFFSET] &=
            (uint8_t)~AODV_FLAG_RREP_ACK_REQUIRED;
    }
    if (!enqueue_action(core, &action)) {
        if (wait != NULL) {
            ack_wait_cancel(wait);
        }
        return 0;
    }
    return 1;
}

static int release_pending_on_route(aodv_core_t *core, aodv_route_entry_t *route,
                                    uint16_t destination)
{
    aodv_core_state_t *state = state_of(core);
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_PENDING_DATA_CAPACITY; i++) {
        aodv_action_t action;

        if (state->pending[i].occupied == 0u ||
            state->pending[i].data.final_destination.value != destination) {
            continue;
        }
        if (!action_space(state, 1u)) {
            core->counters.action_backpressure++;
            return 0;
        }
        memset(&action, 0, sizeof(action));
        action.type = AODV_ACTION_FORWARD_DATA;
        action.detail.data.next_hop = route->next_hop;
        action.detail.data.data = state->pending[i].data;
        if (!enqueue_action(core, &action)) {
            return 0;
        }
        state->pending[i].occupied = 0u;
    }
    if (!has_pending_destination(core, destination)) {
        aodv_discovery_entry_t *discovery = discovery_find(core, destination);

        if (discovery != NULL) {
            discovery->occupied = 0u;
        }
    }
    return 1;
}

static int release_pending_destination(aodv_core_t *core, uint16_t destination,
                                       uint32_t now_ms)
{
    aodv_route_entry_t *route = route_find(core, destination);

    if (!route_is_valid_at(core, route, now_ms)) {
        return 0;
    }
    return release_pending_on_route(core, route, destination);
}

static void drain_pending_routes(aodv_core_t *core)
{
    aodv_core_state_t *state = state_of(core);
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        aodv_route_entry_t *route = &state->routes[i];

        if (route->occupied != 0u && route->state == AODV_ROUTE_VALID &&
            has_pending_destination(core, route->destination)) {
            (void)release_pending_on_route(core, route, route->destination);
        }
    }
}

static void blacklist_peer(aodv_core_t *core, const tavrn_direct_peer_t *peer,
                           uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);
    aodv_blacklist_entry_t *slot = &state->blacklist[0];
    uint8_t i;

    for (i = 0u; i < AODV_BLACKLIST_CAPACITY; i++) {
        if (state->blacklist[i].occupied == 0u ||
            peer_equal(&state->blacklist[i].peer, peer)) {
            slot = &state->blacklist[i];
            break;
        }
        if (time_due(state->blacklist[i].expires_at_ms, slot->expires_at_ms)) {
            slot = &state->blacklist[i];
        }
    }
    slot->occupied = 1u;
    slot->peer = *peer;
    slot->expires_at_ms = now_ms + core->config.blacklist_ms;
}

static int peer_is_blacklisted(aodv_core_t *core,
                               const tavrn_direct_peer_t *peer,
                               uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);
    uint8_t i;

    for (i = 0u; i < AODV_BLACKLIST_CAPACITY; i++) {
        if (state->blacklist[i].occupied != 0u &&
            time_due(now_ms, state->blacklist[i].expires_at_ms)) {
            state->blacklist[i].occupied = 0u;
        }
        if (state->blacklist[i].occupied != 0u &&
            peer_equal(&state->blacklist[i].peer, peer)) {
            return 1;
        }
    }
    return 0;
}

static void expire_ack_wait(aodv_core_t *core, aodv_rrep_ack_wait_t *wait,
                            uint32_t now_ms)
{
    aodv_action_t action;

    if (!action_space(state_of(core), 1u)) {
        core->counters.action_backpressure++;
        return;
    }
    memset(&action, 0, sizeof(action));
    action.type = AODV_ACTION_BLACKLIST_NEIGHBOR;
    action.detail.failure.destination.width = core->config.local_peer.logical_id.width;
    action.detail.failure.destination.value = wait->destination;
    action.detail.failure.peer = wait->expected_peer;
    blacklist_peer(core, &wait->expected_peer, now_ms);
    ack_wait_cancel(wait);
    core->counters.rrep_ack_timeout++;
    (void)enqueue_action(core, &action);
}

static int control_is_valid_for_core(const aodv_core_t *core,
                                     const tavrn_validated_control_t *control,
                                     tavrn_wire_type_t expected,
                                     uint8_t minimum_length)
{
    tavrn_identity_width_t width;

    if (control == NULL || control->type != expected || control->pdu_len < minimum_length ||
        control->pdu[0] != 0x54u || control->pdu[1] != 0x52u ||
        control->pdu[2] != 0x02u || control->pdu[3] != core->config.network_id ||
        control->pdu[4] != (uint8_t)expected) {
        return 0;
    }
    width = (control->pdu[AODV_PDU_FLAGS_OFFSET] & AODV_FLAG_IDENTITY_SID8) != 0u ?
        TAVRN_IDENTITY_SID8 : TAVRN_IDENTITY_SID16;
    return width == core->config.local_peer.logical_id.width;
}

static aodv_status_t ingest_rreq(aodv_core_t *core,
                                 const aodv_control_input_t *input,
                                 uint32_t now_ms)
{
    const tavrn_validated_control_t *control = &input->control;
    tavrn_identity_width_t width = core->config.local_peer.logical_id.width;
    uint8_t width_len = width_bytes(width);
    uint8_t destination_offset = (uint8_t)(9u + width_len);
    uint8_t destination_sequence_offset = (uint8_t)(9u + 2u * width_len);
    uint8_t origin_sequence_offset = (uint8_t)(11u + 2u * width_len);
    uint16_t origin;
    uint16_t request_id;
    uint16_t destination;
    uint16_t destination_sequence;
    uint16_t origin_sequence;
    uint8_t ttl;
    uint8_t hops;
    aodv_route_entry_t *reverse;

    if (!control_is_valid_for_core(core, control, TAVRN_WIRE_E_RREQ,
                                   (uint8_t)(13u + 2u * width_len)) ||
        !peer_is_valid_for_core(core, &input->transmitter)) {
        return AODV_STATUS_INVALID;
    }
    origin = pdu_id(control->pdu, 7u, width);
    request_id = pdu_u16(control->pdu, (uint8_t)(7u + width_len));
    destination = pdu_id(control->pdu, destination_offset, width);
    destination_sequence = pdu_u16(control->pdu, destination_sequence_offset);
    origin_sequence = pdu_u16(control->pdu, origin_sequence_offset);
    ttl = (uint8_t)(control->pdu[AODV_PDU_TTL_HOPS_OFFSET] >> 4);
    hops = (uint8_t)(control->pdu[AODV_PDU_TTL_HOPS_OFFSET] & 0x0fu);
    if (!id_is_unicast(width, origin) || !id_is_unicast(width, destination) ||
        ((control->pdu[AODV_PDU_FLAGS_OFFSET] & AODV_FLAG_RREQ_DEST_UNKNOWN) != 0u &&
         destination_sequence != 0u)) {
        return AODV_STATUS_INVALID;
    }
    if (peer_equal(&input->transmitter, &core->config.local_peer) ||
        origin == core->config.local_peer.logical_id.value) {
        core->counters.rreq_loop_prevented++;
        return AODV_STATUS_LOOP_PREVENTED;
    }
    if (peer_is_blacklisted(core, &input->transmitter, now_ms)) {
        return AODV_STATUS_BUSY;
    }
    if (rreq_seen_contains(core, origin, request_id, now_ms)) {
        core->counters.rreq_duplicate++;
        return AODV_STATUS_DUPLICATE;
    }
    if ((destination == core->config.local_peer.logical_id.value &&
         (!action_space(state_of(core), 1u) ||
          (core->config.request_rrep_ack != 0u && !ack_wait_has_space(core)))) ||
        (destination != core->config.local_peer.logical_id.value && ttl != 0u &&
         !action_space(state_of(core), 1u))) {
        core->counters.action_backpressure++;
        return AODV_STATUS_BUSY;
    }
    if (!route_install(core, origin, &input->transmitter, origin_sequence, 1u,
                       (uint8_t)(hops + 1u),
                       now_ms + core->config.active_route_ms)) {
        return AODV_STATUS_BUSY;
    }
    rreq_seen_add(core, origin, request_id, now_ms);
    reverse = route_find(core, origin);

    if (destination == core->config.local_peer.logical_id.value) {
        tavrn_validated_control_t reply;

        if ((control->pdu[AODV_PDU_FLAGS_OFFSET] & AODV_FLAG_RREQ_DEST_UNKNOWN) == 0u &&
            aodv_serial_is_newer(destination_sequence,
                                 state_of(core)->local_origin_sequence)) {
            state_of(core)->local_origin_sequence = destination_sequence;
        }

        begin_control(&reply, TAVRN_WIRE_E_RREP,
                      (uint8_t)(13u + 3u * width_len), core);
        reply.pdu[AODV_PDU_TTL_HOPS_OFFSET] =
            (uint8_t)(core->config.net_diameter << 4);
        pdu_put_id(reply.pdu, 7u, width, input->transmitter.logical_id.value);
        pdu_put_id(reply.pdu, (uint8_t)(7u + width_len), width,
                   core->config.local_peer.logical_id.value);
        pdu_put_u16(reply.pdu, (uint8_t)(7u + 2u * width_len),
                    state_of(core)->local_origin_sequence);
        pdu_put_id(reply.pdu, (uint8_t)(9u + 2u * width_len), width, origin);
        pdu_put_u16(reply.pdu, (uint8_t)(9u + 3u * width_len), request_id);
        pdu_put_u16(reply.pdu, (uint8_t)(11u + 3u * width_len),
                    lifetime_encode(core->config.active_route_ms));
        if (!enqueue_rrep(core, AODV_ACTION_SEND_RREP, &input->transmitter,
                          &reply, destination, state_of(core)->local_origin_sequence,
                          origin, request_id)) {
            return AODV_STATUS_BUSY;
        }
        return AODV_STATUS_OK;
    }

    if (ttl != 0u && reverse != NULL) {
        aodv_action_t action;

        if (!action_space(state_of(core), 1u)) {
            core->counters.action_backpressure++;
            return AODV_STATUS_BUSY;
        }
        memset(&action, 0, sizeof(action));
        action.type = AODV_ACTION_SEND_RREQ;
        action.detail.control.controlled_flood = 1u;
        action.detail.control.control = *control;
        action.detail.control.control.pdu[AODV_PDU_TTL_HOPS_OFFSET] =
            (uint8_t)(((ttl - 1u) << 4) | ((hops + 1u) & 0x0fu));
        (void)enqueue_action(core, &action);
    }
    return AODV_STATUS_OK;
}

static aodv_status_t ingest_rrep(aodv_core_t *core,
                                 const aodv_control_input_t *input,
                                 uint32_t now_ms)
{
    const tavrn_validated_control_t *control = &input->control;
    tavrn_identity_width_t width = core->config.local_peer.logical_id.width;
    uint8_t width_len = width_bytes(width);
    uint8_t destination_offset = (uint8_t)(7u + width_len);
    uint8_t destination_sequence_offset = (uint8_t)(7u + 2u * width_len);
    uint8_t origin_offset = (uint8_t)(9u + 2u * width_len);
    uint8_t request_offset = (uint8_t)(9u + 3u * width_len);
    uint8_t lifetime_offset = (uint8_t)(11u + 3u * width_len);
    uint16_t immediate_receiver;
    uint16_t destination;
    uint16_t destination_sequence;
    uint16_t origin;
    uint16_t request_id;
    uint32_t lifetime;
    uint8_t ttl;
    uint8_t hops;
    uint8_t required_actions;
    int ack_required;
    aodv_route_entry_t *reverse;
    aodv_discovery_entry_t *discovery;

    if (!control_is_valid_for_core(core, control, TAVRN_WIRE_E_RREP,
                                   (uint8_t)(13u + 3u * width_len)) ||
        !peer_is_valid_for_core(core, &input->transmitter)) {
        return AODV_STATUS_INVALID;
    }
    immediate_receiver = pdu_id(control->pdu, 7u, width);
    destination = pdu_id(control->pdu, destination_offset, width);
    destination_sequence = pdu_u16(control->pdu, destination_sequence_offset);
    origin = pdu_id(control->pdu, origin_offset, width);
    request_id = pdu_u16(control->pdu, request_offset);
    lifetime = lifetime_decode(pdu_u16(control->pdu, lifetime_offset));
    ttl = (uint8_t)(control->pdu[AODV_PDU_TTL_HOPS_OFFSET] >> 4);
    hops = (uint8_t)(control->pdu[AODV_PDU_TTL_HOPS_OFFSET] & 0x0fu);
    ack_required = (control->pdu[AODV_PDU_FLAGS_OFFSET] &
                    AODV_FLAG_RREP_ACK_REQUIRED) != 0u;
    if (immediate_receiver != core->config.local_peer.logical_id.value ||
        !id_is_unicast(width, destination) || !id_is_unicast(width, origin) ||
        lifetime == 0u) {
        return AODV_STATUS_INVALID;
    }
    if (peer_is_blacklisted(core, &input->transmitter, now_ms)) {
        return AODV_STATUS_BUSY;
    }
    discovery = discovery_find(core, destination);
    reverse = route_find(core, origin);
    if (origin == core->config.local_peer.logical_id.value) {
        if (discovery == NULL || !discovery_has_request(discovery, request_id)) {
            return AODV_STATUS_UNMATCHED;
        }
    } else if (!route_is_valid_at(core, reverse, now_ms) || ttl == 0u) {
        return AODV_STATUS_UNMATCHED;
    }
    if (rrep_seen_contains(core, destination, destination_sequence, origin,
                           request_id, now_ms)) {
        return AODV_STATUS_DUPLICATE;
    }
    required_actions = ack_required != 0 ? 1u : 0u;
    if (origin != core->config.local_peer.logical_id.value) {
        if (peer_is_blacklisted(core, &reverse->next_hop, now_ms)) {
            return AODV_STATUS_BUSY;
        }
        required_actions++;
        if (core->config.request_rrep_ack != 0u && !ack_wait_has_space(core)) {
            core->counters.action_backpressure++;
            return AODV_STATUS_BUSY;
        }
    }
    if (!action_space(state_of(core), required_actions)) {
        core->counters.action_backpressure++;
        return AODV_STATUS_BUSY;
    }
    if (!route_install(core, destination, &input->transmitter, destination_sequence,
                       1u, (uint8_t)(hops + 1u), now_ms + lifetime)) {
        return AODV_STATUS_BUSY;
    }
    rrep_seen_add(core, destination, destination_sequence, origin, request_id, now_ms);
    if (ack_required != 0 && !enqueue_rrep_ack(core, input, destination,
                                                destination_sequence, origin,
                                                request_id)) {
        return AODV_STATUS_BUSY;
    }
    if (origin == core->config.local_peer.logical_id.value) {
        if (release_pending_destination(core, destination, now_ms) != 0 &&
            !has_pending_destination(core, destination)) {
            discovery->occupied = 0u;
        }
        return AODV_STATUS_OK;
    }
    {
        tavrn_validated_control_t forwarded = *control;

        forwarded.pdu[AODV_PDU_TTL_HOPS_OFFSET] =
            (uint8_t)(((ttl - 1u) << 4) | ((hops + 1u) & 0x0fu));
        pdu_put_id(forwarded.pdu, 7u, width, reverse->next_hop.logical_id.value);
        if (!enqueue_rrep(core, AODV_ACTION_FORWARD_RREP, &reverse->next_hop,
                          &forwarded, destination, destination_sequence, origin,
                          request_id)) {
            return AODV_STATUS_BUSY;
        }
    }
    return AODV_STATUS_OK;
}

static aodv_status_t ingest_rrep_ack(aodv_core_t *core,
                                     const aodv_control_input_t *input,
                                     uint32_t now_ms)
{
    const tavrn_validated_control_t *control = &input->control;
    aodv_core_state_t *state = state_of(core);
    tavrn_identity_width_t width = core->config.local_peer.logical_id.width;
    uint8_t width_len = width_bytes(width);
    uint16_t receiver;
    uint16_t destination;
    uint16_t destination_sequence;
    uint16_t origin;
    uint16_t request_id;
    uint8_t i;

    if (!control_is_valid_for_core(core, control, TAVRN_WIRE_E_RREP_ACK,
                                   (uint8_t)(10u + 3u * width_len)) ||
        !peer_is_valid_for_core(core, &input->transmitter)) {
        return AODV_STATUS_INVALID;
    }
    receiver = pdu_id(control->pdu, 6u, width);
    destination = pdu_id(control->pdu, (uint8_t)(6u + width_len), width);
    destination_sequence = pdu_u16(control->pdu, (uint8_t)(6u + 2u * width_len));
    origin = pdu_id(control->pdu, (uint8_t)(8u + 2u * width_len), width);
    request_id = pdu_u16(control->pdu, (uint8_t)(8u + 3u * width_len));
    if (receiver != core->config.local_peer.logical_id.value) {
        return AODV_STATUS_UNMATCHED;
    }
    for (i = 0u; i < TAVRN_AODV_RREP_ACK_WAIT_CAPACITY; i++) {
        aodv_rrep_ack_wait_t *wait = &state->ack_waits[i];

        if (wait->occupied != 0u && wait->sent != 0u &&
            peer_equal(&wait->expected_peer, &input->transmitter) &&
            wait->destination == destination &&
            wait->destination_sequence == destination_sequence &&
            wait->origin == origin && wait->request_id == request_id) {
            if (time_due(now_ms, wait->expires_at_ms)) {
                expire_ack_wait(core, wait, now_ms);
                core->counters.rrep_ack_unmatched++;
                return AODV_STATUS_UNMATCHED;
            }
            ack_wait_cancel(wait);
            return AODV_STATUS_OK;
        }
    }
    core->counters.rrep_ack_unmatched++;
    return AODV_STATUS_UNMATCHED;
}

static aodv_status_t ingest_rerr(aodv_core_t *core,
                                 const aodv_control_input_t *input,
                                 uint32_t now_ms)
{
    const tavrn_validated_control_t *control = &input->control;
    tavrn_identity_width_t width = core->config.local_peer.logical_id.width;
    uint8_t width_len = width_bytes(width);
    uint8_t entries_offset = (uint8_t)(10u + width_len);
    uint16_t reporter;
    uint16_t sequence;
    uint8_t count;
    uint8_t ttl;
    uint8_t hops;
    uint8_t i;

    if (!control_is_valid_for_core(core, control, TAVRN_WIRE_E_RERR,
                                   (uint8_t)(10u + width_len)) ||
        !peer_is_valid_for_core(core, &input->transmitter)) {
        return AODV_STATUS_INVALID;
    }
    reporter = pdu_id(control->pdu, 7u, width);
    sequence = pdu_u16(control->pdu, (uint8_t)(7u + width_len));
    count = control->pdu[(uint8_t)(9u + width_len)];
    ttl = (uint8_t)(control->pdu[AODV_PDU_TTL_HOPS_OFFSET] >> 4);
    hops = (uint8_t)(control->pdu[AODV_PDU_TTL_HOPS_OFFSET] & 0x0fu);
    if (!id_is_unicast(width, reporter) || count == 0u ||
        count > (width == TAVRN_IDENTITY_SID16 ? TAVRN_AODV_RERR_SID16_PER_ACTION :
                 TAVRN_AODV_RERR_SID8_PER_ACTION) ||
        control->pdu_len != (uint8_t)(entries_offset + count * (width_len + 2u))) {
        return AODV_STATUS_INVALID;
    }
    if (rerr_seen_contains(core, reporter, sequence, now_ms)) {
        return AODV_STATUS_DUPLICATE;
    }
    if (ttl != 0u && !action_space(state_of(core), 1u)) {
        core->counters.action_backpressure++;
        return AODV_STATUS_BUSY;
    }
    rerr_seen_add(core, reporter, sequence, now_ms);
    for (i = 0u; i < count; i++) {
        uint8_t offset = (uint8_t)(entries_offset + i * (width_len + 2u));
        uint16_t destination = pdu_id(control->pdu, offset, width);
        uint16_t destination_sequence = pdu_u16(control->pdu,
                                                  (uint8_t)(offset + width_len));
        aodv_route_entry_t *route = route_find(core, destination);

        if (route != NULL && route->state == AODV_ROUTE_VALID &&
            peer_equal(&route->next_hop, &input->transmitter) &&
            (!route->sequence_valid || route->destination_sequence == destination_sequence ||
             aodv_serial_is_newer(destination_sequence,
                                  route->destination_sequence))) {
            route->state = AODV_ROUTE_INVALID;
            route->destination_sequence = destination_sequence;
            route->sequence_valid = 1u;
        }
    }
    if (ttl != 0u) {
        aodv_action_t action;

        memset(&action, 0, sizeof(action));
        action.type = AODV_ACTION_SEND_RERR;
        action.detail.control.controlled_flood = 1u;
        action.detail.control.control = *control;
        action.detail.control.control.pdu[AODV_PDU_TTL_HOPS_OFFSET] =
            (uint8_t)(((ttl - 1u) << 4) | ((hops + 1u) & 0x0fu));
        (void)enqueue_action(core, &action);
    }
    return AODV_STATUS_OK;
}

int aodv_serial_is_newer(uint16_t candidate, uint16_t current)
{
    uint16_t difference = (uint16_t)(candidate - current);

    return difference != 0u && difference < 0x8000u;
}

static int config_is_valid(const aodv_core_config_t *config)
{
    const uint32_t durations[] = {
        config->node_traversal_ms, config->path_discovery_ms, config->rreq_seen_ms,
        config->active_route_ms, config->pending_data_ms, config->blacklist_ms,
        config->rrep_dedupe_ms, config->rerr_dedupe_ms, config->rrep_ack_wait_ms,
    };
    uint8_t i;
    tavrn_identity_width_t width;
    uint16_t derived;

    if (config->network_id == 0u || config->network_id == 0xffu ||
        config->net_diameter == 0u || config->net_diameter > 15u ||
        config->rreq_rate_per_second == 0u || config->rerr_rate_per_second == 0u ||
        config->request_rrep_ack > 1u) {
        return 0;
    }
    width = config->local_peer.logical_id.width;
    if (width != TAVRN_IDENTITY_SID8 && width != TAVRN_IDENTITY_SID16) {
        return 0;
    }
    derived = width == TAVRN_IDENTITY_SID8 ?
        (uint16_t)config->local_peer.adva.bytes[0] :
        (uint16_t)((uint16_t)config->local_peer.adva.bytes[0] |
                   ((uint16_t)config->local_peer.adva.bytes[1] << 8));
    if (!id_is_unicast(width, derived) || derived != config->local_peer.logical_id.value) {
        return 0;
    }
    for (i = 0u; i < (uint8_t)(sizeof(durations) / sizeof(durations[0])); i++) {
        if (durations[i] == 0u || durations[i] >= 0x80000000u) {
            return 0;
        }
    }
    return 1;
}

aodv_init_status_t aodv_core_init(aodv_core_t *core,
                                  const aodv_core_config_t *config,
                                  uint32_t now_ms)
{
    aodv_core_state_t *state;

    if (core == NULL || config == NULL) {
        return AODV_INIT_INVALID_ARGUMENT;
    }
    if (!config_is_valid(config)) {
        return AODV_INIT_INVALID_CONFIG;
    }
    memset(core, 0, sizeof(*core));
    core->config = *config;
    state = state_of(core);
    state->next_request_id = config->initial_request_id == 0u ? 1u :
        config->initial_request_id;
    state->next_rerr_sequence = config->initial_rerr_sequence == 0u ? 1u :
        config->initial_rerr_sequence;
    state->next_action_token = 1u;
    state->next_data_sequence = 1u;
    state->local_origin_sequence = config->initial_origin_sequence;
    state->rreq_window_started_ms = now_ms;
    state->rerr_window_started_ms = now_ms;
    return AODV_INIT_OK;
}

aodv_status_t aodv_core_submit_application(aodv_core_t *core,
                                           const tron_application_data_t *data,
                                           uint32_t now_ms)
{
    aodv_core_state_t *state;
    aodv_route_entry_t *route;
    tavrn_link_data_t link_data;
    aodv_discovery_entry_t *discovery;

    if (core == NULL || data == NULL || !local_width_is_valid(core) ||
        data->final_destination.width != core->config.local_peer.logical_id.width ||
        !id_is_unicast(data->final_destination.width, data->final_destination.value) ||
        data->app_len > TAVRN_LINK_APP_BYTES) {
        return AODV_STATUS_INVALID;
    }
    state = state_of(core);
    memset(&link_data, 0, sizeof(link_data));
    link_data.origin = core->config.local_peer.logical_id;
    link_data.final_destination = data->final_destination;
    link_data.data_seq = next_nonzero(&state->next_data_sequence);
    link_data.ttl = core->config.net_diameter;
    link_data.app_kind = data->app_kind;
    link_data.app_source = data->app_source;
    link_data.urgent = data->urgent;
    link_data.app_len = data->app_len;
    link_data.ownership = TAVRN_DATA_ORIGINATED;
    if (link_data.app_len != 0u) {
        memcpy(link_data.app_bytes, data->app_bytes, link_data.app_len);
    }
    route = route_find(core, data->final_destination.value);
    if (route_is_valid_at(core, route, now_ms)) {
        aodv_action_t action;

        if (!action_space(state, 1u)) {
            core->counters.action_backpressure++;
            return AODV_STATUS_BUSY;
        }
        memset(&action, 0, sizeof(action));
        action.type = AODV_ACTION_FORWARD_DATA;
        action.detail.data.next_hop = route->next_hop;
        action.detail.data.data = link_data;
        (void)enqueue_action(core, &action);
        return AODV_STATUS_OK;
    }
    discovery = discovery_find(core, data->final_destination.value);
    if (route == NULL && !route_has_slot(core)) {
        core->counters.route_capacity_rejected++;
        return AODV_STATUS_BUSY;
    }
    if (discovery == NULL &&
        (discovery_slot(core) == NULL || !action_space(state, 1u))) {
        if (discovery_slot(core) == NULL) {
            core->counters.pending_busy++;
        } else {
            core->counters.action_backpressure++;
        }
        return AODV_STATUS_BUSY;
    }
    if (!queue_pending(core, &link_data, now_ms)) {
        return AODV_STATUS_BUSY;
    }
    if (discovery != NULL) {
        return AODV_STATUS_QUEUED;
    }
    discovery = discovery_slot(core);
    memset(discovery, 0, sizeof(*discovery));
    discovery->occupied = 1u;
    discovery->destination = data->final_destination.value;
    discovery->next_attempt_ms = now_ms;
    (void)emit_rreq(core, discovery, now_ms);
    return AODV_STATUS_QUEUED;
}

aodv_status_t aodv_core_ingest_control(aodv_core_t *core,
                                        const aodv_control_input_t *input,
                                        uint32_t now_ms)
{
    if (core == NULL || input == NULL) {
        return AODV_STATUS_INVALID;
    }
    switch (input->control.type) {
    case TAVRN_WIRE_E_RREQ:
        return ingest_rreq(core, input, now_ms);
    case TAVRN_WIRE_E_RREP:
        return ingest_rrep(core, input, now_ms);
    case TAVRN_WIRE_E_RERR:
        return ingest_rerr(core, input, now_ms);
    case TAVRN_WIRE_E_RREP_ACK:
        return ingest_rrep_ack(core, input, now_ms);
    default:
        return AODV_STATUS_INVALID;
    }
}

aodv_status_t aodv_core_ingest_data(aodv_core_t *core,
                                     const aodv_data_input_t *input,
                                     uint32_t now_ms)
{
    aodv_route_entry_t *route;
    aodv_action_t action;
    tavrn_link_data_t data;

    if (core == NULL || input == NULL || !peer_is_valid_for_core(core, &input->transmitter) ||
        input->data.origin.width != core->config.local_peer.logical_id.width ||
        input->data.final_destination.width != core->config.local_peer.logical_id.width ||
        !id_is_unicast(input->data.origin.width, input->data.origin.value) ||
        !id_is_unicast(input->data.final_destination.width,
                       input->data.final_destination.value) ||
        input->data.app_len > TAVRN_LINK_APP_BYTES) {
        return AODV_STATUS_INVALID;
    }
    if (!action_space(state_of(core), 1u)) {
        core->counters.action_backpressure++;
        return AODV_STATUS_BUSY;
    }
    if (input->data.final_destination.value == core->config.local_peer.logical_id.value) {
        memset(&action, 0, sizeof(action));
        action.type = AODV_ACTION_DELIVER_DATA;
        action.detail.data.data = input->data;
        (void)enqueue_action(core, &action);
        return AODV_STATUS_OK;
    }
    if (input->data.ttl == 0u) {
        return AODV_STATUS_INVALID;
    }
    route = route_find(core, input->data.final_destination.value);
    if (!route_is_valid_at(core, route, now_ms)) {
        return AODV_STATUS_NOT_FOUND;
    }
    data = input->data;
    data.ttl--;
    data.hops++;
    memset(&action, 0, sizeof(action));
    action.type = AODV_ACTION_FORWARD_DATA;
    action.detail.data.next_hop = route->next_hop;
    action.detail.data.data = data;
    route_add_precursor(core, route, input->transmitter.logical_id.value);
    (void)enqueue_action(core, &action);
    return AODV_STATUS_OK;
}

aodv_status_t aodv_core_mark_action_sent(aodv_core_t *core,
                                          uint16_t action_token,
                                          uint32_t now_ms)
{
    aodv_core_state_t *state;
    uint8_t i;

    if (core == NULL || action_token == 0u) {
        return AODV_STATUS_INVALID;
    }
    state = state_of(core);
    for (i = 0u; i < TAVRN_AODV_RREP_ACK_WAIT_CAPACITY; i++) {
        if (state->ack_waits[i].occupied != 0u &&
            state->ack_waits[i].token == action_token) {
            if (state->ack_waits[i].sent != 0u) {
                return AODV_STATUS_DUPLICATE;
            }
            state->ack_waits[i].sent = 1u;
            state->ack_waits[i].expires_at_ms =
                now_ms + core->config.rrep_ack_wait_ms;
            return AODV_STATUS_OK;
        }
    }
    return AODV_STATUS_NOT_FOUND;
}

static void expire_pending(aodv_core_t *core, uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_PENDING_DATA_CAPACITY; i++) {
        if (state->pending[i].occupied != 0u &&
            time_due(now_ms, state->pending[i].expires_at_ms)) {
            aodv_action_t action;

            if (!action_space(state, 1u)) {
                core->counters.action_backpressure++;
                continue;
            }
            memset(&action, 0, sizeof(action));
            action.type = AODV_ACTION_PENDING_DATA_FAILED;
            action.detail.failure.destination = state->pending[i].data.final_destination;
            (void)enqueue_action(core, &action);
            state->pending[i].occupied = 0u;
            core->counters.pending_expired++;
        }
    }
}

static void advance_discoveries(aodv_core_t *core, uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);
    uint8_t i;

    for (i = 0u; i < AODV_DISCOVERY_CAPACITY; i++) {
        aodv_discovery_entry_t *discovery = &state->discoveries[i];

        if (discovery->occupied == 0u ||
            !has_pending_destination(core, discovery->destination)) {
            discovery->occupied = 0u;
            continue;
        }
        if (route_is_valid_at(core, route_find(core, discovery->destination), now_ms)) {
            continue;
        }
        if (time_due(now_ms, discovery->next_attempt_ms)) {
            if (discovery->next_ring <
                (uint8_t)(AODV_RREQ_RING_COUNT + core->config.rreq_retries)) {
                (void)emit_rreq(core, discovery, now_ms);
            } else {
                discovery->occupied = 0u;
            }
        }
    }
}

aodv_status_t aodv_core_tick(aodv_core_t *core, uint32_t now_ms)
{
    aodv_core_state_t *state;
    uint8_t i;

    if (core == NULL) {
        return AODV_STATUS_INVALID;
    }
    state = state_of(core);
    for (i = 0u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        if (state->routes[i].occupied != 0u &&
            state->routes[i].state == AODV_ROUTE_VALID &&
            time_due(now_ms, state->routes[i].expires_at_ms)) {
            state->routes[i].state = AODV_ROUTE_INVALID;
            core->counters.route_expired++;
        }
    }
    for (i = 0u; i < TAVRN_AODV_RREP_ACK_WAIT_CAPACITY; i++) {
        aodv_rrep_ack_wait_t *wait = &state->ack_waits[i];

        if (wait->occupied != 0u && wait->sent != 0u &&
            time_due(now_ms, wait->expires_at_ms)) {
            expire_ack_wait(core, wait, now_ms);
        }
    }
    for (i = 0u; i < AODV_BLACKLIST_CAPACITY; i++) {
        if (state->blacklist[i].occupied != 0u &&
            time_due(now_ms, state->blacklist[i].expires_at_ms)) {
            state->blacklist[i].occupied = 0u;
        }
    }
    expire_pending(core, now_ms);
    drain_pending_routes(core);
    advance_discoveries(core, now_ms);
    return AODV_STATUS_OK;
}

aodv_action_poll_status_t aodv_core_poll_action(aodv_core_t *core,
                                                 aodv_action_t *action_out)
{
    aodv_core_state_t *state;

    if (core == NULL || action_out == NULL) {
        return AODV_ACTION_POLL_INVALID;
    }
    state = state_of(core);
    if (state->action_count == 0u) {
        memset(action_out, 0, sizeof(*action_out));
        return AODV_ACTION_POLL_EMPTY;
    }
    *action_out = state->actions[state->action_head];
    state->action_head = (uint8_t)((state->action_head + 1u) %
                                   TAVRN_AODV_ACTION_CAPACITY);
    state->action_count--;
    drain_pending_routes(core);
    return AODV_ACTION_POLL_OK;
}

aodv_route_query_status_t aodv_core_route_snapshot(
    const aodv_core_t *core, const tavrn_logical_id_t *destination,
    aodv_route_snapshot_t *snapshot_out)
{
    const aodv_route_entry_t *route;
    tavrn_logical_id_t destination_copy;
    uint8_t i;

    if (core == NULL || destination == NULL || snapshot_out == NULL ||
        !local_width_is_valid(core) ||
        destination->width != core->config.local_peer.logical_id.width ||
        !id_is_unicast(destination->width, destination->value)) {
        return AODV_ROUTE_QUERY_INVALID;
    }
    destination_copy = *destination;
    route = const_route_find(core, destination_copy.value);
    if (route == NULL) {
        return AODV_ROUTE_QUERY_NOT_FOUND;
    }
    memset(snapshot_out, 0, sizeof(*snapshot_out));
    snapshot_out->destination = destination_copy;
    snapshot_out->next_hop = route->next_hop;
    snapshot_out->destination_sequence = route->destination_sequence;
    snapshot_out->expires_at_ms = route->expires_at_ms;
    snapshot_out->hop_count = route->hop_count;
    snapshot_out->sequence_valid = route->sequence_valid;
    snapshot_out->state = route->state;
    snapshot_out->precursor_count = route->precursor_count;
    snapshot_out->precursor_overflow = route->precursor_overflow;
    for (i = 0u; i < route->precursor_count; i++) {
        snapshot_out->precursors[i].width = core->config.local_peer.logical_id.width;
        snapshot_out->precursors[i].value = route->precursors[i];
    }
    return AODV_ROUTE_QUERY_FOUND;
}

const aodv_counters_t *aodv_core_counters(const aodv_core_t *core)
{
    return core == NULL ? NULL : &core->counters;
}

static void sort_routes(aodv_route_entry_t *routes[], uint8_t count)
{
    uint8_t i;

    for (i = 1u; i < count; i++) {
        aodv_route_entry_t *current = routes[i];
        uint8_t j = i;

        while (j > 0u && routes[(uint8_t)(j - 1u)]->destination > current->destination) {
            routes[j] = routes[(uint8_t)(j - 1u)];
            j--;
        }
        routes[j] = current;
    }
}

static void build_rerr_action(aodv_core_t *core, aodv_action_t *action,
                              aodv_route_entry_t *routes[], uint8_t count)
{
    tavrn_identity_width_t width = core->config.local_peer.logical_id.width;
    uint8_t width_len = width_bytes(width);
    uint8_t entries_offset = (uint8_t)(10u + width_len);
    uint8_t i;
    tavrn_validated_control_t *control;
    aodv_core_state_t *state = state_of(core);

    memset(action, 0, sizeof(*action));
    action->type = AODV_ACTION_SEND_RERR;
    action->detail.control.controlled_flood = 1u;
    control = &action->detail.control.control;
    begin_control(control, TAVRN_WIRE_E_RERR,
                  (uint8_t)(entries_offset + count * (width_len + 2u)), core);
    control->pdu[AODV_PDU_TTL_HOPS_OFFSET] =
        (uint8_t)(core->config.net_diameter << 4);
    pdu_put_id(control->pdu, 7u, width, core->config.local_peer.logical_id.value);
    pdu_put_u16(control->pdu, (uint8_t)(7u + width_len),
                next_nonzero(&state->next_rerr_sequence));
    control->pdu[(uint8_t)(9u + width_len)] = count;
    for (i = 0u; i < count; i++) {
        uint8_t offset = (uint8_t)(entries_offset + i * (width_len + 2u));

        pdu_put_id(control->pdu, offset, width, routes[i]->destination);
        pdu_put_u16(control->pdu, (uint8_t)(offset + width_len),
                    routes[i]->destination_sequence);
    }
}

static aodv_failure_status_t queue_route_failure(aodv_core_t *core,
                                                 const tavrn_direct_peer_t *failed,
                                                 const tavrn_logical_id_t *deferred,
                                                 uint32_t now_ms)
{
    aodv_core_state_t *state = state_of(core);
    aodv_route_entry_t *affected[TAVRN_AODV_ROUTE_CAPACITY];
    aodv_route_entry_t *reported[TAVRN_AODV_ROUTE_CAPACITY];
    uint8_t affected_count = 0u;
    uint8_t reported_count = 0u;
    uint8_t maximum = core->config.local_peer.logical_id.width == TAVRN_IDENTITY_SID16 ?
        TAVRN_AODV_RERR_SID16_PER_ACTION : TAVRN_AODV_RERR_SID8_PER_ACTION;
    uint8_t required;
    uint8_t i;

    for (i = 0u; i < TAVRN_AODV_ROUTE_CAPACITY; i++) {
        aodv_route_entry_t *route = &state->routes[i];

        if (route->occupied != 0u && route->state == AODV_ROUTE_VALID &&
            peer_equal(&route->next_hop, failed)) {
            affected[affected_count++] = route;
            if (deferred == NULL || route->destination != deferred->value) {
                reported[reported_count++] = route;
            }
        }
    }
    required = (uint8_t)((reported_count + maximum - 1u) / maximum);
    if (!action_space(state, required)) {
        core->counters.action_backpressure++;
        return AODV_FAILURE_BUSY;
    }
    if (required != 0u && !rerr_rate_allowed(core, now_ms)) {
        return AODV_FAILURE_BUSY;
    }
    sort_routes(reported, reported_count);
    for (i = 0u; i < affected_count; i++) {
        affected[i]->state = AODV_ROUTE_INVALID;
        if (affected[i]->sequence_valid != 0u) {
            affected[i]->destination_sequence++;
        } else {
            affected[i]->sequence_valid = 1u;
        }
    }
    for (i = 0u; i < reported_count; i = (uint8_t)(i + maximum)) {
        aodv_action_t action;
        uint8_t count = (uint8_t)(reported_count - i);

        if (count > maximum) {
            count = maximum;
        }
        build_rerr_action(core, &action, &reported[i], count);
        (void)enqueue_action(core, &action);
    }
    return AODV_FAILURE_OK;
}

aodv_failure_status_t aodv_core_report_link_failure(
    aodv_core_t *core, const tavrn_direct_peer_t *failed_next_hop,
    const tavrn_logical_id_t *repair_destination,
    aodv_link_failure_mode_t mode, uint32_t now_ms)
{
    aodv_failure_status_t result;

    if (core == NULL || !peer_is_valid_for_core(core, failed_next_hop) ||
        (mode != AODV_LINK_FAILURE_IMMEDIATE_RERR &&
         mode != AODV_LINK_FAILURE_DEFER_REPAIR_DESTINATION_RERR)) {
        return AODV_FAILURE_INVALID;
    }
    if (mode == AODV_LINK_FAILURE_DEFER_REPAIR_DESTINATION_RERR) {
        if (repair_destination == NULL ||
            repair_destination->width != core->config.local_peer.logical_id.width ||
            !id_is_unicast(repair_destination->width, repair_destination->value) ||
            state_of(core)->deferred.active != 0u) {
            return AODV_FAILURE_INVALID;
        }
        result = queue_route_failure(core, failed_next_hop, repair_destination, now_ms);
        if (result == AODV_FAILURE_OK) {
            aodv_route_entry_t *route = route_find(core, repair_destination->value);

            state_of(core)->deferred.active = 1u;
            state_of(core)->deferred.destination = repair_destination->value;
            state_of(core)->deferred.destination_sequence = route == NULL ? 0u :
                route->destination_sequence;
        }
        return result;
    }
    return queue_route_failure(core, failed_next_hop, NULL, now_ms);
}

aodv_failure_status_t aodv_core_finish_deferred_rerr(
    aodv_core_t *core, const tavrn_logical_id_t *repair_destination,
    aodv_deferred_rerr_decision_t decision, uint32_t now_ms)
{
    aodv_core_state_t *state;
    aodv_route_entry_t *route;
    aodv_route_entry_t *routes[1];
    aodv_action_t action;

    if (core == NULL || repair_destination == NULL ||
        repair_destination->width != core->config.local_peer.logical_id.width ||
        (decision != AODV_DEFERRED_RERR_ROUTE_REPAIRED &&
         decision != AODV_DEFERRED_RERR_REPAIR_FAILED)) {
        return AODV_FAILURE_INVALID;
    }
    state = state_of(core);
    if (state->deferred.active == 0u ||
        state->deferred.destination != repair_destination->value) {
        return AODV_FAILURE_INVALID;
    }
    if (decision == AODV_DEFERRED_RERR_ROUTE_REPAIRED) {
        memset(&state->deferred, 0, sizeof(state->deferred));
        return AODV_FAILURE_OK;
    }
    if (!action_space(state, 1u) || !rerr_rate_allowed(core, now_ms)) {
        return AODV_FAILURE_BUSY;
    }
    route = route_find(core, repair_destination->value);
    if (route == NULL) {
        return AODV_FAILURE_INVALID;
    }
    route->state = AODV_ROUTE_INVALID;
    route->sequence_valid = 1u;
    route->destination_sequence = state->deferred.destination_sequence;
    routes[0] = route;
    build_rerr_action(core, &action, routes, 1u);
    (void)enqueue_action(core, &action);
    memset(&state->deferred, 0, sizeof(state->deferred));
    return AODV_FAILURE_OK;
}
