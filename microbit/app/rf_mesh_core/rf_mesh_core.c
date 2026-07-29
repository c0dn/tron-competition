#include "rf_mesh_core.h"

#include <string.h>

#define RF_MESH_PRNG_FALLBACK_SEED 0x4d455348u
#define RF_MESH_TIME_HALF_RANGE    0x80000000u

static int time_reached(uint32_t now_ms, uint32_t due_ms)
{
    return (uint32_t)(now_ms - due_ms) < RF_MESH_TIME_HALF_RANGE;
}

static int time_before(uint32_t a_ms, uint32_t b_ms)
{
    return a_ms != b_ms && (uint32_t)(b_ms - a_ms) < RF_MESH_TIME_HALF_RANGE;
}

static int age_expired(uint32_t now_ms, uint32_t seen_ms, uint32_t expiry_ms)
{
    return (uint32_t)(now_ms - seen_ms) >= expiry_ms;
}

static int sequence_newer(uint16_t candidate, uint16_t current)
{
    uint16_t diff = (uint16_t)(candidate - current);

    return diff != 0u && diff < 0x8000u;
}

static int sequence_older(uint16_t candidate, uint16_t current)
{
    uint16_t diff = (uint16_t)(current - candidate);

    return diff != 0u && diff < 0x8000u;
}

static int packet_is_broadcast(const rf_mesh_packet_t *packet)
{
    return packet->destination_id == RF_MESH_BROADCAST_ID &&
           (packet->flags & RF_MESH_FLAG_BROADCAST) != 0u;
}

static uint8_t increment_saturating(uint8_t value)
{
    if (value == UINT8_MAX) {
        return value;
    }
    return (uint8_t)(value + 1u);
}

static int route_is_expired(const rf_mesh_route_t *route, uint32_t now_ms)
{
    return route->valid == 0u ||
           age_expired(now_ms, route->last_seen_ms, RF_MESH_ROUTE_EXPIRY_MS);
}

static int candidate_route_is_better(const rf_mesh_route_t *current,
                                     uint8_t hop_count,
                                     uint8_t metric,
                                     uint16_t sequence_id,
                                     uint32_t now_ms)
{
    if (route_is_expired(current, now_ms)) {
        return 1;
    }
    if (sequence_newer(sequence_id, current->sequence_id)) {
        return 1;
    }
    if (sequence_older(sequence_id, current->sequence_id)) {
        return 0;
    }
    if (metric < current->metric) {
        return 1;
    }
    if (metric > current->metric) {
        return 0;
    }
    if (hop_count < current->hop_count) {
        return 1;
    }
    if (hop_count > current->hop_count) {
        return 0;
    }

    return 1; /* same-quality refresh is the most recently seen route */
}

static uint32_t prng_next(rf_mesh_state_t *state)
{
    if (state->prng_state == 0u) {
        state->prng_state = RF_MESH_PRNG_FALLBACK_SEED;
    }
    state->prng_state = state->prng_state * 1664525u + 1013904223u;
    return state->prng_state;
}

void rf_mesh_default_config(rf_mesh_config_t *config)
{
    if (config == 0) {
        return;
    }

    config->node_id = 0;
    config->network_id = RF_MESH_DEFAULT_NETWORK_ID;
    config->root_id = RF_MESH_DEFAULT_ROOT_ID;
    config->is_root = 0;
    config->prng_seed = 0;
}

void rf_mesh_init(rf_mesh_state_t *state, const rf_mesh_config_t *config)
{
    rf_mesh_config_t default_config;
    uint32_t seed;

    if (state == 0) {
        return;
    }

    if (config == 0) {
        rf_mesh_default_config(&default_config);
        config = &default_config;
    }

    memset(state, 0, sizeof(*state));
    state->config = *config;
    state->local_sequence = 1;
    state->queue_order_next = 1;

    seed = config->prng_seed;
    if (seed == 0u) {
        seed = RF_MESH_PRNG_FALLBACK_SEED ^
               ((uint32_t)config->node_id << 16) ^
               (uint32_t)config->network_id ^
               ((uint32_t)config->root_id << 1) ^
               (uint32_t)config->is_root;
    }
    if (seed == 0u) {
        seed = RF_MESH_PRNG_FALLBACK_SEED;
    }
    state->prng_state = seed;
}

