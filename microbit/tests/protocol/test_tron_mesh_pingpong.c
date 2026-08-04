#include "tron_mesh_pingpong.h"

#include <stdint.h>
#include <stdio.h>

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            printf("FAIL %s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            return 1; \
        } \
    } while (0)

#define ASSERT_EQ_U32(expected, actual) \
    do { \
        uint32_t expected__ = (uint32_t)(expected); \
        uint32_t actual__ = (uint32_t)(actual); \
        if (expected__ != actual__) { \
            printf("FAIL %s:%d: expected %lu, got %lu\n", \
                   __FILE__, __LINE__, (unsigned long)expected__, \
                   (unsigned long)actual__); \
            return 1; \
        } \
    } while (0)

#define ASSERT_EQ_PP(expected, actual) \
    do { \
        tron_mesh_pingpong_result_t expected__ = (expected); \
        tron_mesh_pingpong_result_t actual__ = (actual); \
        if (expected__ != actual__) { \
            printf("FAIL %s:%d: expected %s, got %s\n", \
                   __FILE__, __LINE__, \
                   tron_mesh_pingpong_result_name(expected__), \
                   tron_mesh_pingpong_result_name(actual__)); \
            return 1; \
        } \
    } while (0)

typedef int (*test_func_t)(void);

typedef struct test_case {
    const char *name;
    test_func_t func;
} test_case_t;

static tron_mesh_packet_t make_ping(uint32_t seq24)
{
    tron_mesh_packet_t ping;

    (void)tron_mesh_pingpong_build_ping(&ping, TRON_MESH_TTL_MAX, seq24);
    return ping;
}

static tron_mesh_packet_t make_pong(uint32_t seq24)
{
    tron_mesh_packet_t ping = make_ping(seq24);
    tron_mesh_packet_t pong;

    (void)tron_mesh_pingpong_build_pong(&pong, &ping, TRON_MESH_TTL_MAX);
    return pong;
}

static int start_pending(tron_mesh_pingpong_root_t *state,
                         uint32_t init_ms,
                         uint32_t seq24,
                         uint32_t *queued_at_ms)
{
    uint32_t due;

    tron_mesh_pingpong_root_init(state, init_ms);
    due = init_ms + TRON_MESH_PINGPONG_INTERVAL_MS;
    ASSERT_TRUE(tron_mesh_pingpong_root_ping_due(state, due));
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_ATTEMPT_QUEUED,
                  tron_mesh_pingpong_root_record_ping_attempt(state, seq24,
                                                               due, 1));
    if (queued_at_ms != NULL) {
        *queued_at_ms = due;
    }
    return 0;
}

static int test_ping_builder_and_wire_shape(void)
{
    tron_mesh_packet_t ping;
    tron_mesh_packet_t decoded;
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len = 0u;

    ASSERT_EQ_PP(TRON_MESH_PINGPONG_OK,
                 tron_mesh_pingpong_build_ping(&ping, TRON_MESH_TTL_MAX,
                                                0x12ABCDEFu));
    ASSERT_EQ_U32(TRON_MESH_MSG_TYPE_PING, ping.msg_type);
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_NET_ID, ping.net_id);
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_ROOT_ID, ping.src);
    ASSERT_EQ_U32(0x00ABCDEFu, ping.seq24);
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_PAYLOAD_LEN, ping.payload_len);
    ASSERT_EQ_U32(0x02u, ping.payload[0]);
    ASSERT_EQ_U32(0x00u, ping.payload[1]);
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_OK, tron_mesh_pingpong_validate(&ping));

    ASSERT_EQ_U32(TRON_MESH_PINGPONG_ADV_LEN,
                  tron_mesh_packet_encoded_len(ping.payload_len));
    ASSERT_EQ_U32(TRON_MESH_PACKET_OK,
                  tron_mesh_packet_encode(&ping, adv, sizeof(adv), &adv_len));
    ASSERT_EQ_U32(21u, adv_len);
    ASSERT_EQ_U32(TRON_MESH_MSG_TYPE_PING, adv[10]);
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_PAYLOAD_LEN, adv[18]);
    ASSERT_EQ_U32(0x02u, adv[19]);
    ASSERT_EQ_U32(0x00u, adv[20]);
    ASSERT_EQ_U32(TRON_MESH_PACKET_OK,
                  tron_mesh_packet_decode(adv, adv_len, &decoded));
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_OK,
                 tron_mesh_pingpong_validate(&decoded));
    return 0;
}

