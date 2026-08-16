#include "tavrn_gtt.h"

#include <string.h>

#define TAVRN_GTT_HALF_RANGE 0x80000000u

static uint32_t next_generation(tavrn_gtt_t *gtt);
static void revise_entry(tavrn_gtt_t *gtt, tavrn_gtt_entry_t *entry);

static int time_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static int duration_is_valid(uint32_t duration_ms)
{
    return duration_ms != 0u && duration_ms < TAVRN_GTT_HALF_RANGE;
}

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

static int identities_equal(const tavrn_adva_t *left, const tavrn_adva_t *right)
{
    return memcmp(left->bytes, right->bytes, TAVRN_ADVA_LEN) == 0;
}

static int config_is_valid(const tavrn_gtt_config_t *config)
{
    if (config == NULL || !adva_is_valid(&config->local_identity) ||
        !duration_is_valid(config->soft_expiry_ms) ||
        !duration_is_valid(config->hard_expiry_ms) ||
        !duration_is_valid(config->departed_retention_ms) ||
        config->soft_expiry_ms >= config->hard_expiry_ms) {
        return 0;
    }
    return 1;
}

static int gtt_is_usable(const tavrn_gtt_t *gtt)
{
    return gtt != NULL && gtt->storage != NULL && config_is_valid(&gtt->config);
}

static int evidence_is_valid(const tavrn_gtt_evidence_t *evidence)
{
    return evidence != NULL && adva_is_valid(&evidence->identity) &&
        evidence->serial_present <= 1u && evidence->hop_count <= TAVRN_GTT_HOP_MAX &&
        (evidence->kind == TAVRN_GTT_EVIDENCE_LIVENESS ||
         evidence->kind == TAVRN_GTT_EVIDENCE_DEPARTED);
}

static int entry_is_expired_tombstone(const tavrn_gtt_entry_t *entry,
                                      uint32_t now_ms)
{
    return entry->occupied != 0u && entry->departed != 0u &&
        time_due(now_ms, entry->departed_deadline_ms);
}

static tavrn_gtt_freshness_t entry_freshness(const tavrn_gtt_t *gtt,
                                             const tavrn_gtt_entry_t *entry,
                                             uint32_t now_ms)
{
    if (identities_equal(&entry->identity, &gtt->config.local_identity)) {
        return TAVRN_GTT_FRESHNESS_ACTIVE;
    }
    if (entry->departed != 0u) {
        return TAVRN_GTT_FRESHNESS_DEPARTED;
    }
    if (time_due(now_ms, entry->hard_deadline_ms)) {
        return TAVRN_GTT_FRESHNESS_HARD_EXPIRED;
    }
    if (time_due(now_ms, entry->soft_deadline_ms)) {
        return TAVRN_GTT_FRESHNESS_SOFT_STALE;
    }
    return TAVRN_GTT_FRESHNESS_ACTIVE;
}

static tavrn_gtt_entry_t *find_entry(tavrn_gtt_t *gtt,
                                     const tavrn_adva_t *identity,
                                     uint32_t now_ms)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        tavrn_gtt_entry_t *entry = &gtt->storage->entries[index];

        if (entry->occupied != 0u && !entry_is_expired_tombstone(entry, now_ms) &&
            identities_equal(&entry->identity, identity)) {
            return entry;
        }
    }
    return NULL;
}

static const tavrn_gtt_entry_t *find_const_entry(const tavrn_gtt_t *gtt,
                                                  const tavrn_adva_t *identity,
                                                  uint32_t now_ms)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &gtt->storage->entries[index];

        if (entry->occupied != 0u && !entry_is_expired_tombstone(entry, now_ms) &&
            identities_equal(&entry->identity, identity)) {
            return entry;
        }
    }
    return NULL;
}

static int timestamp_precedes(uint32_t left, uint32_t right)
{
    uint32_t difference = left - right;

    return difference != 0u && difference != TAVRN_GTT_HALF_RANGE &&
        (int32_t)difference < 0;
}

static int entry_is_older(const tavrn_gtt_entry_t *candidate,
                          const tavrn_gtt_entry_t *current)
{
    if (timestamp_precedes(candidate->last_evidence_ms, current->last_evidence_ms)) {
        return 1;
    }
    if (timestamp_precedes(current->last_evidence_ms, candidate->last_evidence_ms)) {
        return 0;
    }
    return memcmp(candidate->identity.bytes, current->identity.bytes,
                  TAVRN_ADVA_LEN) < 0;
}

static tavrn_gtt_entry_t *find_slot_for_new_entry(tavrn_gtt_t *gtt,
                                                   uint32_t now_ms)
{
    tavrn_gtt_entry_t *oldest_departed = NULL;
    tavrn_gtt_entry_t *oldest_stale = NULL;
    uint8_t index;

    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        tavrn_gtt_entry_t *entry = &gtt->storage->entries[index];

        if (entry->occupied == 0u || entry_is_expired_tombstone(entry, now_ms)) {
            return entry;
        }
        if (entry->departed != 0u) {
            if (oldest_departed == NULL || entry_is_older(entry, oldest_departed)) {
                oldest_departed = entry;
            }
        }
    }
    if (oldest_departed != NULL) {
        return oldest_departed;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        tavrn_gtt_entry_t *entry = &gtt->storage->entries[index];
        tavrn_gtt_freshness_t freshness = entry_freshness(gtt, entry, now_ms);

        if ((freshness == TAVRN_GTT_FRESHNESS_SOFT_STALE ||
             freshness == TAVRN_GTT_FRESHNESS_HARD_EXPIRED) &&
            (oldest_stale == NULL || entry_is_older(entry, oldest_stale))) {
            oldest_stale = entry;
        }
    }
    return oldest_stale;
}

