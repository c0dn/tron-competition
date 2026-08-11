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
#include "schema.h"
#include "tron_mesh_dedupe.h"
#include "tron_mesh_packet.h"

/* Local byte copy rather than <string.h>: newlib's string.h drags in stddef's
 * size_t (unsigned int), which collides with the kernel's SZ (long int) in
 * tk/syslib.h. ble_mesh_scheduler.c carries the same helper. */
static void node_copy_bytes(void *dst, const void *src, uint8_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (len > 0u) {
        *d++ = *s++;
        len--;
    }
}

/* Heartbeats are liveness traffic, not incidents, and at the wearable's
 * 500 ms cadence they would otherwise account for most of this log. Print
 * one in this many; incidents are never throttled. */
#ifndef NODE_HEARTBEAT_LOG_EVERY
#define NODE_HEARTBEAT_LOG_EVERY 8u
#endif

static uint32_t hb_seen;

/* Short labels for the wearable's incident types, for the serial log. */
static const char *mind_event_name(uint8_t event_type)
{
    switch (event_type) {
    case MIND_EVT_HEARTBEAT:         return "heartbeat";
    case MIND_EVT_MOTION:            return "motion";
    case MIND_EVT_POSSIBLE_FALL:     return "possible-fall";
    case MIND_EVT_CONFIRMED_FALL:    return "CONFIRMED-FALL";
    case MIND_EVT_POSSIBLE_DISTRESS: return "shout";
    case MIND_EVT_FALL_AND_SHOUT:    return "FALL+SHOUT";
    default:                         return "?";
    }
}

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

#define TRON_NODE_NET_ID          0x01u
#define TRON_NODE_OWN_TTL         TRON_MESH_TTL_MAX
#define TRON_NODE_OWN_INTERVAL_MS 1000u
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
} tron_node_counters_t;

static tron_mesh_dedupe_t dedupe_cache;
static tron_node_counters_t node_counters;
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

static void schedule_own_packet(ble_mesh_scheduler_t *sched,
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

static void maybe_schedule_relay(ble_mesh_scheduler_t *sched,
                                 const tron_mesh_packet_t *packet,
                                 uint16_t self,
                                 uint32_t now,
                                 int verbose)
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
        if (verbose) {
            tm_printf((UB *)"  relay -> src=0x%04x seq=%lu ttl=%u backoff=%lums\n",
                      relay.src,
                      (UW)relay.seq24,
                      (UINT)relay.ttl,
                      (UW)backoff);
        }
    }
}

static void handle_packet(ble_mesh_scheduler_t *sched,
                          const ble_mesh_sched_event_t *event,
                          uint16_t self,
                          uint32_t now)
{
    tron_mesh_packet_t packet;
    tron_mesh_packet_result_t result;
    /* Whether this packet's relay is worth a log line. Heartbeat relays are
     * throttled alongside their rx lines so the pair stays consistent. */
    int verbose = 1;

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

    node_counters.rx_ok++;

    if (dedupe_seen_or_insert(&packet, now)) {
        node_counters.duplicate_drop++;
        return;
    }

    switch (packet.msg_type) {
    case TRON_MESH_MSG_TYPE_DUMMY_STATUS:
        tm_printf((UB *)"mesh rx dummy src=0x%04x seq=%lu ttl=%u rssi=-%u dBm payload=%s\n",
                  packet.src,
                  (UW)packet.seq24,
                  (UINT)packet.ttl,
                  (UINT)event->rssi_dbm,
                  payload_hex(&packet));
        break;

    case TRON_MESH_MSG_TYPE_MIND_EVENT: {
        mind_adv_payload_t p;

        if (packet.payload_len < (uint8_t)MIND_PAYLOAD_SIZE) {
            tm_printf((UB *)"mesh rx event TRUNCATED src=0x%04x seq=%lu len=%u\n",
                      packet.src, (UW)packet.seq24, (UINT)packet.payload_len);
            return;
        }
        node_copy_bytes(&p, packet.payload, (uint8_t)MIND_PAYLOAD_SIZE);

        /* Heartbeats arrive twice a second and incidents every few seconds,
         * so logging both the same way buries the thing the log exists to
         * show. Heartbeats get a short line, and only every Nth, while every
         * incident is printed. Both stay under 80 columns: the old single
         * format ran past it and wrapped mid-token, splitting the event name
         * across two lines. */
        if (p.event_type == MIND_EVT_HEARTBEAT) {
            verbose = ((hb_seen++ % NODE_HEARTBEAT_LOG_EVERY) == 0u);
            if (verbose) {
                tm_printf((UB *)"hb    src=0x%04x seq=%lu -%udBm svm=%u\n",
                          packet.src,
                          (UW)packet.seq24,
                          (UINT)event->rssi_dbm,
                          (UINT)p.accel_svm);
            }
        } else {
            tm_printf((UB *)"EVENT src=0x%04x seq=%lu ttl=%u -%udBm %s conf=%u svm=%u mic=%u\n",
                      packet.src,
                      (UW)packet.seq24,
                      (UINT)packet.ttl,
                      (UINT)event->rssi_dbm,
                      mind_event_name(p.event_type),
                      (UINT)p.confidence,
                      (UINT)p.accel_svm,
                      (UINT)p.mic_level);
        }
        break;
    }

    default:
        /* Unknown types are not relayed: forwarding something this build
         * cannot parse would spend other nodes' airtime on traffic no one in
         * this network can act on. */
        tm_printf((UB *)"mesh unknown msg_type=0x%02x ignored src=0x%04x seq=%lu\n",
                  (UINT)packet.msg_type,
                  packet.src,
                  (UW)packet.seq24);
        return;
    }

    maybe_schedule_relay(sched, &packet, self, now, verbose);
}

LOCAL void mesh_node_task(INT stacd, void *exinf)
{
    ble_mesh_scheduler_t sched;
    ble_mesh_sched_event_t event;
    uint16_t self = node_id();
    uint32_t t = now_ms();
    uint32_t next_own_at = t + TRON_NODE_OWN_INTERVAL_MS;
    uint32_t next_stats_at = t + TRON_NODE_STATS_MS;

    (void)stacd;
    (void)exinf;

    tron_mesh_dedupe_reset(&dedupe_cache);
    clear_memory(&node_counters, sizeof(node_counters));
    next_seq24 = 0u;
    prng_state = ((uint32_t)self << 16) | 1u;

    tm_printf((UB *)"experimental BLE advertising mesh-like transport; not Bluetooth Mesh compliant\n");
    tm_printf((UB *)"mesh node src=0x%04x net=0x%02x ttl=%u passive RX with interleaved TX\n",
              self,
              (UINT)TRON_NODE_NET_ID,
              (UINT)TRON_NODE_OWN_TTL);

    ble_mesh_scheduler_init(&sched, t);
    ble_mesh_scheduler_start_rx(&sched, t);

    while (1) {
        t = now_ms();

        if (tron_mesh_time_reached(t, next_own_at)) {
            schedule_own_packet(&sched, self, t);
            next_own_at = t + TRON_NODE_OWN_INTERVAL_MS;
        }

        if (ble_mesh_scheduler_poll(&sched, t, &event) &&
            event.type == BLE_MESH_SCHED_EVENT_RX_ADV) {
            handle_packet(&sched, &event, self, t);
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
