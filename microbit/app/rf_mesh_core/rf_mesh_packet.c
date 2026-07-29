#include "rf_mesh_packet.h"

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void write_u16_le(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

int rf_mesh_packet_type_is_valid(uint8_t type)
{
    return type == RF_MESH_MSG_HELLO ||
           type == RF_MESH_MSG_RREQ ||
           type == RF_MESH_MSG_RREP ||
           type == RF_MESH_MSG_DATA ||
           type == RF_MESH_MSG_ACK;
}

int rf_mesh_packet_type_can_emit_v1(uint8_t type)
{
    return type == RF_MESH_MSG_HELLO ||
           type == RF_MESH_MSG_RREQ ||
           type == RF_MESH_MSG_RREP ||
           type == RF_MESH_MSG_DATA;
}

size_t rf_mesh_packet_wire_len(const rf_mesh_packet_t *packet)
{
    if (packet == 0 || packet->payload_len > RF_MESH_MAX_PAYLOAD_LEN) {
        return 0;
    }

    return (size_t)RF_MESH_HEADER_LEN + packet->payload_len;
}

int rf_mesh_packet_pack(const rf_mesh_packet_t *packet,
                        uint8_t *buf,
                        size_t buf_len,
                        size_t *out_len)
{
    size_t wire_len;
    size_t i;

    if (packet == 0 || buf == 0 || out_len == 0) {
        return RF_MESH_PACKET_ERR_NULL;
    }
    if (!rf_mesh_packet_type_can_emit_v1(packet->type)) {
        return RF_MESH_PACKET_ERR_TYPE;
    }
    if ((packet->flags & (uint8_t)~RF_MESH_KNOWN_FLAGS) != 0u) {
        return RF_MESH_PACKET_ERR_FLAGS;
    }

    wire_len = rf_mesh_packet_wire_len(packet);
    if (wire_len == 0) {
        return RF_MESH_PACKET_ERR_PAYLOAD_LEN;
    }
    if (wire_len > RF_MESH_MAX_PACKET_SIZE) {
        return RF_MESH_PACKET_ERR_PAYLOAD_LEN;
    }
    if (buf_len < wire_len) {
        return RF_MESH_PACKET_ERR_BUFFER_SMALL;
    }

    buf[0] = (uint8_t)((RF_MESH_PROTOCOL_VERSION << 4) |
                       (packet->type & 0x0fu));
    buf[1] = packet->flags;
    write_u16_le(&buf[2], packet->network_id);
    write_u16_le(&buf[4], packet->source_id);
    write_u16_le(&buf[6], packet->destination_id);
    write_u16_le(&buf[8], packet->sequence_id);
    buf[10] = packet->ttl;
    buf[11] = packet->hop_count;
    buf[12] = packet->metric;
    buf[13] = packet->payload_len;
    write_u16_le(&buf[14], packet->previous_hop_id);
    write_u16_le(&buf[16], packet->immediate_receiver_id);

    for (i = 0; i < packet->payload_len; i++) {
        buf[RF_MESH_HEADER_LEN + i] = packet->payload[i];
    }

    *out_len = wire_len;
    return RF_MESH_PACKET_OK;
}

int rf_mesh_packet_unpack(const uint8_t *buf,
                          size_t len,
                          uint16_t expected_network_id,
                          rf_mesh_packet_t *packet)
{
    uint8_t version;
    uint8_t type;
    uint8_t payload_len;
    size_t expected_len;
    size_t i;

    if (buf == 0 || packet == 0) {
        return RF_MESH_PACKET_ERR_NULL;
    }
    if (len < RF_MESH_HEADER_LEN) {
        return RF_MESH_PACKET_ERR_TRUNCATED;
    }
    if (len > RF_MESH_MAX_PACKET_SIZE) {
        return RF_MESH_PACKET_ERR_LENGTH;
    }

    version = (uint8_t)(buf[0] >> 4);
    type = (uint8_t)(buf[0] & 0x0fu);
    if (version != RF_MESH_PROTOCOL_VERSION) {
        return RF_MESH_PACKET_ERR_VERSION;
    }
    if (!rf_mesh_packet_type_is_valid(type)) {
        return RF_MESH_PACKET_ERR_TYPE;
    }

    payload_len = buf[13];
    if (payload_len > RF_MESH_MAX_PAYLOAD_LEN) {
        return RF_MESH_PACKET_ERR_PAYLOAD_LEN;
    }
    expected_len = (size_t)RF_MESH_HEADER_LEN + payload_len;
    if (len < expected_len) {
        return RF_MESH_PACKET_ERR_TRUNCATED;
    }
    if (len != expected_len) {
        return RF_MESH_PACKET_ERR_LENGTH;
    }

    if (read_u16_le(&buf[2]) != expected_network_id) {
        return RF_MESH_PACKET_ERR_NETWORK;
    }
    packet->type = type;
    packet->flags = (uint8_t)(buf[1] & RF_MESH_KNOWN_FLAGS);
    packet->network_id = read_u16_le(&buf[2]);
    packet->source_id = read_u16_le(&buf[4]);
    packet->destination_id = read_u16_le(&buf[6]);
    packet->sequence_id = read_u16_le(&buf[8]);
    packet->ttl = buf[10];
    packet->hop_count = buf[11];
    packet->metric = buf[12];
    packet->payload_len = payload_len;
    packet->previous_hop_id = read_u16_le(&buf[14]);
    packet->immediate_receiver_id = read_u16_le(&buf[16]);

    for (i = 0; i < RF_MESH_MAX_PAYLOAD_LEN; i++) {
        packet->payload[i] = 0;
    }
    for (i = 0; i < payload_len; i++) {
        packet->payload[i] = buf[RF_MESH_HEADER_LEN + i];
    }

    return RF_MESH_PACKET_OK;
}

int rf_mesh_packet_is_for_receiver(const rf_mesh_packet_t *packet,
                                   uint16_t local_node_id)
{
    return packet != 0 &&
           (packet->immediate_receiver_id == local_node_id ||
            packet->immediate_receiver_id == RF_MESH_BROADCAST_ID);
}
