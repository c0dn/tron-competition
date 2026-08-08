#ifndef TAVRN_PHASE5_MAINTENANCE_CONTRACT_H
#define TAVRN_PHASE5_MAINTENANCE_CONTRACT_H

/*
 * Frozen Step 5a seam for the first FULL-only maintenance increment and its
 * Step 6.5 adaptive extension.
 *
 * Production will own this API in tavrn_maintenance.{h,c}.  Until that lands,
 * the RED backend deliberately provides only inert lifecycle plumbing around
 * the established router/GTT objects; it never supplies cadence, queueing,
 * HELLO RX, dedupe, or counters.
 */
#include <stdint.h>

#include "tavrn_gtt.h"
#include "tavrn_router.h"

#ifdef TAVRN_MAINTENANCE_API

#include "tavrn_maintenance.h"

#else

#define TAVRN_MAINTENANCE_PENDING_CAPACITY 1u

typedef char tavrn_phase5_pending_capacity_guard[
    (TAVRN_MAINTENANCE_PENDING_CAPACITY == 1u) ? 1 : -1];

typedef enum tavrn_maintenance_status {
    TAVRN_MAINTENANCE_OK = 0,
    TAVRN_MAINTENANCE_GATED,
    TAVRN_MAINTENANCE_BUSY,
    TAVRN_MAINTENANCE_RX_UNIQUE,
    TAVRN_MAINTENANCE_RX_DUPLICATE,
    TAVRN_MAINTENANCE_RX_REJECTED,
    TAVRN_MAINTENANCE_IGNORED,
    TAVRN_MAINTENANCE_INVALID,
} tavrn_maintenance_status_t;

typedef struct tavrn_maintenance_config {
    uint32_t hello_change_ms;
    uint32_t hello_stable_ms;
    /* The production timer object currently exposes float ratios.  The
     * maintenance boundary receives deterministic integer permille values. */
    uint16_t hello_alpha_permille;
    uint16_t hello_snap_permille;
    /* This will be sourced from timer.gtt_maintenance_ms by production. */
    uint32_t topology_sample_ms;
    uint32_t hello_dedupe_ms;
    uint16_t initial_node_sequence;
} tavrn_maintenance_config_t;

/* `pending` is a copied 0/1 gauge; every other member is monotonic.  Wire
 * malformed/foreign frames remain owned by the codec and do not increment
 * rx_rejected.  That counter is for maintenance-owned, fully decoded N=0
 * HELLO policy rejections such as a targeted ordinary HELLO. */
typedef struct tavrn_maintenance_counters {
    uint32_t due;
    uint32_t enqueued;
    uint32_t busy;
    uint32_t rx_unique;
    uint32_t rx_duplicate;
    uint32_t rx_rejected;
    uint32_t interval_advanced;
    uint32_t interval_snapped;
    uint32_t local_broadcast_suppressed;
    uint32_t topology_reset;
    uint32_t topology_unchanged;
    uint32_t liveness_sample;
    uint32_t liveness_floor_decayed;
    uint8_t pending;
} tavrn_maintenance_counters_t;

typedef struct tavrn_maintenance_snapshot {
    tavrn_validated_control_t pending_hello;
    uint32_t next_hello_due_ms;
    /* Sample cadence is sourced from gtt_maintenance_ms by production. */
    uint32_t next_topology_sample_ms;
    uint32_t current_interval_ms;
    /* `liveness_timeout_ms` is evaluated with the pre-decay floor; the stored
     * floor is the value retained for the following sample. */
    uint32_t liveness_floor_ms;
    uint32_t liveness_timeout_ms;
    uint16_t next_node_sequence;
    uint8_t direct_one_hop_count;
    uint8_t armed;
    uint8_t pending;
} tavrn_maintenance_snapshot_t;

typedef struct tavrn_maintenance {
    tavrn_router_t *router;
    tavrn_gtt_t *gtt;
    tavrn_maintenance_config_t config;
    tavrn_maintenance_counters_t counters;
    tavrn_maintenance_snapshot_t snapshot;
} tavrn_maintenance_t;

tavrn_maintenance_status_t tavrn_maintenance_init(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router, tavrn_gtt_t *gtt,
    const tavrn_maintenance_config_t *config);
/* Activation is legal only after the actual FULL mentorship handover has made
 * the router/link local identity SID8.  It arms `now + hello_change_ms`; it
 * neither sends nor retroactively catches up a HELLO. */
tavrn_maintenance_status_t tavrn_maintenance_activate(
    tavrn_maintenance_t *maintenance, uint32_t now_ms);
/* Tick owns the single local ordinary HELLO slot.  A due control is retained
 * byte-for-byte on BUSY/NO_SLOT and cadence moves only on enqueue success. */
tavrn_maintenance_status_t tavrn_maintenance_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms);
/* Call only after the real wire -> link -> router path has reported a copied
 * RX_CONTROL event.  It accepts only the ordinary SID8 N=0, untargeted,
 * one-hop HELLO shape and never calls a router dispatch/send entry. */
tavrn_maintenance_status_t tavrn_maintenance_handle_rx_control(
    tavrn_maintenance_t *maintenance,
    const tavrn_rx_control_event_t *control_event, uint32_t now_ms);
/* This is the one suppression-observation surface for a successfully accepted
 * locally originated broadcast that is not maintenance's own ordinary HELLO.
 * A production caller may aggregate copied router state/counters before it
 * reaches this surface; no radio or scheduler callback is implied. */
tavrn_maintenance_status_t tavrn_maintenance_observe_local_broadcast(
    tavrn_maintenance_t *maintenance, uint32_t accepted_at_ms);
tavrn_maintenance_status_t tavrn_maintenance_snapshot(
    const tavrn_maintenance_t *maintenance,
    tavrn_maintenance_snapshot_t *snapshot_out);
const tavrn_maintenance_counters_t *tavrn_maintenance_counters(
    const tavrn_maintenance_t *maintenance);

#endif /* TAVRN_MAINTENANCE_API */

#endif /* TAVRN_PHASE5_MAINTENANCE_CONTRACT_H */
