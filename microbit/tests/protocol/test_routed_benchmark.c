#include "routed_benchmark.h"
#include "routed_benchmark_full.h"
#include "routed_full_telemetry.h"
#include "aodv_core.h"
#include "ble_mesh_scheduler.h"
#include "tavrn_link_v2.h"
#include "tavrn_mentorship.h"
#include "tavrn_router.h"

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

static int observe_with_serial_hop(tavrn_gtt_t *gtt, tavrn_adva_t identity,
                                   uint16_t serial, uint8_t hop_count,
                                   uint32_t now)
{
    tavrn_gtt_evidence_t evidence;

    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = identity;
    evidence.serial = serial;
    evidence.serial_present = 1u;
    evidence.hop_count = hop_count;
    evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    {
        tavrn_gtt_observe_status_t status = tavrn_gtt_observe(gtt, &evidence, now);

        return status == TAVRN_GTT_OBSERVE_ADDED ||
            status == TAVRN_GTT_OBSERVE_REFRESHED;
    }
}

static int observe_with_serial(tavrn_gtt_t *gtt, tavrn_adva_t identity,
                               uint16_t serial, uint32_t now)
{
    return observe_with_serial_hop(gtt, identity, serial, 1u, now);
}

static int observe(tavrn_gtt_t *gtt, tavrn_adva_t identity, uint32_t now)
{
    return observe_with_serial(gtt, identity, 1u, now);
}

static int depart(tavrn_gtt_t *gtt, tavrn_adva_t identity, uint32_t now)
{
    tavrn_gtt_evidence_t evidence;

    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = identity;
    evidence.serial = 2u;
    evidence.serial_present = 1u;
    evidence.hop_count = 1u;
    evidence.kind = TAVRN_GTT_EVIDENCE_DEPARTED;
    return tavrn_gtt_observe(gtt, &evidence, now) == TAVRN_GTT_OBSERVE_DEPARTED;
}

typedef struct benchmark_full_fixture {
    ble_mesh_scheduler_t scheduler;
    tavrn_link_v2_t link;
    aodv_core_t aodv;
    tavrn_gtt_storage_t storage;
    tavrn_gtt_t gtt;
    tavrn_full_t full;
    tavrn_router_t router;
    tavrn_mentorship_t mentorship;
} benchmark_full_fixture_t;

static tavrn_direct_peer_t benchmark_peer(tavrn_adva_t identity,
                                          tavrn_identity_width_t width)
{
    tavrn_direct_peer_t peer;

    memset(&peer, 0, sizeof(peer));
    peer.adva = identity;
    peer.logical_id.width = width;
    peer.logical_id.value = width == TAVRN_IDENTITY_SID8 ? identity.bytes[0] :
        (uint16_t)identity.bytes[0] | ((uint16_t)identity.bytes[1] << 8);
    return peer;
}

