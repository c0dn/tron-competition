#ifndef TAVRN_ROUTER_H
#define TAVRN_ROUTER_H

#include <stdint.h>

#include "aodv_core.h"
#include "tavrn_link_v2.h"

#define TAVRN_ROUTER_EVIDENCE_CAPACITY 2u

typedef char tavrn_router_evidence_capacity_guard[
    (TAVRN_ROUTER_EVIDENCE_CAPACITY == 2u) ? 1 : -1];

/* The immediate transmitter is carried separately as a full direct peer.
 * These are the only additional Phase 3 semantic identity roles. */
typedef enum tavrn_router_evidence_role {
    TAVRN_ROUTER_EVIDENCE_ORIGIN = 0,
    TAVRN_ROUTER_EVIDENCE_ROUTE_DESTINATION,
    TAVRN_ROUTER_EVIDENCE_REPORTER,
} tavrn_router_evidence_role_t;

typedef enum tavrn_router_evidence_serial_kind {
    TAVRN_ROUTER_EVIDENCE_SERIAL_NONE = 0,
    TAVRN_ROUTER_EVIDENCE_SERIAL_NODE_SEQUENCE,
    TAVRN_ROUTER_EVIDENCE_SERIAL_DESTINATION_SEQUENCE,
} tavrn_router_evidence_serial_kind_t;

typedef enum tavrn_router_frame_admission {
    TAVRN_ROUTER_FRAME_COMMITTED = 0,
    TAVRN_ROUTER_FRAME_UNRESOLVED_DATA_CANDIDATE,
    TAVRN_ROUTER_FRAME_OVERHEARD_ADDRESSED_DATA,
    TAVRN_ROUTER_FRAME_MALFORMED,
    TAVRN_ROUTER_FRAME_AMBIGUOUS,
    TAVRN_ROUTER_FRAME_REJECTED,
} tavrn_router_frame_admission_t;

typedef struct tavrn_router_evidence_subject {
    tavrn_logical_id_t logical_id;
    /* Canonical identity is optional.  An unresolved SID16 remains a logical
     * fact and never authorizes synthesis of a full AdvA. */
    tavrn_adva_t canonical_identity;
    tavrn_router_evidence_role_t role;
    tavrn_router_evidence_serial_kind_t serial_kind;
    uint16_t serial;
    uint8_t hop_count;
    uint8_t hop_present;
    uint8_t canonical_present;
} tavrn_router_evidence_subject_t;

/* The callback receives only committed frame facts and must copy any retained
 * data before it returns.  No FULL_TAVRN type participates in this seam. */
typedef struct tavrn_router_observation {
    tavrn_wire_type_t frame_type;
    tavrn_direct_peer_t transmitter;
    uint8_t subject_count;
    tavrn_router_evidence_subject_t subjects[TAVRN_ROUTER_EVIDENCE_CAPACITY];
} tavrn_router_observation_t;

typedef struct tavrn_router_frame_observation {
    tavrn_router_frame_admission_t admission;
    tavrn_wire_type_t frame_type;
    tavrn_direct_peer_t transmitter;
    tavrn_hack_status_t hack_status;
    uint8_t subject_count;
    tavrn_router_evidence_subject_t subjects[TAVRN_ROUTER_EVIDENCE_CAPACITY];
} tavrn_router_frame_observation_t;

typedef struct tavrn_router_scope_hint {
    uint8_t has_hint;
    uint8_t initial_scope;
} tavrn_router_scope_hint_t;

typedef void (*tavrn_router_observation_fn)(
    void *context, const tavrn_router_observation_t *observation,
    uint32_t now_ms);
typedef tavrn_router_scope_hint_t (*tavrn_router_initial_scope_fn)(
    void *context, const tavrn_logical_id_t *destination,
    uint8_t full_scope, uint32_t now_ms);

typedef struct tavrn_router_augmentation_hooks {
    void *context;
    tavrn_router_observation_fn observe;
    tavrn_router_initial_scope_fn initial_scope;
} tavrn_router_augmentation_hooks_t;

typedef struct tavrn_router {
    tavrn_link_v2_t *link;
    aodv_core_t *aodv;
    tavrn_router_augmentation_hooks_t augmentation;
} tavrn_router_t;

typedef enum tavrn_router_init_status {
    TAVRN_ROUTER_INIT_OK = 0,
    TAVRN_ROUTER_INIT_INVALID_ARGUMENT,
} tavrn_router_init_status_t;

typedef enum tavrn_router_observe_status {
    TAVRN_ROUTER_OBSERVE_REPORTED = 0,
    TAVRN_ROUTER_OBSERVE_IGNORED,
    TAVRN_ROUTER_OBSERVE_INVALID,
} tavrn_router_observe_status_t;

typedef enum tavrn_router_scope_status {
    TAVRN_ROUTER_SCOPE_OK = 0,
    TAVRN_ROUTER_SCOPE_INVALID,
} tavrn_router_scope_status_t;

typedef enum tavrn_router_event_status {
    TAVRN_ROUTER_EVENT_OK = 0,
    TAVRN_ROUTER_EVENT_IGNORED,
    TAVRN_ROUTER_EVENT_BUSY,
    TAVRN_ROUTER_EVENT_INVALID,
} tavrn_router_event_status_t;

tavrn_router_init_status_t tavrn_router_init(
    tavrn_router_t *router, tavrn_link_v2_t *link, aodv_core_t *aodv,
    const tavrn_router_augmentation_hooks_t *augmentation_or_null);
tavrn_router_event_status_t tavrn_router_handle_scheduler_event(
    tavrn_router_t *router, const ble_mesh_sched_event_t *event,
    uint32_t now_ms);
tavrn_router_event_status_t tavrn_router_handle_link_event(
    tavrn_router_t *router, const tavrn_link_event_t *event, uint32_t now_ms);
aodv_status_t tavrn_router_submit_application(
    tavrn_router_t *router, const tron_application_data_t *data,
    uint32_t now_ms);
aodv_status_t tavrn_router_tick(tavrn_router_t *router, uint32_t now_ms);
tavrn_router_observe_status_t tavrn_router_observe_frame(
    tavrn_router_t *router,
    const tavrn_router_frame_observation_t *frame_observation,
    uint32_t now_ms);
tavrn_router_scope_status_t tavrn_router_initial_scope(
    const tavrn_router_t *router, const tavrn_logical_id_t *destination,
    uint8_t full_scope, uint32_t now_ms,
    tavrn_router_scope_hint_t *hint_out);

#endif /* TAVRN_ROUTER_H */
