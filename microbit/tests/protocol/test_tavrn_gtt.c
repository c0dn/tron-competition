#include "tavrn_gtt.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;
static const char *reported[8];
static unsigned int reported_count;

static int first_for(const char *requirement)
{
    unsigned int i;

    for (i = 0u; i < reported_count; i++) {
        if (strcmp(reported[i], requirement) == 0) {
            return 0;
        }
    }
    reported[reported_count++] = requirement;
    return 1;
}

#define CHECK(requirement, expression) \
    do { \
        if (!(expression)) { \
            if (first_for(requirement)) { \
                printf("FAIL %s: %s:%d: assertion failed: %s\n", \
                       (requirement), __FILE__, __LINE__, #expression); \
            } \
            failures++; \
        } \
    } while (0)

static tavrn_adva_t make_identity(uint8_t value)
{
    tavrn_adva_t identity;

    memset(&identity, 0, sizeof(identity));
    identity.bytes[0] = value;
    identity.bytes[1] = 0x42u;
    identity.bytes[5] = 0xc0u;
    return identity;
}

static int same_identity(const tavrn_adva_t *left, const tavrn_adva_t *right)
{
    return memcmp(left->bytes, right->bytes, TAVRN_ADVA_LEN) == 0;
}

static tavrn_gtt_config_t make_config(void)
{
    tavrn_gtt_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_identity = make_identity(0x80u);
    config.soft_expiry_ms = 50u;
    config.hard_expiry_ms = 100u;
    config.departed_retention_ms = 200u;
    return config;
}

static tavrn_gtt_evidence_t make_evidence(tavrn_adva_t identity,
                                           uint16_t serial, uint8_t hop_count,
                                           tavrn_gtt_evidence_kind_t kind)
{
    tavrn_gtt_evidence_t evidence;

    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = identity;
    evidence.serial = serial;
    evidence.serial_present = 1u;
    evidence.hop_count = hop_count;
    evidence.kind = kind;
    return evidence;
}

static void test_gtt_01_capacity_and_self_protection(void)
{
    tavrn_gtt_t gtt;
    tavrn_gtt_storage_t storage;
    tavrn_gtt_config_t config = make_config();
    tavrn_gtt_snapshot_t snapshot;
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_config_t invalid_config;
    uint8_t i;

    CHECK("GTT-01", TAVRN_GTT_CAPACITY == 16u && TAVRN_GTT_HOP_MAX == 15u);
    CHECK("GTT-01", tavrn_gtt_init(&gtt, &storage, &config, 0u) ==
                        TAVRN_GTT_INIT_OK);
    CHECK("GTT-01", tavrn_gtt_snapshot(&gtt, &config.local_identity, 0u,
                                          &snapshot) == TAVRN_GTT_QUERY_FOUND &&
                        snapshot.freshness == TAVRN_GTT_FRESHNESS_ACTIVE);
    invalid_config = config;
    invalid_config.soft_expiry_ms = invalid_config.hard_expiry_ms;
    CHECK("GTT-01", tavrn_gtt_init(&gtt, &storage, &invalid_config, 0u) ==
                        TAVRN_GTT_INIT_INVALID_CONFIG);
    CHECK("GTT-01", tavrn_gtt_init(&gtt, &storage, &config, 0u) ==
                        TAVRN_GTT_INIT_OK);
    evidence = make_evidence(config.local_identity, 1u, 9u,
                             TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-01", tavrn_gtt_observe(&gtt, &evidence, 1u) ==
                        TAVRN_GTT_OBSERVE_SELF_IGNORED &&
                        tavrn_gtt_counters(&gtt)->self_ignored == 1u);
    for (i = 0u; i < TAVRN_GTT_CAPACITY - 1u; i++) {
        evidence = make_evidence(make_identity((uint8_t)(i + 1u)),
                                 (uint16_t)(i + 1u), 1u,
                                 TAVRN_GTT_EVIDENCE_LIVENESS);
        CHECK("GTT-01", tavrn_gtt_observe(&gtt, &evidence, 2u) ==
                            TAVRN_GTT_OBSERVE_ADDED);
    }
    evidence = make_evidence(make_identity(0x70u), 1u, 1u,
                             TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-01", tavrn_gtt_observe(&gtt, &evidence, 2u) ==
                        TAVRN_GTT_OBSERVE_CAPACITY_REJECTED &&
                        tavrn_gtt_counters(&gtt)->capacity_rejected == 1u);
}

static void test_gtt_02_replacement_order(void)
{
    tavrn_gtt_t gtt;
    tavrn_gtt_t no_victim;
    tavrn_gtt_storage_t storage;
    tavrn_gtt_storage_t no_victim_storage;
    tavrn_gtt_config_t config = make_config();
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_snapshot_t snapshot;
    tavrn_adva_t oldest_departed_identity = make_identity(0x0eu);
    tavrn_adva_t newer_departed_identity = make_identity(0x0fu);
    tavrn_adva_t oldest_identity = make_identity(0x01u);
    tavrn_adva_t second_oldest_identity = make_identity(0x02u);
    tavrn_adva_t departed_replacement = make_identity(0x60u);
    tavrn_adva_t second_departed_replacement = make_identity(0x62u);
    tavrn_adva_t stale_replacement = make_identity(0x61u);
    uint8_t i;

    CHECK("GTT-02", tavrn_gtt_init(&gtt, &storage, &config, 0u) ==
                        TAVRN_GTT_INIT_OK);
    for (i = 0u; i < TAVRN_GTT_CAPACITY - 1u; i++) {
        evidence = make_evidence(make_identity((uint8_t)(i + 1u)),
                                 (uint16_t)(i + 1u), 1u,
                                 TAVRN_GTT_EVIDENCE_LIVENESS);
        CHECK("GTT-02", tavrn_gtt_observe(&gtt, &evidence,
                                            (uint32_t)i + 1u) ==
                            TAVRN_GTT_OBSERVE_ADDED);
    }
    evidence = make_evidence(oldest_departed_identity, 0x20u, 1u,
                             TAVRN_GTT_EVIDENCE_DEPARTED);
    CHECK("GTT-02", tavrn_gtt_observe(&gtt, &evidence, 19u) ==
                        TAVRN_GTT_OBSERVE_DEPARTED);
    evidence = make_evidence(newer_departed_identity, 0x20u, 1u,
                             TAVRN_GTT_EVIDENCE_DEPARTED);
    CHECK("GTT-02", tavrn_gtt_observe(&gtt, &evidence, 20u) ==
                        TAVRN_GTT_OBSERVE_DEPARTED);
    evidence = make_evidence(departed_replacement, 1u, 1u,
                             TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-02", tavrn_gtt_observe(&gtt, &evidence, 60u) ==
                        TAVRN_GTT_OBSERVE_ADDED &&
                        tavrn_gtt_snapshot(&gtt, &oldest_departed_identity, 60u,
                                           &snapshot) == TAVRN_GTT_QUERY_NOT_FOUND &&
                        tavrn_gtt_snapshot(&gtt, &newer_departed_identity, 60u,
                                           &snapshot) == TAVRN_GTT_QUERY_FOUND &&
                        snapshot.freshness == TAVRN_GTT_FRESHNESS_DEPARTED);
    evidence = make_evidence(second_departed_replacement, 1u, 1u,
                             TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-02", tavrn_gtt_observe(&gtt, &evidence, 61u) ==
                        TAVRN_GTT_OBSERVE_ADDED &&
                        tavrn_gtt_snapshot(&gtt, &newer_departed_identity, 61u,
                                           &snapshot) == TAVRN_GTT_QUERY_NOT_FOUND);
    evidence = make_evidence(stale_replacement, 1u, 1u,
                              TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-02", tavrn_gtt_snapshot(&gtt, &oldest_identity, 62u,
                                         &snapshot) == TAVRN_GTT_QUERY_FOUND &&
                        snapshot.freshness == TAVRN_GTT_FRESHNESS_SOFT_STALE &&
                        snapshot.soft_deadline_ms == 51u &&
                        snapshot.hard_deadline_ms == 101u);
    CHECK("GTT-02", tavrn_gtt_snapshot(&gtt, &second_oldest_identity, 62u,
                                         &snapshot) == TAVRN_GTT_QUERY_FOUND &&
                        snapshot.freshness == TAVRN_GTT_FRESHNESS_SOFT_STALE);
    CHECK("GTT-02", tavrn_gtt_observe(&gtt, &evidence, 62u) ==
                        TAVRN_GTT_OBSERVE_ADDED &&
                        tavrn_gtt_snapshot(&gtt, &oldest_identity, 62u,
                                           &snapshot) == TAVRN_GTT_QUERY_NOT_FOUND &&
                        tavrn_gtt_snapshot(&gtt, &second_oldest_identity, 62u,
                                           &snapshot) == TAVRN_GTT_QUERY_FOUND);

    CHECK("GTT-02", tavrn_gtt_init(&no_victim, &no_victim_storage, &config,
                                     0u) == TAVRN_GTT_INIT_OK);
    for (i = 0u; i < TAVRN_GTT_CAPACITY - 1u; i++) {
        evidence = make_evidence(make_identity((uint8_t)(i + 1u)),
                                 (uint16_t)(i + 1u), 1u,
                                 TAVRN_GTT_EVIDENCE_LIVENESS);
        CHECK("GTT-02", tavrn_gtt_observe(&no_victim, &evidence, 1u) ==
                            TAVRN_GTT_OBSERVE_ADDED);
    }
    evidence = make_evidence(make_identity(0x71u), 1u, 1u,
                             TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-02", tavrn_gtt_observe(&no_victim, &evidence, 2u) ==
                        TAVRN_GTT_OBSERVE_CAPACITY_REJECTED);
}

