/*
 * tron_mesh_dedupe - duplicate suppression cache for the TRON BLE mesh node.
 *
 * A flood without duplicate suppression does not terminate: every node
 * rebroadcasts every copy it hears, including copies of copies. This cache is
 * what bounds that. It lives here rather than in the node application so it can
 * be exercised by host tests - the original in-app version silently degraded to
 * a single usable slot and no test could see it.
 *
 * Keying is (net_id, msg_type, src, seq24). TTL is deliberately not part of the
 * key: the whole point is to recognise a relayed copy of a message already
 * seen, and a relayed copy differs from the original only by its hop count.
 */

#ifndef TRON_MESH_DEDUPE_H
#define TRON_MESH_DEDUPE_H

#include <stdint.h>

#include "tron_mesh_packet.h"

/* 16 entries covers a bench population with headroom. Aging at 10 s must
   outlive the longest relay cascade, or the tail of a flood is treated as new
   traffic and re-floods. */
#ifndef TRON_MESH_DEDUPE_SIZE
#define TRON_MESH_DEDUPE_SIZE     16u
#endif

#ifndef TRON_MESH_DEDUPE_TTL_MS
#define TRON_MESH_DEDUPE_TTL_MS   10000u
#endif

typedef struct tron_mesh_dedupe_entry {
    uint8_t valid;
    uint8_t net_id;
    uint8_t msg_type;
    uint16_t src;
    uint32_t seq24;
    uint32_t expires_at_ms;
} tron_mesh_dedupe_entry_t;

typedef struct tron_mesh_dedupe {
    tron_mesh_dedupe_entry_t entries[TRON_MESH_DEDUPE_SIZE];
} tron_mesh_dedupe_t;

/* Wrap-safe "now >= deadline" on the low 32 bits of a millisecond clock. */
int tron_mesh_time_reached(uint32_t now, uint32_t deadline);

void tron_mesh_dedupe_reset(tron_mesh_dedupe_t *cache);

/* Returns 1 if this packet was already seen (caller should drop it), 0 if it is
   new, in which case it is recorded. Expired entries are reclaimed in passing. */
int tron_mesh_dedupe_seen_or_insert(tron_mesh_dedupe_t *cache,
                                    const tron_mesh_packet_t *packet,
                                    uint32_t now_ms);

/* Live entry count. Exposed so tests can assert the cache actually uses its
   capacity rather than thrashing one slot. */
unsigned int tron_mesh_dedupe_slots_used(const tron_mesh_dedupe_t *cache);

#endif /* TRON_MESH_DEDUPE_H */