static void refresh_liveness_deadlines(tavrn_gtt_entry_t *entry,
                                       const tavrn_gtt_config_t *config,
                                       uint32_t now_ms)
{
    entry->last_evidence_ms = now_ms;
    entry->soft_deadline_ms = now_ms + config->soft_expiry_ms;
    entry->hard_deadline_ms = now_ms + config->hard_expiry_ms;
    entry->departed_deadline_ms = 0u;
    entry->departed = 0u;
    entry->application_requested = 0u;
}

static void refresh_departed_deadlines(tavrn_gtt_entry_t *entry,
                                       const tavrn_gtt_config_t *config,
                                       uint32_t now_ms)
{
    entry->last_evidence_ms = now_ms;
    entry->soft_deadline_ms = now_ms + config->soft_expiry_ms;
    entry->hard_deadline_ms = now_ms + config->hard_expiry_ms;
    entry->departed_deadline_ms = now_ms + config->departed_retention_ms;
    entry->departed = 1u;
}

static void improve_or_retain_hop(tavrn_gtt_entry_t *entry, uint8_t hop_count)
{
    if (entry->hop_count == 0u ||
        (hop_count != 0u && hop_count < entry->hop_count)) {
        entry->hop_count = hop_count;
    }
}

static void initialize_entry(tavrn_gtt_entry_t *entry,
                             const tavrn_gtt_evidence_t *evidence,
                             const tavrn_gtt_config_t *config,
                             uint32_t now_ms)
{
    memset(entry, 0, sizeof(*entry));
    entry->identity = evidence->identity;
    entry->occupied = 1u;
    entry->serial = evidence->serial;
    entry->serial_present = evidence->serial_present;
    entry->hop_count = evidence->hop_count;
    if (evidence->kind == TAVRN_GTT_EVIDENCE_DEPARTED) {
        refresh_departed_deadlines(entry, config, now_ms);
    } else {
        refresh_liveness_deadlines(entry, config, now_ms);
    }
}

static tavrn_gtt_observe_status_t merge_existing_entry(
    tavrn_gtt_t *gtt, tavrn_gtt_entry_t *entry,
    const tavrn_gtt_evidence_t *evidence, uint32_t now_ms)
{
    int evidence_is_newer = 0;

    if (evidence->serial_present != 0u) {
        if (entry->serial_present == 0u) {
            evidence_is_newer = 1;
        } else if (evidence->serial != entry->serial) {
            if (!tavrn_gtt_serial_is_newer(evidence->serial, entry->serial)) {
                gtt->counters.stale_evidence++;
                return TAVRN_GTT_OBSERVE_STALE;
            }
            evidence_is_newer = 1;
        }
    }

    if (evidence->kind == TAVRN_GTT_EVIDENCE_DEPARTED) {
        if (evidence_is_newer != 0) {
            entry->serial = evidence->serial;
            entry->serial_present = 1u;
            entry->hop_count = evidence->hop_count;
        } else {
            improve_or_retain_hop(entry, evidence->hop_count);
        }
        refresh_departed_deadlines(entry, &gtt->config, now_ms);
        gtt->counters.departed++;
        return TAVRN_GTT_OBSERVE_DEPARTED;
    }

    if (entry->departed != 0u) {
        if (evidence_is_newer != 0) {
            entry->serial = evidence->serial;
            entry->serial_present = 1u;
        }
        entry->hop_count = evidence->hop_count;
        refresh_liveness_deadlines(entry, &gtt->config, now_ms);
        gtt->counters.resurrected++;
        return TAVRN_GTT_OBSERVE_REFRESHED;
    }

    if (evidence_is_newer != 0) {
        entry->serial = evidence->serial;
        entry->serial_present = 1u;
        entry->hop_count = evidence->hop_count;
    } else {
        improve_or_retain_hop(entry, evidence->hop_count);
    }
    refresh_liveness_deadlines(entry, &gtt->config, now_ms);
    return TAVRN_GTT_OBSERVE_REFRESHED;
}

static void copy_snapshot(const tavrn_gtt_t *gtt,
                          const tavrn_gtt_entry_t *entry, uint32_t now_ms,
                          tavrn_gtt_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->identity = entry->identity;
    snapshot->last_evidence_ms = entry->last_evidence_ms;
    snapshot->soft_deadline_ms = entry->soft_deadline_ms;
    snapshot->hard_deadline_ms = entry->hard_deadline_ms;
    snapshot->departed_deadline_ms = entry->departed_deadline_ms;
    snapshot->serial = entry->serial;
    snapshot->serial_present = entry->serial_present;
    snapshot->hop_count = entry->hop_count;
    snapshot->freshness = entry_freshness(gtt, entry, now_ms);
}

int tavrn_gtt_serial_is_newer(uint16_t candidate, uint16_t current)
{
    uint16_t difference = (uint16_t)(candidate - current);

    return difference != 0u && difference < 0x8000u;
}

tavrn_gtt_init_status_t tavrn_gtt_init(tavrn_gtt_t *gtt,
                                        tavrn_gtt_storage_t *storage,
                                        const tavrn_gtt_config_t *config,
                                        uint32_t now_ms)
{
    tavrn_gtt_entry_t *self;

    if (gtt == NULL || storage == NULL || config == NULL) {
        return TAVRN_GTT_INIT_INVALID_ARGUMENT;
    }
    memset(gtt, 0, sizeof(*gtt));
    if (!config_is_valid(config)) {
        return TAVRN_GTT_INIT_INVALID_CONFIG;
    }
    memset(storage, 0, sizeof(*storage));
    gtt->storage = storage;
    gtt->config = *config;
    storage->next_generation = 1u;
    self = &storage->entries[0];
    self->identity = config->local_identity;
    self->occupied = 1u;
    refresh_liveness_deadlines(self, config, now_ms);
    self->revision = 1u;
    self->storage_generation = 1u;
    return TAVRN_GTT_INIT_OK;
}

