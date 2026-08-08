/* RED links the established AODV core while retaining public Phase 4
 * telemetry seams as deliberately nonfunctional test ports. */
#define aodv_core_submit_application_scoped_ex \
    tavrn_phase4_red_production_submit_application_scoped_ex
#define aodv_core_set_rreq_telemetry \
    tavrn_phase4_red_production_set_rreq_telemetry
#define aodv_core_report_rreq_link_enqueue \
    tavrn_phase4_red_production_report_rreq_link_enqueue

#include "../../../app/protocol/aodv_core.c"

#undef aodv_core_report_rreq_link_enqueue
#undef aodv_core_set_rreq_telemetry
#undef aodv_core_submit_application_scoped_ex
