/*
 * test_tx_adapter.c - host tests for the wearable's transmit policy.
 *
 * These run the adapter against a simulated clock rather than the kernel, so
 * the cases that matter (two events sharing the air, heartbeats surviving a
 * burst, the millisecond counter wrapping) are reachable without hardware.
 */

#include <stdio.h>
#include <string.h>

#include "tx_adapter.h"
#include "schema.h"

static int failures;
static int checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* Defaults mirroring app_config.h. */
#define BUDGET_MS     1000u
#define INTERVAL_MS    100u
#define MIN_TX           8u
#define HB_MS         1500u
#define TICK_MS         30u

static incident_state_t mk(UB type, UB seq)
{
    incident_state_t i;
    memset(&i, 0, sizeof(i));
    i.event_type = type;
    i.confidence = 50;
    i.accel_svm = 2000;
    i.seq = seq;
    return i;
}

/* Drive the adapter from t0 for span_ms, tallying what went out. */
typedef struct {
    UW event_tx;
    UW hb_tx;
    UW per_seq[256];
    UW max_hb_gap;
} run_t;

static run_t drive(tx_adapter_t *a, UW t0, UW span_ms)
{
    run_t r;
    UW t;
    UW last_hb = t0;

    memset(&r, 0, sizeof(r));

    for (t = t0; (UW)(t - t0) <= span_ms; t += TICK_MS) {
        incident_state_t out;
        tx_adapter_send_t s = tx_adapter_next(a, t, &out);

        if (s == TX_ADAPTER_SEND_EVENT) {
            r.event_tx++;
            r.per_seq[out.seq]++;
        } else if (s == TX_ADAPTER_SEND_HEARTBEAT) {
            UW gap = (UW)(t - last_hb);
            if (gap > r.max_hb_gap) {
                r.max_hb_gap = gap;
            }
            last_hb = t;
            r.hb_tx++;
        }
    }
    return r;
}

/* 1. A single event is re-sent across its budget, every copy identical. */
static void test_single_event_sprays(void)
{
    tx_adapter_t a;
    incident_state_t e = mk(MIND_EVT_CONFIRMED_FALL, 7);
    run_t r;

    printf("test_single_event_sprays\n");
    tx_adapter_init(&a, BUDGET_MS, INTERVAL_MS, MIN_TX, HB_MS, 0);
    check(tx_adapter_admit(&a, &e, 0) == TRUE, "event admitted");

    /* Run past the deadline: retirement is evaluated on the tick after it. */
    r = drive(&a, 0, BUDGET_MS * 2u);

    check(r.per_seq[7] >= MIN_TX, "at least min_tx copies sent");
    check(r.event_tx == r.per_seq[7], "no other event was emitted");
    check(tx_adapter_active(&a) == 0, "slot retired after the budget");
}

/* 2. The identity is stable across the spray. This is the invariant that lets
 *    relay-side duplicate suppression collapse a burst into one relay. */
static void test_identity_stable_across_burst(void)
{
    tx_adapter_t a;
    incident_state_t e = mk(MIND_EVT_FALL_AND_SHOUT, 42);
    UW t;
    int copies = 0;
    int mismatched = 0;

    printf("test_identity_stable_across_burst\n");
    tx_adapter_init(&a, BUDGET_MS, INTERVAL_MS, MIN_TX, HB_MS, 0);
    tx_adapter_admit(&a, &e, 0);

    for (t = 0; t <= BUDGET_MS; t += TICK_MS) {
        incident_state_t out;
        if (tx_adapter_next(&a, t, &out) == TX_ADAPTER_SEND_EVENT) {
            copies++;
            if (out.seq != 42 || out.event_type != MIND_EVT_FALL_AND_SHOUT) {
                mismatched++;
            }
        }
    }

    check(copies >= (int)MIN_TX, "burst produced copies");
    check(mismatched == 0, "every copy carried the same seq and type");
}

