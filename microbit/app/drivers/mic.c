/*
 * mic.c - micro:bit v2 microphone via SAADC (see mic.h)
 *
 * MIC analog signal -> P0.05 (SAADC AIN3).  MIC enable -> P0.20 (high).
 * mic_level() bursts a window of samples into a RAM buffer and returns
 * the peak-to-peak amplitude.
 */

#include "mic.h"
#include "gpio.h"

/* --- SAADC peripheral (base 0x40007000) --- */
#define SAADC_BASE          0x40007000UL
#define A(off)              (SAADC_BASE + (off))

#define SAADC_TASKS_START   A(0x000)
#define SAADC_TASKS_SAMPLE  A(0x004)
#define SAADC_TASKS_STOP    A(0x008)
#define SAADC_TASKS_CALIBRATEOFFSET A(0x00C)
#define SAADC_EVENTS_STARTED A(0x100)
#define SAADC_EVENTS_END    A(0x104)
#define SAADC_EVENTS_RESULTDONE A(0x10C)
#define SAADC_EVENTS_CALIBRATEDONE A(0x110)
#define SAADC_EVENTS_STOPPED A(0x114)
#define SAADC_ENABLE        A(0x500)
#define SAADC_CH0_PSELP     A(0x510)
#define SAADC_CH0_PSELN     A(0x514)
#define SAADC_CH0_CONFIG    A(0x518)
#define SAADC_RESOLUTION    A(0x5F0)
#define SAADC_SAMPLERATE    A(0x5F8)
#define SAADC_RESULT_PTR    A(0x62C)
#define SAADC_RESULT_MAXCNT A(0x630)

#define PSELP_AIN3          4           /* AIN3 = P0.05 */
#define RESOLUTION_10BIT    1
#define RESOLUTION_12BIT    2
#define SAMPLERATE_MODE_TIMER (1UL << 12)   /* internal timer drives sampling */

/* CONFIG: gain 1/6, internal 0.6V ref, TACQ 40us, single-ended.
 * Gain stays low because the mic signal carries a DC bias (single-ended,
 * so absolute) - higher gain would rail the ADC on the bias. Longer
 * acquisition (40us) settles better against the mic's source impedance. */
#define CH_CONFIG           0x00050000

#define PIN_MIC_ENABLE      PIN(0, 20)

#define MIC_SAMPLES         128
static H sample_buf[MIC_SAMPLES] __attribute__((aligned(4)));

void mic_init(void)
{
    gpio_make_output(PIN_MIC_ENABLE);
    gpio_high(PIN_MIC_ENABLE);           /* power the microphone */

    out_w(SAADC_ENABLE, 0);
    out_w(SAADC_CH0_PSELP, PSELP_AIN3);
    out_w(SAADC_CH0_PSELN, 0);
    out_w(SAADC_CH0_CONFIG, CH_CONFIG);
    out_w(SAADC_RESOLUTION, RESOLUTION_10BIT);
    out_w(SAADC_ENABLE, 1);

    /* one-time offset calibration for a cleaner zero */
    out_w(SAADC_EVENTS_CALIBRATEDONE, 0);
    out_w(SAADC_TASKS_CALIBRATEOFFSET, 1);
    while (in_w(SAADC_EVENTS_CALIBRATEDONE) == 0) {}
    out_w(SAADC_EVENTS_CALIBRATEDONE, 0);
}

UINT mic_level(void)
{
    INT i;
    H mn, mx;

    out_w(SAADC_RESULT_PTR, (UW)sample_buf);
    out_w(SAADC_RESULT_MAXCNT, MIC_SAMPLES);

    out_w(SAADC_EVENTS_STARTED, 0);
    out_w(SAADC_EVENTS_END, 0);
    out_w(SAADC_TASKS_START, 1);
    while (in_w(SAADC_EVENTS_STARTED) == 0) {}

    for (i = 0; i < MIC_SAMPLES; i++) {
        out_w(SAADC_EVENTS_RESULTDONE, 0);
        out_w(SAADC_TASKS_SAMPLE, 1);
        while (in_w(SAADC_EVENTS_RESULTDONE) == 0) {}
    }

    while (in_w(SAADC_EVENTS_END) == 0) {}   /* DMA flushed */
    out_w(SAADC_EVENTS_END, 0);
    out_w(SAADC_TASKS_STOP, 1);
    while (in_w(SAADC_EVENTS_STOPPED) == 0) {}
    out_w(SAADC_EVENTS_STOPPED, 0);

    mn = sample_buf[0];
    mx = sample_buf[0];
    for (i = 1; i < MIC_SAMPLES; i++) {
        H s = sample_buf[i];
        if (s < mn) mn = s;
        if (s > mx) mx = s;
    }
    return (UINT)(mx - mn);
}

void mic_capture(H *buf, UINT n)
{
    /* 16 MHz peripheral clock / CC = sample rate */
    UW cc = 16000000UL / MIC_CAPTURE_RATE;

    out_w(SAADC_RESOLUTION, RESOLUTION_12BIT);
    out_w(SAADC_SAMPLERATE, SAMPLERATE_MODE_TIMER | (cc & 0x7FF));
    out_w(SAADC_RESULT_PTR, (UW)buf);
    out_w(SAADC_RESULT_MAXCNT, n);

    out_w(SAADC_EVENTS_STARTED, 0);
    out_w(SAADC_EVENTS_END, 0);
    out_w(SAADC_TASKS_START, 1);
    while (in_w(SAADC_EVENTS_STARTED) == 0) {}

    /* one SAMPLE kicks off the internal timer; it free-runs to MAXCNT */
    out_w(SAADC_TASKS_SAMPLE, 1);
    while (in_w(SAADC_EVENTS_END) == 0) {}
    out_w(SAADC_EVENTS_END, 0);

    out_w(SAADC_TASKS_STOP, 1);
    while (in_w(SAADC_EVENTS_STOPPED) == 0) {}
    out_w(SAADC_EVENTS_STOPPED, 0);

    /* restore task-driven 10-bit sampling so mic_level() still works */
    out_w(SAADC_SAMPLERATE, 0);
    out_w(SAADC_RESOLUTION, RESOLUTION_10BIT);
}
