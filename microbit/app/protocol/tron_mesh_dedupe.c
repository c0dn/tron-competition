#include "tron_mesh_dedupe.h"

int tron_mesh_time_reached(uint32_t now, uint32_t deadline)
{
    return (uint32_t)(now - deadline) < 0x80000000UL;
}

void tron_mesh_dedupe_reset(tron_mesh_dedupe_t *cache)
{
    unsigned int i;

    if (cache == NULL) {
        return;
    }

    cache->timers = tron_timer_config_is_valid(&tron_timer_config) ?
        &tron_timer_config : NULL;

    for (i = 0u; i < TRON_MESH_DEDUPE_SIZE; i++) {
        cache->entries[i].valid = 0u;
        cache->entries[i].net_id = 0u;
        cache->entries[i].msg_type = 0u;
        cache->entries[i].src = 0u;
        cache->entries[i].seq24 = 0u;
        cache->entries[i].expires_at_ms = 0u;
    }
}

static int same_packet(const tron_mesh_dedupe_entry_t *entry,
                       const tron_mesh_packet_t *packet)
{
    return entry->valid != 0u &&
           entry->net_id == packet->net_id &&
           entry->msg_type == packet->msg_type &&
           entry->src == packet->src &&
           entry->seq24 == (packet->seq24 & TRON_MESH_SEQ24_MAX);
}

int tron_mesh_dedupe_seen_or_insert(tron_mesh_dedupe_t *cache,
                                    const tron_mesh_packet_t *packet,
                                    uint32_t now_ms)
{
    int free_slot = -1;
    int oldest_slot = -1;
    uint32_t oldest_expiry = 0u;
    int insert;
    unsigned int i;

    if (cache == NULL || packet == NULL ||
        !tron_timer_config_is_valid(cache->timers)) {
        return 0;
    }

    for (i = 0u; i < TRON_MESH_DEDUPE_SIZE; i++) {
        tron_mesh_dedupe_entry_t *entry = &cache->entries[i];

        if (entry->valid != 0u && tron_mesh_time_reached(now_ms, entry->expires_at_ms)) {
            entry->valid = 0u;
        }

        if (same_packet(entry, packet)) {
            return 1;
        }

        /* Track the first free slot and the oldest live entry separately.
           Folding both into one index makes every free slot after the first
           candidate unreachable, which collapses the cache to a single entry
           and lets duplicates through. */
        if (entry->valid == 0u) {
            if (free_slot < 0) {
                free_slot = (int)i;
            }
        } else if (oldest_slot < 0 ||
                   tron_mesh_time_reached(oldest_expiry, entry->expires_at_ms)) {
            oldest_slot = (int)i;
            oldest_expiry = entry->expires_at_ms;
        }
    }

    if (free_slot >= 0) {
        insert = free_slot;
    } else if (oldest_slot >= 0) {
        insert = oldest_slot;
    } else {
        insert = 0;
    }

    cache->entries[insert].valid = 1u;
    cache->entries[insert].net_id = packet->net_id;
    cache->entries[insert].msg_type = packet->msg_type;
    cache->entries[insert].src = packet->src;
    cache->entries[insert].seq24 = packet->seq24 & TRON_MESH_SEQ24_MAX;
    cache->entries[insert].expires_at_ms = now_ms + cache->timers->legacy_dedupe_ms;
    return 0;
}

unsigned int tron_mesh_dedupe_slots_used(const tron_mesh_dedupe_t *cache)
{
    unsigned int used = 0u;
    unsigned int i;

    if (cache == NULL) {
        return 0u;
    }

    for (i = 0u; i < TRON_MESH_DEDUPE_SIZE; i++) {
        if (cache->entries[i].valid != 0u) {
            used++;
        }
    }
    return used;
}
