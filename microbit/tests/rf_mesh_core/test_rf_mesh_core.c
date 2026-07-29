#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "rf_mesh_core.h"
#include "rf_mesh_packet.h"

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static rf_mesh_packet_t make_packet(uint8_t type,
                                    uint16_t source_id,
                                    uint16_t destination_id,
                                    uint16_t sequence_id)
{
    rf_mesh_packet_t packet = { 0 };

    packet.type = type;
    packet.flags = 0;
    packet.network_id = RF_MESH_DEFAULT_NETWORK_ID;
    packet.source_id = source_id;
    packet.destination_id = destination_id;
    packet.sequence_id = sequence_id;
    packet.ttl = RF_MESH_DEFAULT_TTL;
    packet.hop_count = 0;
    packet.metric = 0;
    packet.payload_len = 0;
    packet.previous_hop_id = source_id;
    packet.immediate_receiver_id = RF_MESH_BROADCAST_ID;
    return packet;
}

static void init_mesh(rf_mesh_state_t *mesh)
{
    rf_mesh_config_t config;

    rf_mesh_default_config(&config);
    config.node_id = 0x1234u;
    config.root_id = RF_MESH_DEFAULT_ROOT_ID;
    config.network_id = RF_MESH_DEFAULT_NETWORK_ID;
    config.prng_seed = 1u;
    rf_mesh_init(mesh, &config);
}

static void test_packet_pack_unpack(void)
{
    rf_mesh_packet_t packet = make_packet(RF_MESH_MSG_DATA,
                                          0x1001u,
                                          RF_MESH_DEFAULT_ROOT_ID,
                                          0x2222u);
    rf_mesh_packet_t decoded;
    uint8_t buf[RF_MESH_MAX_PACKET_SIZE];
    size_t len = 0;
    int rc;

    packet.flags = RF_MESH_FLAG_RELAYED;
    packet.ttl = 4;
    packet.hop_count = 1;
    packet.metric = 2;
    packet.previous_hop_id = 0x3002u;
    packet.immediate_receiver_id = 0x4003u;
    packet.payload_len = 3;
    packet.payload[0] = 0xa1u;
    packet.payload[1] = 0xb2u;
    packet.payload[2] = 0xc3u;

    rc = rf_mesh_packet_pack(&packet, buf, sizeof(buf), &len);
    CHECK(rc == RF_MESH_PACKET_OK);
    CHECK(len == RF_MESH_HEADER_LEN + 3u);
    CHECK(buf[0] == (uint8_t)((RF_MESH_PROTOCOL_VERSION << 4) | RF_MESH_MSG_DATA));
    CHECK(buf[2] == 0x52u);
    CHECK(buf[3] == 0x54u);
    CHECK(buf[14] == 0x02u);
    CHECK(buf[15] == 0x30u);
    CHECK(buf[16] == 0x03u);
    CHECK(buf[17] == 0x40u);

    rc = rf_mesh_packet_unpack(buf, len, RF_MESH_DEFAULT_NETWORK_ID, &decoded);
    CHECK(rc == RF_MESH_PACKET_OK);
    CHECK(decoded.type == packet.type);
    CHECK(decoded.flags == packet.flags);
    CHECK(decoded.network_id == packet.network_id);
    CHECK(decoded.source_id == packet.source_id);
    CHECK(decoded.destination_id == packet.destination_id);
    CHECK(decoded.sequence_id == packet.sequence_id);
    CHECK(decoded.ttl == packet.ttl);
    CHECK(decoded.hop_count == packet.hop_count);
    CHECK(decoded.metric == packet.metric);
    CHECK(decoded.payload_len == packet.payload_len);
    CHECK(decoded.previous_hop_id == packet.previous_hop_id);
    CHECK(decoded.immediate_receiver_id == packet.immediate_receiver_id);
    CHECK(decoded.payload[0] == packet.payload[0]);
    CHECK(decoded.payload[1] == packet.payload[1]);
    CHECK(decoded.payload[2] == packet.payload[2]);
}

