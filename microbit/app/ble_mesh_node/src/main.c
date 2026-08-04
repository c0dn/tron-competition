/*
 * ble_mesh_node - experimental TRON BLE advertising mesh-like node.
 *
 * This firmware passively listens most of the time and interleaves bounded
 * legacy-advertising TX windows through ble_mesh_scheduler. It is explicitly
 * not Bluetooth Mesh compliant: there is no provisioning, mesh security, IV
 * index, replay protection, SAR, GATT/PB-GATT, or standard mesh model layer.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include <stdint.h>

#include "ble_mesh_scheduler.h"
#include "ble_radio.h"
#include "tron_mesh_dedupe.h"
#include "tron_mesh_packet.h"
#include "tron_mesh_pingpong.h"

#define FICR_DEVICEADDR0          0x100000A4UL

/* Defined by CMake; 0 means "derive the id from FICR". Kept here so the file
   still compiles if it is built outside the project's CMake setup. */
#ifndef TRON_NODE_ID
#define TRON_NODE_ID              0
#endif

/* Test-only. Emulates being out of radio range of one specific peer so a
   multi-hop path can be forced on a desk where every board hears every other.
   0 disables. See the drop rule in handle_packet() for why this keys on TTL. */
#ifndef TRON_NODE_BLOCK_DIRECT_PEER_ID
#define TRON_NODE_BLOCK_DIRECT_PEER_ID 0
#endif

#define TRON_NODE_NET_ID          TRON_MESH_PINGPONG_NET_ID
#define TRON_NODE_OWN_TTL         TRON_MESH_TTL_MAX
#define TRON_NODE_DUMMY_INTERVAL_MS 1000u
#define TRON_NODE_STATS_MS        5000u
#define TRON_NODE_RELAY_MIN_MS    20u
#define TRON_NODE_RELAY_MAX_MS    120u
#define TRON_NODE_LOOP_DELAY_MS   2u

typedef struct tron_node_counters {
    uint32_t rx_ok;
    uint32_t decode_error;
    uint32_t duplicate_drop;
    uint32_t ttl_drop;
    uint32_t queue_drop;
    uint32_t relay_scheduled;
    uint32_t blocked_direct;
    uint32_t semantic_drop;
    uint32_t ping_queued;
    uint32_t ping_queue_fail;
    uint32_t ping_received;
    uint32_t pong_queued;
    uint32_t pong_queue_fail;
    uint32_t pong_matched;
    uint32_t pong_unmatched;
    uint32_t ping_timeout;
    uint32_t rtt_last_ms;
    uint32_t rtt_min_ms;
    uint32_t rtt_max_ms;
} tron_node_counters_t;

static tron_mesh_dedupe_t dedupe_cache;
static tron_node_counters_t node_counters;
static tron_mesh_pingpong_root_t ping_root;
static uint32_t next_seq24;
static uint32_t prng_state;

static uint32_t now_ms(void)
{
    SYSTIM t;
    tk_get_otm(&t);
    return t.lo;                    /* low 32 bits of ms; unsigned diffs wrap-safe */
}

static void clear_memory(void *ptr, size_t len)
{
    UB *p = (UB *)ptr;

    while (len > 0) {
        *p++ = 0u;
        len--;
    }
}

static uint16_t node_id(void)
{
    /* 0 keeps the FICR-derived identity, which is unique per board but opaque.
       Any other value pins a readable id so a bench topology can be labelled
       in logs without hand-mapping device addresses. */
    if ((uint16_t)((uint32_t)TRON_NODE_ID & 0xFFFFu) != 0u) {
        return (uint16_t)((uint32_t)TRON_NODE_ID & 0xFFFFu);
    }
    return (uint16_t)(in_w(FICR_DEVICEADDR0) & 0xFFFFu);
}

static uint32_t next_prng(void)
{
    prng_state = (prng_state * 1103515245UL) + 12345UL;
    return prng_state;
}

static uint32_t relay_backoff_ms(const tron_mesh_packet_t *packet, uint32_t now)
{
    uint32_t span = (TRON_NODE_RELAY_MAX_MS - TRON_NODE_RELAY_MIN_MS) + 1u;

    prng_state ^= ((uint32_t)packet->src << 16) ^
                  (packet->seq24 & TRON_MESH_SEQ24_MAX) ^
                  ((uint32_t)packet->net_id << 8) ^ now;
    return TRON_NODE_RELAY_MIN_MS + (next_prng() % span);
}

static int dedupe_seen_or_insert(const tron_mesh_packet_t *packet, uint32_t now)
{
    return tron_mesh_dedupe_seen_or_insert(&dedupe_cache, packet, now);
}

