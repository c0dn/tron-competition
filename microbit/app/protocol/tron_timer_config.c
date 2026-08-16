#include "tron_timer_config.h"

/* Frozen BALANCED host fixture.  Firmware does not link this file: CMake emits
 * a same-symbol generated definition from the selected profile. */
const tron_timer_config_t tron_timer_config = {
    50u, 200u, 2u, 2u, 2u, 8u,
    20u, 120u, 10000u, 2000u, 1500u,
    250u, 3u, 0u, 30u, 750u, 840u, 10u, 500u, 3u, 5000u,
    10000u, 10000u, 20u, 120u,
    40u, 15u, 1200u, 2400u, 10000u, 2u, 10u, 10u, 360000u,
    30000u, 2400u, 10000u, 10000u, 250u,
    150000u, 300000u, 600000u, 25000u,
    24000u, 120000u, 0.8f, 0.95f, 10000u, 3000u,
    10u, 100u, 6000u, 4u, 4u, 2000u, 2400u, 3u, 3000u, 10000u,
    90u, 30u, 500u, 10u, 0u, 50u, 10000u,
    5000u, 30000u, 1000u, 5300u, 1000u, 5000u, 2u,
};
