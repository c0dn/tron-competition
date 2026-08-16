#include "tavrn_esc.h"

#include <string.h>

static int time_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static int tombstone_is_purged(const tavrn_gtt_entry_t *entry, uint32_t now_ms)
{
    return entry->occupied != 0u && entry->departed != 0u &&
        time_due(now_ms, entry->departed_deadline_ms);
}

tavrn_esc_context_status_t tavrn_esc_resolve_sid8(
    const tavrn_gtt_t *gtt, uint8_t sid8, uint32_t now_ms,
    tavrn_esc_context_match_t *match_out)
{
    uint8_t index;
    uint8_t matches = 0u;
    tavrn_adva_t identity;

    if (match_out != NULL) {
        memset(match_out, 0, sizeof(*match_out));
        match_out->sid8 = sid8;
    }
    if (gtt == NULL || gtt->storage == NULL) {
        return TAVRN_ESC_CONTEXT_INVALID;
    }
    if (sid8 == 0u || sid8 == 0xffu) {
        return TAVRN_ESC_CONTEXT_RESERVED;
    }
    memset(&identity, 0, sizeof(identity));
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &gtt->storage->entries[index];

        if (entry->occupied == 0u || entry->departed != 0u ||
            tombstone_is_purged(entry, now_ms) || entry->identity.bytes[0] != sid8) {
            continue;
        }
        matches++;
        if (matches == 1u) {
            identity = entry->identity;
        }
    }
    if (matches == 0u) {
        return TAVRN_ESC_CONTEXT_UNKNOWN;
    }
    if (matches != 1u) {
        return TAVRN_ESC_CONTEXT_COLLIDING;
    }
    if (match_out != NULL) {
        match_out->identity = identity;
    }
    return TAVRN_ESC_CONTEXT_UNIQUE;
}
