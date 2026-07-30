/*
 * mesh_flood.c - controlled flood mesh (see mesh_flood.h for the design).
 *
 * DEPENDENCE ON THE PAYLOAD CONTRACT
 * ----------------------------------
 * This file includes schema.h, which is a deliberate choice rather than an
 * oversight. Duplicate suppression needs a key that identifies a message, and
 * the only fields that do so are in the payload: event type and sequence
 * number. A transport-only mesh would have to carry its own message id,
 * spending on-air bytes to restate something the payload already says.
 *
 * The cost is that the mesh is tied to schema v1, so the two offsets it reads
 * are named and isolated below - if the payload layout changes, those are the
 * only lines here that follow it.
 */

#include "mesh_flood.h"
#include "hw_timer.h"
#include "schema.h"

/* --- Payload fields the cache key is built from (schema v1) --------------- */
/* mind_adv_payload_t: [0]=schema_version [1]=event_type [2]=confidence
                       [3..4]=accel_svm  [5]=mic_level  [6]=seq              */
#define PAY_OFF_EVENT       1
#define PAY_OFF_SEQ         6

/* Exact manufacturer-data length of a mesh advert, from the shared contract.
   Checking exactly, rather than a minimum, is what keeps a plain non-mesh
   advert from being misread as a mesh one - see the note in mesh_wire.h. */
#define MSD_MESH_LEN        MESH_WIRE_MSD_LEN

#define AD_TYPE_FLAGS       MIND_AD_TYPE_FLAGS
#define AD_TYPE_MSD         MIND_AD_TYPE_MSD

/* Offsets into a received PDU: [S0][LENGTH][AdvA(6)][AdvData...] */
#define PDU_OFF_ADVA        2
#define PDU_OFF_ADVDATA     8
#define PDU_ADVA_LEN        6

static mesh_cfg_t   cfg;
static mesh_stats_t st;
static UB           own_adva[6];
static BOOL         ready = FALSE;

/* --- Duplicate cache ------------------------------------------------------
 *
 * Linear scan of a small fixed array. A hash table would be faster in theory,
 * but 32 entries is a handful of comparisons against a ~1.2 ms transmit and a
 * 50 ms backoff, so the scan is nowhere near the critical path and the array
 * cannot degrade or fragment.
 *
 * Entries are aged rather than explicitly evicted: an entry older than
 * MESH_CACHE_AGE_MS is free for reuse. When every slot is live the oldest is
 * taken, which is the least likely to still be re-arriving. */
typedef struct {
    BOOL used;
    UB   orig_id;
    UB   event_type;
    UB   seq;
    UW   t_ms;
} cache_ent_t;

static cache_ent_t cache[MESH_CACHE_ENTRIES];

static UW now_ms(void)
{
    SYSTIM t;

    if (tk_get_otm(&t) != E_OK) {
        return 0;
    }
    return (UW)t.lo;
}

/* Look the key up; if absent, insert it. Returns TRUE when the message is new
   (i.e. should be relayed), FALSE when it is a duplicate. */
static BOOL cache_seen(UB orig_id, UB event_type, UB seq, UW t)
{
    INT free_i = -1;
    INT oldest_i = 0;
    UW  oldest_age = 0;
    INT i;

    for (i = 0; i < MESH_CACHE_ENTRIES; i++) {
        UW age;

        if (!cache[i].used) {
            if (free_i < 0) {
                free_i = i;
            }
            continue;
        }

        age = (UW)(t - cache[i].t_ms);
        if (age >= MESH_CACHE_AGE_MS) {
            cache[i].used = FALSE;          /* expired - reclaim */
            if (free_i < 0) {
                free_i = i;
            }
            continue;
        }

        if (cache[i].orig_id == orig_id &&
            cache[i].event_type == event_type &&
            cache[i].seq == seq) {
            cache[i].t_ms = t;              /* refresh: the burst is ongoing */
            return TRUE;
        }

        if (age > oldest_age) {
            oldest_age = age;
            oldest_i = i;
        }
    }

    if (free_i < 0) {
        free_i = oldest_i;
    }
    cache[free_i].used = TRUE;
    cache[free_i].orig_id = orig_id;
    cache[free_i].event_type = event_type;
    cache[free_i].seq = seq;
    cache[free_i].t_ms = t;
    return FALSE;
}

/* --- Framing -------------------------------------------------------------- */