tavrn_gtt_observe_status_t tavrn_gtt_observe(
    tavrn_gtt_t *gtt, const tavrn_gtt_evidence_t *evidence, uint32_t now_ms)
{
    tavrn_gtt_entry_t *entry;

    if (!gtt_is_usable(gtt) || !evidence_is_valid(evidence)) {
        return TAVRN_GTT_OBSERVE_INVALID;
    }
    gtt->counters.observed++;
    if (identities_equal(&evidence->identity, &gtt->config.local_identity)) {
        gtt->counters.self_ignored++;
        return TAVRN_GTT_OBSERVE_SELF_IGNORED;
    }
    entry = find_entry(gtt, &evidence->identity, now_ms);
    if (entry != NULL) {
        tavrn_gtt_observe_status_t status =
            merge_existing_entry(gtt, entry, evidence, now_ms);

        if (status == TAVRN_GTT_OBSERVE_REFRESHED ||
            status == TAVRN_GTT_OBSERVE_DEPARTED) {
            revise_entry(gtt, entry);
        }
        return status;
    }
    entry = find_slot_for_new_entry(gtt, now_ms);
    if (entry == NULL) {
        gtt->counters.capacity_rejected++;
        return TAVRN_GTT_OBSERVE_CAPACITY_REJECTED;
    }
    initialize_entry(entry, evidence, &gtt->config, now_ms);
    revise_entry(gtt, entry);
    if (evidence->kind == TAVRN_GTT_EVIDENCE_DEPARTED) {
        gtt->counters.departed++;
        return TAVRN_GTT_OBSERVE_DEPARTED;
    }
    return TAVRN_GTT_OBSERVE_ADDED;
}

uint8_t tavrn_gtt_known_remote_count(const tavrn_gtt_t *gtt)
{
    uint8_t count = 0u;
    uint8_t index;

    if (!gtt_is_usable(gtt)) {
        return 0u;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &gtt->storage->entries[index];

        if (entry->occupied == 0u || entry->departed != 0u ||
            identities_equal(&entry->identity, &gtt->config.local_identity)) {
            continue;
        }
        count++;
    }
    return count;
}

tavrn_gtt_query_status_t tavrn_gtt_snapshot(
    const tavrn_gtt_t *gtt, const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_snapshot_t *snapshot_out)
{
    const tavrn_gtt_entry_t *entry;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (!gtt_is_usable(gtt) || !adva_is_valid(identity) || snapshot_out == NULL) {
        return TAVRN_GTT_QUERY_INVALID;
    }
    entry = find_const_entry(gtt, identity, now_ms);
    if (entry == NULL) {
        return TAVRN_GTT_QUERY_NOT_FOUND;
    }
    copy_snapshot(gtt, entry, now_ms, snapshot_out);
    return TAVRN_GTT_QUERY_FOUND;
}

tavrn_gtt_serial_clear_status_t tavrn_gtt_clear_serial(
    tavrn_gtt_t *gtt, const tavrn_adva_t *identity, uint32_t now_ms)
{
    tavrn_gtt_entry_t *entry;

    if (!gtt_is_usable(gtt) || !adva_is_valid(identity) ||
        identities_equal(identity, &gtt->config.local_identity)) {
        return TAVRN_GTT_SERIAL_CLEAR_INVALID;
    }
    entry = find_entry(gtt, identity, now_ms);
    if (entry == NULL) {
        return TAVRN_GTT_SERIAL_CLEAR_NOT_FOUND;
    }
    if (entry->serial_present != 0u) {
        entry->serial = 0u;
        entry->serial_present = 0u;
        revise_entry(gtt, entry);
    }
    return TAVRN_GTT_SERIAL_CLEAR_FOUND;
}

tavrn_gtt_query_status_t tavrn_gtt_enumerate_active(
    const tavrn_gtt_t *gtt, uint32_t now_ms,
    tavrn_gtt_snapshot_t *snapshots_out, uint8_t snapshot_capacity,
    uint8_t *snapshot_count_out)
{
    tavrn_gtt_snapshot_t snapshots[TAVRN_GTT_CAPACITY];
    uint8_t snapshot_count = 0u;
    uint8_t index;

    if (snapshot_count_out != NULL) {
        *snapshot_count_out = 0u;
    }
    if (!gtt_is_usable(gtt) || snapshot_count_out == NULL ||
        (snapshots_out == NULL && snapshot_capacity != 0u)) {
        return TAVRN_GTT_QUERY_INVALID;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &gtt->storage->entries[index];
        tavrn_gtt_freshness_t freshness;

        if (entry->occupied == 0u || entry->departed != 0u ||
            entry_is_expired_tombstone(entry, now_ms)) {
            continue;
        }
        freshness = entry_freshness(gtt, entry, now_ms);
        if (freshness == TAVRN_GTT_FRESHNESS_HARD_EXPIRED ||
            freshness == TAVRN_GTT_FRESHNESS_DEPARTED) {
            continue;
        }
        copy_snapshot(gtt, entry, now_ms, &snapshots[snapshot_count]);
        snapshot_count++;
    }
    *snapshot_count_out = snapshot_count;
    if (snapshot_count > snapshot_capacity) {
        return TAVRN_GTT_QUERY_OUTPUT_TOO_SMALL;
    }
    for (index = 1u; index < snapshot_count; index++) {
        tavrn_gtt_snapshot_t current = snapshots[index];
        uint8_t insertion = index;

        while (insertion > 0u &&
               memcmp(current.identity.bytes,
                      snapshots[(uint8_t)(insertion - 1u)].identity.bytes,
                      TAVRN_ADVA_LEN) < 0) {
            snapshots[insertion] = snapshots[(uint8_t)(insertion - 1u)];
            insertion--;
        }
        snapshots[insertion] = current;
    }
    for (index = 0u; index < snapshot_count; index++) {
        snapshots_out[index] = snapshots[index];
    }
    return TAVRN_GTT_QUERY_FOUND;
}

