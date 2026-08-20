/* Target-private first-event injector for the competition hardware proof. */

#ifndef WEARABLE_TEST_INJECTOR_H
#define WEARABLE_TEST_INJECTOR_H

#ifdef WEARABLE_TEST_INJECTOR

#include <tk/tkernel.h>

#include "fall.h"
#include "sound.h"

typedef struct {
    fall_event_t  fall_event;
    sound_event_t sound_event;
    UW            accel_svm;
    UB            mic_level;
} wearable_test_injection_t;

void wearable_test_injector_seed(wearable_test_injection_t *injection);

INT wearable_test_injector_sensors_read(sensor_sample_t *out);
fall_event_t wearable_test_injector_fall_update(const sensor_sample_t *sample);
UW wearable_test_injector_fall_last_peak(void);
sound_event_t wearable_test_injector_sound_update(void);
UINT wearable_test_injector_sound_last_level(void);
ER wearable_test_injector_set_flg(ID flgid, UINT ptn);

/* This header is force-included only for wearable_test_injector.  It changes
 * the first sensor/shout producer outputs before main.c is preprocessed, while
 * leaving the production source files and their compiled line information
 * untouched.  The implementation and host test call the real symbols. */
#if !defined(WEARABLE_TEST_INJECTOR_IMPLEMENTATION) && \
    !defined(WEARABLE_TEST_INJECTOR_HOST_TEST)
#define sensors_read(out) \
    wearable_test_injector_sensors_read((out))
#define fall_update(sample) \
    wearable_test_injector_fall_update((sample))
#define fall_last_peak() \
    wearable_test_injector_fall_last_peak()
#define sound_update() \
    wearable_test_injector_sound_update()
#define sound_last_level() \
    wearable_test_injector_sound_last_level()
#define tk_set_flg(flgid, ptn) \
    wearable_test_injector_set_flg((flgid), (ptn))
#endif

#endif /* WEARABLE_TEST_INJECTOR */

#endif /* WEARABLE_TEST_INJECTOR_H */
