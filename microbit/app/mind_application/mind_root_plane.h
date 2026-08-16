#ifndef MIND_ROOT_PLANE_H
#define MIND_ROOT_PLANE_H

/* Layer-7 runtime root state.  TAVRN provides only opaque DATA, copied
 * topology, and SID8 resolution to the integration binding. */

#include <stdint.h>

#include "mind_application_wire.h"
#include "mind_topology_adapter.h"
#include "tavrn_wire_v2.h"

#define MIND_APP_ROOT_CAPACITY 16u
#define MIND_APP_CAMPAIGN_CAPACITY 16u
#define MIND_APP_ROOT_ACK_CAPACITY 16u
#define MIND_APP_ROOT_INITIAL_DUE_MIN_MS 20u
#define MIND_APP_ROOT_INITIAL_DUE_MAX_MS 120u
#define MIND_APP_ROOT_SUBMIT_SPACING_MS 50u
#define MIND_APP_ROOT_ACK_RETRY_MS 3000u

typedef enum mind_root_apply_status {
    MIND_ROOT_APPLY_ACCEPTED = 0,
    MIND_ROOT_APPLY_DUPLICATE,
    MIND_ROOT_APPLY_CAPACITY_REJECTED,
    MIND_ROOT_APPLY_STALE,
    MIND_ROOT_APPLY_INVALID,
} mind_root_apply_status_t;

typedef struct mind_root_entry {
    tavrn_adva_t canonical_adva;
    mind_application_root_state_t current;
    uint16_t prior_nonce;
    uint8_t occupied;
    uint8_t local;
} mind_root_entry_t;

typedef struct mind_root_campaign_target {
    tavrn_adva_t canonical_adva;
    uint32_t due_at_ms;
    uint32_t last_submit_ms;
    uint8_t occupied;
    uint8_t completed;
    uint8_t rejected;
    uint8_t resolver_blocked;
    uint8_t resolver_failed_closed;
} mind_root_campaign_target_t;

typedef struct mind_root_ack_obligation {
    tavrn_adva_t canonical_adva;
    mind_application_root_ack_t ack;
    uint8_t occupied;
    uint8_t resolver_blocked;
    uint8_t resolver_failed_closed;
} mind_root_ack_obligation_t;

typedef struct mind_root_plane_counters {
    uint32_t capacity_rejected;
    uint32_t stale;
    uint32_t duplicate;
    uint32_t accepted;
    uint32_t departed_cleared;
    uint32_t snapshot_failures;
    uint32_t campaign_deferred;
    uint32_t resolver_unknown;
    uint32_t resolver_colliding;
    uint32_t resolver_reserved;
    uint32_t resolver_invalid;
    uint32_t ack_unmatched;
    uint32_t ack_late;
    uint32_t ack_queue_full;
    uint32_t submissions;
    uint32_t submission_busy;
    uint32_t root_state_submit_invoked;
} mind_root_plane_counters_t;

typedef struct mind_root_plane {
    mind_root_entry_t roots[MIND_APP_ROOT_CAPACITY];
    mind_root_campaign_target_t campaigns[MIND_APP_CAMPAIGN_CAPACITY];
    mind_root_ack_obligation_t acknowledgements[MIND_APP_ROOT_ACK_CAPACITY];
    mind_topology_adapter_t topology;
    mind_root_plane_counters_t counters;
    tavrn_adva_t local_adva;
    uint32_t last_root_state_invoked_ms;
    uint8_t local_index;
    uint8_t sid8_ready;
    uint8_t root_state_submission_seen;
} mind_root_plane_t;

typedef struct mind_root_plane_status {
    uint8_t active_roots;
    uint8_t announced;
    uint8_t acked;
    uint8_t rejected;
    uint8_t pending;
} mind_root_plane_status_t;

typedef enum mind_root_submission_kind {
    MIND_ROOT_SUBMISSION_NONE = 0,
    MIND_ROOT_SUBMISSION_ACK,
    MIND_ROOT_SUBMISSION_STATE,
    MIND_ROOT_SUBMISSION_EVENT,
} mind_root_submission_kind_t;

typedef struct mind_root_submission {
    tavrn_adva_t target;
    mind_application_wire_record_t record;
    uint8_t slot;
    /* EVENT uses slot for its retained event and target_slot for its immutable
     * root snapshot target.  ROOT_ACK/ROOT_STATE leave target_slot zero. */
    uint8_t target_slot;
    mind_root_submission_kind_t kind;
} mind_root_submission_t;