void rf_mesh_reset_counters(rf_mesh_state_t *state)
{
    if (state == 0) {
        return;
    }
    memset(&state->counters, 0, sizeof(state->counters));
}

const rf_mesh_counters_t *rf_mesh_counters(const rf_mesh_state_t *state)
{
    if (state == 0) {
        return 0;
    }
    return &state->counters;
}

uint16_t rf_mesh_next_sequence(rf_mesh_state_t *state)
{
    uint16_t sequence;

    if (state == 0) {
        return 0;
    }

    sequence = state->local_sequence;
    state->local_sequence = (uint16_t)(state->local_sequence + 1u);
    return sequence;
}

int rf_mesh_dedup_check_and_store(rf_mesh_state_t *state,
                                  const rf_mesh_packet_t *packet,
                                  uint32_t now_ms)
{
    size_t i;
    size_t target = RF_MESH_DEDUP_ENTRIES;

    if (state == 0 || packet == 0 || !rf_mesh_packet_type_is_valid(packet->type)) {
        return RF_MESH_ERR_INVALID;
    }
    if (packet->network_id != state->config.network_id) {
        state->counters.packet_drops++;
        return RF_MESH_ERR_INVALID;
    }

    for (i = 0; i < RF_MESH_DEDUP_ENTRIES; i++) {
        rf_mesh_dedup_entry_t *entry = &state->dedup[i];

        if (entry->valid != 0u &&
            entry->network_id == packet->network_id &&
            entry->message_type == packet->type &&
            entry->source_id == packet->source_id &&
            entry->sequence_id == packet->sequence_id) {
            if (!age_expired(now_ms, entry->first_seen_ms, RF_MESH_DEDUP_EXPIRY_MS)) {
                state->counters.duplicate_drops++;
                return RF_MESH_ERR_DUPLICATE;
            }
            target = i;
            break;
        }
    }

    if (target == RF_MESH_DEDUP_ENTRIES) {
        for (i = 0; i < RF_MESH_DEDUP_ENTRIES; i++) {
            if (state->dedup[i].valid == 0u) {
                target = i;
                break;
            }
        }
    }
    if (target == RF_MESH_DEDUP_ENTRIES) {
        for (i = 0; i < RF_MESH_DEDUP_ENTRIES; i++) {
            if (age_expired(now_ms,
                            state->dedup[i].first_seen_ms,
                            RF_MESH_DEDUP_EXPIRY_MS)) {
                target = i;
                break;
            }
        }
    }
    if (target == RF_MESH_DEDUP_ENTRIES) {
        state->counters.packet_drops++;
        return RF_MESH_ERR_FULL;
    }

    state->dedup[target].valid = 1;
    state->dedup[target].network_id = packet->network_id;
    state->dedup[target].message_type = packet->type;
    state->dedup[target].source_id = packet->source_id;
    state->dedup[target].sequence_id = packet->sequence_id;
    state->dedup[target].first_seen_ms = now_ms;
    return RF_MESH_OK;
}

int rf_mesh_build_relay_packet(rf_mesh_state_t *state,
                               const rf_mesh_packet_t *input,
                               rf_mesh_packet_t *output)
{
    if (state == 0 || input == 0 || output == 0 ||
        !rf_mesh_packet_type_can_emit_v1(input->type)) {
        return RF_MESH_ERR_INVALID;
    }
    if (input->ttl <= 1u) {
        state->counters.ttl_drops++;
        return RF_MESH_ERR_TTL_EXPIRED;
    }

    *output = *input;
    output->ttl = (uint8_t)(input->ttl - 1u);
    output->hop_count = increment_saturating(input->hop_count);
    output->metric = increment_saturating(input->metric);
    output->previous_hop_id = state->config.node_id;
    output->flags = (uint8_t)((input->flags | RF_MESH_FLAG_RELAYED) &
                              RF_MESH_KNOWN_FLAGS);
    state->counters.relay_packets++;
    return RF_MESH_OK;
}

