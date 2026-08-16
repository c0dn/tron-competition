#ifndef MIND_ROOT_COORDINATOR_H
#define MIND_ROOT_COORDINATOR_H

/* Production Layer-7 root orchestration.  The routed binding supplies copied
 * topology, SID8 resolution, final DATA, logger, UI, and generic-submit seams;
 * this module never reads or mutates TAVRN/GTT storage directly. */

#include <stdint.h>

#include "mind_event_forwarder.h"
#include "mind_log.h"
#include "mind_root_inbox.h"

typedef enum mind_root_coordinator_submit_status {
    MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED = 0,
    MIND_ROOT_COORDINATOR_SUBMIT_QUEUED,
    MIND_ROOT_COORDINATOR_SUBMIT_BUSY,
    MIND_ROOT_COORDINATOR_SUBMIT_NOT_READY,
    MIND_ROOT_COORDINATOR_SUBMIT_INVALID,
} mind_root_coordinator_submit_status_t;

typedef struct mind_root_coordinator_token {
    mind_root_submission_t submission;
    uint32_t sequence;
    uint8_t valid;
} mind_root_coordinator_token_t;

typedef struct mind_root_coordinator_request {
    tron_application_data_t data;
    mind_root_coordinator_token_t token;
} mind_root_coordinator_request_t;

typedef int (*mind_root_coordinator_snapshot_fn)(
    void *context, uint32_t now_ms, routed_cycle_gtt_snapshot_t *snapshot_out);
typedef uint8_t (*mind_root_coordinator_sid8_ready_fn)(void *context);
typedef mind_root_resolver_status_t (*mind_root_coordinator_outgoing_resolve_fn)(
    void *context, const tavrn_adva_t *canonical, tavrn_logical_id_t *sid8_out);
typedef mind_root_resolver_status_t (*mind_root_coordinator_incoming_resolve_fn)(
    void *context, uint8_t sid8, uint32_t now_ms, tavrn_adva_t *canonical_out);
typedef int (*mind_root_coordinator_command_peek_fn)(
    void *context, mind_command_attempt_t *attempt_out);
typedef int (*mind_root_coordinator_command_consume_fn)(void *context);
typedef int (*mind_root_coordinator_final_peek_fn)(
    void *context, mind_root_inbox_entry_t *entry_out);
typedef int (*mind_root_coordinator_final_consume_fn)(void *context);
typedef int (*mind_root_coordinator_final_pin_observer_fn)(
    void *context, const tavrn_adva_t *observer);
typedef mind_application_ingress_take_status_t
    (*mind_root_coordinator_ingress_take_fn)(
        void *context, mind_application_ingress_event_t *event_out);
typedef tavrn_router_delivery_status_t
    (*mind_root_coordinator_final_publish_local_fn)(
        void *context, const mind_application_wire_record_t *record,
        const tavrn_adva_t *local_observer, uint32_t now_ms);
typedef int (*mind_root_coordinator_log_reserve_fn)(
    void *context, uint8_t count, mind_log_reservation_t *reservation_out);
typedef int (*mind_root_coordinator_log_commit_fn)(
    void *context, mind_log_reservation_t *reservation,
    const mind_log_record_t *records);
typedef void (*mind_root_coordinator_log_cancel_fn)(
    void *context, mind_log_reservation_t *reservation);
typedef void (*mind_root_coordinator_ui_publish_fn)(
    void *context, uint8_t local_active, uint8_t active_roots);
typedef mind_root_coordinator_submit_status_t
    (*mind_root_coordinator_generic_submit_fn)(
        void *context, const tron_application_data_t *data, uint32_t now_ms);

typedef struct mind_root_coordinator_operations {
    void *context;
    mind_root_coordinator_snapshot_fn snapshot;
    mind_root_coordinator_sid8_ready_fn sid8_ready;
    mind_root_coordinator_outgoing_resolve_fn resolve_outgoing;
    mind_root_coordinator_incoming_resolve_fn resolve_incoming;
    mind_root_coordinator_command_peek_fn command_peek;
    mind_root_coordinator_command_consume_fn command_consume;
    mind_root_coordinator_final_peek_fn final_peek;
    mind_root_coordinator_final_consume_fn final_consume;
    mind_root_coordinator_final_pin_observer_fn final_pin_observer;
    mind_root_coordinator_ingress_take_fn ingress_take;
    mind_root_coordinator_final_publish_local_fn final_publish_local;
    mind_root_coordinator_log_reserve_fn log_reserve;
    mind_root_coordinator_log_commit_fn log_commit;
    mind_root_coordinator_log_cancel_fn log_cancel;
    mind_root_coordinator_ui_publish_fn ui_publish;
    mind_root_coordinator_generic_submit_fn generic_submit;
} mind_root_coordinator_operations_t;

typedef struct mind_root_coordinator_counters {
    uint32_t invalid_completion;
    uint32_t invalid_submit;
    uint32_t final_rejected;
    uint32_t final_resolver_rejected;
    uint32_t final_logger_busy;
} mind_root_coordinator_counters_t;

typedef struct mind_root_coordinator {
    mind_root_plane_t plane;
    mind_event_forwarder_t events;
    mind_root_coordinator_operations_t operations;
    mind_root_coordinator_token_t pending;
    tron_application_data_t pending_data;
    mind_root_coordinator_counters_t counters;
    uint32_t next_token_sequence;
    uint8_t node_number;
    uint8_t initialized;
} mind_root_coordinator_t;

/* Every operation is mandatory: a partial binding cannot silently bypass an
 * ownership boundary. */
int mind_root_coordinator_init(mind_root_coordinator_t *coordinator,
                               const tavrn_adva_t *local_adva,
                               uint16_t router_boot_nonce,
                               uint8_t node_number,
                               const mind_root_coordinator_operations_t *operations);

/* Services copied commands/final data/topology and prepares at most one opaque
 * generic DATA request.  A pending request blocks a second submission until its
 * exact token is completed. */
int mind_root_coordinator_prepare(mind_root_coordinator_t *coordinator,
                                  uint32_t now_ms,
                                  mind_root_coordinator_request_t *request_out);

/* Invokes the injected generic submit seam once for this exact prepared token.
 * It synchronously records accepted/BUSY ownership before releasing the token. */
mind_root_coordinator_submit_status_t mind_root_coordinator_submit(
    mind_root_coordinator_t *coordinator,
    const mind_root_coordinator_token_t *token,
    const tron_application_data_t *data, uint32_t now_ms);

/* Available to bindings that need to complete an exact retained submission
 * separately.  Wrong/stale tokens preserve the pending owner unchanged. */
int mind_root_coordinator_complete(mind_root_coordinator_t *coordinator,
                                   const mind_root_coordinator_token_t *token,
                                   mind_root_coordinator_submit_status_t status,
                                   uint32_t now_ms);

const mind_root_coordinator_token_t *mind_root_coordinator_pending(
    const mind_root_coordinator_t *coordinator);

#endif /* MIND_ROOT_COORDINATOR_H */