static int test_pong_builder_echoes_sequence_and_reverses_endpoints(void)
{
    tron_mesh_packet_t ping = make_ping(0x654321u);
    tron_mesh_packet_t pong;

    ASSERT_EQ_PP(TRON_MESH_PINGPONG_OK,
                 tron_mesh_pingpong_build_pong(&pong, &ping, 1u));
    ASSERT_EQ_U32(TRON_MESH_MSG_TYPE_PONG, pong.msg_type);
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_LEAF_ID, pong.src);
    ASSERT_EQ_U32(ping.seq24, pong.seq24);
    ASSERT_EQ_U32(0x01u, pong.payload[0]);
    ASSERT_EQ_U32(0x00u, pong.payload[1]);
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_OK, tron_mesh_pingpong_validate(&pong));

    ping.src = 0x1234u;
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_ERR_SOURCE,
                 tron_mesh_pingpong_build_pong(&pong, &ping, 1u));
    return 0;
}

static int test_pong_builder_supports_in_place_conversion(void)
{
    tron_mesh_packet_t packet = make_ping(0x654321u);

    ASSERT_EQ_PP(TRON_MESH_PINGPONG_OK,
                 tron_mesh_pingpong_build_pong(&packet, &packet, 1u));
    ASSERT_EQ_U32(TRON_MESH_MSG_TYPE_PONG, packet.msg_type);
    ASSERT_EQ_U32(0x654321u, packet.seq24);
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_OK,
                 tron_mesh_pingpong_validate(&packet));
    return 0;
}

static int test_endpoint_and_semantic_validation(void)
{
    tron_mesh_packet_t packet = make_ping(7u);

    packet.net_id = 0x02u;
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_ERR_NETWORK,
                 tron_mesh_pingpong_validate(&packet));
    packet = make_ping(7u);
    packet.src = TRON_MESH_PINGPONG_LEAF_ID;
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_ERR_SOURCE,
                 tron_mesh_pingpong_validate(&packet));
    packet = make_ping(7u);
    packet.payload[0] = 0x01u;
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_ERR_DESTINATION,
                 tron_mesh_pingpong_validate(&packet));
    packet = make_ping(7u);
    packet.payload_len = 1u;
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_ERR_PAYLOAD_LEN,
                 tron_mesh_pingpong_validate(&packet));
    packet = make_ping(7u);
    packet.msg_type = TRON_MESH_MSG_TYPE_DUMMY_STATUS;
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_ERR_TYPE,
                 tron_mesh_pingpong_validate(&packet));
    packet = make_ping(7u);
    packet.ttl = (uint8_t)(TRON_MESH_TTL_MAX + 1u);
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_ERR_TTL,
                 tron_mesh_pingpong_validate(&packet));
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_ERR_NULL,
                 tron_mesh_pingpong_validate(NULL));
    return 0;
}

static int test_ttl_zero_is_valid_for_local_delivery(void)
{
    tron_mesh_packet_t ping;
    tron_mesh_packet_t pong;

    ASSERT_EQ_PP(TRON_MESH_PINGPONG_OK,
                 tron_mesh_pingpong_build_ping(&ping, 0u, 11u));
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_OK, tron_mesh_pingpong_validate(&ping));
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_OK,
                 tron_mesh_pingpong_build_pong(&pong, &ping, 0u));
    ASSERT_EQ_PP(TRON_MESH_PINGPONG_OK, tron_mesh_pingpong_validate(&pong));
    return 0;
}

static int test_timing_constants_are_wrap_safe(void)
{
    ASSERT_EQ_U32(2000u, TRON_MESH_PINGPONG_INTERVAL_MS);
    ASSERT_EQ_U32(1500u, TRON_MESH_PINGPONG_TIMEOUT_MS);
    ASSERT_TRUE(TRON_MESH_PINGPONG_TIMEOUT_MS <
                TRON_MESH_PINGPONG_INTERVAL_MS);
    ASSERT_TRUE(TRON_MESH_PINGPONG_INTERVAL_MS < 0x80000000UL);
    ASSERT_TRUE(TRON_MESH_PINGPONG_TIMEOUT_MS < 0x80000000UL);
    return 0;
}

