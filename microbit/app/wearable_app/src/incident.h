/*
 * incident.h - detector output shared by fusion and the transmit policy.
 *
 * The event id is assigned once at admission.  Its low 24 bits are serialized
 * in the TM/01 frame, and every copy in the event spray retains that id.
 */

#ifndef INCIDENT_H
#define INCIDENT_H

#ifdef WEARABLE_HOST_TEST
#include <stdint.h>
typedef uint8_t  UB;
typedef uint32_t UW;
typedef int      INT;
typedef int      BOOL;
#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#else
#include <tk/tkernel.h>
#endif

typedef struct {
    UB  event_type;
    UB  confidence;
    UW  accel_svm;
    UB  mic_level;
    UB  seq;
    UW  event_id;
} incident_state_t;

#endif /* INCIDENT_H */
