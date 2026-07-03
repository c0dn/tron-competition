/*
 * fall.c - fall-detection state machine (see fall.h).
 *
 * NORMAL -> FREEFALL (SVM low) -> IMPACT (SVM high within the window) ->
 * post-impact immobility observation -> CONFIRMED (still) or POSSIBLE (moved).
 * Thresholds are in app_config.h. Fed one sample per accel tick.
 */

#include "fall.h"
#include "app_config.h"

static fall_phase_t phase;
static UW t_ms;         /* elapsed time in the current phase   */
static UW still_ms;     /* continuous time observed "still"    */
static UW peak_mg;      /* peak SVM of the impact window        */

void fall_init(void)
{
    phase = FALL_NORMAL;
    t_ms = 0;
    still_ms = 0;
    peak_mg = 0;
}

/* |svm - 1g| as an absolute deviation in milli-g. */
static UW dev_from_1g(UW svm)
{
    return (svm > 1000u) ? (svm - 1000u) : (1000u - svm);
}

UW fall_last_peak(void)
{
    return peak_mg;
}

fall_event_t fall_update(const sensor_sample_t *s)
{
    UW svm = s->svm_mg;

    switch (phase) {
    case FALL_NORMAL:
        if (svm < FALL_FREEFALL_MG) {
            phase = FALL_FREEFALL;
            t_ms = 0;
        }
        break;

    case FALL_FREEFALL:
        t_ms += SAMPLE_INTERVAL_MS;
        if (svm > FALL_IMPACT_MG) {
            phase = FALL_IMPACT;
            t_ms = 0;
            still_ms = 0;
            peak_mg = svm;
        } else if (t_ms > FALL_IMPACT_WINDOW_MS) {
            phase = FALL_NORMAL;    /* no impact followed the free-fall */
        }
        break;

    case FALL_IMPACT:               /* observing post-impact (im)mobility */
        t_ms += SAMPLE_INTERVAL_MS;
        if (svm > peak_mg) peak_mg = svm;

        /* Count continuous stillness (near 1 g). The impact spike itself is
         * not "still", so it simply delays the start of the count. */
        if (dev_from_1g(svm) < FALL_STILL_BAND_MG) {
            still_ms += SAMPLE_INTERVAL_MS;
        } else {
            still_ms = 0;
        }

        if (still_ms >= FALL_IMMOBILE_MS) {
            phase = FALL_NORMAL;
            t_ms = 0;
            return FALL_EVT_CONFIRMED;      /* impact then stayed still */
        }
        if (t_ms >= FALL_OBSERVE_MS) {
            phase = FALL_NORMAL;
            t_ms = 0;
            return FALL_EVT_POSSIBLE;       /* impact but never settled */
        }
        break;

    default:
        phase = FALL_NORMAL;
        break;
    }

    return FALL_EVT_NONE;
}
