#ifndef TAVRN_FULL_H
#define TAVRN_FULL_H

#include "tavrn_gtt.h"
#include "tavrn_esc.h"
#include "tavrn_router.h"

#define TAVRN_FULL_EXPIRY_API 1

typedef struct tavrn_full {
    tavrn_gtt_t *gtt;
} tavrn_full_t;

typedef enum tavrn_full_init_status {
    TAVRN_FULL_INIT_OK = 0,
    TAVRN_FULL_INIT_INVALID_ARGUMENT,
} tavrn_full_init_status_t;

typedef enum tavrn_full_application_mailbox_status {
    TAVRN_FULL_APPLICATION_MAILBOX_QUEUED = 0,
    TAVRN_FULL_APPLICATION_MAILBOX_PROCESSING,
    TAVRN_FULL_APPLICATION_MAILBOX_BUSY,
    TAVRN_FULL_APPLICATION_MAILBOX_EMPTY,
    TAVRN_FULL_APPLICATION_MAILBOX_READY,
    TAVRN_FULL_APPLICATION_MAILBOX_INVALID,
    TAVRN_FULL_APPLICATION_MAILBOX_UNAVAILABLE,
} tavrn_full_application_mailbox_status_t;

/* The application and mesh-owner tasks exchange copied values only.  Main
 * supplies the short critical sections around the take/publish transitions. */
typedef struct tavrn_full_application_mailbox {
    tavrn_gtt_application_command_t command;
    tavrn_gtt_application_command_result_t result;
    uint8_t command_pending;
    uint8_t owner_processing;
    uint8_t result_ready;
} tavrn_full_application_mailbox_t;

tavrn_full_init_status_t tavrn_full_init(tavrn_full_t *full, tavrn_gtt_t *gtt);
tavrn_router_augmentation_hooks_t tavrn_full_router_hooks(tavrn_full_t *full);
tavrn_esc_context_status_t tavrn_full_resolve_unique_sid8(
    const tavrn_full_t *full, const tavrn_adva_t *identity,
    tavrn_logical_id_t *sid8_out);
tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_init(
    tavrn_full_application_mailbox_t *mailbox);
tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_submit(
    tavrn_full_application_mailbox_t *mailbox,
    tavrn_gtt_application_command_t command);
tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_owner_take(
    tavrn_full_application_mailbox_t *mailbox,
    tavrn_gtt_application_command_t *command_out);
tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_owner_publish(
    tavrn_full_application_mailbox_t *mailbox,
    tavrn_gtt_application_command_result_t result);
tavrn_full_application_mailbox_status_t tavrn_full_application_mailbox_consumer_take(
    tavrn_full_application_mailbox_t *mailbox,
    tavrn_gtt_application_command_result_t *result_out);

#endif /* TAVRN_FULL_H */