UINT mesh_build_adv(UB *buf, const UB *payload, UINT payload_len,
                    UB orig_id, UB relay_id, UB ttl, UB hops)
{
    UINT len = 0;
    UINT i;

    if (payload_len > MESH_MAX_PAYLOAD) {
        payload_len = MESH_MAX_PAYLOAD;
    }

    /* AD 1 - Flags: LE General Discoverable, BR/EDR not supported. */
    buf[len++] = 0x02;
    buf[len++] = AD_TYPE_FLAGS;
    buf[len++] = 0x06;

    /* AD 2 - Manufacturer Specific Data. */
#if MESH_ENABLE
    buf[len++] = (UB)(1 + 2 + payload_len + MESH_HDR_SIZE);
#else
    buf[len++] = (UB)(1 + 2 + payload_len);
#endif
    buf[len++] = AD_TYPE_MSD;
    buf[len++] = (UB)(MIND_COMPANY_ID & 0xFF);
    buf[len++] = (UB)(MIND_COMPANY_ID >> 8);

    for (i = 0; i < payload_len; i++) {
        buf[len++] = payload[i];
    }

#if MESH_ENABLE
    buf[len++] = MESH_VERSION;
    buf[len++] = orig_id;
    buf[len++] = relay_id;
    buf[len++] = ttl;
    buf[len++] = hops;
#else
    (void)orig_id; (void)relay_id; (void)ttl; (void)hops;
#endif

    return len;
}

/* --- Receive parsing ------------------------------------------------------ */

typedef struct {
    const UB *adva;         /* originator's address, 6 bytes, verbatim  */
    const UB *payload;      /* MIND_PAYLOAD_SIZE bytes                  */
    UB        orig_id;
    UB        relay_id;
    UB        ttl;
    UB        hops;
} mesh_msg_t;

/* Validate a received PDU as a mesh message. Returns TRUE and fills 'out' on
   success. Ordered cheapest-check-first: at the ~530 packets/s this radio sees
   in an ordinary room, most of what arrives is other people's traffic and the
   cost that matters is how quickly it is rejected. */
static BOOL parse_mesh(const ble_rx_t *rx, mesh_msg_t *out)
{
    const UB *ad;
    UINT ad_len, i;

    if (rx->len < PDU_OFF_ADVDATA) {
        return FALSE;
    }

    /* 1. Identity gate. MIND nodes use a random-static AdvA ending 0xC0, so a
          single byte rejects nearly all ambient traffic. */
    if (rx->pdu[PDU_OFF_ADVA + 5] != 0xC0) {
        return FALSE;
    }

    ad_len = (UINT)rx->pdu[1];
    if (ad_len < PDU_ADVA_LEN) {
        return FALSE;
    }
    ad_len -= PDU_ADVA_LEN;                 /* AdvData length */
    if (PDU_OFF_ADVDATA + ad_len > rx->len) {
        return FALSE;
    }
    ad = &rx->pdu[PDU_OFF_ADVDATA];

    /* 2. Walk the AD structures for Manufacturer Specific Data. */
    i = 0;
    while (i < ad_len) {
        UINT l = (UINT)ad[i];
        const UB *msd;

        if (l == 0 || i + 1 + l > ad_len) {
            return FALSE;                   /* truncated or padded - not ours */
        }
        if (ad[i + 1] != AD_TYPE_MSD) {
            i += 1 + l;
            continue;
        }

        /* l counts the type byte; the data after it is l - 1 bytes. */
        if (l - 1 != MSD_MESH_LEN) {
            return FALSE;                   /* right AD type, not a mesh advert */
        }
        msd = &ad[i + 2];

        if (((UH)msd[0] | ((UH)msd[1] << 8)) != MIND_COMPANY_ID) {
            return FALSE;
        }

        out->payload  = &msd[2];
        out->orig_id  = msd[2 + MIND_PAYLOAD_SIZE + 1];
        out->relay_id = msd[2 + MIND_PAYLOAD_SIZE + 2];
        out->ttl      = msd[2 + MIND_PAYLOAD_SIZE + 3];
        out->hops     = msd[2 + MIND_PAYLOAD_SIZE + 4];

        if (msd[2 + MIND_PAYLOAD_SIZE] != MESH_VERSION) {
            st.malformed++;
            return FALSE;
        }
        /* orig_id restates AdvA[4]. It is redundant by design: a mismatch means
           a relay corrupted the address or the header, and forwarding it would
           spread the corruption under a valid-looking identity. */
        if (out->orig_id != rx->pdu[PDU_OFF_ADVA + 4]) {
            st.malformed++;
            return FALSE;
        }

        out->adva = &rx->pdu[PDU_OFF_ADVA];
        return TRUE;
    }

    return FALSE;
}

