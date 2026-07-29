/*
 * fusion.c - rule-based fusion of the two detectors (see fusion.h).
 *
 * Two independent sources (fall state machine, shout detector) map to a single
 * incident and confidence. Event numbers come from the shared beacon contract
 * (schema.h). Zero ML.
 *
 * Coincidence: the sources do not fire together. A shout confirms ~350 ms after
 * it starts, whereas a fall event is only reported once the post-impact
 * immobility window resolves (1500-3000 ms after the impact), so a person who
 * shouts as they land produces the shout first. Each source is therefore
 * latched for FUSION_COINCIDENCE_MS and fused in either order:
 *
 *   shout then fall -> the fall resolves onto a live shout, emitted directly
 *                      as FALL_AND_SHOUT (one event).
 *   fall then shout -> the fall is emitted immediately (never delay an alarm),
 *                      then upgraded to FALL_AND_SHOUT by a second event with a
 *                      fresh seq. The observer de-dups on
 *                      (device_id, event_type, seq), so the upgrade is a
 *                      distinct record and needs no observer-side change.
 *
 * A fused pair is consumed so it cannot fire twice.
 */

#include "fusion.h"
#include "app_config.h"
#include "schema.h"

static UB seq;

/* Latched sources. *_ms are valid only while *_live is TRUE. */
static fall_event_t  last_fall;
static UW            last_fall_ms;
static UW            last_fall_svm;
static BOOL          fall_live;

static UB            last_shout_level;
static UW            last_shout_ms;
static BOOL          shout_live;

void fusion_init(void)
{
    seq = 0;
    last_fall = FALL_EVT_NONE;
    last_fall_ms = 0;
    last_fall_svm = 0;
    fall_live = FALSE;
    last_shout_level = 0;
    last_shout_ms = 0;
    shout_live = FALSE;
}

/* Unsigned difference stays correct across the 32-bit millisecond wrap. */
static BOOL within_window(UW now_ms, UW then_ms)
{
    return (BOOL)((UW)(now_ms - then_ms) <= FUSION_COINCIDENCE_MS);
}

BOOL fusion_update(fall_event_t fe, sound_event_t se,
                   UW accel_svm, UB mic_level, UW now_ms,
                   incident_state_t *out)
{
    BOOL fall_now  = (fe == FALL_EVT_POSSIBLE) || (fe == FALL_EVT_CONFIRMED);
    BOOL shout_now = (se == SOUND_EVT_POSSIBLE_DISTRESS);
    UB   et;
    UINT conf;
    UW   svm;
    UB   mic;

    /* Latch whatever arrived on this wake. */
    if (fall_now) {
        last_fall = fe;
        last_fall_ms = now_ms;
        last_fall_svm = accel_svm;
        fall_live = TRUE;
    }
    if (shout_now) {
        last_shout_level = mic_level;
        last_shout_ms = now_ms;
        shout_live = TRUE;
    }

    /* Expire stale latches. */
    if (fall_live && !within_window(now_ms, last_fall_ms)) {
        fall_live = FALSE;
        last_fall = FALL_EVT_NONE;
    }
    if (shout_live && !within_window(now_ms, last_shout_ms)) {
        shout_live = FALSE;
    }

    if (fall_live && shout_live) {
        /* Both sources live: highest-priority incident. Consume the pair so a
         * later lone event cannot re-fuse the same fall or shout. */
        et = MIND_EVT_FALL_AND_SHOUT;
        conf = ((last_fall == FALL_EVT_CONFIRMED) ? 75 : 50) + 25;
        svm = last_fall_svm;
        mic = last_shout_level;

        fall_live = FALSE;
        last_fall = FALL_EVT_NONE;
        shout_live = FALSE;
    } else if (fall_now) {
        et = (fe == FALL_EVT_CONFIRMED) ? MIND_EVT_CONFIRMED_FALL
                                        : MIND_EVT_POSSIBLE_FALL;
        conf = (fe == FALL_EVT_CONFIRMED) ? 75 : 50;
        svm = accel_svm;
        mic = 0;
    } else if (shout_now) {
        et = MIND_EVT_POSSIBLE_DISTRESS;
        conf = 50;
        svm = accel_svm;
        mic = mic_level;
    } else {
        return FALSE;           /* nothing new resolved on this wake */
    }

    if (conf > 100) conf = 100;

    out->event_type = et;
    out->confidence = (UB)conf;
    out->accel_svm  = svm;
    out->mic_level  = mic;
    out->seq        = seq++;
    return TRUE;
}
