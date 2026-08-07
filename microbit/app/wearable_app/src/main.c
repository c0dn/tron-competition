/*
 * wearable_app - competition wearable firmware (micro:bit v2 / uT-Kernel 3.0).
 *
 * Zero-ML incident detection, decomposed into deterministic tasks:
 *   sensor_task   (pri 4, 25 Hz)  - read accel, SVM, drive the fall FSM
 *   sound_task    (pri 5, ~150 ms)- owns the mic; always-on shout detector
 *   fusion_task   (pri 3, on flag)- fuse fall + shout -> incident + confidence
 *   advertise_task(pri 6, adaptive)- pack schema v1 + advertise (fixed AdvA)
 *
 * The mic-owning task is separate and lower priority so its blocking capture
 * never disturbs the accel cadence. Comms: an event flag (detector -> fusion)
 * plus a mutex-guarded shared state. Wire format is the shared contract
 * (shared/schema.h). Serial console at 115200.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "app_config.h"
#include "schema.h"
#include "sensors.h"
#include "fall.h"
#include "sound.h"
#include "fusion.h"
#include "tx_adapter.h"
#include "ble_emit.h"
#include "imu.h"        /* LSM303AGR_WHOAMI */

#define FLG_FALL    (0x01U)
#define FLG_SOUND   (0x02U)

static ID g_mtx;
static ID g_flg;

static struct {
    UW              cur_svm;        /* latest SVM (for heartbeats)        */
    fall_event_t    fall_evt;       /* pending fall event                 */
    sound_event_t   sound_evt;      /* pending shout event                */
    UW              fall_peak;      /* peak SVM of the last fall window    */
    UB              sound_level;    /* scaled loudness of the last shout  */
    tx_adapter_t    tx;             /* live events + heartbeat scheduling  */
    UW              next_msg_id;    /* one id space for events + heartbeats */
} g;

/* One counter for everything this device originates.
 *
 * There used to be two - fusion's seq for events and advertise_task's hb_seq
 * for heartbeats - both written into the same one-byte payload field, so the
 * pair only stayed distinguishable because receivers happened to include
 * event_type in their dedup key. A single space removes that coupling, and it
 * is what lands in the mesh seq24, where 24 bits make collisions a non-issue
 * instead of something that recurs every 256 messages.
 *
 * Assigned once per logical message; retransmissions reuse it. */
static UW next_msg_id(void)
{
    return g.next_msg_id++;
}

static void lock(void)   { tk_loc_mtx(g_mtx, TMO_FEVR); }
static void unlock(void) { tk_unl_mtx(g_mtx); }

/* Monotonic millisecond clock for fusion's coincidence window. The low 32 bits
 * of the kernel's operating time are enough: fusion only ever takes unsigned
 * differences, which stay correct across the wrap. */
static UW now_ms(void)
{
    SYSTIM t;

    if (tk_get_otm(&t) != E_OK) {
        return 0;
    }
    return (UW)t.lo;
}

/* ---- sensor_task: accel sampling + fall state machine ------------------- */
LOCAL void sensor_task(INT stacd, void *exinf)
{
    UW n = 0;

    while (1) {
        sensor_sample_t s;

        if (sensors_read(&s) == 0) {
            fall_event_t fe;

            lock();
            g.cur_svm = s.svm_mg;
            unlock();

            fe = fall_update(&s);
            if (fe != FALL_EVT_NONE) {
                lock();
                g.fall_evt = fe;
                g.fall_peak = fall_last_peak();
                unlock();
                tk_set_flg(g_flg, FLG_FALL);
            }

            if ((n++ % LOG_EVERY_N) == 0) {
                tm_printf((UB *)"accel=%d,%d,%d svm=%u mg\n",
                          s.ax, s.ay, s.az, s.svm_mg);
            }
        }
        tk_dly_tsk(SAMPLE_INTERVAL_MS);
    }
}

/* ---- sound_task: always-on shout detector (owns the mic) ---------------- */
LOCAL void sound_task(INT stacd, void *exinf)
{
    while (1) {
        sound_event_t se = sound_update();

        if (se != SOUND_EVT_NONE) {
            lock();
            g.sound_evt = se;
            g.sound_level = (UB)sound_last_level();
            unlock();
            tk_set_flg(g_flg, FLG_SOUND);
            tm_printf((UB *)"shout detected (level=%u)\n", sound_last_level());
        }
        tk_dly_tsk(SHOUT_GATE_INTERVAL_MS);
    }
}

