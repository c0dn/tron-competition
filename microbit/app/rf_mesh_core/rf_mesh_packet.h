#ifndef RF_MESH_PACKET_H
#define RF_MESH_PACKET_H

#include <stddef.h>
#include <stdint.h>

#define RF_MESH_PROTOCOL_VERSION       2u
#define RF_MESH_MAX_PACKET_SIZE        32u
#define RF_MESH_HEADER_LEN             18u
#define RF_MESH_MAX_PAYLOAD_LEN        14u

#define RF_MESH_BROADCAST_ID           0xffffu
#define RF_MESH_DEFAULT_NETWORK_ID     0x5452u
#define RF_MESH_DEFAULT_ROOT_ID        0x0001u
#define RF_MESH_DEFAULT_TTL            5u

#define RF_MESH_MAX_ROUTES             8u
#define RF_MESH_DEDUP_ENTRIES          16u
#define RF_MESH_TX_QUEUE_ENTRIES       8u

#define RF_MESH_ROUTE_EXPIRY_MS        10000u
#define RF_MESH_DEDUP_EXPIRY_MS        8000u
#define RF_MESH_ROOT_BEACON_INTERVAL_MS 1000u
#define RF_MESH_DUMMY_DATA_INTERVAL_MS 3000u
#define RF_MESH_RELAY_JITTER_MIN_MS    20u
#define RF_MESH_RELAY_JITTER_MAX_MS    120u
#define RF_MESH_DISCOVERY_JITTER_MIN_MS 40u
#define RF_MESH_DISCOVERY_JITTER_MAX_MS 180u

typedef enum {
    RF_MESH_MSG_HELLO = 1,
    RF_MESH_MSG_RREQ = 2,
    RF_MESH_MSG_RREP = 3,
    RF_MESH_MSG_DATA = 4,
    RF_MESH_MSG_ACK = 5
} rf_mesh_message_type_t;

enum {
    RF_MESH_FLAG_ROOT = 0x01u,
    RF_MESH_FLAG_BROADCAST = 0x02u,
    RF_MESH_FLAG_RELAYED = 0x04u,
    RF_MESH_FLAG_ROUTE_REPLY = 0x08u,
    RF_MESH_KNOWN_FLAGS = RF_MESH_FLAG_ROOT |
                          RF_MESH_FLAG_BROADCAST |
                          RF_MESH_FLAG_RELAYED |
                          RF_MESH_FLAG_ROUTE_REPLY
};

typedef enum {
    RF_MESH_PACKET_OK = 0,
    RF_MESH_PACKET_ERR_NULL = -1,
    RF_MESH_PACKET_ERR_BUFFER_SMALL = -2,
    RF_MESH_PACKET_ERR_PAYLOAD_LEN = -3,
    RF_MESH_PACKET_ERR_VERSION = -4,
    RF_MESH_PACKET_ERR_TYPE = -5,
    RF_MESH_PACKET_ERR_LENGTH = -6,
    RF_MESH_PACKET_ERR_TRUNCATED = -7,
    RF_MESH_PACKET_ERR_NETWORK = -8,
    RF_MESH_PACKET_ERR_RESERVED = -9,
    RF_MESH_PACKET_ERR_FLAGS = -10
} rf_mesh_packet_status_t;

typedef struct {
    uint8_t type;
    uint8_t flags;
    uint16_t network_id;
    uint16_t source_id;
    uint16_t destination_id;
    uint16_t sequence_id;
    uint8_t ttl;
    uint8_t hop_count;
    uint8_t metric;
    uint8_t payload_len;
    uint16_t previous_hop_id;
    uint16_t immediate_receiver_id;
    uint8_t payload[RF_MESH_MAX_PAYLOAD_LEN];
} rf_mesh_packet_t;

int rf_mesh_packet_type_is_valid(uint8_t type);
int rf_mesh_packet_type_can_emit_v1(uint8_t type);
size_t rf_mesh_packet_wire_len(const rf_mesh_packet_t *packet);
int rf_mesh_packet_pack(const rf_mesh_packet_t *packet,
                        uint8_t *buf,
                        size_t buf_len,
                        size_t *out_len);
int rf_mesh_packet_unpack(const uint8_t *buf,
                          size_t len,
                          uint16_t expected_network_id,
                          rf_mesh_packet_t *packet);
int rf_mesh_packet_is_for_receiver(const rf_mesh_packet_t *packet,
                                   uint16_t local_node_id);

#endif /* RF_MESH_PACKET_H */
