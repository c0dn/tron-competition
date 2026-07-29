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

#if SHOUT_DEBUG
#include <tm/tmonitor.h>
#endif

static H  buf[SHOUT_WIN_SAMPLES] __attribute__((aligned(4)));
static UW sustain_ms;
static UW refractory_ms;
static UINT last_level;     /* raw mic_level of the most recent step */

/* Rolling ambient-level history feeding the adaptive gate. */
static UINT lvl_hist[SHOUT_BASELINE_N];
static UINT lvl_count;      /* valid entries, saturates at SHOUT_BASELINE_N */
static UINT lvl_head;       /* next write index (ring)                      */

#if SHOUT_DEBUG
static UW   dbg_n;          /* window counter, throttles the level trace   */
static UINT dbg_max;        /* peak level since the last trace line        */
#endif

void sound_init(void)
{
    sustain_ms = 0;
    refractory_ms = 0;
    last_level = 0;
    lvl_count = 0;
    lvl_head = 0;
#if SHOUT_DEBUG
    dbg_n = 0;
    dbg_max = 0;
#endif
}

/* Median of the recorded ambient levels. Insertion sort over a <=32 entry copy:
 * a few hundred operations once per gate interval, which is far cheaper than
 * the Goertzel work it guards. Returns 0 before any sample is recorded. */
static UINT ambient_median(void)
{
    UINT tmp[SHOUT_BASELINE_N];
    UINT n = lvl_count;
    UINT i, j;

    if (n == 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        tmp[i] = lvl_hist[i];
    }
    for (i = 1; i < n; i++) {
        UINT key = tmp[i];
        j = i;
        while (j > 0 && tmp[j - 1] > key) {
            tmp[j] = tmp[j - 1];
            j--;
        }
        tmp[j] = key;
    }
    return tmp[n / 2];
}

/* Adaptive Stage A threshold, floored so a near-silent room cannot produce a
 * hair-trigger gate. */
static UINT shout_gate(void)
{
    UINT gate = (ambient_median() * SHOUT_GATE_MULT_PCT) / 100u;

    return (gate < SHOUT_GATE_MIN) ? SHOUT_GATE_MIN : gate;
}

static void ambient_record(UINT level)
{
    lvl_hist[lvl_head] = level;
    lvl_head = (lvl_head + 1u) % SHOUT_BASELINE_N;
    if (lvl_count < SHOUT_BASELINE_N) {
        lvl_count++;
    }
}

/* One gate interval of sustain credit, floored at zero. Used instead of a hard
 * reset so a single marginal window inside a real shout (a breath, a dip
 * between syllables) does not discard the whole sustain history. */
static UW decay_sustain(UW s)
{
    return (s > SHOUT_GATE_INTERVAL_MS) ? (s - SHOUT_GATE_INTERVAL_MS) : 0;
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
    UINT level, gate;
    W    sum;
    INT  i, mean_i, prev, zcr;
    UW   sumsq, rms;
    float mean_f, low_p, high_p;
    BOOL candidate;
    H    mn, mx;

    if (refractory_ms > 0) {
        refractory_ms = (refractory_ms > SHOUT_GATE_INTERVAL_MS)
                        ? refractory_ms - SHOUT_GATE_INTERVAL_MS : 0;
    }

    /* ONE acquisition drives both the gate and the features.
     *
     * These used to be separate: mic_level() sampled ~5 ms, then mic_capture()
     * sampled the NEXT 32 ms. Speech is syllabic, so the gate routinely fired on
     * an onset while the features landed in the gap after it - measured
     * lvl=667 alongside rms=28, and candidate windows that could never be
     * consecutive because every other window saw silence. Sharing one buffer
     * makes the gate and the features describe the same 32 ms. */
    mic_capture(buf, SHOUT_WIN_SAMPLES);

    mn = buf[0];
    mx = buf[0];
    sum = 0;
    for (i = 0; i < SHOUT_WIN_SAMPLES; i++) {
        H s = buf[i];
        if (s < mn) mn = s;
        if (s > mx) mx = s;
        sum += s;
    }

    /* mic_capture() is 12-bit; shift to the 10-bit units the gate, the ambient
     * history and sound_last_level() are all expressed in. */
    level = (UINT)(mx - mn) >> 2;
    last_level = level;
    gate = shout_gate();

    /* Record before gating: the median is robust to the handful of loud windows
     * a shout occupies, so the baseline can keep adapting during an event. */
    ambient_record(level);

#if SHOUT_DEBUG
    /* Trace the gate INPUT unconditionally (throttled, with a peak hold so a
     * short shout cannot slip between lines). Gating this behind the threshold
     * would hide exactly the case being calibrated. */
    if (level > dbg_max) dbg_max = level;
    if ((dbg_n++ & 0x03) == 0) {
        /* min/max are raw 12-bit here: their midpoint is the DC bias and the
         * distance to the nearer rail is the remaining gain headroom. */
        tm_printf((UB *)"snd lvl=%u peak=%u med=%u gate=%u min=%d max=%d\n",
                  level, dbg_max, ambient_median(), gate, (INT)mn, (INT)mx);
        dbg_max = 0;
    }
#endif

    if (level < gate) {
        sustain_ms = decay_sustain(sustain_ms);
        return SOUND_EVT_NONE;
    }

    /* Stage B - features from the buffer already captured above. */
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

#if SHOUT_DEBUG
    {
        /* tm_printf supports only %d/%u/%x/%s - no %f. Report brightness as an
         * integer percentage directly comparable to SHOUT_BRIGHT_RATIO * 100.
         * The raw Goertzel powers reach ~1e12 and would overflow an INT cast,
         * so only the ratio is printed, clamped to a sane range. */
        float ratio = high_p / (low_p + 1.0f);
        INT   bright_pct;

        if (ratio < 0.0f)        bright_pct = 0;
        else if (ratio > 999.0f) bright_pct = 99900;
        else                     bright_pct = (INT)(ratio * 100.0f);

        tm_printf((UB *)"snd lvl=%u rms=%u zcr=%d bright=%d sus=%u cand=%d\n",
                  level, rms, zcr, bright_pct, sustain_ms, (INT)candidate);
    }
#endif

    if (!candidate) {
        sustain_ms = decay_sustain(sustain_ms);
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