const tavrn_gtt_counters_t *tavrn_gtt_counters(const tavrn_gtt_t *gtt)
{
    return gtt == NULL ? NULL : &gtt->counters;
}

/* Phase-5 expiry API.  Legacy callers above retain their original no-jitter
 * behavior; these helpers are the only revision-owning mutation path. */
static uint32_t next_generation(tavrn_gtt_t *gtt)
{
    uint32_t value = gtt->storage->next_generation + 1u;

    if (value == 0u) {
        value = 1u;
    }
    gtt->storage->next_generation = value;
    return value;
}

static void revise_entry(tavrn_gtt_t *gtt, tavrn_gtt_entry_t *entry)
{
    uint32_t generation = next_generation(gtt);

    entry->revision = generation;
    entry->storage_generation = generation;
}

static uint32_t expiry_hash(const tavrn_gtt_t *gtt,
                            const tavrn_gtt_evidence_t *evidence,
                            uint32_t now_ms)
{
    uint32_t hash = 2166136261u;
    uint16_t serial = evidence->serial_present != 0u ? evidence->serial : 0u;
    uint8_t bytes[3];
    uint8_t index;

    bytes[0] = evidence->serial_present != 0u ? 1u : 0u;
    bytes[1] = (uint8_t)serial;
    bytes[2] = (uint8_t)(serial >> 8);
    for (index = 0u; index < TAVRN_ADVA_LEN; index++) {
        hash ^= gtt->config.local_identity.bytes[index];
        hash *= 16777619u;
    }
    for (index = 0u; index < TAVRN_ADVA_LEN; index++) {
        hash ^= evidence->identity.bytes[index];
        hash *= 16777619u;
    }
    for (index = 0u; index < sizeof(bytes); index++) {
        hash ^= bytes[index];
        hash *= 16777619u;
    }
    for (index = 0u; index < 4u; index++) {
        hash ^= (uint8_t)(now_ms >> (8u * index));
        hash *= 16777619u;
    }
    return hash;
}

static int provenance_is_direct(tavrn_gtt_provenance_t provenance)
{
    return provenance == TAVRN_GTT_PROVENANCE_DIRECT_HELLO ||
        provenance == TAVRN_GTT_PROVENANCE_DIRECT_OUTER_TRANSMITTER ||
        provenance == TAVRN_GTT_PROVENANCE_DIRECT_BOOTSTRAP ||
        provenance == TAVRN_GTT_PROVENANCE_DIRECT_INCARNATION;
}

static void expiry_set_liveness(tavrn_gtt_t *gtt, tavrn_gtt_entry_t *entry,
                                 const tavrn_gtt_evidence_t *evidence,
                                 tavrn_gtt_provenance_t provenance,
                                 uint32_t now_ms)
{
    uint32_t hard_offset = gtt->config.hard_expiry_ms;
    uint32_t soft_offset = gtt->config.soft_expiry_ms;

    uint32_t jitter = expiry_hash(gtt, evidence, now_ms) %
        (gtt->config.hard_expiry_ms / 6u + 1u);

    hard_offset += jitter;
    soft_offset = (uint32_t)(((uint64_t)hard_offset * gtt->config.soft_expiry_ms) /
                             gtt->config.hard_expiry_ms);
    if (provenance_is_direct(provenance)) {
        entry->direct = 1u;
        entry->last_direct_evidence_ms = now_ms;
    }
    entry->last_evidence_ms = now_ms;
    entry->soft_deadline_ms = now_ms + soft_offset;
    entry->hard_deadline_ms = now_ms + hard_offset;
    entry->departed_deadline_ms = 0u;
    entry->departed = 0u;
}

/* A one-shot application request asks for liveness, not a durable membership
 * flag.  Only accepted liveness reaches this mutation path; rejected, stale,
 * and duplicate evidence returns before it can consume the request. */
static void expiry_clear_application_request(tavrn_gtt_entry_t *entry)
{
    entry->application_requested = 0u;
}

static void expiry_copy_snapshot(const tavrn_gtt_t *gtt,
                                 const tavrn_gtt_entry_t *entry,
                                 uint32_t now_ms,
                                 tavrn_gtt_expiry_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->identity = entry->identity;
    snapshot->last_evidence_ms = entry->last_evidence_ms;
    snapshot->last_direct_evidence_ms = entry->last_direct_evidence_ms;
    snapshot->soft_deadline_ms = entry->soft_deadline_ms;
    snapshot->hard_deadline_ms = entry->hard_deadline_ms;
    snapshot->departed_deadline_ms = entry->departed_deadline_ms;
    snapshot->revision = entry->revision;
    snapshot->storage_generation = entry->storage_generation;
    snapshot->serial = entry->serial;
    snapshot->serial_present = entry->serial_present;
    snapshot->hop_count = entry->hop_count;
    snapshot->direct = entry->direct;
    snapshot->application_requested = entry->application_requested;
    snapshot->freshness = entry_freshness(gtt, entry, now_ms);
}

