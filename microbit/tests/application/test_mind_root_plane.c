#include "mind_root_plane.h"

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

static tavrn_adva_t identity(uint8_t value)
{
    tavrn_adva_t result = { { value, (uint8_t)(value + 1u), 0x54u, 0x56u,
                              0x78u, 0xc0u } };
    return result;
}

static mind_application_root_state_t root_state(uint8_t active, uint16_t nonce,
                                                 uint16_t generation)
{
    mind_application_root_state_t state;

    state.active = active;
    state.nonce = nonce;
    state.generation = generation;
    return state;
}

static void test_registry_freshness_and_capacity(void)
{
    mind_root_plane_t plane;
    tavrn_adva_t local = identity(0x80u);
    tavrn_adva_t remote = identity(1u);
    mind_root_plane_status_t status;
    mind_application_root_state_t on = root_state(1u, 10u, 1u);
    mind_application_root_state_t off = root_state(0u, 10u, 2u);
    uint8_t value;

    mind_root_plane_init(&plane, &local, 7u);
    check(mind_root_plane_local_state(&plane).generation == 1u &&
          mind_root_plane_local_active(&plane) == 0u &&
          mind_root_plane_active_count(&plane) == 0u,
          "local starts inactive at generation one with zero roots");
    check(mind_root_plane_set_local(&plane, 1u, 0u) == MIND_ROOT_APPLY_ACCEPTED &&
          mind_root_plane_local_state(&plane).generation == 2u &&
          mind_root_plane_active_count(&plane) == 1u,
          "local on advances generation and produces one root");
    check(mind_root_plane_set_local(&plane, 1u, 1u) == MIND_ROOT_APPLY_DUPLICATE,
          "repeated local on is duplicate");
    check(mind_root_plane_receive_root_state(&plane, &remote, &on) ==
              MIND_ROOT_APPLY_ACCEPTED &&
          mind_root_plane_active_count(&plane) == 2u &&
          mind_root_plane_receive_root_state(&plane, &remote, &on) ==
              MIND_ROOT_APPLY_DUPLICATE,
          "remote state reaches two roots then deduplicates");
    on.active = 0u;
    check(mind_root_plane_receive_root_state(&plane, &remote, &on) ==
              MIND_ROOT_APPLY_STALE,
          "equal generation conflicting active is stale");
    check(mind_root_plane_receive_root_state(&plane, &remote, &off) ==
              MIND_ROOT_APPLY_ACCEPTED && mind_root_plane_active_count(&plane) == 1u,
          "newer off remains accepted inactive high-water");
    check(mind_root_plane_receive_root_state(&plane, &remote, &off) ==
              MIND_ROOT_APPLY_DUPLICATE,
          "duplicate OFF preserves inactive high-water");
    on = root_state(1u, 11u, 1u);
    check(mind_root_plane_receive_root_state(&plane, &remote, &on) ==
              MIND_ROOT_APPLY_ACCEPTED &&
          mind_root_plane_receive_root_state(&plane, &remote, &off) ==
              MIND_ROOT_APPLY_STALE,
          "new nonce replaces current and tombstones prior nonce");
    {
        mind_application_root_state_t older = root_state(1u, 11u, 65535u);

        check(mind_root_plane_receive_root_state(&plane, &remote, &older) ==
                  MIND_ROOT_APPLY_STALE,
              "older same-session generation is stale");
    }

    for (value = 2u; value <= 15u; value++) {
        tavrn_adva_t next = identity(value);

        check(mind_root_plane_receive_root_state(&plane, &next, &on) ==
                  MIND_ROOT_APPLY_ACCEPTED,
              "registry admits through sixteen total records");
    }
    mind_root_plane_status(&plane, &status);
    check(status.active_roots == 16u, "active-root view supports sixteen roots");
    {
        tavrn_adva_t full_plus_one = identity(16u);

        check(mind_root_plane_receive_root_state(&plane, &full_plus_one, &on) ==
                  MIND_ROOT_APPLY_CAPACITY_REJECTED &&
              mind_root_plane_receive_root_state(&plane, &full_plus_one, &on) ==
                  MIND_ROOT_APPLY_CAPACITY_REJECTED,
              "full unknown state is re-evaluated without hidden history");
        {
            mind_application_root_state_t unknown_off = root_state(0u, 12u, 1u);

            check(mind_root_plane_receive_root_state(&plane, &full_plus_one,
                                                      &unknown_off) ==
                      MIND_ROOT_APPLY_CAPACITY_REJECTED,
                  "full unknown OFF is rejected without displacing retained history");
        }
        {
            routed_cycle_gtt_snapshot_t departed;

            memset(&departed, 0, sizeof(departed));
            departed.entry_count = 1u;
            departed.entries[0].canonical_adva = identity(2u);
            departed.entries[0].departed = ROUTED_CYCLE_BOOLEAN_TRUE;
            check(mind_root_plane_apply_topology(&plane, &departed),
                  "explicit copied departure applies");
        }
        check(mind_root_plane_receive_root_state(&plane, &full_plus_one, &on) ==
                  MIND_ROOT_APPLY_ACCEPTED,
              "previously rejected unknown is admitted after explicit slot free");
    }
}

