/* Host checks for the dedicated wearable test injector's normal event path. */

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "ble_emit.h"
#include "ble_radio.h"
#include "fusion.h"
#include "schema.h"
#include "tx_adapter.h"
#include "wearable_test_injector.h"

static int failures;
static int checks;
static ID flagged_id;
static UINT flagged_pattern;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* Required to link ble_emit.c; this test checks its pack path only. */
void ble_radio_init(void)
{
}

void ble_radio_advertise(const UB *adv, UINT adv_len, const UB *addr6)
{
    (void)adv;
    (void)adv_len;
    (void)addr6;
}

/* These are the injector's normal-driver fallbacks; first-event assertions
 * below never invoke them. */
INT sensors_read(sensor_sample_t *out)
{
    (void)out;
    return -1;
}

fall_event_t fall_update(const sensor_sample_t *sample)
{
    (void)sample;
    return FALL_EVT_NONE;
}

UW fall_last_peak(void)
{
    return 0u;
}

sound_event_t sound_update(void)
{
    return SOUND_EVT_NONE;
}

UINT sound_last_level(void)
{
    return 0u;
}

ER tk_set_flg(ID flgid, UINT ptn)
{
    flagged_id = flgid;
    flagged_pattern = ptn;
    return E_OK;
}

static void test_injected_incident_uses_normal_path(void)
{
    wearable_test_injection_t injection;
    incident_state_t incident;
    incident_state_t emitted;
    tx_adapter_t adapter;
    UB packet[BLE_ADV_MAX_DATA];
    UINT packet_len;
    UW now_ms;
    UINT copies = 0;
    int identity_mismatches = 0;

    printf("test_injected_incident_uses_normal_path\n");
    check(EVENT_TX_BUDGET_MS == 1000u,
          "injector uses the production 1000 ms spray budget");
    check(EVENT_TX_MIN_COUNT == 8u,
          "injector uses the production minimum eight-copy policy");
    wearable_test_injector_seed(&injection);
    check(injection.fall_event == FALL_EVT_CONFIRMED,
          "injector seeds a confirmed fall input");
    check(injection.sound_event == SOUND_EVT_POSSIBLE_DISTRESS,
          "injector seeds a distress-shout input");
    check(injection.accel_svm == 4242u, "injector seeds the fixed SVM");
    check(injection.mic_level == 96u, "injector seeds the fixed microphone level");

    {
        sensor_sample_t sample;

        memset(&sample, 0, sizeof(sample));
        check(wearable_test_injector_sensors_read(&sample) == 0,
              "first injector sensor sample succeeds without hardware input");
        check(sample.svm_mg == injection.accel_svm,
              "first injector sensor sample uses the fixed SVM");
        check(wearable_test_injector_fall_update(&sample) == injection.fall_event,
              "first injector fall update uses the fixed fall input");
        check(wearable_test_injector_fall_last_peak() == injection.accel_svm,
              "first injector fall peak uses the fixed SVM");
        check(wearable_test_injector_sound_update() == injection.sound_event,
              "first injector sound update uses the fixed shout input");
        check(wearable_test_injector_sound_last_level() == injection.mic_level,
              "first injector sound level uses the fixed microphone level");

        flagged_id = 0;
        flagged_pattern = 0u;
        check(wearable_test_injector_set_flg(7, 0x01u) == E_OK,
              "injector holds the fall flag until the shout is ready");
        check(flagged_pattern == 0u, "held fall flag is not published alone");
        check(wearable_test_injector_set_flg(7, 0x02u) == E_OK,
              "injector publishes the combined event wakeup");
        check(flagged_id == 7 && flagged_pattern == 0x03u,
              "combined wakeup contains both fusion producer flags");
    }

    fusion_init();
    memset(&incident, 0, sizeof(incident));
    check(fusion_update(injection.fall_event, injection.sound_event,
                        injection.accel_svm, injection.mic_level, 0u,
                        &incident) == TRUE,
          "injected detector values resolve through fusion");
    check(incident.event_type == MIND_EVT_FALL_AND_SHOUT,
          "injected incident is a non-heartbeat fused event");
    check(incident.confidence == 100u, "fused incident has deterministic confidence");
    check(incident.accel_svm == injection.accel_svm,
          "fused incident preserves injected SVM");
    check(incident.mic_level == injection.mic_level,
          "fused incident preserves injected microphone level");

    /* The normal admission path assigns the first packet identity from its
     * zero-initialized device-local sequence. Zero is a legal packet id. */
    incident.event_id = 0u;
    incident.seq = (UB)(incident.event_id & 0xffu);
    tx_adapter_init(&adapter, EVENT_TX_BUDGET_MS, EVENT_TX_INTERVAL_MS,
                    EVENT_TX_MIN_COUNT, HEARTBEAT_INTERVAL_MS, 0u);
    check(tx_adapter_admit(&adapter, &incident, 0u) == TRUE,
          "fused incident is admitted by the production adapter");

    for (now_ms = 0u; now_ms <= EVENT_TX_BUDGET_MS; now_ms += TX_TICK_MS) {
        if (tx_adapter_next(&adapter, now_ms, &emitted) == TX_ADAPTER_SEND_EVENT) {
            copies++;
            if (emitted.event_id != incident.event_id ||
                emitted.seq != incident.seq ||
                emitted.event_type != incident.event_type ||
                emitted.confidence != incident.confidence ||
                emitted.accel_svm != incident.accel_svm ||
                emitted.mic_level != incident.mic_level) {
                identity_mismatches++;
            }
        }
    }
    check(copies >= EVENT_TX_MIN_COUNT,
          "injected incident receives the normal minimum-copy spray");
    check(identity_mismatches == 0,
          "every injected spray copy retains the fused packet identity");

    packet_len = ble_emit_pack(&incident, packet);
    check(packet_len == 26u, "fused incident uses the production TM/01 encoder");
    check(packet[15] == 0x00u && packet[16] == 0x00u && packet[17] == 0x00u,
          "encoded packet preserves the first admitted 24-bit identity");
    check(packet[20] == MIND_EVT_FALL_AND_SHOUT && packet[21] == 100u,
          "encoded packet preserves fused event type and confidence");
    check(packet[22] == 0x92u && packet[23] == 0x10u && packet[24] == 96u &&
          packet[25] == 0x00u,
          "encoded packet preserves fused values and schema sequence");
}

int main(void)
{
    printf("wearable test injector tests\n\n");
    test_injected_incident_uses_normal_path();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
