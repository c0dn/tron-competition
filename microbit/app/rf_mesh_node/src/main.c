/*
 * rf_mesh_node - RX-first root-aware/AODV-style dummy RF mesh target.
 *
 * This is a buildable firmware integration target for the project-owned RF mesh
 * draft. It intentionally makes no runtime RF-success claim: packets and
 * payloads are dummy/test-only, the radio is broadcast-style, and any next-hop
 * choice is protocol metadata for the routing core.
 */

#include <stddef.h>
#include <stdint.h>

/* rf_mesh_core uses the toolchain's size_t; ask μT-Kernel not to redefine it. */
#define PROHIBIT_DEF_SIZE_T 1

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "display.h"
#include "rf_mesh_config.h"
#include "rf_mesh_core.h"
#include "rf_mesh_packet.h"
#include "rf_radio.h"

#define FICR_DEVICEADDR0        0x100000A4UL
#define FICR_DEVICEADDR1        0x100000A8UL

#define RF_MESH_TASK_PRIORITY   10
#define RF_MESH_TASK_STACK      1536

static rf_mesh_state_t mesh;
static uint16_t local_node_id;
static uint32_t local_deliveries;
static uint32_t app_packet_drops;

static uint32_t now_ms(void)
{
    SYSTIM t;

    tk_get_otm(&t);
    return t.lo;
}

static int time_reached(uint32_t now, uint32_t due)
{
    return (uint32_t)(now - due) < 0x80000000UL;
}

static uint16_t configured_node_id(void)
{
    uint16_t id = (uint16_t)RF_MESH_NODE_ID;
    uint16_t root_id = (uint16_t)RF_MESH_ROOT_ID;

    if ((uint8_t)RF_MESH_IS_ROOT != 0u) {
        return root_id;
    }
    if (id != 0u) {
        return id;
    }

    id = (uint16_t)((in_w(FICR_DEVICEADDR0) ^ in_w(FICR_DEVICEADDR1)) & 0xffffu);
    if (id == 0u || id == RF_MESH_BROADCAST_ID || id == root_id) {
        id = (uint16_t)(0x8000u | (((uint16_t)RF_MESH_NETWORK_ID) & 0x7fffu));
    }
    if (id == 0u || id == RF_MESH_BROADCAST_ID || id == root_id) {
        id = (root_id != 0x8001u) ? 0x8001u : 0x8002u;
    }
    return id;
}

static uint32_t config_seed(uint16_t node_id)
{
    return in_w(FICR_DEVICEADDR0) ^
           (in_w(FICR_DEVICEADDR1) << 1) ^
           ((uint32_t)node_id << 16) ^
           (uint32_t)((uint16_t)RF_MESH_NETWORK_ID);
}

static void mesh_packet_base(rf_mesh_packet_t *packet, uint8_t type)
{
    size_t i;

    packet->type = type;
    packet->flags = 0;
    packet->network_id = (uint16_t)RF_MESH_NETWORK_ID;
    packet->source_id = local_node_id;
    packet->destination_id = (uint16_t)RF_MESH_ROOT_ID;
    packet->sequence_id = rf_mesh_next_sequence(&mesh);
    packet->ttl = RF_MESH_DEFAULT_TTL;
    packet->hop_count = 0;
    packet->metric = 0;
    packet->payload_len = 0;
    packet->previous_hop_id = local_node_id;
    packet->immediate_receiver_id = RF_MESH_BROADCAST_ID;
    for (i = 0; i < RF_MESH_MAX_PAYLOAD_LEN; i++) {
        packet->payload[i] = 0;
    }
}

static void build_hello(rf_mesh_packet_t *packet)
{
    mesh_packet_base(packet, RF_MESH_MSG_HELLO);
    packet->source_id = (uint16_t)RF_MESH_ROOT_ID;
    packet->destination_id = RF_MESH_BROADCAST_ID;
    packet->flags = RF_MESH_FLAG_ROOT | RF_MESH_FLAG_BROADCAST;
}

