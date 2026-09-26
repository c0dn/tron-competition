#ifdef TAVRN_GTT_H
#error "cycle-only contract must not pre-include tavrn_gtt.h"
#endif
#ifdef TAVRN_FULL_H
#error "cycle-only contract must not pre-include tavrn_full.h"
#endif
#ifdef ROUTED_FULL_TELEMETRY_H
#error "cycle-only contract must not pre-include routed_full_telemetry.h"
#endif

#include "routed_cycle.h"

#ifdef TAVRN_GTT_H
#error "routed_cycle.h must not include tavrn_gtt.h"
#endif
#ifdef TAVRN_FULL_H
#error "routed_cycle.h must not include tavrn_full.h"
#endif
#ifdef ROUTED_FULL_TELEMETRY_H
#error "routed_cycle.h must not include routed_full_telemetry.h"
#endif

int main(void)
{
    return sizeof(routed_cycle_t) == 0u || sizeof(aodv_core_t) == 0u;
}
