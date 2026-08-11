#ifndef TAVRN_PHASE5_RREQ_VERIFICATION_CONTRACT_H
#define TAVRN_PHASE5_RREQ_VERIFICATION_CONTRACT_H

/*
 * Step 5f retained-hop/full-diameter contract.  Maintenance retains the
 * verification context; AODV remains the only request-ID/action owner; and
 * the router/link path owns physical admission and completion.  The RED
 * backend below is intentionally inert.  GREEN may use only the production
 * API guarded by TAVRN_MAINTENANCE_RREQ_VERIFICATION_API.
 */
#include <stdint.h>

#include "aodv_core.h"
#include "tavrn_gtt.h"
#include "tavrn_link_v2.h"
#include "tavrn_maintenance.h"
#include "tavrn_router.h"

#if !defined(TAVRN_MAINTENANCE_RREQ_VERIFICATION_API)

typedef enum tavrn_rreq_verification_status {
    TAVRN_RREQ_VERIFICATION_OK = 0,
    TAVRN_RREQ_VERIFICATION_BUSY,
    TAVRN_RREQ_VERIFICATION_RATE_DEFERRED,
    TAVRN_RREQ_VERIFICATION_IGNORED,
    TAVRN_RREQ_VERIFICATION_CANCELED,
    TAVRN_RREQ_VERIFICATION_INVALID,
    TAVRN_RREQ_VERIFICATION_UNAVAILABLE,
} tavrn_rreq_verification_status_t;

typedef enum tavrn_rreq_verification_purpose {
    TAVRN_RREQ_VERIFICATION_PURPOSE_NONE = 0,
    TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE1_RETAINED_HOP,
    TAVRN_RREQ_VERIFICATION_PURPOSE_STAGE2_FULL_DIAMETER,
} tavrn_rreq_verification_purpose_t;

typedef enum tavrn_rreq_verification_stage {
    TAVRN_RREQ_VERIFICATION_STAGE_NONE = 0,
    TAVRN_RREQ_VERIFICATION_STAGE1_READY,
    TAVRN_RREQ_VERIFICATION_STAGE1_WAIT_RESPONSE,
    TAVRN_RREQ_VERIFICATION_STAGE2_READY,
    TAVRN_RREQ_VERIFICATION_STAGE2_WAIT_RESPONSE,
    TAVRN_RREQ_VERIFICATION_WAIT_DIRECT_DEADLINE,
    TAVRN_RREQ_VERIFICATION_DEPARTED,
    TAVRN_RREQ_VERIFICATION_DEPARTURE_DEFERRED,
} tavrn_rreq_verification_stage_t;

typedef enum tavrn_rreq_verification_terminal_kind {
    TAVRN_RREQ_VERIFICATION_TERMINAL_TX_DONE = 0,
    TAVRN_RREQ_VERIFICATION_TERMINAL_TX_FAILED,
    TAVRN_RREQ_VERIFICATION_TERMINAL_EVICTED,
    TAVRN_RREQ_VERIFICATION_TERMINAL_LOCAL_NOT_ATTEMPTED,
} tavrn_rreq_verification_terminal_kind_t;

typedef struct tavrn_rreq_verification_action {
    /* Exact copied AODV action consumed by the existing router/link path. */
    aodv_action_t aodv_action;
    aodv_rreq_attempt_t attempt;
    tavrn_adva_t subject;
    uint16_t token;
    tavrn_rreq_verification_purpose_t purpose;
    uint8_t enqueued;
} tavrn_rreq_verification_action_t;

/* This is the callback fact produced by the tracked router/link RREQ action,
 * not a synthetic lifecycle or telemetry record.  The implementation must
 * validate every discriminator against the retained production context. */
typedef struct tavrn_rreq_verification_completion {
    aodv_rreq_attempt_t attempt;
    uint16_t token;
    tavrn_rreq_verification_purpose_t purpose;
    tavrn_rreq_verification_terminal_kind_t kind;
    uint8_t completed_channel_mask;
} tavrn_rreq_verification_completion_t;

typedef struct tavrn_rreq_verification_context_snapshot {
    tavrn_adva_t subject;
    uint32_t expected_gtt_revision;
    uint32_t response_deadline_ms;
    uint32_t direct_evidence_deadline_ms;
    uint32_t verification_deadline_ms;
    uint16_t stage1_request_id;
    uint16_t stage2_request_id;
    uint16_t token;
    uint8_t retained_hop;
    uint8_t direct_subject;
    uint8_t valid;
    tavrn_rreq_verification_stage_t stage;
} tavrn_rreq_verification_context_snapshot_t;

typedef struct tavrn_rreq_verification_snapshot {
    tavrn_rreq_verification_context_snapshot_t
        contexts[TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY];
    uint32_t rreq_rate_deferrals;
    uint32_t checked_departures;
    uint8_t active_contexts;
    uint8_t ordinary_pending_data_created;
    uint8_t ordinary_inner_retry_created;
    uint8_t ordinary_smart_ttl_fallback_created;
    uint8_t expiry_tc_or_leave_created;
} tavrn_rreq_verification_snapshot_t;

tavrn_rreq_verification_status_t tavrn_maintenance_verification_owner_tick(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, uint32_t now_ms,
    tavrn_rreq_verification_action_t *action_out);
tavrn_rreq_verification_status_t tavrn_maintenance_verification_terminal(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link,
    const tavrn_rreq_verification_completion_t *completion, uint32_t now_ms);
tavrn_rreq_verification_status_t tavrn_maintenance_verification_snapshot(
    const tavrn_maintenance_t *maintenance, const tavrn_router_t *router,
    const tavrn_link_v2_t *link, const tavrn_gtt_t *gtt,
    tavrn_rreq_verification_snapshot_t *snapshot_out);

#endif /* production API absent */

#if defined(TAVRN_PHASE5_RREQ_VERIFICATION_RED_MODE)
#define phase5_rreq_owner_tick phase5_rreq_red_owner_tick
#define phase5_rreq_terminal phase5_rreq_red_terminal
#define phase5_rreq_snapshot phase5_rreq_red_snapshot

tavrn_rreq_verification_status_t phase5_rreq_red_owner_tick(
    tavrn_maintenance_t *, tavrn_router_t *, tavrn_link_v2_t *, tavrn_gtt_t *,
    uint32_t, tavrn_rreq_verification_action_t *);
tavrn_rreq_verification_status_t phase5_rreq_red_terminal(
    tavrn_maintenance_t *, tavrn_router_t *, tavrn_link_v2_t *,
    const tavrn_rreq_verification_completion_t *, uint32_t);
tavrn_rreq_verification_status_t phase5_rreq_red_snapshot(
    const tavrn_maintenance_t *, const tavrn_router_t *, const tavrn_link_v2_t *,
    const tavrn_gtt_t *, tavrn_rreq_verification_snapshot_t *);
#else
#define phase5_rreq_owner_tick tavrn_maintenance_verification_owner_tick
#define phase5_rreq_terminal tavrn_maintenance_verification_terminal
#define phase5_rreq_snapshot tavrn_maintenance_verification_snapshot
#endif

#endif /* TAVRN_PHASE5_RREQ_VERIFICATION_CONTRACT_H */
