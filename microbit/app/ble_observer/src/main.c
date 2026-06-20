/*
 * ble_observer - connectionless BLE scanner for the uTK-sensor beacon.
 *
 * Passive scan (no connection): hops across the three primary advertising
 * channels (37/38/39), decodes the Manufacturer Specific Data broadcast by
 * ble_beacon (company 0xFFFF: accel x/y/z + mic level), prints the sensor
 * values, and shows the received signal strength (RSSI) as a fill-level
 * bar on the 5x5 LED display. Other BLE advertisers are received too but
 * filtered out by the company id.
 *
 * Watch the serial console (115200); watch the LEDs for the RSSI meter.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "ble_radio.h"
#include "display.h"

#define COMPANY_ID      0xFFFF
#define HOP_DWELL_MS    50          /* time on each channel before hopping */
#define SIGNAL_TIMEOUT  1000        /* clear the bar if no beacon for this long */

static const UINT channels[3] = { 37, 38, 39 };

static UH rd_u16(const UB *p)
{
    return (UH)(p[0] | (p[1] << 8));
}

static UW now_ms(void)
{
    SYSTIM t;
    tk_get_otm(&t);
    return t.lo;                    /* low 32 bits of ms; unsigned diffs wrap-safe */
}

/* Map RSSI (positive, = -dBm) to a 0..5 bar: strong signal -> tall bar. */
static void show_rssi_bar(UINT rssi_dbm)
{
    UB rows[5];
    INT h = (90 - (INT)rssi_dbm) / 9;   /* ~-90 dBm -> 0, ~-45 dBm -> 5 */
    INT y;

    if (h < 0) h = 0;
    if (h > 5) h = 5;

    for (y = 0; y < 5; y++) {
        rows[y] = (y >= 5 - h) ? 0x1F : 0x00;   /* fill from the bottom up */
    }
    display_set_rows(rows);
}

/* Walk the AD structures; if our manufacturer payload is present, print the
   decoded sensor data and update the bar. Returns TRUE if it was ours. */
static BOOL decode(const UB *buf, UINT len, UINT rssi)
{
    UINT adv_len, p;
    const UB *ad;

    if (len < 8 || buf[1] < 6) {
        return FALSE;
    }
    ad = &buf[8];
    adv_len = (UINT)buf[1] - 6;

    for (p = 0; p + 1 < adv_len; ) {
        UINT flen = ad[p];
        UINT ftype = ad[p + 1];
        if (flen == 0 || p + 1 + flen > adv_len) {
            break;
        }
        if (ftype == 0xFF && flen >= 11 && rd_u16(&ad[p + 2]) == COMPANY_ID) {
            H    ax = (H)rd_u16(&ad[p + 4]);
            H    ay = (H)rd_u16(&ad[p + 6]);
            H    az = (H)rd_u16(&ad[p + 8]);
            UINT lvl = rd_u16(&ad[p + 10]);
            UH   node = rd_u16(&buf[6]);

            tm_printf((UB *)"node=0x%04x accel=%d,%d,%d mic=%u rssi=-%d dBm\n",
                      node, ax, ay, az, lvl, rssi);
            show_rssi_bar(rssi);
            return TRUE;
        }
        p += flen + 1;
    }
    return FALSE;
}

LOCAL void observer_task(INT stacd, void *exinf)
{
    UB buf[BLE_RX_MAX];
    UINT len, rssi;
    INT ch_idx = 0;
    UW hop_at = now_ms() + HOP_DWELL_MS;
    UW last_seen = now_ms();

    ble_radio_listen(channels[ch_idx]);
    tm_printf((UB *)"BLE observer hopping 37/38/39 for uTK-sensor\n");

    while (1) {
        UW t = now_ms();

        if (ble_radio_poll(buf, &len, &rssi)) {
            if (decode(buf, len, rssi)) {
                last_seen = t;
            }
        } else {
            tk_dly_tsk(2);
        }

        /* hop to the next advertising channel */
        if ((UW)(t - hop_at) < 0x80000000UL) {      /* t >= hop_at (wrap-safe) */
            ch_idx = (ch_idx + 1) % 3;
            ble_radio_listen(channels[ch_idx]);
            hop_at = t + HOP_DWELL_MS;
        }

        /* blank the meter if the beacon has gone quiet */
        if ((UW)(t - last_seen) > SIGNAL_TIMEOUT) {
            display_clear();
        }
    }
}

EXPORT INT usermain(void)
{
    T_CTSK ctsk = {
        .exinf   = NULL,
        .tskatr  = TA_HLNG | TA_RNG3,
        .task    = (FP)observer_task,
        .itskpri = 10,
        .stksz   = 1024,
    };
    ID tskid;

    ble_radio_init();
    display_init();

    tskid = tk_cre_tsk(&ctsk);
    if (tskid > 0) {
        tk_sta_tsk(tskid, 0);
    } else {
        tm_printf((UB *)"tk_cre_tsk failed: %d\n", tskid);
    }

    tk_slp_tsk(TMO_FEVR);
    return 0;
}
