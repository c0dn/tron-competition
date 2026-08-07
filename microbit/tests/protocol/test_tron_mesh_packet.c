#include "tron_mesh_packet.h"

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
            printf("FAIL %s:%d: expected 0x%08lx, got 0x%08lx\n", \
                   __FILE__, \
                   __LINE__, \
                   (unsigned long)expected_value__, \
                   (unsigned long)actual_value__); \
            return 1; \
        } \
    } while (0)

#define ASSERT_EQ_RESULT(expected, actual) \
    do { \
        tron_mesh_packet_result_t expected_result__ = (expected); \
        tron_mesh_packet_result_t actual_result__ = (actual); \
        if (expected_result__ != actual_result__) { \
            printf("FAIL %s:%d: expected %s, got %s\n", \
                   __FILE__, \
                   __LINE__, \
                   tron_mesh_packet_result_name(expected_result__), \
                   tron_mesh_packet_result_name(actual_result__)); \
            return 1; \
        } \
    } while (0)

typedef int (*test_func_t)(void);

typedef struct test_case {
    const char *name;
    test_func_t func;
} test_case_t;

static tron_mesh_packet_t make_packet(uint8_t payload_len)
{
    tron_mesh_packet_t packet;
    uint8_t i;

    memset(&packet, 0, sizeof(packet));
    packet.msg_type = TRON_MESH_MSG_TYPE_DUMMY_STATUS;
    packet.net_id = 0x42u;
    packet.ttl = 2u;
    packet.src = 0xBEEFu;
    packet.seq24 = 0x00A1B2C3u;
    packet.payload_len = payload_len;

    for (i = 0u; i < payload_len && i < TRON_MESH_PAYLOAD_MAX; i++) {
        packet.payload[i] = (uint8_t)(0xA0u + i);
    }

    return packet;
}

static int packets_equal(const tron_mesh_packet_t *expected, const tron_mesh_packet_t *actual)
{
    if (expected->msg_type != actual->msg_type ||
        expected->net_id != actual->net_id ||
        expected->ttl != actual->ttl ||
        expected->src != actual->src ||
        expected->seq24 != actual->seq24 ||
        expected->payload_len != actual->payload_len) {
        return 0;
    }

    if (expected->payload_len == 0u) {
        return 1;
    }

    return memcmp(expected->payload, actual->payload, expected->payload_len) == 0;
}

static int encode_packet(const tron_mesh_packet_t *packet, uint8_t *adv, size_t *adv_len)
{
    tron_mesh_packet_result_t result;

    result = tron_mesh_packet_encode(packet, adv, TRON_MESH_ADV_MAX_LEN, adv_len);
    ASSERT_EQ_RESULT(TRON_MESH_PACKET_OK, result);
    ASSERT_TRUE(*adv_len <= TRON_MESH_ADV_MAX_LEN);
    return 0;
}

static int test_round_trip(void)
{
    tron_mesh_packet_t packet = make_packet(3u);
    tron_mesh_packet_t decoded;
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len;

    ASSERT_TRUE(encode_packet(&packet, adv, &adv_len) == 0);
    ASSERT_EQ_U32(TRON_MESH_ADV_BASE_LEN + packet.payload_len, adv_len);
    ASSERT_EQ_RESULT(TRON_MESH_PACKET_OK, tron_mesh_packet_decode(adv, adv_len, &decoded));
    ASSERT_TRUE(packets_equal(&packet, &decoded));
    return 0;
}

static int test_network_admission(void)
{
    tron_mesh_packet_t packet = make_packet(0u);

    ASSERT_TRUE(tron_mesh_packet_is_for_network(&packet, 0x42u));
    ASSERT_TRUE(!tron_mesh_packet_is_for_network(&packet, 0x01u));
    ASSERT_TRUE(!tron_mesh_packet_is_for_network(NULL, 0x42u));
    return 0;
}

static int test_zero_length_payload(void)
{
    tron_mesh_packet_t packet = make_packet(0u);
    tron_mesh_packet_t decoded;
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len;

    ASSERT_TRUE(encode_packet(&packet, adv, &adv_len) == 0);
    ASSERT_EQ_U32(TRON_MESH_ADV_BASE_LEN, adv_len);
    ASSERT_EQ_U32(TRON_MESH_MANUF_BASE_LEN, adv[3]);
    ASSERT_EQ_RESULT(TRON_MESH_PACKET_OK, tron_mesh_packet_decode(adv, adv_len, &decoded));
    ASSERT_TRUE(packets_equal(&packet, &decoded));
    return 0;
}

