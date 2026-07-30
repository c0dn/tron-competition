/*
 * mesh_injector - traffic generator and instrument for the flood mesh.
 *
 * WHY THIS EXISTS
 * ---------------
 * The mesh has to be measured under traffic it does not produce itself. A pair
 * of wearables emits a heartbeat every 1.5 s from two identities, which is far
 * too sparse and too uniform to show whether duplicate suppression, the hop
 * budget or the backoff actually work. This app manufactures the rest:
 *
 *   - N synthetic originators from one radio, so the duplicate cache is
 *     exercised at a population size we do not have boards for
 *   - deliberately pre-aged and expired messages, which a healthy mesh can
 *     never generate on its own, so the TTL branches are reachable on a bench
 *   - exact duplicates on demand, so suppression can be observed rather than
 *     assumed
 *
 * It is a measurement tool, not part of the product. No sensors, no fusion.
 *
 * HOW TO RUN
 * ----------
 *   source board:  ./flash.sh mesh_injector --uid <A>   (INJ_MODE_SOURCE)
 *   relay board:   ./flash.sh mesh_injector --uid <B>   (INJ_MODE_RELAY)
 * and watch the relay's serial output, or point the ESP32-C3 ble_sniffer at
 * the pair to see what actually leaves the mesh.
 *
 * The serial output is role-specific and written as a demo narrative rather
 * than a raw struct dump. On the relay, accepted/tx_copies increasing proves
 * that packets are being received and retransmitted; the ESP32-C3 supplies the
 * independent proof by printing RELAYED, the relay id, hops and decremented TTL.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "injector_config.h"
#include "mesh_flood.h"
#include "ble_radio.h"
#include "hw_timer.h"
#include "display.h"
#include "schema.h"

/* Injector traffic bypasses mesh_originate() so it can impersonate several
   originators. Keep explicit source counters instead of showing the mesh
   library's originated/tx fields as zero, which was correct but confusing in
   a demo. Reads and writes are naturally aligned 32-bit accesses on Cortex-M4. */
static volatile UW generated_messages = 0;
static volatile UW generated_rounds = 0;
static volatile UW generated_probe_sets = 0;

/* Advertising address for an arbitrary originator id (schema.h MIND_ADVA). */
static void adva_for(UB id, UB *out)
{
    out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0;
    out[4] = id;
    out[5] = 0xC0;
}

/* A schema-v1 payload. accel_svm carries the round counter so a receiver can
   see which round a packet belongs to without decoding the mesh header. */
static void build_payload(UB *p, UB evt, UB seq, UH svm)
{
    mind_adv_payload_t v;
    const UB *b = (const UB *)&v;
    UINT i;

    v.schema_version = MIND_SCHEMA_VERSION;
    v.event_type     = evt;
    v.confidence     = 0;
    v.accel_svm      = svm;
    v.mic_level      = 0;
    v.seq            = seq;

    for (i = 0; i < MIND_PAYLOAD_SIZE; i++) {
        p[i] = b[i];
    }
}

/* Transmit one message as though it came from 'orig' with the given transport
   state. Goes through mesh_build_adv() + ble_radio_tx() rather than
   mesh_originate() precisely because mesh_originate() will only ever produce
   well-formed, full-TTL, zero-hop messages from THIS node - which is the one
   thing that cannot test a relay. */
static void emit_as(UB orig, UB relay, UB ttl, UB hops, UB evt, UB seq, UH svm)
{
    UB adv[BLE_ADV_MAX_DATA];
    UB payload[MIND_PAYLOAD_SIZE];
    UB addr[6];
    UINT len;

    build_payload(payload, evt, seq, svm);
    adva_for(orig, addr);
    len = mesh_build_adv(adv, payload, MIND_PAYLOAD_SIZE, orig, relay, ttl, hops);
    ble_radio_tx(adv, len, addr, BLE_CHAN_MASK_ALL);
    generated_messages++;
}

/* --- LED: role and liveness ---------------------------------------------- *
 * Row 0 marks the role (left pixel = sourcing, right = relaying) and the rest
 * is a 20-pixel bar that advances with activity. The point is to tell at a
 * glance, without a serial cable, whether a board across the room is still
 * doing anything. */
