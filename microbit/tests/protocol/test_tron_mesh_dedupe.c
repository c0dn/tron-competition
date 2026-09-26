#include "tron_mesh_dedupe.h"
#include "tron_mesh_pingpong.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ASSERT_TRUE(expr) \
    do { \
        if (!(expr)) { \
            printf("FAIL %s:%d: assertion failed: %s\n", __FILE__, __LINE__, #expr); \
            return 1; \
        } \
    } while (0)

#define ASSERT_EQ_U32(expected, actual) \
    do { \
        uint32_t expected_value__ = (uint32_t)(expected); \
        uint32_t actual_value__ = (uint32_t)(actual); \
        if (expected_value__ != actual_value__) { \
            printf("FAIL %s:%d: expected %lu, got %lu\n", \
                   __FILE__, \
                   __LINE__, \
                   (unsigned long)expected_value__, \
                   (unsigned long)actual_value__); \
            return 1; \
        } \
    } while (0)

typedef int (*test_func_t)(void);

typedef struct test_case {
    const char *name;
    test_func_t func;
} test_case_t;

static tron_mesh_dedupe_t cache;

static tron_mesh_packet_t make_packet(uint16_t src, uint32_t seq24)
{
    tron_mesh_packet_t packet;

    memset(&packet, 0, sizeof(packet));
    packet.net_id = 0x01u;
    packet.msg_type = TRON_MESH_MSG_TYPE_DUMMY_STATUS;
    packet.ttl = TRON_MESH_TTL_MAX;
    packet.src = src;
    packet.seq24 = seq24;
    return packet;
}

static int test_first_sighting_is_new(void)
{
    tron_mesh_packet_t p = make_packet(0xf602u, 33u);

    tron_mesh_dedupe_reset(&cache);
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &p, 1000u));
    ASSERT_EQ_U32(1u, tron_mesh_dedupe_slots_used(&cache));
    return 0;
}

static int test_immediate_repeat_is_duplicate(void)
{
    tron_mesh_packet_t p = make_packet(0xf602u, 33u);

    tron_mesh_dedupe_reset(&cache);
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &p, 1000u));
    ASSERT_EQ_U32(1u, tron_mesh_dedupe_seen_or_insert(&cache, &p, 1050u));
    return 0;
}

/* Regression: two interleaved sources must not evict each other. The original
   implementation folded "first free slot" and "oldest live entry" into one
   index, so only slot 0 was ever reachable and any second key displaced the
   first. */
static int test_second_key_does_not_evict_first(void)
{
    tron_mesh_packet_t a = make_packet(0xf602u, 33u);
    tron_mesh_packet_t b = make_packet(0xb653u, 22u);

    tron_mesh_dedupe_reset(&cache);
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &a, 1000u));
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &b, 1100u));
    ASSERT_EQ_U32(2u, tron_mesh_dedupe_slots_used(&cache));
    ASSERT_EQ_U32(1u, tron_mesh_dedupe_seen_or_insert(&cache, &a, 1200u));
    ASSERT_EQ_U32(1u, tron_mesh_dedupe_seen_or_insert(&cache, &b, 1300u));
    return 0;
}

/* Regression: the cache must actually use its configured capacity. */
static int test_cache_uses_full_capacity(void)
{
    unsigned int k;

    tron_mesh_dedupe_reset(&cache);
    for (k = 0u; k < TRON_MESH_DEDUPE_SIZE; k++) {
        tron_mesh_packet_t p = make_packet((uint16_t)(0x100u + k), k);
        ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &p, 1000u + k));
    }
    ASSERT_EQ_U32(TRON_MESH_DEDUPE_SIZE, tron_mesh_dedupe_slots_used(&cache));

    /* Every one of them must still be recognised. */
    for (k = 0u; k < TRON_MESH_DEDUPE_SIZE; k++) {
        tron_mesh_packet_t p = make_packet((uint16_t)(0x100u + k), k);
        ASSERT_EQ_U32(1u, tron_mesh_dedupe_seen_or_insert(&cache, &p, 2000u + k));
    }
    return 0;
}

