/*
 * sensors.c - accelerometer + microphone sampling (see sensors.h).
 */

#include "sensors.h"
#include "imath.h"
#include "app_config.h"
#include "imu.h"
#include "mic.h"

INT sensors_init(void)
{
    INT who = imu_init();
    if (who < 0) {
        return who;
    }
    imu_set_fullscale(IMU_FULLSCALE_G);
    mic_init();
    return who;
}

INT sensors_read(sensor_sample_t *out)
{
    H  ax = 0, ay = 0, az = 0;
    UW sum;

    if (imu_read(&ax, &ay, &az) != 0) {
        return -1;
    }

    /* Accumulate in UW: three squared int16 axes can exceed INT32_MAX. */
    sum  = (UW)((W)ax * (W)ax);
    sum += (UW)((W)ay * (W)ay);
    sum += (UW)((W)az * (W)az);

    out->ax = ax;
    out->ay = ay;
    out->az = az;
    out->svm_mg = usqrt(sum) * 1000u / IMU_COUNTS_PER_G;
    return 0;
}

UINT sensors_mic_level(void)
{
    return mic_level();
}
