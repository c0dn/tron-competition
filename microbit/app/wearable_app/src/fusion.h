/*
 * fusion.h - rule-based fusion of the two detectors. Stub.
 */

#ifndef FUSION_H
#define FUSION_H

#include <tk/tkernel.h>
#include "sensors.h"
#include "fall.h"
#include "sound.h"

/* Projection to the Plan 02 wire payload (identity rides AdvA). */
typedef struct {
    UB  event_type;
    UB  confidence;
    UW  accel_svm;      /* milli-g */
    UB  mic_level;      /* 0-255 */
    UB  seq;
} incident_state_t;

void fusion_init(void);

/* Fuse the latest fall + sound events into 'out'. accel_svm is the fall peak
 * (or current SVM), mic_level the shout loudness (or 0), now_ms a monotonic
 * millisecond clock. Assigns event_type (mind_event_type), confidence, and the
 * next seq.
 *
 * Each source is latched for FUSION_COINCIDENCE_MS so the two can fuse in
 * either order. Returns TRUE when 'out' holds an incident to advertise, FALSE
 * when the call produced nothing new (a fall/shout pair is only fused once). */
BOOL fusion_update(fall_event_t fe, sound_event_t se,
                   UW accel_svm, UB mic_level, UW now_ms,
                   incident_state_t *out);

#endif /* FUSION_H */