int rf_mesh_route_update(rf_mesh_state_t *state,
                         uint16_t destination_id,
                         uint16_t next_hop_id,
                         uint8_t hop_count,
                         uint8_t metric,
                         uint16_t sequence_id,
                         uint32_t now_ms)
{
    size_t i;
    size_t slot = RF_MESH_MAX_ROUTES;

    if (state == 0) {
        return RF_MESH_ERR_INVALID;
    }

    for (i = 0; i < RF_MESH_MAX_ROUTES; i++) {
        if (state->routes[i].valid != 0u &&
            state->routes[i].destination_id == destination_id) {
            slot = i;
            break;
        }
    }

    if (slot == RF_MESH_MAX_ROUTES) {
        for (i = 0; i < RF_MESH_MAX_ROUTES; i++) {
            if (route_is_expired(&state->routes[i], now_ms)) {
                slot = i;
                break;
            }
        }
    }

    if (slot == RF_MESH_MAX_ROUTES) {
        state->counters.route_rejects++;
        return RF_MESH_ERR_FULL;
    }

    if (state->routes[slot].valid != 0u &&
        state->routes[slot].destination_id == destination_id &&
        !candidate_route_is_better(&state->routes[slot],
                                   hop_count,
                                   metric,
                                   sequence_id,
                                   now_ms)) {
        state->counters.route_rejects++;
        return RF_MESH_ERR_REJECTED;
    }

    state->routes[slot].valid = 1;
    state->routes[slot].destination_id = destination_id;
    state->routes[slot].next_hop_id = next_hop_id;
    state->routes[slot].hop_count = hop_count;
    state->routes[slot].metric = metric;
    state->routes[slot].sequence_id = sequence_id;
    state->routes[slot].last_seen_ms = now_ms;
    state->counters.route_updates++;
    return RF_MESH_OK;
}

const rf_mesh_route_t *rf_mesh_route_lookup(const rf_mesh_state_t *state,
                                            uint16_t destination_id,
                                            uint32_t now_ms)
{
    size_t i;

    if (state == 0) {
        return 0;
    }

    for (i = 0; i < RF_MESH_MAX_ROUTES; i++) {
        if (state->routes[i].valid != 0u &&
            state->routes[i].destination_id == destination_id &&
            !route_is_expired(&state->routes[i], now_ms)) {
            return &state->routes[i];
        }
    }

    return 0;
}

int rf_mesh_has_route(const rf_mesh_state_t *state,
                      uint16_t destination_id,
                      uint32_t now_ms)
{
    return rf_mesh_route_lookup(state, destination_id, now_ms) != 0;
}

int rf_mesh_learn_from_packet(rf_mesh_state_t *state,
                              const rf_mesh_packet_t *packet,
                              uint16_t heard_from_id,
                              uint32_t now_ms)
{
    uint8_t hop_count;
    uint8_t metric;

    if (state == 0 || packet == 0 || !rf_mesh_packet_type_is_valid(packet->type)) {
        return RF_MESH_ERR_INVALID;
    }
    if (packet->network_id != state->config.network_id) {
        state->counters.packet_drops++;
        return RF_MESH_ERR_INVALID;
    }
    if (packet->source_id == 0u ||
        packet->source_id == RF_MESH_BROADCAST_ID ||
        heard_from_id == 0u ||
        heard_from_id == RF_MESH_BROADCAST_ID) {
        state->counters.packet_drops++;
        return RF_MESH_ERR_INVALID;
    }

    if (packet->type == RF_MESH_MSG_DATA || packet->type == RF_MESH_MSG_ACK) {
        return RF_MESH_ERR_UNSUPPORTED;
    }
    switch (packet->type) {
    case RF_MESH_MSG_HELLO:
        if ((packet->flags & RF_MESH_FLAG_ROOT) == 0u ||
            packet->source_id != state->config.root_id ||
            !packet_is_broadcast(packet)) {
            state->counters.packet_drops++;
            return RF_MESH_ERR_INVALID;
        }
        break;
    case RF_MESH_MSG_RREQ:
        if (packet->source_id == state->config.root_id ||
            (packet->flags & RF_MESH_FLAG_BROADCAST) == 0u ||
            (packet->destination_id != state->config.root_id &&
             packet->destination_id != RF_MESH_BROADCAST_ID)) {
            state->counters.packet_drops++;
            return RF_MESH_ERR_INVALID;
        }
        break;
    case RF_MESH_MSG_RREP:
        if ((packet->flags & RF_MESH_FLAG_ROUTE_REPLY) == 0u ||
            packet->source_id != state->config.root_id) {
            state->counters.packet_drops++;
            return RF_MESH_ERR_INVALID;
        }
        if (packet->destination_id == RF_MESH_BROADCAST_ID) {
            if ((packet->flags & RF_MESH_FLAG_BROADCAST) == 0u) {
                state->counters.packet_drops++;
                return RF_MESH_ERR_INVALID;
            }
        } else if (packet->destination_id == 0u ||
                   packet->destination_id == state->config.root_id ||
                   packet->destination_id == packet->source_id) {
            state->counters.packet_drops++;
            return RF_MESH_ERR_INVALID;
        }
        break;
    default:
        state->counters.packet_drops++;
        return RF_MESH_ERR_INVALID;
    }

    hop_count = increment_saturating(packet->hop_count);
    metric = increment_saturating(packet->metric);
    return rf_mesh_route_update(state,
                                packet->source_id,
                                heard_from_id,
                                hop_count,
                                metric,
                                packet->sequence_id,
                                now_ms);
}

