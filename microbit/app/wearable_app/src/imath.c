/*
 * imath.c - integer math helpers (see imath.h).
 */

#include "imath.h"

UW usqrt(UW x)
{
    UW res = 0;
    UW bit = 1UL << 30;

    while (bit > x) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (x >= res + bit) {
            x -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}