static void test_gtt_04_serial_tombstone_and_rejoin(void)
{
    tavrn_gtt_t gtt;
    tavrn_gtt_storage_t storage;
    tavrn_gtt_config_t config = make_config();
    tavrn_adva_t remote = make_identity(0x11u);
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_snapshot_t snapshot;

    CHECK("GTT-04", tavrn_gtt_serial_is_newer(0u, 0xffffu) &&
                        !tavrn_gtt_serial_is_newer(0x8000u, 0u));
    if (tavrn_gtt_init(&gtt, &storage, &config, 0u) != TAVRN_GTT_INIT_OK) {
        CHECK("GTT-04", 0);
        return;
    }
    evidence = make_evidence(remote, 0xffffu, 4u, TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-04", tavrn_gtt_observe(&gtt, &evidence, 1u) ==
                        TAVRN_GTT_OBSERVE_ADDED);
    evidence = make_evidence(remote, 0u, 2u, TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-04", tavrn_gtt_observe(&gtt, &evidence, 2u) ==
                        TAVRN_GTT_OBSERVE_REFRESHED);
    evidence = make_evidence(remote, 0x8000u, 1u,
                             TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-04", tavrn_gtt_observe(&gtt, &evidence, 3u) ==
                        TAVRN_GTT_OBSERVE_STALE &&
                        tavrn_gtt_snapshot(&gtt, &remote, 3u, &snapshot) ==
                            TAVRN_GTT_QUERY_FOUND && snapshot.serial == 0u &&
                        snapshot.hop_count == 2u);
    evidence = make_evidence(remote, 0u, 9u, TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-04", tavrn_gtt_observe(&gtt, &evidence, 4u) ==
                        TAVRN_GTT_OBSERVE_REFRESHED &&
                        tavrn_gtt_snapshot(&gtt, &remote, 4u, &snapshot) ==
                            TAVRN_GTT_QUERY_FOUND && snapshot.hop_count == 2u);
    CHECK("GTT-04", snapshot.soft_deadline_ms == 54u &&
                        snapshot.hard_deadline_ms == 104u);
    evidence = make_evidence(remote, 1u, 2u, TAVRN_GTT_EVIDENCE_DEPARTED);
    CHECK("GTT-04", tavrn_gtt_observe(&gtt, &evidence, 5u) ==
                        TAVRN_GTT_OBSERVE_DEPARTED &&
                        tavrn_gtt_snapshot(&gtt, &remote, 5u, &snapshot) ==
                            TAVRN_GTT_QUERY_FOUND &&
                        snapshot.freshness == TAVRN_GTT_FRESHNESS_DEPARTED);
    evidence = make_evidence(remote, 0u, 7u, TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-04", tavrn_gtt_observe(&gtt, &evidence, 6u) ==
                        TAVRN_GTT_OBSERVE_STALE &&
                        tavrn_gtt_snapshot(&gtt, &remote, 6u, &snapshot) ==
                            TAVRN_GTT_QUERY_FOUND &&
                        snapshot.freshness == TAVRN_GTT_FRESHNESS_DEPARTED);
    evidence = make_evidence(remote, 2u, 10u, TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-04", tavrn_gtt_observe(&gtt, &evidence, 7u) ==
                        TAVRN_GTT_OBSERVE_REFRESHED &&
                        tavrn_gtt_snapshot(&gtt, &remote, 7u, &snapshot) ==
                            TAVRN_GTT_QUERY_FOUND &&
                        snapshot.freshness == TAVRN_GTT_FRESHNESS_ACTIVE &&
                        snapshot.hop_count == 10u &&
                        snapshot.hard_deadline_ms == 107u);
    remote = make_identity(0x12u);
    evidence = make_evidence(remote, 0u, 1u, TAVRN_GTT_EVIDENCE_LIVENESS);
    evidence.serial_present = 0u;
    CHECK("GTT-04", tavrn_gtt_observe(&gtt, &evidence, 8u) ==
                        TAVRN_GTT_OBSERVE_ADDED &&
                        tavrn_gtt_snapshot(&gtt, &remote, 8u, &snapshot) ==
                            TAVRN_GTT_QUERY_FOUND && snapshot.serial_present == 0u);

    remote = make_identity(0x13u);
    evidence = make_evidence(remote, 1u, 1u, TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-04", tavrn_gtt_observe(&gtt, &evidence, 9u) ==
                        TAVRN_GTT_OBSERVE_ADDED);
    evidence = make_evidence(remote, 2u, 1u, TAVRN_GTT_EVIDENCE_DEPARTED);
    CHECK("GTT-04", tavrn_gtt_observe(&gtt, &evidence, 10u) ==
                        TAVRN_GTT_OBSERVE_DEPARTED &&
                        tavrn_gtt_snapshot(&gtt, &remote, 209u, &snapshot) ==
                            TAVRN_GTT_QUERY_FOUND &&
                        snapshot.freshness == TAVRN_GTT_FRESHNESS_DEPARTED &&
                        tavrn_gtt_snapshot(&gtt, &remote, 210u, &snapshot) ==
                            TAVRN_GTT_QUERY_NOT_FOUND);
}

