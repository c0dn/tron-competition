/*
 * display.c - micro:bit v2 5x5 LED matrix driver (see display.h)
 *
 * micro:bit v2 matrix pin map (nRF52833):
 *   ROW1..5 = P0.21 P0.22 P0.15 P0.24 P0.19  (anodes, drive HIGH)
 *   COL1..5 = P0.28 P0.11 P0.31 P1.05 P0.30  (cathodes, drive LOW = lit)
 * Note COL4 is on GPIO port 1; the gpio helpers handle that.
 *
 * A background task drives one row at a time. With CNF_TIMER_PERIOD = 1ms
 * each row is held ~1ms, so a full frame is ~5ms (~200 Hz) - flicker free.
 */

#include "display.h"
#include "gpio.h"

static const UINT row_pin[5] = {
    PIN(0, 21), PIN(0, 22), PIN(0, 15), PIN(0, 24), PIN(0, 19)
};
static const UINT col_pin[5] = {
    PIN(0, 28), PIN(0, 11), PIN(0, 31), PIN(1, 5), PIN(0, 30)
};

/* framebuffer: fb[y] bit x  ->  pixel at column x, row y */
static volatile UB fb[5];

/* Immutable 5x5 glyphs for the six physical routed-board positions. */
static const UB role_digit_glyphs[6][5] = {
    { 0x04u, 0x06u, 0x04u, 0x04u, 0x0eu },
    { 0x0eu, 0x11u, 0x08u, 0x04u, 0x1fu },
    { 0x0eu, 0x11u, 0x0cu, 0x11u, 0x0eu },
    { 0x08u, 0x0cu, 0x0au, 0x1fu, 0x08u },
    { 0x1fu, 0x01u, 0x0fu, 0x10u, 0x0fu },
    { 0x0eu, 0x01u, 0x0fu, 0x11u, 0x0eu },
};

static void all_off(void)
{
    INT i;
    for (i = 0; i < 5; i++) {
        gpio_low(row_pin[i]);     /* rows low  = no source  */
        gpio_high(col_pin[i]);    /* cols high = no sink     */
    }
}

static void configure_pins(void)
{
    INT i;

    for (i = 0; i < 5; i++) {
        gpio_make_output(row_pin[i]);
        gpio_make_output(col_pin[i]);
    }
    all_off();
}

static void scan_rows(const UB rows[5], UINT scan_ms)
{
    UINT elapsed = 0u;

    while (elapsed < scan_ms) {
        INT y;

        for (y = 0; y < 5 && elapsed < scan_ms; y++) {
            INT x;
            UB mask = rows[y];

            all_off();
            gpio_high(row_pin[y]);
            for (x = 0; x < 5; x++) {
                if ((mask & (1U << x)) != 0u) {
                    gpio_low(col_pin[x]);
                }
            }
            (void)tk_dly_tsk(1u);
            elapsed++;
        }
    }
}

static void display_task(INT stacd, void *exinf)
{
    INT y = 0;

    while (1) {
        INT x;
        UB mask = fb[y];

        all_off();
        gpio_high(row_pin[y]);            /* source this row */
        for (x = 0; x < 5; x++) {
            if (mask & (1U << x)) {
                gpio_low(col_pin[x]);     /* sink lit columns */
            }
        }

        tk_dly_tsk(1);                    /* hold ~1ms */

        y++;
        if (y >= 5) {
            y = 0;
        }
    }
}

void display_init(void)
{
    T_CTSK ctsk = {
        .exinf   = NULL,
        .tskatr  = TA_HLNG | TA_RNG3,
        .task    = (FP)display_task,
        .itskpri = 8,                     /* higher than the app task */
        .stksz   = 512,
    };
    ID tskid;
    configure_pins();
    display_clear();

    tskid = tk_cre_tsk(&ctsk);
    if (tskid > 0) {
        tk_sta_tsk(tskid, 0);
    }
}

void display_clear(void)
{
    INT y;
    for (y = 0; y < 5; y++) {
        fb[y] = 0;
    }
}

void display_fill(void)
{
    INT y;
    for (y = 0; y < 5; y++) {
        fb[y] = 0x1F;                     /* all 5 columns */
    }
}

void display_set_pixel(INT x, INT y, BOOL on)
{
    if (x < 0 || x > 4 || y < 0 || y > 4) {
        return;
    }
    if (on) {
        fb[y] |= (UB)(1U << x);
    } else {
        fb[y] &= (UB)~(1U << x);
    }
}

void display_show_digit(UINT digit)
{
    if (digit < 1u || digit > 6u) {
        return;
    }
    display_set_rows(role_digit_glyphs[digit - 1u]);
}

void display_show_benchmark_role(UINT role, UINT scan_ms)
{
    if (role < 1u || role > 6u) {
        return;
    }
    configure_pins();
    scan_rows(role_digit_glyphs[role - 1u], scan_ms);
    all_off();
}

void display_set_rows(const UB rows[5])
{
    INT y;
    for (y = 0; y < 5; y++) {
        fb[y] = rows[y] & 0x1F;
    }
}