static void test_immediate_receiver_admission(void)
{
    rf_mesh_packet_t packet = make_packet(RF_MESH_MSG_DATA,
                                          0x1001u,
                                          RF_MESH_DEFAULT_ROOT_ID,
                                          0x2222u);

    packet.immediate_receiver_id = 0x2002u;
    CHECK(rf_mesh_packet_is_for_receiver(&packet, 0x2002u));
    CHECK(!rf_mesh_packet_is_for_receiver(&packet, 0x2003u));
    packet.immediate_receiver_id = RF_MESH_BROADCAST_ID;
    CHECK(rf_mesh_packet_is_for_receiver(&packet, 0x2003u));
    CHECK(!rf_mesh_packet_is_for_receiver(0, 0x2003u));
}

static void test_packet_boundaries_and_rejects(void)
{
    rf_mesh_packet_t packet = make_packet(RF_MESH_MSG_DATA,
                                          0x1001u,
                                          RF_MESH_BROADCAST_ID,
                                          0x0001u);
    rf_mesh_packet_t decoded;
    uint8_t buf[RF_MESH_MAX_PACKET_SIZE];
    size_t len = 0;
    size_t i;
    int rc;

    packet.flags = RF_MESH_FLAG_BROADCAST;
    packet.payload_len = RF_MESH_MAX_PAYLOAD_LEN;
    for (i = 0; i < RF_MESH_MAX_PAYLOAD_LEN; i++) {
        packet.payload[i] = (uint8_t)i;
    }
    rc = rf_mesh_packet_pack(&packet, buf, sizeof(buf), &len);
    CHECK(rc == RF_MESH_PACKET_OK);
    CHECK(len == RF_MESH_MAX_PACKET_SIZE);
    rc = rf_mesh_packet_unpack(buf, len, RF_MESH_DEFAULT_NETWORK_ID, &decoded);
    CHECK(rc == RF_MESH_PACKET_OK);
    CHECK(decoded.payload_len == RF_MESH_MAX_PAYLOAD_LEN);
    CHECK(decoded.payload[RF_MESH_MAX_PAYLOAD_LEN - 1u] ==
          packet.payload[RF_MESH_MAX_PAYLOAD_LEN - 1u]);

    packet.payload_len = RF_MESH_MAX_PAYLOAD_LEN + 1u;
    rc = rf_mesh_packet_pack(&packet, buf, sizeof(buf), &len);
    CHECK(rc == RF_MESH_PACKET_ERR_PAYLOAD_LEN);

    packet.payload_len = 0;
    packet.flags = 0x80u;
    rc = rf_mesh_packet_pack(&packet, buf, sizeof(buf), &len);
    CHECK(rc == RF_MESH_PACKET_ERR_FLAGS);

    packet.flags = 0;
    rc = rf_mesh_packet_pack(&packet, buf, sizeof(buf), &len);
    CHECK(rc == RF_MESH_PACKET_OK);
    buf[0] = (uint8_t)(((RF_MESH_PROTOCOL_VERSION + 1u) << 4) | RF_MESH_MSG_DATA);
    CHECK(rf_mesh_packet_unpack(buf, len, RF_MESH_DEFAULT_NETWORK_ID, &decoded) ==
          RF_MESH_PACKET_ERR_VERSION);

    rc = rf_mesh_packet_pack(&packet, buf, sizeof(buf), &len);
    CHECK(rc == RF_MESH_PACKET_OK);
    buf[0] = (uint8_t)((RF_MESH_PROTOCOL_VERSION << 4) | 0x0fu);
    CHECK(rf_mesh_packet_unpack(buf, len, RF_MESH_DEFAULT_NETWORK_ID, &decoded) ==
          RF_MESH_PACKET_ERR_TYPE);

    rc = rf_mesh_packet_pack(&packet, buf, sizeof(buf), &len);
    CHECK(rc == RF_MESH_PACKET_OK);
    buf[13] = 4u;
    CHECK(rf_mesh_packet_unpack(buf, RF_MESH_HEADER_LEN + 3u,
                                RF_MESH_DEFAULT_NETWORK_ID, &decoded) ==
          RF_MESH_PACKET_ERR_TRUNCATED);

    rc = rf_mesh_packet_pack(&packet, buf, sizeof(buf), &len);
    CHECK(rc == RF_MESH_PACKET_OK);
    CHECK(rf_mesh_packet_unpack(buf, len + 1u,
                                RF_MESH_DEFAULT_NETWORK_ID, &decoded) ==
          RF_MESH_PACKET_ERR_LENGTH);

    rc = rf_mesh_packet_pack(&packet, buf, sizeof(buf), &len);
    CHECK(rc == RF_MESH_PACKET_OK);
    CHECK(rf_mesh_packet_unpack(buf, len,
                                (uint16_t)(RF_MESH_DEFAULT_NETWORK_ID + 1u),
                                &decoded) == RF_MESH_PACKET_ERR_NETWORK);

    rc = rf_mesh_packet_pack(&packet, buf, sizeof(buf), &len);
    CHECK(rc == RF_MESH_PACKET_OK);
    buf[1] = (uint8_t)(RF_MESH_FLAG_BROADCAST | 0x80u);
    CHECK(rf_mesh_packet_unpack(buf, len, RF_MESH_DEFAULT_NETWORK_ID, &decoded) ==
          RF_MESH_PACKET_OK);
    CHECK(decoded.flags == RF_MESH_FLAG_BROADCAST);
}

