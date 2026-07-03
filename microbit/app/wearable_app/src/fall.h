/*
 * fall.h - fall-detection state machine (Plan 01 4.2). Stub.
 */

#ifndef FALL_H
#define FALL_H

#include <tk/tkernel.h>
#include "sensors.h"

typedef enum {
    FALL_NORMAL = 0,
    FALL_FREEFALL,
    FALL_IMPACT
} fall_phase_t;

/* Values align with the Plan 02 event_type enum. */
typedef enum {
    FALL_EVT_NONE      = 0,
    FALL_EVT_POSSIBLE  = 2,
    FALL_EVT_CONFIRMED = 3
} fall_event_t;

void fall_init(void);

/* Feed one sample (called at the accel rate). Returns a fall event at the
 * moment the sequence resolves, otherwise FALL_EVT_NONE. */
fall_event_t fall_update(const sensor_sample_t *s);

/* Peak SVM (milli-g) of the most recent impact window; valid after an event. */
UW fall_last_peak(void);

#endif /* FALL_H */