static int test_pending_starts_only_after_successful_queue(void)
{
    tron_mesh_pingpong_root_t state;

    tron_mesh_pingpong_root_init(&state, 100u);
    ASSERT_TRUE(!tron_mesh_pingpong_root_ping_due(&state, 2099u));
    ASSERT_TRUE(tron_mesh_pingpong_root_ping_due(&state, 2100u));
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_ATTEMPT_QUEUE_FAILED,
                  tron_mesh_pingpong_root_record_ping_attempt(&state, 10u,
                                                               2100u, 0));
    ASSERT_EQ_U32(0u, state.pending);
    ASSERT_EQ_U32(4100u, state.next_ping_at_ms);

    ASSERT_TRUE(tron_mesh_pingpong_root_ping_due(&state, 4100u));
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_ATTEMPT_QUEUED,
                  tron_mesh_pingpong_root_record_ping_attempt(&state, 11u,
                                                               4100u, 1));
    ASSERT_EQ_U32(1u, state.pending);
    ASSERT_EQ_U32(11u, state.pending_seq24);
    ASSERT_TRUE(!tron_mesh_pingpong_root_ping_due(&state, 6100u));
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_ATTEMPT_NOT_DUE,
                  tron_mesh_pingpong_root_record_ping_attempt(&state, 12u,
                                                               6100u, 1));
    ASSERT_EQ_U32(11u, state.pending_seq24);
    return 0;
}

static int test_matching_pong_reports_enqueue_latency(void)
{
    tron_mesh_pingpong_root_t state;
    tron_mesh_packet_t pong = make_pong(42u);
    uint32_t queued_at;
    uint32_t rtt = 0u;

    ASSERT_TRUE(start_pending(&state, 1000u, 42u, &queued_at) == 0);
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_PONG_MATCHED,
                  tron_mesh_pingpong_root_match_pong(&state, &pong,
                                                      queued_at + 321u, &rtt));
    ASSERT_EQ_U32(321u, rtt);
    ASSERT_EQ_U32(0u, state.pending);
    return 0;
}

static int test_wrong_source_destination_and_correlation_are_unmatched(void)
{
    tron_mesh_pingpong_root_t state;
    tron_mesh_packet_t pong = make_pong(42u);
    uint32_t queued_at;

    ASSERT_TRUE(start_pending(&state, 0u, 42u, &queued_at) == 0);
    pong.src = TRON_MESH_PINGPONG_ROOT_ID;
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_PONG_UNMATCHED,
                  tron_mesh_pingpong_root_match_pong(&state, &pong,
                                                      queued_at + 10u, NULL));
    ASSERT_EQ_U32(1u, state.pending);

    pong = make_pong(42u);
    pong.payload[0] = 0x02u;
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_PONG_UNMATCHED,
                  tron_mesh_pingpong_root_match_pong(&state, &pong,
                                                      queued_at + 20u, NULL));
    ASSERT_EQ_U32(1u, state.pending);

    pong = make_pong(43u);
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_PONG_UNMATCHED,
                  tron_mesh_pingpong_root_match_pong(&state, &pong,
                                                      queued_at + 30u, NULL));
    ASSERT_EQ_U32(1u, state.pending);
    return 0;
}

static int test_late_pong_and_timeout(void)
{
    tron_mesh_pingpong_root_t state;
    tron_mesh_packet_t pong = make_pong(9u);
    uint32_t queued_at;

    ASSERT_TRUE(start_pending(&state, 0u, 9u, &queued_at) == 0);
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_PONG_LATE,
                  tron_mesh_pingpong_root_match_pong(
                      &state, &pong,
                      queued_at + TRON_MESH_PINGPONG_TIMEOUT_MS, NULL));
    ASSERT_EQ_U32(0u, state.pending);

    ASSERT_TRUE(start_pending(&state, 10000u, 9u, &queued_at) == 0);
    ASSERT_TRUE(!tron_mesh_pingpong_root_check_timeout(
        &state, queued_at + TRON_MESH_PINGPONG_TIMEOUT_MS - 1u));
    ASSERT_TRUE(tron_mesh_pingpong_root_check_timeout(
        &state, queued_at + TRON_MESH_PINGPONG_TIMEOUT_MS));
    ASSERT_EQ_U32(0u, state.pending);
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_PONG_UNMATCHED,
                  tron_mesh_pingpong_root_match_pong(
                      &state, &pong,
                      queued_at + TRON_MESH_PINGPONG_TIMEOUT_MS + 1u, NULL));
    return 0;
}

