/*
 * fusion.c - rule-based fusion of the two detectors (see fusion.h).
 *
 * Two independent sources (fall state machine, shout detector) map to a
 * single incident and confidence. When both fire together the confidence is
 * raised and the event is FALL_AND_SHOUT. Event numbers come from the shared
 * beacon contract (schema.h). Zero ML.
 */

#include "fusion.h"
#include "schema.h"

static UB seq;

void fusion_init(void)
{
    seq = 0;
}

void fusion_update(fall_event_t fe, sound_event_t se,
                   UW accel_svm, UB mic_level, incident_state_t *out)
{
    BOOL fall  = (fe == FALL_EVT_POSSIBLE) || (fe == FALL_EVT_CONFIRMED);
    BOOL shout = (se == SOUND_EVT_POSSIBLE_DISTRESS);
    UB   et;
    UINT conf;

    if (fall && shout) {
        et = MIND_EVT_FALL_AND_SHOUT;
        conf = ((fe == FALL_EVT_CONFIRMED) ? 75 : 50) + 25;
    } else if (fe == FALL_EVT_CONFIRMED) {
        et = MIND_EVT_CONFIRMED_FALL;
        conf = 75;
    } else if (fe == FALL_EVT_POSSIBLE) {
        et = MIND_EVT_POSSIBLE_FALL;
        conf = 50;
    } else if (shout) {
        et = MIND_EVT_POSSIBLE_DISTRESS;
        conf = 50;
    } else {
        et = MIND_EVT_HEARTBEAT;
        conf = 0;
    }

    if (conf > 100) conf = 100;

    out->event_type = et;
    out->confidence = (UB)conf;
    out->accel_svm  = accel_svm;
    out->mic_level  = mic_level;
    out->seq        = seq++;
}
