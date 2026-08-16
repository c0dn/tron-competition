/* Host vector test for the real wearable TM/01 encoder. */

#include <stdio.h>
#include <string.h>

#include "app_config.h"
#include "ble_emit.h"
#include "ble_radio.h"
#include "schema.h"
#include "tron_mesh_packet.h"

static int failures;
static int checks;
static UB advertised[BLE_ADV_MAX_DATA];
static UB advertised_addr[6];
static UINT advertised_len;
static UINT advertised_calls;

static void check(int condition, const char *what)
{
    checks++;
    if (!condition) {
        failures++;
        printf("  FAIL: %s\n", what);
    }
}

/* ble_emit.c owns framing; these radio calls are only required to link it. */
void ble_radio_init(void)
{
}

void ble_radio_advertise(const UB *adv, UINT adv_len, const UB *addr6)
{
    advertised_calls++;
    advertised_len = adv_len;
    memcpy(advertised, adv, adv_len);
    memcpy(advertised_addr, addr6, sizeof(advertised_addr));
}

static void test_tm01_vector_and_repeated_copy(void)
{
    incident_state_t incident;
    UB first[BLE_ADV_MAX_DATA];
    UB second[BLE_ADV_MAX_DATA];
    UINT first_len;
    UINT second_len;
    static const UB expected[] = {
        0x02u, 0x01u, 0x06u, 0x16u, 0xffu, 0xffu, 0xffu,
        'T', 'M', 0x01u, TRON_MESH_MSG_TYPE_MIND_EVENT, 0x01u, 0x00u,
        0x01u, 0x01u, 0xefu, 0xcdu, 0xabu, 0x07u,
        MIND_SCHEMA_VERSION, MIND_EVT_FALL_AND_SHOUT, 73u, 0x1fu, 0x1au,
        0x7du, 0xefu,
    };
    static const UB expected_adva[] = MIND_ADVA(DEVICE_ID);

    memset(&incident, 0, sizeof(incident));
    incident.event_type = MIND_EVT_FALL_AND_SHOUT;
    incident.confidence = 73u;
    incident.accel_svm = 0x1a1fu;
    incident.mic_level = 0x7du;
    incident.event_id = 0x12abcdefu;
    incident.seq = 0u; /* Encoder must derive schema seq from event_id. */

    first_len = ble_emit_pack(&incident, first);
    second_len = ble_emit_pack(&incident, second);

    check(first_len == 26u, "TM/01 output is exactly 26 bytes");
    check(second_len == first_len, "repeated copy has the same length");
    check(memcmp(first, expected, sizeof(expected)) == 0,
          "TM/01 contains the exact direct-ingress vector");
    check(memcmp(first, second, first_len) == 0,
          "repeated copy is byte-identical");
    check(first[10] == TRON_MESH_MSG_TYPE_MIND_EVENT &&
          TRON_MESH_MSG_TYPE_MIND_EVENT == 0x04u,
          "MIND_EVENT uses the non-conflicting 0x04 type");
    check(first[12] == 0u, "direct ingress TTL is zero");
    check(first[11] == MIND_MESH_NET_ID && first[13] == 0x01u &&
          first[14] == 0x01u,
          "network and wearable source are encoded");
    check(first[15] == 0xefu && first[16] == 0xcdu && first[17] == 0xabu,
           "packet id is masked to its low 24 bits");
    check(first[25] == (UB)(incident.event_id & 0xFFu),
          "schema seq matches the packet id low byte");
    check(memcmp(&first[19], expected + 19u, MIND_PAYLOAD_SIZE) == 0,
           "all seven schema bytes are preserved");

    ble_emit_advertise(&incident);
    check(advertised_calls == 1u, "direct-ingress packet advertises once");
    check(advertised_len == first_len &&
          memcmp(advertised, first, first_len) == 0,
          "advertise emits the tested TM/01 packet");
    check(memcmp(advertised_addr, expected_adva, sizeof(expected_adva)) == 0,
          "advertise uses the configured wearable AdvA");
}

int main(void)
{
    printf("ble_emit tests\n\n");
    test_tm01_vector_and_repeated_copy();
    printf("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