static void build_rreq(rf_mesh_packet_t *packet)
{
    mesh_packet_base(packet, RF_MESH_MSG_RREQ);
    packet->flags = RF_MESH_FLAG_BROADCAST;
}

static void build_rrep(rf_mesh_packet_t *packet, uint16_t requester_id)
{
    mesh_packet_base(packet, RF_MESH_MSG_RREP);
    packet->source_id = (uint16_t)RF_MESH_ROOT_ID;
    packet->destination_id = requester_id;
    packet->flags = RF_MESH_FLAG_ROUTE_REPLY;
}

static void build_dummy_data(rf_mesh_packet_t *packet, uint32_t t)
{
    uint16_t seq;

    mesh_packet_base(packet, RF_MESH_MSG_DATA);
    seq = packet->sequence_id;
    packet->payload_len = 8;
    packet->payload[0] = (uint8_t)'D';
    packet->payload[1] = (uint8_t)'U';
    packet->payload[2] = (uint8_t)'M';
    packet->payload[3] = (uint8_t)'Y';
    packet->payload[4] = (uint8_t)seq;
    packet->payload[5] = (uint8_t)(seq >> 8);
    packet->payload[6] = (uint8_t)t;
    packet->payload[7] = (uint8_t)(t >> 8);
}

static void enqueue_local(const rf_mesh_packet_t *packet,
                          uint16_t next_hop_id,
                          uint32_t due_ms)
{
    if (rf_mesh_enqueue(&mesh,
                        packet,
                        next_hop_id,
                        RF_MESH_TX_PRIORITY_LOCAL,
                        due_ms) != RF_MESH_OK) {
        app_packet_drops++;
    }
}

static void enqueue_rrep(const rf_mesh_packet_t *packet,
                         uint16_t next_hop_id,
                         uint32_t now)
{
    if (rf_mesh_enqueue_with_jitter(&mesh,
                                    packet,
                                    next_hop_id,
                                    RF_MESH_TX_PRIORITY_RREP,
                                    now,
                                    RF_MESH_DISCOVERY_JITTER_MIN_MS,
                                    RF_MESH_DISCOVERY_JITTER_MAX_MS) != RF_MESH_OK) {
        app_packet_drops++;
    }
}

static void schedule_relay_or_count(const rf_mesh_packet_t *packet,
                                    uint16_t next_hop_id,
                                    uint32_t now,
                                    uint32_t min_jitter,
                                    uint32_t max_jitter)
{
    if (rf_mesh_schedule_relay(&mesh,
                               packet,
                               next_hop_id,
                               now,
                               min_jitter,
                               max_jitter) != RF_MESH_OK) {
        app_packet_drops++;
    }
}

static void schedule_broadcast_fallback(const rf_mesh_packet_t *packet, uint32_t now)
{
    rf_mesh_packet_t relay;

    if (rf_mesh_build_relay_packet(&mesh, packet, &relay) != RF_MESH_OK) {
        app_packet_drops++;
        return;
    }
    relay.flags = (uint8_t)(relay.flags | RF_MESH_FLAG_BROADCAST);
    if (rf_mesh_enqueue_with_jitter(&mesh,
                                    &relay,
                                    RF_MESH_BROADCAST_ID,
                                    RF_MESH_TX_PRIORITY_RELAY,
                                    now,
                                    RF_MESH_RELAY_JITTER_MIN_MS,
                                    RF_MESH_RELAY_JITTER_MAX_MS) != RF_MESH_OK) {
        app_packet_drops++;
    }
}

static int learn_control_route(const rf_mesh_packet_t *packet, uint32_t now)
{
    if (packet->type == RF_MESH_MSG_HELLO ||
        packet->type == RF_MESH_MSG_RREQ ||
        packet->type == RF_MESH_MSG_RREP) {
        return rf_mesh_learn_from_packet(&mesh,
                                         packet,
                                         packet->previous_hop_id,
                                         now);
    }
    return RF_MESH_OK;
}