static int test_max_payload_length(void)
{
    tron_mesh_packet_t packet = make_packet(TRON_MESH_PAYLOAD_MAX);
    tron_mesh_packet_t decoded;
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len;

    ASSERT_TRUE(encode_packet(&packet, adv, &adv_len) == 0);
    ASSERT_EQ_U32(TRON_MESH_ADV_BASE_LEN + TRON_MESH_PAYLOAD_MAX, adv_len);
    ASSERT_EQ_U32(TRON_MESH_MANUF_MAX_LEN, adv[3]);
    ASSERT_EQ_RESULT(TRON_MESH_PACKET_OK, tron_mesh_packet_decode(adv, adv_len, &decoded));
    ASSERT_TRUE(packets_equal(&packet, &decoded));
    return 0;
}

static int test_exact_31_byte_packet(void)
{
    tron_mesh_packet_t packet = make_packet(TRON_MESH_PAYLOAD_MAX);
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len;

    ASSERT_TRUE(encode_packet(&packet, adv, &adv_len) == 0);
    ASSERT_EQ_U32(31u, adv_len);
    ASSERT_EQ_U32(TRON_MESH_ADV_MAX_LEN, adv_len);
    return 0;
}

static int test_payload_overflow_reject(void)
{
    tron_mesh_packet_t packet = make_packet((uint8_t)(TRON_MESH_PAYLOAD_MAX + 1u));
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len = 123u;

    ASSERT_EQ_U32(0u, tron_mesh_packet_encoded_len(packet.payload_len));
    ASSERT_EQ_RESULT(
        TRON_MESH_PACKET_ERR_PAYLOAD_LEN,
        tron_mesh_packet_encode(&packet, adv, sizeof(adv), &adv_len));
    ASSERT_EQ_U32(0u, adv_len);
    return 0;
}

static int test_payload_length_mismatch_reject(void)
{
    tron_mesh_packet_t packet = make_packet(3u);
    tron_mesh_packet_t decoded;
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len;

    ASSERT_TRUE(encode_packet(&packet, adv, &adv_len) == 0);
    adv[18] = (uint8_t)(packet.payload_len + 1u);
    ASSERT_EQ_RESULT(
        TRON_MESH_PACKET_ERR_PAYLOAD_LEN,
        tron_mesh_packet_decode(adv, adv_len, &decoded));
    return 0;
}

static int test_invalid_ad_length_reject(void)
{
    tron_mesh_packet_t packet = make_packet(3u);
    tron_mesh_packet_t decoded;
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len;

    ASSERT_TRUE(encode_packet(&packet, adv, &adv_len) == 0);
    adv[3] = TRON_MESH_MANUF_MAX_LEN;
    ASSERT_EQ_RESULT(
        TRON_MESH_PACKET_ERR_LENGTH,
        tron_mesh_packet_decode(adv, adv_len, &decoded));
    return 0;
}

static int test_missing_flags_reject(void)
{
    tron_mesh_packet_t packet = make_packet(0u);
    tron_mesh_packet_t decoded;
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len;

    ASSERT_TRUE(encode_packet(&packet, adv, &adv_len) == 0);
    ASSERT_EQ_RESULT(
        TRON_MESH_PACKET_ERR_MISSING_FLAGS,
        tron_mesh_packet_decode(&adv[TRON_MESH_FLAGS_LEN], adv_len - TRON_MESH_FLAGS_LEN, &decoded));
    return 0;
}

static int test_missing_manufacturer_reject(void)
{
    uint8_t adv[] = {
        TRON_MESH_FLAGS_AD_LEN,
        TRON_MESH_FLAGS_AD_TYPE,
        TRON_MESH_FLAGS_VALUE,
    };
    tron_mesh_packet_t decoded;

    ASSERT_EQ_RESULT(
        TRON_MESH_PACKET_ERR_MISSING_MANUFACTURER,
        tron_mesh_packet_decode(adv, sizeof(adv), &decoded));
    return 0;
}

