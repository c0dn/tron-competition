/*
 * sound.c - always-on, zero-ML shout/scream detector (see sound.h).
 *
 * Stage A: cheap loudness gate (mic_level). Below the gate, do nothing.
 * Stage B: on a loud window, capture ~32 ms and compute RMS energy,
 *          zero-crossing rate, and a high/low band-power ratio (Goertzel).
 * A candidate must be loud AND bright AND scream-like ZCR, and must SUSTAIN
 * across windows before firing; a refractory period rejects re-fires. The
 * sustain requirement is what rejects impulsive transients (slam, drop).
 */

#include "sound.h"
#include "app_config.h"
#include "imath.h"
#include "mic.h"

static H  buf[SHOUT_WIN_SAMPLES] __attribute__((aligned(4)));
static UW sustain_ms;
static UW refractory_ms;
static UINT last_level;     /* raw mic_level of the most recent step */

void sound_init(void)
{
    sustain_ms = 0;
    refractory_ms = 0;
    last_level = 0;
}

/* Goertzel single-bin power for coefficient 'coeff', DC removed. */
static float goertzel_power(const H *x, INT n, float coeff, float mean)
{
    float s1 = 0.0f, s2 = 0.0f, s0;
    INT i;

    for (i = 0; i < n; i++) {
        s0 = coeff * s1 - s2 + ((float)x[i] - mean);
        s2 = s1;
        s1 = s0;
    }
    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

sound_event_t sound_update(void)
{
    UINT level;
    W    sum;
    INT  i, mean_i, prev, zcr;
    UW   sumsq, rms;
    float mean_f, low_p, high_p;
    BOOL candidate;

    if (refractory_ms > 0) {
        refractory_ms = (refractory_ms > SHOUT_GATE_INTERVAL_MS)
                        ? refractory_ms - SHOUT_GATE_INTERVAL_MS : 0;
    }

    /* Stage A - loudness gate. */
    level = mic_level();
    last_level = level;
    if (level < SHOUT_GATE_LEVEL) {
        sustain_ms = 0;
        return SOUND_EVT_NONE;
    }

    /* Stage B - capture and feature extraction. */
    mic_capture(buf, SHOUT_WIN_SAMPLES);

    sum = 0;
    for (i = 0; i < SHOUT_WIN_SAMPLES; i++) {
        sum += buf[i];
    }
    mean_i = (INT)(sum / SHOUT_WIN_SAMPLES);
    mean_f = (float)sum / (float)SHOUT_WIN_SAMPLES;

    /* RMS energy and zero-crossing rate about the DC mean. */
    sumsq = 0;
    zcr = 0;
    prev = (buf[0] - mean_i) >= 0;
    for (i = 0; i < SHOUT_WIN_SAMPLES; i++) {
        W d = (W)buf[i] - mean_i;
        INT sign = (d >= 0);
        sumsq += (UW)(d * d);
        if (sign != prev) zcr++;
        prev = sign;
    }
    rms = usqrt(sumsq / SHOUT_WIN_SAMPLES);

    /* Band brightness: high-band power vs low-band power. */
    low_p  = goertzel_power(buf, SHOUT_WIN_SAMPLES, SHOUT_G_LOW_COEFF,  mean_f);
    high_p = goertzel_power(buf, SHOUT_WIN_SAMPLES, SHOUT_G_HIGH_COEFF, mean_f);

    candidate = (rms > SHOUT_RMS_MIN)
             && (high_p > SHOUT_BRIGHT_RATIO * (low_p + 1.0f))
             && (zcr >= SHOUT_ZCR_MIN)
             && (zcr <= SHOUT_ZCR_MAX);

    if (!candidate) {
        sustain_ms = 0;
        return SOUND_EVT_NONE;
    }

    sustain_ms += SHOUT_GATE_INTERVAL_MS;
    if (sustain_ms >= SHOUT_SUSTAIN_MS && refractory_ms == 0) {
        refractory_ms = SHOUT_REFRACTORY_MS;
        sustain_ms = 0;
        return SOUND_EVT_POSSIBLE_DISTRESS;
    }
    return SOUND_EVT_NONE;
}

UINT sound_last_level(void)
{
    UINT scaled = last_level >> 2;      /* 10-bit p2p -> 0..255 */
    return (scaled > 255) ? 255 : scaled;
}