static int control_semantics_valid(const rf_mesh_packet_t *packet)
{
    if (packet->source_id == 0u ||
        packet->source_id == RF_MESH_BROADCAST_ID ||
        packet->previous_hop_id == 0u ||
        packet->previous_hop_id == RF_MESH_BROADCAST_ID) {
        return 0;
    }

    switch (packet->type) {
    case RF_MESH_MSG_HELLO:
        return packet->source_id == (uint16_t)RF_MESH_ROOT_ID &&
               packet->destination_id == RF_MESH_BROADCAST_ID &&
               (packet->flags & (RF_MESH_FLAG_ROOT | RF_MESH_FLAG_BROADCAST)) ==
                   (RF_MESH_FLAG_ROOT | RF_MESH_FLAG_BROADCAST);
    case RF_MESH_MSG_RREQ:
        return packet->source_id != (uint16_t)RF_MESH_ROOT_ID &&
               (packet->flags & RF_MESH_FLAG_BROADCAST) != 0u &&
               (packet->destination_id == (uint16_t)RF_MESH_ROOT_ID ||
                packet->destination_id == RF_MESH_BROADCAST_ID);
    case RF_MESH_MSG_RREP:
        if (packet->source_id != (uint16_t)RF_MESH_ROOT_ID ||
            (packet->flags & RF_MESH_FLAG_ROUTE_REPLY) == 0u) {
            return 0;
        }
        if (packet->destination_id == RF_MESH_BROADCAST_ID) {
            return (packet->flags & RF_MESH_FLAG_BROADCAST) != 0u;
        }
        return packet->destination_id != 0u &&
               packet->destination_id != (uint16_t)RF_MESH_ROOT_ID &&
               packet->destination_id != packet->source_id;
    case RF_MESH_MSG_DATA:
        if (packet->destination_id == RF_MESH_BROADCAST_ID ||
            (packet->flags & RF_MESH_FLAG_BROADCAST) != 0u) {
            return packet->destination_id == RF_MESH_BROADCAST_ID &&
                   (packet->flags & RF_MESH_FLAG_BROADCAST) != 0u;
        }
        return 1;
    default:
        return 0;
    }
}

static void handle_hello(const rf_mesh_packet_t *packet, uint32_t now)
{
    if ((uint8_t)RF_MESH_IS_ROOT != 0u) {
        return;
    }
    if (packet->source_id != (uint16_t)RF_MESH_ROOT_ID) {
        app_packet_drops++;
        return;
    }
    schedule_relay_or_count(packet,
                            RF_MESH_BROADCAST_ID,
                            now,
                            RF_MESH_RELAY_JITTER_MIN_MS,
                            RF_MESH_RELAY_JITTER_MAX_MS);
}

static void handle_rreq(const rf_mesh_packet_t *packet, uint32_t now)
{
    if ((uint8_t)RF_MESH_IS_ROOT != 0u &&
        (packet->destination_id == (uint16_t)RF_MESH_ROOT_ID ||
         packet->destination_id == RF_MESH_BROADCAST_ID)) {
        rf_mesh_packet_t rrep;
        const rf_mesh_route_t *route;
        uint16_t next_hop = packet->previous_hop_id;

        build_rrep(&rrep, packet->source_id);
        route = rf_mesh_route_lookup(&mesh, packet->source_id, now);
        if (route != NULL) {
            next_hop = route->next_hop_id;
        } else {
            rrep.flags = (uint8_t)(rrep.flags | RF_MESH_FLAG_BROADCAST);
            next_hop = RF_MESH_BROADCAST_ID;
        }
        enqueue_rrep(&rrep, next_hop, now);
        return;
    }

    schedule_relay_or_count(packet,
                            RF_MESH_BROADCAST_ID,
                            now,
                            RF_MESH_DISCOVERY_JITTER_MIN_MS,
                            RF_MESH_DISCOVERY_JITTER_MAX_MS);
}

