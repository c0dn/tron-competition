/*
 * hw_timer.c - microsecond time base and randomness (see hw_timer.h)
 */

#include "hw_timer.h"

/* Physical timer numbers, 1-based as the API defines them:
 *   2 -> TIMER1, free-running time base
 *   3 -> TIMER2, re-armed per backoff
 * (4 -> TIMER3 is reserved by ble_radio.c as a PPI packet counter.) */
#define HW_TMR_CLOCK        2
#define HW_TMR_DELAY        3

/* TA_CYC_PTMR clears the counter on compare, so the largest possible limit
   turns it into a free-running counter that recycles at the natural 32-bit
   boundary - which is what keeps unsigned difference arithmetic valid. */
#define HW_CLOCK_LIMIT      0xFFFFFFFFUL

/* --- nRF52833 RNG (base 0x4000D000). No kernel abstraction exists for this
       peripheral, and nothing else in the system uses it. --- */
#define RNG_BASE            0x4000D000UL
#define RNG_TASKS_START     (RNG_BASE + 0x000)
#define RNG_TASKS_STOP      (RNG_BASE + 0x004)
#define RNG_EVENTS_VALRDY   (RNG_BASE + 0x100)
#define RNG_CONFIG          (RNG_BASE + 0x504)
#define RNG_VALUE           (RNG_BASE + 0x508)

#define HW_FLG_DELAY        (0x01U)

static ID hw_flg = 0;
static UW rng_state = 2463534242UL;     /* any non-zero xorshift seed */

/* One byte from the hardware RNG. Blocking, tens of microseconds; only ever
   called while seeding. */
static UB rng_byte(void)
{
    UB v;

    out_w(RNG_EVENTS_VALRDY, 0);
    out_w(RNG_TASKS_START, 1);
    while (in_w(RNG_EVENTS_VALRDY) == 0) {}
    v = (UB)in_w(RNG_VALUE);
    out_w(RNG_EVENTS_VALRDY, 0);
    out_w(RNG_TASKS_STOP, 1);
    return v;
}

UW hw_rand32(void)
{
    UW x = rng_state;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

void hw_rand_seed_mix(UW extra)
{
    rng_state ^= extra * 2654435761UL;      /* Knuth multiplicative */
    if (rng_state == 0) {
        rng_state = 2463534242UL;           /* xorshift dies on zero */
    }
    (void)hw_rand32();
}

/* Backoff elapsed. Reached through the kernel's physical timer handler, which
   has already entered task-independent context and cleared the compare event,
   so tk_set_flg() is legal here (implementation spec 4.5.2). */
LOCAL void delay_expired(void *exinf)
{
    if (hw_flg > 0) {
        tk_set_flg(hw_flg, HW_FLG_DELAY);
    }
}

void hw_timer_init(void)
{
    T_CFLG cflg = { .exinf = NULL, .flgatr = TA_TFIFO | TA_WSGL, .iflgptn = 0 };
    T_DPTMR dptmr = { .exinf = NULL, .ptmratr = TA_HLNG,
                      .ptmrhdr = (FP)delay_expired };
    UINT i;

    if (hw_flg <= 0) {
        hw_flg = tk_cre_flg(&cflg);
    }

    (void)DefinePhysicalTimerHandler(HW_TMR_DELAY, &dptmr);
    (void)StartPhysicalTimer(HW_TMR_CLOCK, HW_CLOCK_LIMIT, TA_CYC_PTMR);

    /* Seed the PRNG. Four hardware bytes is ample to decorrelate nodes. */
    out_w(RNG_CONFIG, 1);               /* DERCEN: bias correction on */
    rng_state = 0;
    for (i = 0; i < 4; i++) {
        rng_state = (rng_state << 8) | rng_byte();
    }
    if (rng_state == 0) {
        rng_state = 2463534242UL;
    }
}

UW hw_timer_now(void)
{
    UW count = 0;

    (void)GetPhysicalTimerCount(HW_TMR_CLOCK, &count);
    return count;
}

UW hw_timer_elapsed_us(UW since_ticks)
{
    return (UW)(hw_timer_now() - since_ticks) / HW_TIMER_TICKS_PER_US;
}

static void spin_us(UW us)
{
    UW start = hw_timer_now();
    UW ticks = us * HW_TIMER_TICKS_PER_US;

    while ((UW)(hw_timer_now() - start) < ticks) {}
}

void hw_timer_delay_us(UW us)
{
    UINT ptn;

    if (us == 0) {
        return;
    }

    /* Short waits: spinning beats arming and suspending. */
    if (us <= HW_TIMER_SPIN_MAX_US || hw_flg <= 0) {
        spin_us(us);
        return;
    }

    /* The one-shot counts up from zero to the limit, so unlike an absolute
       compare there is no way for the target to have already slipped past
       between arming and waiting. */
    tk_clr_flg(hw_flg, ~HW_FLG_DELAY);
    if (StartPhysicalTimer(HW_TMR_DELAY, us * HW_TIMER_TICKS_PER_US,
                           TA_ALM_PTMR) != E_OK) {
        spin_us(us);
        return;
    }

    tk_wai_flg(hw_flg, HW_FLG_DELAY, TWF_ANDW | TWF_CLR, &ptn, TMO_FEVR);
}
