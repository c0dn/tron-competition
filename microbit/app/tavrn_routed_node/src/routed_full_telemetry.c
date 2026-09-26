#include "routed_full_telemetry.h"

#include <string.h>

static int adva_is_valid(const tavrn_adva_t *identity)
{
    uint8_t random_all_zero = 1u;
    uint8_t random_all_one = 1u;
    uint8_t index;

    if (identity == NULL || (identity->bytes[5] & 0xc0u) != 0xc0u) {
        return 0;
    }
    for (index = 0u; index < TAVRN_ADVA_LEN - 1u; index++) {
        if (identity->bytes[index] != 0u) {
            random_all_zero = 0u;
        }
        if (identity->bytes[index] != 0xffu) {
            random_all_one = 0u;
        }
    }
    if ((identity->bytes[5] & 0x3fu) != 0u) {
        random_all_zero = 0u;
    }
    if ((identity->bytes[5] & 0x3fu) != 0x3fu) {
        random_all_one = 0u;
    }
    return random_all_zero == 0u && random_all_one == 0u;
}

static int gtt_config_is_valid(const tavrn_gtt_config_t *config)
{
    return config != NULL && adva_is_valid(&config->local_identity) &&
        config->soft_expiry_ms != 0u &&
        config->soft_expiry_ms < ROUTED_CYCLE_HALF_RANGE &&
        config->hard_expiry_ms != 0u &&
        config->hard_expiry_ms < ROUTED_CYCLE_HALF_RANGE &&
        config->departed_retention_ms != 0u &&
        config->departed_retention_ms < ROUTED_CYCLE_HALF_RANGE &&
        config->soft_expiry_ms < config->hard_expiry_ms &&
        config->hard_expiry_ms < ROUTED_CYCLE_HALF_RANGE / 2u &&
        config->departed_retention_ms == config->hard_expiry_ms * 2u;
}

static int time_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static int adva_equal(const tavrn_adva_t *left, const tavrn_adva_t *right)
{
    return left != NULL && right != NULL &&
        memcmp(left->bytes, right->bytes, TAVRN_ADVA_LEN) == 0;
}

static routed_cycle_gtt_freshness_t copy_freshness(
    const tavrn_gtt_t *gtt, const tavrn_gtt_entry_t *entry, uint32_t query_at_ms)
{
    if (adva_equal(&entry->identity, &gtt->config.local_identity)) {
        return ROUTED_CYCLE_GTT_FRESHNESS_ACTIVE;
    }
    if (entry->departed != 0u) {
        return ROUTED_CYCLE_GTT_FRESHNESS_DEPARTED;
    }
    if (time_due(query_at_ms, entry->hard_deadline_ms)) {
        return ROUTED_CYCLE_GTT_FRESHNESS_HARD_EXPIRED;
    }
    if (time_due(query_at_ms, entry->soft_deadline_ms)) {
        return ROUTED_CYCLE_GTT_FRESHNESS_SOFT_STALE;
    }
    return ROUTED_CYCLE_GTT_FRESHNESS_ACTIVE;
}

