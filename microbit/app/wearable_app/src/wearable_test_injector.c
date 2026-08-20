/* Target-private producer hooks for wearable_test_injector. */

#define WEARABLE_TEST_INJECTOR_IMPLEMENTATION 1
#include "wearable_test_injector.h"

/* This source is force-included after the aliases are installed for the
 * injector target. Its fallback calls must resolve to the real drivers. */
#undef sensors_read
#undef fall_update
#undef fall_last_peak
#undef sound_update
#undef sound_last_level
#undef tk_set_flg

#define WEARABLE_INJECTOR_FLG_FALL  0x01u
#define WEARABLE_INJECTOR_FLG_SOUND 0x02u

static wearable_test_injection_t injection;
static BOOL injection_ready;
static BOOL fall_sent;
static BOOL fall_peak_pending;
static BOOL sound_sent;
static BOOL sound_level_active;
static BOOL fall_flag_held;

void wearable_test_injector_seed(wearable_test_injection_t *out)
{
    if (out == 0) {
        return;
    }

    out->fall_event = FALL_EVT_CONFIRMED;
    out->sound_event = SOUND_EVT_POSSIBLE_DISTRESS;
    out->accel_svm = 4242u;
    out->mic_level = 96u;
}

static void ensure_injection(void)
{
    if (!injection_ready) {
        wearable_test_injector_seed(&injection);
        injection_ready = TRUE;
    }
}

INT wearable_test_injector_sensors_read(sensor_sample_t *out)
{
    ensure_injection();
    if (!fall_sent && out != 0) {
        out->ax = 0;
        out->ay = 0;
        out->az = 0;
        out->svm_mg = injection.accel_svm;
        return 0;
    }
    return sensors_read(out);
}

fall_event_t wearable_test_injector_fall_update(const sensor_sample_t *sample)
{
    ensure_injection();
    if (!fall_sent) {
        fall_sent = TRUE;
        fall_peak_pending = TRUE;
        return injection.fall_event;
    }
    return fall_update(sample);
}

UW wearable_test_injector_fall_last_peak(void)
{
    ensure_injection();
    if (fall_peak_pending) {
        fall_peak_pending = FALSE;
        return injection.accel_svm;
    }
    return fall_last_peak();
}

sound_event_t wearable_test_injector_sound_update(void)
{
    ensure_injection();
    if (!sound_sent) {
        sound_sent = TRUE;
        sound_level_active = TRUE;
        return injection.sound_event;
    }
    sound_level_active = FALSE;
    return sound_update();
}

UINT wearable_test_injector_sound_last_level(void)
{
    ensure_injection();
    if (sound_level_active) {
        return injection.mic_level;
    }
    return sound_last_level();
}

ER wearable_test_injector_set_flg(ID flgid, UINT ptn)
{
    /* usermain starts the waiting fusion task first, then the priority-4
     * sensor task, then the priority-5 sound task. Hold the synthetic fall
     * signal until the synthetic shout state is stored so fusion receives one
     * combined wakeup before either real lower-priority producer can race it. */
    if (!fall_flag_held && ptn == WEARABLE_INJECTOR_FLG_FALL) {
        fall_flag_held = TRUE;
        return E_OK;
    }
    if (fall_flag_held && ptn == WEARABLE_INJECTOR_FLG_SOUND) {
        fall_flag_held = FALSE;
        return tk_set_flg(flgid,
                          WEARABLE_INJECTOR_FLG_FALL | WEARABLE_INJECTOR_FLG_SOUND);
    }
    return tk_set_flg(flgid, ptn);
}