tavrn_gtt_expiry_query_status_t tavrn_gtt_expiry_snapshot(
    const tavrn_gtt_t *gtt, const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out)
{
    const tavrn_gtt_entry_t *entry;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (!gtt_is_usable(gtt) || !adva_is_valid(identity) || snapshot_out == NULL) {
        return TAVRN_GTT_EXPIRY_QUERY_INVALID;
    }
    entry = find_const_entry(gtt, identity, now_ms);
    if (entry == NULL) {
        return TAVRN_GTT_EXPIRY_QUERY_NOT_FOUND;
    }
    expiry_copy_snapshot(gtt, entry, now_ms, snapshot_out);
    return TAVRN_GTT_EXPIRY_QUERY_FOUND;
}

tavrn_gtt_expiry_observe_status_t tavrn_gtt_observe_with_provenance(
    tavrn_gtt_t *gtt, const tavrn_gtt_evidence_t *evidence,
    tavrn_gtt_provenance_t provenance, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out)
{
    tavrn_gtt_entry_t *entry;
    tavrn_gtt_freshness_t prior_freshness = TAVRN_GTT_FRESHNESS_ACTIVE;
    uint8_t direct;
    uint8_t serial_changes = 0u;
    uint8_t hop_improves = 0u;
    uint8_t created = 0u;
    uint8_t resurrected = 0u;
    tavrn_gtt_expiry_observe_status_t result;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (!gtt_is_usable(gtt) || !evidence_is_valid(evidence) ||
        provenance > TAVRN_GTT_PROVENANCE_DIRECT_INCARNATION ||
        (provenance_is_direct(provenance) && evidence->hop_count != 1u) ||
        (evidence->kind == TAVRN_GTT_EVIDENCE_DEPARTED &&
         provenance != TAVRN_GTT_PROVENANCE_CONFIRMED_DEPARTURE)) {
        return TAVRN_GTT_EXPIRY_OBSERVE_INVALID;
    }
    if (identities_equal(&evidence->identity, &gtt->config.local_identity)) {
        return TAVRN_GTT_EXPIRY_OBSERVE_SELF_IGNORED;
    }
    entry = find_entry(gtt, &evidence->identity, now_ms);
    direct = provenance_is_direct(provenance) ? 1u : 0u;
    if (entry != NULL && evidence->serial_present != 0u &&
        entry->serial_present != 0u) {
        if (direct != 0u &&
            !tavrn_gtt_serial_is_newer(evidence->serial, entry->serial)) {
            /* Direct evidence has a stricter high-water rule: equal is stale
             * too, so a direct replay cannot refresh a liveness deadline. */
            return TAVRN_GTT_EXPIRY_OBSERVE_STALE;
        }
        if (direct == 0u && evidence->serial != entry->serial &&
            !tavrn_gtt_serial_is_newer(evidence->serial, entry->serial)) {
            return TAVRN_GTT_EXPIRY_OBSERVE_STALE;
        }
    }
    if (entry == NULL) {
        tavrn_gtt_entry_t *slot = find_slot_for_new_entry(gtt, now_ms);

        if (slot == NULL) {
            gtt->counters.capacity_rejected++;
            return TAVRN_GTT_EXPIRY_OBSERVE_NO_VICTIM;
        }
        if (slot->occupied != 0u) {
            prior_freshness = entry_freshness(gtt, slot, now_ms);
        }
        memset(slot, 0, sizeof(*slot));
        slot->identity = evidence->identity;
        slot->occupied = 1u;
        slot->serial = evidence->serial;
        slot->serial_present = evidence->serial_present;
        slot->hop_count = evidence->hop_count;
        entry = slot;
        created = 1u;
        result = prior_freshness == TAVRN_GTT_FRESHNESS_DEPARTED ?
            TAVRN_GTT_EXPIRY_OBSERVE_REPLACED_DEPARTED :
            (prior_freshness == TAVRN_GTT_FRESHNESS_SOFT_STALE ||
             prior_freshness == TAVRN_GTT_FRESHNESS_HARD_EXPIRED ?
             TAVRN_GTT_EXPIRY_OBSERVE_REPLACED_OLDEST_STALE :
             TAVRN_GTT_EXPIRY_OBSERVE_ADDED);
    } else {
        serial_changes = evidence->serial_present != 0u &&
            (entry->serial_present == 0u || evidence->serial != entry->serial);
        hop_improves = evidence->hop_count != 0u &&
            (entry->hop_count == 0u || evidence->hop_count < entry->hop_count);
        if (evidence->kind == TAVRN_GTT_EVIDENCE_LIVENESS && entry->departed == 0u &&
            entry->last_evidence_ms == now_ms && serial_changes == 0u &&
            hop_improves == 0u && (direct == 0u ||
                                    (entry->direct != 0u &&
                                     entry->last_direct_evidence_ms == now_ms))) {
            if (snapshot_out != NULL) {
                expiry_copy_snapshot(gtt, entry, now_ms, snapshot_out);
            }
            return TAVRN_GTT_EXPIRY_OBSERVE_DUPLICATE;
        }
        result = TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED;
        if (evidence->serial_present != 0u &&
            (entry->serial_present == 0u || evidence->serial != entry->serial)) {
            entry->serial = evidence->serial;
            entry->serial_present = 1u;
        }
        if (evidence->hop_count != 0u &&
            (entry->hop_count == 0u || evidence->hop_count < entry->hop_count)) {
            entry->hop_count = evidence->hop_count;
        }
    }
    if (evidence->kind == TAVRN_GTT_EVIDENCE_DEPARTED) {
        entry->last_evidence_ms = now_ms;
        entry->soft_deadline_ms = now_ms;
        entry->hard_deadline_ms = now_ms;
        entry->departed_deadline_ms = now_ms + gtt->config.departed_retention_ms;
        entry->departed = 1u;
        entry->direct = 0u;
        entry->last_direct_evidence_ms = 0u;
        entry->application_requested = 0u;
    } else {
        if (entry->departed != 0u) {
            resurrected = 1u;
            entry->direct = 0u;
            entry->last_direct_evidence_ms = 0u;
        }
        /* A same-time direct admission changes only provenance.  It preserves
         * the imported lifetime used by the preceding membership observation. */
        if (created == 0u && direct != 0u && entry->last_evidence_ms == now_ms &&
            entry->departed == 0u) {
            entry->direct = 1u;
            entry->last_direct_evidence_ms = now_ms;
            expiry_clear_application_request(entry);
            revise_entry(gtt, entry);
            if (snapshot_out != NULL) {
                expiry_copy_snapshot(gtt, entry, now_ms, snapshot_out);
            }
            return result;
        }
        expiry_set_liveness(gtt, entry, evidence, provenance, now_ms);
        expiry_clear_application_request(entry);
        if (resurrected != 0u) {
            gtt->counters.resurrected++;
        }
    }
    revise_entry(gtt, entry);
    if (snapshot_out != NULL) {
        expiry_copy_snapshot(gtt, entry, now_ms, snapshot_out);
    }
    return result;
}