static const UB *payload_hex(const tron_mesh_packet_t *packet)
{
    static UB text[(TRON_MESH_PAYLOAD_MAX * 2u) + 1u];
    static const UB digits[] = "0123456789abcdef";
    UINT i;

    for (i = 0u; i < packet->payload_len; i++) {
        text[i * 2u] = digits[(packet->payload[i] >> 4) & 0x0Fu];
        text[(i * 2u) + 1u] = digits[packet->payload[i] & 0x0Fu];
    }
    text[packet->payload_len * 2u] = '\0';
    return text;
}

static void log_counters(const ble_mesh_scheduler_t *sched)
{
    const ble_mesh_sched_counters_t *sched_counters = ble_mesh_scheduler_counters(sched);
    uint32_t tx_ok = 0u;
    uint32_t queue_drop = node_counters.queue_drop;
    uint32_t relay_rate_limited = 0u;

    if (sched_counters != NULL) {
        tx_ok = sched_counters->tx_ok;
        /* Scheduler can drop queued relay packets internally while admitting
           own traffic and still return enqueue success. Expose that queue-drop
           source without double-counting failed enqueues observed by the node. */
        if (sched_counters->queue_drop > queue_drop) {
            queue_drop = sched_counters->queue_drop;
        }
        relay_rate_limited = sched_counters->relay_rate_limited;
    }

    tm_printf((UB *)"mesh counters id=0x%04x rx_ok=%lu tx_ok=%lu decode_error=%lu duplicate_drop=%lu ttl_drop=%lu queue_drop=%lu relay_scheduled=%lu relay_rate_limited=%lu blocked_direct=%lu\n",
              node_id(),
              (UW)node_counters.rx_ok,
              (UW)tx_ok,
              (UW)node_counters.decode_error,
              (UW)node_counters.duplicate_drop,
              (UW)node_counters.ttl_drop,
              (UW)queue_drop,
              (UW)node_counters.relay_scheduled,
              (UW)relay_rate_limited,
              (UW)node_counters.blocked_direct);
    tm_printf((UB *)"bidi counters id=0x%04x sem_drop=%lu ping_q=%lu ping_qfail=%lu ping_rx=%lu pong_q=%lu pong_qfail=%lu pong_match=%lu pong_unmatch=%lu timeout=%lu rtt=%lu/%lu/%lums\n",
              node_id(),
              (UW)node_counters.semantic_drop,
              (UW)node_counters.ping_queued,
              (UW)node_counters.ping_queue_fail,
              (UW)node_counters.ping_received,
              (UW)node_counters.pong_queued,
              (UW)node_counters.pong_queue_fail,
              (UW)node_counters.pong_matched,
              (UW)node_counters.pong_unmatched,
              (UW)node_counters.ping_timeout,
              (UW)node_counters.rtt_last_ms,
              (UW)node_counters.rtt_min_ms,
              (UW)node_counters.rtt_max_ms);
}

static void fill_dummy_payload(tron_mesh_packet_t *packet)
{
    packet->payload_len = 8u;
    packet->payload[0] = 'D';
    packet->payload[1] = 'U';
    packet->payload[2] = 'M';
    packet->payload[3] = 'Y';
    packet->payload[4] = (uint8_t)(packet->seq24 & 0xFFu);
    packet->payload[5] = (uint8_t)((packet->seq24 >> 8) & 0xFFu);
    packet->payload[6] = (uint8_t)((packet->seq24 >> 16) & 0xFFu);
    packet->payload[7] = packet->ttl;
}

static int enqueue_packet(ble_mesh_scheduler_t *sched,
                          const tron_mesh_packet_t *packet,
                          ble_mesh_sched_tx_kind_t kind,
                          uint32_t not_before_ms)
{
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len = 0u;
    tron_mesh_packet_result_t result;

    result = tron_mesh_packet_encode(packet, adv, sizeof(adv), &adv_len);
    if (result != TRON_MESH_PACKET_OK) {
        tm_printf((UB *)"mesh encode failed: %s\n",
                  (UB *)tron_mesh_packet_result_name(result));
        return 0;
    }

    if (!ble_mesh_scheduler_enqueue(sched,
                                    adv,
                                    (uint8_t)adv_len,
                                    BLE_MESH_SCHED_CH_ALL,
                                    kind,
                                    not_before_ms)) {
        node_counters.queue_drop++;
        return 0;
    }

    return 1;
}