static void handle_rrep(const rf_mesh_packet_t *packet, uint32_t now)
{
    const rf_mesh_route_t *route;

    if (packet->destination_id == local_node_id) {
        return;
    }

    if (packet->destination_id == RF_MESH_BROADCAST_ID) {
        schedule_relay_or_count(packet,
                                RF_MESH_BROADCAST_ID,
                                now,
                                RF_MESH_RELAY_JITTER_MIN_MS,
                                RF_MESH_RELAY_JITTER_MAX_MS);
        return;
    }

    route = rf_mesh_route_lookup(&mesh, packet->destination_id, now);
    if (route != NULL) {
        schedule_relay_or_count(packet,
                                route->next_hop_id,
                                now,
                                RF_MESH_RELAY_JITTER_MIN_MS,
                                RF_MESH_RELAY_JITTER_MAX_MS);
    } else {
        schedule_broadcast_fallback(packet, now);
    }
}

static void handle_data(const rf_mesh_packet_t *packet, uint32_t now)
{
    const rf_mesh_route_t *route;
    int broadcast_data = packet->destination_id == RF_MESH_BROADCAST_ID &&
                         (packet->flags & RF_MESH_FLAG_BROADCAST) != 0u;
    int addressed_here = packet->destination_id == local_node_id ||
                         broadcast_data ||
                         ((uint8_t)RF_MESH_IS_ROOT != 0u &&
                          packet->destination_id == (uint16_t)RF_MESH_ROOT_ID);

    if (addressed_here) {
        local_deliveries++;
    }

    if (packet->destination_id == local_node_id ||
        ((uint8_t)RF_MESH_IS_ROOT != 0u &&
         packet->destination_id == (uint16_t)RF_MESH_ROOT_ID)) {
        return;
    }

    if (broadcast_data) {
        schedule_relay_or_count(packet,
                                RF_MESH_BROADCAST_ID,
                                now,
                                RF_MESH_RELAY_JITTER_MIN_MS,
                                RF_MESH_RELAY_JITTER_MAX_MS);
        return;
    }

    route = rf_mesh_route_lookup(&mesh, packet->destination_id, now);
    if (route != NULL) {
        schedule_relay_or_count(packet,
                                route->next_hop_id,
                                now,
                                RF_MESH_RELAY_JITTER_MIN_MS,
                                RF_MESH_RELAY_JITTER_MAX_MS);
    } else {
        mesh.counters.no_route_drops++;
    }
}

static void handle_rx_packet(uint32_t now)
{
    UB raw[RF_RADIO_MAX_PACKET];
    UINT raw_len = 0;
    UINT rssi = 0;
    rf_mesh_packet_t packet;
    int rc;

    if (!rf_radio_poll(raw, &raw_len, &rssi)) {
        return;
    }

    rc = rf_mesh_packet_unpack(raw,
                               (size_t)raw_len,
                               (uint16_t)RF_MESH_NETWORK_ID,
                               &packet);
    if (rc != RF_MESH_PACKET_OK) {
        mesh.counters.packet_drops++;
        return;
    }

    if (packet.source_id == local_node_id || packet.previous_hop_id == local_node_id) {
        mesh.counters.packet_drops++;
        return;
    }

    if (!rf_mesh_packet_is_for_receiver(&packet, local_node_id)) {
        mesh.counters.packet_drops++;
        return;
    }

    mesh.counters.rx_packets++;
    if (!control_semantics_valid(&packet)) {
        mesh.counters.packet_drops++;
        return;
    }

    rc = rf_mesh_dedup_check_and_store(&mesh, &packet, now);
    if (rc != RF_MESH_OK) {
        return;
    }

    rc = learn_control_route(&packet, now);
    if (rc != RF_MESH_OK) {
        return;
    }

    switch (packet.type) {
    case RF_MESH_MSG_HELLO:
        handle_hello(&packet, now);
        break;
    case RF_MESH_MSG_RREQ:
        handle_rreq(&packet, now);
        break;
    case RF_MESH_MSG_RREP:
        handle_rrep(&packet, now);
        break;
    case RF_MESH_MSG_DATA:
        handle_data(&packet, now);
        break;
    default:
        mesh.counters.packet_drops++;
        break;
    }

    (void)rssi;
}