static void copy_entry(const tavrn_gtt_t *gtt, const tavrn_gtt_entry_t *entry,
                       uint32_t query_at_ms,
                       routed_cycle_gtt_snapshot_entry_t *snapshot_entry)
{
    uint8_t is_self = adva_equal(&entry->identity, &gtt->config.local_identity) ?
        1u : 0u;

    memset(snapshot_entry, 0, sizeof(*snapshot_entry));
    snapshot_entry->canonical_adva = entry->identity;
    snapshot_entry->last_evidence_ms = entry->last_evidence_ms;
    snapshot_entry->soft_deadline_ms = entry->soft_deadline_ms;
    snapshot_entry->hard_deadline_ms = entry->hard_deadline_ms;
    snapshot_entry->departed_deadline_ms = entry->departed_deadline_ms;
    snapshot_entry->serial = entry->serial;
    snapshot_entry->hop_count = entry->hop_count;
    snapshot_entry->serial_state = entry->serial_present != 0u ?
        ROUTED_CYCLE_VALUE_KNOWN : ROUTED_CYCLE_VALUE_UNKNOWN;
    snapshot_entry->hop_state = ROUTED_CYCLE_VALUE_KNOWN;
    snapshot_entry->freshness = copy_freshness(gtt, entry, query_at_ms);
    snapshot_entry->departed = entry->departed != 0u ?
        ROUTED_CYCLE_BOOLEAN_TRUE : ROUTED_CYCLE_BOOLEAN_FALSE;
    if (is_self != 0u) {
        snapshot_entry->serial = 0u;
        snapshot_entry->hop_count = 0u;
        snapshot_entry->serial_state = ROUTED_CYCLE_VALUE_UNKNOWN;
        snapshot_entry->hop_state = ROUTED_CYCLE_VALUE_KNOWN;
        snapshot_entry->freshness = ROUTED_CYCLE_GTT_FRESHNESS_ACTIVE;
        snapshot_entry->departed = ROUTED_CYCLE_BOOLEAN_FALSE;
    }
}

static void sort_entries(routed_cycle_gtt_snapshot_t *snapshot)
{
    uint8_t index;

    for (index = 1u; index < snapshot->entry_count; index++) {
        routed_cycle_gtt_snapshot_entry_t current = snapshot->entries[index];
        uint8_t insertion = index;

        while (insertion > 0u && memcmp(current.canonical_adva.bytes,
                                         snapshot->entries[insertion - 1u]
                                             .canonical_adva.bytes,
                                         TAVRN_ADVA_LEN) < 0) {
            snapshot->entries[insertion] = snapshot->entries[insertion - 1u];
            insertion--;
        }
        snapshot->entries[insertion] = current;
    }
}

uint32_t routed_full_telemetry_max_scheduler_gap(uint32_t recorded_gap_ms,
                                                  uint32_t sample_gap_ms)
{
    return sample_gap_ms > recorded_gap_ms ? sample_gap_ms : recorded_gap_ms;
}

routed_full_telemetry_status_t routed_full_telemetry_snapshot_gtt(
    const tavrn_gtt_t *gtt, uint32_t query_at_ms,
    routed_cycle_gtt_snapshot_t *snapshot_out)
{
    uint8_t index;
    uint8_t retained_count = 0u;

    if (gtt == NULL || gtt->storage == NULL || snapshot_out == NULL ||
        !gtt_config_is_valid(&gtt->config)) {
        return ROUTED_FULL_TELEMETRY_INVALID;
    }

    /* Preflight every retained source entry before changing the caller output. */
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &gtt->storage->entries[index];

        if (entry->occupied == 0u) {
            continue;
        }
        if (entry->departed != 0u && time_due(query_at_ms,
                                                entry->departed_deadline_ms)) {
            continue;
        }
        if (retained_count >= ROUTED_CYCLE_GTT_SNAPSHOT_CAPACITY) {
            return ROUTED_FULL_TELEMETRY_INVALID;
        }
        retained_count++;
    }

    memset(snapshot_out, 0, sizeof(*snapshot_out));
    snapshot_out->query_at_ms = query_at_ms;
    snapshot_out->local_canonical_adva = gtt->config.local_identity;
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &gtt->storage->entries[index];

        if (entry->occupied == 0u ||
            (entry->departed != 0u && time_due(query_at_ms,
                                                entry->departed_deadline_ms))) {
            continue;
        }
        copy_entry(gtt, entry, query_at_ms,
                    &snapshot_out->entries[snapshot_out->entry_count]);
        if (entry->departed == 0u ||
            adva_equal(&entry->identity, &gtt->config.local_identity)) {
            snapshot_out->nondeparted_count++;
        }
        snapshot_out->entry_count++;
    }
    sort_entries(snapshot_out);
    return ROUTED_FULL_TELEMETRY_OK;
}
