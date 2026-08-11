#include "routed_benchmark.h"
#include "routed_benchmark_full.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;

#define CHECK(tag, expression) \
    do { \
        if (!(expression)) { \
            printf("FAIL %s: %s:%d: %s\n", (tag), __FILE__, __LINE__, #expression); \
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

static void test_absolute_deadlines_and_exact_target(void)
{
    routed_benchmark_state_t state;
    routed_benchmark_slot_t slot;

    CHECK("BENCH-SCHEDULE", routed_benchmark_init(&state, 1000u, 60000u, 100u, 3u));
    CHECK("BENCH-SCHEDULE", state.start_at_ms == 61000u &&
                                routed_benchmark_schedule_due(&state, 60999u, &slot) ==
                                    ROUTED_BENCHMARK_SCHEDULE_NONE);
    CHECK("BENCH-SCHEDULE", routed_benchmark_schedule_due(&state, 61000u, &slot) ==
                                    ROUTED_BENCHMARK_SCHEDULE_DUE &&
                                slot.slot == 0u && slot.counter == 0u &&
                                slot.deadline_ms == 61000u && state.offered == 1u);
    routed_benchmark_record_submission(&state, 0u);
    CHECK("BENCH-SCHEDULE", state.attempted == 1u && state.rejected == 1u &&
                                state.payload_counter == 0u &&
                                routed_benchmark_schedule_due(&state, 61099u, &slot) ==
                                    ROUTED_BENCHMARK_SCHEDULE_NONE);
    CHECK("BENCH-SCHEDULE", routed_benchmark_schedule_due(&state, 61100u, &slot) ==
                                    ROUTED_BENCHMARK_SCHEDULE_DUE &&
                                slot.slot == 1u && slot.counter == 0u &&
                                slot.deadline_ms == 61100u);
    routed_benchmark_record_submission(&state, 1u);
    CHECK("BENCH-SCHEDULE", state.accepted == 1u && state.payload_counter == 1u &&
                                routed_benchmark_schedule_due(&state, 61200u, &slot) ==
                                    ROUTED_BENCHMARK_SCHEDULE_DUE &&
                                slot.slot == 2u && slot.counter == 1u);
    routed_benchmark_record_submission(&state, 1u);
    CHECK("BENCH-SCHEDULE", state.offered == 3u && state.attempted == 3u &&
                                state.accepted == 2u && state.rejected == 1u &&
                                routed_benchmark_schedule_due(&state, 61300u, &slot) ==
                                    ROUTED_BENCHMARK_SCHEDULE_NONE);
}

static void test_skipped_deadlines_and_wrap(void)
{
    routed_benchmark_state_t state;
    routed_benchmark_slot_t slot;

    CHECK("BENCH-SKIP", routed_benchmark_init(&state, 0u, 100u, 100u, 6u) &&
                            routed_benchmark_schedule_due(&state, 450u, &slot) ==
                                ROUTED_BENCHMARK_SCHEDULE_DUE &&
                            slot.slot == 3u && slot.deadline_ms == 400u &&
                            state.offered == 4u && state.skipped == 3u &&
                            state.attempted == 0u);
    routed_benchmark_record_not_ready(&state);
    CHECK("BENCH-SKIP", state.rejected == 1u && state.not_ready == 1u &&
                            state.payload_counter == 0u &&
                            routed_benchmark_schedule_due(&state, 650u, &slot) ==
                                ROUTED_BENCHMARK_SCHEDULE_DUE &&
                            slot.slot == 5u && slot.deadline_ms == 600u &&
                            state.offered == 6u && state.skipped == 4u);

    CHECK("BENCH-SKIP", routed_benchmark_init(&state, UINT32_MAX - 49u, 100u,
                                                100u, 1u) &&
                            state.start_at_ms == 50u &&
                            routed_benchmark_schedule_due(&state, 49u, &slot) ==
                                ROUTED_BENCHMARK_SCHEDULE_NONE &&
                            routed_benchmark_schedule_due(&state, 50u, &slot) ==
                                ROUTED_BENCHMARK_SCHEDULE_DUE);
}

static void test_attempt_queue_and_counter_decode(void)
{
    routed_benchmark_attempt_queue_t queue;
    routed_benchmark_attempt_queue_snapshot_t snapshot;
    routed_benchmark_attempt_t attempt;
    routed_benchmark_attempt_t observed;
    uint8_t bytes[4] = { 0x78u, 0x56u, 0x34u, 0x12u };
    uint32_t counter = UINT32_MAX;
    uint8_t index;

    routed_benchmark_attempt_queue_init(&queue);
    memset(&attempt, 0, sizeof(attempt));
    /* A healthy 10 Hz producer and logger drain never retain more than one
     * record and therefore cannot change the offered-load denominator. */
    for (index = 0u; index < 100u; index++) {
        attempt.slot = index;
        CHECK("BENCH-QUEUE", routed_benchmark_attempt_queue_offer(&queue, &attempt) ==
                                 ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK &&
                                 routed_benchmark_attempt_queue_take(&queue, &observed) ==
                                     ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK &&
                                 observed.slot == index);
    }
    CHECK("BENCH-QUEUE", routed_benchmark_attempt_queue_snapshot(&queue, &snapshot) ==
                             ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK && snapshot.count == 0u &&
                             snapshot.high_water == 1u && snapshot.dropped_count == 0u);
    for (index = 0u; index < ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY; index++) {
        attempt.slot = index;
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
    queue.count = ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY + 1u;
    CHECK("BENCH-QUEUE", routed_benchmark_attempt_queue_take(&queue, &observed) ==
                             ROUTED_BENCHMARK_ATTEMPT_QUEUE_INVALID);

    CHECK("BENCH-DECODE", routed_benchmark_decode_counter(0x7fu, 4u, bytes, &counter) &&
                              counter == 0x12345678u);
    counter = UINT32_MAX;
    CHECK("BENCH-DECODE", !routed_benchmark_decode_counter(0x7fu, 3u, bytes, &counter) &&
                              counter == 0u &&
                              !routed_benchmark_decode_counter(0x01u, 4u, bytes, &counter));
}

static void test_full_destination_readiness_is_fail_closed(void)
{
    tavrn_gtt_storage_t storage;
    tavrn_gtt_t gtt;
    tavrn_full_t full;
    tavrn_gtt_config_t config;
    tavrn_logical_id_t resolved;
    routed_benchmark_state_t state;
    routed_benchmark_slot_t slot;
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

    CHECK("BENCH-FULL", routed_benchmark_init(&state, 0u, 1u, 10u, 1u) &&
                            routed_benchmark_schedule_due(&state, 1u, &slot) ==
                                ROUTED_BENCHMARK_SCHEDULE_DUE);
    routed_benchmark_record_not_ready(&state);
    CHECK("BENCH-FULL", state.offered == 1u && state.attempted == 0u &&
                            state.rejected == 1u && state.not_ready == 1u);

    CHECK("BENCH-FULL", observe(&gtt, destination, 1u) &&
                            routed_benchmark_destination_ready(&full, &destination, 2u,
                                                               &resolved) ==
                                ROUTED_BENCHMARK_DESTINATION_READY &&
                            resolved.width == TAVRN_IDENTITY_SID8 &&
                            resolved.value == 0x42u && observe(&gtt, collision, 2u) &&
                            routed_benchmark_destination_ready(&full, &destination, 2u,
                                                               &resolved) ==
                                ROUTED_BENCHMARK_DESTINATION_COLLIDING);

    memset(&storage, 0, sizeof(storage));
    CHECK("BENCH-FULL", tavrn_gtt_init(&gtt, &storage, &config, 0u) ==
                            TAVRN_GTT_INIT_OK && tavrn_full_init(&full, &gtt) ==
                            TAVRN_FULL_INIT_OK && observe(&gtt, destination, 1u) &&
                            routed_benchmark_destination_ready(&full, &destination, 22u,
                                                               &resolved) ==
                                ROUTED_BENCHMARK_DESTINATION_NOT_ACTIVE);
}

int main(void)
{
    test_absolute_deadlines_and_exact_target();
    test_skipped_deadlines_and_wrap();
    test_attempt_queue_and_counter_decode();
    test_full_destination_readiness_is_fail_closed();
    if (failures != 0u) {
        printf("routed benchmark tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("routed benchmark tests passed\n");
    return 0;
}
