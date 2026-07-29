/*
 * mic.h - micro:bit v2 MEMS microphone driver (SAADC on AIN3 / P0.05).
 *
 * The microphone must be powered by driving the mic-enable line (P0.20)
 * high. mic_level() takes a short burst of ADC samples and returns the
 * peak-to-peak amplitude, which tracks loudness regardless of the DC
 * bias of the mic signal.
 */

#ifndef MIC_H
#define MIC_H

#include <tk/tkernel.h>

/* Enable the microphone and configure the SAADC. */
void mic_init(void);

/* Sample a short window and return peak-to-peak amplitude (larger =
   louder). Units are raw 10-bit ADC counts. */
UINT mic_level(void);

/* Absolute min/max of the window mic_level() last sampled, in raw counts
   (0..1023). Their midpoint is the mic's DC bias, which decides how much SAADC
   gain the signal can take before it rails - a bias near mid-scale leaves
   headroom, one near a rail does not. Diagnostic use. */
H mic_last_min(void);
H mic_last_max(void);

/* Sample rate of mic_capture(), in Hz (SAADC internal timer). */
#define MIC_CAPTURE_RATE    8000

/* Capture 'n' uniformly-spaced samples at MIC_CAPTURE_RATE into 'buf'
   (12-bit, single-ended). Blocks for n / MIC_CAPTURE_RATE seconds.
   Unlike mic_level() this gives the constant sample rate an FFT needs. */
void mic_capture(H *buf, UINT n);

#endif /* MIC_H */