/* Regression: replays the exact on-air interleave captured from three boards,
   where two sources alternate while their relayed copies come back. Every copy
   after the first sighting of each key must be suppressed. */
static int test_observed_hardware_interleave(void)
{
    struct { uint16_t src; uint32_t seq; uint32_t t; int expect_dup; } trace[] = {
        { 0xf602u, 33u, 14011u, 0 },   /* ttl=3 original */
        { 0xb653u, 22u, 14094u, 0 },   /* ttl=3 original */
        { 0xf602u, 33u, 14111u, 1 },   /* ttl=2 relayed copy */
        { 0xb653u, 22u, 14204u, 1 },   /* ttl=2 relayed copy */
        { 0xf602u, 33u, 14314u, 1 },   /* ttl=1 relayed copy */
        { 0xb653u, 22u, 14416u, 1 },   /* ttl=1 relayed copy */
        { 0xb653u, 22u, 14818u, 1 },   /* ttl=0 terminal copy */
    };
    unsigned int i;

    tron_mesh_dedupe_reset(&cache);
    for (i = 0u; i < sizeof(trace) / sizeof(trace[0]); i++) {
        tron_mesh_packet_t p = make_packet(trace[i].src, trace[i].seq);
        int seen = tron_mesh_dedupe_seen_or_insert(&cache, &p, trace[i].t);

        if (seen != trace[i].expect_dup) {
            printf("FAIL %s:%d: step %u src=0x%04x seq=%lu expected %s, got %s\n",
                   __FILE__, __LINE__, i, trace[i].src,
                   (unsigned long)trace[i].seq,
                   trace[i].expect_dup ? "duplicate" : "new",
                   seen ? "duplicate" : "new");
            return 1;
        }
    }
    return 0;
}

static int test_entry_expires_after_ttl(void)
{
    tron_mesh_packet_t p = make_packet(0xf602u, 33u);

    tron_mesh_dedupe_reset(&cache);
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &p, 1000u));
    /* Just inside the window: still a duplicate. */
    ASSERT_EQ_U32(1u, tron_mesh_dedupe_seen_or_insert(&cache, &p,
                                                       1000u + tron_timer_config.legacy_dedupe_ms - 1u));
    /* At and past the window: treated as new again. */
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &p,
                                                       1000u + tron_timer_config.legacy_dedupe_ms));
    return 0;
}

static int test_overflow_evicts_oldest_not_newest(void)
{
    unsigned int k;
    tron_mesh_packet_t newest;
    tron_mesh_packet_t oldest = make_packet(0x100u, 0u);

    tron_mesh_dedupe_reset(&cache);
    for (k = 0u; k < TRON_MESH_DEDUPE_SIZE; k++) {
        tron_mesh_packet_t p = make_packet((uint16_t)(0x100u + k), k);
        (void)tron_mesh_dedupe_seen_or_insert(&cache, &p, 1000u + (k * 10u));
    }

    /* One more key with the cache full must displace the oldest entry. */
    newest = make_packet(0x200u, 99u);
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &newest, 2000u));
    ASSERT_EQ_U32(TRON_MESH_DEDUPE_SIZE, tron_mesh_dedupe_slots_used(&cache));
    /* The newest key is retained. */
    ASSERT_EQ_U32(1u, tron_mesh_dedupe_seen_or_insert(&cache, &newest, 2010u));
    /* The oldest key was the one dropped. */
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &oldest, 2020u));
    return 0;
}

static int test_distinct_fields_are_distinct_keys(void)
{
    tron_mesh_packet_t base = make_packet(0xf602u, 33u);
    tron_mesh_packet_t other_src = make_packet(0xf603u, 33u);
    tron_mesh_packet_t other_seq = make_packet(0xf602u, 34u);
    tron_mesh_packet_t other_net = make_packet(0xf602u, 33u);

    other_net.net_id = 0x02u;

    tron_mesh_dedupe_reset(&cache);
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &base, 1000u));
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &other_src, 1000u));
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &other_seq, 1000u));
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &other_net, 1000u));
    ASSERT_EQ_U32(4u, tron_mesh_dedupe_slots_used(&cache));
    return 0;
}

