#include "mind_root_inbox.h"

#include <string.h>

#include "mind_application_wire.h"

void mind_root_inbox_init(mind_root_inbox_t *inbox)
{
    if (inbox != NULL) {
        memset(inbox, 0, sizeof(*inbox));
    }
}

int mind_root_inbox_handles(const tavrn_link_data_t *data)
{
    /* Generic router DATA remains opaque.  Unknown/malformed application
     * records must still reserve before HACK and later be rejected by Layer 7. */
    return data != NULL;
}

tavrn_router_delivery_status_t mind_root_inbox_reserve(
    mind_root_inbox_t *inbox, const tavrn_direct_peer_t *transmitter,
    const tavrn_link_data_t *data, tavrn_router_delivery_token_t *token_out)
{
    if (token_out != NULL) {
        *token_out = TAVRN_ROUTER_DELIVERY_TOKEN_NONE;
    }
    if (inbox == NULL || transmitter == NULL || data == NULL || token_out == NULL ||
        !mind_root_inbox_handles(data) || inbox->head >= MIND_ROOT_INBOX_CAPACITY ||
        inbox->tail >= MIND_ROOT_INBOX_CAPACITY || inbox->count > MIND_ROOT_INBOX_CAPACITY) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    if (inbox->provisional != 0u || inbox->count == MIND_ROOT_INBOX_CAPACITY) {
        return TAVRN_ROUTER_DELIVERY_BUSY;
    }
    inbox->provisional_token++;
    if (inbox->provisional_token == TAVRN_ROUTER_DELIVERY_TOKEN_NONE) {
        inbox->provisional_token++;
    }
    inbox->provisional_transmitter = *transmitter;
    inbox->provisional_data = *data;
    inbox->provisional = 1u;
    *token_out = inbox->provisional_token;
    return TAVRN_ROUTER_DELIVERY_OK;
}

tavrn_router_delivery_status_t mind_root_inbox_commit(
    mind_root_inbox_t *inbox, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t delivered_at_ms)
{
    mind_root_inbox_entry_t *entry;

    if (inbox == NULL || transmitter == NULL || data == NULL ||
        inbox->provisional == 0u || token == TAVRN_ROUTER_DELIVERY_TOKEN_NONE ||
        token != inbox->provisional_token ||
        memcmp(transmitter, &inbox->provisional_transmitter, sizeof(*transmitter)) != 0 ||
        memcmp(data, &inbox->provisional_data, sizeof(*data)) != 0 ||
        inbox->tail >= MIND_ROOT_INBOX_CAPACITY ||
        inbox->count >= MIND_ROOT_INBOX_CAPACITY) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    entry = &inbox->entries[inbox->tail];
    entry->transmitter = *transmitter;
    entry->data = *data;
    memset(&entry->observer, 0, sizeof(entry->observer));
    entry->delivered_at_ms = delivered_at_ms;
    entry->path = MIND_ROOT_INBOX_PATH_TAVRN;
    entry->observer_pinned = 0u;
    inbox->tail = (uint8_t)((inbox->tail + 1u) % MIND_ROOT_INBOX_CAPACITY);
    inbox->count++;
    inbox->provisional = 0u;
    memset(&inbox->provisional_transmitter, 0, sizeof(inbox->provisional_transmitter));
    memset(&inbox->provisional_data, 0, sizeof(inbox->provisional_data));
    return TAVRN_ROUTER_DELIVERY_OK;
}

tavrn_router_delivery_status_t mind_root_inbox_cancel(
    mind_root_inbox_t *inbox, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data)
{
    if (inbox == NULL || transmitter == NULL || data == NULL ||
        inbox->provisional == 0u || token == TAVRN_ROUTER_DELIVERY_TOKEN_NONE ||
        token != inbox->provisional_token ||
        memcmp(transmitter, &inbox->provisional_transmitter, sizeof(*transmitter)) != 0 ||
        memcmp(data, &inbox->provisional_data, sizeof(*data)) != 0) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    inbox->provisional = 0u;
    memset(&inbox->provisional_transmitter, 0, sizeof(inbox->provisional_transmitter));
    memset(&inbox->provisional_data, 0, sizeof(inbox->provisional_data));
    return TAVRN_ROUTER_DELIVERY_OK;
}