/* --- Relay queue and backoff task -----------------------------------------
 *
 * The backoff deliberately does not run on the receive path. A relay waiting
 * out a 50 ms jitter is a relay that is not listening, and traffic arrives in
 * bursts exactly when a relay has just been given something to forward - so
 * doing the wait inline loses most of the burst. Measured on two boards before
 * this queue existed: a relay caught 20 of roughly 80 messages from a source
 * emitting four per second, and never once observed the duplicate or expired-
 * TTL cases, because those probes were always sent while it was asleep.
 *
 * Receiving now only decides and queues; this task does the waiting. */
typedef struct {
    UB payload[MIND_PAYLOAD_SIZE];
    UB adva[6];
    UB orig_id;
    UB ttl;
    UB hops;
} relay_job_t;

static relay_job_t rq[MESH_RELAY_QUEUE];
static UINT rq_head = 0, rq_tail = 0, rq_count = 0;
static ID   rq_mtx = 0;
static ID   rq_flg = 0;

#define RQ_FLG_WORK     0x01U

static void relay_enqueue(const mesh_msg_t *m, UB ttl, UB hops)
{
    relay_job_t *j;
    UINT i;

    tk_loc_mtx(rq_mtx, TMO_FEVR);

    if (rq_count >= MESH_RELAY_QUEUE) {
        /* Full: drop this one rather than the head. A queued message has
           already served part of its backoff; discarding it in favour of a
           newer arrival would waste that and reorder the flood. */
        st.queue_dropped++;
        tk_unl_mtx(rq_mtx);
        return;
    }

    j = &rq[rq_head];
    for (i = 0; i < MIND_PAYLOAD_SIZE; i++) {
        j->payload[i] = m->payload[i];
    }
    for (i = 0; i < 6; i++) {
        j->adva[i] = m->adva[i];        /* originator's address, verbatim */
    }
    j->orig_id = m->orig_id;
    j->ttl = ttl;
    j->hops = hops;

    rq_head = (rq_head + 1) % MESH_RELAY_QUEUE;
    rq_count++;
    tk_unl_mtx(rq_mtx);

    tk_set_flg(rq_flg, RQ_FLG_WORK);
}

static BOOL relay_dequeue(relay_job_t *out)
{
    BOOL got = FALSE;

    tk_loc_mtx(rq_mtx, TMO_FEVR);
    if (rq_count > 0) {
        *out = rq[rq_tail];
        rq_tail = (rq_tail + 1) % MESH_RELAY_QUEUE;
        rq_count--;
        got = TRUE;
    }
    tk_unl_mtx(rq_mtx);
    return got;
}

LOCAL void relay_tx_task(INT stacd, void *exinf)
{
    while (1) {
        relay_job_t job;
        UB adv[BLE_ADV_MAX_DATA];
        UINT len, ptn;
        INT r;

        /* Clear before checking, not after. Clearing afterwards would discard
           a set that landed between the empty check and the clear, and the
           task would sleep with work already queued. */
        tk_clr_flg(rq_flg, ~RQ_FLG_WORK);

        if (!relay_dequeue(&job)) {
            tk_wai_flg(rq_flg, RQ_FLG_WORK, TWF_ORW, &ptn, TMO_FEVR);
            continue;
        }

        len = mesh_build_adv(adv, job.payload, MIND_PAYLOAD_SIZE,
                             job.orig_id, cfg.device_id, job.ttl, job.hops);

        for (r = 0; r < (INT)cfg.relay_repeats; r++) {
            if (cfg.jitter_max_us > 0) {
                hw_timer_delay_us(hw_rand32() % cfg.jitter_max_us);
            }
            /* The originator's address goes back out untouched. This is what
               makes the hop invisible to the C3, and why the payload is
               forwarded byte for byte alongside it. */
            ble_radio_tx(adv, len, job.adva, cfg.relay_chan_mask);
            st.tx_bursts++;
        }
    }
}

/* --- Public API ----------------------------------------------------------- */

void mesh_default_cfg(mesh_cfg_t *out, UB device_id)
{
    out->device_id       = device_id;
    out->role            = MESH_ROLE;
    out->ttl             = MESH_TTL_DEFAULT;
    out->relay_repeats   = MESH_RELAY_REPEATS;
    out->relay_chan_mask = MESH_RELAY_CHAN_MASK;
    out->jitter_max_us   = MESH_JITTER_MAX_US;
    out->listen_ch       = MESH_LISTEN_CH;
    out->tx_task_pri     = MESH_TX_TASK_PRI;
}

