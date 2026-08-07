/*
 * incident.h - the detector output that gets advertised, split out so the
 * transmit-side policy can be compiled and tested on a host.
 *
 * fusion.h pulls in <tk/tkernel.h>, sensors.h, fall.h and sound.h, none of
 * which a host test of the transmit scheduler has any use for. The struct
 * itself is just plain data, so it lives here and both sides include it.
 *
 * The host typedefs mirror the kernel's: uT-Kernel's UW is a 32-bit unsigned
 * and every millisecond comparison in this app relies on that width wrapping,
 * so widening it on the host would hide exactly the bugs the tests are for.
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

/* Projection to the Plan 02 wire payload (identity rides AdvA / mesh src). */
typedef struct {
    UB  event_type;
    UB  confidence;
    UW  accel_svm;      /* milli-g */
    UB  mic_level;      /* 0-255 */
    UB  seq;
} incident_state_t;

#endif /* INCIDENT_H */