/* 3. Two concurrent events interleave and BOTH reach the floor. The old
 *    single-slot code lost the first event entirely here. */
static void test_two_events_interleave(void)
{
    tx_adapter_t a;
    incident_state_t fall  = mk(MIND_EVT_POSSIBLE_FALL, 1);
    incident_state_t shout = mk(MIND_EVT_POSSIBLE_DISTRESS, 2);
    run_t r;

    printf("test_two_events_interleave\n");
    tx_adapter_init(&a, BUDGET_MS, INTERVAL_MS, MIN_TX, HB_MS, 0);
    check(tx_adapter_admit(&a, &fall, 0) == TRUE, "first admitted");
    check(tx_adapter_admit(&a, &shout, 0) == TRUE, "second admitted");
    check(tx_adapter_active(&a) == 2, "both live simultaneously");

    r = drive(&a, 0, BUDGET_MS * 2u);

    check(r.per_seq[1] >= MIN_TX, "fall reached the floor");
    check(r.per_seq[2] >= MIN_TX, "shout reached the floor");
    check(tx_adapter_active(&a) == 0, "both retired");
}

/* 4. A second event must not truncate the first - the actual reported bug. */
static void test_second_event_does_not_clobber(void)
{
    tx_adapter_t a;
    incident_state_t first  = mk(MIND_EVT_POSSIBLE_FALL, 10);
    incident_state_t second = mk(MIND_EVT_CONFIRMED_FALL, 11);
    run_t r;

    printf("test_second_event_does_not_clobber\n");
    tx_adapter_init(&a, BUDGET_MS, INTERVAL_MS, MIN_TX, HB_MS, 0);
    tx_adapter_admit(&a, &first, 0);
    /* Upgrade arrives mid-spray, as a confirmed fall does. */
    tx_adapter_admit(&a, &second, 300);

    r = drive(&a, 0, BUDGET_MS * 2u);

    check(r.per_seq[10] >= MIN_TX, "first event still completed its budget");
    check(r.per_seq[11] >= MIN_TX, "upgrade also completed its budget");
}

/* 5. Heartbeats keep flowing during a burst. Previously they were the else
 *    branch of the burst check and stopped for the whole event. */
static void test_heartbeat_not_starved(void)
{
    tx_adapter_t a;
    incident_state_t e = mk(MIND_EVT_FALL_AND_SHOUT, 3);
    run_t r;

    printf("test_heartbeat_not_starved\n");
    tx_adapter_init(&a, BUDGET_MS, INTERVAL_MS, MIN_TX, HB_MS, 0);
    tx_adapter_admit(&a, &e, 0);

    r = drive(&a, 0, 6000);

    check(r.hb_tx >= 3, "heartbeats continued during and after the burst");
    check(r.max_hb_gap <= HB_MS + (TICK_MS * 2u), "no heartbeat gap beyond one interval");
}

/* 6. The table is bounded, and severity decides who survives. */
static void test_eviction_prefers_severity(void)
{
    tx_adapter_t a;
    incident_state_t low = mk(MIND_EVT_POSSIBLE_DISTRESS, 20);
    incident_state_t high = mk(MIND_EVT_FALL_AND_SHOUT, 99);
    incident_state_t another_low = mk(MIND_EVT_POSSIBLE_DISTRESS, 21);
    UB i;

    printf("test_eviction_prefers_severity\n");
    tx_adapter_init(&a, BUDGET_MS, INTERVAL_MS, MIN_TX, HB_MS, 0);

    for (i = 0; i < TX_ADAPTER_MAX_EVENTS; i++) {
        low.seq = (UB)(20 + i);
        check(tx_adapter_admit(&a, &low, 0) == TRUE, "table fills");
    }
    check(tx_adapter_active(&a) == TX_ADAPTER_MAX_EVENTS, "table is full");

    /* Equal severity must not thrash the table. */
    check(tx_adapter_admit(&a, &another_low, 0) == FALSE,
          "equal severity is dropped, not swapped in");
    /* Higher severity displaces a resident. */
    check(tx_adapter_admit(&a, &high, 0) == TRUE, "higher severity evicts");
    check(tx_adapter_active(&a) == TX_ADAPTER_MAX_EVENTS, "still bounded");
    check(a.evicted == 1, "exactly one eviction recorded");
}

