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
static UW t_ms;         /* elapsed time in the current phase */
static UW peak_mg;      /* peak SVM of the impact window      */
static UW smin, smax;   /* SVM spread during the immobility window */

void fall_init(void)
{
    phase = FALL_NORMAL;
    t_ms = 0;
    peak_mg = 0;
    smin = 0;
    smax = 0;
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
            peak_mg = svm;
            smin = svm;
            smax = svm;
        } else if (t_ms > FALL_IMPACT_WINDOW_MS) {
            phase = FALL_NORMAL;    /* no impact followed the free-fall */
        }
        break;

    case FALL_IMPACT:               /* observing post-impact (im)mobility */
        t_ms += SAMPLE_INTERVAL_MS;
        if (svm > peak_mg) peak_mg = svm;
        if (svm < smin)    smin = svm;
        if (svm > smax)    smax = svm;

        if (t_ms >= FALL_IMMOBILE_MS) {
            fall_event_t e = ((smax - smin) < FALL_IMMOBILE_RANGE_MG)
                               ? FALL_EVT_CONFIRMED   /* stayed still */
                               : FALL_EVT_POSSIBLE;   /* motion resumed */
            phase = FALL_NORMAL;
            t_ms = 0;
            return e;
        }
        break;

    default:
        phase = FALL_NORMAL;
        break;
    }

    return FALL_EVT_NONE;
}