static void led_update(UB sourcing, UB relaying, UW activity)
{
    UB rows[5];
    UINT n = (UINT)(activity % 21);
    UINT i;

    rows[0] = (UB)((sourcing ? 0x01 : 0) | (relaying ? 0x10 : 0));
    rows[1] = 0; rows[2] = 0; rows[3] = 0; rows[4] = 0;

    for (i = 0; i < n; i++) {
        rows[1 + (i / 5)] |= (UB)(1U << (i % 5));
    }
    display_set_rows(rows);
}

/* --- Injection ----------------------------------------------------------- */

LOCAL void inject_task(INT stacd, void *exinf)
{
    UW round = 0;
    UB seq = 0;

    tm_printf((UB *)"[SOURCE] generating %d virtual devices (0x%02x..0x%02x) every %d ms\n",
              INJ_VIRTUAL_SRC, INJ_SRC_BASE_ID,
              INJ_SRC_BASE_ID + INJ_VIRTUAL_SRC - 1, INJ_INTERVAL_MS);

    while (1) {
        INT v;

        for (v = 0; v < INJ_VIRTUAL_SRC; v++) {
            UB id = (UB)(INJ_SRC_BASE_ID + v);

            /* A fresh message from a distinct originator: full hop budget,
               zero hops, relay id equal to the origin because nobody has
               forwarded it yet. */
            emit_as(id, id, MESH_TTL_DEFAULT, 0,
                    MIND_EVT_HEARTBEAT, seq, (UH)round);
        }

#if INJ_TEST_EVERY > 0
        if (INJ_TEST_EVERY > 0 && (round % INJ_TEST_EVERY) == 0) {
            /* 1. Nearly spent: ttl 1 means the receiver may forward it exactly
                  once, and whoever hears THAT copy must discard it. This is the
                  only way to see a flood stop by hop budget on two boards. */
            emit_as(INJ_TEST_ID, INJ_TEST_ID, 1, 2,
                    MIND_EVT_MOTION, seq, (UH)round);

            /* 2. Byte-identical key to the message above. Must be suppressed:
                  same origin, same event type, same sequence number. */
            emit_as(INJ_TEST_ID, INJ_TEST_ID, MESH_TTL_DEFAULT, 0,
                    MIND_EVT_MOTION, seq, (UH)round);

            /* 3. Already dead on arrival: a relay must not forward ttl 0. */
            emit_as(INJ_TEST_ID, INJ_TEST_ID, 0, 5,
                    MIND_EVT_MOTION, (UB)(seq + 1), (UH)round);

            generated_probe_sets++;
            tm_printf((UB *)"[SOURCE TEST] seq=%u sent: ttl=1 + duplicate + ttl=0 probes\n",
                      seq);
        }
#endif

        round++;
        seq++;
        generated_rounds = round;
        tk_dly_tsk(INJ_INTERVAL_MS);
    }
}

/* --- Relay --------------------------------------------------------------- */

LOCAL void relay_task(INT stacd, void *exinf)
{
    /* Lowest priority in the app by construction: this spins instead of
       sleeping, because sleeping loses packets (see MESH_POLL_SPIN). Every
       other task here is timer-driven and preempts it. */
    mesh_task();
}

/* --- Reporting ----------------------------------------------------------- */