static void test_dedup_and_ttl(void)
{
    rf_mesh_state_t mesh;
    rf_mesh_packet_t packet = make_packet(RF_MESH_MSG_DATA,
                                          0x2001u,
                                          RF_MESH_BROADCAST_ID,
                                          0x0101u);
    rf_mesh_packet_t relay;
    int rc;

    init_mesh(&mesh);
    packet.flags = RF_MESH_FLAG_BROADCAST;

    rc = rf_mesh_dedup_check_and_store(&mesh, &packet, 100u);
    CHECK(rc == RF_MESH_OK);
    packet.previous_hop_id = 0x9999u;
    rc = rf_mesh_dedup_check_and_store(&mesh, &packet, 101u);
    CHECK(rc == RF_MESH_ERR_DUPLICATE);
    CHECK(mesh.counters.duplicate_drops == 1u);
    rc = rf_mesh_dedup_check_and_store(&mesh, &packet,
                                       100u + RF_MESH_DEDUP_EXPIRY_MS);
    CHECK(rc == RF_MESH_OK);

    packet.ttl = 2;
    packet.hop_count = 4;
    packet.metric = 5;
    rc = rf_mesh_build_relay_packet(&mesh, &packet, &relay);
    CHECK(rc == RF_MESH_OK);
    CHECK(relay.ttl == 1u);
    CHECK(relay.hop_count == 5u);
    CHECK(relay.metric == 6u);
    CHECK(relay.previous_hop_id == mesh.config.node_id);
    CHECK((relay.flags & RF_MESH_FLAG_RELAYED) != 0u);
    CHECK(mesh.counters.relay_packets == 1u);

    packet.ttl = 1;
    rc = rf_mesh_build_relay_packet(&mesh, &packet, &relay);
    CHECK(rc == RF_MESH_ERR_TTL_EXPIRED);
    CHECK(mesh.counters.ttl_drops == 1u);

    init_mesh(&mesh);
    packet = make_packet(RF_MESH_MSG_DATA, 0x2100u,
                         RF_MESH_BROADCAST_ID, 0x0100u);
    packet.flags = RF_MESH_FLAG_BROADCAST;
    for (uint16_t i = 0; i < RF_MESH_DEDUP_ENTRIES; i++) {
        packet.source_id = (uint16_t)(0x2100u + i);
        packet.sequence_id = (uint16_t)(0x0100u + i);
        CHECK(rf_mesh_dedup_check_and_store(&mesh, &packet, 500u) == RF_MESH_OK);
    }
    packet.source_id = 0x3300u;
    packet.sequence_id = 0x4400u;
    CHECK(rf_mesh_dedup_check_and_store(&mesh, &packet, 501u) == RF_MESH_ERR_FULL);
    CHECK(mesh.counters.packet_drops == 1u);
    packet.source_id = 0x2100u;
    packet.sequence_id = 0x0100u;
    CHECK(rf_mesh_dedup_check_and_store(&mesh, &packet, 502u) ==
          RF_MESH_ERR_DUPLICATE);
}