int rf_mesh_needs_root_discovery(const rf_mesh_state_t *state,
                                 uint32_t now_ms)
{
    if (state == 0 || state->config.is_root != 0u) {
        return 0;
    }
    return !rf_mesh_has_route(state, state->config.root_id, now_ms);
}

uint32_t rf_mesh_next_jitter_ms(rf_mesh_state_t *state,
                                uint32_t min_ms,
                                uint32_t max_ms)
{
    uint32_t span;
    uint32_t value;

    if (state == 0) {
        return min_ms;
    }
    if (max_ms < min_ms) {
        value = min_ms;
        min_ms = max_ms;
        max_ms = value;
    }

    value = prng_next(state);
    span = max_ms - min_ms + 1u;
    if (span == 0u) {
        return value;
    }
    return min_ms + (value % span);
}

static int queue_priority_is_valid(uint8_t priority)
{
    return priority == RF_MESH_TX_PRIORITY_RELAY ||
           priority == RF_MESH_TX_PRIORITY_LOCAL ||
           priority == RF_MESH_TX_PRIORITY_RREP;
}

static void queue_store(rf_mesh_state_t *state,
                        size_t slot,
                        const rf_mesh_packet_t *packet,
                        uint16_t next_hop_id,
                        uint8_t priority,
                        uint32_t due_ms)
{
    state->tx_queue[slot].valid = 1;
    state->tx_queue[slot].priority = priority;
    state->tx_queue[slot].next_hop_id = next_hop_id;
    state->tx_queue[slot].due_ms = due_ms;
    state->tx_queue[slot].enqueue_order = state->queue_order_next++;
    state->tx_queue[slot].packet = *packet;
}

static size_t queue_find_replacement(const rf_mesh_state_t *state,
                                     uint8_t new_priority)
{
    size_t i;
    size_t victim = RF_MESH_TX_QUEUE_ENTRIES;

    for (i = 0; i < RF_MESH_TX_QUEUE_ENTRIES; i++) {
        const rf_mesh_tx_item_t *item = &state->tx_queue[i];

        if (item->valid == 0u || item->priority >= new_priority) {
            continue;
        }
        if (victim == RF_MESH_TX_QUEUE_ENTRIES ||
            item->priority < state->tx_queue[victim].priority ||
            (item->priority == state->tx_queue[victim].priority &&
             item->enqueue_order > state->tx_queue[victim].enqueue_order)) {
            victim = i;
        }
    }

    return victim;
}