LOCAL void stats_task(INT stacd, void *exinf)
{
    UB sourcing = (INJ_MODE != INJ_MODE_RELAY);
    UB relaying = (INJ_MODE != INJ_MODE_SOURCE);
#if INJ_MODE == INJ_MODE_RELAY
    mesh_stats_t prev = { 0 };
#endif

    while (1) {
        mesh_stats_t s;

        mesh_stats(&s);

#if INJ_MODE == INJ_MODE_SOURCE
        tm_printf((UB *)"[SOURCE 0x%02x] rounds=%u generated=%u probe_sets=%u | "
                        "relay_copies_seen=%u duplicates_blocked=%u\n",
                  INJ_DEVICE_ID, generated_rounds, generated_messages,
                  generated_probe_sets, s.rx_mesh, s.dup_dropped);
        tm_printf((UB *)"[SOURCE HEALTH] ttl_expired=%u malformed=%u radio_drops=%u\n",
                  s.ttl_dropped, s.malformed, s.radio_dropped);
#elif INJ_MODE == INJ_MODE_RELAY
        tm_printf((UB *)"[RELAY 0x%02x] mesh_rx=%u (+%u) accepted=%u (+%u) "
                        "tx_copies=%u (+%u)\n",
                  INJ_DEVICE_ID,
                  s.rx_mesh, s.rx_mesh - prev.rx_mesh,
                  s.relayed, s.relayed - prev.relayed,
                  s.tx_bursts, s.tx_bursts - prev.tx_bursts);
        tm_printf((UB *)"[RELAY FILTER] duplicates=%u ttl_expired=%u queue_drops=%u "
                        "malformed=%u radio_drops=%u\n",
                  s.dup_dropped, s.ttl_dropped, s.queue_dropped,
                  s.malformed, s.radio_dropped);
#else
        tm_printf((UB *)"[BOTH 0x%02x] rounds=%u generated=%u mesh_rx=%u "
                        "accepted=%u tx_copies=%u\n",
                  INJ_DEVICE_ID, generated_rounds, generated_messages,
                  s.rx_mesh, s.relayed, s.tx_bursts);
        tm_printf((UB *)"[MESH HEALTH] duplicates=%u ttl_expired=%u queue_drops=%u "
                        "malformed=%u radio_drops=%u\n",
                  s.dup_dropped, s.ttl_dropped, s.queue_dropped,
                  s.malformed, s.radio_dropped);
#endif

        led_update(sourcing, relaying, s.tx_bursts + s.rx_mesh);
#if INJ_MODE == INJ_MODE_RELAY
        prev = s;
#endif
        tk_dly_tsk(INJ_STATS_MS);
    }
}

static ID make_task(FP entry, PRI pri)
{
    T_CTSK ctsk = {
        .exinf   = NULL,
        .tskatr  = TA_HLNG | TA_RNG3,
        .task    = entry,
        .itskpri = pri,
        .stksz   = 1024,
    };
    ID id = tk_cre_tsk(&ctsk);

    if (id > 0) {
        tk_sta_tsk(id, 0);
    } else {
        tm_printf((UB *)"tk_cre_tsk failed: %d\n", id);
    }
    return id;
}

EXPORT INT usermain(void)
{
    mesh_cfg_t cfg;
    static const char *rolename[] = { "SOURCE", "RELAY", "BOTH" };

    display_init();

    mesh_default_cfg(&cfg, INJ_DEVICE_ID);
#if   INJ_MODE == INJ_MODE_SOURCE
    cfg.role = MESH_ROLE_SOURCE;
#elif INJ_MODE == INJ_MODE_RELAY
    cfg.role = MESH_ROLE_RELAY;
#else
    cfg.role = MESH_ROLE_BOTH;
#endif

    mesh_init(&cfg);            /* brings up radio + hw_timer, starts listening */

#if INJ_TXPOWER_DBM != 127
    ble_radio_set_txpower(INJ_TXPOWER_DBM);
    tm_printf((UB *)"txpower: %d dBm\n", INJ_TXPOWER_DBM);
#endif

    tm_printf((UB *)"\n=== MIND FLOOD MESH DEMO ===\n");
    tm_printf((UB *)"[NODE] id=0x%02x role=%s listen_ch=%d ttl=%d repeats=%d "
                    "jitter=0..%u us\n",
              INJ_DEVICE_ID, rolename[INJ_MODE], cfg.listen_ch, cfg.ttl,
              cfg.relay_repeats, cfg.jitter_max_us);
#if INJ_MODE == INJ_MODE_RELAY
    tm_printf((UB *)"[VERIFY] accepted and tx_copies must rise; C3 should print "
                    "RELAYED via=0x%02x hops=1 ttl=%d\n",
              INJ_DEVICE_ID, cfg.ttl - 1);
#elif INJ_MODE == INJ_MODE_SOURCE
    tm_printf((UB *)"[VERIFY] generated must rise; relay should show accepted/tx_copies\n");
#else
    tm_printf((UB *)"[VERIFY] generated, accepted and tx_copies should rise\n");
#endif

    /* Priorities: display refresh runs at 8, so the spinning relay must sit
       below it or the matrix stops scanning. */
    make_task((FP)stats_task,  6);
#if INJ_MODE != INJ_MODE_RELAY
    make_task((FP)inject_task, 7);
#endif
    make_task((FP)relay_task, 12);

    tk_slp_tsk(TMO_FEVR);
    return 0;
}