/* 7. Severity ordering matches the schema's own priority comment. */
static void test_severity_order(void)
{
    printf("test_severity_order\n");
    check(tx_adapter_severity(MIND_EVT_FALL_AND_SHOUT) >
          tx_adapter_severity(MIND_EVT_CONFIRMED_FALL), "fall+shout outranks confirmed");
    check(tx_adapter_severity(MIND_EVT_CONFIRMED_FALL) >
          tx_adapter_severity(MIND_EVT_POSSIBLE_FALL), "confirmed outranks possible");
    check(tx_adapter_severity(MIND_EVT_POSSIBLE_FALL) >
          tx_adapter_severity(MIND_EVT_POSSIBLE_DISTRESS), "possible fall outranks distress");
    check(tx_adapter_severity(MIND_EVT_HEARTBEAT) == 0, "heartbeat has no severity");
}

/* 8. Everything still works across the 32-bit millisecond wrap. A naive
 *    (now >= deadline) fails here: the deadline wraps to a small number while
 *    now is still near the top of the range. */
static void test_clock_wrap(void)
{
    tx_adapter_t a;
    incident_state_t e = mk(MIND_EVT_CONFIRMED_FALL, 55);
    UW t0 = 0xFFFFFF00u;        /* wraps ~256 ms in */
    run_t r;

    printf("test_clock_wrap\n");
    tx_adapter_init(&a, BUDGET_MS, INTERVAL_MS, MIN_TX, HB_MS, t0);
    check(tx_adapter_admit(&a, &e, t0) == TRUE, "admitted before the wrap");

    r = drive(&a, t0, BUDGET_MS * 2u);

    check(r.per_seq[55] >= MIN_TX, "spray survived the wrap");
    check(tx_adapter_active(&a) == 0, "slot retired correctly across the wrap");
}

/* 9. An idle adapter emits only heartbeats. */
static void test_idle_only_heartbeats(void)
{
    tx_adapter_t a;
    run_t r;

    printf("test_idle_only_heartbeats\n");
    tx_adapter_init(&a, BUDGET_MS, INTERVAL_MS, MIN_TX, HB_MS, 0);

    r = drive(&a, 0, 5000);

    check(r.event_tx == 0, "no event traffic when idle");
    check(r.hb_tx >= 3, "heartbeats still emitted");
}

/* 10. Copies are paced, not dumped as fast as the caller polls. */
static void test_pacing_respected(void)
{
    tx_adapter_t a;
    incident_state_t e = mk(MIND_EVT_CONFIRMED_FALL, 77);
    incident_state_t out;
    int back_to_back = 0;
    UW t;

    printf("test_pacing_respected\n");
    tx_adapter_init(&a, BUDGET_MS, INTERVAL_MS, MIN_TX, HB_MS, 0);
    tx_adapter_admit(&a, &e, 0);

    /* Poll far faster than the interval; only the first should go out. */
    for (t = 0; t < INTERVAL_MS; t += 1u) {
        if (tx_adapter_next(&a, t, &out) == TX_ADAPTER_SEND_EVENT) {
            back_to_back++;
        }
    }
    check(back_to_back == 1, "one copy per interval regardless of poll rate");
}

int main(void)
{
    printf("tx_adapter tests\n\n");

    test_single_event_sprays();
    test_identity_stable_across_burst();
    test_two_events_interleave();
    test_second_event_does_not_clobber();
    test_heartbeat_not_starved();
    test_eviction_prefers_severity();
    test_severity_order();
    test_clock_wrap();
    test_idle_only_heartbeats();
    test_pacing_respected();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
