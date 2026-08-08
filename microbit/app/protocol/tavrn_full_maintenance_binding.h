#ifndef TAVRN_FULL_MAINTENANCE_BINDING_H
#define TAVRN_FULL_MAINTENANCE_BINDING_H

#include "tavrn_full.h"
#include "tavrn_maintenance.h"
#include "tavrn_mentorship.h"

#define TAVRN_FULL_MAINTENANCE_BINDING_API 1

typedef enum tavrn_full_maintenance_binding_status {
    TAVRN_FULL_MAINTENANCE_BINDING_OK = 0,
    TAVRN_FULL_MAINTENANCE_BINDING_INVALID,
    TAVRN_FULL_MAINTENANCE_BINDING_UNAVAILABLE,
} tavrn_full_maintenance_binding_status_t;

typedef struct tavrn_full_maintenance_binding_input {
    tavrn_router_t *router;
    tavrn_mentorship_t *mentorship;
    tavrn_maintenance_t *maintenance;
    tavrn_gtt_application_command_t application_command;
    uint8_t application_present;
} tavrn_full_maintenance_binding_input_t;

typedef struct tavrn_full_maintenance_binding_result {
    tavrn_full_maintenance_binding_status_t status;
    tavrn_maintenance_owner_pre_tick_result_t pre_tick;
    tavrn_gtt_application_command_result_t application_result;
    uint8_t application_present;
    aodv_status_t router_status;
    tavrn_router_phase_trace_t router_trace;
    tavrn_mentorship_status_t mentorship_status;
    tavrn_router_local_broadcast_status_t broadcast_snapshot_status;
    tavrn_router_local_broadcast_snapshot_t broadcast;
    tavrn_maintenance_status_t broadcast_observation_status;
    tavrn_maintenance_status_t activation_status;
    tavrn_maintenance_owner_post_tick_result_t post_tick;
} tavrn_full_maintenance_binding_result_t;

/* `result_out` is required caller-owned storage.  A NULL output is invalid and
 * leaves no caller state to mutate.  Every non-NULL invalid invocation fills
 * the complete invalid result before returning its status. */
tavrn_full_maintenance_binding_status_t tavrn_full_maintenance_binding_tick(
    tavrn_full_maintenance_binding_input_t input, uint32_t now_ms,
    tavrn_full_maintenance_binding_result_t *result_out);

#endif /* TAVRN_FULL_MAINTENANCE_BINDING_H */