int rf_mesh_enqueue(rf_mesh_state_t *state,
                    const rf_mesh_packet_t *packet,
                    uint16_t next_hop_id,
                    uint8_t priority,
                    uint32_t due_ms)
{
    size_t i;
    size_t slot = RF_MESH_TX_QUEUE_ENTRIES;

    if (state == 0 || packet == 0 || !rf_mesh_packet_type_can_emit_v1(packet->type) ||
        (packet->flags & (uint8_t)~RF_MESH_KNOWN_FLAGS) != 0u ||
        packet->payload_len > RF_MESH_MAX_PAYLOAD_LEN ||
        !queue_priority_is_valid(priority)) {
        return RF_MESH_ERR_INVALID;
    }

    for (i = 0; i < RF_MESH_TX_QUEUE_ENTRIES; i++) {
        if (state->tx_queue[i].valid == 0u) {
            slot = i;
            break;
        }
    }

    if (slot == RF_MESH_TX_QUEUE_ENTRIES) {
        slot = queue_find_replacement(state, priority);
        if (slot == RF_MESH_TX_QUEUE_ENTRIES) {
            state->counters.queue_drops++;
            return RF_MESH_ERR_FULL;
        }
        state->counters.queue_drops++;
    }

    queue_store(state, slot, packet, next_hop_id, priority, due_ms);
    return RF_MESH_OK;
}

int rf_mesh_enqueue_with_jitter(rf_mesh_state_t *state,
                                const rf_mesh_packet_t *packet,
                                uint16_t next_hop_id,
                                uint8_t priority,
                                uint32_t now_ms,
                                uint32_t min_jitter_ms,
                                uint32_t max_jitter_ms)
{
    uint32_t jitter;

    if (state == 0) {
        return RF_MESH_ERR_INVALID;
    }
    jitter = rf_mesh_next_jitter_ms(state, min_jitter_ms, max_jitter_ms);
    return rf_mesh_enqueue(state,
                           packet,
                           next_hop_id,
                           priority,
                           now_ms + jitter);
}

int rf_mesh_schedule_relay(rf_mesh_state_t *state,
                           const rf_mesh_packet_t *input,
                           uint16_t next_hop_id,
                           uint32_t now_ms,
                           uint32_t min_jitter_ms,
                           uint32_t max_jitter_ms)
{
    rf_mesh_packet_t relay;
    int rc;

    rc = rf_mesh_build_relay_packet(state, input, &relay);
    if (rc != RF_MESH_OK) {
        return rc;
    }
    return rf_mesh_enqueue_with_jitter(state,
                                       &relay,
                                       next_hop_id,
                                       RF_MESH_TX_PRIORITY_RELAY,
                                       now_ms,
                                       min_jitter_ms,
                                       max_jitter_ms);
}

int rf_mesh_dequeue_due(rf_mesh_state_t *state,
                        uint32_t now_ms,
                        rf_mesh_tx_item_t *out_item)
{
    size_t i;
    size_t best = RF_MESH_TX_QUEUE_ENTRIES;

    if (state == 0 || out_item == 0) {
        return RF_MESH_ERR_INVALID;
    }

    for (i = 0; i < RF_MESH_TX_QUEUE_ENTRIES; i++) {
        const rf_mesh_tx_item_t *item = &state->tx_queue[i];

        if (item->valid == 0u || !time_reached(now_ms, item->due_ms)) {
            continue;
        }
        if (best == RF_MESH_TX_QUEUE_ENTRIES ||
            item->priority > state->tx_queue[best].priority ||
            (item->priority == state->tx_queue[best].priority &&
             time_before(item->due_ms, state->tx_queue[best].due_ms)) ||
            (item->priority == state->tx_queue[best].priority &&
             item->due_ms == state->tx_queue[best].due_ms &&
             item->enqueue_order < state->tx_queue[best].enqueue_order)) {
            best = i;
        }
    }

    if (best == RF_MESH_TX_QUEUE_ENTRIES) {
        return RF_MESH_ERR_EMPTY;
    }

    *out_item = state->tx_queue[best];
    state->tx_queue[best].valid = 0;
    state->counters.tx_packets++;
    return RF_MESH_OK;
}

size_t rf_mesh_queue_count(const rf_mesh_state_t *state)
{
    size_t i;
    size_t count = 0;

    if (state == 0) {
        return 0;
    }

    for (i = 0; i < RF_MESH_TX_QUEUE_ENTRIES; i++) {
        if (state->tx_queue[i].valid != 0u) {
            count++;
        }
    }

    return count;
}
