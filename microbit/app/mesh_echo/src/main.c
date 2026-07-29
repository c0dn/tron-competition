/*
 * mesh_echo - bench check for the driver layer the flood mesh sits on.
 *
 * Covers two prerequisites, each reported separately so a failure points at one
 * of them and not the other:
 *   1. transmit-after-receive in ble_radio.c   (the echo loop)
 *   2. the microsecond time base in hw_timer.c (the boot self-check)
 *
 * WHY THIS EXISTS (do not delete - it has no product function by design)
 * ---------------------------------------------------------------------
 * ble_radio.c was written as a beacon driver: it only ever transmitted, and
 * ble_observer only ever received. Nothing exercised a transmit that FOLLOWS a
 * receive, so a defect sat latent in that path:
 *
 *   ble_radio_init()   sets SHORTS = READY_START | END_DISABLE
 *   ble_radio_listen() OVERWRITES it with READY_START | ADDRESS_RSSISTART |
 *                      END_START
 *   tx_on_channel()    never wrote SHORTS at all - it inherited whatever was
 *                      left behind, and never called radio_disable() either
 *
 * After any listen(), END_START replaces END_DISABLE, so a transmit sequences
 * TXEN -> READY -> START -> END -> START -> ... and EVENTS_DISABLED is never
 * raised. The busy-wait in tx_on_channel() then spins forever. Keying TASKS_TXEN
 * straight out of RX is invalid anyway; the RADIO state machine has to pass
 * through DISABLED.
 *
 * A flood-mesh relay does exactly transmit-after-receive, continuously, so this
 * had to be fixed before any mesh work could start. This app is the instrument
 * that proved it and the regression test that keeps it fixed.
 *
 * WHAT IT DOES
 * ------------
 * Camps on one advertising channel, and on each received packet transmits a
 * marker advert. Roughly eighty lines, no mesh logic, no protocol layer: it
 * isolates the driver behaviour and nothing else.
 *
 * It deliberately does NOT re-arm the receiver itself. Restoring RX after a
 * transmit burst is the driver's job, so leaving it out lets this app tell three
 * outcomes apart from the serial log alone:
 *
 *   "TX START" then silence         -> the SHORTS defect is back. The busy-wait
 *                                      is spinning; the task is wedged.
 *   one TX START/TX DONE, then the
 *   alive line keeps printing but
 *   rx= stops climbing              -> transmit works, but the driver is not
 *                                      returning to RX. The node has gone deaf.
 *   rx= and echo= both climbing     -> PASS.
 *
 * HOW TO RUN (needs two micro:bits; a third receiver is optional)
 * --------------------------------------------------------------
 *   board A, any beacon source:
 *     ./flash.sh ble_beacon --uid <PROBE_A>
 *   board B, this app:
 *     ./flash.sh mesh_echo  --uid <PROBE_B> --monitor
 *
 * Markers go out as a schema-v1 advert under device id 0xEE, so the ESP32-C3
 * ble_sniffer decodes them with no changes if one is to hand. accel_svm carries
 * the running receive count and seq the echo count, which makes loss visible
 * from the receiving end too.
 *
 * The alive line also reports ble_radio_stats(). hw= is counted by the radio
 * itself over PPI and sw= by the poll loop; a growing gap means packets are
 * being overwritten before anyone reads them, which is the one failure that
 * would otherwise make a broken mesh look like a working one.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "ble_radio.h"
#include "hw_timer.h"
#include "schema.h"

/* Marker identity. 0xEE is outside the wearable id range so these adverts are
   never mistaken for a real device on a shared sniffer. */
#define ECHO_DEVICE_ID   0xEE
#define ECHO_CHANNEL     37
/* Ambient BLE traffic can trigger constantly; this keeps the log readable and
   the radio from saturating once the driver actually works. */
#define ECHO_MIN_GAP_MS  500
#define ALIVE_EVERY_MS   2000

static const UB echo_adva[6] = MIND_ADVA(ECHO_DEVICE_ID);

static UW now_ms(void)
{
    SYSTIM t;

    if (tk_get_otm(&t) != E_OK) {
        return 0;
    }
    return (UW)t.lo;
}

/* Frame a schema-v1 advert: Flags + Manufacturer Specific Data, same layout as
   the wearable's ble_emit.c so existing decoders need no special case. */
