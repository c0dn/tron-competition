#ifndef MIND_EVENT_FORWARDER_H
#define MIND_EVENT_FORWARDER_H

/* Bounded Layer-7 fanout for one first-seen direct wearable observation.  The
 * immutable target snapshot is application-owned and never follows later root
 * additions.  TAVRN sees only the eventual opaque generic DATA submission. */

#include <stdint.h>

#include "mind_application_ingress.h"
#include "mind_root_inbox.h"
#include "mind_root_plane.h"

#define MIND_APP_EVENT_CAPACITY 16u

typedef enum mind_event_target_state {
    MIND_EVENT_TARGET_PENDING = 0,
    MIND_EVENT_TARGET_COMPLETE,
    MIND_EVENT_TARGET_CANCELLED,
} mind_event_target_state_t;

typedef struct mind_event_target {
    tavrn_adva_t canonical_adva;
    uint8_t resolver_blocked;
    uint8_t resolver_failed_closed;
    mind_event_target_state_t state;
} mind_event_target_t;

typedef struct mind_event_item {
    mind_application_wire_record_t report;
    mind_event_target_t targets[MIND_APP_ROOT_CAPACITY];
    uint32_t observed_at_ms;
    uint8_t target_count;
    /* Target rotation belongs to this immutable event snapshot.  A blocked
     * target in one retained event must not select a target for another. */
    uint8_t target_cursor;
    uint8_t occupied;
} mind_event_item_t;

typedef struct mind_event_forwarder_counters {
    uint32_t rootless_drop;
    uint32_t event_capacity_busy;
    uint32_t local_busy;
    uint32_t local_published;
    uint32_t remote_handoff;
    uint32_t remote_busy;
    uint32_t resolver_unknown;
    uint32_t resolver_colliding;
    uint32_t resolver_reserved;
    uint32_t resolver_invalid;
    uint32_t cancelled_targets;
    uint32_t completed_events;
} mind_event_forwarder_counters_t;

typedef struct mind_event_forwarder {
    mind_event_item_t events[MIND_APP_EVENT_CAPACITY];
    mind_event_forwarder_counters_t counters;
    tavrn_adva_t local_adva;
    /* Global event rotation shares the sole generic-submit slot fairly.  Each
     * retained item owns its own target cursor above. */
    uint8_t event_cursor;
} mind_event_forwarder_t;

void mind_event_forwarder_init(mind_event_forwarder_t *forwarder,
                               const tavrn_adva_t *local_adva);

/* Takes at most one ingress entry only after a free event item exists.  A
 * rootless event is intentionally consumed without allocating a future target. */
typedef mind_application_ingress_take_status_t (*mind_event_ingress_take_fn)(
    void *context, mind_application_ingress_event_t *event_out);
int mind_event_forwarder_consume_ingress(
    mind_event_forwarder_t *forwarder, void *context,
    mind_event_ingress_take_fn ingress_take, const mind_root_plane_t *plane,
    uint32_t now_ms);

/* Local-root targets publish into the shared final inbox and never self-route.
 * BUSY preserves the exact target. */
typedef tavrn_router_delivery_status_t (*mind_event_local_publish_fn)(
    void *context, const mind_application_wire_record_t *record,
    const tavrn_adva_t *local_observer, uint32_t now_ms);
void mind_event_forwarder_service_local(mind_event_forwarder_t *forwarder,
                                        void *context,
                                        mind_event_local_publish_fn publish_local,
                                        uint32_t now_ms);

/* Select one remote report owner from immutable target snapshots.  urgent=1
 * selects non-heartbeats and urgent=0 selects heartbeat reports. */
int mind_event_forwarder_prepare_submission(
    mind_event_forwarder_t *forwarder, uint8_t urgent,
    mind_root_submission_t *submission_out);
int mind_event_forwarder_submission_resolved(
    mind_event_forwarder_t *forwarder, const mind_root_submission_t *submission,
    mind_root_resolver_status_t status);
void mind_event_forwarder_submission_complete(
    mind_event_forwarder_t *forwarder, const mind_root_submission_t *submission,
    uint8_t accepted);

/* A later successful topology observation is the retry signal for UNKNOWN and
 * COLLIDING targets.  Only accepted newer OFF/departure clears matching roots. */
void mind_event_forwarder_note_topology_observed(mind_event_forwarder_t *forwarder);
void mind_event_forwarder_clear_target(mind_event_forwarder_t *forwarder,
                                       const tavrn_adva_t *canonical_adva);

#endif /* MIND_EVENT_FORWARDER_H */
