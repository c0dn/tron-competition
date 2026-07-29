#ifndef RF_MESH_CORE_H
#define RF_MESH_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "rf_mesh_packet.h"

typedef enum {
    RF_MESH_OK = 0,
    RF_MESH_ERR_INVALID = -1,
    RF_MESH_ERR_FULL = -2,
    RF_MESH_ERR_EMPTY = -3,
    RF_MESH_ERR_DUPLICATE = -4,
    RF_MESH_ERR_TTL_EXPIRED = -5,
    RF_MESH_ERR_NO_ROUTE = -6,
    RF_MESH_ERR_REJECTED = -7,
    RF_MESH_ERR_UNSUPPORTED = -8
} rf_mesh_status_t;

typedef enum {
    RF_MESH_TX_PRIORITY_RELAY = 0,
    RF_MESH_TX_PRIORITY_LOCAL = 1,
    RF_MESH_TX_PRIORITY_RREP = 2
} rf_mesh_tx_priority_t;

typedef struct {
    uint16_t node_id;
    uint16_t network_id;
    uint16_t root_id;
    uint8_t is_root;
    uint32_t prng_seed;
} rf_mesh_config_t;

typedef struct {
    uint8_t valid;
    uint16_t destination_id;
    uint16_t next_hop_id;
    uint8_t hop_count;
    uint8_t metric;
    uint16_t sequence_id;
    uint32_t last_seen_ms;
} rf_mesh_route_t;

typedef struct {
    uint8_t valid;
    uint16_t network_id;
    uint8_t message_type;
    uint16_t source_id;
    uint16_t sequence_id;
    uint32_t first_seen_ms;
} rf_mesh_dedup_entry_t;

typedef struct {
    uint8_t valid;
    uint8_t priority;
    uint16_t next_hop_id;
    uint32_t due_ms;
    uint32_t enqueue_order;
    rf_mesh_packet_t packet;
} rf_mesh_tx_item_t;

typedef struct {
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t relay_packets;
    uint32_t duplicate_drops;
    uint32_t ttl_drops;
    uint32_t queue_drops;
    uint32_t route_updates;
    uint32_t route_rejects;
    uint32_t packet_drops;
    uint32_t no_route_drops;
} rf_mesh_counters_t;

typedef struct {
    rf_mesh_config_t config;
    rf_mesh_route_t routes[RF_MESH_MAX_ROUTES];
    rf_mesh_dedup_entry_t dedup[RF_MESH_DEDUP_ENTRIES];
    rf_mesh_tx_item_t tx_queue[RF_MESH_TX_QUEUE_ENTRIES];
    rf_mesh_counters_t counters;
    uint16_t local_sequence;
    uint32_t prng_state;
    uint8_t dedup_next;
    uint32_t queue_order_next;
} rf_mesh_state_t;

void rf_mesh_default_config(rf_mesh_config_t *config);
void rf_mesh_init(rf_mesh_state_t *state, const rf_mesh_config_t *config);
void rf_mesh_reset_counters(rf_mesh_state_t *state);
const rf_mesh_counters_t *rf_mesh_counters(const rf_mesh_state_t *state);
uint16_t rf_mesh_next_sequence(rf_mesh_state_t *state);

int rf_mesh_dedup_check_and_store(rf_mesh_state_t *state,
                                  const rf_mesh_packet_t *packet,
                                  uint32_t now_ms);

int rf_mesh_build_relay_packet(rf_mesh_state_t *state,
                               const rf_mesh_packet_t *input,
                               rf_mesh_packet_t *output);

int rf_mesh_route_update(rf_mesh_state_t *state,
                         uint16_t destination_id,
                         uint16_t next_hop_id,
                         uint8_t hop_count,
                         uint8_t metric,
                         uint16_t sequence_id,
                         uint32_t now_ms);
const rf_mesh_route_t *rf_mesh_route_lookup(const rf_mesh_state_t *state,
                                            uint16_t destination_id,
                                            uint32_t now_ms);
int rf_mesh_has_route(const rf_mesh_state_t *state,
                      uint16_t destination_id,
                      uint32_t now_ms);
/* Learn routing metadata from a decoded control packet. heard_from_id is the
   immediate transmitter for this hop, normally packet->previous_hop_id. */
int rf_mesh_learn_from_packet(rf_mesh_state_t *state,
                              const rf_mesh_packet_t *packet,
                              uint16_t heard_from_id,
                              uint32_t now_ms);
int rf_mesh_needs_root_discovery(const rf_mesh_state_t *state,
                                 uint32_t now_ms);

uint32_t rf_mesh_next_jitter_ms(rf_mesh_state_t *state,
                                uint32_t min_ms,
                                uint32_t max_ms);
int rf_mesh_enqueue(rf_mesh_state_t *state,
                    const rf_mesh_packet_t *packet,
                    uint16_t next_hop_id,
                    uint8_t priority,
                    uint32_t due_ms);
int rf_mesh_enqueue_with_jitter(rf_mesh_state_t *state,
                                const rf_mesh_packet_t *packet,
                                uint16_t next_hop_id,
                                uint8_t priority,
                                uint32_t now_ms,
                                uint32_t min_jitter_ms,
                                uint32_t max_jitter_ms);
int rf_mesh_schedule_relay(rf_mesh_state_t *state,
                           const rf_mesh_packet_t *input,
                           uint16_t next_hop_id,
                           uint32_t now_ms,
                           uint32_t min_jitter_ms,
                           uint32_t max_jitter_ms);
int rf_mesh_dequeue_due(rf_mesh_state_t *state,
                        uint32_t now_ms,
                        rf_mesh_tx_item_t *out_item);
size_t rf_mesh_queue_count(const rf_mesh_state_t *state);

#endif /* RF_MESH_CORE_H */