static void test_topology_campaign_and_ack(void)
{
    mind_root_plane_t plane;
    tavrn_adva_t local = identity(0x80u);
    tavrn_adva_t target = identity(1u);
    routed_cycle_gtt_snapshot_t snapshot;
    mind_root_submission_t submission;
    mind_application_root_ack_t ack;
    mind_root_plane_status_t status;
    uint8_t index;

    mind_root_plane_init(&plane, &local, 5u);
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.query_at_ms = 100u;
    snapshot.entry_count = 1u;
    snapshot.entries[0].canonical_adva = target;
    snapshot.entries[0].departed = ROUTED_CYCLE_BOOLEAN_FALSE;
    check(mind_root_plane_apply_topology(&plane, &snapshot),
          "OK snapshot copies topology");
    check(plane.campaigns[0].occupied != 0u && plane.topology.valid != 0u,
          "topology produces retained campaign target");
    mind_root_plane_note_snapshot_failure(&plane);
    check(plane.campaigns[0].occupied != 0u && plane.topology.valid != 0u &&
          plane.counters.snapshot_failures == 1u,
          "non-OK snapshot preserves state and records evidence");
    mind_root_plane_set_sid8_ready(&plane, 1u, 100u);
    check(!mind_root_plane_prepare_submission(&plane, 119u, &submission),
          "initial deterministic due is no earlier than 20 ms");
    check(mind_root_plane_prepare_submission(&plane, 250u, &submission) &&
          submission.kind == MIND_ROOT_SUBMISSION_STATE,
          "due campaign selects opaque root state");
    check(mind_root_plane_submission_resolved(&plane, &submission,
                                              MIND_ROOT_RESOLVER_UNIQUE, 250u),
          "unique SID8 resolution permits submit");
    mind_root_plane_submission_complete(&plane, &submission, 1u, 250u);
    check(!mind_root_plane_prepare_submission(&plane, 299u, &submission),
          "actual root submissions obey global fifty ms spacing");
    ack.state = mind_root_plane_local_state(&plane);
    ack.status = MIND_APPLICATION_ROOT_ACK_ACCEPTED;
    mind_root_plane_receive_root_ack(&plane, &target, &ack);
    mind_root_plane_status(&plane, &status);
    check(status.announced == 1u && status.acked == 1u && status.pending == 0u,
          "matching ACK completes exact campaign target");
    ack.state.generation++;
    mind_root_plane_receive_root_ack(&plane, &target, &ack);
    check(plane.counters.ack_late == 1u, "late ACK cannot complete another campaign");

    for (index = 2u; index <= 18u; index++) {
        tavrn_adva_t extra = identity(index);

        mind_root_plane_observe_target(&plane, &extra, 300u);
    }
    check(plane.counters.campaign_deferred >= 1u,
          "campaign capacity defers rather than evicting live targets");
}