/* ---- fusion_task: combine the two event sources ------------------------- */
LOCAL void fusion_task(INT stacd, void *exinf)
{
    while (1) {
        UINT ptn;
        fall_event_t  fe;
        sound_event_t se;
        UW asvm, peak, cur;
        UB mlvl;
        incident_state_t inc;
        BOOL admitted;
        UB active;

        tk_wai_flg(g_flg, FLG_FALL | FLG_SOUND,
                   TWF_ORW | TWF_CLR, &ptn, TMO_FEVR);

        lock();
        fe = g.fall_evt;   se = g.sound_evt;
        peak = g.fall_peak; cur = g.cur_svm; mlvl = g.sound_level;
        g.fall_evt = FALL_EVT_NONE;
        g.sound_evt = SOUND_EVT_NONE;
        unlock();

        asvm = (fe != FALL_EVT_NONE) ? peak : cur;
        mlvl = (se != SOUND_EVT_NONE) ? mlvl : 0;

        if (!fusion_update(fe, se, asvm, mlvl, now_ms(), &inc)) {
            continue;           /* latch updated, nothing new to advertise */
        }

        lock();
        inc.event_id = next_msg_id();
        inc.seq = (UB)(inc.event_id & 0xFFu);
        admitted = tx_adapter_admit(&g.tx, &inc, now_ms());
        active = tx_adapter_active(&g.tx);
        unlock();

        tm_printf((UB *)"EVENT type=%u conf=%u svm=%u mic=%u seq=%u %s active=%u\n",
                  inc.event_type, inc.confidence, inc.accel_svm,
                  inc.mic_level, inc.seq,
                  admitted ? (UB *)"admitted" : (UB *)"DROPPED", active);
    }
}

/* ---- advertise_task: fixed-tick poll, schema-v1 emit -------------------- */
/*
 * The cadence lives in tx_adapter, which schedules against absolute deadlines.
 * This task only polls it at a fixed tick and puts whatever it returns on air.
 *
 * The tick is deliberately uniform rather than "sleep until the next thing is
 * due": tk_dly_tsk() quantises to CNF_TIMER_PERIOD (10 ms here) and may
 * overshoot by a full tick, so a chain of variable sleeps accumulates error
 * across a burst. Polling on a fixed tick and comparing absolute deadlines
 * keeps the drift bounded to one tick no matter how long the burst runs.
 */
LOCAL void advertise_task(INT stacd, void *exinf)
{
    while (1) {
        incident_state_t out;
        tx_adapter_send_t what;

        lock();
        what = tx_adapter_next(&g.tx, now_ms(), &out);
        if (what == TX_ADAPTER_SEND_HEARTBEAT) {
            UW id = next_msg_id();

            out.event_type = MIND_EVT_HEARTBEAT;
            out.confidence = 0;
            out.accel_svm = g.cur_svm;
            out.mic_level = 0;
            out.event_id = id;
            out.seq = (UB)(id & 0xFFu);
        }
        unlock();

        if (what != TX_ADAPTER_SEND_NONE) {
            ble_emit_advertise(&out);
        }
        tk_dly_tsk(TX_TICK_MS);
    }
}

static ID make_task(FP entry, PRI pri)
{
    T_CTSK ctsk = {
        .exinf  = NULL,
        .tskatr = TA_HLNG | TA_RNG3,
        .task   = entry,
        .itskpri = pri,
        .stksz  = 1024,
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
    T_CMTX cmtx = { .exinf = NULL, .mtxatr = TA_INHERIT, .ceilpri = 0 };
    T_CFLG cflg = { .exinf = NULL, .flgatr = TA_TFIFO | TA_WMUL, .iflgptn = 0 };
    INT who;

    ble_emit_init();

    who = sensors_init();
    if (who == LSM303AGR_WHOAMI) {
        tm_printf((UB *)"IMU: LSM303AGR (WHO_AM_I=0x%02x) @ +/-%dg\n",
                  who, IMU_FULLSCALE_G);
    } else {
        tm_printf((UB *)"IMU: unexpected WHO_AM_I=0x%02x\n", who);
    }

    fall_init();
    sound_init();
    fusion_init();
    tx_adapter_init(&g.tx, EVENT_TX_BUDGET_MS, EVENT_TX_INTERVAL_MS,
                    EVENT_TX_MIN_COUNT, HEARTBEAT_INTERVAL_MS, now_ms());

    g_mtx = tk_cre_mtx(&cmtx);
    g_flg = tk_cre_flg(&cflg);
    if (g_mtx <= 0 || g_flg <= 0) {
        tm_printf((UB *)"kernel object create failed (mtx=%d flg=%d)\n",
                  g_mtx, g_flg);
    }

    tm_printf((UB *)"wearable_app: detection running (zero-ML)\n");

    make_task((FP)fusion_task,    3);
    make_task((FP)sensor_task,    4);
    make_task((FP)sound_task,     5);
    make_task((FP)advertise_task, 6);

    tk_slp_tsk(TMO_FEVR);
    return 0;
}