/* TTL is deliberately not part of the key: a relayed copy differs from the
   original only by hop count and must still be recognised. */
static int test_ttl_is_not_part_of_key(void)
{
    tron_mesh_packet_t fresh = make_packet(0xf602u, 33u);
    tron_mesh_packet_t relayed = make_packet(0xf602u, 33u);

    relayed.ttl = (uint8_t)(TRON_MESH_TTL_MAX - 1u);

    tron_mesh_dedupe_reset(&cache);
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &fresh, 1000u));
    ASSERT_EQ_U32(1u, tron_mesh_dedupe_seen_or_insert(&cache, &relayed, 1100u));
    return 0;
}

static int test_ping_and_pong_echo_are_distinct_keys(void)
{
    tron_mesh_packet_t ping = make_packet(TRON_MESH_PINGPONG_ROOT_ID, 77u);
    tron_mesh_packet_t pong = make_packet(TRON_MESH_PINGPONG_LEAF_ID, 77u);
    tron_mesh_packet_t same_source_pong = ping;

    ping.msg_type = TRON_MESH_MSG_TYPE_PING;
    pong.msg_type = TRON_MESH_MSG_TYPE_PONG;
    same_source_pong.msg_type = TRON_MESH_MSG_TYPE_PONG;

    tron_mesh_dedupe_reset(&cache);
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &ping, 1000u));
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &pong, 1000u));
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, &same_source_pong, 1000u));
    ASSERT_EQ_U32(3u, tron_mesh_dedupe_slots_used(&cache));
    ASSERT_EQ_U32(1u, tron_mesh_dedupe_seen_or_insert(&cache, &ping, 1100u));
    ASSERT_EQ_U32(1u, tron_mesh_dedupe_seen_or_insert(&cache, &pong, 1100u));
    return 0;
}

static int test_null_arguments_are_safe(void)
{
    tron_mesh_packet_t p = make_packet(0xf602u, 33u);

    tron_mesh_dedupe_reset(NULL);
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(NULL, &p, 1000u));
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_seen_or_insert(&cache, NULL, 1000u));
    ASSERT_EQ_U32(0u, tron_mesh_dedupe_slots_used(NULL));
    return 0;
}

static int test_time_reached_wraps_safely(void)
{
    ASSERT_TRUE(tron_mesh_time_reached(1000u, 1000u));
    ASSERT_TRUE(tron_mesh_time_reached(1001u, 1000u));
    ASSERT_TRUE(!tron_mesh_time_reached(999u, 1000u));
    /* Across a 32-bit wrap the comparison must still order correctly. */
    ASSERT_TRUE(tron_mesh_time_reached(10u, 0xFFFFFFF0u));
    ASSERT_TRUE(!tron_mesh_time_reached(0xFFFFFFF0u, 10u));
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
        { "first sighting is new", test_first_sighting_is_new },
        { "immediate repeat is duplicate", test_immediate_repeat_is_duplicate },
        { "second key does not evict first", test_second_key_does_not_evict_first },
        { "cache uses full capacity", test_cache_uses_full_capacity },
        { "observed hardware interleave", test_observed_hardware_interleave },
        { "entry expires after TTL", test_entry_expires_after_ttl },
        { "overflow evicts oldest not newest", test_overflow_evicts_oldest_not_newest },
        { "distinct fields are distinct keys", test_distinct_fields_are_distinct_keys },
        { "TTL is not part of key", test_ttl_is_not_part_of_key },
        { "PING/PONG echo keys keep type and source distinct", test_ping_and_pong_echo_are_distinct_keys },
        { "NULL arguments are safe", test_null_arguments_are_safe },
        { "time_reached wraps safely", test_time_reached_wraps_safely },
    };
    size_t i;

    for (i = 0u; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (run_test(&tests[i]) != 0) {
            return 1;
        }
    }

    printf("All TRON mesh dedupe tests passed (%lu tests).\n",
           (unsigned long)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