static void test_routes(void)
{
    rf_mesh_state_t mesh;
    rf_mesh_packet_t hello = make_packet(RF_MESH_MSG_HELLO,
                                         RF_MESH_DEFAULT_ROOT_ID,
                                         RF_MESH_BROADCAST_ID,
                                         20u);
    const rf_mesh_route_t *route;
    int rc;

    init_mesh(&mesh);
    CHECK(rf_mesh_needs_root_discovery(&mesh, 0u) != 0);

    rc = rf_mesh_route_update(&mesh, RF_MESH_DEFAULT_ROOT_ID,
                              0x3001u, 2u, 2u, 10u, 100u);
    CHECK(rc == RF_MESH_OK);
    route = rf_mesh_route_lookup(&mesh, RF_MESH_DEFAULT_ROOT_ID, 100u);
    CHECK(route != 0);
    if (route != 0) {
        CHECK(route->next_hop_id == 0x3001u);
        CHECK(route->metric == 2u);
    }
    CHECK(rf_mesh_needs_root_discovery(&mesh, 100u) == 0);

    rc = rf_mesh_route_update(&mesh, RF_MESH_DEFAULT_ROOT_ID,
                              0x3002u, 4u, 5u, 10u, 101u);
    CHECK(rc == RF_MESH_ERR_REJECTED);
    route = rf_mesh_route_lookup(&mesh, RF_MESH_DEFAULT_ROOT_ID, 101u);
    CHECK(route != 0);
    if (route != 0) {
        CHECK(route->next_hop_id == 0x3001u);
    }

    rc = rf_mesh_route_update(&mesh, RF_MESH_DEFAULT_ROOT_ID,
                              0x3003u, 3u, 1u, 10u, 102u);
    CHECK(rc == RF_MESH_OK);
    route = rf_mesh_route_lookup(&mesh, RF_MESH_DEFAULT_ROOT_ID, 102u);
    CHECK(route != 0);
    if (route != 0) {
        CHECK(route->next_hop_id == 0x3003u);
        CHECK(route->metric == 1u);
    }

    rc = rf_mesh_route_update(&mesh, RF_MESH_DEFAULT_ROOT_ID,
                              0x3004u, 8u, 8u, 11u, 103u);
    CHECK(rc == RF_MESH_OK);
    route = rf_mesh_route_lookup(&mesh, RF_MESH_DEFAULT_ROOT_ID, 103u);
    CHECK(route != 0);
    if (route != 0) {
        CHECK(route->next_hop_id == 0x3004u);
        CHECK(route->sequence_id == 11u);
    }

    rc = rf_mesh_route_update(&mesh, RF_MESH_DEFAULT_ROOT_ID,
                              0x3005u, 1u, 1u, 9u, 104u);
    CHECK(rc == RF_MESH_ERR_REJECTED);

    rc = rf_mesh_route_update(&mesh, RF_MESH_DEFAULT_ROOT_ID,
                              0x3006u, 1u, 1u, 0xfffeu, 105u);
    CHECK(rc == RF_MESH_ERR_REJECTED);
    rc = rf_mesh_route_update(&mesh, RF_MESH_DEFAULT_ROOT_ID,
                              0x3007u, 1u, 1u, 1u, 106u);
    CHECK(rc == RF_MESH_ERR_REJECTED);

    route = rf_mesh_route_lookup(&mesh, RF_MESH_DEFAULT_ROOT_ID,
                                 103u + RF_MESH_ROUTE_EXPIRY_MS);
    CHECK(route == 0);
    CHECK(rf_mesh_needs_root_discovery(&mesh,
                                       103u + RF_MESH_ROUTE_EXPIRY_MS) != 0);

    hello.flags = RF_MESH_FLAG_ROOT | RF_MESH_FLAG_BROADCAST;
    hello.destination_id = RF_MESH_BROADCAST_ID;
    hello.hop_count = 0;
    hello.metric = 0;
    rc = rf_mesh_learn_from_packet(&mesh, &hello, 0x4444u,
                                   200u + RF_MESH_ROUTE_EXPIRY_MS);
    CHECK(rc == RF_MESH_OK);
    route = rf_mesh_route_lookup(&mesh, RF_MESH_DEFAULT_ROOT_ID,
                                 200u + RF_MESH_ROUTE_EXPIRY_MS);
    CHECK(route != 0);
    if (route != 0) {
        CHECK(route->next_hop_id == 0x4444u);
        CHECK(route->hop_count == 1u);
        CHECK(route->metric == 1u);
    }

    hello.destination_id = RF_MESH_DEFAULT_ROOT_ID;
    rc = rf_mesh_learn_from_packet(&mesh, &hello, 0x4444u,
                                   201u + RF_MESH_ROUTE_EXPIRY_MS);
    CHECK(rc == RF_MESH_ERR_INVALID);
    hello.destination_id = RF_MESH_BROADCAST_ID;
    rc = rf_mesh_learn_from_packet(&mesh, &hello, 0u,
                                   202u + RF_MESH_ROUTE_EXPIRY_MS);
    CHECK(rc == RF_MESH_ERR_INVALID);
    rc = rf_mesh_learn_from_packet(&mesh, &hello, RF_MESH_BROADCAST_ID,
                                   203u + RF_MESH_ROUTE_EXPIRY_MS);
    CHECK(rc == RF_MESH_ERR_INVALID);

    {
        rf_mesh_packet_t rreq = make_packet(RF_MESH_MSG_RREQ,
                                            0x2222u,
                                            RF_MESH_DEFAULT_ROOT_ID,
                                            30u);
        rreq.flags = RF_MESH_FLAG_BROADCAST;
        rc = rf_mesh_learn_from_packet(&mesh, &rreq, 0x5555u,
                                       300u + RF_MESH_ROUTE_EXPIRY_MS);
        CHECK(rc == RF_MESH_OK);
        route = rf_mesh_route_lookup(&mesh, 0x2222u,
                                     300u + RF_MESH_ROUTE_EXPIRY_MS);
        CHECK(route != 0);
        if (route != 0) {
            CHECK(route->next_hop_id == 0x5555u);
        }

        rreq.flags = 0;
        rreq.sequence_id = 31u;
        rc = rf_mesh_learn_from_packet(&mesh, &rreq, 0x5555u,
                                       301u + RF_MESH_ROUTE_EXPIRY_MS);
        CHECK(rc == RF_MESH_ERR_INVALID);

        rreq.flags = RF_MESH_FLAG_BROADCAST;
        rreq.destination_id = 0x9999u;
        rreq.sequence_id = 32u;
        rc = rf_mesh_learn_from_packet(&mesh, &rreq, 0x5555u,
                                       302u + RF_MESH_ROUTE_EXPIRY_MS);
        CHECK(rc == RF_MESH_ERR_INVALID);

        rreq.source_id = RF_MESH_DEFAULT_ROOT_ID;
        rreq.destination_id = RF_MESH_DEFAULT_ROOT_ID;
        rreq.sequence_id = 33u;
        route = rf_mesh_route_lookup(&mesh, RF_MESH_DEFAULT_ROOT_ID,
                                     303u + RF_MESH_ROUTE_EXPIRY_MS);
        CHECK(route != 0);
        if (route != 0) {
            CHECK(route->next_hop_id != 0x7777u);
        }
        rc = rf_mesh_learn_from_packet(&mesh, &rreq, 0x7777u,
                                       303u + RF_MESH_ROUTE_EXPIRY_MS);
        CHECK(rc == RF_MESH_ERR_INVALID);
        route = rf_mesh_route_lookup(&mesh, RF_MESH_DEFAULT_ROOT_ID,
                                     303u + RF_MESH_ROUTE_EXPIRY_MS);
        CHECK(route != 0);
        if (route != 0) {
            CHECK(route->next_hop_id != 0x7777u);
        }
    }

    {
        rf_mesh_packet_t rrep = make_packet(RF_MESH_MSG_RREP,
                                            0x9999u,
                                            0x2222u,
                                            40u);
        rrep.flags = RF_MESH_FLAG_ROUTE_REPLY;
        rc = rf_mesh_learn_from_packet(&mesh, &rrep, 0x6666u,
                                       400u + RF_MESH_ROUTE_EXPIRY_MS);
        CHECK(rc == RF_MESH_ERR_INVALID);
        rrep.source_id = RF_MESH_DEFAULT_ROOT_ID;
        rc = rf_mesh_learn_from_packet(&mesh, &rrep, 0x6666u,
                                       401u + RF_MESH_ROUTE_EXPIRY_MS);
        CHECK(rc == RF_MESH_OK);

        rrep.destination_id = 0u;
        rrep.sequence_id = 41u;
        CHECK(rf_mesh_learn_from_packet(&mesh, &rrep, 0x6666u,
                                        402u + RF_MESH_ROUTE_EXPIRY_MS) ==
              RF_MESH_ERR_INVALID);
        rrep.destination_id = RF_MESH_DEFAULT_ROOT_ID;
        rrep.sequence_id = 42u;
        CHECK(rf_mesh_learn_from_packet(&mesh, &rrep, 0x6666u,
                                        403u + RF_MESH_ROUTE_EXPIRY_MS) ==
              RF_MESH_ERR_INVALID);
        rrep.destination_id = RF_MESH_BROADCAST_ID;
        rrep.flags = RF_MESH_FLAG_ROUTE_REPLY;
        rrep.sequence_id = 43u;
        CHECK(rf_mesh_learn_from_packet(&mesh, &rrep, 0x6666u,
                                        404u + RF_MESH_ROUTE_EXPIRY_MS) ==
              RF_MESH_ERR_INVALID);
        rrep.flags = RF_MESH_FLAG_ROUTE_REPLY | RF_MESH_FLAG_BROADCAST;
        rrep.sequence_id = 44u;
        CHECK(rf_mesh_learn_from_packet(&mesh, &rrep, 0x6666u,
                                        405u + RF_MESH_ROUTE_EXPIRY_MS) ==
              RF_MESH_OK);
    }
}