tavrn_gtt_expiry_query_status_t tavrn_gtt_enumerate_known(
    const tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshots_out, uint8_t capacity,
    uint8_t *count_out)
{
    uint8_t index;
    uint8_t count = 0u;

    if (count_out != NULL) {
        *count_out = 0u;
    }
    if (!gtt_is_usable(gtt) || count_out == NULL ||
        (snapshots_out == NULL && capacity != 0u)) {
        return TAVRN_GTT_EXPIRY_QUERY_INVALID;
    }
    if (lane != TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER) {
        return TAVRN_GTT_EXPIRY_QUERY_MARSHAL_REQUIRED;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &gtt->storage->entries[index];

        if (entry->occupied == 0u || entry->departed != 0u ||
            entry_is_expired_tombstone(entry, now_ms)) {
            continue;
        }
        count++;
    }
    *count_out = count;
    if (count > capacity) {
        return TAVRN_GTT_EXPIRY_QUERY_OUTPUT_TOO_SMALL;
    }
    count = 0u;
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &gtt->storage->entries[index];

        if (entry->occupied == 0u || entry->departed != 0u ||
            entry_is_expired_tombstone(entry, now_ms)) {
            continue;
        }
        expiry_copy_snapshot(gtt, entry, now_ms, &snapshots_out[count]);
        count++;
    }
    return TAVRN_GTT_EXPIRY_QUERY_FOUND;
}

static tavrn_gtt_application_request_status_t expiry_application_mutate(
    tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane,
    const tavrn_adva_t *identity, uint32_t now_ms, uint8_t request,
    tavrn_gtt_expiry_snapshot_t *snapshot_out)
{
    tavrn_gtt_entry_t *entry;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (!gtt_is_usable(gtt) || !adva_is_valid(identity)) {
        return TAVRN_GTT_APPLICATION_INVALID;
    }
    if (lane != TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER) {
        return TAVRN_GTT_APPLICATION_MARSHAL_REQUIRED;
    }
    if (identities_equal(identity, &gtt->config.local_identity)) {
        entry = find_entry(gtt, identity, now_ms);
        if (entry != NULL && snapshot_out != NULL) {
            expiry_copy_snapshot(gtt, entry, now_ms, snapshot_out);
        }
        return TAVRN_GTT_APPLICATION_SELF;
    }
    entry = find_entry(gtt, identity, now_ms);
    if (entry == NULL) {
        return TAVRN_GTT_APPLICATION_NOT_FOUND;
    }
    if (entry->departed != 0u) {
        if (snapshot_out != NULL) {
            expiry_copy_snapshot(gtt, entry, now_ms, snapshot_out);
        }
        return TAVRN_GTT_APPLICATION_DEPARTED;
    }
    if (entry->application_requested == request) {
        if (snapshot_out != NULL) {
            expiry_copy_snapshot(gtt, entry, now_ms, snapshot_out);
        }
        return TAVRN_GTT_APPLICATION_UNCHANGED_DUPLICATE;
    }
    entry->application_requested = request;
    revise_entry(gtt, entry);
    if (snapshot_out != NULL) {
        expiry_copy_snapshot(gtt, entry, now_ms, snapshot_out);
    }
    return request != 0u ? TAVRN_GTT_APPLICATION_CHANGED :
        TAVRN_GTT_APPLICATION_CANCELED;
}

tavrn_gtt_application_request_status_t tavrn_gtt_application_request(
    tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane,
    const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out)
{
    return expiry_application_mutate(gtt, lane, identity, now_ms, 1u, snapshot_out);
}

tavrn_gtt_application_request_status_t tavrn_gtt_application_cancel(
    tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane,
    const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out)
{
    return expiry_application_mutate(gtt, lane, identity, now_ms, 0u, snapshot_out);
}

