/* Audit-only pre-fix binding fixture.  It is never compiled into firmware or
 * either Phase 4 test binary. */
#include "routed_cycle.h"

LOCAL void routed_mesh_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;
    for (;;) {
    }
}
