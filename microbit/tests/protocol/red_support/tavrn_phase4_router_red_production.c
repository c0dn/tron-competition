/* RED links the established router while retaining public Phase 4 trace seams
 * as deliberately nonfunctional test ports. */
#define tavrn_router_handle_scheduler_event_ex \
    tavrn_phase4_red_production_handle_scheduler_event_ex
#define tavrn_router_tick_ex tavrn_phase4_red_production_tick_ex
#define tavrn_router_submit_application_ex \
    tavrn_phase4_red_production_submit_application_ex
#define tavrn_router_dispatch_trace_ex \
    tavrn_phase4_red_production_dispatch_trace_ex
#define tavrn_router_service_link_ex \
    tavrn_phase4_red_production_service_link_ex

#include "../../../app/protocol/tavrn_router.c"

#undef tavrn_router_service_link_ex
#undef tavrn_router_dispatch_trace_ex
#undef tavrn_router_submit_application_ex
#undef tavrn_router_tick_ex
#undef tavrn_router_handle_scheduler_event_ex