tavrn_gtt_force_due_status_t tavrn_gtt_force_due(
    tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane,
    const tavrn_adva_t *identity, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out)
{
    tavrn_gtt_entry_t *entry;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (gtt == NULL || gtt->storage == NULL) {
        return TAVRN_GTT_FORCE_DUE_UNAVAILABLE;
    }
    if (!gtt_is_usable(gtt) || !adva_is_valid(identity)) {
        return TAVRN_GTT_FORCE_DUE_INVALID;
    }
    if (lane != TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER) {
        return TAVRN_GTT_FORCE_DUE_MARSHAL_REQUIRED;
    }
    if (identities_equal(identity, &gtt->config.local_identity)) {
        return TAVRN_GTT_FORCE_DUE_SELF;
    }
    entry = find_entry(gtt, identity, now_ms);
    if (entry == NULL) {
        return TAVRN_GTT_FORCE_DUE_NOT_FOUND;
    }
    if (entry->departed != 0u) {
        if (snapshot_out != NULL) {
            expiry_copy_snapshot(gtt, entry, now_ms, snapshot_out);
        }
        return TAVRN_GTT_FORCE_DUE_DEPARTED;
    }
    if (time_due(now_ms, entry->soft_deadline_ms) &&
        time_due(now_ms, entry->hard_deadline_ms)) {
        if (snapshot_out != NULL) {
            expiry_copy_snapshot(gtt, entry, now_ms, snapshot_out);
        }
        return TAVRN_GTT_FORCE_DUE_ALREADY_DUE;
    }
    /* Deliberately change only the two expiry facts.  Real/direct evidence,
     * serial, retained hop, directness, and application demand are preserved. */
    entry->soft_deadline_ms = now_ms;
    entry->hard_deadline_ms = now_ms;
    revise_entry(gtt, entry);
    if (snapshot_out != NULL) {
        expiry_copy_snapshot(gtt, entry, now_ms, snapshot_out);
    }
    return TAVRN_GTT_FORCE_DUE_CHANGED;
}

tavrn_gtt_application_command_result_t tavrn_gtt_apply_application_command(
    tavrn_gtt_t *gtt, tavrn_phase5_expiry_lane_t lane,
    tavrn_gtt_application_command_t command)
{
    tavrn_gtt_application_command_result_t result;

    memset(&result, 0, sizeof(result));
    result.query_status = TAVRN_GTT_EXPIRY_QUERY_INVALID;
    if (command.kind == TAVRN_GTT_APPLICATION_COMMAND_QUERY) {
        if (lane != TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER) {
            result.status = TAVRN_GTT_APPLICATION_MARSHAL_REQUIRED;
            result.query_status = TAVRN_GTT_EXPIRY_QUERY_MARSHAL_REQUIRED;
            return result;
        }
        result.query_status = tavrn_gtt_expiry_snapshot(gtt, &command.identity,
                                                         command.query_time_ms,
                                                         &result.snapshot);
        result.status = result.query_status == TAVRN_GTT_EXPIRY_QUERY_FOUND ?
            TAVRN_GTT_APPLICATION_CHANGED : TAVRN_GTT_APPLICATION_NOT_FOUND;
    } else if (command.kind == TAVRN_GTT_APPLICATION_COMMAND_REQUEST) {
        result.status = tavrn_gtt_application_request(gtt, lane, &command.identity,
                                                       command.now_ms, &result.snapshot);
        result.query_status = result.status == TAVRN_GTT_APPLICATION_MARSHAL_REQUIRED ?
            TAVRN_GTT_EXPIRY_QUERY_MARSHAL_REQUIRED :
            (result.status == TAVRN_GTT_APPLICATION_INVALID ?
             TAVRN_GTT_EXPIRY_QUERY_INVALID : TAVRN_GTT_EXPIRY_QUERY_FOUND);
    } else if (command.kind == TAVRN_GTT_APPLICATION_COMMAND_CANCEL) {
        result.status = tavrn_gtt_application_cancel(gtt, lane, &command.identity,
                                                      command.now_ms, &result.snapshot);
        result.query_status = result.status == TAVRN_GTT_APPLICATION_MARSHAL_REQUIRED ?
            TAVRN_GTT_EXPIRY_QUERY_MARSHAL_REQUIRED :
            (result.status == TAVRN_GTT_APPLICATION_INVALID ?
             TAVRN_GTT_EXPIRY_QUERY_INVALID : TAVRN_GTT_EXPIRY_QUERY_FOUND);
    } else {
        result.status = TAVRN_GTT_APPLICATION_INVALID;
    }
    return result;
}

