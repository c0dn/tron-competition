#ifndef TAVRN_GTT_H
#define TAVRN_GTT_H

#include <stdint.h>

#include "tavrn_wire_v2.h"

#define TAVRN_GTT_CAPACITY 16u
#define TAVRN_GTT_HOP_MAX  15u

typedef char tavrn_gtt_capacity_guard[
    (TAVRN_GTT_CAPACITY == 16u) ? 1 : -1];
typedef char tavrn_gtt_hop_guard[
    (TAVRN_GTT_HOP_MAX == 15u) ? 1 : -1];

typedef enum tavrn_gtt_freshness {
    TAVRN_GTT_FRESHNESS_ACTIVE = 0,
    TAVRN_GTT_FRESHNESS_SOFT_STALE,
    TAVRN_GTT_FRESHNESS_HARD_EXPIRED,
    TAVRN_GTT_FRESHNESS_DEPARTED,
} tavrn_gtt_freshness_t;

typedef enum tavrn_gtt_evidence_kind {
    TAVRN_GTT_EVIDENCE_LIVENESS = 0,
    TAVRN_GTT_EVIDENCE_DEPARTED,
} tavrn_gtt_evidence_kind_t;

typedef struct tavrn_gtt_evidence {
    tavrn_adva_t identity;
    uint16_t serial;
    uint8_t serial_present;
    uint8_t hop_count;
    tavrn_gtt_evidence_kind_t kind;
} tavrn_gtt_evidence_t;

typedef struct tavrn_gtt_entry {
    tavrn_adva_t identity;
    uint32_t last_evidence_ms;
    uint32_t soft_deadline_ms;
    uint32_t hard_deadline_ms;
    uint32_t departed_deadline_ms;
    uint16_t serial;
    uint8_t occupied;
    uint8_t serial_present;
    uint8_t hop_count;
    uint8_t departed;
} tavrn_gtt_entry_t;

typedef struct tavrn_gtt_storage {
    tavrn_gtt_entry_t entries[TAVRN_GTT_CAPACITY];
} tavrn_gtt_storage_t;

typedef struct tavrn_gtt_config {
    tavrn_adva_t local_identity;
    uint32_t soft_expiry_ms;
    uint32_t hard_expiry_ms;
    uint32_t departed_retention_ms;
} tavrn_gtt_config_t;

typedef struct tavrn_gtt_snapshot {
    tavrn_adva_t identity;
    uint32_t last_evidence_ms;
    uint32_t soft_deadline_ms;
    uint32_t hard_deadline_ms;
    uint32_t departed_deadline_ms;
    uint16_t serial;
    uint8_t serial_present;
    uint8_t hop_count;
    tavrn_gtt_freshness_t freshness;
} tavrn_gtt_snapshot_t;

typedef struct tavrn_gtt_counters {
    uint32_t observed;
    uint32_t self_ignored;
    uint32_t stale_evidence;
    uint32_t capacity_rejected;
    uint32_t departed;
    uint32_t resurrected;
} tavrn_gtt_counters_t;

typedef struct tavrn_gtt {
    tavrn_gtt_storage_t *storage;
    tavrn_gtt_config_t config;
    tavrn_gtt_counters_t counters;
} tavrn_gtt_t;

typedef enum tavrn_gtt_init_status {
    TAVRN_GTT_INIT_OK = 0,
    TAVRN_GTT_INIT_INVALID_ARGUMENT,
    TAVRN_GTT_INIT_INVALID_CONFIG,
} tavrn_gtt_init_status_t;

typedef enum tavrn_gtt_observe_status {
    TAVRN_GTT_OBSERVE_ADDED = 0,
    TAVRN_GTT_OBSERVE_REFRESHED,
    TAVRN_GTT_OBSERVE_DEPARTED,
    TAVRN_GTT_OBSERVE_STALE,
    TAVRN_GTT_OBSERVE_SELF_IGNORED,
    TAVRN_GTT_OBSERVE_CAPACITY_REJECTED,
    TAVRN_GTT_OBSERVE_INVALID,
} tavrn_gtt_observe_status_t;

typedef enum tavrn_gtt_query_status {
    TAVRN_GTT_QUERY_FOUND = 0,
    TAVRN_GTT_QUERY_NOT_FOUND,
    TAVRN_GTT_QUERY_OUTPUT_TOO_SMALL,
    TAVRN_GTT_QUERY_INVALID,
} tavrn_gtt_query_status_t;

/* Returns true only for 0 < candidate-current < 0x8000. */
int tavrn_gtt_serial_is_newer(uint16_t candidate, uint16_t current);

tavrn_gtt_init_status_t tavrn_gtt_init(tavrn_gtt_t *gtt,
                                        tavrn_gtt_storage_t *storage,
                                        const tavrn_gtt_config_t *config,
                                        uint32_t now_ms);
tavrn_gtt_observe_status_t tavrn_gtt_observe(
    tavrn_gtt_t *gtt, const tavrn_gtt_evidence_t *evidence, uint32_t now_ms);
tavrn_gtt_query_status_t tavrn_gtt_snapshot(
    const tavrn_gtt_t *gtt, const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_snapshot_t *snapshot_out);
tavrn_gtt_query_status_t tavrn_gtt_enumerate_active(
    const tavrn_gtt_t *gtt, uint32_t now_ms,
    tavrn_gtt_snapshot_t *snapshots_out, uint8_t snapshot_capacity,
    uint8_t *snapshot_count_out);
const tavrn_gtt_counters_t *tavrn_gtt_counters(const tavrn_gtt_t *gtt);

#endif /* TAVRN_GTT_H */
