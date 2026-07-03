/*
 * sound.h - always-on shout/scream detector (Plan 01 4.3.1). Stub.
 */

#ifndef SOUND_H
#define SOUND_H

#include <tk/tkernel.h>

/* Values align with the Plan 02 event_type enum. */
typedef enum {
    SOUND_EVT_NONE              = 0,
    SOUND_EVT_POSSIBLE_DISTRESS = 4
} sound_event_t;

void sound_init(void);
sound_event_t sound_update(void);
UINT sound_last_level(void);        /* mic loudness 0-255 for the wire payload */

#endif /* SOUND_H */