static int benchmark_full_fixture_init(benchmark_full_fixture_t *fixture,
                                       tavrn_adva_t local)
{
    tavrn_link_config_t link_config;
    aodv_core_config_t aodv_config;
    tavrn_gtt_config_t gtt_config;
    tavrn_router_incarnation_config_t incarnation;
    tavrn_router_augmentation_hooks_t hooks;
    tavrn_mentorship_config_t mentorship_config;

    if (fixture == NULL) {
        return 0;
    }
    memset(fixture, 0, sizeof(*fixture));
    memset(&link_config, 0, sizeof(link_config));
    link_config.local_peer = benchmark_peer(local, TAVRN_IDENTITY_SID16);
    link_config.network_id = 0x2au;
    link_config.hack_max_attempts = 3u;
    link_config.busy_max_responses = 3u;
    link_config.hack_response_ms = 250u;
    link_config.hack_turnaround_ms = 8u;
    link_config.busy_backoff_ms = 500u;
    link_config.data_forward_deadline_ms = 5000u;
    link_config.candidate_resolve_ms = 10u;
    link_config.data_dedupe_ms = 10000u;
    link_config.flood_dedupe_ms = 10000u;
    link_config.flood_jitter_min_ms = 20u;
    link_config.flood_jitter_max_ms = 120u;
    memset(&aodv_config, 0, sizeof(aodv_config));
    aodv_config.local_peer = link_config.local_peer;
    aodv_config.network_id = link_config.network_id;
    aodv_config.net_diameter = 15u;
    aodv_config.rreq_retries = 2u;
    aodv_config.rreq_rate_per_second = 10u;
    aodv_config.rerr_rate_per_second = 10u;
    aodv_config.request_rrep_ack = 1u;
    aodv_config.initial_origin_sequence = 1u;
    aodv_config.initial_request_id = 1u;
    aodv_config.initial_rerr_sequence = 1u;
    aodv_config.node_traversal_ms = 10u;
    aodv_config.path_discovery_ms = 600u;
    aodv_config.rreq_seen_ms = 10000u;
    aodv_config.active_route_ms = 36000u;
    aodv_config.pending_data_ms = 5000u;
    aodv_config.blacklist_ms = 600u;
    aodv_config.rrep_dedupe_ms = 10000u;
    aodv_config.rerr_dedupe_ms = 10000u;
    aodv_config.rrep_ack_wait_ms = 250u;
    memset(&gtt_config, 0, sizeof(gtt_config));
    gtt_config.local_identity = local;
    gtt_config.soft_expiry_ms = 10u;
    gtt_config.hard_expiry_ms = 20u;
    gtt_config.departed_retention_ms = 40u;
    memset(&incarnation, 0, sizeof(incarnation));
    incarnation.feature_level = TAVRN_ROUTER_FEATURE_FULL_TAVRN;
    incarnation.boot_nonce = 0x7a01u;
    incarnation.reboot_announce_ms = 30u;
    memset(&mentorship_config, 0, sizeof(mentorship_config));
    mentorship_config.offer_window_ms = 10u;
    mentorship_config.page_timeout_ms = 30u;
    mentorship_config.self_bootstrap_ms = 1u;
    mentorship_config.offer_suppression_ms = 100u;
    mentorship_config.join_dedupe_ms = 100u;
    mentorship_config.sync_dedupe_ms = 100u;
    mentorship_config.rssi_weak_magnitude_db = 90u;
    mentorship_config.rssi_strong_magnitude_db = 30u;
    mentorship_config.rssi_weak_delay_ms = 500u;
    mentorship_config.rssi_strong_delay_ms = 10u;
    mentorship_config.jitter_max_ms = 50u;
    mentorship_config.page_attempts = 3u;
    ble_mesh_scheduler_init(&fixture->scheduler, 0u, local.bytes);
    if (tavrn_link_v2_init(&fixture->link, &fixture->scheduler, &link_config, 0u) !=
            TAVRN_LINK_INIT_OK ||
        aodv_core_init(&fixture->aodv, &aodv_config, 0u) != AODV_INIT_OK ||
        tavrn_gtt_init(&fixture->gtt, &fixture->storage, &gtt_config, 0u) !=
            TAVRN_GTT_INIT_OK ||
        tavrn_full_init(&fixture->full, &fixture->gtt) != TAVRN_FULL_INIT_OK) {
        return 0;
    }
    hooks = tavrn_full_router_hooks(&fixture->full);
    if (tavrn_router_init_with_incarnation(&fixture->router, &fixture->link,
                                            &fixture->aodv, &hooks, &incarnation,
                                            0u) != TAVRN_ROUTER_INCARNATION_OK ||
        tavrn_mentorship_init(&fixture->mentorship, &fixture->router, &fixture->gtt,
                              &mentorship_config, 0u) != TAVRN_MENTORSHIP_OK ||
        tavrn_mentorship_tick(&fixture->mentorship, 1u) !=
            TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED) {
        return 0;
    }
    return 1;
}

/* This deliberately uses the production readiness helper and the exact
 * mentorship submission boundary used by main, instead of restating either
 * resolver or scope policy in the test. */
static aodv_status_t benchmark_prepare_and_submit(
    benchmark_full_fixture_t *fixture, const tavrn_adva_t *destination,
    uint32_t now_ms, unsigned int *router_submission_calls)
{
    routed_benchmark_destination_submission_t submission;
    tron_application_data_t data;

    if (fixture == NULL || destination == NULL || router_submission_calls == NULL ||
        routed_benchmark_destination_prepare(&fixture->full, destination, now_ms,
                                             &submission) !=
            ROUTED_BENCHMARK_DESTINATION_READY) {
        return AODV_STATUS_NOT_FOUND;
    }
    memset(&data, 0, sizeof(data));
    data.final_destination = submission.logical_destination;
    data.app_kind = 0x7fu;
    data.app_len = 1u;
    data.app_bytes[0] = 0xa5u;
    (*router_submission_calls)++;
    return tavrn_mentorship_submit_application(&fixture->mentorship, &data, now_ms);
}