static void test_gtt_05_wrap_safe_expiry(void)
{
    tavrn_gtt_t gtt;
    tavrn_gtt_t departed_gtt;
    tavrn_gtt_storage_t storage;
    tavrn_gtt_storage_t departed_storage;
    tavrn_gtt_config_t config = make_config();
    tavrn_adva_t remote = make_identity(0x20u);
    tavrn_gtt_evidence_t evidence = make_evidence(
        remote, 1u, 1u, TAVRN_GTT_EVIDENCE_LIVENESS);
    tavrn_gtt_snapshot_t snapshot;
    uint32_t observed_at = UINT32_MAX - 25u;

    CHECK("GTT-05", tavrn_gtt_init(&gtt, &storage, &config, observed_at) ==
                        TAVRN_GTT_INIT_OK);
    CHECK("GTT-05", tavrn_gtt_observe(&gtt, &evidence, observed_at) ==
                        TAVRN_GTT_OBSERVE_ADDED);
    CHECK("GTT-05", tavrn_gtt_snapshot(&gtt, &remote, 23u, &snapshot) ==
                        TAVRN_GTT_QUERY_FOUND &&
                        snapshot.freshness == TAVRN_GTT_FRESHNESS_ACTIVE &&
                        snapshot.soft_deadline_ms == 24u &&
                        snapshot.hard_deadline_ms == 74u);
    CHECK("GTT-05", tavrn_gtt_snapshot(&gtt, &remote, 24u, &snapshot) ==
                        TAVRN_GTT_QUERY_FOUND &&
                        snapshot.freshness == TAVRN_GTT_FRESHNESS_SOFT_STALE);
    CHECK("GTT-05", tavrn_gtt_snapshot(&gtt, &remote, 74u, &snapshot) ==
                        TAVRN_GTT_QUERY_FOUND &&
                        snapshot.freshness == TAVRN_GTT_FRESHNESS_HARD_EXPIRED);

    remote = make_identity(0x21u);
    evidence = make_evidence(remote, 1u, 1u, TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-05", tavrn_gtt_init(&departed_gtt, &departed_storage, &config,
                                     observed_at) == TAVRN_GTT_INIT_OK &&
                        tavrn_gtt_observe(&departed_gtt, &evidence, observed_at) ==
                            TAVRN_GTT_OBSERVE_ADDED);
    evidence = make_evidence(remote, 2u, 1u, TAVRN_GTT_EVIDENCE_DEPARTED);
    CHECK("GTT-05", tavrn_gtt_observe(&departed_gtt, &evidence,
                                        UINT32_MAX - 20u) ==
                        TAVRN_GTT_OBSERVE_DEPARTED &&
                        tavrn_gtt_snapshot(&departed_gtt, &remote, 178u,
                                           &snapshot) == TAVRN_GTT_QUERY_FOUND &&
                        snapshot.freshness == TAVRN_GTT_FRESHNESS_DEPARTED &&
                        snapshot.departed_deadline_ms == 179u &&
                        tavrn_gtt_snapshot(&departed_gtt, &remote, 179u,
                                           &snapshot) == TAVRN_GTT_QUERY_NOT_FOUND);
}

