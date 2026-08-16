/* Host checks for the wearable event spray policy. */

#include <stdio.h>
#include <string.h>

#include "tx_adapter.h"
#include "schema.h"

#define BUDGET_MS       1000u
#define INTERVAL_MS      100u
#define MIN_TX              8u
#define HEARTBEAT_MS      500u
#define TICK_MS            30u

static int failures;
static int checks;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

static incident_state_t make_incident(UB type, UB seq, UW event_id)
{
    incident_state_t incident;

    memset(&incident, 0, sizeof(incident));
    incident.event_type = type;
    incident.confidence = 50;
    incident.accel_svm = 2000;
    incident.seq = seq;
    incident.event_id = event_id;
    return incident;
}

typedef struct {
    UW event_tx;
    UW heartbeat_tx;
    UW per_seq[256];
    UW maximum_heartbeat_gap;
} run_result_t;

static run_result_t drive(tx_adapter_t *adapter, UW started_at_ms, UW span_ms)
{
    run_result_t result;
    UW now_ms;
    UW last_heartbeat_ms = started_at_ms;

    memset(&result, 0, sizeof(result));
    for (now_ms = started_at_ms;
         (UW)(now_ms - started_at_ms) <= span_ms;
         now_ms += TICK_MS) {
        incident_state_t output;
        tx_adapter_send_t send = tx_adapter_next(adapter, now_ms, &output);

        if (send == TX_ADAPTER_SEND_EVENT) {
            result.event_tx++;
            result.per_seq[output.seq]++;
        } else if (send == TX_ADAPTER_SEND_HEARTBEAT) {
            UW gap = now_ms - last_heartbeat_ms;

            if (gap > result.maximum_heartbeat_gap) {
                result.maximum_heartbeat_gap = gap;
            }
            last_heartbeat_ms = now_ms;
            result.heartbeat_tx++;
        }
    }
    return result;
}

static void test_single_event_sprays(void)
{
    tx_adapter_t adapter;
    incident_state_t incident = make_incident(MIND_EVT_CONFIRMED_FALL, 7,
                                               0x00a1b2c3u);
    run_result_t result;

    printf("test_single_event_sprays\n");
    tx_adapter_init(&adapter, BUDGET_MS, INTERVAL_MS, MIN_TX, HEARTBEAT_MS, 0);
    check(tx_adapter_admit(&adapter, &incident, 0) == TRUE, "event admitted");
    result = drive(&adapter, 0, BUDGET_MS * 2u);
    check(result.per_seq[7] >= MIN_TX, "at least eight copies sent");
    check(result.event_tx == result.per_seq[7], "no unrelated event sent");
    check(tx_adapter_active(&adapter) == 0, "event retires after its spray");
}

static void test_packet_id_is_stable_across_spray(void)
{
    tx_adapter_t adapter;
    incident_state_t incident = make_incident(MIND_EVT_FALL_AND_SHOUT, 42,
                                               0x00fedcbau);
    UW now_ms;
    int copies = 0;
    int mismatches = 0;

    printf("test_packet_id_is_stable_across_spray\n");
    tx_adapter_init(&adapter, BUDGET_MS, INTERVAL_MS, MIN_TX, HEARTBEAT_MS, 0);
    (void)tx_adapter_admit(&adapter, &incident, 0);
    for (now_ms = 0; now_ms <= BUDGET_MS; now_ms += TICK_MS) {
        incident_state_t output;

        if (tx_adapter_next(&adapter, now_ms, &output) == TX_ADAPTER_SEND_EVENT) {
            copies++;
            if (output.event_id != incident.event_id || output.seq != incident.seq ||
                output.event_type != incident.event_type) {
                mismatches++;
            }
        }
    }
    check(copies >= (int)MIN_TX, "event produced the minimum copies");
    check(mismatches == 0, "every copy retained its packet id and payload identity");
}

static void test_concurrent_events_interleave(void)
{
    tx_adapter_t adapter;
    incident_state_t fall = make_incident(MIND_EVT_POSSIBLE_FALL, 1, 0x100001u);
    incident_state_t shout = make_incident(MIND_EVT_POSSIBLE_DISTRESS, 2, 0x100002u);
    run_result_t result;

    printf("test_concurrent_events_interleave\n");
    tx_adapter_init(&adapter, BUDGET_MS, INTERVAL_MS, MIN_TX, HEARTBEAT_MS, 0);
    check(tx_adapter_admit(&adapter, &fall, 0) == TRUE, "fall admitted");
    check(tx_adapter_admit(&adapter, &shout, 0) == TRUE, "shout admitted");
    result = drive(&adapter, 0, BUDGET_MS * 2u);
    check(result.per_seq[1] >= MIN_TX, "fall completed its spray");
    check(result.per_seq[2] >= MIN_TX, "shout completed its spray");
    check(tx_adapter_active(&adapter) == 0, "both events retired");
}

static void test_heartbeat_is_not_starved(void)
{
    tx_adapter_t adapter;
    incident_state_t incident = make_incident(MIND_EVT_FALL_AND_SHOUT, 3,
                                               0x00cafe03u);
    run_result_t result;

    printf("test_heartbeat_is_not_starved\n");
    tx_adapter_init(&adapter, BUDGET_MS, INTERVAL_MS, MIN_TX, HEARTBEAT_MS, 0);
    (void)tx_adapter_admit(&adapter, &incident, 0);
    result = drive(&adapter, 0, 6000);
    check(result.heartbeat_tx >= 3, "heartbeats continue during an event spray");
    check(result.maximum_heartbeat_gap <= HEARTBEAT_MS + (TICK_MS * 2u),
          "heartbeat gap remains bounded by cadence and polling jitter");
}

static void test_clock_wrap_and_severity_eviction(void)
{
    tx_adapter_t adapter;
    incident_state_t low = make_incident(MIND_EVT_POSSIBLE_DISTRESS, 20, 20);
    incident_state_t high = make_incident(MIND_EVT_FALL_AND_SHOUT, 99, 99);
    incident_state_t wrap_event = make_incident(MIND_EVT_CONFIRMED_FALL, 55, 55);
    run_result_t result;
    UB index;
    UW started_at_ms = 0xffffff00u;

    printf("test_clock_wrap_and_severity_eviction\n");
    tx_adapter_init(&adapter, BUDGET_MS, INTERVAL_MS, MIN_TX, HEARTBEAT_MS,
                    started_at_ms);
    check(tx_adapter_admit(&adapter, &wrap_event, started_at_ms) == TRUE,
          "event admitted before clock wrap");
    result = drive(&adapter, started_at_ms, BUDGET_MS * 2u);
    check(result.per_seq[55] >= MIN_TX, "spray survives the millisecond wrap");

    tx_adapter_init(&adapter, BUDGET_MS, INTERVAL_MS, MIN_TX, HEARTBEAT_MS, 0);
    for (index = 0; index < TX_ADAPTER_MAX_EVENTS; index++) {
        low.seq = (UB)(20 + index);
        check(tx_adapter_admit(&adapter, &low, 0) == TRUE, "bounded table fills");
    }
    check(tx_adapter_admit(&adapter, &high, 0) == TRUE,
          "higher severity event evicts a lower severity event");
    check(adapter.evicted == 1, "one eviction is recorded");
}

int main(void)
{
    printf("tx_adapter tests\n\n");
    test_single_event_sprays();
    test_packet_id_is_stable_across_spray();
    test_concurrent_events_interleave();
    test_heartbeat_is_not_starved();
    test_clock_wrap_and_severity_eviction();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
