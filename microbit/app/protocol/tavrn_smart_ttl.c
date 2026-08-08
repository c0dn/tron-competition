#include "tavrn_smart_ttl.h"

tavrn_smart_ttl_decision_t tavrn_smart_ttl_decide(
    const tavrn_gtt_snapshot_t *snapshot_or_null, uint8_t net_diameter)
{
    tavrn_smart_ttl_decision_t decision;
    uint16_t suggested_scope;

    decision.has_hint = 0u;
    decision.initial_scope = net_diameter;
    if (snapshot_or_null == NULL || snapshot_or_null->hop_count == 0u ||
        (snapshot_or_null->freshness != TAVRN_GTT_FRESHNESS_ACTIVE &&
         snapshot_or_null->freshness != TAVRN_GTT_FRESHNESS_SOFT_STALE)) {
        return decision;
    }
    suggested_scope = (uint16_t)snapshot_or_null->hop_count + 2u;
    decision.has_hint = 1u;
    decision.initial_scope = suggested_scope < net_diameter ?
        (uint8_t)suggested_scope : net_diameter;
    return decision;
}
