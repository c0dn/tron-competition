/*
 * sensors.h - accelerometer + microphone sampling.
 */

#ifndef SENSORS_H
#define SENSORS_H

#include <tk/tkernel.h>

typedef struct {
    H   ax;
    H   ay;
    H   az;
    UW  svm_mg;     /* sqrt(ax^2+ay^2+az^2) in milli-g */
} sensor_sample_t;

/* Init IMU (+/-8g) and mic. Returns IMU WHO_AM_I, or negative on error. */
INT sensors_init(void);

/* Read accel + fill 'out'. Returns 0 on success, negative on I2C error. */
INT sensors_read(sensor_sample_t *out);

/* Mic loudness, peak-to-peak raw ADC counts. */
UINT sensors_mic_level(void);

#endif /* SENSORS_H */
