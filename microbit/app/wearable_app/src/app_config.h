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

/* TM/01 direct wearable ingress identity.  The packet is advertising-only;
 * backbone application ingress consumes it and does not legacy-relay it. */
#define MIND_MESH_NET_ID        0x01u
#define MIND_MESH_SRC_BASE      0x0100u
#define MIND_MESH_SRC           (MIND_MESH_SRC_BASE + DEVICE_ID)

/* Accelerometer: +/-8g so impacts don't clip. IMU_COUNTS_PER_G is the
 * rest-SVM calibration knob (imu.h: ~4096 counts/g at +/-8g). */
#define IMU_FULLSCALE_G         8
#define IMU_COUNTS_PER_G        4096

/* Accel sampling at 25 Hz; serial log throttled to LOG_EVERY_N samples. */
#define SAMPLE_INTERVAL_MS      40
#define LOG_EVERY_N             25

/* --- Fall state machine (Plan 01 4.2), milli-g --------------------------- */
#define FALL_FREEFALL_MG        600     /* SVM below this => free-fall         */
#define FALL_IMPACT_MG          2000    /* SVM above this => impact            */
#define FALL_IMPACT_WINDOW_MS   1000    /* impact must follow free-fall in     */
#define FALL_STILL_BAND_MG      300     /* |SVM-1000| under this => "still"    */
#define FALL_IMMOBILE_MS        1500    /* continuous stillness => CONFIRMED   */
#define FALL_OBSERVE_MS         3000    /* if never still by here => POSSIBLE  */

/* --- Shout detector (Plan 01 4.3.1) -------------------------------------- */
/* Set to 1 to dump per-window mic features over serial for threshold tuning.
 * Off in committed builds: the dump fires every gate interval while loud. */
#define SHOUT_DEBUG             0
/* Gate cadence. mic_capture() blocks ~32 ms per window, so at the old 150 ms
 * the detector listened only ~20% of the time and a 300 ms shout produced at
 * most 1-2 windows - never the 3 the old sustain demanded. At 60 ms the cycle
 * is ~95 ms (32 ms capture + delay + DSP), ~34% duty, so the same shout yields
 * 3+ windows. sound_task sits at priority 5, below sensor_task at 4, so the
 * busy-wait capture cannot disturb the 25 Hz accel cadence. */
#define SHOUT_GATE_INTERVAL_MS  60      /* Stage A loudness-gate cadence      */
/* Stage A gate is adaptive, not a fixed count. mic_level() is a 10-bit SAADC
 * peak-to-peak reading (0..1023) at gain 1/6 against the 0.6 V reference, i.e.
 * ~3.5 mV/count - an absolute threshold depends on room noise, per-unit mic
 * bias and gain, so it does not travel between venues.
 *
 * gate = max(SHOUT_GATE_MIN, median(last N levels) * SHOUT_GATE_MULT_PCT/100)
 *
 * The median is used rather than the mean because a shout occupies only a few
 * of the N windows, so it barely moves the baseline it is being measured
 * against - no need to freeze adaptation during an event. SHOUT_GATE_MIN is the
 * floor that stops a near-silent room producing a hair-trigger gate. */
#define SHOUT_BASELINE_N        32      /* ~4.8 s of history @ 150 ms/window  */
#define SHOUT_GATE_MULT_PCT     500     /* fire at 5x the ambient median      */
#define SHOUT_GATE_MIN          25      /* absolute floor, counts (provisional)*/
#define SHOUT_WIN_SAMPLES       256     /* Stage B capture (~32 ms @ 8 kHz)   */
/* Thresholds set from a measured session (gain-4 mic, quiet room). Observed:
 *   strong shout  rms 206 / zcr 108 / bright 1130   then rms 149 / bright 55
 *   quick yap     rms  81 / zcr  46 / bright 3478
 *   fall impact   rms 259 / zcr  72 / bright   18   <- rejected on brightness
 * RMS 150 and brightness 0.6 clipped the tail of a genuine shout by ~1%, so
 * both are relaxed. Brightness stays the primary voice-vs-impact discriminator:
 * the impact measured bright=18 against 1130+ for voice. */
#define SHOUT_RMS_MIN           100     /* min RMS energy of the window       */
#define SHOUT_ZCR_MIN           20      /* zero-crossings/window, low bound   */
#define SHOUT_ZCR_MAX           180     /* zero-crossings/window, high bound  */
#define SHOUT_BRIGHT_RATIO      0.4f    /* high-band / low-band power min     */
/* Two consecutive windows (~190 ms at the 60 ms cadence). One window would fire
 * on the fall's own impact crack; three was unreachable. */
#define SHOUT_SUSTAIN_MS        120     /* candidate must hold this long      */
#define SHOUT_REFRACTORY_MS     2000    /* suppress re-fire after a shout     */
/* Goertzel coefficients 2*cos(2*pi*f/8000): low=500 Hz, high=1800 Hz. */
#define SHOUT_G_LOW_COEFF       1.8478f
#define SHOUT_G_HIGH_COEFF      0.3129f

/* --- Fusion (fall + shout coincidence) ----------------------------------- */
/* A fall and a shout fuse into MIND_EVT_FALL_AND_SHOUT when they land within
 * this window, in either order. Sized off FALL_OBSERVE_MS: a fall event is
 * reported 1500-3000 ms after the impact (the immobility observation), while a
 * shout confirms ~350 ms after it starts, so the shout normally arrives first
 * and must still be "live" when the fall resolves. */
#define FUSION_COINCIDENCE_MS   3000

/* --- Advertising cadence -------------------------------------------------- */
#define HEARTBEAT_INTERVAL_MS   500     /* heartbeat remains live during spray */
#define EVENT_TX_BUDGET_MS      1000    /* event spray wall-clock budget       */
#define EVENT_TX_INTERVAL_MS    100     /* same-event copy spacing              */
#define EVENT_TX_MIN_COUNT      8       /* copies before normal retirement      */
#define TX_TICK_MS              30      /* fixed poll cadence                   */

#endif /* APP_CONFIG_H */