static int test_wrong_magic_reject(void)
{
    tron_mesh_packet_t packet = make_packet(1u);
    tron_mesh_packet_t decoded;
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len;

    ASSERT_TRUE(encode_packet(&packet, adv, &adv_len) == 0);
    adv[7] = (uint8_t)'X';
    ASSERT_EQ_RESULT(
        TRON_MESH_PACKET_ERR_MAGIC,
        tron_mesh_packet_decode(adv, adv_len, &decoded));
    return 0;
}

static int test_wrong_version_reject(void)
{
    tron_mesh_packet_t packet = make_packet(1u);
    tron_mesh_packet_t decoded;
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len;

    ASSERT_TRUE(encode_packet(&packet, adv, &adv_len) == 0);
    adv[9] = 0x02u;
    ASSERT_EQ_RESULT(
        TRON_MESH_PACKET_ERR_VERSION,
        tron_mesh_packet_decode(adv, adv_len, &decoded));
    return 0;
}

static int test_wrong_company_reject(void)
{
    tron_mesh_packet_t packet = make_packet(1u);
    tron_mesh_packet_t decoded;
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len;

    ASSERT_TRUE(encode_packet(&packet, adv, &adv_len) == 0);
    adv[5] = 0x34u;
    adv[6] = 0x12u;
    ASSERT_EQ_RESULT(
        TRON_MESH_PACKET_ERR_COMPANY,
        tron_mesh_packet_decode(adv, adv_len, &decoded));
    return 0;
}

static int test_ttl_over_max_reject(void)
{
    tron_mesh_packet_t packet = make_packet(1u);
    tron_mesh_packet_t decoded;
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len;

    packet.ttl = (uint8_t)(TRON_MESH_TTL_MAX + 1u);
    ASSERT_EQ_RESULT(
        TRON_MESH_PACKET_ERR_TTL,
        tron_mesh_packet_encode(&packet, adv, sizeof(adv), &adv_len));

    packet.ttl = TRON_MESH_TTL_MAX;
    ASSERT_TRUE(encode_packet(&packet, adv, &adv_len) == 0);
    adv[12] = (uint8_t)(TRON_MESH_TTL_MAX + 1u);
    ASSERT_EQ_RESULT(
        TRON_MESH_PACKET_ERR_TTL,
        tron_mesh_packet_decode(adv, adv_len, &decoded));
    return 0;
}

static int test_sequence_24_bit_little_endian(void)
{
    tron_mesh_packet_t packet = make_packet(2u);
    tron_mesh_packet_t decoded;
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len;

    packet.seq24 = 0x00A1B2C3u;
    ASSERT_TRUE(encode_packet(&packet, adv, &adv_len) == 0);
    ASSERT_EQ_U32(0xC3u, adv[15]);
    ASSERT_EQ_U32(0xB2u, adv[16]);
    ASSERT_EQ_U32(0xA1u, adv[17]);
    ASSERT_EQ_RESULT(TRON_MESH_PACKET_OK, tron_mesh_packet_decode(adv, adv_len, &decoded));
    ASSERT_EQ_U32(0x00A1B2C3u, decoded.seq24);
    return 0;
}

static int test_multiple_ad_structures(void)
{
    tron_mesh_packet_t packet = make_packet(0u);
    tron_mesh_packet_t decoded;
    uint8_t encoded[TRON_MESH_ADV_MAX_LEN];
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t encoded_len;
    size_t pos = 0u;
    size_t manufacturer_len;

    ASSERT_TRUE(encode_packet(&packet, encoded, &encoded_len) == 0);
    manufacturer_len = encoded_len - TRON_MESH_FLAGS_LEN;

    memcpy(&adv[pos], encoded, TRON_MESH_FLAGS_LEN);
    pos += TRON_MESH_FLAGS_LEN;

    adv[pos++] = 0x03u;
    adv[pos++] = 0x08u;
    adv[pos++] = (uint8_t)'T';
    adv[pos++] = (uint8_t)'M';

    ASSERT_TRUE(pos + manufacturer_len <= sizeof(adv));
    memcpy(&adv[pos], &encoded[TRON_MESH_FLAGS_LEN], manufacturer_len);
    pos += manufacturer_len;

    ASSERT_EQ_RESULT(TRON_MESH_PACKET_OK, tron_mesh_packet_decode(adv, pos, &decoded));
    ASSERT_TRUE(packets_equal(&packet, &decoded));
    return 0;
}