static void enqueue_periodic_work(uint32_t now,
                                  uint32_t *next_root_hello_ms,
                                  uint32_t *next_dummy_data_ms)
{
    rf_mesh_packet_t packet;

    if ((uint8_t)RF_MESH_IS_ROOT != 0u) {
        if (time_reached(now, *next_root_hello_ms)) {
            build_hello(&packet);
            enqueue_local(&packet, RF_MESH_BROADCAST_ID, now);
            *next_root_hello_ms = now + RF_MESH_APP_ROOT_BEACON_INTERVAL_MS;
        }
        return;
    }

    if (!time_reached(now, *next_dummy_data_ms)) {
        return;
    }

    if (rf_mesh_needs_root_discovery(&mesh, now)) {
        build_rreq(&packet);
        if (rf_mesh_enqueue_with_jitter(&mesh,
                                        &packet,
                                        RF_MESH_BROADCAST_ID,
                                        RF_MESH_TX_PRIORITY_LOCAL,
                                        now,
                                        RF_MESH_DISCOVERY_JITTER_MIN_MS,
                                        RF_MESH_DISCOVERY_JITTER_MAX_MS) != RF_MESH_OK) {
            app_packet_drops++;
        }
    } else {
        const rf_mesh_route_t *route = rf_mesh_route_lookup(&mesh,
                                                            (uint16_t)RF_MESH_ROOT_ID,
                                                            now);
        build_dummy_data(&packet, now);
        enqueue_local(&packet,
                      (route != NULL) ? route->next_hop_id : RF_MESH_BROADCAST_ID,
                      now);
    }
    *next_dummy_data_ms = now + RF_MESH_APP_DUMMY_DATA_INTERVAL_MS;
}

static void process_one_tx(uint32_t now)
{
    rf_mesh_tx_item_t item;
    UB raw[RF_RADIO_MAX_PACKET];
    size_t raw_len = 0;
    int rc;

    if (rf_mesh_dequeue_due(&mesh, now, &item) != RF_MESH_OK) {
        return;
    }

    item.packet.previous_hop_id = local_node_id;
    item.packet.immediate_receiver_id = item.next_hop_id;
    rc = rf_mesh_packet_pack(&item.packet, raw, sizeof(raw), &raw_len);
    if (rc != RF_MESH_PACKET_OK) {
        mesh.counters.packet_drops++;
        return;
    }

    if (!rf_radio_send(raw, (UINT)raw_len)) {
        mesh.counters.packet_drops++;
    }
    rf_radio_listen();
}

static void update_display(uint32_t now)
{
    UB rows[5] = { 0, 0, 0, 0, 0 };
    const rf_mesh_route_t *route;
    UINT level = 1;

    if ((uint8_t)RF_MESH_IS_ROOT != 0u) {
        rows[0] = 0x1F;
    }

    route = rf_mesh_route_lookup(&mesh, (uint16_t)RF_MESH_ROOT_ID, now);
    if (route != NULL) {
        level = (UINT)(5u - ((route->metric < 4u) ? route->metric : 4u));
        rows[2] = (UB)((1u << level) - 1u);
    } else {
        rows[2] = 0x04;
    }

    if (rf_mesh_queue_count(&mesh) != 0u) {
        rows[4] = 0x11;
    }
    display_set_rows(rows);
}

