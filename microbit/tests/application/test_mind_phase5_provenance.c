#include "mind_phase5_provenance.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        failures++;
        printf("FAIL: %s\n", message);
    }
}

static void test_stable_branches_and_sentinels(void)
{
    mind_phase5_provenance_snapshot_t snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    check(MIND_PHASE5_INVALID_BRANCH_INITIAL_QUEUE_GUARD == 1u &&
          MIND_PHASE5_INVALID_BRANCH_FULL_MAILBOX_TAKE == 2u &&
          MIND_PHASE5_INVALID_BRANCH_BINDING_CALL == 3u &&
          MIND_PHASE5_INVALID_BRANCH_RAW_ROUTER_STATUS == 4u &&
          MIND_PHASE5_INVALID_BRANCH_REPAIR_BINDING == 5u &&
          MIND_PHASE5_INVALID_BRANCH_EXPIRY_SWEEP_ENQUEUE == 6u &&
          MIND_PHASE5_INVALID_BRANCH_APPLICATION_PUBLISH_GUARD == 7u &&
          MIND_PHASE5_INVALID_BRANCH_APPLICATION_PUBLISH == 8u &&
          MIND_PHASE5_INVALID_BRANCH_APPLICATION_RESULT_RANGE == 9u &&
          MIND_PHASE5_INVALID_BRANCH_FINAL_BINDING_RESULT == 10u &&
          MIND_PHASE5_INVALID_BRANCH_COUNT == 11u,
          "phase-5 branch identifiers remain stable and complete");
    check(mind_phase5_provenance_begin(
              &snapshot, MIND_PHASE5_INVALID_BRANCH_INITIAL_QUEUE_GUARD, 153007u),
          "first phase-5 capture reserves the empty static snapshot");
    check(snapshot.valid == 0u && snapshot.capture_in_progress != 0u &&
          snapshot.timestamp_ms == 153007u &&
          snapshot.first_invalid_branch == MIND_PHASE5_INVALID_BRANCH_INITIAL_QUEUE_GUARD &&
          snapshot.mailbox_take_present == 0u &&
          snapshot.binding_call_present == 0u && snapshot.repair_call_present == 0u &&
          snapshot.expiry_sweep_present == 0u && snapshot.tick_trace_present == 0u &&
          snapshot.uart_present == 0u &&
          snapshot.mailbox_take_status == MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN &&
          snapshot.binding_call_status == MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN &&
          snapshot.raw_router_status == MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN &&
          snapshot.repair_binding_status == MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN &&
          snapshot.expiry_enqueue_outcome == MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN &&
          snapshot.expiry_queue_count == MIND_PHASE5_PROVENANCE_QUEUE_UNKNOWN &&
          snapshot.tick_status == MIND_PHASE5_PROVENANCE_STATUS_UNKNOWN,
          "early branches expose explicit absence bits and non-OK sentinels");
}

static void test_first_write_wins_and_one_shot_format_right(void)
{
    mind_phase5_provenance_snapshot_t snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    check(mind_phase5_provenance_begin(
              &snapshot, MIND_PHASE5_INVALID_BRANCH_EXPIRY_SWEEP_ENQUEUE, 17u),
          "first writer reserves the snapshot");
    check(!mind_phase5_provenance_begin(
              &snapshot, MIND_PHASE5_INVALID_BRANCH_APPLICATION_PUBLISH, 18u),
          "in-progress capture rejects a second writer before publication");
    snapshot.expiry_sweep_present = 1u;
    snapshot.expiry_sweep_pass = 1u;
    snapshot.expiry_enqueue_attempted = 1u;
    snapshot.expiry_enqueue_outcome = 0u;
    snapshot.expiry_queue_occupancy_present = 1u;
    snapshot.expiry_queue_count = 1u;
    mind_phase5_provenance_publish(&snapshot);
    check(snapshot.valid != 0u && snapshot.capture_in_progress == 0u &&
          snapshot.first_invalid_branch ==
              MIND_PHASE5_INVALID_BRANCH_EXPIRY_SWEEP_ENQUEUE &&
          snapshot.expiry_queue_count == 1u,
          "publication exposes the fully written first capture only");
    check(!mind_phase5_provenance_begin(
              &snapshot, MIND_PHASE5_INVALID_BRANCH_FINAL_BINDING_RESULT, 19u),
          "published first capture remains first-write-wins");
    check(mind_phase5_provenance_mark_logged(&snapshot) &&
          !mind_phase5_provenance_mark_logged(&snapshot) && snapshot.logged != 0u,
          "logger formatting permission is consumed exactly once");
}

int main(void)
{
    test_stable_branches_and_sentinels();
    test_first_write_wins_and_one_shot_format_right();
    if (failures != 0u) {
        printf("mind_phase5_provenance failures=%u\n", failures);
        return 1;
    }
    printf("mind_phase5_provenance tests passed\n");
    return 0;
}
