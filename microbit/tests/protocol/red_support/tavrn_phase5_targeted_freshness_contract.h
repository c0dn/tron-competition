#ifndef TAVRN_PHASE5_TARGETED_FRESHNESS_CONTRACT_H
#define TAVRN_PHASE5_TARGETED_FRESHNESS_CONTRACT_H

/*
 * Step 5f Stage-0 API contract.  Targeted state belongs to maintenance; route
 * truth belongs to the router/AODV core; tracked low tokens remain link-owned;
 * and GTT remains the source of canonical evidence.  RED definitions are
 * deliberately inert, but use those real owners so the test cannot pass by
 * implementing a detached model.
 */
#include <stdint.h>

#include "tavrn_gtt.h"
#include "tavrn_link_v2.h"
#include "tavrn_maintenance.h"
#include "tavrn_router.h"

#if !defined(TAVRN_MAINTENANCE_TARGETED_FRESHNESS_API)

#define TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY 4u
#define TAVRN_TARGETED_FRESHNESS_DEDUPE_CAPACITY 16u
#define TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY 4u
#define TAVRN_TARGETED_RESPONSE_CONTEXT_INDEX_BASE 0x80u

typedef enum tavrn_targeted_freshness_status {
    TAVRN_TARGETED_FRESHNESS_OK = 0,
    TAVRN_TARGETED_FRESHNESS_BUSY,
    TAVRN_TARGETED_FRESHNESS_DEFERRED,
    TAVRN_TARGETED_FRESHNESS_DUPLICATE,
    TAVRN_TARGETED_FRESHNESS_DROPPED,
    TAVRN_TARGETED_FRESHNESS_INVALID,
    TAVRN_TARGETED_FRESHNESS_UNAVAILABLE,
} tavrn_targeted_freshness_status_t;

typedef enum tavrn_targeted_freshness_work_kind {
    TAVRN_TARGETED_WORK_NONE = 0,
    TAVRN_TARGETED_WORK_LOCAL_REQUEST,
    TAVRN_TARGETED_WORK_REQUEST_RELAY,
    TAVRN_TARGETED_WORK_TARGET_RESPONSE,
    TAVRN_TARGETED_WORK_INTERMEDIARY_RESPONSE,
    TAVRN_TARGETED_WORK_RESPONSE_RELAY,
} tavrn_targeted_freshness_work_kind_t;

typedef enum tavrn_targeted_freshness_stage {
    TAVRN_TARGETED_STAGE_NONE = 0,
    TAVRN_TARGETED_STAGE0_READY,
    TAVRN_TARGETED_STAGE0_WAIT_RESPONSE,
    TAVRN_TARGETED_STAGE1_READY,
    TAVRN_TARGETED_STAGE_CANCELED_TOMBSTONE,
} tavrn_targeted_freshness_stage_t;

typedef enum tavrn_targeted_freshness_low_work {
    TAVRN_TARGETED_LOW_CUSTODY = 0,
    TAVRN_TARGETED_LOW_BOOTSTRAP,
} tavrn_targeted_freshness_low_work_t;

typedef struct tavrn_targeted_freshness_context_snapshot {
    tavrn_adva_t origin;
    tavrn_adva_t subject;
    tavrn_direct_peer_t receiver;
    tavrn_targeted_freshness_work_kind_t work_kind;
    tavrn_targeted_freshness_stage_t stage;
    uint32_t obligation_started_ms;
    uint32_t obligation_deadline_ms;
    uint32_t not_before_ms;
    uint16_t node_sequence;
    uint16_t token;
    uint8_t request_bucket;
    uint8_t response_bucket;
    uint8_t queued;
    uint8_t in_flight;
    uint8_t valid;
} tavrn_targeted_freshness_context_snapshot_t;

typedef struct tavrn_targeted_freshness_snapshot {
    tavrn_targeted_freshness_context_snapshot_t
        contexts[TAVRN_TARGETED_FRESHNESS_CONTEXT_CAPACITY];
    tavrn_targeted_freshness_context_snapshot_t
        response_contexts[TAVRN_TARGETED_RESPONSE_OBLIGATION_CAPACITY];
    uint16_t next_node_sequence;
    uint16_t reservation_frontier;
    uint8_t active_contexts;
    uint8_t active_response_contexts;
    uint8_t dedupe_live;
    uint8_t ordinary_pending;
} tavrn_targeted_freshness_snapshot_t;

typedef struct tavrn_targeted_freshness_action {
    tavrn_validated_control_t control;
    uint16_t token;
    uint8_t context_index;
    uint8_t enqueued;
} tavrn_targeted_freshness_action_t;

/* `control_event` is the real copied link RX event.  The implementation derives
 * route and GTT evidence from the owners rather than accepting test-provided
 * booleans or scalar deadlines. */
typedef struct tavrn_targeted_freshness_rx_input {
    tavrn_rx_control_event_t control_event;
} tavrn_targeted_freshness_rx_input_t;

tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_begin_stage0(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, const tavrn_adva_t *subject,
    uint32_t now_ms, tavrn_targeted_freshness_action_t *action_out);
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_receive(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt,
    const tavrn_targeted_freshness_rx_input_t *input, uint32_t now_ms,
    tavrn_targeted_freshness_action_t *action_out);
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_owner_tick(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, uint32_t now_ms,
    tavrn_targeted_freshness_action_t *action_out);
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_revalidate(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, const tavrn_adva_t *subject,
    uint32_t now_ms);
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_terminal(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, const ble_mesh_sched_event_t *event,
    uint32_t now_ms);
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_cancel_subject(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, tavrn_gtt_t *gtt, const tavrn_adva_t *subject,
    uint32_t now_ms);
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_evicted(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router,
    tavrn_link_v2_t *link, uint16_t token, uint32_t now_ms);