static void test_sequence_wrap_route_update(void)
{
    rf_mesh_state_t mesh;
    const rf_mesh_route_t *route;
    int rc;

    init_mesh(&mesh);
    rc = rf_mesh_route_update(&mesh, 0x7777u, 0x1000u,
                              3u, 3u, 0xfffeu, 10u);
    CHECK(rc == RF_MESH_OK);
    rc = rf_mesh_route_update(&mesh, 0x7777u, 0x1001u,
                              9u, 9u, 1u, 11u);
    CHECK(rc == RF_MESH_OK);
    route = rf_mesh_route_lookup(&mesh, 0x7777u, 11u);
    CHECK(route != 0);
    if (route != 0) {
        CHECK(route->next_hop_id == 0x1001u);
        CHECK(route->sequence_id == 1u);
    }
}

static void test_queue_and_backoff(void)
{
    rf_mesh_state_t mesh;
    rf_mesh_packet_t packet = make_packet(RF_MESH_MSG_DATA,
                                          0x2001u,
                                          RF_MESH_DEFAULT_ROOT_ID,
                                          1u);
    rf_mesh_tx_item_t item;
    uint32_t jitter;
    size_t i;
    int rc;

    init_mesh(&mesh);
    jitter = rf_mesh_next_jitter_ms(&mesh,
                                    RF_MESH_RELAY_JITTER_MIN_MS,
                                    RF_MESH_RELAY_JITTER_MAX_MS);
    CHECK(jitter >= RF_MESH_RELAY_JITTER_MIN_MS);
    CHECK(jitter <= RF_MESH_RELAY_JITTER_MAX_MS);
    jitter = rf_mesh_next_jitter_ms(&mesh, 0u, UINT32_MAX);
    CHECK(jitter <= UINT32_MAX);

    CHECK(rf_mesh_dequeue_due(&mesh, 99u, &item) == RF_MESH_ERR_EMPTY);
    rc = rf_mesh_enqueue(&mesh, &packet, 0x3001u,
                         RF_MESH_TX_PRIORITY_RELAY, 100u);
    CHECK(rc == RF_MESH_OK);
    packet.sequence_id = 2u;
    rc = rf_mesh_enqueue(&mesh, &packet, 0x3002u,
                         RF_MESH_TX_PRIORITY_LOCAL, 100u);
    CHECK(rc == RF_MESH_OK);
    packet.type = RF_MESH_MSG_RREP;
    packet.flags = RF_MESH_FLAG_ROUTE_REPLY;
    packet.sequence_id = 3u;
    rc = rf_mesh_enqueue(&mesh, &packet, 0x3003u,
                         RF_MESH_TX_PRIORITY_RREP, 100u);
    CHECK(rc == RF_MESH_OK);

    CHECK(rf_mesh_dequeue_due(&mesh, 99u, &item) == RF_MESH_ERR_EMPTY);
    rc = rf_mesh_dequeue_due(&mesh, 100u, &item);
    CHECK(rc == RF_MESH_OK);
    CHECK(item.priority == RF_MESH_TX_PRIORITY_RREP);
    rc = rf_mesh_dequeue_due(&mesh, 100u, &item);
    CHECK(rc == RF_MESH_OK);
    CHECK(item.priority == RF_MESH_TX_PRIORITY_LOCAL);
    rc = rf_mesh_dequeue_due(&mesh, 100u, &item);
    CHECK(rc == RF_MESH_OK);
    CHECK(item.priority == RF_MESH_TX_PRIORITY_RELAY);
    CHECK(mesh.counters.tx_packets == 3u);

    init_mesh(&mesh);
    packet = make_packet(RF_MESH_MSG_DATA, 0x2001u,
                         RF_MESH_DEFAULT_ROOT_ID, 10u);
    for (i = 0; i < RF_MESH_TX_QUEUE_ENTRIES; i++) {
        packet.sequence_id = (uint16_t)(10u + i);
        CHECK(rf_mesh_enqueue(&mesh, &packet, 0x4000u,
                              RF_MESH_TX_PRIORITY_RELAY, 100u) == RF_MESH_OK);
    }
    CHECK(rf_mesh_queue_count(&mesh) == RF_MESH_TX_QUEUE_ENTRIES);
    packet.sequence_id = 99u;
    CHECK(rf_mesh_enqueue(&mesh, &packet, 0x4001u,
                          RF_MESH_TX_PRIORITY_LOCAL, 100u) == RF_MESH_OK);
    CHECK(rf_mesh_queue_count(&mesh) == RF_MESH_TX_QUEUE_ENTRIES);
    CHECK(mesh.counters.queue_drops == 1u);

    init_mesh(&mesh);
    for (i = 0; i < RF_MESH_TX_QUEUE_ENTRIES; i++) {
        packet.sequence_id = (uint16_t)(20u + i);
        CHECK(rf_mesh_enqueue(&mesh, &packet, 0x5000u,
                              RF_MESH_TX_PRIORITY_LOCAL, 100u) == RF_MESH_OK);
    }
    packet.sequence_id = 200u;
    CHECK(rf_mesh_enqueue(&mesh, &packet, 0x5001u,
                          RF_MESH_TX_PRIORITY_RELAY, 100u) == RF_MESH_ERR_FULL);
    CHECK(mesh.counters.queue_drops == 1u);

    packet.type = RF_MESH_MSG_ACK;
    CHECK(rf_mesh_enqueue(&mesh, &packet, 0x5001u,
                          RF_MESH_TX_PRIORITY_LOCAL, 100u) == RF_MESH_ERR_INVALID);
    packet.type = RF_MESH_MSG_DATA;
    packet.flags = 0x80u;
    CHECK(rf_mesh_enqueue(&mesh, &packet, 0x5001u,
                          RF_MESH_TX_PRIORITY_LOCAL, 100u) == RF_MESH_ERR_INVALID);
}

