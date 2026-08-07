/*
 * imu.h - micro:bit v2 accelerometer driver (LSM303AGR over internal I2C).
 *
 * NOTE: micro:bit v2 hardware revisions have shipped with more than one
 * motion sensor (most commonly the ST LSM303AGR; some boards use a
 * different part). This driver targets the LSM303AGR accelerometer at
 * I2C address 0x19. imu_init() returns the WHO_AM_I value so the app can
 * confirm the chip - LSM303AGR reports 0x33.
 */

#ifndef IMU_H
#define IMU_H

#include <tk/tkernel.h>

#define LSM303AGR_WHOAMI    0x33

/* Bring up the internal I2C (TWIM) and start the accelerometer.
   Returns the WHO_AM_I register value (0x33 for LSM303AGR), or a
   negative value on I2C error. */
INT imu_init(void);

/* Read the current acceleration. x/y/z are raw signed 16-bit readings
   (sign/axes are what matter for tilt; scale on hardware as needed).
   Returns 0 on success, negative on I2C error. */
INT imu_read(H *x, H *y, H *z);

/* Set the accelerometer full-scale range: 2, 4, 8, or 16 (g).
   imu_init() leaves it at the power-on default (+/-2g); fall detection
   needs +/-8g so impact spikes do not clip. With +/-8g, 1g ~= 4096 raw
   counts. Returns 0 on success, negative on error. */
INT imu_set_fullscale(INT g);

#endif /* IMU_H */