static void schedule_dummy_packet(ble_mesh_scheduler_t *sched,
                                  uint16_t self,
                                  uint32_t now)
{
    tron_mesh_packet_t packet;

    clear_memory(&packet, sizeof(packet));
    packet.net_id = TRON_NODE_NET_ID;
    packet.msg_type = TRON_MESH_MSG_TYPE_DUMMY_STATUS;
    packet.ttl = TRON_NODE_OWN_TTL;
    packet.src = self;
    packet.seq24 = next_seq24 & TRON_MESH_SEQ24_MAX;
    next_seq24 = (next_seq24 + 1u) & TRON_MESH_SEQ24_MAX;
    fill_dummy_payload(&packet);

    if (enqueue_packet(sched, &packet, BLE_MESH_SCHED_TX_OWN, now)) {
        (void)dedupe_seen_or_insert(&packet, now);
        tm_printf((UB *)"mesh own dummy queued src=0x%04x seq=%lu ttl=%u\n",
                  packet.src,
                  (UW)packet.seq24,
                  (UINT)packet.ttl);
    }
}

static void maybe_schedule_ping(ble_mesh_scheduler_t *sched, uint32_t now)
{
    tron_mesh_packet_t ping;
    tron_mesh_pingpong_result_t build_result;
    tron_mesh_pingpong_attempt_result_t attempt_result;
    uint32_t seq24;
    int queued = 0;

    if (!tron_mesh_pingpong_root_ping_due(&ping_root, now)) {
        return;
    }

    seq24 = next_seq24 & TRON_MESH_SEQ24_MAX;
    build_result = tron_mesh_pingpong_build_ping(&ping,
                                                 TRON_NODE_OWN_TTL,
                                                 seq24);
    if (build_result == TRON_MESH_PINGPONG_OK) {
        queued = enqueue_packet(sched, &ping, BLE_MESH_SCHED_TX_OWN, now);
        if (!queued) {
            node_counters.ping_queue_fail++;
        }
    } else {
        tm_printf((UB *)"mesh PING build failed: %s\n",
                  (UB *)tron_mesh_pingpong_result_name(build_result));
    }

    attempt_result = tron_mesh_pingpong_root_record_ping_attempt(&ping_root,
                                                                  seq24,
                                                                  now,
                                                                  queued);
    if (attempt_result == TRON_MESH_PINGPONG_ATTEMPT_QUEUED) {
        next_seq24 = (next_seq24 + 1u) & TRON_MESH_SEQ24_MAX;
        node_counters.ping_queued++;
        (void)dedupe_seen_or_insert(&ping, now);
        tm_printf((UB *)"mesh PING queued src=0x%04x dst=0x%04x seq=%lu timeout=%lums\n",
                  ping.src,
                  TRON_MESH_PINGPONG_LEAF_ID,
                  (UW)ping.seq24,
                  (UW)TRON_MESH_PINGPONG_TIMEOUT_MS);
    }
}

static void respond_to_ping(ble_mesh_scheduler_t *sched,
                            const tron_mesh_packet_t *ping,
                            uint32_t now)
{
    tron_mesh_packet_t pong;
    tron_mesh_pingpong_result_t result;

    node_counters.ping_received++;
    tm_printf((UB *)"mesh PING received src=0x%04x dst=0x%04x seq=%lu ttl=%u\n",
              ping->src,
              TRON_MESH_PINGPONG_LEAF_ID,
              (UW)ping->seq24,
              (UINT)ping->ttl);

    result = tron_mesh_pingpong_build_pong(&pong, ping, TRON_NODE_OWN_TTL);
    if (result != TRON_MESH_PINGPONG_OK) {
        tm_printf((UB *)"mesh PONG build failed: %s\n",
                  (UB *)tron_mesh_pingpong_result_name(result));
        return;
    }

    if (enqueue_packet(sched, &pong, BLE_MESH_SCHED_TX_OWN, now)) {
        node_counters.pong_queued++;
        (void)dedupe_seen_or_insert(&pong, now);
        tm_printf((UB *)"mesh PONG queued src=0x%04x dst=0x%04x seq=%lu\n",
                  pong.src,
                  TRON_MESH_PINGPONG_ROOT_ID,
                  (UW)pong.seq24);
    } else {
        node_counters.pong_queue_fail++;
    }
}