static void test_schedule_relay(void)
{
    rf_mesh_state_t mesh;
    rf_mesh_packet_t packet = make_packet(RF_MESH_MSG_HELLO,
                                          RF_MESH_DEFAULT_ROOT_ID,
                                          RF_MESH_BROADCAST_ID,
                                          1u);
    rf_mesh_tx_item_t item;
    int rc;

    init_mesh(&mesh);
    packet.flags = RF_MESH_FLAG_ROOT | RF_MESH_FLAG_BROADCAST;
    packet.ttl = 3u;
    rc = rf_mesh_schedule_relay(&mesh, &packet, RF_MESH_BROADCAST_ID,
                                1000u,
                                RF_MESH_RELAY_JITTER_MIN_MS,
                                RF_MESH_RELAY_JITTER_MAX_MS);
    CHECK(rc == RF_MESH_OK);
    CHECK(rf_mesh_queue_count(&mesh) == 1u);
    CHECK(rf_mesh_dequeue_due(&mesh,
                              1000u + RF_MESH_RELAY_JITTER_MAX_MS,
                              &item) == RF_MESH_OK);
    CHECK(item.packet.ttl == 2u);
    CHECK((item.packet.flags & RF_MESH_FLAG_RELAYED) != 0u);
}

int main(void)
{
    test_packet_pack_unpack();
    test_immediate_receiver_admission();
    test_packet_boundaries_and_rejects();
    test_dedup_and_ttl();
    test_routes();
    test_sequence_wrap_route_update();
    test_queue_and_backoff();
    test_schedule_relay();

    if (failures != 0) {
        printf("rf_mesh_core tests failed: %d\n", failures);
        return 1;
    }

    printf("rf_mesh_core tests passed\n");
    return 0;
}