/* Host-test-only cursor setup operates on maintenance-owned high-token state;
 * it exists solely to test wrap without creating another token domain. */
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_seed_high_token(
    tavrn_maintenance_t *maintenance, uint16_t next_token);
tavrn_targeted_freshness_status_t tavrn_maintenance_targeted_snapshot(
    const tavrn_maintenance_t *maintenance, const tavrn_router_t *router,
    const tavrn_link_v2_t *link, const tavrn_gtt_t *gtt,
    tavrn_targeted_freshness_snapshot_t *snapshot_out);
tavrn_targeted_freshness_status_t tavrn_link_v2_reserve_targeted_low_token(
    tavrn_link_v2_t *link, tavrn_targeted_freshness_low_work_t work,
    uint16_t *token_out);

#endif /* production targeted-freshness API absent */

#if defined(TAVRN_PHASE5_TARGETED_RED_MODE)
#define phase5_targeted_begin_stage0 phase5_targeted_red_begin_stage0
#define phase5_targeted_receive phase5_targeted_red_receive
#define phase5_targeted_owner_tick phase5_targeted_red_owner_tick
#define phase5_targeted_revalidate phase5_targeted_red_revalidate
#define phase5_targeted_terminal phase5_targeted_red_terminal
#define phase5_targeted_cancel_subject phase5_targeted_red_cancel_subject
#define phase5_targeted_evicted phase5_targeted_red_evicted
#define phase5_targeted_seed_high_token phase5_targeted_red_seed_high_token
#define phase5_targeted_snapshot phase5_targeted_red_snapshot
#define phase5_targeted_reserve_low_token phase5_targeted_red_reserve_low_token

tavrn_targeted_freshness_status_t phase5_targeted_red_begin_stage0(
    tavrn_maintenance_t *, tavrn_router_t *, tavrn_link_v2_t *, tavrn_gtt_t *,
    const tavrn_adva_t *, uint32_t, tavrn_targeted_freshness_action_t *);
tavrn_targeted_freshness_status_t phase5_targeted_red_receive(
    tavrn_maintenance_t *, tavrn_router_t *, tavrn_link_v2_t *, tavrn_gtt_t *,
    const tavrn_targeted_freshness_rx_input_t *, uint32_t,
    tavrn_targeted_freshness_action_t *);
tavrn_targeted_freshness_status_t phase5_targeted_red_owner_tick(
    tavrn_maintenance_t *, tavrn_router_t *, tavrn_link_v2_t *, tavrn_gtt_t *,
    uint32_t, tavrn_targeted_freshness_action_t *);
tavrn_targeted_freshness_status_t phase5_targeted_red_revalidate(
    tavrn_maintenance_t *, tavrn_router_t *, tavrn_link_v2_t *, tavrn_gtt_t *,
    const tavrn_adva_t *, uint32_t);
tavrn_targeted_freshness_status_t phase5_targeted_red_terminal(
    tavrn_maintenance_t *, tavrn_router_t *, tavrn_link_v2_t *,
    const ble_mesh_sched_event_t *, uint32_t);
tavrn_targeted_freshness_status_t phase5_targeted_red_cancel_subject(
    tavrn_maintenance_t *, tavrn_router_t *, tavrn_link_v2_t *, tavrn_gtt_t *,
    const tavrn_adva_t *, uint32_t);
tavrn_targeted_freshness_status_t phase5_targeted_red_evicted(
    tavrn_maintenance_t *, tavrn_router_t *, tavrn_link_v2_t *, uint16_t,
    uint32_t);
tavrn_targeted_freshness_status_t phase5_targeted_red_seed_high_token(
    tavrn_maintenance_t *, uint16_t);
tavrn_targeted_freshness_status_t phase5_targeted_red_snapshot(
    const tavrn_maintenance_t *, const tavrn_router_t *, const tavrn_link_v2_t *,
    const tavrn_gtt_t *, tavrn_targeted_freshness_snapshot_t *);
tavrn_targeted_freshness_status_t phase5_targeted_red_reserve_low_token(
    tavrn_link_v2_t *, tavrn_targeted_freshness_low_work_t, uint16_t *);
#else
#define phase5_targeted_begin_stage0 tavrn_maintenance_targeted_begin_stage0
#define phase5_targeted_receive tavrn_maintenance_targeted_receive
#define phase5_targeted_owner_tick tavrn_maintenance_targeted_owner_tick
#define phase5_targeted_revalidate tavrn_maintenance_targeted_revalidate
#define phase5_targeted_terminal tavrn_maintenance_targeted_terminal
#define phase5_targeted_cancel_subject tavrn_maintenance_targeted_cancel_subject
#define phase5_targeted_evicted tavrn_maintenance_targeted_evicted
#define phase5_targeted_seed_high_token tavrn_maintenance_targeted_seed_high_token
#define phase5_targeted_snapshot tavrn_maintenance_targeted_snapshot
#define phase5_targeted_reserve_low_token tavrn_link_v2_reserve_targeted_low_token
#endif

#endif /* TAVRN_PHASE5_TARGETED_FRESHNESS_CONTRACT_H */