typedef enum mind_root_resolver_status {
    MIND_ROOT_RESOLVER_UNIQUE = 0,
    MIND_ROOT_RESOLVER_UNKNOWN,
    MIND_ROOT_RESOLVER_COLLIDING,
    MIND_ROOT_RESOLVER_RESERVED,
    MIND_ROOT_RESOLVER_INVALID,
} mind_root_resolver_status_t;

void mind_root_plane_init(mind_root_plane_t *plane,
                          const tavrn_adva_t *local_adva,
                          uint16_t router_boot_nonce);

mind_root_apply_status_t mind_root_plane_set_local(mind_root_plane_t *plane,
                                                    uint8_t active,
                                                    uint32_t now_ms);
mind_application_root_state_t mind_root_plane_local_state(
    const mind_root_plane_t *plane);
uint8_t mind_root_plane_local_active(const mind_root_plane_t *plane);

/* A successful copied snapshot updates only application copies.  Explicit
 * departed=true is the sole topology signal that clears retained root/campaign
 * history. */
int mind_root_plane_apply_topology(mind_root_plane_t *plane,
                                   const routed_cycle_gtt_snapshot_t *snapshot);
/* Testable/application-owned campaign admission seam used by the topology
 * adapter; it never reads or writes GTT state. */
void mind_root_plane_observe_target(mind_root_plane_t *plane,
                                    const tavrn_adva_t *identity,
                                    uint32_t now_ms);
void mind_root_plane_note_snapshot_failure(mind_root_plane_t *plane);
void mind_root_plane_set_sid8_ready(mind_root_plane_t *plane, uint8_t ready,
                                    uint32_t now_ms);

mind_root_apply_status_t mind_root_plane_receive_root_state(
    mind_root_plane_t *plane, const tavrn_adva_t *origin,
    const mind_application_root_state_t *state);
void mind_root_plane_receive_root_ack(mind_root_plane_t *plane,
                                      const tavrn_adva_t *origin,
                                      const mind_application_root_ack_t *ack);

/* Selects one Layer-7 owner for the existing generic application-submit
 * phase.  The caller resolves submission.target through the frozen SID8 seam
 * before constructing opaque transport DATA. */
int mind_root_plane_prepare_submission(mind_root_plane_t *plane,
                                        uint32_t now_ms,
                                        mind_root_submission_t *submission_out);
/* The unified application arbiter needs to interleave event reports between
 * ACK and ROOT_STATE work without weakening the root-plane's own ordering. */
int mind_root_plane_prepare_ack_submission(
    mind_root_plane_t *plane, uint32_t now_ms,
    mind_root_submission_t *submission_out);
int mind_root_plane_prepare_state_submission(
    mind_root_plane_t *plane, uint32_t now_ms,
    mind_root_submission_t *submission_out);
int mind_root_plane_submission_resolved(mind_root_plane_t *plane,
                                        const mind_root_submission_t *submission,
                                        mind_root_resolver_status_t status,
                                        uint32_t now_ms);
void mind_root_plane_note_incoming_resolver(mind_root_plane_t *plane,
                                            mind_root_resolver_status_t status);
/* Call immediately before every generic ROOT_STATE submit invocation.  This
 * throttles BUSY/NOT_READY attempts as well as successful handoffs. */
void mind_root_plane_submission_invoked(mind_root_plane_t *plane,
                                        const mind_root_submission_t *submission,
                                        uint32_t now_ms);
void mind_root_plane_submission_complete(mind_root_plane_t *plane,
                                         const mind_root_submission_t *submission,
                                         uint8_t accepted, uint32_t now_ms);

void mind_root_plane_status(const mind_root_plane_t *plane,
                            mind_root_plane_status_t *status_out);
uint8_t mind_root_plane_active_count(const mind_root_plane_t *plane);
/* Copies the current active Layer-7 root view.  It never exposes GTT storage
 * and is used once when an ingress event begins fanout. */
uint8_t mind_root_plane_snapshot_active_roots(
    const mind_root_plane_t *plane, tavrn_adva_t roots_out[MIND_APP_ROOT_CAPACITY]);

#endif /* MIND_ROOT_PLANE_H */
