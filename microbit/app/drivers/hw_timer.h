/*
 * hw_timer.h - microsecond time base and randomness for the flood mesh.
 *
 * WHY THIS EXISTS
 * ---------------
 * The kernel clock is not fine enough for relay scheduling. tk_dly_tsk()
 * resolves to CNF_TIMER_PERIOD, which is set per-app: mesh_echo and
 * ble_observer use a 1 ms tick, wearable_app uses 10 ms. A backoff drawn from a
 * 50 ms window would land on 50 distinct values in one app and 5 in the other,
 * and 5 is close to useless - randomised backoff is the one mechanism keeping a
 * flood from collapsing into mutual collisions once several nodes rebroadcast
 * at once. Sleeping the remainder on tk_dly_tsk() does not rescue it either,
 * because tk_dly_tsk() can overshoot by a full tick, so the only way to keep
 * sub-tick granularity would be to spin for most of the backoff.
 *
 * WHICH TIMER, AND WHY THROUGH THE OS
 * -----------------------------------
 * mtkernel drives its own tick from ARM SysTick, so the nRF52833 TIMERs are
 * free - but they are NOT unclaimed. USE_PTMR is 1 in every app config here, so
 * micro T-Kernel/SM's physical timer driver (lib/libtk/sysdepend/cpu/nrf5/
 * ptimer_nrf5.c) already owns TIMER0..TIMER4 as physical timers 1..5. Poking
 * those registers directly would work right up until anything else called
 * StartPhysicalTimer() on the same unit, so this goes through the documented
 * API instead. It also gets tk_def_int() and EnableInt() handling for free.
 *
 * Physical timer 2 (TIMER1) is the free-running time base; physical timer 3
 * (TIMER2) is re-armed per backoff. Physical timer 4 (TIMER3) is reserved by
 * ble_radio.c as a hardware packet counter, which the ptimer API cannot express
 * because it has no counter mode - see the note there. Physical timers 1 and 5
 * are unused.
 *
 * The hardware runs at 16 MHz (ptimer_nrf5.c fixes PRESCALER at 0), so the
 * primitive here is a raw tick, not a microsecond. That is deliberate: raw
 * ticks wrap cleanly at 2^32 and unsigned differences stay correct across the
 * wrap, whereas a divided-down microsecond counter would wrap at 2^28 and
 * quietly break the usual (UW)(now - then) idiom.
 */

#ifndef HW_TIMER_H
#define HW_TIMER_H

#include <tk/tkernel.h>
#include <tk/syslib.h>

/* Physical timer clock, fixed by ptimer_nrf5.c. */
#define HW_TIMER_HZ             16000000UL
#define HW_TIMER_TICKS_PER_US   (HW_TIMER_HZ / 1000000UL)    /* 16 */

/* Start the time base, register the backoff handler, seed the PRNG. Call once
   at boot from task context, before anything else here. */
void hw_timer_init(void);

/* Free-running counter in 16 MHz ticks. Wraps every ~268 s, cleanly at 2^32,
   so compare as (UW)(now - then) and never as now > then. */
UW hw_timer_now(void);

/* Microseconds elapsed since a stamp taken from hw_timer_now(). Wrap-safe. */
UW hw_timer_elapsed_us(UW since_ticks);

/* Sleep for at least 'us' microseconds.
 *
 * Waits at or under HW_TIMER_SPIN_MAX_US spin, because arming a timer and
 * suspending costs more than the wait. Longer waits arm a one-shot physical
 * timer and block on an event flag, leaving the CPU free - which matters,
 * because the caller is a relay sitting on a backoff while the detector still
 * needs to sample on time.
 *
 * SINGLE WAITER. One event flag and one physical timer back this, so exactly
 * one task may be inside it at a time. That task is the mesh relay. */
void hw_timer_delay_us(UW us);

#define HW_TIMER_SPIN_MAX_US    200

/* Uniform 32-bit random number.
 *
 * xorshift32 seeded once from the nRF hardware RNG. The hardware RNG is true
 * random but costs tens of microseconds per byte, far too slow to sit in the
 * path of every relay decision; seeding a cheap PRNG from it once buys
 * per-node divergence without the per-call cost. Node identity is mixed in as
 * well, so two units that happened to sample the same RNG bytes still choose
 * different backoffs.
 *
 * Not for anything security-bearing. */
UW hw_rand32(void);

/* Mix a per-unit value (a device id) into the PRNG state. Call after
   hw_timer_init() if the app has an identity worth decorrelating on. */
void hw_rand_seed_mix(UW extra);

#endif /* HW_TIMER_H */
