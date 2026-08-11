#include "routed_benchmark.h"
#include "routed_benchmark_full.h"
#include "routed_full_telemetry.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;

#define CHECK(tag, expression) \
    do { \
        if (!(expression)) { \
            printf("FAIL %s: %s:%d: %s\n", tag, __FILE__, __LINE__, #expression); \
            failures++; \
        } \
    } while (0)

static tavrn_adva_t adva(uint8_t first, uint8_t seed)
{
    tavrn_adva_t value = {
        .bytes = { first, (uint8_t)(seed + 1u), (uint8_t)(seed + 2u),
                   (uint8_t)(seed + 3u), (uint8_t)(seed + 4u),
                   (uint8_t)(0xc0u | (seed & 0x3fu)) },
    };

    return value;
}

static int observe(tavrn_gtt_t *gtt, tavrn_adva_t identity, uint32_t now)
{
    tavrn_gtt_evidence_t evidence;

    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = identity;
    evidence.serial = 1u;
    evidence.serial_present = 1u;
    evidence.hop_count = 1u;
    evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    return tavrn_gtt_observe(gtt, &evidence, now) == TAVRN_GTT_OBSERVE_ADDED;
}

static void test_continuous_absolute_deadlines(void)
{
    routed_benchmark_state_t state;
    routed_benchmark_slot_t slot;
    routed_benchmark_workload_t workload;
    uint32_t burst;
    uint16_t sequence;
    uint64_t record_id;

    CHECK("BENCH-SCHEDULE", !routed_benchmark_init(&state, 0u, 0u));
    CHECK("BENCH-SCHEDULE", routed_benchmark_init(&state, 0u, 73u));
    CHECK("BENCH-SCHEDULE", state.started_at_ms == 0u);
    CHECK("BENCH-SCHEDULE", routed_benchmark_schedule_due(&state, 0u, &slot) ==
                                ROUTED_BENCHMARK_SCHEDULE_DUE &&
                                slot.workload == ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT &&
                                slot.deadline_ms == 0u && slot.burst == 0u &&
                                slot.sequence == 0u && slot.identity == 0u &&
                                state.offered == 1u);
    routed_benchmark_record_submission(&state, 0u);
    CHECK("BENCH-SCHEDULE", state.accepted == 0u && state.rejected == 1u &&
                                routed_benchmark_schedule_due(&state, 999u, &slot) ==
                                    ROUTED_BENCHMARK_SCHEDULE_NONE &&
                                routed_benchmark_schedule_due(&state, 1000u, &slot) ==
                                    ROUTED_BENCHMARK_SCHEDULE_DUE &&
                                slot.sequence == 1u && slot.identity == 1u);
    routed_benchmark_record_not_ready(&state);
    CHECK("BENCH-SCHEDULE", state.rejected == 2u && state.not_ready == 1u);

    CHECK("BENCH-SCHEDULE", routed_benchmark_schedule_due(&state, 60000u, &slot) ==
                                ROUTED_BENCHMARK_SCHEDULE_DUE &&
                                slot.workload == ROUTED_BENCHMARK_WORKLOAD_THROUGHPUT &&
                                slot.deadline_ms == 60000u && slot.burst == 0u &&
                                slot.sequence == 0u && slot.identity == 0x80000000u &&
                                routed_benchmark_identity_decode(slot.identity, &workload,
                                                                 &burst, &sequence) &&
                                workload == ROUTED_BENCHMARK_WORKLOAD_THROUGHPUT &&
                                burst == 0u && sequence == 0u);
    CHECK("BENCH-SCHEDULE", routed_benchmark_schedule_due(&state, 60001u, &slot) ==
                                ROUTED_BENCHMARK_SCHEDULE_DUE &&
                                slot.workload == ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT &&
                                slot.deadline_ms == 60000u && slot.burst == 0u &&
                                slot.sequence == 60u && state.heartbeat_skipped == 58u);
    CHECK("BENCH-SCHEDULE", routed_benchmark_schedule_due(&state, 61050u, &slot) ==
                                ROUTED_BENCHMARK_SCHEDULE_DUE &&
                                slot.workload == ROUTED_BENCHMARK_WORKLOAD_THROUGHPUT &&
                                slot.deadline_ms == 61000u && slot.sequence == 10u &&
                                state.throughput_skipped == 9u);
    CHECK("BENCH-SCHEDULE", routed_benchmark_schedule_due(&state, 510000u, &slot) ==
                                ROUTED_BENCHMARK_SCHEDULE_DUE &&
                                slot.workload == ROUTED_BENCHMARK_WORKLOAD_THROUGHPUT &&
                                slot.deadline_ms == 510000u && slot.burst == 1u &&
                                slot.sequence == 0u && state.throughput_skipped == 598u);
    CHECK("BENCH-SCHEDULE", routed_benchmark_next_record_id(&state, &record_id) &&
                                record_id == 1u &&
                                state.session_id == 73u &&
                                routed_benchmark_schedule_due(&state,
                                                             UINT32_MAX - 100u, &slot) !=
                                    ROUTED_BENCHMARK_SCHEDULE_INVALID);
}

static void test_newest_due_slot_selection(void)
{
    routed_benchmark_state_t state;
    routed_benchmark_slot_t slot;
    uint32_t wrapped_start = UINT32_MAX - 59999u;

    /* Equal deadlines retain the losing workload for the following cycle. */
    CHECK("BENCH-SCHEDULE-TIE", routed_benchmark_init(&state, 0u, 71u) &&
                                      routed_benchmark_schedule_due(&state, 60000u,
                                                                   &slot) ==
                                          ROUTED_BENCHMARK_SCHEDULE_DUE &&
                                      slot.workload ==
                                          ROUTED_BENCHMARK_WORKLOAD_THROUGHPUT &&
                                      slot.deadline_ms == 60000u &&
                                      slot.sequence == 0u &&
                                      routed_benchmark_schedule_due(&state, 60000u,
                                                                   &slot) ==
                                          ROUTED_BENCHMARK_SCHEDULE_DUE &&
                                      slot.workload ==
                                          ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT &&
                                      slot.deadline_ms == 60000u &&
                                      slot.sequence == 60u &&
                                      state.heartbeat_skipped == 60u &&
                                      state.throughput_skipped == 0u);

    /* A strictly older heartbeat is accounted and never replayed. */
    CHECK("BENCH-SCHEDULE-NEWEST", routed_benchmark_init(&state, 0u, 72u) &&
                                         routed_benchmark_schedule_due(&state, 60150u,
                                                                      &slot) ==
                                             ROUTED_BENCHMARK_SCHEDULE_DUE &&
                                         slot.workload ==
                                             ROUTED_BENCHMARK_WORKLOAD_THROUGHPUT &&
                                         slot.deadline_ms == 60100u &&
                                         slot.sequence == 1u &&
                                         state.heartbeat_skipped == 61u &&
                                         state.throughput_skipped == 1u &&
                                         routed_benchmark_schedule_due(&state, 60150u,
                                                                      &slot) ==
                                             ROUTED_BENCHMARK_SCHEDULE_NONE);

    /* The same strict-order rule must survive the uint32_t time wrap. */
    CHECK("BENCH-SCHEDULE-WRAP", routed_benchmark_init(&state, wrapped_start, 73u) &&
                                       state.started_at_ms == wrapped_start &&
                                       routed_benchmark_schedule_due(&state, 150u,
                                                                    &slot) ==
                                           ROUTED_BENCHMARK_SCHEDULE_DUE &&
                                       slot.workload ==
                                           ROUTED_BENCHMARK_WORKLOAD_THROUGHPUT &&
                                       slot.deadline_ms == 100u && slot.sequence == 1u &&
                                       state.heartbeat_skipped == 61u &&
                                       state.throughput_skipped == 1u &&
                                       routed_benchmark_schedule_due(&state, 150u,
                                                                    &slot) ==
                                           ROUTED_BENCHMARK_SCHEDULE_NONE);
}

static void test_queue_and_identity_decode(void)
{
    routed_benchmark_attempt_queue_t queue;
    routed_benchmark_attempt_queue_snapshot_t snapshot;
    routed_benchmark_attempt_t attempt;
    routed_benchmark_attempt_t observed;
    uint8_t bytes[ROUTED_BENCHMARK_APP_PAYLOAD_BYTES] = {
        0x44u, 0x33u, 0x22u, 0x11u, 0x78u, 0x56u, 0x34u, 0x92u,
    };
    uint32_t origin_session = UINT32_MAX;
    uint32_t identity = UINT32_MAX;
    uint16_t index;

    routed_benchmark_attempt_queue_init(&queue);
    memset(&attempt, 0, sizeof(attempt));
    for (index = 0u; index < ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY; index++) {
        attempt.sequence = index;
        attempt.attempted = (uint8_t)(index & 1u);
        CHECK("BENCH-QUEUE", routed_benchmark_attempt_queue_offer(&queue, &attempt) ==
                                 ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK);
    }
    CHECK("BENCH-QUEUE", routed_benchmark_attempt_queue_offer(&queue, &attempt) ==
                             ROUTED_BENCHMARK_ATTEMPT_QUEUE_DROPPED &&
                             routed_benchmark_attempt_queue_snapshot(&queue, &snapshot) ==
                                 ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK &&
                             snapshot.count == ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY &&
                             snapshot.high_water == ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY &&
                             snapshot.dropped_count == 1u);
    CHECK("BENCH-QUEUE", routed_benchmark_attempt_queue_take(&queue, &observed) ==
                              ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK && observed.sequence == 0u &&
                              observed.attempted == 0u);
    queue.count = ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY + 1u;
    CHECK("BENCH-QUEUE", routed_benchmark_attempt_queue_take(&queue, &observed) ==
                             ROUTED_BENCHMARK_ATTEMPT_QUEUE_INVALID);

    CHECK("BENCH-DECODE", routed_benchmark_decode_payload(
                               ROUTED_BENCHMARK_APP_KIND,
                               ROUTED_BENCHMARK_APP_PAYLOAD_BYTES, bytes,
                               &origin_session, &identity) &&
                               origin_session == 0x11223344u &&
                               identity == 0x92345678u);
    identity = UINT32_MAX;
    origin_session = UINT32_MAX;
    CHECK("BENCH-DECODE", !routed_benchmark_decode_payload(
                               ROUTED_BENCHMARK_APP_KIND, 7u, bytes,
                               &origin_session, &identity) && identity == 0u &&
                               origin_session == 0u &&
                               !routed_benchmark_decode_payload(
                                   0x01u, ROUTED_BENCHMARK_APP_PAYLOAD_BYTES,
                                   bytes, &origin_session, &identity));
    memset(bytes, 0, 4u);
    identity = UINT32_MAX;
    origin_session = UINT32_MAX;
    CHECK("BENCH-DECODE", !routed_benchmark_decode_payload(
                               ROUTED_BENCHMARK_APP_KIND,
                               ROUTED_BENCHMARK_APP_PAYLOAD_BYTES, bytes,
                               &origin_session, &identity) &&
                               origin_session == 0u && identity == 0u);
}

static void test_full_destination_readiness_is_fail_closed(void)
{
    tavrn_gtt_storage_t storage;
    tavrn_gtt_t gtt;
    tavrn_full_t full;
    tavrn_gtt_config_t config;
    tavrn_logical_id_t resolved;
    tavrn_adva_t local = adva(0x11u, 1u);
    tavrn_adva_t destination = adva(0x42u, 2u);
    tavrn_adva_t collision = adva(0x42u, 9u);

    memset(&storage, 0, sizeof(storage));
    memset(&config, 0, sizeof(config));
    config.local_identity = local;
    config.soft_expiry_ms = 10u;
    config.hard_expiry_ms = 20u;
    config.departed_retention_ms = 40u;
    CHECK("BENCH-FULL", tavrn_gtt_init(&gtt, &storage, &config, 0u) ==
                             TAVRN_GTT_INIT_OK && tavrn_full_init(&full, &gtt) ==
                             TAVRN_FULL_INIT_OK &&
                             routed_benchmark_destination_ready(&full, &destination, 0u,
                                                                &resolved) ==
                                 ROUTED_BENCHMARK_DESTINATION_UNKNOWN);
    CHECK("BENCH-FULL", observe(&gtt, destination, 1u) &&
                             routed_benchmark_destination_ready(&full, &destination, 2u,
                                                                &resolved) ==
                                 ROUTED_BENCHMARK_DESTINATION_READY &&
                             resolved.width == TAVRN_IDENTITY_SID8 &&
                             resolved.value == 0x42u && observe(&gtt, collision, 2u) &&
                             routed_benchmark_destination_ready(&full, &destination, 2u,
                                                                &resolved) ==
                                 ROUTED_BENCHMARK_DESTINATION_COLLIDING);
}

static void test_gtt_snapshot_is_read_only(void)
{
    tavrn_gtt_storage_t storage;
    tavrn_gtt_storage_t before;
    tavrn_gtt_t gtt;
    tavrn_gtt_config_t config;
    routed_cycle_gtt_snapshot_t snapshot;
    tavrn_adva_t local = adva(0x11u, 1u);
    tavrn_adva_t destination = adva(0x42u, 2u);

    memset(&storage, 0, sizeof(storage));
    memset(&config, 0, sizeof(config));
    config.local_identity = local;
    config.soft_expiry_ms = 10u;
    config.hard_expiry_ms = 20u;
    config.departed_retention_ms = 40u;
    CHECK("BENCH-GTT", tavrn_gtt_init(&gtt, &storage, &config, 0u) ==
                            TAVRN_GTT_INIT_OK && observe(&gtt, destination, 1u));
    before = storage;
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK("BENCH-GTT", routed_full_telemetry_snapshot_gtt(&gtt, 2u, &snapshot) ==
                           ROUTED_FULL_TELEMETRY_OK && snapshot.query_at_ms == 2u &&
                           snapshot.entry_count == 2u &&
                           snapshot.nondeparted_count == 2u &&
                           memcmp(&storage, &before, sizeof(storage)) == 0);
    memset(&snapshot, 0xff, sizeof(snapshot));
    gtt.storage = NULL;
    CHECK("BENCH-GTT", routed_full_telemetry_snapshot_gtt(&gtt, 2u, &snapshot) ==
                           ROUTED_FULL_TELEMETRY_INVALID &&
                           snapshot.entries[0].canonical_adva.bytes[0] == 0xffu);
}

int main(void)
{
    test_continuous_absolute_deadlines();
    test_newest_due_slot_selection();
    test_queue_and_identity_decode();
    test_full_destination_readiness_is_fail_closed();
    test_gtt_snapshot_is_read_only();
    if (failures != 0u) {
        printf("routed benchmark tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("routed benchmark tests passed\n");
    return 0;
}
