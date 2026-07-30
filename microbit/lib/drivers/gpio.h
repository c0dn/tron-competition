/*
 * gpio.h - tiny nRF52833 GPIO helpers shared by the sense drivers.
 *
 * Pins are encoded as (port << 5) | number, so P0.05 == PIN(0,5) and
 * P1.05 == PIN(1,5). The nRF52833 has two GPIO ports at different bases.
 */

#ifndef GPIO_H
#define GPIO_H

#include <tk/tkernel.h>

#define PIN(port, n)    ((((UINT)(port)) << 5) | ((UINT)(n)))
#define PIN_PORT(p)     ((p) >> 5)
#define PIN_NUM(p)      ((p) & 0x1F)

#define GPIO0_BASE      0x50000000UL
#define GPIO1_BASE      0x50000300UL

/* register offsets (named to avoid clashing with the kernel's GPIO_* macros) */
#define GPIOH_OUTSET(b)   ((b) + 0x508)
#define GPIOH_OUTCLR(b)   ((b) + 0x50C)
#define GPIOH_DIRSET(b)   ((b) + 0x518)
#define GPIOH_PINCNF(b, n) ((b) + 0x700 + ((n) * 4))

static inline UW gpio_base(UINT pin)
{
    return PIN_PORT(pin) ? GPIO1_BASE : GPIO0_BASE;
}

static inline void gpio_make_output(UINT pin)
{
    out_w(GPIOH_DIRSET(gpio_base(pin)), 1UL << PIN_NUM(pin));
}

static inline void gpio_high(UINT pin)
{
    out_w(GPIOH_OUTSET(gpio_base(pin)), 1UL << PIN_NUM(pin));
}

static inline void gpio_low(UINT pin)
{
    out_w(GPIOH_OUTCLR(gpio_base(pin)), 1UL << PIN_NUM(pin));
}

#endif /* GPIO_H */
