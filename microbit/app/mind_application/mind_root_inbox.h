#ifndef MIND_ROOT_INBOX_H
#define MIND_ROOT_INBOX_H

/* Copied generic DATA reservation for every Layer-7 final DATA record.  The
 * router callback only reserves/copies before HACK acceptance; mesh ownership
 * performs semantic validation, mutation, or logger reservation later. */

#include <stdint.h>

#include "mind_application_wire.h"
#include "tavrn_router.h"

#define MIND_ROOT_INBOX_CAPACITY 8u

typedef struct mind_root_inbox_entry {
    tavrn_direct_peer_t transmitter;
    tavrn_link_data_t data;
    tavrn_adva_t observer;
    uint32_t delivered_at_ms;
    uint8_t path;
    uint8_t observer_pinned;
} mind_root_inbox_entry_t;

typedef enum mind_root_inbox_path {
    MIND_ROOT_INBOX_PATH_TAVRN = 0,
    MIND_ROOT_INBOX_PATH_LOCAL,
} mind_root_inbox_path_t;

typedef struct mind_root_inbox {
    mind_root_inbox_entry_t entries[MIND_ROOT_INBOX_CAPACITY];
    tavrn_direct_peer_t provisional_transmitter;
    tavrn_link_data_t provisional_data;
    tavrn_router_delivery_token_t provisional_token;
    uint8_t head;
    uint8_t tail;
    uint8_t count;
    uint8_t provisional;
} mind_root_inbox_t;

void mind_root_inbox_init(mind_root_inbox_t *inbox);
int mind_root_inbox_handles(const tavrn_link_data_t *data);
tavrn_router_delivery_status_t mind_root_inbox_reserve(
    mind_root_inbox_t *inbox, const tavrn_direct_peer_t *transmitter,
    const tavrn_link_data_t *data, tavrn_router_delivery_token_t *token_out);
tavrn_router_delivery_status_t mind_root_inbox_commit(
    mind_root_inbox_t *inbox, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t delivered_at_ms);
tavrn_router_delivery_status_t mind_root_inbox_cancel(
    mind_root_inbox_t *inbox, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data);
int mind_root_inbox_take(mind_root_inbox_t *inbox,
                          mind_root_inbox_entry_t *entry_out);
/* Peek/consume keeps committed opaque DATA owned by this inbox while the
 * Layer-7 consumer waits for logger capacity. */
int mind_root_inbox_peek(const mind_root_inbox_t *inbox,
                         mind_root_inbox_entry_t *entry_out);
int mind_root_inbox_consume(mind_root_inbox_t *inbox);
/* Pins the canonical routed observer after the first successful Layer-7
 * resolution.  Later logger retries must not re-resolve mutable topology. */
int mind_root_inbox_pin_observer(mind_root_inbox_t *inbox,
                                 const tavrn_adva_t *observer);
/* Direct-local fanout uses this same final inbox without claiming transport
 * HACK custody.  BUSY leaves the event target retained for a later retry. */
tavrn_router_delivery_status_t mind_root_inbox_publish_local(
    mind_root_inbox_t *inbox, const mind_application_wire_record_t *record,
    const tavrn_adva_t *local_observer, uint32_t delivered_at_ms);

#endif /* MIND_ROOT_INBOX_H */