static int run_test(const test_case_t *test)
{
    int result = test->func();

    if (result == 0) {
        printf("PASS %s\n", test->name);
    } else {
        printf("FAIL %s\n", test->name);
    }

    return result;
}

/*
 * The wearable rides the mesh by carrying shared/schema.h's 7-byte payload as
 * msg_type MIND_EVENT. This pins the two properties that makes that legal:
 * the frame fits the 31-byte advertising cap, and the payload survives the
 * round trip byte for byte.
 */
static int test_mind_event_round_trip(void)
{
    /* mind_adv_payload_t laid out by hand so this test does not depend on the
     * struct's packing: version, type, confidence, svm LE, mic, seq. */
    static const uint8_t mind_payload[7] = {
        0x01, 0x05, 0x4B, 0x1F, 0x1A, 0x7D, 0x2A
    };
    tron_mesh_packet_t in;
    tron_mesh_packet_t out;
    uint8_t adv[TRON_MESH_ADV_MAX_LEN];
    size_t adv_len = 0u;
    size_t i;

    memset(&in, 0, sizeof(in));
    in.net_id = 0x01u;
    in.ttl = TRON_MESH_TTL_MAX;
    in.src = 0x0101u;                   /* wearable range: 0x0100 + device id */
    in.seq24 = 0x0ABCDEu;
    in.msg_type = TRON_MESH_MSG_TYPE_MIND_EVENT;
    in.payload_len = (uint8_t)sizeof(mind_payload);
    memcpy(in.payload, mind_payload, sizeof(mind_payload));

    ASSERT_EQ_U32(TRON_MESH_PACKET_OK,
                  tron_mesh_packet_encode(&in, adv, sizeof(adv), &adv_len));

    /* 19 bytes of framing + 7 of payload, with 5 to spare under the cap. */
    ASSERT_EQ_U32(26u, (uint32_t)adv_len);
    ASSERT_TRUE(adv_len <= TRON_MESH_ADV_MAX_LEN);

    ASSERT_EQ_U32(TRON_MESH_PACKET_OK,
                  tron_mesh_packet_decode(adv, adv_len, &out));

    ASSERT_EQ_U32(TRON_MESH_MSG_TYPE_MIND_EVENT, out.msg_type);
    ASSERT_EQ_U32(0x0101u, out.src);
    ASSERT_EQ_U32(0x0ABCDEu, out.seq24);
    ASSERT_EQ_U32(TRON_MESH_TTL_MAX, out.ttl);
    ASSERT_EQ_U32((uint32_t)sizeof(mind_payload), out.payload_len);

    for (i = 0u; i < sizeof(mind_payload); i++) {
        ASSERT_EQ_U32(mind_payload[i], out.payload[i]);
    }

    /* MIND_EVENT must not be confused with the dummy status type. */
    ASSERT_TRUE(TRON_MESH_MSG_TYPE_MIND_EVENT != TRON_MESH_MSG_TYPE_DUMMY_STATUS);
    return 0;
}

int main(void)
{
    static const test_case_t tests[] = {
        { "round trip", test_round_trip },
        { "MIND event round trip", test_mind_event_round_trip },
        { "network admission", test_network_admission },
        { "zero-length payload", test_zero_length_payload },
        { "max payload length", test_max_payload_length },
        { "exact 31-byte packet", test_exact_31_byte_packet },
        { "payload overflow reject", test_payload_overflow_reject },
        { "payload length mismatch reject", test_payload_length_mismatch_reject },
        { "invalid AD length reject", test_invalid_ad_length_reject },
        { "missing Flags AD reject", test_missing_flags_reject },
        { "missing Manufacturer Specific Data AD reject", test_missing_manufacturer_reject },
        { "wrong magic reject", test_wrong_magic_reject },
        { "wrong version reject", test_wrong_version_reject },
        { "wrong company reject", test_wrong_company_reject },
        { "TTL > 3 reject", test_ttl_over_max_reject },
        { "24-bit sequence little-endian", test_sequence_24_bit_little_endian },
        { "multiple AD structures", test_multiple_ad_structures },
    };
    size_t i;

    for (i = 0u; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (run_test(&tests[i]) != 0) {
            return 1;
        }
    }

    printf("All TRON mesh packet tests passed (%lu tests).\n", (unsigned long)(sizeof(tests) / sizeof(tests[0])));
    return 0;
}