static void test_gtt_05_deterministic_lazy_enumeration(void)
{
    tavrn_gtt_t gtt;
    tavrn_gtt_storage_t storage;
    tavrn_gtt_config_t config = make_config();
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_snapshot_t snapshots[TAVRN_GTT_CAPACITY];
    tavrn_gtt_snapshot_t snapshot;
    tavrn_adva_t first = make_identity(0x30u);
    tavrn_adva_t second = make_identity(0x10u);
    uint8_t count = 0u;

    CHECK("GTT-05", tavrn_gtt_init(&gtt, &storage, &config, 0u) ==
                        TAVRN_GTT_INIT_OK);
    evidence = make_evidence(first, 1u, 3u, TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-05", tavrn_gtt_observe(&gtt, &evidence, 0u) ==
                        TAVRN_GTT_OBSERVE_ADDED);
    evidence = make_evidence(second, 1u, 2u, TAVRN_GTT_EVIDENCE_LIVENESS);
    CHECK("GTT-05", tavrn_gtt_observe(&gtt, &evidence, 0u) ==
                        TAVRN_GTT_OBSERVE_ADDED);
    CHECK("GTT-05", tavrn_gtt_enumerate_active(&gtt, 0u, snapshots,
                                                   TAVRN_GTT_CAPACITY, &count) ==
                        TAVRN_GTT_QUERY_FOUND && count == 3u &&
                        same_identity(&snapshots[0].identity, &second) &&
                        same_identity(&snapshots[1].identity,
                                       &config.local_identity) &&
                        same_identity(&snapshots[2].identity, &first));
    CHECK("GTT-05", tavrn_gtt_enumerate_active(&gtt, 50u, snapshots,
                                                  TAVRN_GTT_CAPACITY, &count) ==
                        TAVRN_GTT_QUERY_FOUND && count == 3u &&
                        snapshots[0].freshness == TAVRN_GTT_FRESHNESS_SOFT_STALE &&
                        snapshots[1].freshness == TAVRN_GTT_FRESHNESS_ACTIVE &&
                        snapshots[2].freshness == TAVRN_GTT_FRESHNESS_SOFT_STALE);
    CHECK("GTT-05", tavrn_gtt_enumerate_active(&gtt, 100u, snapshots,
                                                  TAVRN_GTT_CAPACITY, &count) ==
                        TAVRN_GTT_QUERY_FOUND && count == 1u &&
                        same_identity(&snapshots[0].identity,
                                      &config.local_identity));
    CHECK("GTT-05", tavrn_gtt_snapshot(&gtt, &first, 100u, &snapshot) ==
                        TAVRN_GTT_QUERY_FOUND &&
                        snapshot.freshness == TAVRN_GTT_FRESHNESS_HARD_EXPIRED);
}

int main(void)
{
    test_gtt_01_capacity_and_self_protection();
    test_gtt_02_replacement_order();
    test_gtt_04_serial_tombstone_and_rejoin();
    test_gtt_05_wrap_safe_expiry();
    test_gtt_05_deterministic_lazy_enumeration();
    if (failures != 0u) {
        printf("tavrn_gtt RED tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_gtt tests passed\n");
    return 0;
}