static void test_nonclearing_wrap_and_campaign_retry(void)
{
    mind_root_plane_t plane;
    tavrn_adva_t local = identity(0x80u);
    tavrn_adva_t remote = identity(1u);
    mind_application_root_state_t on = root_state(1u, 2u, 1u);
    routed_cycle_gtt_snapshot_t snapshot;
    uint8_t index;
    uint8_t found_deferred = 0u;

    mind_root_plane_init(&plane, &local, 7u);
    plane.roots[plane.local_index].current.generation = 65535u;
    check(mind_root_plane_set_local(&plane, 1u, 1u) == MIND_ROOT_APPLY_ACCEPTED &&
          mind_root_plane_local_state(&plane).generation == 1u,
          "local generation wraps nonzero from 65535 to one");
    check(mind_root_plane_receive_root_state(&plane, &remote, &on) ==
              MIND_ROOT_APPLY_ACCEPTED,
          "remote root admitted before non-clearing evidence");
    memset(&snapshot, 0, sizeof(snapshot));
    check(mind_root_plane_apply_topology(&plane, &snapshot) &&
          mind_root_plane_active_count(&plane) == 2u,
          "absence from copied topology does not clear a root");
    snapshot.entry_count = 1u;
    snapshot.entries[0].canonical_adva = remote;
    snapshot.entries[0].freshness = ROUTED_CYCLE_GTT_FRESHNESS_SOFT_STALE;
    snapshot.entries[0].departed = ROUTED_CYCLE_BOOLEAN_FALSE;
    check(mind_root_plane_apply_topology(&plane, &snapshot) &&
          mind_root_plane_active_count(&plane) == 2u,
          "soft expiry does not clear root history");
    snapshot.entries[0].freshness = ROUTED_CYCLE_GTT_FRESHNESS_HARD_EXPIRED;
    check(mind_root_plane_apply_topology(&plane, &snapshot) &&
          mind_root_plane_active_count(&plane) == 2u,
          "hard expiry does not clear root history");
    mind_root_plane_note_snapshot_failure(&plane);
    check(mind_root_plane_active_count(&plane) == 2u,
          "non-OK snapshot preserves root history");

    for (index = 2u; index <= 18u; index++) {
        tavrn_adva_t target = identity(index);

        mind_root_plane_observe_target(&plane, &target, 10u);
    }
    check(plane.counters.campaign_deferred != 0u,
          "seventeenth campaign target is deferred at independent capacity");
    snapshot.entries[0].canonical_adva = identity(2u);
    snapshot.entries[0].departed = ROUTED_CYCLE_BOOLEAN_TRUE;
    check(mind_root_plane_apply_topology(&plane, &snapshot),
          "explicit departure frees one campaign target only");
    {
        tavrn_adva_t deferred = identity(18u);
        mind_root_plane_observe_target(&plane, &deferred, 20u);
    }
    for (index = 0u; index < MIND_APP_CAMPAIGN_CAPACITY; index++) {
        if (plane.campaigns[index].occupied != 0u &&
            plane.campaigns[index].canonical_adva.bytes[0] == 18u) {
            found_deferred = 1u;
        }
    }
    check(found_deferred != 0u, "deferred target is admitted after later capacity frees");
}