int mind_root_inbox_peek(const mind_root_inbox_t *inbox,
                         mind_root_inbox_entry_t *entry_out)
{
    if (inbox == NULL || entry_out == NULL || inbox->head >= MIND_ROOT_INBOX_CAPACITY ||
        inbox->tail >= MIND_ROOT_INBOX_CAPACITY || inbox->count > MIND_ROOT_INBOX_CAPACITY ||
        inbox->count == 0u) {
        return 0;
    }
    *entry_out = inbox->entries[inbox->head];
    return 1;
}

int mind_root_inbox_consume(mind_root_inbox_t *inbox)
{
    if (inbox == NULL || inbox->head >= MIND_ROOT_INBOX_CAPACITY ||
        inbox->tail >= MIND_ROOT_INBOX_CAPACITY ||
        inbox->count > MIND_ROOT_INBOX_CAPACITY || inbox->count == 0u) {
        return 0;
    }
    inbox->head = (uint8_t)((inbox->head + 1u) % MIND_ROOT_INBOX_CAPACITY);
    inbox->count--;
    return 1;
}

int mind_root_inbox_pin_observer(mind_root_inbox_t *inbox,
                                 const tavrn_adva_t *observer)
{
    mind_root_inbox_entry_t *entry;

    if (inbox == NULL || observer == NULL || inbox->head >= MIND_ROOT_INBOX_CAPACITY ||
        inbox->tail >= MIND_ROOT_INBOX_CAPACITY ||
        inbox->count > MIND_ROOT_INBOX_CAPACITY || inbox->count == 0u) {
        return 0;
    }
    entry = &inbox->entries[inbox->head];
    if (entry->path != MIND_ROOT_INBOX_PATH_TAVRN) {
        return 0;
    }
    if (entry->observer_pinned != 0u) {
        return memcmp(entry->observer.bytes, observer->bytes, TAVRN_ADVA_LEN) == 0;
    }
    entry->observer = *observer;
    entry->observer_pinned = 1u;
    return 1;
}

int mind_root_inbox_take(mind_root_inbox_t *inbox,
                          mind_root_inbox_entry_t *entry_out)
{
    return mind_root_inbox_peek(inbox, entry_out) && mind_root_inbox_consume(inbox);
}

tavrn_router_delivery_status_t mind_root_inbox_publish_local(
    mind_root_inbox_t *inbox, const mind_application_wire_record_t *record,
    const tavrn_adva_t *local_observer, uint32_t delivered_at_ms)
{
    mind_root_inbox_entry_t *entry;

    if (inbox == NULL || record == NULL || local_observer == NULL ||
        inbox->head >= MIND_ROOT_INBOX_CAPACITY ||
        inbox->tail >= MIND_ROOT_INBOX_CAPACITY ||
        inbox->count > MIND_ROOT_INBOX_CAPACITY) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    if (inbox->provisional != 0u || inbox->count == MIND_ROOT_INBOX_CAPACITY) {
        return TAVRN_ROUTER_DELIVERY_BUSY;
    }
    entry = &inbox->entries[inbox->tail];
    memset(entry, 0, sizeof(*entry));
    entry->data.app_kind = record->app_kind;
    entry->data.app_source = record->app_source;
    entry->data.urgent = record->urgent;
    entry->data.app_len = record->app_len;
    memcpy(entry->data.app_bytes, record->app_bytes, sizeof(entry->data.app_bytes));
    entry->observer = *local_observer;
    entry->delivered_at_ms = delivered_at_ms;
    entry->path = MIND_ROOT_INBOX_PATH_LOCAL;
    entry->observer_pinned = 1u;
    inbox->tail = (uint8_t)((inbox->tail + 1u) % MIND_ROOT_INBOX_CAPACITY);
    inbox->count++;
    return TAVRN_ROUTER_DELIVERY_OK;
}
