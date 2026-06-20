/*
 * ble_beacon - broadcast live sensor data as a BLE beacon (raw radio).
 *
 * No SoftDevice: transmits ADV_NONCONN_IND packets a phone's BLE scanner
 * (nRF Connect, etc.) can discover. Each advertising event reads the
 * accelerometer and microphone and packs them into Manufacturer Specific
 * Data, so the beacon is a live wireless sensor:
 *   - Flags (general discoverable)
 *   - Manufacturer Specific Data: company 0xFFFF + accel x/y/z (int16 LE)
 *     + mic level (uint16 LE)
 *   - Complete Local Name "uTK-sensor"
 * repeating every ~100 ms.
 *
 * Watch the serial console (115200) for live readings.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include "ble_radio.h"
#include "imu.h"
#include "mic.h"

#define ADV_INTERVAL_MS     100
#define COMPANY_ID          0xFFFF      /* SIG test/reserved company id */

static void put_u16(UB *p, UW v)
{
    p[0] = (UB)(v);
    p[1] = (UB)(v >> 8);
}

/* Build the AD structures for this reading into 'buf', return the length. */
static UINT build_adv(UB *buf, H ax, H ay, H az, UINT level)
{
    static const char name[] = "uTK-sensor";
    UINT len = 0;
    UINT i;

    /* Flags: LE General Discoverable, BR/EDR not supported */
    buf[len++] = 0x02;
    buf[len++] = 0x01;
    buf[len++] = 0x06;

    /* Manufacturer Specific Data: company(2) + ax,ay,az(6) + level(2) */
    buf[len++] = 0x0B;                  /* length: type + 2 + 8 */
    buf[len++] = 0xFF;
    put_u16(&buf[len], COMPANY_ID);     len += 2;
    put_u16(&buf[len], (UW)(UH)ax);     len += 2;
    put_u16(&buf[len], (UW)(UH)ay);     len += 2;
    put_u16(&buf[len], (UW)(UH)az);     len += 2;
    put_u16(&buf[len], level);          len += 2;

    /* Complete Local Name */
    buf[len++] = (UB)(1 + (sizeof(name) - 1));
    buf[len++] = 0x09;
    for (i = 0; i < sizeof(name) - 1; i++) {
        buf[len++] = (UB)name[i];
    }

    return len;
}

LOCAL void beacon_task(INT stacd, void *exinf)
{
    UB adv[BLE_ADV_MAX_DATA];
    UW n = 0;

    tm_printf((UB *)"BLE sensor beacon advertising as 'uTK-sensor'\n");

    while (1) {
        H ax = 0, ay = 0, az = 0;
        UINT level;
        UINT len;

        imu_read(&ax, &ay, &az);
        level = mic_level();

        len = build_adv(adv, ax, ay, az, level);
        ble_radio_advertise(adv, len, NULL);

        if ((n++ & 0x1F) == 0) {
            tm_printf((UB *)"accel=%d,%d,%d mic=%u\n", ax, ay, az, level);
        }
        tk_dly_tsk(ADV_INTERVAL_MS);
    }
}

EXPORT INT usermain(void)
{
    T_CTSK ctsk = {
        .exinf   = NULL,
        .tskatr  = TA_HLNG | TA_RNG3,
        .task    = (FP)beacon_task,
        .itskpri = 10,
        .stksz   = 1024,
    };
    ID tskid;
    INT who;

    ble_radio_init();

    who = imu_init();
    if (who == LSM303AGR_WHOAMI) {
        tm_printf((UB *)"IMU: LSM303AGR (WHO_AM_I=0x%02x)\n", who);
    } else {
        tm_printf((UB *)"IMU: unexpected WHO_AM_I=0x%02x\n", who);
    }
    mic_init();

    tskid = tk_cre_tsk(&ctsk);
    if (tskid > 0) {
        tk_sta_tsk(tskid, 0);
    } else {
        tm_printf((UB *)"tk_cre_tsk failed: %d\n", tskid);
    }

    tk_slp_tsk(TMO_FEVR);
    return 0;
}