static void test_campaign_pacing_and_ack_outcomes(void)
{
    mind_root_plane_t plane;
    tavrn_adva_t local = identity(0x80u);
    tavrn_adva_t first = identity(1u);
    tavrn_adva_t second = identity(2u);
    tavrn_adva_t wrong = identity(3u);
    mind_root_submission_t submission;
    mind_application_root_ack_t ack;

    mind_root_plane_init(&plane, &local, 3u);
    mind_root_plane_observe_target(&plane, &first, 0u);
    mind_root_plane_observe_target(&plane, &second, 0u);
    check(plane.campaigns[0].due_at_ms >= 20u && plane.campaigns[0].due_at_ms <= 120u &&
          plane.campaigns[1].due_at_ms >= 20u && plane.campaigns[1].due_at_ms <= 120u,
          "each target receives deterministic 20..120 ms initial due");
    mind_root_plane_set_sid8_ready(&plane, 1u, 0u);
    check(mind_root_plane_prepare_submission(&plane, 200u, &submission) &&
          mind_root_plane_submission_resolved(&plane, &submission,
                                              MIND_ROOT_RESOLVER_UNIQUE, 200u),
          "first simultaneous target reaches generic submit seam");
    mind_root_plane_submission_invoked(&plane, &submission, 200u);
    mind_root_plane_submission_complete(&plane, &submission, 0u, 200u);
    check(!mind_root_plane_prepare_submission(&plane, 249u, &submission),
          "BUSY generic invocation still enforces 50 ms global pacing");
    check(mind_root_plane_prepare_submission(&plane, 250u, &submission) &&
          submission.target.bytes[0] == second.bytes[0],
          "second simultaneously due target progresses after a BUSY first target");
    mind_root_plane_submission_invoked(&plane, &submission, 250u);
    mind_root_plane_submission_complete(&plane, &submission, 0u, 250u);
    check(!mind_root_plane_prepare_submission(&plane, 299u, &submission),
          "repeated NOT_READY/BUSY path remains globally paced");

    mind_root_plane_init(&plane, &local, 4u);
    mind_root_plane_observe_target(&plane, &first, 0u);
    mind_root_plane_set_sid8_ready(&plane, 1u, 0u);
    check(mind_root_plane_prepare_submission(&plane, 200u, &submission),
          "retry-boundary fixture submits state");
    mind_root_plane_submission_invoked(&plane, &submission, 200u);
    mind_root_plane_submission_complete(&plane, &submission, 1u, 200u);
    check(!mind_root_plane_prepare_submission(&plane, 3199u, &submission) &&
          mind_root_plane_prepare_submission(&plane, 3200u, &submission),
          "ACK retry starts only after accepted handoff at exact 2999/3000 boundary");

    ack.state = mind_root_plane_local_state(&plane);
    ack.status = MIND_APPLICATION_ROOT_ACK_ACCEPTED;
    mind_root_plane_receive_root_ack(&plane, &wrong, &ack);
    ack.state.generation++;
    mind_root_plane_receive_root_ack(&plane, &first, &ack);
    check(plane.counters.ack_unmatched == 2u,
          "wrong sender and wrong echoed tuple cannot complete campaign");
    ack.state = mind_root_plane_local_state(&plane);
    ack.status = MIND_APPLICATION_ROOT_ACK_DUPLICATE;
    mind_root_plane_receive_root_ack(&plane, &first, &ack);
    check(plane.campaigns[0].completed != 0u,
          "duplicate ACK completes success exactly like accepted");
    mind_root_plane_receive_root_ack(&plane, &first, &ack);
    check(plane.counters.ack_late != 0u, "late duplicate ACK is counted only");

    mind_root_plane_observe_target(&plane, &second, 4000u);
    plane.campaigns[1].due_at_ms = 4000u;
    ack.status = MIND_APPLICATION_ROOT_ACK_CAPACITY_REJECTED;
    mind_root_plane_receive_root_ack(&plane, &second, &ack);
    check(plane.campaigns[1].rejected != 0u,
          "capacity ACK completes terminal rejected outcome");
    plane.campaigns[1].completed = 0u;
    plane.campaigns[1].rejected = 0u;
    ack.status = MIND_APPLICATION_ROOT_ACK_STALE;
    mind_root_plane_receive_root_ack(&plane, &second, &ack);
    check(plane.campaigns[1].rejected != 0u,
          "stale ACK completes terminal rejected outcome");
}

