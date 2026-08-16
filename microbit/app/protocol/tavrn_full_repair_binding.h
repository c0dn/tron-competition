#ifndef TAVRN_FULL_REPAIR_BINDING_H
#define TAVRN_FULL_REPAIR_BINDING_H

#include "tavrn_gtt.h"
#include "tavrn_maintenance.h"
#include "tavrn_repair.h"
#include "tavrn_router.h"

#define TAVRN_FULL_REPAIR_BINDING_API 1
#define TAVRN_FULL_REPAIR_HIGH_TOKEN_OWNER   0x5250u
#define TAVRN_FULL_REPAIR_HIGH_PURPOSE_SMART 0x0001u
#define TAVRN_FULL_REPAIR_HIGH_PURPOSE_FULL  0x0002u

typedef enum tavrn_full_repair_binding_status {
    TAVRN_FULL_REPAIR_BINDING_OK = 0,
    TAVRN_FULL_REPAIR_BINDING_BUSY,
    TAVRN_FULL_REPAIR_BINDING_IGNORED,
    TAVRN_FULL_REPAIR_BINDING_INVALID,
} tavrn_full_repair_binding_status_t;

typedef struct tavrn_full_repair_binding_config {
    tavrn_repair_t *repair;
    tavrn_router_t *router;
    tavrn_link_v2_t *link;
    aodv_core_t *aodv;
    tavrn_maintenance_t *maintenance;
    tavrn_gtt_t *gtt;
    uint8_t net_diameter;
    uint32_t path_discovery_ms;
    uint32_t repair_timeout_ms;
    uint32_t repair_cooldown_ms;
} tavrn_full_repair_binding_config_t;

typedef struct tavrn_full_repair_binding_tick_result {
    tavrn_repair_action_t action;
    tavrn_repair_terminal_t terminal;
    tavrn_repair_status_t repair_status;
    tavrn_router_reforward_status_t reforward_status;
    tavrn_maintenance_high_token_dispatch_result_t eviction;
    uint8_t action_present;
} tavrn_full_repair_binding_tick_result_t;

typedef struct tavrn_full_repair_binding {
    tavrn_repair_t *repair;
    tavrn_router_t *router;
    tavrn_link_v2_t *link;
    aodv_core_t *aodv;
    tavrn_maintenance_t *maintenance;
    tavrn_gtt_t *gtt;
    tavrn_repair_dependency_ops_t operations;
    uint8_t initialized;
} tavrn_full_repair_binding_t;

/* Initializes the one repair context and installs its generic router ports.
 * The caller retains all storage; no route, link, or token owner is cloned. */
tavrn_full_repair_binding_status_t tavrn_full_repair_binding_init(
    tavrn_full_repair_binding_t *binding,
    const tavrn_full_repair_binding_config_t *config);

/* Run one bounded repair-owner action.  Scheduler completions are delivered by
 * tavrn_maintenance_high_token_scheduler_event(), not directly here. */
tavrn_full_repair_binding_status_t tavrn_full_repair_binding_tick(
    tavrn_full_repair_binding_t *binding, uint32_t now_ms,
    tavrn_full_repair_binding_tick_result_t *result_out);

/* First in the FULL RREP interceptor chain. */
tavrn_router_control_intercept_status_t tavrn_full_repair_binding_receive_rrep(
    tavrn_full_repair_binding_t *binding,
    const tavrn_rx_control_event_t *control_event, uint32_t now_ms);

#endif /* TAVRN_FULL_REPAIR_BINDING_H */