static void log_observability(uint32_t now, uint32_t *next_log_ms)
{
    const rf_mesh_counters_t *counters;
    const rf_mesh_route_t *root_route;
    UINT root_metric = 255;
    UINT root_next_hop = 0;

    if (!time_reached(now, *next_log_ms)) {
        return;
    }

    counters = rf_mesh_counters(&mesh);
    root_route = rf_mesh_route_lookup(&mesh, (uint16_t)RF_MESH_ROOT_ID, now);
    if (root_route != NULL) {
        root_metric = root_route->metric;
        root_next_hop = root_route->next_hop_id;
    }

    tm_printf((UB *)"mesh id=0x%04x root=%u q=%u rx=%lu tx_attempt=%lu dup=%lu drop=%lu relay_attempt=%lu route=%lu metric=%u next=0x%04x delivered=%lu appdrop=%lu\n",
              (UINT)local_node_id,
              (UINT)((uint8_t)RF_MESH_IS_ROOT != 0u),
              (UINT)rf_mesh_queue_count(&mesh),
              (UW)counters->rx_packets,
              (UW)counters->tx_packets,
              (UW)counters->duplicate_drops,
              (UW)(counters->packet_drops + counters->queue_drops +
                   counters->ttl_drops + counters->no_route_drops),
              (UW)counters->relay_packets,
              (UW)counters->route_updates,
              root_metric,
              root_next_hop,
              (UW)local_deliveries,
              (UW)app_packet_drops);
    update_display(now);
    *next_log_ms = now + RF_MESH_APP_OBSERVABILITY_INTERVAL_MS;
}

LOCAL void mesh_task(INT stacd, void *exinf)
{
    uint32_t next_root_hello_ms = now_ms();
    uint32_t next_dummy_data_ms = now_ms() + RF_MESH_APP_DUMMY_DATA_INTERVAL_MS;
    uint32_t next_log_ms = now_ms();

    (void)stacd;
    (void)exinf;

    while (1) {
        uint32_t now = now_ms();

        handle_rx_packet(now);
        enqueue_periodic_work(now, &next_root_hello_ms, &next_dummy_data_ms);
        process_one_tx(now);
        log_observability(now, &next_log_ms);

        tk_dly_tsk(RF_MESH_APP_IDLE_DELAY_MS);
    }
}

EXPORT INT usermain(void)
{
    rf_mesh_config_t mesh_config;
    rf_radio_config_t radio_config;
    T_CTSK ctsk = {
        .exinf   = NULL,
        .tskatr  = TA_HLNG | TA_RNG3,
        .task    = (FP)mesh_task,
        .itskpri = RF_MESH_TASK_PRIORITY,
        .stksz   = RF_MESH_TASK_STACK,
    };
    ID tskid;

    local_node_id = configured_node_id();

    rf_mesh_default_config(&mesh_config);
    mesh_config.node_id = local_node_id;
    mesh_config.network_id = (uint16_t)RF_MESH_NETWORK_ID;
    mesh_config.root_id = (uint16_t)RF_MESH_ROOT_ID;
    mesh_config.is_root = (uint8_t)((uint8_t)RF_MESH_IS_ROOT != 0u);
    mesh_config.prng_seed = config_seed(local_node_id);
    rf_mesh_init(&mesh, &mesh_config);

    radio_config.network_id = (UH)((uint16_t)RF_MESH_NETWORK_ID);
    radio_config.channel = (UINT)RF_MESH_RF_CHANNEL;
    radio_config.tx_power = (INT)RF_MESH_RF_TXPOWER;

    display_init();
    rf_radio_init(&radio_config);
    rf_radio_listen();

    tm_printf((UB *)"rf_mesh_node boot id=0x%04x root=%u net=0x%04x rf_channel=%u dummy-payload-only\n",
              (UINT)local_node_id,
              (UINT)mesh_config.is_root,
              (UINT)mesh_config.network_id,
              (UINT)radio_config.channel);

    tskid = tk_cre_tsk(&ctsk);
    if (tskid > 0) {
        tk_sta_tsk(tskid, 0);
    } else {
        tm_printf((UB *)"tk_cre_tsk failed: %d\n", tskid);
    }

    tk_slp_tsk(TMO_FEVR);
    return 0;
}