tavrn_gtt_sync_merge_status_t tavrn_gtt_sync_merge(
    tavrn_gtt_t *gtt, const tavrn_gtt_sync_record_t *records,
    uint8_t record_count, uint32_t now_ms)
{
    tavrn_gtt_storage_t candidate;
    tavrn_gtt_t candidate_gtt;
    uint8_t index;
    uint8_t changed = 0u;

    if (!gtt_is_usable(gtt) || records == NULL || record_count == 0u ||
        record_count > TAVRN_GTT_CAPACITY) {
        return TAVRN_GTT_SYNC_MERGE_INVALID;
    }
    for (index = 0u; index < record_count; index++) {
        const tavrn_gtt_sync_record_t *record = &records[index];

        if (!adva_is_valid(&record->identity) || record->serial_present > 1u ||
            record->hop_count > TAVRN_GTT_HOP_MAX || record->departed > 1u ||
            (record->departed == 0u &&
             (record->remaining_lifetime_ms == 0u ||
              record->remaining_lifetime_ms >= TAVRN_GTT_HALF_RANGE))) {
            return TAVRN_GTT_SYNC_MERGE_INVALID;
        }
    }
    candidate = *gtt->storage;
    candidate_gtt = *gtt;
    candidate_gtt.storage = &candidate;
    for (index = 0u; index < record_count; index++) {
        tavrn_gtt_entry_t *entry;
        const tavrn_gtt_sync_record_t *record = &records[index];
        uint32_t hard_deadline = now_ms + record->remaining_lifetime_ms;
        uint32_t soft_lifetime = record->remaining_lifetime_ms;
        uint32_t soft_deadline;
        uint16_t effective_serial;
        uint8_t effective_serial_present;
        uint8_t created = 0u;

        if (identities_equal(&record->identity, &candidate_gtt.config.local_identity)) {
            continue;
        }
        if (soft_lifetime > candidate_gtt.config.soft_expiry_ms) {
            soft_lifetime = candidate_gtt.config.soft_expiry_ms;
        }
        soft_deadline = now_ms + soft_lifetime;
        entry = find_entry(&candidate_gtt, &record->identity, now_ms);
        if (entry == NULL) {
            entry = find_slot_for_new_entry(&candidate_gtt, now_ms);
            if (entry == NULL) {
                return TAVRN_GTT_SYNC_MERGE_INVALID;
            }
            memset(entry, 0, sizeof(*entry));
            entry->occupied = 1u;
            entry->identity = record->identity;
            created = 1u;
        } else if (entry->serial_present != 0u && record->serial_present != 0u &&
                   entry->serial != record->serial &&
                   !tavrn_gtt_serial_is_newer(record->serial, entry->serial)) {
            continue;
        }
        effective_serial = record->serial_present != 0u ? record->serial : entry->serial;
        effective_serial_present = record->serial_present != 0u ? 1u :
            entry->serial_present;
        if (created == 0u && entry->serial == effective_serial &&
            entry->serial_present == effective_serial_present &&
            entry->hop_count == record->hop_count &&
            entry->last_evidence_ms == now_ms && entry->departed == record->departed &&
            ((record->departed != 0u &&
              entry->soft_deadline_ms == now_ms && entry->hard_deadline_ms == now_ms &&
              entry->departed_deadline_ms ==
                  now_ms + candidate_gtt.config.departed_retention_ms) ||
             (record->departed == 0u && entry->soft_deadline_ms == soft_deadline &&
              entry->hard_deadline_ms == hard_deadline &&
              entry->departed_deadline_ms == 0u))) {
            continue;
        }
        entry->serial = effective_serial;
        entry->serial_present = effective_serial_present;
        entry->hop_count = record->hop_count;
        entry->last_evidence_ms = now_ms;
        if (record->departed != 0u) {
            entry->departed = 1u;
            entry->soft_deadline_ms = now_ms;
            entry->hard_deadline_ms = now_ms;
            entry->departed_deadline_ms = now_ms + candidate_gtt.config.departed_retention_ms;
            entry->direct = 0u;
            entry->last_direct_evidence_ms = 0u;
            entry->application_requested = 0u;
        } else {
            entry->departed = 0u;
            entry->hard_deadline_ms = hard_deadline;
            entry->soft_deadline_ms = soft_deadline;
            entry->departed_deadline_ms = 0u;
            entry->application_requested = 0u;
        }
        revise_entry(&candidate_gtt, entry);
        changed = 1u;
    }
    if (changed == 0u) {
        return TAVRN_GTT_SYNC_MERGE_UNCHANGED;
    }
    *gtt->storage = candidate;
    return TAVRN_GTT_SYNC_MERGE_COMMITTED;
}

tavrn_gtt_metadata_merge_status_t tavrn_gtt_metadata_merge(
    tavrn_gtt_t *gtt, const tavrn_adva_t *identity, uint8_t ttl_bucket,
    uint32_t now_ms)
{
    tavrn_gtt_entry_t *entry;
    uint32_t hard_lifetime_ms;
    uint32_t soft_lifetime_ms;

    if (gtt == NULL || gtt->storage == NULL) {
        return TAVRN_GTT_METADATA_MERGE_UNAVAILABLE;
    }
    if (!gtt_is_usable(gtt) || !adva_is_valid(identity) || ttl_bucket > 15u) {
        return TAVRN_GTT_METADATA_MERGE_INVALID;
    }
    if (identities_equal(identity, &gtt->config.local_identity)) {
        return TAVRN_GTT_METADATA_MERGE_UNCHANGED;
    }
    entry = find_entry(gtt, identity, now_ms);
    if (entry != NULL && entry->departed != 0u) {
        return TAVRN_GTT_METADATA_MERGE_TOMBSTONE;
    }
    if (entry == NULL) {
        entry = find_slot_for_new_entry(gtt, now_ms);
        if (entry == NULL) {
            return TAVRN_GTT_METADATA_MERGE_UNAVAILABLE;
        }
        memset(entry, 0, sizeof(*entry));
        entry->identity = *identity;
        entry->occupied = 1u;
    }

    hard_lifetime_ms = (uint32_t)(ttl_bucket + 1u) * 20000u;
    if (hard_lifetime_ms > gtt->config.hard_expiry_ms) {
        hard_lifetime_ms = gtt->config.hard_expiry_ms;
    }
    soft_lifetime_ms = hard_lifetime_ms < gtt->config.soft_expiry_ms ?
        hard_lifetime_ms : gtt->config.soft_expiry_ms;
    entry->last_evidence_ms = now_ms;
    entry->soft_deadline_ms = now_ms + soft_lifetime_ms;
    entry->hard_deadline_ms = now_ms + hard_lifetime_ms;
    entry->departed_deadline_ms = 0u;
    entry->departed = 0u;
    entry->application_requested = 0u;
    /* Metadata is imported social evidence.  It must neither manufacture a
     * subject serial nor overwrite a separately established direct binding. */
    revise_entry(gtt, entry);
    return TAVRN_GTT_METADATA_MERGE_COMMITTED;
}