static void correlate_pong(const tron_mesh_packet_t *pong, uint32_t now)
{
    tron_mesh_pingpong_match_result_t match;
    uint32_t rtt_ms = 0u;

    match = tron_mesh_pingpong_root_match_pong(&ping_root, pong, now, &rtt_ms);
    if (match == TRON_MESH_PINGPONG_PONG_MATCHED) {
        if (node_counters.pong_matched == 0u || rtt_ms < node_counters.rtt_min_ms) {
            node_counters.rtt_min_ms = rtt_ms;
        }
        if (node_counters.pong_matched == 0u || rtt_ms > node_counters.rtt_max_ms) {
            node_counters.rtt_max_ms = rtt_ms;
        }
        node_counters.rtt_last_ms = rtt_ms;
        node_counters.pong_matched++;
        tm_printf((UB *)"mesh PONG matched src=0x%04x dst=0x%04x seq=%lu RTT=%lums\n",
                  pong->src,
                  TRON_MESH_PINGPONG_ROOT_ID,
                  (UW)pong->seq24,
                  (UW)rtt_ms);
        return;
    }

    node_counters.pong_unmatched++;
    if (match == TRON_MESH_PINGPONG_PONG_LATE) {
        node_counters.ping_timeout++;
    }
    tm_printf((UB *)"mesh PONG unmatched src=0x%04x dst=0x%04x seq=%lu late=%u\n",
              pong->src,
              TRON_MESH_PINGPONG_ROOT_ID,
              (UW)pong->seq24,
              (UINT)(match == TRON_MESH_PINGPONG_PONG_LATE));
}

static void maybe_schedule_relay(ble_mesh_scheduler_t *sched,
                                 const tron_mesh_packet_t *packet,
                                 uint16_t self,
                                 uint32_t now)
{
    tron_mesh_packet_t relay;
    uint32_t backoff;

    if (packet->src == self) {
        return;
    }

    if (packet->ttl == 0u) {
        node_counters.ttl_drop++;
        return;
    }

    relay = *packet;
    relay.ttl = (uint8_t)(packet->ttl - 1u);
    backoff = relay_backoff_ms(packet, now);

    if (enqueue_packet(sched, &relay, BLE_MESH_SCHED_TX_RELAY, now + backoff)) {
        node_counters.relay_scheduled++;
        tm_printf((UB *)"mesh relay queued src=0x%04x seq=%lu ttl=%u backoff=%lums\n",
                  relay.src,
                  (UW)relay.seq24,
                  (UINT)relay.ttl,
                  (UW)backoff);
    }
}

static void handle_packet(ble_mesh_scheduler_t *sched,
                          const ble_mesh_sched_event_t *event,
                          uint16_t self,
                          uint32_t now)
{
    tron_mesh_packet_t packet;
    tron_mesh_packet_result_t result;
    tron_mesh_pingpong_result_t semantic_result;

    result = tron_mesh_packet_decode(event->adv_data, event->adv_len, &packet);
    if (result != TRON_MESH_PACKET_OK) {
        node_counters.decode_error++;
        return;
    }

    if (!tron_mesh_packet_is_for_network(&packet, TRON_NODE_NET_ID)) {
        node_counters.decode_error++;
        return;
    }

    /* Test-only range emulation. This format carries no relayer id, so a
       relayed copy is indistinguishable from a direct one except by hop count:
       only a packet still at full TTL can have come straight from its
       originator. Dropping exactly those, and nothing else, makes this peer
       unreachable directly while leaving its relayed copies intact - which is
       what forces a real multi-hop path between boards sitting on one desk.
       Note this is weaker than a link-layer block: the radio still hears the
       frame, so it proves routing behaviour, not range. */
    if ((uint16_t)TRON_NODE_BLOCK_DIRECT_PEER_ID != 0u &&
        packet.src == (uint16_t)TRON_NODE_BLOCK_DIRECT_PEER_ID &&
        packet.ttl == TRON_MESH_TTL_MAX) {
        node_counters.blocked_direct++;
        return;
    }

    /* Preserve the transport counter contract: this counts every decoded,
       in-network frame admitted past the test-only direct-peer block. */
    node_counters.rx_ok++;

    if (tron_mesh_pingpong_is_supported_type(packet.msg_type)) {
        semantic_result = tron_mesh_pingpong_validate(&packet);
        if (semantic_result != TRON_MESH_PINGPONG_OK) {
            node_counters.semantic_drop++;
            return;
        }
    }

    if (dedupe_seen_or_insert(&packet, now)) {
        node_counters.duplicate_drop++;
        return;
    }

    if (packet.msg_type == TRON_MESH_MSG_TYPE_PING) {
        if (self == TRON_MESH_PINGPONG_LEAF_ID) {
            respond_to_ping(sched, &packet, now);
        }
    } else if (packet.msg_type == TRON_MESH_MSG_TYPE_PONG) {
        if (self == TRON_MESH_PINGPONG_ROOT_ID) {
            correlate_pong(&packet, now);
        }
    } else if (packet.msg_type == TRON_MESH_MSG_TYPE_DUMMY_STATUS) {
        tm_printf((UB *)"mesh rx dummy src=0x%04x seq=%lu ttl=%u rssi=-%u dBm payload=%s\n",
                  packet.src,
                  (UW)packet.seq24,
                  (UINT)packet.ttl,
                  (UINT)event->rssi_magnitude_db,
                  payload_hex(&packet));
    } else {
        tm_printf((UB *)"mesh unsupported msg_type=0x%02x ignored src=0x%04x seq=%lu\n",
                  (UINT)packet.msg_type,
                  packet.src,
                  (UW)packet.seq24);
        return;
    }

    maybe_schedule_relay(sched, &packet, self, now);
}

