#ifndef TAVRN_SMART_TTL_H
#define TAVRN_SMART_TTL_H

#include <stdint.h>

#include "tavrn_gtt.h"

typedef struct tavrn_smart_ttl_decision {
    uint8_t has_hint;
    uint8_t initial_scope;
} tavrn_smart_ttl_decision_t;

/* This value-only policy neither owns nor mutates GTT or AODV state. */
tavrn_smart_ttl_decision_t tavrn_smart_ttl_decide(
    const tavrn_gtt_snapshot_t *snapshot_or_null, uint8_t net_diameter);

#endif /* TAVRN_SMART_TTL_H */