void mesh_init(const mesh_cfg_t *c)
{
    T_CMTX cmtx = { .exinf = NULL, .mtxatr = TA_INHERIT, .ceilpri = 0 };
    T_CFLG cflg = { .exinf = NULL, .flgatr = TA_TFIFO | TA_WMUL, .iflgptn = 0 };
    T_CTSK ctsk = {
        .exinf   = NULL,
        .tskatr  = TA_HLNG | TA_RNG3,
        .task    = (FP)relay_tx_task,
        .itskpri = MESH_TX_TASK_PRI,
        .stksz   = 1024,
    };
    ID tskid;
    INT i;

    cfg = *c;

    for (i = 0; i < MESH_CACHE_ENTRIES; i++) {
        cache[i].used = FALSE;
    }
    st.rx_total = 0;  st.rx_mesh = 0;     st.originated = 0;
    st.relayed = 0;   st.tx_bursts = 0;   st.dup_dropped = 0;
    st.ttl_dropped = 0; st.own_dropped = 0; st.malformed = 0;
    st.queue_dropped = 0; st.radio_dropped = 0;
    rq_head = 0; rq_tail = 0; rq_count = 0;

    own_adva[0] = 0; own_adva[1] = 0; own_adva[2] = 0; own_adva[3] = 0;
    own_adva[4] = cfg.device_id;
    own_adva[5] = 0xC0;                 /* random-static, per MIND_ADVA */

    hw_timer_init();
    hw_rand_seed_mix(cfg.device_id);    /* decorrelate backoff between units */
    ble_radio_init();
    ble_radio_listen(cfg.listen_ch);

    rq_mtx = tk_cre_mtx(&cmtx);
    rq_flg = tk_cre_flg(&cflg);
    if (rq_mtx <= 0 || rq_flg <= 0) {
        return;                         /* stays !ready: nothing will relay */
    }

    ctsk.itskpri = cfg.tx_task_pri;
    tskid = tk_cre_tsk(&ctsk);
    if (tskid <= 0) {
        return;
    }
    tk_sta_tsk(tskid, 0);

    ready = TRUE;
}

void mesh_originate(const UB *payload, UINT payload_len)
{
    UB adv[BLE_ADV_MAX_DATA];
    UINT len;

    if (!ready) {
        return;
    }

    len = mesh_build_adv(adv, payload, payload_len,
                         cfg.device_id, cfg.device_id, cfg.ttl, 0);
    ble_radio_tx(adv, len, own_adva, BLE_CHAN_MASK_ALL);
    st.originated++;
    st.tx_bursts++;

#if MESH_ENABLE
    /* Record our own message so a relay's copy coming back is recognised as a
       duplicate rather than treated as new. The origin check below catches it
       too; this is the cheaper of the two paths and costs one insert. */
    if (payload_len > PAY_OFF_SEQ) {
        (void)cache_seen(cfg.device_id, payload[PAY_OFF_EVENT],
                         payload[PAY_OFF_SEQ], now_ms());
    }
#endif
}

int mesh_poll_once(void)
{
    ble_rx_t rx;
#if MESH_ENABLE
    mesh_msg_t m;
    UW t;
#endif

    if (!ready || !ble_radio_poll_ex(&rx)) {
        return 0;
    }
    st.rx_total++;

#if MESH_ENABLE
    if (!parse_mesh(&rx, &m)) {
        return 1;                       /* not ours - already counted */
    }
    st.rx_mesh++;

    /* Our own message, relayed back by a neighbour. Rebroadcasting it would
       bounce the message between us until the TTL ran out. */
    if (m.orig_id == cfg.device_id) {
        st.own_dropped++;
        return 1;
    }

    t = now_ms();
    if (cache_seen(m.orig_id, m.payload[PAY_OFF_EVENT], m.payload[PAY_OFF_SEQ], t)) {
        st.dup_dropped++;               /* the flood terminating, as designed */
        return 1;
    }

    if (m.ttl == 0) {
        st.ttl_dropped++;
        return 1;
    }
    if (cfg.role == MESH_ROLE_SOURCE) {
        return 1;                       /* hears the mesh, does not carry it */
    }
#if MESH_FLOOD_CLASS == MESH_FLOOD_INCIDENTS
    if (m.payload[PAY_OFF_EVENT] == MIND_EVT_HEARTBEAT) {
        return 1;
    }
#endif

    /* Hand off and keep listening. The backoff happens on relay_tx_task. */
    relay_enqueue(&m, (UB)(m.ttl - 1), (UB)(m.hops + 1));
    st.relayed++;
#endif /* MESH_ENABLE */

    return 1;
}

void mesh_task(void)
{
    while (1) {
        if (!mesh_poll_once()) {
#if !MESH_POLL_SPIN
            tk_dly_tsk(1);
#endif
        }
    }
}

void mesh_stats(mesh_stats_t *out)
{
    ble_radio_stats_t rs;

    ble_radio_stats(&rs);
    st.radio_dropped = rs.dropped;
    *out = st;
}