static int test_deadlines_and_rtt_wrap_at_32_bits(void)
{
    tron_mesh_pingpong_root_t state;
    tron_mesh_packet_t pong = make_pong(77u);
    uint32_t init = UINT32_MAX - 2500u;
    uint32_t queued_at = init + TRON_MESH_PINGPONG_INTERVAL_MS;
    uint32_t rtt = 0u;

    tron_mesh_pingpong_root_init(&state, init);
    ASSERT_TRUE(!tron_mesh_pingpong_root_ping_due(&state, queued_at - 1u));
    ASSERT_TRUE(tron_mesh_pingpong_root_ping_due(&state, queued_at));
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_ATTEMPT_QUEUED,
                  tron_mesh_pingpong_root_record_ping_attempt(&state, 77u,
                                                               queued_at, 1));
    ASSERT_TRUE(!tron_mesh_pingpong_root_check_timeout(
        &state, queued_at + TRON_MESH_PINGPONG_TIMEOUT_MS - 1u));
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_PONG_MATCHED,
                  tron_mesh_pingpong_root_match_pong(&state, &pong,
                                                      queued_at + 1000u, &rtt));
    ASSERT_EQ_U32(1000u, rtt);

    tron_mesh_pingpong_root_init(&state, init);
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_ATTEMPT_QUEUED,
                  tron_mesh_pingpong_root_record_ping_attempt(&state, 77u,
                                                               queued_at, 1));
    ASSERT_TRUE(tron_mesh_pingpong_root_check_timeout(
        &state, queued_at + TRON_MESH_PINGPONG_TIMEOUT_MS));
    return 0;
}

static int test_failed_late_attempt_advances_periodic_deadline(void)
{
    tron_mesh_pingpong_root_t state;

    tron_mesh_pingpong_root_init(&state, 0u);
    ASSERT_EQ_U32(TRON_MESH_PINGPONG_ATTEMPT_QUEUE_FAILED,
                  tron_mesh_pingpong_root_record_ping_attempt(&state, 5u,
                                                               6500u, 0));
    ASSERT_EQ_U32(8000u, state.next_ping_at_ms);
    ASSERT_EQ_U32(0u, state.pending);
    ASSERT_TRUE(!tron_mesh_pingpong_root_ping_due(&state, 7999u));
    ASSERT_TRUE(tron_mesh_pingpong_root_ping_due(&state, 8000u));
    return 0;
}

static int run_test(const test_case_t *test)
{
    if (test->func() != 0) {
        printf("FAILED: %s\n", test->name);
        return 1;
    }
    printf("ok - %s\n", test->name);
    return 0;
}

int main(void)
{
    static const test_case_t tests[] = {
        { "PING builder and exact wire shape", test_ping_builder_and_wire_shape },
        { "PONG builder echoes sequence and reverses endpoints", test_pong_builder_echoes_sequence_and_reverses_endpoints },
        { "PONG builder supports in-place conversion", test_pong_builder_supports_in_place_conversion },
        { "endpoint and semantic validation", test_endpoint_and_semantic_validation },
        { "TTL zero remains valid for local delivery", test_ttl_zero_is_valid_for_local_delivery },
        { "timing constants are wrap-safe", test_timing_constants_are_wrap_safe },
        { "pending starts only after successful queue", test_pending_starts_only_after_successful_queue },
        { "matching PONG reports enqueue latency", test_matching_pong_reports_enqueue_latency },
        { "wrong source, destination, and correlation are unmatched", test_wrong_source_destination_and_correlation_are_unmatched },
        { "late PONG and timeout", test_late_pong_and_timeout },
        { "32-bit deadline and RTT wrap", test_deadlines_and_rtt_wrap_at_32_bits },
        { "failed late attempt advances periodic deadline", test_failed_late_attempt_advances_periodic_deadline },
    };
    size_t i;

    for (i = 0u; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (run_test(&tests[i]) != 0) {
            return 1;
        }
    }

    printf("All TRON mesh PING/PONG tests passed (%lu tests).\n",
           (unsigned long)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
