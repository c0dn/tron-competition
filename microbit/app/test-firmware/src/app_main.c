/*
 *----------------------------------------------------------------------
 *    micro T-Kernel 3.00.05
 *
 *    Copyright (C) 2006-2021 by Ken Sakamura.
 *    This software is distributed under the T-License 2.2.
 *----------------------------------------------------------------------
 *
 *    Released by TRON Forum(http://www.tron.org) at 2021/11.
 *
 *----------------------------------------------------------------------
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#if USE_TMONITOR
#define TM_PUTSTRING(a) tm_putstring(a)

void print_err(UB *str, ER err)
{
    tm_printf(str, err);
}

#else
#define TM_PUTSTRING(a)

void print_err(UB *str, INT par) {}

#endif /* USE_TMONITOR */

EXPORT INT usermain(void)
{
    T_RVER rver;

    TM_PUTSTRING((UB *)"Hello, micro:bit V2 from uT-Kernel 3.0!\n");

    tk_ref_ver(&rver);

#if USE_TMONITOR
    tm_printf((UB *)"Maker: %04x  Product ID: %04x\n", rver.maker, rver.prid);
    tm_printf((UB *)"Version: %04x  Product Num: %04x %04x %04x %04x\n",
              rver.prver, rver.prno[0], rver.prno[1], rver.prno[2], rver.prno[3]);
    tm_printf((UB *)"UART console is alive.\n");
    tm_printf((UB *)"Hello World\n");
#endif

    tk_slp_tsk(TMO_FEVR);

    return 0;
}
