/*
 * app_config.h - application constants for wearable_app.
 * Separate from config/config.h (the uT-Kernel system config).
 *
 * Detection thresholds are literature starting values (Plan 01 4.2 / 4.3.1),
 * to be tuned on hardware. Zero ML: thresholds / DSP / state machines only.
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* Per-unit id, baked in at flash time; used in the fixed AdvA (Plan 02 2.3). */
#define DEVICE_ID               0x01

/* Accelerometer: +/-8g so impacts don't clip. IMU_COUNTS_PER_G is the
 * rest-SVM calibration knob (imu.h: ~4096 counts/g at +/-8g). */
#define IMU_FULLSCALE_G         8
#define IMU_COUNTS_PER_G        4096

/* Accel sampling at 25 Hz; serial log throttled to LOG_EVERY_N samples. */
#define SAMPLE_INTERVAL_MS      40
#define LOG_EVERY_N             25

/* --- Fall state machine (Plan 01 4.2), milli-g --------------------------- */
#define FALL_FREEFALL_MG        600     /* SVM below this => free-fall        */
#define FALL_IMPACT_MG          2000    /* SVM above this => impact           */
#define FALL_IMPACT_WINDOW_MS   1000    /* impact must follow free-fall in    */
#define FALL_IMMOBILE_MS        1500    /* post-impact observation window     */
#define FALL_IMMOBILE_RANGE_MG  350     /* SVM spread under this => immobile  */

/* --- Shout detector (Plan 01 4.3.1) -------------------------------------- */
#define SHOUT_GATE_INTERVAL_MS  150     /* Stage A loudness-gate cadence      */
#define SHOUT_GATE_LEVEL        120     /* mic_level (p2p, 10-bit) gate       */
#define SHOUT_WIN_SAMPLES       256     /* Stage B capture (~32 ms @ 8 kHz)   */
#define SHOUT_RMS_MIN           150     /* min RMS energy of the window       */
#define SHOUT_ZCR_MIN           20      /* zero-crossings/window, low bound   */
#define SHOUT_ZCR_MAX           180     /* zero-crossings/window, high bound  */
#define SHOUT_BRIGHT_RATIO      0.6f    /* high-band / low-band power min     */
#define SHOUT_SUSTAIN_MS        350     /* candidate must hold this long      */
#define SHOUT_REFRACTORY_MS     2000    /* suppress re-fire after a shout     */
/* Goertzel coefficients 2*cos(2*pi*f/8000): low=500 Hz, high=1800 Hz. */
#define SHOUT_G_LOW_COEFF       1.8478f
#define SHOUT_G_HIGH_COEFF      0.3129f

/* --- Advertising cadence (Plan 01 5) ------------------------------------- */
#define HEARTBEAT_INTERVAL_MS   1500    /* slow alive beacon                  */
#define BURST_INTERVAL_MS       120     /* fast re-broadcast on an event      */
#define BURST_COUNT             50      /* ~6 s of burst per event            */

#endif /* APP_CONFIG_H */