static UINT build_marker(UB *buf, UH rx_count, UB seq)
{
    mind_adv_payload_t p;
    const UB *pb = (const UB *)&p;
    UINT len = 0;
    UINT i;

    p.schema_version = MIND_SCHEMA_VERSION;
    p.event_type     = MIND_EVT_HEARTBEAT;
    p.confidence     = 0;
    p.accel_svm      = rx_count;   /* debug channel: receives so far */
    p.mic_level      = 0;
    p.seq            = seq;        /* debug channel: echoes so far   */

    buf[len++] = 0x02;
    buf[len++] = MIND_AD_TYPE_FLAGS;
    buf[len++] = 0x06;

    buf[len++] = (UB)(1 + 2 + MIND_PAYLOAD_SIZE);
    buf[len++] = MIND_AD_TYPE_MSD;
    buf[len++] = (UB)(MIND_COMPANY_ID & 0xFF);
    buf[len++] = (UB)(MIND_COMPANY_ID >> 8);
    for (i = 0; i < MIND_PAYLOAD_SIZE; i++) {
        buf[len++] = pb[i];
    }
    return len;
}

/* Boot self-check for hw_timer: request a known delay repeatedly and measure it
   against the same clock. Runs above HW_TIMER_SPIN_MAX_US so it exercises the
   compare-and-suspend path rather than the spin shortcut - that is the path the
   relay backoff will use, and the one that can silently deadlock if the
   interrupt never arrives. A hang here means the TIMER1 interrupt is not
   reaching the handler. */
#define SELFTEST_N          100
#define SELFTEST_TARGET_US  1000

static void timer_selfcheck(void)
{
    UW worst = 0;
    UW total = 0;
    INT i;

    tm_printf((UB *)"hw_timer: %d delays of %d us...\n",
              SELFTEST_N, SELFTEST_TARGET_US);

    for (i = 0; i < SELFTEST_N; i++) {
        UW t0 = hw_timer_now();
        UW dt, err;

        hw_timer_delay_us(SELFTEST_TARGET_US);
        dt = hw_timer_elapsed_us(t0);

        err = (dt > SELFTEST_TARGET_US) ? (dt - SELFTEST_TARGET_US)
                                        : (SELFTEST_TARGET_US - dt);
        if (err > worst) {
            worst = err;
        }
        total += dt;
    }

    tm_printf((UB *)"hw_timer: mean %u us, worst error %u us, rand %08x\n",
              total / SELFTEST_N, worst, hw_rand32());
}

LOCAL void echo_task(INT stacd, void *exinf)
{
    UB rxbuf[BLE_RX_MAX];
    UB adv[BLE_ADV_MAX_DATA];
    UINT len, rssi;
    UW rx_count = 0;
    UW echo_count = 0;
    UW last_echo = 0;
    UW last_alive = 0;

    timer_selfcheck();

    tm_printf((UB *)"mesh_echo: camping on ch%d, echoing as dev 0x%02x\n",
              ECHO_CHANNEL, ECHO_DEVICE_ID);
    tm_printf((UB *)"expect: 'TX START' followed by 'TX DONE'. START with no\n");
    tm_printf((UB *)"DONE means the transmit-after-receive defect is back.\n");

    ble_radio_listen(ECHO_CHANNEL);

    while (1) {
        UW t = now_ms();

        if (ble_radio_poll(rxbuf, &len, &rssi)) {
            rx_count++;

            if (rx_count == 1 || (UW)(t - last_echo) >= ECHO_MIN_GAP_MS) {
                UINT alen = build_marker(adv, (UH)rx_count, (UB)echo_count);

                tm_printf((UB *)"rx #%u (len=%u rssi=-%u dBm) -> TX START\n",
                          rx_count, len, rssi);
                /* If the defect is present, control never returns from here. */
                ble_radio_advertise(adv, alen, echo_adva);
                tm_printf((UB *)"    TX DONE (echo #%u)\n", echo_count);

                echo_count++;
                last_echo = t;
            }
        } else {
            tk_dly_tsk(2);
        }

        /* Proof of life. If this keeps printing while rx stops climbing, the
           driver transmitted but never went back to receive. */
        if ((UW)(t - last_alive) >= ALIVE_EVERY_MS) {
            ble_radio_stats_t st;

            ble_radio_stats(&st);
            tm_printf((UB *)"alive: rx=%u echo=%u | hw=%u sw=%u crc_err=%u dropped=%u\n",
                      rx_count, echo_count,
                      st.hw_end, st.observed, st.crc_err, st.dropped);
            last_alive = t;
        }
    }
}

EXPORT INT usermain(void)
{
    T_CTSK ctsk = {
        .exinf   = NULL,
        .tskatr  = TA_HLNG | TA_RNG3,
        .task    = (FP)echo_task,
        .itskpri = 10,
        .stksz   = 1024,
    };
    ID tskid;

    hw_timer_init();
    hw_rand_seed_mix(ECHO_DEVICE_ID);
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