static int submission_action_matches(benchmark_full_fixture_t *fixture,
                                     uint8_t scope,
                                     aodv_rreq_scope_source_t source)
{
    aodv_action_t action;

    memset(&action, 0, sizeof(action));
    return fixture != NULL &&
        aodv_core_poll_action(&fixture->aodv, &action) == AODV_ACTION_POLL_OK &&
        action.type == AODV_ACTION_SEND_RREQ &&
        (action.detail.control.control.pdu[6] >> 4) == scope &&
        action.detail.control.rreq_attempt.initial_scope == scope &&
        ((source == AODV_RREQ_SCOPE_ROUTER_HINT &&
          action.detail.control.rreq_attempt.initial_scope <
              fixture->aodv.config.net_diameter) ||
         (source == AODV_RREQ_SCOPE_ORDINARY &&
          action.detail.control.rreq_attempt.initial_scope == 1u)) &&
        aodv_core_poll_action(&fixture->aodv, &action) == AODV_ACTION_POLL_EMPTY;
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
                                slot.sequence == 0u && slot.identity == 0x00800000u &&
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
    CHECK("BENCH-SCHEDULE", routed_benchmark_init(&state, 0u, 74u) &&
                                routed_benchmark_schedule_due(&state, 3600000u, &slot) ==
                                    ROUTED_BENCHMARK_SCHEDULE_DUE &&
                                slot.workload == ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT &&
                                slot.burst == 3u && slot.sequence == 528u &&
                                slot.identity == 3600u);
    CHECK("BENCH-SCHEDULE", routed_benchmark_identity_encode(
                                ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT, 0x1fffu,
                                0x03ffu) == 0x007fffffu &&
                                routed_benchmark_identity_encode(
                                    ROUTED_BENCHMARK_WORKLOAD_THROUGHPUT, 0x1fffu,
                                    0x03ffu) == ROUTED_BENCHMARK_IDENTITY_MAX);
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
        0x44u, 0x33u, 0x22u, 0x11u, 0x78u, 0x56u, 0xb4u,
    };
    uint32_t origin_session = UINT32_MAX;
    uint32_t identity = UINT32_MAX;
    uint16_t index;
    uint16_t expected_sequence;

    routed_benchmark_attempt_queue_init(&queue);
    memset(&attempt, 0, sizeof(attempt));
    CHECK("BENCH-QUEUE", ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY == 1024u &&
                             sizeof(queue.head) == sizeof(uint16_t) &&
                             sizeof(queue.tail) == sizeof(uint16_t) &&
                             sizeof(queue.count) == sizeof(uint16_t));
    for (index = 0u; index < ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY; index++) {
        attempt.sequence = index;
        attempt.attempted = (uint8_t)(index & 1u);
        CHECK("BENCH-QUEUE", routed_benchmark_attempt_queue_offer(&queue, &attempt) ==
                                 ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK);
    }
    attempt.sequence = ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY;
    CHECK("BENCH-QUEUE", queue.head == 0u && queue.tail == 0u &&
                              routed_benchmark_attempt_queue_offer(&queue, &attempt) ==
                              ROUTED_BENCHMARK_ATTEMPT_QUEUE_DROPPED &&
                              routed_benchmark_attempt_queue_offer(&queue, &attempt) ==
                              ROUTED_BENCHMARK_ATTEMPT_QUEUE_DROPPED &&
                              routed_benchmark_attempt_queue_snapshot(&queue, &snapshot) ==
                                  ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK &&
                              snapshot.count == ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY &&
                              snapshot.high_water == ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY &&
                              snapshot.dropped_count == 2u);
    for (index = 0u; index < ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY / 2u;
         index++) {
        CHECK("BENCH-QUEUE", routed_benchmark_attempt_queue_take(&queue, &observed) ==
                                 ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK &&
                                 observed.sequence == index &&
                                 observed.attempted == (uint8_t)(index & 1u));
    }
    CHECK("BENCH-QUEUE", queue.head == ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY / 2u &&
                              queue.tail == 0u &&
                              queue.count == ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY / 2u);
    for (index = 0u; index < ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY / 2u;
         index++) {
        attempt.sequence = (uint16_t)(ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY + index);
        attempt.attempted = (uint8_t)(attempt.sequence & 1u);
        CHECK("BENCH-QUEUE", routed_benchmark_attempt_queue_offer(&queue, &attempt) ==
                                 ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK);
    }
    CHECK("BENCH-QUEUE", queue.head == ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY / 2u &&
                              queue.tail == ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY / 2u &&
                              queue.count == ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY &&
                              routed_benchmark_attempt_queue_snapshot(&queue, &snapshot) ==
                                  ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK &&
                              snapshot.high_water == ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY &&
                              snapshot.dropped_count == 2u);
    for (expected_sequence = ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY / 2u;
         expected_sequence < ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY +
             ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY / 2u;
         expected_sequence++) {
        CHECK("BENCH-QUEUE", routed_benchmark_attempt_queue_take(&queue, &observed) ==
                                 ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK &&
                                 observed.sequence == expected_sequence &&
                                 observed.attempted ==
                                     (uint8_t)(expected_sequence & 1u));
    }
    CHECK("BENCH-QUEUE", queue.head == ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY / 2u &&
                              queue.tail == ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY / 2u &&
                              queue.count == 0u &&
                              routed_benchmark_attempt_queue_take(&queue, &observed) ==
                                  ROUTED_BENCHMARK_ATTEMPT_QUEUE_EMPTY);
    queue.count = ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY + 1u;
    CHECK("BENCH-QUEUE", routed_benchmark_attempt_queue_take(&queue, &observed) ==
                             ROUTED_BENCHMARK_ATTEMPT_QUEUE_INVALID);

    CHECK("BENCH-DECODE", routed_benchmark_decode_payload(
                               ROUTED_BENCHMARK_APP_KIND,
                                ROUTED_BENCHMARK_APP_PAYLOAD_BYTES, bytes,
                                &origin_session, &identity) &&
                                origin_session == 0x11223344u &&
                                identity == 0x00b45678u);
    identity = UINT32_MAX;
    origin_session = UINT32_MAX;
    CHECK("BENCH-DECODE", !routed_benchmark_decode_payload(
                                ROUTED_BENCHMARK_APP_KIND, 6u, bytes,
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
    CHECK("BENCH-DECODE", !routed_benchmark_identity_decode(
                               0x01000000u, &(routed_benchmark_workload_t){0},
                               &(uint32_t){0}, &(uint16_t){0}));
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
    tavrn_adva_t missing = adva(0x43u, 3u);
    tavrn_adva_t reserved = adva(0xffu, 4u);

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
                              resolved.value == 0x42u &&
                              routed_benchmark_destination_ready(&full, &destination, 11u,
                                                                 &resolved) ==
                                  ROUTED_BENCHMARK_DESTINATION_READY &&
                              routed_benchmark_destination_ready(&full, &destination, 21u,
                                                                 &resolved) ==
                                  ROUTED_BENCHMARK_DESTINATION_READY &&
                              routed_benchmark_destination_ready(&full, &missing, 21u,
                                                                 &resolved) ==
                                  ROUTED_BENCHMARK_DESTINATION_UNKNOWN &&
                              routed_benchmark_destination_ready(&full, &reserved, 21u,
                                                                 &resolved) ==
                                  ROUTED_BENCHMARK_DESTINATION_RESERVED &&
                              depart(&gtt, destination, 22u) &&
                              routed_benchmark_destination_ready(&full, &destination, 22u,
                                                                 &resolved) ==
                                  ROUTED_BENCHMARK_DESTINATION_UNKNOWN &&
                              observe_with_serial(&gtt, destination, 3u, 23u) &&
                              observe(&gtt, collision, 24u) &&
                               routed_benchmark_destination_ready(&full, &destination, 24u,
                                                                  &resolved) ==
                                   ROUTED_BENCHMARK_DESTINATION_COLLIDING);
}