LOCAL void mesh_node_task(INT stacd, void *exinf)
{
    ble_mesh_scheduler_t sched;
    ble_mesh_sched_event_t event;
    uint16_t self = node_id();
    uint32_t t = now_ms();
    uint32_t next_dummy_at = t + TRON_NODE_DUMMY_INTERVAL_MS;
    uint32_t next_stats_at = t + TRON_NODE_STATS_MS;

    (void)stacd;
    (void)exinf;

    tron_mesh_dedupe_reset(&dedupe_cache);
    clear_memory(&node_counters, sizeof(node_counters));
    tron_mesh_pingpong_root_init(&ping_root, t);
    next_seq24 = 0u;
    prng_state = ((uint32_t)self << 16) | 1u;

    tm_printf((UB *)"experimental BLE advertising mesh-like transport; not Bluetooth Mesh compliant\n");
    tm_printf((UB *)"mesh node src=0x%04x net=0x%02x ttl=%u passive RX with interleaved TX\n",
              self,
              (UINT)TRON_NODE_NET_ID,
              (UINT)TRON_NODE_OWN_TTL);
    tm_printf((UB *)"mesh PING/PONG root=0x%04x leaf=0x%04x interval=%lums timeout=%lums role=%s\n",
              TRON_MESH_PINGPONG_ROOT_ID,
              TRON_MESH_PINGPONG_LEAF_ID,
              (UW)TRON_MESH_PINGPONG_INTERVAL_MS,
              (UW)TRON_MESH_PINGPONG_TIMEOUT_MS,
              self == TRON_MESH_PINGPONG_ROOT_ID ? (UB *)"root" :
              (self == TRON_MESH_PINGPONG_LEAF_ID ? (UB *)"leaf" : (UB *)"generic"));

    ble_mesh_scheduler_init_legacy(&sched, t);
    ble_mesh_scheduler_start_rx(&sched, t);

    while (1) {
        t = now_ms();

        if (self == TRON_MESH_PINGPONG_ROOT_ID) {
            maybe_schedule_ping(&sched, t);
        } else if (self != TRON_MESH_PINGPONG_LEAF_ID &&
                   tron_mesh_time_reached(t, next_dummy_at)) {
            schedule_dummy_packet(&sched, self, t);
            next_dummy_at = t + TRON_NODE_DUMMY_INTERVAL_MS;
        }

        if (ble_mesh_scheduler_poll(&sched, t, &event) &&
            event.type == BLE_MESH_SCHED_EVENT_RX_ADV) {
            handle_packet(&sched, &event, self, t);
        }

        /* Give an on-time packet captured in this scheduler poll a chance to
           match before expiring the pending transaction at the same tick. */
        if (self == TRON_MESH_PINGPONG_ROOT_ID &&
            tron_mesh_pingpong_root_check_timeout(&ping_root, t)) {
            node_counters.ping_timeout++;
            tm_printf((UB *)"mesh PING timeout seq=%lu after=%lums\n",
                      (UW)ping_root.pending_seq24,
                      (UW)TRON_MESH_PINGPONG_TIMEOUT_MS);
        }

        if (tron_mesh_time_reached(t, next_stats_at)) {
            log_counters(&sched);
            next_stats_at = t + TRON_NODE_STATS_MS;
        }

        tk_dly_tsk(TRON_NODE_LOOP_DELAY_MS);
    }
}

EXPORT INT usermain(void)
{
    T_CTSK ctsk = {
        .exinf   = NULL,
        .tskatr  = TA_HLNG | TA_RNG3,
        .task    = (FP)mesh_node_task,
        .itskpri = 10,
        .stksz   = 1536,
    };
    ID tskid;

    ble_radio_init();

    tskid = tk_cre_tsk(&ctsk);
    if (tskid > 0) {
        tk_sta_tsk(tskid, 0);
    } else {
        tm_printf((UB *)"tk_cre_tsk failed: %d\n", tskid);
    }

    tk_slp_tsk(TMO_FEVR);
    return 0;
}