static void test_resolver_fail_closed(void)
{
    mind_root_plane_t plane;
    tavrn_adva_t local = identity(0x80u);
    tavrn_adva_t target = identity(1u);
    mind_root_submission_t submission;

    mind_root_plane_init(&plane, &local, 3u);
    mind_root_plane_observe_target(&plane, &target, 0u);
    mind_root_plane_set_sid8_ready(&plane, 1u, 0u);
    check(mind_root_plane_prepare_submission(&plane, 200u, &submission) &&
          !mind_root_plane_submission_resolved(&plane, &submission,
                                               MIND_ROOT_RESOLVER_COLLIDING, 200u) &&
          plane.campaigns[0].occupied != 0u,
          "COLLIDING retains campaign obligation");
    plane.campaigns[0].resolver_blocked = 0u;
    check(mind_root_plane_prepare_submission(&plane, 200u, &submission) &&
          !mind_root_plane_submission_resolved(&plane, &submission,
                                               MIND_ROOT_RESOLVER_RESERVED, 200u) &&
          plane.campaigns[0].occupied != 0u &&
          plane.campaigns[0].resolver_failed_closed != 0u,
          "RESERVED fails closed without removing campaign obligation");
    plane.campaigns[0].resolver_failed_closed = 0u;
    check(mind_root_plane_prepare_submission(&plane, 200u, &submission) &&
          !mind_root_plane_submission_resolved(&plane, &submission,
                                               MIND_ROOT_RESOLVER_INVALID, 200u) &&
          plane.counters.resolver_invalid != 0u,
          "INVALID resolver is retained fail-closed application evidence");
}

static void test_ack_and_resolver_paths(void)
{
    mind_root_plane_t plane;
    tavrn_adva_t local = identity(0x80u);
    tavrn_adva_t remote = identity(1u);
    mind_application_root_state_t state = root_state(1u, 4u, 1u);
    mind_root_submission_t submission;

    mind_root_plane_init(&plane, &local, 3u);
    mind_root_plane_set_sid8_ready(&plane, 1u, 0u);
    check(mind_root_plane_receive_root_state(&plane, &remote, &state) ==
              MIND_ROOT_APPLY_ACCEPTED,
          "remote root state creates application ACK obligation");
    check(mind_root_plane_prepare_submission(&plane, 1u, &submission) &&
          submission.kind == MIND_ROOT_SUBMISSION_ACK,
          "ACK is selected before root-state campaigns");
    check(!mind_root_plane_submission_resolved(&plane, &submission,
                                               MIND_ROOT_RESOLVER_UNKNOWN, 1u) &&
          plane.counters.resolver_unknown == 1u,
          "unknown resolver retains ACK for a later topology observation");
    {
        routed_cycle_gtt_snapshot_t snapshot;

        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.entry_count = 1u;
        snapshot.entries[0].canonical_adva = remote;
        snapshot.entries[0].departed = ROUTED_CYCLE_BOOLEAN_FALSE;
        check(mind_root_plane_apply_topology(&plane, &snapshot),
              "topology refresh releases unresolved ACK retry");
    }
    check(mind_root_plane_prepare_submission(&plane, 2u, &submission) &&
          mind_root_plane_submission_resolved(&plane, &submission,
                                              MIND_ROOT_RESOLVER_UNIQUE, 2u),
          "unique retry remains an ACK for its original identity");
    mind_root_plane_submission_complete(&plane, &submission, 1u, 2u);
}

int main(void)
{
    test_registry_freshness_and_capacity();
    test_topology_campaign_and_ack();
    test_ack_and_resolver_paths();
    test_nonclearing_wrap_and_campaign_retry();
    test_campaign_pacing_and_ack_outcomes();
    test_resolver_fail_closed();
    if (failures != 0u) {
        printf("mind_root_plane failures=%u\n", failures);
        return 1;
    }
    printf("mind_root_plane tests passed\n");
    return 0;
}