static void test_full_destination_submission_composition(void)
{
    benchmark_full_fixture_t fixture;
    tavrn_adva_t local = adva(0x11u, 1u);
    tavrn_adva_t destination = adva(0x42u, 2u);
    tavrn_adva_t collision = adva(0x42u, 9u);
    tavrn_adva_t missing = adva(0x43u, 3u);
    tavrn_adva_t reserved = adva(0xffu, 4u);
    routed_benchmark_destination_submission_t submission;
    unsigned int calls = 0u;

    CHECK("BENCH-FULL-SUBMIT", benchmark_full_fixture_init(&fixture, local) &&
                                  observe_with_serial_hop(&fixture.gtt, destination,
                                                          1u, 3u, 1u) &&
                                  routed_benchmark_destination_prepare(
                                      &fixture.full, &destination, 2u, &submission) ==
                                      ROUTED_BENCHMARK_DESTINATION_READY &&
                                  submission.freshness == TAVRN_GTT_FRESHNESS_ACTIVE &&
                                  submission.hop_count == 3u &&
                                  benchmark_prepare_and_submit(&fixture, &destination, 2u,
                                                               &calls) == AODV_STATUS_QUEUED &&
                                  calls == 1u &&
                                  submission_action_matches(&fixture, 5u,
                                                            AODV_RREQ_SCOPE_ROUTER_HINT));

    CHECK("BENCH-FULL-SUBMIT", benchmark_full_fixture_init(&fixture, local) &&
                                  observe_with_serial_hop(&fixture.gtt, destination,
                                                          1u, 3u, 1u) &&
                                  routed_benchmark_destination_prepare(
                                      &fixture.full, &destination, 11u, &submission) ==
                                      ROUTED_BENCHMARK_DESTINATION_READY &&
                                  submission.freshness == TAVRN_GTT_FRESHNESS_SOFT_STALE &&
                                  benchmark_prepare_and_submit(&fixture, &destination, 11u,
                                                               &calls) == AODV_STATUS_QUEUED &&
                                  calls == 2u &&
                                  submission_action_matches(&fixture, 5u,
                                                            AODV_RREQ_SCOPE_ROUTER_HINT));

    CHECK("BENCH-FULL-SUBMIT", benchmark_full_fixture_init(&fixture, local) &&
                                  observe_with_serial_hop(&fixture.gtt, destination,
                                                          1u, 3u, 1u) &&
                                  routed_benchmark_destination_prepare(
                                      &fixture.full, &destination, 21u, &submission) ==
                                      ROUTED_BENCHMARK_DESTINATION_READY &&
                                  submission.freshness == TAVRN_GTT_FRESHNESS_HARD_EXPIRED &&
                                  benchmark_prepare_and_submit(&fixture, &destination, 21u,
                                                               &calls) == AODV_STATUS_QUEUED &&
                                  calls == 3u &&
                                  submission_action_matches(&fixture, 1u,
                                                            AODV_RREQ_SCOPE_ORDINARY));

    CHECK("BENCH-FULL-SUBMIT", benchmark_full_fixture_init(&fixture, local) &&
                                  observe_with_serial_hop(&fixture.gtt, destination,
                                                          1u, 0u, 1u) &&
                                  benchmark_prepare_and_submit(&fixture, &destination, 2u,
                                                               &calls) == AODV_STATUS_QUEUED &&
                                  calls == 4u &&
                                  submission_action_matches(&fixture, 1u,
                                                            AODV_RREQ_SCOPE_ORDINARY));

    CHECK("BENCH-FULL-SUBMIT", benchmark_full_fixture_init(&fixture, local) &&
                                  observe(&fixture.gtt, destination, 1u) &&
                                  depart(&fixture.gtt, destination, 2u) &&
                                  benchmark_prepare_and_submit(&fixture, &destination, 2u,
                                                               &calls) == AODV_STATUS_NOT_FOUND &&
                                  calls == 4u &&
                                  aodv_core_poll_action(&fixture.aodv,
                                                        &(aodv_action_t){ 0 }) ==
                                      AODV_ACTION_POLL_EMPTY);

    CHECK("BENCH-FULL-SUBMIT", benchmark_full_fixture_init(&fixture, local) &&
                                  observe(&fixture.gtt, destination, 1u) &&
                                  observe(&fixture.gtt, collision, 2u) &&
                                  benchmark_prepare_and_submit(&fixture, &destination, 2u,
                                                               &calls) == AODV_STATUS_NOT_FOUND &&
                                  calls == 4u &&
                                  aodv_core_poll_action(&fixture.aodv,
                                                        &(aodv_action_t){ 0 }) ==
                                      AODV_ACTION_POLL_EMPTY);

    CHECK("BENCH-FULL-SUBMIT", benchmark_full_fixture_init(&fixture, local) &&
                                  benchmark_prepare_and_submit(&fixture, &missing, 2u,
                                                               &calls) == AODV_STATUS_NOT_FOUND &&
                                  benchmark_prepare_and_submit(&fixture, &reserved, 2u,
                                                               &calls) == AODV_STATUS_NOT_FOUND &&
                                  calls == 4u &&
                                  aodv_core_poll_action(&fixture.aodv,
                                                        &(aodv_action_t){ 0 }) ==
                                      AODV_ACTION_POLL_EMPTY);
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
    test_full_destination_submission_composition();
    test_gtt_snapshot_is_read_only();
    if (failures != 0u) {
        printf("routed benchmark tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("routed benchmark tests passed\n");
    return 0;
}
