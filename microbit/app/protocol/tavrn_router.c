#include "tavrn_router.h"

#include <string.h>

static int time_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static int logical_id_is_valid(const tavrn_logical_id_t *logical_id);

tavrn_router_subject_demand_snapshot_t tavrn_router_subject_demand_snapshot(
    const tavrn_router_t *router, const tavrn_logical_id_t *subject,
    const tavrn_adva_t *canonical_subject, uint32_t now_ms)
{
    tavrn_router_subject_demand_snapshot_t snapshot;

    (void)now_ms;
    memset(&snapshot, 0, sizeof(snapshot));
    if (router == NULL || subject == NULL || canonical_subject == NULL ||
        router->aodv == NULL || router->link == NULL ||
        subject->width != router->aodv->config.local_peer.logical_id.width ||
        !logical_id_is_valid(subject) ||
        router->fault_reason != TAVRN_ROUTER_FAULT_NONE) {
        return snapshot;
    }
    snapshot.snapshot_available = 1u;
    if (router->pending_ingest.valid != 0u &&
        router->pending_ingest.input.data.final_destination.width == subject->width &&
        router->pending_ingest.input.data.final_destination.value == subject->value) {
        snapshot.reason_mask |= TAVRN_MAINT_DEMAND_PENDING_INGEST;
    }
    if (router->retained_action_valid != 0u &&
        (router->retained_action.type == AODV_ACTION_FORWARD_DATA ||
         router->retained_action.type == AODV_ACTION_DELIVER_DATA)) {
        const aodv_data_action_t *data = &router->retained_action.detail.data;

        if (data->data.final_destination.width == subject->width &&
            data->data.final_destination.value == subject->value) {
            snapshot.reason_mask |= TAVRN_MAINT_DEMAND_RETAINED_FINAL;
        }
        if (data->next_hop.logical_id.width == subject->width &&
            data->next_hop.logical_id.value == subject->value &&
            memcmp(data->next_hop.adva.bytes, canonical_subject->bytes,
                   TAVRN_ADVA_LEN) == 0) {
            snapshot.reason_mask |= TAVRN_MAINT_DEMAND_RETAINED_NEXT_HOP;
        }
    }
    return snapshot;
}

static int logical_id_is_valid(const tavrn_logical_id_t *logical_id)
{
    if (logical_id == NULL) {
        return 0;
    }
    if (logical_id->width == TAVRN_IDENTITY_SID16) {
        return logical_id->value != 0u && logical_id->value != 0xffffu;
    }
    if (logical_id->width == TAVRN_IDENTITY_SID8) {
        return (logical_id->value & 0xff00u) == 0u && logical_id->value != 0u &&
               logical_id->value != 0x00ffu;
    }
    return 0;
}

static int logical_id_equal(const tavrn_logical_id_t *left,
                            const tavrn_logical_id_t *right)
{
    return left != NULL && right != NULL && left->width == right->width &&
           left->value == right->value;
}

static int adva_is_valid(const tavrn_adva_t *adva)
{
    uint8_t random_all_zero = 1u;
    uint8_t random_all_one = 1u;
    uint8_t index;

    if (adva == NULL || (adva->bytes[TAVRN_ADVA_LEN - 1u] & 0xc0u) != 0xc0u) {
        return 0;
    }
    for (index = 0u; index < TAVRN_ADVA_LEN - 1u; index++) {
        if (adva->bytes[index] != 0u) {
            random_all_zero = 0u;
        }
        if (adva->bytes[index] != 0xffu) {
            random_all_one = 0u;
        }
    }
    if ((adva->bytes[TAVRN_ADVA_LEN - 1u] & 0x3fu) != 0u) {
        random_all_zero = 0u;
    }
    if ((adva->bytes[TAVRN_ADVA_LEN - 1u] & 0x3fu) != 0x3fu) {
        random_all_one = 0u;
    }
    return random_all_zero == 0u && random_all_one == 0u;
}

static uint16_t logical_suffix(const tavrn_adva_t *adva,
                               tavrn_identity_width_t width)
{
    if (width == TAVRN_IDENTITY_SID8) {
        return adva->bytes[0];
    }
    return (uint16_t)adva->bytes[0] | ((uint16_t)adva->bytes[1] << 8);
}

static int direct_peer_is_valid(const tavrn_direct_peer_t *peer)
{
    return peer != NULL && adva_is_valid(&peer->adva) &&
           logical_id_is_valid(&peer->logical_id) &&
           peer->logical_id.value == logical_suffix(&peer->adva,
                                                     peer->logical_id.width);
}

static int direct_peer_at_active_width(const tavrn_router_t *router,
                                       const tavrn_direct_peer_t *peer,
                                       tavrn_direct_peer_t *active_out)
{
    if (router == NULL || router->link == NULL || peer == NULL ||
        active_out == NULL) {
        return 0;
    }
    *active_out = *peer;
    active_out->logical_id.width = router->link->config.local_peer.logical_id.width;
    active_out->logical_id.value = logical_suffix(
        &active_out->adva, active_out->logical_id.width);
    return direct_peer_is_valid(active_out);
}

static int direct_peer_equal(const tavrn_direct_peer_t *left,
                             const tavrn_direct_peer_t *right)
{
    return left != NULL && right != NULL &&
           logical_id_equal(&left->logical_id, &right->logical_id) &&
           memcmp(left->adva.bytes, right->adva.bytes, TAVRN_ADVA_LEN) == 0;
}

static int link_data_is_valid(const tavrn_link_data_t *data)
{
    return data != NULL && logical_id_is_valid(&data->origin) &&
        logical_id_is_valid(&data->final_destination) &&
        data->origin.width == data->final_destination.width &&
        data->ttl <= 15u && data->hops <= 15u && data->urgent <= 1u &&
        data->app_len <= TAVRN_LINK_APP_BYTES &&
        (data->ownership == TAVRN_DATA_ORIGINATED ||
         data->ownership == TAVRN_DATA_TRANSIT);
}

static int application_data_fits_identity_width(const tron_application_data_t *data)
{
    return data != NULL && data->app_len <=
        (data->final_destination.width == TAVRN_IDENTITY_SID16 ?
         TAVRN_LINK_SID16_APP_BYTES : TAVRN_LINK_APP_BYTES);
}

static int link_data_equal(const tavrn_link_data_t *left,
                           const tavrn_link_data_t *right)
{
    return left != NULL && right != NULL &&
           logical_id_equal(&left->origin, &right->origin) &&
           logical_id_equal(&left->final_destination, &right->final_destination) &&
           left->data_seq == right->data_seq && left->ttl == right->ttl &&
           left->hops == right->hops && left->app_kind == right->app_kind &&
           left->app_source == right->app_source && left->urgent == right->urgent &&
           left->app_len == right->app_len &&
           memcmp(left->app_bytes, right->app_bytes, TAVRN_LINK_APP_BYTES) == 0 &&
           left->ownership == right->ownership;
}

static int control_action_uses_direct_peer(
    const tavrn_validated_control_t *control, const tavrn_direct_peer_t *peer)
{
    tavrn_identity_width_t width;
    uint8_t width_len;

    if (control == NULL || peer == NULL || control->pdu_len < 7u) {
        return 0;
    }
    width = (control->pdu[5] & 0x80u) != 0u ? TAVRN_IDENTITY_SID8 :
                                               TAVRN_IDENTITY_SID16;
    width_len = width == TAVRN_IDENTITY_SID8 ? 1u : 2u;
    if (width != peer->logical_id.width) {
        return 0;
    }
#define CONTROL_ACTION_ID_IS_PEER(at) \
    ((uint32_t)(at) + (uint32_t)width_len <= control->pdu_len && \
     (width == TAVRN_IDENTITY_SID8 ? (uint16_t)control->pdu[(at)] : \
      (uint16_t)control->pdu[(at)] | \
          ((uint16_t)control->pdu[(uint8_t)((at) + 1u)] << 8)) == \
         peer->logical_id.value)
    if (control->type == TAVRN_WIRE_E_RREQ) {
        return CONTROL_ACTION_ID_IS_PEER(7u) ||
            CONTROL_ACTION_ID_IS_PEER((uint8_t)(9u + width_len));
    }
    if (control->type == TAVRN_WIRE_E_RERR) {
        return CONTROL_ACTION_ID_IS_PEER(7u);
    }
    if (control->type == TAVRN_WIRE_E_RREP) {
        return CONTROL_ACTION_ID_IS_PEER(7u) ||
            CONTROL_ACTION_ID_IS_PEER((uint8_t)(7u + width_len)) ||
            CONTROL_ACTION_ID_IS_PEER((uint8_t)(9u + 2u * width_len));
    }
    if (control->type == TAVRN_WIRE_E_RREP_ACK) {
        return CONTROL_ACTION_ID_IS_PEER(6u) ||
            CONTROL_ACTION_ID_IS_PEER((uint8_t)(6u + width_len)) ||
            CONTROL_ACTION_ID_IS_PEER((uint8_t)(8u + 2u * width_len));
    }
    return 0;
#undef CONTROL_ACTION_ID_IS_PEER
}

static int action_uses_direct_peer(const aodv_action_t *action,
                                   const tavrn_direct_peer_t *peer)
{
    if (action == NULL || peer == NULL) {
        return 0;
    }
    switch (action->type) {
    case AODV_ACTION_SEND_RREQ:
    case AODV_ACTION_SEND_RREP:
    case AODV_ACTION_FORWARD_RREP:
    case AODV_ACTION_SEND_RERR:
    case AODV_ACTION_SEND_RREP_ACK:
        return direct_peer_equal(&action->detail.control.next_hop, peer) ||
            control_action_uses_direct_peer(&action->detail.control.control, peer);
    case AODV_ACTION_FORWARD_DATA:
    case AODV_ACTION_DELIVER_DATA:
        return direct_peer_equal(&action->detail.data.next_hop, peer) ||
            logical_id_equal(&action->detail.data.data.origin,
                             &peer->logical_id) ||
            logical_id_equal(&action->detail.data.data.final_destination,
                             &peer->logical_id);
    case AODV_ACTION_PENDING_DATA_FAILED:
    case AODV_ACTION_BLACKLIST_NEIGHBOR:
        return direct_peer_equal(&action->detail.failure.peer, peer) ||
            logical_id_equal(&action->detail.failure.destination,
                             &peer->logical_id);
    default:
        return 0;
    }
}

static int router_is_initialized(const tavrn_router_t *router)
{
    return router != NULL && router->link != NULL && router->aodv != NULL;
}

static int router_is_usable(const tavrn_router_t *router)
{
    return router_is_initialized(router) &&
           router->fault_reason == TAVRN_ROUTER_FAULT_NONE;
}

static int router_is_rejoining(const tavrn_router_t *router)
{
    return router != NULL && router->incarnation.enabled != 0u &&
        router->incarnation.snapshot.state == TAVRN_ROUTER_INCARNATION_REJOINING;
}

static int adva_equal(const tavrn_adva_t *left, const tavrn_adva_t *right)
{
    return left != NULL && right != NULL &&
        memcmp(left->bytes, right->bytes, TAVRN_ADVA_LEN) == 0;
}

static int incarnation_config_is_valid(
    const tavrn_router_incarnation_config_t *config)
{
    return config != NULL && config->boot_nonce != 0u &&
        (config->feature_level == TAVRN_ROUTER_FEATURE_AODV_ONLY ||
         config->feature_level == TAVRN_ROUTER_FEATURE_FULL_TAVRN) &&
        config->reboot_announce_ms != 0u && config->reboot_announce_ms < 0x80000000u;
}

static int incarnation_peer_index(const tavrn_router_t *router,
                                  const tavrn_adva_t *adva)
{
    uint8_t index;

    if (router == NULL || adva == NULL) {
        return -1;
    }
    for (index = 0u; index < TAVRN_ROUTER_INCARNATION_PEER_CAPACITY; index++) {
        if (router->incarnation.peers[index].valid != 0u &&
            adva_equal(&router->incarnation.peers[index].direct_peer.adva, adva)) {
            return (int)index;
        }
    }
    return -1;
}

static int free_incarnation_peer_index(const tavrn_router_t *router)
{
    uint8_t index;

    if (router == NULL) {
        return -1;
    }
    for (index = 0u; index < TAVRN_ROUTER_INCARNATION_PEER_CAPACITY; index++) {
        if (router->incarnation.peers[index].valid == 0u) {
            return (int)index;
        }
    }
    return -1;
}

static int direct_peer_route_barred(const tavrn_router_t *router,
                                    const tavrn_direct_peer_t *peer)
{
    int index;

    if (router == NULL || peer == NULL || router->incarnation.enabled == 0u) {
        return 0;
    }
    index = incarnation_peer_index(router, &peer->adva);
    return index >= 0 &&
        router->incarnation.peers[(uint8_t)index].route_barred != 0u;
}

static int logical_id_route_barred(const tavrn_router_t *router,
                                   const tavrn_logical_id_t *logical_id)
{
    uint8_t index;

    if (router == NULL || logical_id == NULL ||
        router->incarnation.enabled == 0u) {
        return 0;
    }
    for (index = 0u; index < TAVRN_ROUTER_INCARNATION_PEER_CAPACITY; index++) {
        const tavrn_router_incarnation_peer_t *record =
            &router->incarnation.peers[index];

        if (record->valid != 0u && record->route_barred != 0u &&
            logical_id_equal(&record->direct_peer.logical_id, logical_id)) {
            return 1;
        }
    }
    return 0;
}

static int action_uses_barred_peer(const tavrn_router_t *router,
                                   const aodv_action_t *action)
{
    uint8_t index;

    if (router == NULL || action == NULL ||
        router->incarnation.enabled == 0u) {
        return 0;
    }
    for (index = 0u; index < TAVRN_ROUTER_INCARNATION_PEER_CAPACITY; index++) {
        const tavrn_router_incarnation_peer_t *record =
            &router->incarnation.peers[index];

        if (record->valid != 0u && record->route_barred != 0u &&
            action_uses_direct_peer(action, &record->direct_peer)) {
            return 1;
        }
    }
    return 0;
}

static int sid16_collision_index(const tavrn_router_t *router,
                                 const tavrn_direct_peer_t *peer)
{
    uint8_t index;

    if (router == NULL || peer == NULL ||
        peer->logical_id.width != TAVRN_IDENTITY_SID16) {
        return -1;
    }
    for (index = 0u; index < TAVRN_ROUTER_INCARNATION_PEER_CAPACITY; index++) {
        const tavrn_router_incarnation_peer_t *record =
            &router->incarnation.peers[index];

        if (record->valid != 0u &&
            logical_id_equal(&record->direct_peer.logical_id, &peer->logical_id) &&
            !adva_equal(&record->direct_peer.adva, &peer->adva)) {
            return (int)index;
        }
    }
    return -1;
}

static int logical_id_is_barred_conflict(const tavrn_router_t *router,
                                         const tavrn_logical_id_t *logical_id)
{
    uint8_t index;

    if (router == NULL || logical_id == NULL ||
        router->incarnation.enabled == 0u) {
        return 0;
    }
    for (index = 0u; index < TAVRN_ROUTER_INCARNATION_PEER_CAPACITY; index++) {
        const tavrn_router_incarnation_peer_t *record =
            &router->incarnation.peers[index];

        if (record->valid != 0u && record->route_barred != 0u &&
            record->identity_conflict != 0u &&
            logical_id_equal(&record->direct_peer.logical_id, logical_id)) {
            return 1;
        }
    }
    return 0;
}

static int control_id_is_barred_conflict(
    const tavrn_router_t *router, const tavrn_validated_control_t *control,
    uint8_t offset, tavrn_identity_width_t width)
{
    tavrn_logical_id_t logical_id;
    uint8_t width_len = width == TAVRN_IDENTITY_SID8 ? 1u : 2u;

    if (control == NULL || (uint32_t)offset + width_len > control->pdu_len) {
        return 0;
    }
    logical_id.width = width;
    logical_id.value = width == TAVRN_IDENTITY_SID8 ? control->pdu[offset] :
        (uint16_t)control->pdu[offset] |
            ((uint16_t)control->pdu[(uint8_t)(offset + 1u)] << 8);
    return logical_id_is_barred_conflict(router, &logical_id);
}

static int control_uses_barred_conflict(
    const tavrn_router_t *router, const tavrn_validated_control_t *control)
{
    tavrn_identity_width_t width;
    uint8_t width_len;
    uint8_t count;
    uint8_t maximum;
    uint8_t index;

    if (control == NULL || control->pdu_len < 6u) {
        return 0;
    }
    width = (control->pdu[5] & 0x80u) != 0u ? TAVRN_IDENTITY_SID8 :
                                               TAVRN_IDENTITY_SID16;
    width_len = width == TAVRN_IDENTITY_SID8 ? 1u : 2u;
    switch (control->type) {
    case TAVRN_WIRE_E_RREQ:
        return control_id_is_barred_conflict(router, control, 7u, width) ||
            control_id_is_barred_conflict(router, control,
                                          (uint8_t)(9u + width_len), width);
    case TAVRN_WIRE_E_RREP:
        return control_id_is_barred_conflict(router, control, 7u, width) ||
            control_id_is_barred_conflict(router, control,
                                          (uint8_t)(7u + width_len), width) ||
            control_id_is_barred_conflict(router, control,
                                          (uint8_t)(9u + 2u * width_len), width);
    case TAVRN_WIRE_E_RREP_ACK:
        return control_id_is_barred_conflict(router, control, 6u, width) ||
            control_id_is_barred_conflict(router, control,
                                          (uint8_t)(6u + width_len), width) ||
            control_id_is_barred_conflict(router, control,
                                          (uint8_t)(8u + 2u * width_len), width);
    case TAVRN_WIRE_E_RERR:
        if (control_id_is_barred_conflict(router, control, 7u, width) ||
            control->pdu_len <= (uint8_t)(9u + width_len)) {
            return control_id_is_barred_conflict(router, control, 7u, width);
        }
        count = control->pdu[(uint8_t)(9u + width_len)];
        maximum = width == TAVRN_IDENTITY_SID8 ? TAVRN_AODV_RERR_SID8_PER_ACTION :
                                                 TAVRN_AODV_RERR_SID16_PER_ACTION;
        if (count > maximum) {
            count = maximum;
        }
        for (index = 0u; index < count; index++) {
            if (control_id_is_barred_conflict(
                    router, control,
                    (uint8_t)(10u + width_len + index * (width_len + 2u)),
                    width)) {
                return 1;
            }
        }
        return 0;
    default:
        return 0;
    }
}

static int decoded_frame_uses_barred_conflict(
    const tavrn_router_t *router, const tavrn_decoded_frame_t *frame)
{
    const tavrn_validated_control_t *control;

    if (router == NULL || frame == NULL) {
        return 0;
    }
    if (frame->type == TAVRN_WIRE_HELLO) {
        control = &frame->detail.control;
        /* The only allowed collision traffic is the full-identity N=1 HELLO,
         * which reaches the router solely to report/reject the collision. */
        if (control->pdu_len == 19u && control->pdu[5] == 0x40u) {
            return 0;
        }
    }
    if (logical_id_is_barred_conflict(router, &frame->transmitter.logical_id)) {
        return 1;
    }
    switch (frame->type) {
    case TAVRN_WIRE_DATA:
        return logical_id_is_barred_conflict(
                   router, &frame->detail.data.immediate_receiver) ||
            logical_id_is_barred_conflict(router, &frame->detail.data.data.origin) ||
            logical_id_is_barred_conflict(router,
                                          &frame->detail.data.data.final_destination);
    case TAVRN_WIRE_HACK:
        return logical_id_is_barred_conflict(
                   router, &frame->detail.hack.immediate_receiver) ||
            logical_id_is_barred_conflict(router, &frame->detail.hack.data_origin) ||
            logical_id_is_barred_conflict(router,
                                          &frame->detail.hack.final_destination);
    case TAVRN_WIRE_FLOOD:
        return logical_id_is_barred_conflict(router, &frame->detail.flood.origin);
    case TAVRN_WIRE_E_RREQ:
    case TAVRN_WIRE_E_RREP:
    case TAVRN_WIRE_E_RERR:
    case TAVRN_WIRE_E_RREP_ACK:
        return control_uses_barred_conflict(router, &frame->detail.control);
    default:
        return 0;
    }
}

static void build_hello(tavrn_validated_control_t *control,
                         const tavrn_direct_peer_t *local, uint8_t bootstrap,
                         uint16_t serial, uint8_t network_id)
{
    if (control == NULL || local == NULL) {
        return;
    }
    memset(control, 0, sizeof(*control));
    control->type = TAVRN_WIRE_HELLO;
    control->pdu_len = 19u;
    control->pdu[0] = 0x54u;
    control->pdu[1] = 0x52u;
    control->pdu[2] = 0x02u;
    control->pdu[3] = network_id;
    control->pdu[4] = TAVRN_WIRE_HELLO;
    control->pdu[5] = bootstrap != 0u ? 0x40u : 0u;
    control->pdu[6] = 0x10u;
    control->pdu[7] = 0xffu;
    control->pdu[8] = 0xffu;
    control->pdu[9] = 0xffu;
    control->pdu[10] = 0xffu;
    memcpy(&control->pdu[11], local->adva.bytes, TAVRN_ADVA_LEN);
    control->pdu[17] = (uint8_t)serial;
    control->pdu[18] = (uint8_t)(serial >> 8);
}

static int controls_equal(const tavrn_validated_control_t *left,
                          const tavrn_validated_control_t *right)
{
    return left != NULL && right != NULL && left->type == right->type &&
        left->pdu_len == right->pdu_len &&
        memcmp(left->pdu, right->pdu, left->pdu_len) == 0;
}

static tavrn_router_incarnation_status_t enqueue_local_hello(
    tavrn_router_t *router, uint8_t bootstrap, uint32_t now_ms)
{
    tavrn_validated_control_t hello;
    tavrn_link_event_t local_outcome;
    tavrn_link_send_status_t send_status;
    ble_mesh_tx_token_t token = BLE_MESH_TX_TOKEN_NONE;
    uint16_t serial;

    if (!router_is_usable(router) || router->incarnation.enabled == 0u) {
        return TAVRN_ROUTER_INCARNATION_INVALID;
    }
    serial = bootstrap != 0u ? router->incarnation.config.boot_nonce : 1u;
    build_hello(&hello, &router->link->config.local_peer, bootstrap, serial,
                router->link->config.network_id);
    memset(&local_outcome, 0, sizeof(local_outcome));
    send_status = tavrn_link_v2_send_control_tracked(
        router->link, &hello, NULL, 0u, now_ms, &local_outcome, &token);
    if (send_status != TAVRN_LINK_SEND_OK || token == BLE_MESH_TX_TOKEN_NONE ||
        (local_outcome.type != TAVRN_LINK_EVENT_NONE &&
         tavrn_router_handle_link_event(router, &local_outcome, now_ms) ==
             TAVRN_ROUTER_EVENT_INVALID)) {
        return send_status == TAVRN_LINK_SEND_BUSY ||
                send_status == TAVRN_LINK_SEND_NO_SLOT ?
            TAVRN_ROUTER_INCARNATION_BUSY : TAVRN_ROUTER_INCARNATION_INVALID;
    }
    tavrn_router_note_local_broadcast(router, now_ms);
    router->incarnation.snapshot.latest_hello = hello;
    router->incarnation.snapshot.latest_hello_tx_token = token;
    if (bootstrap == 0u) {
        router->incarnation.snapshot.normal_hello_emitted = 1u;
    }
    return TAVRN_ROUTER_INCARNATION_OK;
}

static int direct_bootstrap_valid(const tavrn_rx_control_event_t *control_event,
                                  uint16_t *nonce_out)
{
    const tavrn_validated_control_t *control;
    const tavrn_direct_peer_t *peer;
    uint16_t nonce;

    if (control_event == NULL || nonce_out == NULL) {
        return 0;
    }
    control = &control_event->control;
    peer = &control_event->transmitter;
    if (control->type != TAVRN_WIRE_HELLO || control->pdu_len != 19u ||
        control->pdu[5] != 0x40u || control->pdu[6] != 0x10u ||
        control->pdu[7] != 0xffu || control->pdu[8] != 0xffu ||
        control->pdu[9] != 0xffu || control->pdu[10] != 0xffu ||
        peer->logical_id.width != TAVRN_IDENTITY_SID16 ||
        !direct_peer_is_valid(peer) ||
        memcmp(&control->pdu[11], peer->adva.bytes, TAVRN_ADVA_LEN) != 0) {
        return 0;
    }
    nonce = (uint16_t)control->pdu[17] | ((uint16_t)control->pdu[18] << 8);
    if (nonce == 0u) {
        return 0;
    }
    *nonce_out = nonce;
    return 1;
}

static int invalid_bootstrap_wire(const ble_mesh_sched_event_t *event)
{
    return event != NULL && event->type == BLE_MESH_SCHED_EVENT_RX_ADV &&
        event->adv_len >= 13u && event->adv_data[7] == 0x54u &&
        event->adv_data[8] == 0x52u && event->adv_data[9] == 0x02u &&
        event->adv_data[11] == TAVRN_WIRE_HELLO &&
        (event->adv_data[12] & 0x40u) != 0u;
}

static void latch_router_fault(tavrn_router_t *router,
                                tavrn_router_fault_reason_t reason);

static tavrn_router_event_status_t discard_internal_transit_custody(
    tavrn_router_t *router, const tavrn_link_data_t *data)
{
    if (router == NULL || data == NULL || !link_data_is_valid(data)) {
        if (router != NULL) {
            latch_router_fault(router,
                               TAVRN_ROUTER_FAULT_TRANSIT_CUSTODY_DISCARD_INVALID);
        }
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (data->ownership != TAVRN_DATA_TRANSIT) {
        return TAVRN_ROUTER_EVENT_OK;
    }
    if (tavrn_link_v2_discard_internal_rx_custody(router->link, data) !=
        TAVRN_LINK_RESOLVE_OK) {
        latch_router_fault(router,
                           TAVRN_ROUTER_FAULT_TRANSIT_CUSTODY_DISCARD_INVALID);
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    return TAVRN_ROUTER_EVENT_OK;
}

static void discard_queued_transit_custody(void *context,
                                           const aodv_data_action_t *action)
{
    tavrn_router_t *router = context;

    if (router != NULL && action != NULL) {
        (void)discard_internal_transit_custody(router, &action->data);
    } else if (router != NULL) {
        latch_router_fault(router,
                           TAVRN_ROUTER_FAULT_TRANSIT_CUSTODY_DISCARD_INVALID);
    }
}

static aodv_failure_status_t reset_aodv_peer_incarnation(
    tavrn_router_t *router, const tavrn_direct_peer_t *peer, uint32_t now_ms)
{
    return aodv_core_reset_peer_incarnation_with_data_discard(
        router->aodv, peer, now_ms, discard_queued_transit_custody, router);
}

static tavrn_router_event_status_t clear_router_peer_work(
    tavrn_router_t *router, const tavrn_direct_peer_t *peer, uint32_t now_ms)
{
    tavrn_router_delivery_status_t delivery_status;
    uint8_t index;

    if (router->delivery.state != TAVRN_ROUTER_DELIVERY_NONE &&
        (direct_peer_equal(&router->delivery.transmitter, peer) ||
         logical_id_equal(&router->delivery.data.origin, &peer->logical_id) ||
         logical_id_equal(&router->delivery.data.final_destination,
                          &peer->logical_id))) {
        if (router->application.cancel == NULL) {
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        delivery_status = router->application.cancel(
            router->application.context, router->delivery.token,
            &router->delivery.transmitter, &router->delivery.data, now_ms);
        if (delivery_status == TAVRN_ROUTER_DELIVERY_BUSY) {
            router->counters.application_cancel_busy++;
            return TAVRN_ROUTER_EVENT_BUSY;
        }
        if (delivery_status != TAVRN_ROUTER_DELIVERY_OK) {
            router->counters.application_cancel_invalid++;
            latch_router_fault(router, TAVRN_ROUTER_FAULT_DELIVERY_CANCEL_INVALID);
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        memset(&router->delivery, 0, sizeof(router->delivery));
    }
    if (router->retained_action_valid != 0u &&
        action_uses_direct_peer(&router->retained_action, peer)) {
        uint16_t token = 0u;

        if ((router->retained_action.type == AODV_ACTION_FORWARD_DATA ||
             router->retained_action.type == AODV_ACTION_DELIVER_DATA) &&
            discard_internal_transit_custody(
                router, &router->retained_action.detail.data.data) !=
                TAVRN_ROUTER_EVENT_OK) {
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        switch (router->retained_action.type) {
        case AODV_ACTION_SEND_RREQ:
        case AODV_ACTION_SEND_RREP:
        case AODV_ACTION_FORWARD_RREP:
        case AODV_ACTION_SEND_RERR:
        case AODV_ACTION_SEND_RREP_ACK:
            token = router->retained_action.detail.control.token;
            break;
        default:
            break;
        }
        if (token != 0u) {
            aodv_status_t cancel_status =
                aodv_core_cancel_unsent_action(router->aodv, token);

            if (cancel_status != AODV_STATUS_OK &&
                cancel_status != AODV_STATUS_NOT_FOUND) {
                latch_router_fault(router,
                                   TAVRN_ROUTER_FAULT_CONTROL_CANCEL_INVALID);
                return TAVRN_ROUTER_EVENT_INVALID;
            }
        }
        memset(&router->retained_action, 0, sizeof(router->retained_action));
        router->retained_action_valid = 0u;
    }
    if (router->pending_ingest.valid != 0u &&
        (direct_peer_equal(&router->pending_ingest.input.transmitter, peer) ||
         logical_id_equal(&router->pending_ingest.input.data.origin,
                          &peer->logical_id) ||
         logical_id_equal(&router->pending_ingest.input.data.final_destination,
                          &peer->logical_id))) {
        if (discard_internal_transit_custody(
                router, &router->pending_ingest.input.data) !=
            TAVRN_ROUTER_EVENT_OK) {
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        memset(&router->pending_ingest, 0, sizeof(router->pending_ingest));
    }
    for (index = 0u; index < TAVRN_ROUTER_FAILURE_CAPACITY; index++) {
        if (router->failures[index].valid != 0u &&
            direct_peer_equal(&router->failures[index].peer, peer)) {
            memset(&router->failures[index], 0, sizeof(router->failures[index]));
        }
    }
    if (router->failure_overflow.valid != 0u &&
        direct_peer_equal(&router->failure_overflow.owned.next_hop, peer)) {
        memset(&router->failure_overflow, 0, sizeof(router->failure_overflow));
    }
    return TAVRN_ROUTER_EVENT_OK;
}

static tavrn_router_event_status_t quarantine_peer_incarnation(
    tavrn_router_t *router, tavrn_router_incarnation_peer_t *record,
    const tavrn_direct_peer_t *peer, uint16_t nonce, uint8_t preserve_bar,
    uint32_t now_ms)
{
    tavrn_router_event_status_t clear_status;

    if (tavrn_link_v2_quarantine_peer_incarnation(router->link, peer) !=
        TAVRN_LINK_RESOLVE_OK ||
        aodv_core_quarantine_peer_incarnation_with_data_discard(
            router->aodv, peer, discard_queued_transit_custody, router) !=
            AODV_FAILURE_OK) {
        latch_router_fault(router, TAVRN_ROUTER_FAULT_CONTROL_CANCEL_INVALID);
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (!router_is_usable(router)) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    record->route_barred = 1u;
    memset(&router->incarnation.pending_reset, 0,
           sizeof(router->incarnation.pending_reset));
    router->incarnation.pending_reset.direct_peer = *peer;
    router->incarnation.pending_reset.boot_nonce = nonce;
    router->incarnation.pending_reset.preserve_bar = preserve_bar;
    router->incarnation.pending_reset.valid = 1u;
    router->incarnation.counters.reset_busy++;
    clear_status = clear_router_peer_work(router, peer, now_ms);
    return clear_status == TAVRN_ROUTER_EVENT_INVALID ? clear_status :
                                                        TAVRN_ROUTER_EVENT_BUSY;
}

static tavrn_router_event_status_t commit_peer_incarnation(
    tavrn_router_t *router, uint8_t index, const tavrn_direct_peer_t *peer,
    uint16_t nonce, uint8_t preserve_bar, uint32_t now_ms)
{
    tavrn_router_incarnation_peer_t *record = &router->incarnation.peers[index];
    tavrn_direct_peer_t bootstrap_peer = record->direct_peer;

    if (tavrn_link_v2_clear_peer_incarnation(router->link, peer, now_ms) !=
        TAVRN_LINK_RESOLVE_OK) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    {
        tavrn_router_event_status_t clear_status =
            clear_router_peer_work(router, peer, now_ms);

        if (clear_status != TAVRN_ROUTER_EVENT_OK) {
            return clear_status;
        }
    }
    if (preserve_bar != 0u) {
        record->route_barred = 1u;
        record->identity_conflict = 1u;
        return TAVRN_ROUTER_EVENT_OK;
    }
    memset(record, 0, sizeof(*record));
    /* The incarnation record retains the full-identity N=1 binding.  `peer`
     * is the same AdvA projected into the common core's active width solely
     * for link/AODV cleanup. */
    record->direct_peer = bootstrap_peer;
    record->boot_nonce = nonce;
    record->valid = 1u;
    router->incarnation.counters.reset_committed++;
    return TAVRN_ROUTER_EVENT_OK;
}

static tavrn_router_event_status_t handle_direct_bootstrap(
    tavrn_router_t *router, const tavrn_rx_control_event_t *control_event,
    uint32_t now_ms)
{
    const tavrn_direct_peer_t *peer;
    tavrn_router_incarnation_peer_t *record;
    tavrn_direct_peer_t active_peer;
    aodv_failure_status_t reset_status;
    uint16_t nonce;
    int index;
    int collision_index;

    if (!direct_bootstrap_valid(control_event, &nonce)) {
        router->incarnation.counters.invalid_bootstrap_rejected++;
        return TAVRN_ROUTER_EVENT_IGNORED;
    }
    peer = &control_event->transmitter;
    if (router->incarnation.pending_reset.valid != 0u) {
        if (adva_equal(&router->incarnation.pending_reset.direct_peer.adva,
                       &peer->adva) &&
            router->incarnation.pending_reset.boot_nonce == nonce) {
            return TAVRN_ROUTER_EVENT_BUSY;
        }
        router->incarnation.counters.reset_overflow++;
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    index = incarnation_peer_index(router, &peer->adva);
    if (index < 0) {
        collision_index = sid16_collision_index(router, peer);
        if (collision_index >= 0) {
            record = &router->incarnation.peers[(uint8_t)collision_index];
            if (record->identity_conflict != 0u) {
                router->incarnation.counters.invalid_bootstrap_rejected++;
                return TAVRN_ROUTER_EVENT_INVALID;
            }
            record->route_barred = 1u;
            record->identity_conflict = 1u;
            if (!direct_peer_at_active_width(router, &record->direct_peer,
                                             &active_peer)) {
                latch_router_fault(router,
                                   TAVRN_ROUTER_FAULT_CONTROL_CANCEL_INVALID);
                return TAVRN_ROUTER_EVENT_INVALID;
            }
            reset_status = reset_aodv_peer_incarnation(router, &active_peer,
                                                        now_ms);
            if (!router_is_usable(router)) {
                router->incarnation.counters.invalid_bootstrap_rejected++;
                return TAVRN_ROUTER_EVENT_INVALID;
            }
            if (reset_status == AODV_FAILURE_BUSY) {
                (void)quarantine_peer_incarnation(
                    router, record, &active_peer, record->boot_nonce, 1u,
                    now_ms);
            } else if (reset_status == AODV_FAILURE_OK) {
                tavrn_router_event_status_t commit_status =
                    commit_peer_incarnation(router, (uint8_t)collision_index,
                                            &active_peer,
                                            record->boot_nonce, 1u, now_ms);

                if (commit_status == TAVRN_ROUTER_EVENT_BUSY) {
                    memset(&router->incarnation.pending_reset, 0,
                           sizeof(router->incarnation.pending_reset));
                    router->incarnation.pending_reset.direct_peer =
                        active_peer;
                    router->incarnation.pending_reset.boot_nonce =
                        record->boot_nonce;
                    router->incarnation.pending_reset.preserve_bar = 1u;
                    router->incarnation.pending_reset.aodv_reset_committed = 1u;
                    router->incarnation.pending_reset.valid = 1u;
                }
            } else {
                latch_router_fault(router,
                                   TAVRN_ROUTER_FAULT_CONTROL_CANCEL_INVALID);
            }
            router->incarnation.counters.invalid_bootstrap_rejected++;
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        index = free_incarnation_peer_index(router);
        if (index < 0) {
            router->incarnation.counters.invalid_bootstrap_rejected++;
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        record = &router->incarnation.peers[(uint8_t)index];
        memset(record, 0, sizeof(*record));
        record->direct_peer = *peer;
        record->boot_nonce = nonce;
        record->valid = 1u;
        router->incarnation.counters.direct_bootstrap_admitted++;
        return TAVRN_ROUTER_EVENT_OK;
    }
    record = &router->incarnation.peers[(uint8_t)index];
    if (record->route_barred != 0u) {
        if (record->identity_conflict == 0u && nonce == record->boot_nonce) {
            router->incarnation.counters.same_tuple_idempotent++;
            return TAVRN_ROUTER_EVENT_OK;
        }
        router->incarnation.counters.reset_overflow++;
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (nonce == record->boot_nonce) {
        router->incarnation.counters.same_tuple_idempotent++;
        return TAVRN_ROUTER_EVENT_OK;
    }
    if (!direct_peer_at_active_width(router, peer, &active_peer)) {
        router->incarnation.counters.invalid_bootstrap_rejected++;
        return TAVRN_ROUTER_EVENT_INVALID;
    }

    /* The core reserves every RERR action before it invalidates a route.  The
     * link and direct binding are cleared only after that reservation succeeds. */
    reset_status = reset_aodv_peer_incarnation(router, &active_peer, now_ms);
    if (!router_is_usable(router)) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (reset_status == AODV_FAILURE_BUSY) {
        return quarantine_peer_incarnation(router, record, &active_peer, nonce, 0u,
                                           now_ms);
    }
    if (reset_status != AODV_FAILURE_OK) {
        router->incarnation.counters.invalid_bootstrap_rejected++;
        latch_router_fault(router, TAVRN_ROUTER_FAULT_CONTROL_CANCEL_INVALID);
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    {
        tavrn_router_event_status_t commit_status =
            commit_peer_incarnation(router, (uint8_t)index, &active_peer, nonce, 0u,
                                    now_ms);

        if (commit_status == TAVRN_ROUTER_EVENT_BUSY) {
            record->route_barred = 1u;
            memset(&router->incarnation.pending_reset, 0,
                   sizeof(router->incarnation.pending_reset));
            router->incarnation.pending_reset.direct_peer = active_peer;
            router->incarnation.pending_reset.boot_nonce = nonce;
            router->incarnation.pending_reset.preserve_bar = 0u;
            router->incarnation.pending_reset.aodv_reset_committed = 1u;
            router->incarnation.pending_reset.valid = 1u;
        }
        return commit_status;
    }
}

static tavrn_router_event_status_t retry_pending_incarnation_reset(
    tavrn_router_t *router, uint32_t now_ms)
{
    tavrn_router_pending_incarnation_reset_t *pending;
    aodv_failure_status_t reset_status;
    tavrn_router_event_status_t commit_status;
    int index;

    if (router == NULL || router->incarnation.pending_reset.valid == 0u) {
        return TAVRN_ROUTER_EVENT_OK;
    }
    pending = &router->incarnation.pending_reset;
    index = incarnation_peer_index(router, &pending->direct_peer.adva);
    if (index < 0) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (pending->aodv_reset_committed == 0u) {
        reset_status = reset_aodv_peer_incarnation(router,
                                                    &pending->direct_peer,
                                                    now_ms);
        if (!router_is_usable(router)) {
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        if (reset_status == AODV_FAILURE_BUSY) {
            return TAVRN_ROUTER_EVENT_BUSY;
        }
        if (reset_status != AODV_FAILURE_OK) {
            latch_router_fault(router,
                               TAVRN_ROUTER_FAULT_CONTROL_CANCEL_INVALID);
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        pending->aodv_reset_committed = 1u;
    }
    commit_status = commit_peer_incarnation(router, (uint8_t)index,
                                            &pending->direct_peer,
                                            pending->boot_nonce,
                                            pending->preserve_bar, now_ms);
    if (commit_status != TAVRN_ROUTER_EVENT_OK) {
        return commit_status;
    }
    memset(pending, 0, sizeof(*pending));
    return TAVRN_ROUTER_EVENT_OK;
}

static void latch_router_fault(tavrn_router_t *router,
                                tavrn_router_fault_reason_t reason)
{
    if (router->fault_reason == TAVRN_ROUTER_FAULT_NONE) {
        router->fault_reason = reason;
    }
}

static void begin_phase_trace(tavrn_router_phase_trace_t *trace_out,
                              tavrn_router_trace_phase_t phase,
                              uint32_t now_ms)
{
    if (trace_out == NULL) {
        return;
    }
    memset(trace_out, 0, sizeof(*trace_out));
    trace_out->completed_at_ms = now_ms;
    trace_out->phase = phase;
}

static void finish_phase_trace(tavrn_router_phase_trace_t *trace_out,
                               const tavrn_router_t *router,
                               uint32_t now_ms)
{
    if (trace_out == NULL) {
        return;
    }
    trace_out->completed_at_ms = now_ms;
    if (router != NULL && router->fault_reason != TAVRN_ROUTER_FAULT_NONE) {
        trace_out->terminal_fault_present = TAVRN_ROUTER_TRACE_PRESENT;
        trace_out->terminal_fault = router->fault_reason;
    }
}

static void capture_dispatch_action(tavrn_router_dispatch_trace_t *trace,
                                    const aodv_action_t *action)
{
    if (trace == NULL || action == NULL) {
        return;
    }
    trace->action = *action;
    trace->action_present = TAVRN_ROUTER_TRACE_PRESENT;
}

static void capture_dispatch_link_result(
    tavrn_router_dispatch_trace_t *trace, tavrn_link_send_status_t link_status,
    const tavrn_link_event_t *local_outcome)
{
    if (trace == NULL) {
        return;
    }
    trace->link_send_status = link_status;
    trace->link_send_present = TAVRN_ROUTER_TRACE_PRESENT;
    if (local_outcome != NULL && local_outcome->type != TAVRN_LINK_EVENT_NONE) {
        trace->link_event = *local_outcome;
        trace->link_event_present = TAVRN_ROUTER_TRACE_PRESENT;
    }
}

/* The copied action is the source of truth for this router-stage report.  It
 * records scheduler admission only; link-v2 still owns later radio outcomes. */
static void report_rreq_link_enqueue(
    aodv_core_t *aodv, const aodv_action_t *action,
    tavrn_link_send_status_t link_status, uint32_t now_ms,
    tavrn_router_dispatch_trace_t *trace)
{
    aodv_rreq_link_enqueue_t enqueue;

    if (aodv == NULL || action == NULL || action->type != AODV_ACTION_SEND_RREQ ||
        action->detail.control.rreq_attempt_present != AODV_RREQ_ATTEMPT_PRESENT) {
        return;
    }
    memset(&enqueue, 0, sizeof(enqueue));
    enqueue.attempt = action->detail.control.rreq_attempt;
    enqueue.link_enqueue_at_ms = now_ms;
    enqueue.outcome = link_status == TAVRN_LINK_SEND_OK ?
        AODV_RREQ_ENQUEUE_ADMITTED : AODV_RREQ_ENQUEUE_NOT_ADMITTED;
    if (trace != NULL) {
        trace->rreq_enqueue = enqueue;
        trace->rreq_enqueue_present = TAVRN_ROUTER_TRACE_PRESENT;
    }
    /* Telemetry has no authority over the already-decided link admission or
     * retained-action ownership path. */
    (void)aodv_core_report_rreq_link_enqueue(aodv, &enqueue);
}

static int supported_observation_type(tavrn_wire_type_t type)
{
    return type == TAVRN_WIRE_DATA || type == TAVRN_WIRE_HACK ||
           type == TAVRN_WIRE_FLOOD || type == TAVRN_WIRE_E_RREQ ||
           type == TAVRN_WIRE_E_RREP || type == TAVRN_WIRE_E_RERR ||
           type == TAVRN_WIRE_E_RREP_ACK;
}

static int ignored_observation_type(tavrn_wire_type_t type)
{
    return type == TAVRN_WIRE_HELLO || type == TAVRN_WIRE_SYNC_OFFER ||
           type == TAVRN_WIRE_SYNC_PULL || type == TAVRN_WIRE_SYNC_DATA ||
           type == TAVRN_WIRE_TC_UPDATE;
}

static int known_noncommitted_admission(tavrn_router_frame_admission_t admission)
{
    return admission == TAVRN_ROUTER_FRAME_UNRESOLVED_DATA_CANDIDATE ||
           admission == TAVRN_ROUTER_FRAME_OVERHEARD_ADDRESSED_DATA ||
           admission == TAVRN_ROUTER_FRAME_MALFORMED ||
           admission == TAVRN_ROUTER_FRAME_AMBIGUOUS ||
           admission == TAVRN_ROUTER_FRAME_REJECTED;
}

static int subject_is_valid(const tavrn_router_evidence_subject_t *subject,
                            tavrn_router_evidence_role_t expected_role,
                            tavrn_router_evidence_serial_kind_t expected_serial)
{
    if (subject == NULL || !logical_id_is_valid(&subject->logical_id) ||
        subject->role != expected_role || subject->serial_kind != expected_serial ||
        subject->hop_present != 1u || subject->hop_count == 0u ||
        subject->hop_count > 15u || subject->canonical_present > 1u) {
        return 0;
    }
    if (subject->canonical_present == 0u) {
        return 1;
    }
    return adva_is_valid(&subject->canonical_identity) &&
           subject->logical_id.value == logical_suffix(&subject->canonical_identity,
                                                       subject->logical_id.width);
}

static int optional_subject_is_valid(
    const tavrn_router_frame_observation_t *frame_observation,
    tavrn_router_evidence_role_t expected_role,
    tavrn_router_evidence_serial_kind_t expected_serial)
{
    if (frame_observation->subject_count == 0u) {
        return 1;
    }
    return frame_observation->subject_count == 1u &&
           subject_is_valid(&frame_observation->subjects[0], expected_role,
                            expected_serial);
}

static void report_observation(tavrn_router_t *router,
                               const tavrn_router_observation_t *observation,
                               uint32_t now_ms)
{
    if (router->augmentation.observe != NULL) {
        router->augmentation.observe(router->augmentation.context, observation,
                                     now_ms);
    }
}

static int hop_plus_one(uint8_t encoded_hops, uint8_t *hop_out)
{
    if (hop_out == NULL || encoded_hops >= 15u) {
        return 0;
    }
    *hop_out = (uint8_t)(encoded_hops + 1u);
    return 1;
}

static uint16_t pdu_u16(const tavrn_validated_control_t *control,
                        uint8_t offset)
{
    return (uint16_t)control->pdu[offset] |
           ((uint16_t)control->pdu[(uint8_t)(offset + 1u)] << 8);
}

static uint16_t pdu_id(const tavrn_validated_control_t *control,
                       uint8_t offset, tavrn_identity_width_t width)
{
    return width == TAVRN_IDENTITY_SID8 ? control->pdu[offset] :
        pdu_u16(control, offset);
}

static void make_logical_id(tavrn_logical_id_t *logical_id,
                            tavrn_identity_width_t width, uint16_t value)
{
    logical_id->width = width;
    logical_id->value = value;
}

static int make_control_observation(
    const tavrn_rx_control_event_t *control_event,
    tavrn_router_frame_observation_t *frame_observation)
{
    const tavrn_validated_control_t *control;
    tavrn_router_evidence_subject_t *subject;
    tavrn_identity_width_t width;
    uint8_t width_bytes;
    uint8_t hop_count;

    if (control_event == NULL || frame_observation == NULL) {
        return 0;
    }
    control = &control_event->control;
    if (control->pdu_len < 6u || control->type != (tavrn_wire_type_t)control->pdu[4]) {
        return 0;
    }
    memset(frame_observation, 0, sizeof(*frame_observation));
    frame_observation->admission = TAVRN_ROUTER_FRAME_COMMITTED;
    frame_observation->frame_type = control->type;
    frame_observation->transmitter = control_event->transmitter;
    frame_observation->hack_status = TAVRN_HACK_ACCEPTED;

    /* E_RREP_ACK is direct-only: PDU[6] begins its receiver ID, not ttl_hops. */
    if (control->type == TAVRN_WIRE_E_RREP_ACK) {
        return 1;
    }
    if (control->pdu_len < 7u) {
        return 0;
    }
    width = (control->pdu[5] & 0x80u) != 0u ? TAVRN_IDENTITY_SID8 :
                                               TAVRN_IDENTITY_SID16;
    width_bytes = width == TAVRN_IDENTITY_SID8 ? 1u : 2u;
    subject = &frame_observation->subjects[0];
    if (!hop_plus_one((uint8_t)(control->pdu[6] & 0x0fu), &hop_count)) {
        /* The direct transmitter remains valid one-hop evidence.  Only the
         * encoded origin/reporter would be distance 16 and is omitted. */
        return 1;
    }
    subject->hop_present = 1u;
    subject->hop_count = hop_count;

    switch (control->type) {
    case TAVRN_WIRE_E_RREQ:
        if (control->pdu_len < (uint8_t)(13u + 2u * width_bytes)) {
            return 0;
        }
        frame_observation->subject_count = 1u;
        make_logical_id(&subject->logical_id, width, pdu_id(control, 7u, width));
        subject->role = TAVRN_ROUTER_EVIDENCE_ORIGIN;
        subject->serial_kind = TAVRN_ROUTER_EVIDENCE_SERIAL_NODE_SEQUENCE;
        subject->serial = pdu_u16(control, (uint8_t)(11u + 2u * width_bytes));
        return 1;
    case TAVRN_WIRE_E_RREP:
        if (control->pdu_len < (uint8_t)(13u + 3u * width_bytes)) {
            return 0;
        }
        frame_observation->subject_count = 1u;
        make_logical_id(&subject->logical_id, width,
                        pdu_id(control, (uint8_t)(7u + width_bytes), width));
        subject->role = TAVRN_ROUTER_EVIDENCE_ROUTE_DESTINATION;
        subject->serial_kind = TAVRN_ROUTER_EVIDENCE_SERIAL_DESTINATION_SEQUENCE;
        subject->serial = pdu_u16(control, (uint8_t)(7u + 2u * width_bytes));
        return 1;
    case TAVRN_WIRE_E_RERR:
        if (control->pdu_len < (uint8_t)(10u + 3u * width_bytes)) {
            return 0;
        }
        frame_observation->subject_count = 1u;
        make_logical_id(&subject->logical_id, width, pdu_id(control, 7u, width));
        subject->role = TAVRN_ROUTER_EVIDENCE_REPORTER;
        subject->serial_kind = TAVRN_ROUTER_EVIDENCE_SERIAL_NONE;
        return 1;
    default:
        return 0;
    }
}

static void observe_data(tavrn_router_t *router,
                         const tavrn_direct_peer_t *transmitter,
                         const tavrn_link_data_t *data, uint32_t now_ms)
{
    tavrn_router_frame_observation_t frame_observation;
    uint8_t hop_count;

    memset(&frame_observation, 0, sizeof(frame_observation));
    frame_observation.admission = TAVRN_ROUTER_FRAME_COMMITTED;
    frame_observation.frame_type = TAVRN_WIRE_DATA;
    frame_observation.transmitter = *transmitter;
    frame_observation.hack_status = TAVRN_HACK_ACCEPTED;
    if (hop_plus_one(data->hops, &hop_count)) {
        frame_observation.subject_count = 1u;
        frame_observation.subjects[0].logical_id = data->origin;
        frame_observation.subjects[0].role = TAVRN_ROUTER_EVIDENCE_ORIGIN;
        frame_observation.subjects[0].serial_kind = TAVRN_ROUTER_EVIDENCE_SERIAL_NONE;
        frame_observation.subjects[0].hop_present = 1u;
        frame_observation.subjects[0].hop_count = hop_count;
    }
    (void)tavrn_router_observe_frame(router, &frame_observation, now_ms);
}

static void observe_flood(tavrn_router_t *router,
                          const tavrn_decoded_frame_t *frame, uint32_t now_ms)
{
    tavrn_router_frame_observation_t frame_observation;
    uint8_t hop_count;

    memset(&frame_observation, 0, sizeof(frame_observation));
    frame_observation.admission = TAVRN_ROUTER_FRAME_COMMITTED;
    frame_observation.frame_type = TAVRN_WIRE_FLOOD;
    frame_observation.transmitter = frame->transmitter;
    frame_observation.hack_status = TAVRN_HACK_ACCEPTED;
    if (hop_plus_one(frame->detail.flood.hops, &hop_count)) {
        frame_observation.subject_count = 1u;
        frame_observation.subjects[0].logical_id = frame->detail.flood.origin;
        frame_observation.subjects[0].role = TAVRN_ROUTER_EVIDENCE_ORIGIN;
        frame_observation.subjects[0].serial_kind = TAVRN_ROUTER_EVIDENCE_SERIAL_NONE;
        frame_observation.subjects[0].hop_present = 1u;
        frame_observation.subjects[0].hop_count = hop_count;
    }
    (void)tavrn_router_observe_frame(router, &frame_observation, now_ms);
}

static tavrn_router_event_status_t consume_link_output(
    tavrn_router_t *router, const tavrn_link_event_t *event, uint32_t now_ms);

static tavrn_router_event_status_t release_transit_custody(
    tavrn_router_t *router, const tavrn_link_data_t *data, uint32_t now_ms)
{
    if (data->ownership != TAVRN_DATA_TRANSIT) {
        return TAVRN_ROUTER_EVENT_OK;
    }
    if (tavrn_link_v2_release_rx_custody(router->link, data, now_ms) !=
        TAVRN_LINK_RESOLVE_OK) {
        router->counters.release_invariant++;
        latch_router_fault(router,
                           TAVRN_ROUTER_FAULT_TRANSIT_CUSTODY_RELEASE_INVALID);
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    return TAVRN_ROUTER_EVENT_OK;
}

static int failure_slot_for_peer(const tavrn_router_t *router,
                                 const tavrn_direct_peer_t *peer)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_ROUTER_FAILURE_CAPACITY; index++) {
        if (router->failures[index].valid != 0u &&
            direct_peer_equal(&router->failures[index].peer, peer)) {
            return (int)index;
        }
    }
    return -1;
}

static int free_failure_slot(const tavrn_router_t *router)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_ROUTER_FAILURE_CAPACITY; index++) {
        if (router->failures[index].valid == 0u) {
            return (int)index;
        }
    }
    return -1;
}

static int failure_order_before(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) < 0;
}

static int oldest_failure_slot(const tavrn_router_t *router)
{
    int oldest = -1;
    uint8_t index;

    for (index = 0u; index < TAVRN_ROUTER_FAILURE_CAPACITY; index++) {
        if (router->failures[index].valid == 0u) {
            continue;
        }
        if (oldest < 0 || failure_order_before(
                              router->failures[index].admission_order,
                              router->failures[(uint8_t)oldest].admission_order)) {
            oldest = (int)index;
        }
    }
    return oldest;
}

static uint32_t next_failure_order(tavrn_router_t *router)
{
    router->next_failure_order++;
    if (router->next_failure_order == 0u) {
        router->next_failure_order++;
    }
    return router->next_failure_order;
}

static tavrn_router_event_status_t process_failure_slot(
    tavrn_router_t *router, uint8_t index, uint32_t now_ms)
{
    aodv_failure_status_t status;

    status = aodv_core_report_link_failure(
        router->aodv, &router->failures[index].peer, NULL,
        AODV_LINK_FAILURE_IMMEDIATE_RERR, now_ms);
    if (status == AODV_FAILURE_OK) {
        memset(&router->failures[index], 0, sizeof(router->failures[index]));
        return TAVRN_ROUTER_EVENT_OK;
    }
    if (status == AODV_FAILURE_BUSY) {
        return TAVRN_ROUTER_EVENT_BUSY;
    }
    router->counters.failure_invariant++;
    latch_router_fault(router, TAVRN_ROUTER_FAULT_FAILURE_REPORT_INVALID);
    return TAVRN_ROUTER_EVENT_INVALID;
}

static tavrn_router_event_status_t record_retry_exhausted(
    tavrn_router_t *router, const tavrn_owned_data_event_t *owned,
    uint32_t now_ms)
{
    const tavrn_direct_peer_t *peer;
    int slot;
    int oldest = oldest_failure_slot(router);

    if (owned == NULL) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    peer = &owned->next_hop;
    slot = failure_slot_for_peer(router, peer);

    if (slot >= 0) {
        return TAVRN_ROUTER_EVENT_BUSY;
    }
    slot = free_failure_slot(router);
    if (slot < 0) {
        router->counters.failure_set_overflow++;
        memset(&router->failure_overflow, 0, sizeof(router->failure_overflow));
        router->failure_overflow.owned = *owned;
        router->failure_overflow.admission_order = next_failure_order(router);
        router->failure_overflow.valid = 1u;
        latch_router_fault(router,
                           TAVRN_ROUTER_FAULT_FAILURE_OBLIGATION_OVERFLOW);
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    router->failures[slot].peer = *peer;
    router->failures[slot].admission_order = next_failure_order(router);
    router->failures[slot].valid = 1u;
    if (oldest >= 0) {
        return TAVRN_ROUTER_EVENT_BUSY;
    }
    return process_failure_slot(router, (uint8_t)slot, now_ms);
}

static tavrn_router_data_terminal_disposition_t tavrn_router_data_terminal_hook(
    tavrn_router_t *router, const tavrn_link_event_t *event, uint32_t now_ms)
{
    if (router->data_terminal_hook.handle == NULL) {
        return TAVRN_ROUTER_DATA_TERMINAL_DECLINED;
    }
    return router->data_terminal_hook.handle(router->data_terminal_hook.context,
                                             event, now_ms);
}

/* Keep the retry-specific internal name as the TC evidence boundary: legacy
 * maintenance checks continue to identify the failed next hop here, while the
 * installed policy receives the complete copied link event. */
static tavrn_router_data_terminal_disposition_t
tavrn_router_retry_exhausted_hook(tavrn_router_t *router,
                                  const tavrn_link_event_t *event,
                                  uint32_t now_ms)
{
    return tavrn_router_data_terminal_hook(router, event, now_ms);
}

static tavrn_router_event_status_t consume_owned_terminal(
    tavrn_router_t *router, const tavrn_link_event_t *event, uint32_t now_ms)
{
    const tavrn_owned_data_event_t *owned;
    tavrn_router_event_status_t result = TAVRN_ROUTER_EVENT_OK;
    tavrn_router_event_status_t release_status;
    tavrn_router_data_terminal_disposition_t disposition;
    const tavrn_adva_t *failed_next_hop;

    if (event == NULL) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    owned = &event->detail.owned_data;
    if (!link_data_is_valid(&owned->data) || !direct_peer_is_valid(&owned->next_hop)) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    failed_next_hop = &owned->next_hop.adva;
    disposition = event->type == TAVRN_LINK_EVENT_RETRY_EXHAUSTED ?
        tavrn_router_retry_exhausted_hook(router, event, now_ms) :
        tavrn_router_data_terminal_hook(router, event, now_ms);
    if (disposition != TAVRN_ROUTER_DATA_TERMINAL_DECLINED &&
        disposition != TAVRN_ROUTER_DATA_TERMINAL_OBSERVED &&
        disposition != TAVRN_ROUTER_DATA_TERMINAL_OWNED) {
        latch_router_fault(router, TAVRN_ROUTER_FAULT_DATA_TERMINAL_HOOK_INVALID);
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (disposition == TAVRN_ROUTER_DATA_TERMINAL_OWNED) {
        if (event->type != TAVRN_LINK_EVENT_RETRY_EXHAUSTED ||
            owned->data.ownership != TAVRN_DATA_TRANSIT) {
            latch_router_fault(router, TAVRN_ROUTER_FAULT_DATA_TERMINAL_HOOK_INVALID);
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        if (tavrn_link_v2_transfer_rx_custody_to_external(router->link,
                                                           &owned->data,
                                                           now_ms) !=
            TAVRN_LINK_RESOLVE_OK) {
            latch_router_fault(router, TAVRN_ROUTER_FAULT_DATA_TERMINAL_HOOK_INVALID);
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        return TAVRN_ROUTER_EVENT_OK;
    }
    if (event->type == TAVRN_LINK_EVENT_RETRY_EXHAUSTED) {

        (void)failed_next_hop;
        result = record_retry_exhausted(router, owned, now_ms);
        if (result == TAVRN_ROUTER_EVENT_INVALID) {
            return result;
        }
    }
    release_status = release_transit_custody(router, &owned->data, now_ms);
    if (release_status == TAVRN_ROUTER_EVENT_INVALID) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    return result;
}

static tavrn_router_event_status_t cancel_pre_ack_delivery(
    tavrn_router_t *router, uint32_t now_ms)
{
    tavrn_router_delivery_status_t status;

    if (router->delivery.state != TAVRN_ROUTER_DELIVERY_CANCEL_PENDING) {
        return TAVRN_ROUTER_EVENT_OK;
    }
    status = router->application.cancel(router->application.context,
                                        router->delivery.token,
                                        &router->delivery.transmitter,
                                        &router->delivery.data, now_ms);
    if (status == TAVRN_ROUTER_DELIVERY_OK) {
        memset(&router->delivery, 0, sizeof(router->delivery));
        return TAVRN_ROUTER_EVENT_OK;
    }
    if (status == TAVRN_ROUTER_DELIVERY_BUSY) {
        router->counters.application_cancel_busy++;
        return TAVRN_ROUTER_EVENT_BUSY;
    }
    if (status != TAVRN_ROUTER_DELIVERY_OK) {
        router->counters.application_cancel_invalid++;
        latch_router_fault(router, TAVRN_ROUTER_FAULT_DELIVERY_CANCEL_INVALID);
    }
    return TAVRN_ROUTER_EVENT_INVALID;
}

static tavrn_router_event_status_t try_pending_ingest(tavrn_router_t *router,
                                                       uint32_t now_ms)
{
    aodv_status_t status;

    if (router->pending_ingest.valid == 0u) {
        return TAVRN_ROUTER_EVENT_OK;
    }
    status = aodv_core_ingest_data(router->aodv, &router->pending_ingest.input,
                                   now_ms);
    if (status == AODV_STATUS_OK) {
        memset(&router->pending_ingest, 0, sizeof(router->pending_ingest));
        return TAVRN_ROUTER_EVENT_OK;
    }
    if (status == AODV_STATUS_BUSY) {
        router->counters.post_ack_commit_busy++;
        return TAVRN_ROUTER_EVENT_BUSY;
    }
    router->counters.post_ack_commit_invalid++;
    latch_router_fault(router, TAVRN_ROUTER_FAULT_POST_ACK_INGEST_INVALID);
    return TAVRN_ROUTER_EVENT_INVALID;
}

/* A failure report always precedes any new AODV/application/link admission.
 * Returning BUSY deliberately leaves the oldest obligation unchanged. */
static tavrn_router_event_status_t gate_oldest_failure(
    tavrn_router_t *router, uint32_t now_ms)
{
    int oldest;
    tavrn_router_event_status_t status;

    oldest = oldest_failure_slot(router);
    if (oldest < 0) {
        return TAVRN_ROUTER_EVENT_OK;
    }
    status = process_failure_slot(router, (uint8_t)oldest, now_ms);
    if (status == TAVRN_ROUTER_EVENT_OK && oldest_failure_slot(router) >= 0) {
        return TAVRN_ROUTER_EVENT_BUSY;
    }
    return status;
}

static tavrn_router_event_status_t resolve_candidate(
    tavrn_router_t *router, const tavrn_rx_data_candidate_t *candidate,
    tavrn_rx_decision_t decision, uint32_t now_ms,
    tavrn_link_resolve_status_t *primary_status_out)
{
    tavrn_link_event_t local_outcome;
    tavrn_link_resolve_status_t resolve_status;
    tavrn_router_event_status_t output_status = TAVRN_ROUTER_EVENT_OK;

    memset(&local_outcome, 0, sizeof(local_outcome));
    resolve_status = tavrn_link_v2_resolve_rx(router->link, candidate->token,
                                               decision, now_ms, &local_outcome);
    if (local_outcome.type != TAVRN_LINK_EVENT_NONE) {
        output_status = consume_link_output(router, &local_outcome, now_ms);
    }
    if (primary_status_out != NULL) {
        *primary_status_out = resolve_status;
    }
    if (resolve_status != TAVRN_LINK_RESOLVE_OK) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    return output_status;
}

static int final_destination_is_local(const tavrn_router_t *router,
                                      const tavrn_link_data_t *data)
{
    return logical_id_equal(&data->final_destination,
                             &router->aodv->config.local_peer.logical_id);
}

static tavrn_router_event_status_t rollback_candidate_reservation(
    tavrn_router_t *router, tavrn_router_candidate_reservation_token_t token,
    const tavrn_rx_data_candidate_t *candidate, uint32_t now_ms)
{
    if (router->candidate_reservation.rollback == NULL ||
        router->candidate_reservation.rollback(
            router->candidate_reservation.context, token, candidate, now_ms) !=
            TAVRN_ROUTER_CANDIDATE_COMPLETION_OK) {
        latch_router_fault(router, TAVRN_ROUTER_FAULT_CANDIDATE_ROLLBACK_INVALID);
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    return TAVRN_ROUTER_EVENT_OK;
}

static tavrn_router_event_status_t handle_data_candidate(
    tavrn_router_t *router, const tavrn_rx_data_candidate_t *candidate,
    uint32_t now_ms)
{
    aodv_data_input_t input;
    aodv_status_t probe_status;
    tavrn_router_event_status_t resolve_status;
    tavrn_router_event_status_t ingest_status;
    tavrn_router_delivery_status_t reserve_status;
    tavrn_router_delivery_token_t token = TAVRN_ROUTER_DELIVERY_TOKEN_NONE;
    tavrn_router_candidate_reservation_status_t candidate_reserve_status;
    tavrn_router_candidate_reservation_token_t candidate_token =
        TAVRN_ROUTER_CANDIDATE_RESERVATION_TOKEN_NONE;
    tavrn_link_resolve_status_t primary_resolve_status;
    uint8_t final_delivery;

    if (candidate == NULL ||
        !tavrn_link_v2_rx_candidate_matches(router->link, candidate)) {
        router->counters.stale_candidate++;
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (router_is_rejoining(router) ||
        direct_peer_route_barred(router, &candidate->transmitter)) {
        resolve_status = resolve_candidate(router, candidate, TAVRN_RX_BUSY, now_ms,
                                           NULL);
        if (resolve_status == TAVRN_ROUTER_EVENT_INVALID) {
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        return router_is_rejoining(router) ? TAVRN_ROUTER_EVENT_REJOINING :
                                             TAVRN_ROUTER_EVENT_BUSY;
    }
    ingest_status = gate_oldest_failure(router, now_ms);
    if (ingest_status != TAVRN_ROUTER_EVENT_OK) {
        if (ingest_status == TAVRN_ROUTER_EVENT_BUSY) {
            resolve_status = resolve_candidate(router, candidate, TAVRN_RX_BUSY,
                                               now_ms, NULL);
            return resolve_status == TAVRN_ROUTER_EVENT_INVALID ?
                TAVRN_ROUTER_EVENT_INVALID : TAVRN_ROUTER_EVENT_BUSY;
        }
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (router->pending_ingest.valid != 0u ||
        (final_destination_is_local(router, &candidate->data) &&
         router->delivery.state != TAVRN_ROUTER_DELIVERY_NONE)) {
        resolve_status = resolve_candidate(router, candidate, TAVRN_RX_BUSY, now_ms,
                                           NULL);
        return resolve_status == TAVRN_ROUTER_EVENT_INVALID ?
            TAVRN_ROUTER_EVENT_INVALID : TAVRN_ROUTER_EVENT_BUSY;
    }

    if (router->candidate_reservation.reserve != NULL) {
        candidate_reserve_status = router->candidate_reservation.reserve(
            router->candidate_reservation.context, candidate, &candidate_token, now_ms);
        if (candidate_reserve_status == TAVRN_ROUTER_CANDIDATE_NOT_APPLICABLE) {
            if (candidate_token != TAVRN_ROUTER_CANDIDATE_RESERVATION_TOKEN_NONE) {
                latch_router_fault(
                    router, TAVRN_ROUTER_FAULT_CANDIDATE_RESERVATION_INVALID);
                return TAVRN_ROUTER_EVENT_INVALID;
            }
        } else if (candidate_reserve_status == TAVRN_ROUTER_CANDIDATE_BUSY) {
            if (candidate_token != TAVRN_ROUTER_CANDIDATE_RESERVATION_TOKEN_NONE) {
                latch_router_fault(
                    router, TAVRN_ROUTER_FAULT_CANDIDATE_RESERVATION_INVALID);
                return TAVRN_ROUTER_EVENT_INVALID;
            }
            resolve_status = resolve_candidate(router, candidate, TAVRN_RX_BUSY, now_ms,
                                               NULL);
            return resolve_status == TAVRN_ROUTER_EVENT_INVALID ?
                TAVRN_ROUTER_EVENT_INVALID : TAVRN_ROUTER_EVENT_BUSY;
        } else if (candidate_reserve_status == TAVRN_ROUTER_CANDIDATE_INVALID) {
            (void)resolve_candidate(router, candidate, TAVRN_RX_REJECTED, now_ms, NULL);
            latch_router_fault(router,
                               TAVRN_ROUTER_FAULT_CANDIDATE_RESERVATION_INVALID);
            return TAVRN_ROUTER_EVENT_INVALID;
        } else if (candidate_reserve_status == TAVRN_ROUTER_CANDIDATE_RESERVED) {
            if (candidate_token == TAVRN_ROUTER_CANDIDATE_RESERVATION_TOKEN_NONE ||
                router->candidate_reservation.commit == NULL ||
                router->candidate_reservation.rollback == NULL) {
                latch_router_fault(
                    router, candidate_token ==
                            TAVRN_ROUTER_CANDIDATE_RESERVATION_TOKEN_NONE ?
                        TAVRN_ROUTER_FAULT_CANDIDATE_RESERVATION_TOKEN_ZERO :
                        TAVRN_ROUTER_FAULT_CANDIDATE_RESERVATION_INVALID);
                return TAVRN_ROUTER_EVENT_INVALID;
            }
            resolve_status = resolve_candidate(router, candidate, TAVRN_RX_ACCEPTED,
                                               now_ms, &primary_resolve_status);
            if (primary_resolve_status != TAVRN_LINK_RESOLVE_OK) {
                (void)rollback_candidate_reservation(router, candidate_token, candidate,
                                                      now_ms);
                return TAVRN_ROUTER_EVENT_INVALID;
            }
            if (router->candidate_reservation.commit(
                    router->candidate_reservation.context, candidate_token, candidate,
                    now_ms) != TAVRN_ROUTER_CANDIDATE_COMPLETION_OK) {
                latch_router_fault(router, TAVRN_ROUTER_FAULT_CANDIDATE_COMMIT_INVALID);
                return TAVRN_ROUTER_EVENT_INVALID;
            }
            if (tavrn_link_v2_transfer_rx_custody_to_external(router->link,
                                                               &candidate->data,
                                                               now_ms) !=
                TAVRN_LINK_RESOLVE_OK) {
                latch_router_fault(router, TAVRN_ROUTER_FAULT_CANDIDATE_COMMIT_INVALID);
                return TAVRN_ROUTER_EVENT_INVALID;
            }
            observe_data(router, &candidate->transmitter, &candidate->data, now_ms);
            return resolve_status;
        } else {
            latch_router_fault(router,
                               TAVRN_ROUTER_FAULT_CANDIDATE_RESERVATION_INVALID);
            return TAVRN_ROUTER_EVENT_INVALID;
        }
    }

    memset(&input, 0, sizeof(input));
    input.transmitter = candidate->transmitter;
    input.data = candidate->data;
    probe_status = aodv_core_probe_data(router->aodv, &input, now_ms);
    if (probe_status != AODV_STATUS_OK) {
        resolve_status = resolve_candidate(
            router, candidate,
            probe_status == AODV_STATUS_BUSY ? TAVRN_RX_BUSY : TAVRN_RX_REJECTED,
            now_ms, NULL);
        if (resolve_status == TAVRN_ROUTER_EVENT_INVALID) {
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        return probe_status == AODV_STATUS_BUSY ? TAVRN_ROUTER_EVENT_BUSY :
                                                   TAVRN_ROUTER_EVENT_IGNORED;
    }

    final_delivery = final_destination_is_local(router, &candidate->data) ? 1u : 0u;
    if (final_delivery != 0u) {
        if (router->application.reserve == NULL || router->application.commit == NULL ||
            router->application.cancel == NULL) {
            resolve_status = resolve_candidate(router, candidate, TAVRN_RX_BUSY, now_ms,
                                               NULL);
            return resolve_status == TAVRN_ROUTER_EVENT_INVALID ?
                TAVRN_ROUTER_EVENT_INVALID : TAVRN_ROUTER_EVENT_BUSY;
        }
        reserve_status = router->application.reserve(
            router->application.context, &candidate->transmitter, &candidate->data,
            &token, now_ms);
        if (reserve_status == TAVRN_ROUTER_DELIVERY_OK &&
            token == TAVRN_ROUTER_DELIVERY_TOKEN_NONE) {
            router->counters.application_reserve_invalid++;
            router->delivery.transmitter = candidate->transmitter;
            router->delivery.data = candidate->data;
            router->delivery.state = TAVRN_ROUTER_DELIVERY_FAULTED_PRE_ACK;
            latch_router_fault(router,
                               TAVRN_ROUTER_FAULT_APPLICATION_RESERVE_TOKEN_ZERO);
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        if (reserve_status != TAVRN_ROUTER_DELIVERY_OK) {
            if (reserve_status == TAVRN_ROUTER_DELIVERY_BUSY) {
                router->counters.application_reserve_busy++;
            } else {
                router->counters.application_reserve_invalid++;
            }
            resolve_status = resolve_candidate(
                router, candidate,
                reserve_status == TAVRN_ROUTER_DELIVERY_BUSY ? TAVRN_RX_BUSY :
                                                               TAVRN_RX_REJECTED,
                now_ms, NULL);
            if (resolve_status == TAVRN_ROUTER_EVENT_INVALID) {
                return TAVRN_ROUTER_EVENT_INVALID;
            }
            return reserve_status == TAVRN_ROUTER_DELIVERY_BUSY ?
                TAVRN_ROUTER_EVENT_BUSY : TAVRN_ROUTER_EVENT_IGNORED;
        }
        router->delivery.transmitter = candidate->transmitter;
        router->delivery.data = candidate->data;
        router->delivery.token = token;
        router->delivery.state = TAVRN_ROUTER_DELIVERY_RESERVED_PRE_ACK;
    }

    /* This is the router-owned, non-evictable post-ACK reservation.  It holds
     * the exact transit or final input until the one core commit succeeds. */
    router->pending_ingest.input = input;
    router->pending_ingest.valid = 1u;
    resolve_status = resolve_candidate(router, candidate, TAVRN_RX_ACCEPTED, now_ms,
                                       &primary_resolve_status);
    if (primary_resolve_status != TAVRN_LINK_RESOLVE_OK) {
        if (final_delivery != 0u) {
            router->delivery.state = TAVRN_ROUTER_DELIVERY_CANCEL_PENDING;
        }
        (void)cancel_pre_ack_delivery(router, now_ms);
        memset(&router->pending_ingest, 0, sizeof(router->pending_ingest));
        return TAVRN_ROUTER_EVENT_INVALID;
    }

    if (final_delivery != 0u) {
        router->delivery.state = TAVRN_ROUTER_DELIVERY_POST_ACK_COMMIT;
    }

    /* Link acceptance is the semantic admission point.  The transmitter is
     * always reported separately at one hop; an unrepresentable origin at 16
     * hops is intentionally omitted by observe_data(). */
    observe_data(router, &candidate->transmitter, &candidate->data, now_ms);
    ingest_status = try_pending_ingest(router, now_ms);
    if (ingest_status == TAVRN_ROUTER_EVENT_INVALID) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (resolve_status == TAVRN_ROUTER_EVENT_INVALID) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (ingest_status == TAVRN_ROUTER_EVENT_BUSY ||
        resolve_status == TAVRN_ROUTER_EVENT_BUSY) {
        return TAVRN_ROUTER_EVENT_BUSY;
    }
    return TAVRN_ROUTER_EVENT_OK;
}

static tavrn_router_event_status_t handle_control(
    tavrn_router_t *router, const tavrn_rx_control_event_t *control_event,
    uint32_t now_ms)
{
    aodv_control_input_t input;
    aodv_status_t ingest_status;
    tavrn_router_event_status_t failure_status;
    tavrn_router_frame_observation_t frame_observation;

    if (control_event == NULL) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    /* These full-identity controls are admitted by the common wire/link seam
     * but consumed by the optional FULL mentorship policy.  They must never
     * fall through to the SID-width AODV parser. */
    if (control_event->control.type == TAVRN_WIRE_SYNC_OFFER ||
        control_event->control.type == TAVRN_WIRE_SYNC_PULL ||
        control_event->control.type == TAVRN_WIRE_SYNC_DATA ||
        control_event->control.type == TAVRN_WIRE_TC_UPDATE) {
        return TAVRN_ROUTER_EVENT_OK;
    }
    if (router->incarnation.enabled != 0u &&
        control_event->control.type == TAVRN_WIRE_HELLO) {
        if ((control_event->control.pdu[5] & 0x40u) != 0u) {
            return handle_direct_bootstrap(router, control_event, now_ms);
        }
        if (router_is_rejoining(router)) {
            return TAVRN_ROUTER_EVENT_REJOINING;
        }
        /* N=0 remains ordinary HELLO handling; it neither reaches AODV nor
         * changes the direct incarnation tuple. */
        return TAVRN_ROUTER_EVENT_IGNORED;
    }
    if (router_is_rejoining(router)) {
        return TAVRN_ROUTER_EVENT_REJOINING;
    }
    if (direct_peer_route_barred(router, &control_event->transmitter)) {
        return TAVRN_ROUTER_EVENT_BUSY;
    }

    failure_status = gate_oldest_failure(router, now_ms);
    if (failure_status != TAVRN_ROUTER_EVENT_OK) {
        return failure_status;
    }
    memset(&input, 0, sizeof(input));
    input.transmitter = control_event->transmitter;
    input.control = control_event->control;
    ingest_status = aodv_core_ingest_control(router->aodv, &input, now_ms);
    if (ingest_status == AODV_STATUS_OK || ingest_status == AODV_STATUS_DUPLICATE) {
        int peer_index = incarnation_peer_index(router,
                                                 &control_event->transmitter.adva);

        if (peer_index >= 0 && router->incarnation.peers[(uint8_t)peer_index]
                .control_dedupe_count != 0xffu) {
            router->incarnation.peers[(uint8_t)peer_index].control_dedupe_count++;
        }
        if (make_control_observation(control_event, &frame_observation)) {
            (void)tavrn_router_observe_frame(router, &frame_observation, now_ms);
        }
        return TAVRN_ROUTER_EVENT_OK;
    }
    if (ingest_status == AODV_STATUS_BUSY) {
        return TAVRN_ROUTER_EVENT_BUSY;
    }
    return ingest_status == AODV_STATUS_INVALID ? TAVRN_ROUTER_EVENT_INVALID :
                                                  TAVRN_ROUTER_EVENT_IGNORED;
}

aodv_status_t tavrn_router_ingest_rrep_for_attempt(
    tavrn_router_t *router, const tavrn_rx_control_event_t *control_event,
    const aodv_rreq_attempt_t *attempt, uint32_t now_ms)
{
    aodv_control_input_t input;
    aodv_status_t ingest_status;
    tavrn_router_event_status_t failure_status;
    tavrn_router_frame_observation_t frame_observation;

    if (!router_is_usable(router) || control_event == NULL || attempt == NULL ||
        control_event->control.type != TAVRN_WIRE_E_RREP) {
        return AODV_STATUS_INVALID;
    }
    if (router_is_rejoining(router)) {
        return AODV_STATUS_REJOINING;
    }
    if (direct_peer_route_barred(router, &control_event->transmitter)) {
        return AODV_STATUS_BUSY;
    }
    failure_status = gate_oldest_failure(router, now_ms);
    if (failure_status == TAVRN_ROUTER_EVENT_BUSY) {
        return AODV_STATUS_BUSY;
    }
    if (failure_status != TAVRN_ROUTER_EVENT_OK) {
        return AODV_STATUS_INVALID;
    }
    memset(&input, 0, sizeof(input));
    input.transmitter = control_event->transmitter;
    input.control = control_event->control;
    ingest_status = aodv_core_ingest_rrep_for_attempt(router->aodv, &input,
                                                       attempt, now_ms);
    if (ingest_status != AODV_STATUS_OK && ingest_status != AODV_STATUS_DUPLICATE) {
        return ingest_status;
    }
    {
        int peer_index = incarnation_peer_index(router, &control_event->transmitter.adva);

        if (peer_index >= 0 && router->incarnation.peers[(uint8_t)peer_index]
                .control_dedupe_count != 0xffu) {
            router->incarnation.peers[(uint8_t)peer_index].control_dedupe_count++;
        }
    }
    if (make_control_observation(control_event, &frame_observation)) {
        (void)tavrn_router_observe_frame(router, &frame_observation, now_ms);
    }
    return ingest_status;
}

static tavrn_router_event_status_t consume_link_output(
    tavrn_router_t *router, const tavrn_link_event_t *event, uint32_t now_ms)
{
    tavrn_router_frame_observation_t frame_observation;
    tavrn_router_event_status_t release_status;
    tavrn_router_data_terminal_disposition_t disposition;

    if (!router_is_usable(router) || event == NULL) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    switch (event->type) {
    case TAVRN_LINK_EVENT_RX_DATA_CANDIDATE:
        return handle_data_candidate(router, &event->detail.candidate, now_ms);
    case TAVRN_LINK_EVENT_RX_CONTROL:
        return handle_control(router, &event->detail.control, now_ms);
    case TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED:
        if (!link_data_is_valid(&event->detail.transferred_data.data) ||
            !direct_peer_is_valid(&event->detail.transferred_data.next_hop)) {
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        disposition = tavrn_router_data_terminal_hook(router, event, now_ms);
        if (disposition != TAVRN_ROUTER_DATA_TERMINAL_DECLINED &&
            disposition != TAVRN_ROUTER_DATA_TERMINAL_OBSERVED) {
            latch_router_fault(router, TAVRN_ROUTER_FAULT_DATA_TERMINAL_HOOK_INVALID);
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        if (event->detail.transferred_data.status != TAVRN_HACK_ACCEPTED &&
            event->detail.transferred_data.status != TAVRN_HACK_DUPLICATE) {
            release_status = release_transit_custody(
                router, &event->detail.transferred_data.data, now_ms);
            return release_status == TAVRN_ROUTER_EVENT_INVALID ?
                TAVRN_ROUTER_EVENT_INVALID : TAVRN_ROUTER_EVENT_INVALID;
        }
        memset(&frame_observation, 0, sizeof(frame_observation));
        frame_observation.admission = TAVRN_ROUTER_FRAME_COMMITTED;
        frame_observation.frame_type = TAVRN_WIRE_HACK;
        frame_observation.transmitter = event->detail.transferred_data.next_hop;
        frame_observation.hack_status = event->detail.transferred_data.status;
        (void)tavrn_router_observe_frame(router, &frame_observation, now_ms);
        return release_transit_custody(router, &event->detail.transferred_data.data,
                                       now_ms);
    case TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED:
    case TAVRN_LINK_EVENT_CUSTODY_BUSY_EXPIRED:
    case TAVRN_LINK_EVENT_CUSTODY_REJECTED:
    case TAVRN_LINK_EVENT_RETRY_EXHAUSTED:
    case TAVRN_LINK_EVENT_RADIO_FAULT_TERMINAL:
    case TAVRN_LINK_EVENT_SERVICE_FAULT_TERMINAL:
        return consume_owned_terminal(router, event, now_ms);
    case TAVRN_LINK_EVENT_NONE:
        return TAVRN_ROUTER_EVENT_IGNORED;
    default:
        return TAVRN_ROUTER_EVENT_INVALID;
    }
}

tavrn_router_init_status_t tavrn_router_init(
    tavrn_router_t *router, tavrn_link_v2_t *link, aodv_core_t *aodv,
    const tavrn_router_augmentation_hooks_t *augmentation_or_null)
{
    if (router == NULL || link == NULL || aodv == NULL) {
        return TAVRN_ROUTER_INIT_INVALID_ARGUMENT;
    }
    memset(router, 0, sizeof(*router));
    router->link = link;
    router->aodv = aodv;
    if (augmentation_or_null != NULL) {
        router->augmentation = *augmentation_or_null;
    }
    return TAVRN_ROUTER_INIT_OK;
}

tavrn_router_incarnation_status_t tavrn_router_init_with_incarnation(
    tavrn_router_t *router, tavrn_link_v2_t *link, aodv_core_t *aodv,
    const tavrn_router_augmentation_hooks_t *augmentation_or_null,
    const tavrn_router_incarnation_config_t *config, uint32_t now_ms)
{
    tavrn_router_incarnation_status_t hello_status;

    if (!incarnation_config_is_valid(config)) {
        return TAVRN_ROUTER_INCARNATION_INVALID_CONFIG;
    }
    if (tavrn_router_init(router, link, aodv, augmentation_or_null) !=
        TAVRN_ROUTER_INIT_OK) {
        return TAVRN_ROUTER_INCARNATION_INVALID;
    }
    router->incarnation.enabled = 1u;
    router->incarnation.config = *config;
    router->incarnation.snapshot.state = TAVRN_ROUTER_INCARNATION_REJOINING;
    router->incarnation.snapshot.boot_nonce = config->boot_nonce;
    hello_status = enqueue_local_hello(router, 1u, now_ms);
    if (hello_status != TAVRN_ROUTER_INCARNATION_OK) {
        memset(&router->incarnation, 0, sizeof(router->incarnation));
        return hello_status;
    }
    router->incarnation.next_bootstrap_announce_ms =
        now_ms + (config->reboot_announce_ms > 1u ?
                  config->reboot_announce_ms / 2u : 1u);
    return TAVRN_ROUTER_INCARNATION_OK;
}

tavrn_router_application_hook_status_t tavrn_router_set_application_hooks(
    tavrn_router_t *router,
    const tavrn_router_application_hooks_t *application_or_null)
{
    if (!router_is_initialized(router) ||
        router->fault_reason != TAVRN_ROUTER_FAULT_NONE) {
        return TAVRN_ROUTER_APPLICATION_HOOK_INVALID;
    }
    if (router->pending_ingest.valid != 0u ||
        router->delivery.state != TAVRN_ROUTER_DELIVERY_NONE ||
        router->retained_action_valid != 0u) {
        return TAVRN_ROUTER_APPLICATION_HOOK_BUSY;
    }
    if (application_or_null != NULL &&
        (application_or_null->reserve == NULL || application_or_null->commit == NULL ||
         application_or_null->cancel == NULL)) {
        return TAVRN_ROUTER_APPLICATION_HOOK_INVALID;
    }
    memset(&router->application, 0, sizeof(router->application));
    if (application_or_null != NULL) {
        router->application = *application_or_null;
    }
    return TAVRN_ROUTER_APPLICATION_HOOK_OK;
}

tavrn_router_event_status_t tavrn_router_set_candidate_reservation_port(
    tavrn_router_t *router,
    const tavrn_router_candidate_reservation_port_t *port_or_null)
{
    if (!router_is_usable(router) ||
        (port_or_null != NULL &&
         (port_or_null->reserve == NULL || port_or_null->commit == NULL ||
          port_or_null->rollback == NULL))) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    memset(&router->candidate_reservation, 0,
           sizeof(router->candidate_reservation));
    if (port_or_null != NULL) {
        router->candidate_reservation = *port_or_null;
    }
    return TAVRN_ROUTER_EVENT_OK;
}

tavrn_router_event_status_t tavrn_router_set_control_interceptor(
    tavrn_router_t *router,
    const tavrn_router_control_interceptor_t *interceptor_or_null)
{
    if (!router_is_usable(router)) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (interceptor_or_null != NULL && interceptor_or_null->receive == NULL) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    memset(&router->control_interceptor, 0, sizeof(router->control_interceptor));
    if (interceptor_or_null != NULL) {
        router->control_interceptor = *interceptor_or_null;
    }
    return TAVRN_ROUTER_EVENT_OK;
}

tavrn_router_event_status_t tavrn_router_set_control_augmentation(
    tavrn_router_t *router,
    const tavrn_router_control_augmentation_t *augmentation_or_null)
{
    if (!router_is_usable(router)) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    memset(&router->control_augmentation, 0, sizeof(router->control_augmentation));
    if (augmentation_or_null != NULL) {
        router->control_augmentation = *augmentation_or_null;
    }
    return TAVRN_ROUTER_EVENT_OK;
}

tavrn_router_event_status_t tavrn_router_set_data_terminal_hook(
    tavrn_router_t *router, const tavrn_router_data_terminal_hook_t *hook_or_null)
{
    if (!router_is_usable(router) ||
        (hook_or_null != NULL && hook_or_null->handle == NULL)) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    memset(&router->data_terminal_hook, 0, sizeof(router->data_terminal_hook));
    if (hook_or_null != NULL) {
        router->data_terminal_hook = *hook_or_null;
    }
    return TAVRN_ROUTER_EVENT_OK;
}

tavrn_router_event_status_t tavrn_router_handle_link_event(
    tavrn_router_t *router, const tavrn_link_event_t *event, uint32_t now_ms)
{
    return consume_link_output(router, event, now_ms);
}

static int ordinary_hello_router_ready(const tavrn_router_t *router)
{
    return router_is_usable(router) && router->incarnation.enabled != 0u &&
        router->incarnation.config.feature_level ==
            TAVRN_ROUTER_FEATURE_FULL_TAVRN &&
        router->incarnation.snapshot.state ==
            TAVRN_ROUTER_INCARNATION_ESTABLISHED &&
        router->link->config.local_peer.logical_id.width == TAVRN_IDENTITY_SID8;
}

static void build_ordinary_hello(tavrn_validated_control_t *control,
                                  const tavrn_router_t *router,
                                  uint16_t node_sequence,
                                  uint8_t known_remote_count)
{
    memset(control, 0, sizeof(*control));
    control->type = TAVRN_WIRE_HELLO;
    control->pdu_len = 20u;
    control->pdu[0] = 0x54u;
    control->pdu[1] = 0x52u;
    control->pdu[2] = 0x02u;
    control->pdu[3] = router->link->config.network_id;
    control->pdu[4] = TAVRN_WIRE_HELLO;
    control->pdu[5] = 0x80u;
    control->pdu[6] = 0x10u;
    control->pdu[7] = 0xffu;
    control->pdu[8] = 0xffu;
    memcpy(&control->pdu[9], router->link->config.local_peer.adva.bytes,
           TAVRN_ADVA_LEN);
    control->pdu[15] = (uint8_t)node_sequence;
    control->pdu[16] = (uint8_t)(node_sequence >> 8);
    control->pdu[17] = known_remote_count;
}

static int ordinary_hello_matches_router(const tavrn_router_t *router,
                                         const tavrn_validated_control_t *control)
{
    tavrn_validated_control_t expected;
    uint16_t node_sequence;

    if (router == NULL || router->link == NULL || control == NULL ||
        control->type != TAVRN_WIRE_HELLO || control->pdu_len != 20u ||
        control->pdu[17] > 15u || control->pdu[18] != 0u ||
        control->pdu[19] != 0u) {
        return 0;
    }
    node_sequence = (uint16_t)control->pdu[15] |
        ((uint16_t)control->pdu[16] << 8);
    build_ordinary_hello(&expected, router, node_sequence, control->pdu[17]);
    return memcmp(control, &expected, sizeof(expected)) == 0;
}

tavrn_router_hello_status_t tavrn_router_build_ordinary_hello(
    const tavrn_router_t *router, uint16_t node_sequence,
    uint8_t known_remote_count,
    tavrn_validated_control_t *control_out)
{
    if (control_out != NULL) {
        memset(control_out, 0, sizeof(*control_out));
    }
    if (control_out == NULL || router == NULL || router->link == NULL) {
        return TAVRN_ROUTER_HELLO_INVALID;
    }
    if (!ordinary_hello_router_ready(router) ||
        known_remote_count > 15u) {
        return TAVRN_ROUTER_HELLO_GATED;
    }
    build_ordinary_hello(control_out, router, node_sequence, known_remote_count);
    return TAVRN_ROUTER_HELLO_OK;
}

tavrn_router_hello_status_t tavrn_router_enqueue_ordinary_hello(
    tavrn_router_t *router, const tavrn_validated_control_t *control,
    uint32_t now_ms)
{
    tavrn_link_event_t local_outcome;
    tavrn_link_send_status_t link_status;
    tavrn_router_event_status_t outcome_status = TAVRN_ROUTER_EVENT_IGNORED;

    if (router == NULL || router->link == NULL || control == NULL) {
        return TAVRN_ROUTER_HELLO_INVALID;
    }
    if (!ordinary_hello_router_ready(router)) {
        return TAVRN_ROUTER_HELLO_GATED;
    }
    if (!ordinary_hello_matches_router(router, control)) {
        return TAVRN_ROUTER_HELLO_INVALID;
    }
    memset(&local_outcome, 0, sizeof(local_outcome));
    link_status = tavrn_link_v2_send_control(router->link, control, NULL, 0u,
                                              now_ms, &local_outcome);
    if (local_outcome.type != TAVRN_LINK_EVENT_NONE) {
        outcome_status = tavrn_router_handle_link_event(router, &local_outcome, now_ms);
    }
    if (outcome_status == TAVRN_ROUTER_EVENT_INVALID) {
        return TAVRN_ROUTER_HELLO_INVALID;
    }
    if (link_status == TAVRN_LINK_SEND_OK) {
        return TAVRN_ROUTER_HELLO_OK;
    }
    if (link_status == TAVRN_LINK_SEND_BUSY ||
        link_status == TAVRN_LINK_SEND_NO_SLOT) {
        return TAVRN_ROUTER_HELLO_BUSY;
    }
    return TAVRN_ROUTER_HELLO_INVALID;
}

tavrn_router_hello_status_t tavrn_router_cancel_ordinary_hello(
    tavrn_router_t *router, const tavrn_validated_control_t *control)
{
    if (router == NULL || router->link == NULL || control == NULL ||
        !ordinary_hello_matches_router(router, control)) {
        return TAVRN_ROUTER_HELLO_INVALID;
    }
    return tavrn_link_v2_cancel_queued_control(router->link, control) ==
            TAVRN_LINK_RESOLVE_OK ? TAVRN_ROUTER_HELLO_OK :
                                       TAVRN_ROUTER_HELLO_INVALID;
}

tavrn_router_tracked_rreq_status_t tavrn_router_enqueue_tracked_rreq(
    tavrn_router_t *router, const aodv_action_t *action, uint16_t token,
    uint32_t now_ms, uint16_t *evicted_token_out)
{
    const aodv_control_action_t *control_action;
    const aodv_rreq_attempt_t *attempt;
    const tavrn_validated_control_t *control;
    tavrn_link_send_status_t link_status;
    ble_mesh_tx_token_t evicted = BLE_MESH_TX_TOKEN_NONE;

    if (evicted_token_out != NULL) {
        *evicted_token_out = BLE_MESH_TX_TOKEN_NONE;
    }
    if (!ordinary_hello_router_ready(router) || action == NULL || token < 0x8000u ||
        action->type != AODV_ACTION_SEND_RREQ) {
        return TAVRN_ROUTER_TRACKED_RREQ_INVALID;
    }
    control_action = &action->detail.control;
    attempt = &control_action->rreq_attempt;
    control = &control_action->control;
    if (control_action->controlled_flood != 1u ||
        control_action->rreq_attempt_present != AODV_RREQ_ATTEMPT_PRESENT ||
        attempt->origin.width != TAVRN_IDENTITY_SID8 ||
        attempt->destination.width != TAVRN_IDENTITY_SID8 ||
        attempt->origin.value != router->link->config.local_peer.logical_id.value ||
        attempt->request_id == 0u || attempt->initial_scope == 0u ||
        attempt->current_scope != attempt->initial_scope ||
        control->type != TAVRN_WIRE_E_RREQ || control->pdu_len != 15u ||
        (control->pdu[5] & 0x80u) == 0u || control->pdu[6] !=
            (uint8_t)(attempt->current_scope << 4) ||
        control->pdu[7] != (uint8_t)attempt->origin.value ||
        ((uint16_t)control->pdu[8] | ((uint16_t)control->pdu[9] << 8)) !=
            attempt->request_id || control->pdu[10] != (uint8_t)attempt->destination.value) {
        return TAVRN_ROUTER_TRACKED_RREQ_INVALID;
    }
    link_status = tavrn_link_v2_send_tracked_control(router->link, control, NULL,
                                                      token, now_ms, &evicted);
    if (evicted_token_out != NULL) {
        *evicted_token_out = evicted;
    }
    if (link_status == TAVRN_LINK_SEND_OK) {
        tavrn_router_note_local_broadcast(router, now_ms);
        return TAVRN_ROUTER_TRACKED_RREQ_OK;
    }
    if (link_status == TAVRN_LINK_SEND_BUSY || link_status == TAVRN_LINK_SEND_NO_SLOT) {
        return TAVRN_ROUTER_TRACKED_RREQ_BUSY;
    }
    if (link_status == TAVRN_LINK_SEND_LOCAL_NOT_ATTEMPTED) {
        return TAVRN_ROUTER_TRACKED_RREQ_LOCAL_NOT_ATTEMPTED;
    }
    return TAVRN_ROUTER_TRACKED_RREQ_INVALID;
}

static void clear_retained_action(tavrn_router_t *router);
static int action_is_control(aodv_action_type_t type);

tavrn_router_event_status_t tavrn_router_retry_retained_control(
    tavrn_router_t *router, const tavrn_validated_control_t *base,
    const tavrn_validated_control_t *sent,
    const tavrn_direct_peer_t *next_hop_or_null, uint8_t controlled_flood,
    uint32_t now_ms)
{
    const aodv_control_action_t *retained = NULL;
    tavrn_link_event_t local_outcome;
    tavrn_link_send_status_t link_status;
    tavrn_router_event_status_t outcome_status = TAVRN_ROUTER_EVENT_IGNORED;
    tavrn_router_control_augmentation_status_t augmentation_status =
        TAVRN_ROUTER_CONTROL_AUGMENTATION_OK;
    ble_mesh_tx_token_t scheduler_token = BLE_MESH_TX_TOKEN_NONE;

    if (!router_is_usable(router) || base == NULL || sent == NULL ||
        controlled_flood > 1u ||
        (controlled_flood == 0u && next_hop_or_null == NULL)) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (router->retained_action_valid != 0u) {
        if (!action_is_control(router->retained_action.type)) {
            return TAVRN_ROUTER_EVENT_BUSY;
        }
        retained = &router->retained_action.detail.control;
        if (!controls_equal(base, &retained->control) ||
            retained->controlled_flood != controlled_flood ||
            (controlled_flood == 0u &&
             memcmp(next_hop_or_null, &retained->next_hop,
                    sizeof(*next_hop_or_null)) != 0)) {
            return TAVRN_ROUTER_EVENT_INVALID;
        }
    }
    memset(&local_outcome, 0, sizeof(local_outcome));
    link_status = tavrn_link_v2_send_control_tracked(
        router->link, sent, next_hop_or_null, controlled_flood, now_ms,
        &local_outcome, &scheduler_token);
    if (router->control_augmentation.admitted != NULL) {
        augmentation_status = router->control_augmentation.admitted(
            router->control_augmentation.context, base, sent, next_hop_or_null,
            controlled_flood, link_status, scheduler_token, now_ms);
    }
    if (local_outcome.type != TAVRN_LINK_EVENT_NONE) {
        outcome_status = tavrn_router_handle_link_event(router, &local_outcome, now_ms);
    }
    if (link_status == TAVRN_LINK_SEND_BUSY || link_status == TAVRN_LINK_SEND_NO_SLOT) {
        return augmentation_status == TAVRN_ROUTER_CONTROL_AUGMENTATION_INVALID ||
               outcome_status == TAVRN_ROUTER_EVENT_INVALID ?
            TAVRN_ROUTER_EVENT_INVALID : TAVRN_ROUTER_EVENT_BUSY;
    }
    if (link_status != TAVRN_LINK_SEND_OK ||
        augmentation_status != TAVRN_ROUTER_CONTROL_AUGMENTATION_OK ||
        outcome_status == TAVRN_ROUTER_EVENT_INVALID) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (controlled_flood != 0u) {
        tavrn_router_note_local_broadcast(router, now_ms);
    }
    if (retained != NULL && retained->token != 0u) {
        if (aodv_core_mark_action_sent(router->aodv, retained->token, now_ms) !=
            AODV_STATUS_OK) {
            latch_router_fault(router, TAVRN_ROUTER_FAULT_CONTROL_CANCEL_INVALID);
            return TAVRN_ROUTER_EVENT_INVALID;
        }
    }
    if (retained != NULL) {
        clear_retained_action(router);
    }
    return outcome_status;
}

tavrn_router_route_status_t tavrn_router_route_to_subject(
    const tavrn_router_t *router, const tavrn_logical_id_t *subject,
    uint32_t now_ms, aodv_route_snapshot_t *route_out)
{
    aodv_route_snapshot_t route;

    if (route_out != NULL) {
        memset(route_out, 0, sizeof(*route_out));
    }
    if (!router_is_usable(router) || subject == NULL || route_out == NULL ||
        subject->width != router->link->config.local_peer.logical_id.width ||
        !logical_id_is_valid(subject) ||
        aodv_core_route_snapshot(router->aodv, subject, &route) !=
            AODV_ROUTE_QUERY_FOUND ||
        route.state != AODV_ROUTE_VALID || time_due(now_ms, route.expires_at_ms)) {
        return TAVRN_ROUTER_ROUTE_NOT_FOUND;
    }
    *route_out = route;
    return TAVRN_ROUTER_ROUTE_OK;
}

tavrn_router_reforward_status_t tavrn_router_reforward_transit_data(
    tavrn_router_t *router, const tavrn_link_data_t *data, uint32_t now_ms,
    tavrn_direct_peer_t *next_hop_out)
{
    aodv_route_snapshot_t route;
    tavrn_link_event_t local_outcome;
    tavrn_link_send_status_t send_status;
    tavrn_router_event_status_t outcome_status = TAVRN_ROUTER_EVENT_IGNORED;
    aodv_route_query_status_t route_status;

    if (next_hop_out != NULL) {
        memset(next_hop_out, 0, sizeof(*next_hop_out));
    }
    if (!router_is_usable(router) || !link_data_is_valid(data) ||
        data->ownership != TAVRN_DATA_TRANSIT ||
        data->final_destination.width !=
            router->link->config.local_peer.logical_id.width) {
        return TAVRN_ROUTER_REFORWARD_INVALID;
    }
    if (router_is_rejoining(router) ||
        logical_id_route_barred(router, &data->final_destination)) {
        return TAVRN_ROUTER_REFORWARD_BUSY;
    }
    {
        tavrn_router_event_status_t failure_status =
            gate_oldest_failure(router, now_ms);

        if (failure_status == TAVRN_ROUTER_EVENT_BUSY) {
            return TAVRN_ROUTER_REFORWARD_BUSY;
        }
        if (failure_status != TAVRN_ROUTER_EVENT_OK) {
            return TAVRN_ROUTER_REFORWARD_INVALID;
        }
    }
    route_status = aodv_core_route_snapshot(router->aodv, &data->final_destination,
                                             &route);
    if (route_status == AODV_ROUTE_QUERY_INVALID) {
        return TAVRN_ROUTER_REFORWARD_INVALID;
    }
    if (route_status != AODV_ROUTE_QUERY_FOUND) {
        return TAVRN_ROUTER_REFORWARD_NOT_FOUND;
    }
    if (route.state != AODV_ROUTE_VALID || time_due(now_ms, route.expires_at_ms)) {
        return TAVRN_ROUTER_REFORWARD_NOT_FOUND;
    }
    if (!direct_peer_is_valid(&route.next_hop)) {
        return TAVRN_ROUTER_REFORWARD_INVALID;
    }
    if (direct_peer_route_barred(router, &route.next_hop)) {
        return TAVRN_ROUTER_REFORWARD_BUSY;
    }
    memset(&local_outcome, 0, sizeof(local_outcome));
    send_status = tavrn_link_v2_send_unicast(router->link, &route.next_hop, data,
                                              now_ms, &local_outcome);
    if (local_outcome.type != TAVRN_LINK_EVENT_NONE) {
        outcome_status = tavrn_router_handle_link_event(router, &local_outcome, now_ms);
    }
    if (outcome_status == TAVRN_ROUTER_EVENT_INVALID) {
        return TAVRN_ROUTER_REFORWARD_INVALID;
    }
    if (send_status == TAVRN_LINK_SEND_OK) {
        if (next_hop_out != NULL) {
            *next_hop_out = route.next_hop;
        }
        return TAVRN_ROUTER_REFORWARD_OK;
    }
    if (send_status == TAVRN_LINK_SEND_BUSY ||
        send_status == TAVRN_LINK_SEND_NO_SLOT) {
        return TAVRN_ROUTER_REFORWARD_BUSY;
    }
    return TAVRN_ROUTER_REFORWARD_INVALID;
}

tavrn_router_event_status_t tavrn_router_release_transit_pin(
    tavrn_router_t *router, const tavrn_link_data_t *data, uint32_t now_ms)
{
    /* A failed reforward may already have latched a router fault, but its exact
     * inbound transit pin still has one bounded terminal cleanup path. */
    if (!router_is_initialized(router) || !link_data_is_valid(data) ||
        data->ownership != TAVRN_DATA_TRANSIT) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    return release_transit_custody(router, data, now_ms);
}

void tavrn_router_note_local_broadcast(tavrn_router_t *router,
                                       uint32_t accepted_at_ms)
{
    if (!router_is_initialized(router)) {
        return;
    }
    router->local_broadcast.generation++;
    if (router->local_broadcast.generation == 0u) {
        router->local_broadcast.generation++;
    }
    router->local_broadcast.accepted_at_ms = accepted_at_ms;
}

tavrn_router_local_broadcast_status_t tavrn_router_local_broadcast_snapshot(
    const tavrn_router_t *router,
    tavrn_router_local_broadcast_snapshot_t *snapshot_out)
{
    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (!router_is_initialized(router) || snapshot_out == NULL) {
        return TAVRN_ROUTER_LOCAL_BROADCAST_INVALID;
    }
    *snapshot_out = router->local_broadcast;
    return TAVRN_ROUTER_LOCAL_BROADCAST_OK;
}

static void capture_scheduler_input(tavrn_router_scheduler_trace_t *trace,
                                    const ble_mesh_sched_event_t *event)
{
    if (trace == NULL || event == NULL) {
        return;
    }
    trace->input_event_type = event->type;
    trace->input_fault = event->fault;
    trace->input_channel = event->channel;
    trace->input_rssi_magnitude_db = event->rssi_magnitude_db;
    memcpy(trace->input_advertiser.bytes, event->adv_addr,
           sizeof(trace->input_advertiser.bytes));
    trace->input_adv_len = event->adv_len;
    trace->input_present = TAVRN_ROUTER_TRACE_PRESENT;
}

static void capture_scheduler_decode(tavrn_router_scheduler_trace_t *trace,
                                     tavrn_codec_result_t decode_status,
                                     const tavrn_decoded_frame_t *decoded_frame)
{
    if (trace == NULL) {
        return;
    }
    trace->wire_decode_result = decode_status;
    trace->wire_decode_present = TAVRN_ROUTER_TRACE_PRESENT;
    if (decode_status == TAVRN_CODEC_OK && decoded_frame != NULL) {
        trace->decoded_frame_type = decoded_frame->type;
        trace->decoded_frame_present = TAVRN_ROUTER_TRACE_PRESENT;
    }
}

static void capture_scheduler_link_step(tavrn_router_scheduler_trace_t *trace,
                                        tavrn_link_step_status_t link_status,
                                        const tavrn_link_event_t *link_event)
{
    if (trace == NULL) {
        return;
    }
    trace->link_step_status = link_status;
    trace->link_step_present = TAVRN_ROUTER_TRACE_PRESENT;
    if (link_event == NULL || link_event->type == TAVRN_LINK_EVENT_NONE) {
        return;
    }
    trace->link_event_type = link_event->type;
    trace->link_event_present = TAVRN_ROUTER_TRACE_PRESENT;
    if (link_event->type == TAVRN_LINK_EVENT_RX_CONTROL) {
        trace->rx_control = link_event->detail.control;
        trace->rx_control_present = TAVRN_ROUTER_TRACE_PRESENT;
    }
}

static tavrn_router_event_status_t router_handle_scheduler_event(
    tavrn_router_t *router, const ble_mesh_sched_event_t *event,
    uint32_t now_ms, tavrn_router_scheduler_trace_t *trace)
{
    tavrn_link_event_t link_event;
    tavrn_link_step_status_t link_status;
    tavrn_router_event_status_t output_status = TAVRN_ROUTER_EVENT_IGNORED;
    tavrn_decoded_frame_t decoded_frame;
    tavrn_codec_config_t codec_config;
    tavrn_codec_result_t decode_status = TAVRN_CODEC_INVALID_ARGUMENT;
    tavrn_link_counters_t counters_before;
    const tavrn_link_counters_t *counters_after;
    tavrn_router_control_augmentation_status_t completion_status =
        TAVRN_ROUTER_CONTROL_AUGMENTATION_OK;
    uint8_t data_admitted;
    uint8_t flood_admitted;
    uint8_t data_busy;

    capture_scheduler_input(trace, event);
    if (!router_is_usable(router) || event == NULL) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (router_is_rejoining(router) &&
        event->type == BLE_MESH_SCHED_EVENT_TX_DONE &&
        event->tx_completed_channel_mask != 0u &&
        router->incarnation.snapshot.bootstrap_tx_done_seen == 0u &&
        event->tx_token == router->incarnation.snapshot.latest_hello_tx_token &&
        router->incarnation.snapshot.latest_hello.type == TAVRN_WIRE_HELLO &&
        router->incarnation.snapshot.latest_hello.pdu[5] == 0x40u) {
        router->incarnation.snapshot.bootstrap_tx_done_seen = 1u;
        router->incarnation.snapshot.last_bootstrap_tx_done_ms = now_ms;
    }
    memset(&decoded_frame, 0, sizeof(decoded_frame));
    if (event->type == BLE_MESH_SCHED_EVENT_RX_ADV) {
        memset(&codec_config, 0, sizeof(codec_config));
        codec_config.network_id = router->link->config.network_id;
        codec_config.local_peer = router->link->config.local_peer;
        codec_config.identity_conflict = router->link->config.identity_conflict;
        codec_config.identity_context = router->link->config.identity_context;
        decode_status = tavrn_wire_v2_decode(&codec_config, event->adv_addr,
                                               event->adv_data, event->adv_len,
                                               &decoded_frame);
        capture_scheduler_decode(trace, decode_status, &decoded_frame);
        if (decode_status != TAVRN_CODEC_OK && router->incarnation.enabled != 0u &&
            invalid_bootstrap_wire(event)) {
            router->incarnation.counters.invalid_bootstrap_rejected++;
        }
        /* Decode supplies bounded logical identities, but link admission owns
         * dedupe/candidate mutation.  Fail closed before that mutation for a
         * recorded SID16 full-identity collision. */
        if (decode_status == TAVRN_CODEC_OK &&
            decoded_frame_uses_barred_conflict(router, &decoded_frame)) {
            return TAVRN_ROUTER_EVENT_IGNORED;
        }
    }
    counters_after = tavrn_link_v2_counters(router->link);
    if (counters_after == NULL) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    counters_before = *counters_after;
    memset(&link_event, 0, sizeof(link_event));
    link_status = tavrn_link_v2_on_scheduler_event(router->link, event, now_ms,
                                                     &link_event);
    capture_scheduler_link_step(trace, link_status, &link_event);
    if ((event->type == BLE_MESH_SCHED_EVENT_TX_DONE ||
         event->type == BLE_MESH_SCHED_EVENT_TX_FAILED) &&
        router->control_augmentation.completed != NULL) {
        completion_status = router->control_augmentation.completed(
            router->control_augmentation.context, event, now_ms);
    }
    if (link_event.type != TAVRN_LINK_EVENT_NONE) {
        if (link_event.type == TAVRN_LINK_EVENT_RX_CONTROL &&
            router->control_augmentation.received != NULL) {
            router->control_augmentation.received(
                router->control_augmentation.context, &link_event.detail.control, now_ms);
        }
        if (link_event.type == TAVRN_LINK_EVENT_RX_CONTROL &&
            router->control_interceptor.receive != NULL) {
            tavrn_router_control_intercept_status_t intercept_status =
                router->control_interceptor.receive(
                    router->control_interceptor.context, &link_event.detail.control, now_ms);

            if (intercept_status == TAVRN_ROUTER_CONTROL_INTERCEPT_CONSUMED) {
                output_status = TAVRN_ROUTER_EVENT_OK;
            } else if (intercept_status == TAVRN_ROUTER_CONTROL_INTERCEPT_BUSY) {
                output_status = TAVRN_ROUTER_EVENT_BUSY;
            } else if (intercept_status == TAVRN_ROUTER_CONTROL_INTERCEPT_INVALID) {
                output_status = TAVRN_ROUTER_EVENT_INVALID;
            } else {
                output_status = consume_link_output(router, &link_event, now_ms);
            }
        } else {
            output_status = consume_link_output(router, &link_event, now_ms);
        }
    }
    if (completion_status != TAVRN_ROUTER_CONTROL_AUGMENTATION_OK) {
        output_status = TAVRN_ROUTER_EVENT_INVALID;
    }
    counters_after = tavrn_link_v2_counters(router->link);
    if (counters_after == NULL) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    data_admitted = counters_after->rx_candidate_accepted !=
            counters_before.rx_candidate_accepted ||
        counters_after->rx_committed_duplicate !=
            counters_before.rx_committed_duplicate;
    flood_admitted = counters_after->rx_flood_committed !=
            counters_before.rx_flood_committed ||
        counters_after->rx_flood_duplicate !=
            counters_before.rx_flood_duplicate;
    data_busy = counters_after->rx_data_dedupe_busy !=
        counters_before.rx_data_dedupe_busy;

    /* A link duplicate has no candidate output.  Its public counter transition
     * is the committed semantic-admission evidence; it cannot enqueue another
     * AODV action. */
    if (decode_status == TAVRN_CODEC_OK && decoded_frame.type == TAVRN_WIRE_DATA &&
        counters_after->rx_candidate_accepted != counters_before.rx_candidate_accepted &&
        link_event.type != TAVRN_LINK_EVENT_RX_DATA_CANDIDATE) {
        /* Normal new DATA evidence is emitted synchronously by the candidate
         * consumer.  Keep the before/after counter boundary explicit so an
         * alternate link-owned output cannot lose a committed admission. */
        observe_data(router, &decoded_frame.transmitter, &decoded_frame.detail.data.data,
                     now_ms);
    }
    if (decode_status == TAVRN_CODEC_OK && decoded_frame.type == TAVRN_WIRE_DATA &&
        counters_after->rx_committed_duplicate !=
            counters_before.rx_committed_duplicate) {
        observe_data(router, &decoded_frame.transmitter, &decoded_frame.detail.data.data,
                     now_ms);
    }
    if (decode_status == TAVRN_CODEC_OK && decoded_frame.type == TAVRN_WIRE_FLOOD &&
        flood_admitted != 0u) {
        observe_flood(router, &decoded_frame, now_ms);
    }
    if (output_status == TAVRN_ROUTER_EVENT_INVALID ||
        link_status == TAVRN_LINK_STEP_INVALID) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (output_status == TAVRN_ROUTER_EVENT_REJOINING) {
        return TAVRN_ROUTER_EVENT_REJOINING;
    }
    if (output_status == TAVRN_ROUTER_EVENT_BUSY ||
        link_status == TAVRN_LINK_STEP_ADDITIONAL_DATA_BUSY ||
        link_status == TAVRN_LINK_STEP_CANDIDATE_TIMEOUT_BUSY ||
        link_status == TAVRN_LINK_STEP_DATA_DEDUPE_BUSY || data_busy != 0u) {
        return TAVRN_ROUTER_EVENT_BUSY;
    }
    if (link_event.type != TAVRN_LINK_EVENT_NONE) {
        return output_status == TAVRN_ROUTER_EVENT_IGNORED ?
            TAVRN_ROUTER_EVENT_IGNORED : TAVRN_ROUTER_EVENT_OK;
    }
    if (data_admitted != 0u || flood_admitted != 0u) {
        return TAVRN_ROUTER_EVENT_OK;
    }
    return TAVRN_ROUTER_EVENT_IGNORED;
}

static aodv_status_t router_submit_application(
    tavrn_router_t *router, const tron_application_data_t *data,
    uint32_t now_ms)
{
    tavrn_router_scope_hint_t hint;
    tavrn_router_scope_status_t scope_status;
    tavrn_router_event_status_t failure_status;
    uint8_t full_scope;

    if (!router_is_usable(router) || !application_data_fits_identity_width(data)) {
        return AODV_STATUS_INVALID;
    }
    if (router_is_rejoining(router)) {
        return AODV_STATUS_REJOINING;
    }
    if (logical_id_route_barred(router, &data->final_destination)) {
        return AODV_STATUS_BUSY;
    }
    failure_status = gate_oldest_failure(router, now_ms);
    if (failure_status == TAVRN_ROUTER_EVENT_BUSY) {
        return AODV_STATUS_BUSY;
    }
    if (failure_status != TAVRN_ROUTER_EVENT_OK) {
        return AODV_STATUS_INVALID;
    }
    full_scope = router->aodv->config.net_diameter;
    scope_status = tavrn_router_initial_scope(router, &data->final_destination,
                                               full_scope, now_ms, &hint);
    if (scope_status == TAVRN_ROUTER_SCOPE_OK && hint.has_hint != 0u) {
        return aodv_core_submit_application_scoped_ex(
            router->aodv, data, hint.initial_scope, AODV_RREQ_SCOPE_ROUTER_HINT,
            now_ms);
    }
    return aodv_core_submit_application(router->aodv, data, now_ms);
}

static aodv_status_t router_tick(tavrn_router_t *router, uint32_t now_ms,
                                 tavrn_router_tick_trace_t *trace)
{
    tavrn_link_event_t output;
    tavrn_link_step_status_t link_status;
    tavrn_router_event_status_t router_status;
    aodv_status_t aodv_status;
    int oldest;
    uint8_t busy = 0u;

    if (!router_is_usable(router)) {
        return AODV_STATUS_INVALID;
    }
    router_status = retry_pending_incarnation_reset(router, now_ms);
    if (router_status == TAVRN_ROUTER_EVENT_INVALID) {
        return AODV_STATUS_INVALID;
    }
    if (router_status == TAVRN_ROUTER_EVENT_BUSY) {
        return AODV_STATUS_BUSY;
    }
    if (router_is_rejoining(router)) {
        if (router->incarnation.config.feature_level ==
                TAVRN_ROUTER_FEATURE_AODV_ONLY &&
            router->incarnation.snapshot.bootstrap_tx_done_seen != 0u &&
            time_due(now_ms, router->incarnation.snapshot.last_bootstrap_tx_done_ms +
                      router->incarnation.config.reboot_announce_ms)) {
            if (tavrn_link_v2_cancel_queued_control(
                    router->link, &router->incarnation.snapshot.latest_hello) !=
                TAVRN_LINK_RESOLVE_OK) {
                return AODV_STATUS_INVALID;
            }
            if (enqueue_local_hello(router, 0u, now_ms) !=
                TAVRN_ROUTER_INCARNATION_OK) {
                return AODV_STATUS_BUSY;
            }
            router->incarnation.snapshot.state = TAVRN_ROUTER_INCARNATION_ESTABLISHED;
            return AODV_STATUS_OK;
        }
        if (time_due(now_ms, router->incarnation.next_bootstrap_announce_ms)) {
            (void)enqueue_local_hello(router, 1u, now_ms);
            router->incarnation.next_bootstrap_announce_ms = now_ms +
                (router->incarnation.config.reboot_announce_ms > 1u ?
                 router->incarnation.config.reboot_announce_ms / 2u : 1u);
        }
        return AODV_STATUS_REJOINING;
    }
    memset(&output, 0, sizeof(output));
    link_status = tavrn_link_v2_tick(router->link, now_ms, &output);
    if (trace != NULL) {
        trace->link_step_status = link_status;
        trace->link_step_present = TAVRN_ROUTER_TRACE_PRESENT;
        if (output.type != TAVRN_LINK_EVENT_NONE) {
            trace->link_event = output;
            trace->link_event_present = TAVRN_ROUTER_TRACE_PRESENT;
        }
    }
    if (link_status == TAVRN_LINK_STEP_INVALID) {
        return AODV_STATUS_INVALID;
    }
    if (output.type != TAVRN_LINK_EVENT_NONE) {
        router_status = consume_link_output(router, &output, now_ms);
        if (router_status == TAVRN_ROUTER_EVENT_INVALID) {
            return AODV_STATUS_INVALID;
        }
        if (router_status == TAVRN_ROUTER_EVENT_BUSY) {
            busy = 1u;
        }
    }
    oldest = oldest_failure_slot(router);
    if (oldest >= 0) {
        router_status = gate_oldest_failure(router, now_ms);
        if (router_status == TAVRN_ROUTER_EVENT_INVALID) {
            return AODV_STATUS_INVALID;
        }
        if (router_status == TAVRN_ROUTER_EVENT_BUSY ||
            oldest_failure_slot(router) >= 0) {
            return AODV_STATUS_BUSY;
        }
    }
    if (router->delivery.state == TAVRN_ROUTER_DELIVERY_CANCEL_PENDING) {
        router_status = cancel_pre_ack_delivery(router, now_ms);
        if (router_status == TAVRN_ROUTER_EVENT_INVALID) {
            return AODV_STATUS_INVALID;
        }
        if (router_status == TAVRN_ROUTER_EVENT_BUSY) {
            return AODV_STATUS_BUSY;
        }
    }
    router_status = try_pending_ingest(router, now_ms);
    if (router_status == TAVRN_ROUTER_EVENT_INVALID) {
        return AODV_STATUS_INVALID;
    }
    if (router_status == TAVRN_ROUTER_EVENT_BUSY) {
        busy = 1u;
    }
    aodv_status = aodv_core_tick(router->aodv, now_ms);
    if (aodv_status != AODV_STATUS_OK) {
        return aodv_status;
    }
    return busy != 0u ? AODV_STATUS_BUSY : AODV_STATUS_OK;
}

static void clear_retained_action(tavrn_router_t *router)
{
    memset(&router->retained_action, 0, sizeof(router->retained_action));
    router->retained_action_valid = 0u;
}

static int action_is_control(aodv_action_type_t type)
{
    return type == AODV_ACTION_SEND_RREQ || type == AODV_ACTION_SEND_RREP ||
           type == AODV_ACTION_FORWARD_RREP || type == AODV_ACTION_SEND_RERR ||
           type == AODV_ACTION_SEND_RREP_ACK;
}

static tavrn_router_event_status_t dispose_data_action(
    tavrn_router_t *router, const aodv_data_action_t *data_action,
    uint32_t now_ms)
{
    tavrn_link_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED;
    event.detail.owned_data.next_hop = data_action->next_hop;
    event.detail.owned_data.data = data_action->data;
    return consume_owned_terminal(router, &event, now_ms);
}

static tavrn_router_event_status_t router_dispatch(
    tavrn_router_t *router, uint32_t now_ms,
    tavrn_router_dispatch_event_t *event_out,
    tavrn_router_dispatch_trace_t *trace)
{
    tavrn_link_event_t local_outcome;
    tavrn_link_send_status_t link_status;
    tavrn_router_event_status_t outcome_status = TAVRN_ROUTER_EVENT_OK;
    aodv_status_t mark_status;
    tavrn_router_delivery_status_t delivery_status;
    tavrn_router_event_status_t failure_status;
    const aodv_counters_t *aodv_counters;
    aodv_action_t action;
    uint32_t action_backpressure_before;
    uint8_t failure_blocked = 0u;

    if (event_out == NULL) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    memset(event_out, 0, sizeof(*event_out));
    event_out->type = TAVRN_ROUTER_DISPATCH_EVENT_NONE;
    if (!router_is_usable(router)) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (router_is_rejoining(router)) {
        return TAVRN_ROUTER_EVENT_REJOINING;
    }
    aodv_counters = aodv_core_counters(router->aodv);
    if (aodv_counters == NULL) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    action_backpressure_before = aodv_counters->action_backpressure;
    failure_status = gate_oldest_failure(router, now_ms);
    if (failure_status == TAVRN_ROUTER_EVENT_INVALID) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (failure_status == TAVRN_ROUTER_EVENT_BUSY) {
        failure_blocked = 1u;
        /* A retained action may move exactly one pre-existing action only
         * when this failure attempt was blocked by the bounded AODV queue.
         * Rate-limited failure reporting never lets ordinary work overtake it. */
        if (aodv_counters->action_backpressure == action_backpressure_before) {
            return TAVRN_ROUTER_EVENT_BUSY;
        }
    }
    if (router->retained_action_valid == 0u) {
        if (aodv_core_poll_action(router->aodv, &router->retained_action) ==
            AODV_ACTION_POLL_EMPTY) {
            return failure_blocked != 0u ? TAVRN_ROUTER_EVENT_BUSY :
                TAVRN_ROUTER_EVENT_IGNORED;
        }
        if (router->retained_action.type == AODV_ACTION_NONE) {
            capture_dispatch_action(trace, &router->retained_action);
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        router->retained_action_valid = 1u;
    }
    action = router->retained_action;
    capture_dispatch_action(trace, &action);

    /* A pending reset only quarantines work owned by its barred peer.  Drop one
     * such action and let the next public dispatch drain unrelated work. */
    if (action_uses_barred_peer(router, &action)) {
        if (action_is_control(action.type) && action.detail.control.token != 0u) {
            aodv_status_t cancel_status = aodv_core_cancel_unsent_action(
                router->aodv, action.detail.control.token);

            if (cancel_status != AODV_STATUS_OK &&
                cancel_status != AODV_STATUS_NOT_FOUND) {
                latch_router_fault(router,
                                   TAVRN_ROUTER_FAULT_CONTROL_CANCEL_INVALID);
                return TAVRN_ROUTER_EVENT_INVALID;
            }
        }
        clear_retained_action(router);
        router->counters.permanent_action_disposed++;
        return TAVRN_ROUTER_EVENT_BUSY;
    }

    if (action_is_control(action.type)) {
        const aodv_control_action_t *control = &action.detail.control;
        const tavrn_direct_peer_t *next_hop = control->controlled_flood != 0u ?
            NULL : &control->next_hop;
        tavrn_validated_control_t augmented;
        const tavrn_validated_control_t *control_to_send = &control->control;
        tavrn_router_control_augmentation_status_t admission_status =
            TAVRN_ROUTER_CONTROL_AUGMENTATION_OK;
        ble_mesh_tx_token_t scheduler_token = BLE_MESH_TX_TOKEN_NONE;
        uint8_t decorated = 0u;

        if (router->control_augmentation.prepare != NULL) {
            tavrn_router_control_augmentation_status_t augmentation_status;

            memset(&augmented, 0, sizeof(augmented));
            augmentation_status = router->control_augmentation.prepare(
                router->control_augmentation.context, &control->control, &augmented, now_ms);
            if (augmentation_status == TAVRN_ROUTER_CONTROL_AUGMENTATION_BUSY) {
                return TAVRN_ROUTER_EVENT_BUSY;
            }
            if (augmentation_status != TAVRN_ROUTER_CONTROL_AUGMENTATION_OK) {
                return TAVRN_ROUTER_EVENT_INVALID;
            }
            control_to_send = &augmented;
            decorated = controls_equal(control_to_send, &control->control) ? 0u : 1u;
        }

        memset(&local_outcome, 0, sizeof(local_outcome));
        if (decorated != 0u) {
            link_status = tavrn_link_v2_send_control_tracked(
                router->link, control_to_send, next_hop, control->controlled_flood,
                now_ms, &local_outcome, &scheduler_token);
        } else {
            link_status = tavrn_link_v2_send_control(router->link, control_to_send,
                                                       next_hop, control->controlled_flood,
                                                       now_ms, &local_outcome);
        }
        if (router->control_augmentation.admitted != NULL) {
            admission_status = router->control_augmentation.admitted(
                router->control_augmentation.context, &control->control,
                control_to_send, next_hop, control->controlled_flood, link_status,
                scheduler_token, now_ms);
        }
        capture_dispatch_link_result(trace, link_status, &local_outcome);
        report_rreq_link_enqueue(router->aodv, &action, link_status, now_ms, trace);
        if (local_outcome.type != TAVRN_LINK_EVENT_NONE) {
            outcome_status = tavrn_router_handle_link_event(router, &local_outcome, now_ms);
        }
        if (link_status == TAVRN_LINK_SEND_BUSY || link_status == TAVRN_LINK_SEND_NO_SLOT) {
            return admission_status != TAVRN_ROUTER_CONTROL_AUGMENTATION_OK ||
                   outcome_status == TAVRN_ROUTER_EVENT_INVALID ?
                TAVRN_ROUTER_EVENT_INVALID : TAVRN_ROUTER_EVENT_BUSY;
        }
        if (link_status == TAVRN_LINK_SEND_OK) {
            if (control->controlled_flood != 0u) {
                tavrn_router_note_local_broadcast(router, now_ms);
            }
            if (control->token != 0u) {
                mark_status = aodv_core_mark_action_sent(router->aodv,
                                                          control->token, now_ms);
                if (mark_status != AODV_STATUS_OK) {
                    clear_retained_action(router);
                    router->counters.permanent_action_disposed++;
                    latch_router_fault(router,
                                       TAVRN_ROUTER_FAULT_CONTROL_CANCEL_INVALID);
                    return TAVRN_ROUTER_EVENT_INVALID;
                }
            }
            clear_retained_action(router);
            if (admission_status != TAVRN_ROUTER_CONTROL_AUGMENTATION_OK ||
                outcome_status == TAVRN_ROUTER_EVENT_INVALID) {
                return TAVRN_ROUTER_EVENT_INVALID;
            }
            return failure_blocked != 0u ?
                TAVRN_ROUTER_EVENT_BUSY : outcome_status;
        }
        /* A decorated control has one bounded FULL owner which retained the
         * exact bytes.  Keep this already-polled AODV action for that owner's
         * direct retry rather than allocating a second route action. */
        if (decorated != 0u) {
            return admission_status != TAVRN_ROUTER_CONTROL_AUGMENTATION_OK ||
                   outcome_status == TAVRN_ROUTER_EVENT_INVALID ?
                TAVRN_ROUTER_EVENT_INVALID : TAVRN_ROUTER_EVENT_BUSY;
        }
        if (control->token != 0u) {
            mark_status = aodv_core_cancel_unsent_action(router->aodv,
                                                           control->token);
            if (mark_status != AODV_STATUS_OK && mark_status != AODV_STATUS_NOT_FOUND) {
                latch_router_fault(router,
                                   TAVRN_ROUTER_FAULT_CONTROL_CANCEL_INVALID);
                return TAVRN_ROUTER_EVENT_INVALID;
            }
        }
        clear_retained_action(router);
        router->counters.permanent_action_disposed++;
        if (outcome_status == TAVRN_ROUTER_EVENT_INVALID) {
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        return failure_blocked != 0u ? TAVRN_ROUTER_EVENT_BUSY :
            TAVRN_ROUTER_EVENT_IGNORED;
    }

    if (action.type == AODV_ACTION_FORWARD_DATA) {
        aodv_data_action_t data_action = action.detail.data;

        memset(&local_outcome, 0, sizeof(local_outcome));
        link_status = tavrn_link_v2_send_unicast(router->link, &data_action.next_hop,
                                                  &data_action.data, now_ms,
                                                  &local_outcome);
        capture_dispatch_link_result(trace, link_status, &local_outcome);
        if (local_outcome.type != TAVRN_LINK_EVENT_NONE) {
            outcome_status = consume_link_output(router, &local_outcome, now_ms);
        }
        if (link_status == TAVRN_LINK_SEND_BUSY || link_status == TAVRN_LINK_SEND_NO_SLOT) {
            return outcome_status == TAVRN_ROUTER_EVENT_INVALID ?
                TAVRN_ROUTER_EVENT_INVALID : TAVRN_ROUTER_EVENT_BUSY;
        }
        clear_retained_action(router);
        if (link_status == TAVRN_LINK_SEND_OK) {
            return failure_blocked != 0u && outcome_status != TAVRN_ROUTER_EVENT_INVALID ?
                TAVRN_ROUTER_EVENT_BUSY : outcome_status;
        }
        router->counters.permanent_action_disposed++;
        if (local_outcome.type == TAVRN_LINK_EVENT_NONE) {
            outcome_status = dispose_data_action(router, &data_action, now_ms);
        }
        if (outcome_status == TAVRN_ROUTER_EVENT_INVALID) {
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        return failure_blocked != 0u ? TAVRN_ROUTER_EVENT_BUSY :
            TAVRN_ROUTER_EVENT_IGNORED;
    }

    if (action.type == AODV_ACTION_DELIVER_DATA) {
        const tavrn_link_data_t *data = &action.detail.data.data;

        if (router->delivery.state != TAVRN_ROUTER_DELIVERY_POST_ACK_COMMIT ||
            !link_data_equal(&router->delivery.data, data) ||
            router->application.commit == NULL) {
            router->counters.delivery_mismatch++;
            latch_router_fault(router, TAVRN_ROUTER_FAULT_DELIVERY_ACTION_MISMATCH);
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        delivery_status = router->application.commit(
            router->application.context, router->delivery.token,
            &router->delivery.transmitter, &router->delivery.data, now_ms);
        if (delivery_status == TAVRN_ROUTER_DELIVERY_BUSY) {
            router->counters.application_commit_busy++;
            return TAVRN_ROUTER_EVENT_BUSY;
        }
        if (delivery_status != TAVRN_ROUTER_DELIVERY_OK) {
            router->counters.application_commit_invalid++;
            router->delivery.state = TAVRN_ROUTER_DELIVERY_FAULTED_POST_ACK;
            latch_router_fault(router, TAVRN_ROUTER_FAULT_DELIVERY_COMMIT_INVALID);
            return TAVRN_ROUTER_EVENT_INVALID;
        }
        memset(&router->delivery, 0, sizeof(router->delivery));
        clear_retained_action(router);
        return failure_blocked != 0u ? TAVRN_ROUTER_EVENT_BUSY :
            TAVRN_ROUTER_EVENT_OK;
    }

    if (action.type == AODV_ACTION_PENDING_DATA_FAILED) {
        event_out->type = TAVRN_ROUTER_DISPATCH_EVENT_PENDING_DATA_FAILED;
        event_out->destination = action.detail.failure.destination;
        clear_retained_action(router);
        router->counters.permanent_action_disposed++;
        return failure_blocked != 0u ? TAVRN_ROUTER_EVENT_BUSY :
            TAVRN_ROUTER_EVENT_OK;
    }

    if (action.type == AODV_ACTION_BLACKLIST_NEIGHBOR) {
        event_out->type = TAVRN_ROUTER_DISPATCH_EVENT_BLACKLIST_NEIGHBOR;
        event_out->destination = action.detail.failure.destination;
        event_out->peer = action.detail.failure.peer;
        clear_retained_action(router);
        router->counters.permanent_action_disposed++;
        return failure_blocked != 0u ? TAVRN_ROUTER_EVENT_BUSY :
            TAVRN_ROUTER_EVENT_OK;
    }

    clear_retained_action(router);
    return TAVRN_ROUTER_EVENT_INVALID;
}

static tavrn_router_event_status_t router_service_link(
    tavrn_router_t *router, uint32_t now_ms,
    tavrn_router_link_service_trace_t *trace)
{
    tavrn_link_event_t output;
    tavrn_link_step_status_t status;
    tavrn_router_event_status_t gate_status;

    if (!router_is_usable(router)) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (router_is_rejoining(router)) {
        return TAVRN_ROUTER_EVENT_REJOINING;
    }
    gate_status = gate_oldest_failure(router, now_ms);
    if (gate_status == TAVRN_ROUTER_EVENT_INVALID) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (gate_status == TAVRN_ROUTER_EVENT_BUSY) {
        return TAVRN_ROUTER_EVENT_BUSY;
    }
    memset(&output, 0, sizeof(output));
    status = tavrn_link_v2_dispatch(router->link, now_ms, &output);
    if (trace != NULL) {
        trace->link_step_status = status;
    }
    if (status == TAVRN_LINK_STEP_INVALID) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    if (output.type != TAVRN_LINK_EVENT_NONE) {
        if (trace != NULL) {
            trace->link_event = output;
            trace->link_event_present = TAVRN_ROUTER_TRACE_PRESENT;
        }
        return consume_link_output(router, &output, now_ms);
    }
    return TAVRN_ROUTER_EVENT_IGNORED;
}

const tavrn_router_counters_t *tavrn_router_counters(
    const tavrn_router_t *router)
{
    return router == NULL ? NULL : &router->counters;
}

tavrn_router_delivery_state_t tavrn_router_delivery_state(
    const tavrn_router_t *router)
{
    return router == NULL ? TAVRN_ROUTER_DELIVERY_NONE : router->delivery.state;
}

tavrn_router_fault_reason_t tavrn_router_fault_reason(
    const tavrn_router_t *router)
{
    return router == NULL ? TAVRN_ROUTER_FAULT_NONE : router->fault_reason;
}

tavrn_router_observe_status_t tavrn_router_observe_frame(
    tavrn_router_t *router,
    const tavrn_router_frame_observation_t *frame_observation,
    uint32_t now_ms)
{
    tavrn_router_observation_t observation;
    tavrn_router_evidence_role_t expected_role;
    tavrn_router_evidence_serial_kind_t expected_serial;

    if (!router_is_usable(router) || frame_observation == NULL) {
        return TAVRN_ROUTER_OBSERVE_INVALID;
    }
    if (ignored_observation_type(frame_observation->frame_type)) {
        return TAVRN_ROUTER_OBSERVE_IGNORED;
    }
    if (!supported_observation_type(frame_observation->frame_type)) {
        return TAVRN_ROUTER_OBSERVE_INVALID;
    }
    if (frame_observation->admission != TAVRN_ROUTER_FRAME_COMMITTED) {
        return known_noncommitted_admission(frame_observation->admission) ?
            TAVRN_ROUTER_OBSERVE_IGNORED : TAVRN_ROUTER_OBSERVE_INVALID;
    }
    if (!direct_peer_is_valid(&frame_observation->transmitter) ||
        frame_observation->subject_count > TAVRN_ROUTER_EVIDENCE_CAPACITY) {
        return TAVRN_ROUTER_OBSERVE_INVALID;
    }
    if (frame_observation->frame_type == TAVRN_WIRE_HACK) {
        if (frame_observation->hack_status == TAVRN_HACK_BUSY ||
            frame_observation->hack_status == TAVRN_HACK_REJECTED) {
            return TAVRN_ROUTER_OBSERVE_IGNORED;
        }
        if (frame_observation->hack_status != TAVRN_HACK_ACCEPTED &&
            frame_observation->hack_status != TAVRN_HACK_DUPLICATE) {
            return TAVRN_ROUTER_OBSERVE_INVALID;
        }
        if (frame_observation->subject_count != 0u) {
            return TAVRN_ROUTER_OBSERVE_INVALID;
        }
    } else if (frame_observation->frame_type == TAVRN_WIRE_E_RREP_ACK) {
        if (frame_observation->subject_count != 0u) {
            return TAVRN_ROUTER_OBSERVE_INVALID;
        }
    } else {
        if (frame_observation->frame_type == TAVRN_WIRE_E_RREP) {
            expected_role = TAVRN_ROUTER_EVIDENCE_ROUTE_DESTINATION;
            expected_serial = TAVRN_ROUTER_EVIDENCE_SERIAL_DESTINATION_SEQUENCE;
        } else if (frame_observation->frame_type == TAVRN_WIRE_E_RERR) {
            expected_role = TAVRN_ROUTER_EVIDENCE_REPORTER;
            expected_serial = TAVRN_ROUTER_EVIDENCE_SERIAL_NONE;
        } else if (frame_observation->frame_type == TAVRN_WIRE_E_RREQ) {
            expected_role = TAVRN_ROUTER_EVIDENCE_ORIGIN;
            expected_serial = TAVRN_ROUTER_EVIDENCE_SERIAL_NODE_SEQUENCE;
        } else {
            expected_role = TAVRN_ROUTER_EVIDENCE_ORIGIN;
            expected_serial = TAVRN_ROUTER_EVIDENCE_SERIAL_NONE;
        }
        if (!optional_subject_is_valid(frame_observation, expected_role,
                                       expected_serial)) {
            return TAVRN_ROUTER_OBSERVE_INVALID;
        }
    }

    memset(&observation, 0, sizeof(observation));
    observation.frame_type = frame_observation->frame_type;
    observation.transmitter = frame_observation->transmitter;
    observation.subject_count = frame_observation->subject_count;
    if (observation.subject_count != 0u) {
        memcpy(observation.subjects, frame_observation->subjects,
               (size_t)observation.subject_count * sizeof(observation.subjects[0]));
    }
    report_observation(router, &observation, now_ms);
    return TAVRN_ROUTER_OBSERVE_REPORTED;
}

tavrn_router_scope_status_t tavrn_router_initial_scope(
    const tavrn_router_t *router, const tavrn_logical_id_t *destination,
    uint8_t full_scope, uint32_t now_ms,
    tavrn_router_scope_hint_t *hint_out)
{
    tavrn_router_scope_hint_t hook_hint;

    if (hint_out != NULL) {
        hint_out->has_hint = 0u;
        hint_out->initial_scope = full_scope;
    }
    if (!router_is_usable(router) || destination == NULL || hint_out == NULL ||
        !logical_id_is_valid(destination) || full_scope == 0u || full_scope > 15u) {
        return TAVRN_ROUTER_SCOPE_INVALID;
    }
    if (router->augmentation.initial_scope == NULL) {
        return TAVRN_ROUTER_SCOPE_OK;
    }
    hook_hint = router->augmentation.initial_scope(router->augmentation.context,
                                                    destination, full_scope,
                                                    now_ms);
    if (hook_hint.has_hint != 0u && hook_hint.initial_scope != 0u &&
        hook_hint.initial_scope <= full_scope) {
        hint_out->has_hint = 1u;
        hint_out->initial_scope = hook_hint.initial_scope;
    }
    return TAVRN_ROUTER_SCOPE_OK;
}

tavrn_router_event_status_t tavrn_router_handle_scheduler_event_ex(
    tavrn_router_t *router, const ble_mesh_sched_event_t *event,
    uint32_t now_ms, tavrn_router_phase_trace_t *trace_out)
{
    tavrn_router_event_status_t status;

    begin_phase_trace(trace_out, TAVRN_ROUTER_TRACE_SCHEDULER_EVENT, now_ms);
    status = router_handle_scheduler_event(
        router, event, now_ms,
        trace_out != NULL ? &trace_out->detail.scheduler_event : NULL);
    if (trace_out != NULL) {
        trace_out->detail.scheduler_event.status = status;
    }
    finish_phase_trace(trace_out, router, now_ms);
    return status;
}

tavrn_router_event_status_t tavrn_router_handle_scheduler_event(
    tavrn_router_t *router, const ble_mesh_sched_event_t *event,
    uint32_t now_ms)
{
    return tavrn_router_handle_scheduler_event_ex(router, event, now_ms, NULL);
}

aodv_status_t tavrn_router_tick_ex(tavrn_router_t *router, uint32_t now_ms,
                                   tavrn_router_phase_trace_t *trace_out)
{
    aodv_status_t status;

    begin_phase_trace(trace_out, TAVRN_ROUTER_TRACE_TICK, now_ms);
    status = router_tick(router, now_ms,
                         trace_out != NULL ? &trace_out->detail.tick : NULL);
    if (trace_out != NULL) {
        trace_out->detail.tick.status = status;
    }
    finish_phase_trace(trace_out, router, now_ms);
    return status;
}

aodv_status_t tavrn_router_tick(tavrn_router_t *router, uint32_t now_ms)
{
    return tavrn_router_tick_ex(router, now_ms, NULL);
}

aodv_status_t tavrn_router_submit_application_ex(
    tavrn_router_t *router, const tron_application_data_t *data,
    uint32_t now_ms, tavrn_router_phase_trace_t *trace_out)
{
    aodv_status_t status;

    begin_phase_trace(trace_out, TAVRN_ROUTER_TRACE_APPLICATION_SUBMIT, now_ms);
    status = router_submit_application(router, data, now_ms);
    if (trace_out != NULL) {
        trace_out->detail.submit.status = status;
    }
    finish_phase_trace(trace_out, router, now_ms);
    return status;
}

aodv_status_t tavrn_router_submit_application(
    tavrn_router_t *router, const tron_application_data_t *data,
    uint32_t now_ms)
{
    return tavrn_router_submit_application_ex(router, data, now_ms, NULL);
}

tavrn_router_event_status_t tavrn_router_dispatch_trace_ex(
    tavrn_router_t *router, uint32_t now_ms,
    tavrn_router_phase_trace_t *trace_out)
{
    tavrn_router_dispatch_event_t event;
    tavrn_router_event_status_t status;
    tavrn_router_dispatch_trace_t *dispatch_trace = NULL;

    begin_phase_trace(trace_out, TAVRN_ROUTER_TRACE_DISPATCH, now_ms);
    if (trace_out != NULL) {
        dispatch_trace = &trace_out->detail.dispatch;
    }
    status = router_dispatch(router, now_ms, &event, dispatch_trace);
    if (trace_out != NULL) {
        trace_out->detail.dispatch.status = status;
        trace_out->detail.dispatch.dispatch_event = event;
    }
    finish_phase_trace(trace_out, router, now_ms);
    return status;
}

tavrn_router_event_status_t tavrn_router_dispatch_ex(
    tavrn_router_t *router, uint32_t now_ms,
    tavrn_router_dispatch_event_t *event_out)
{
    tavrn_router_phase_trace_t trace;
    tavrn_router_event_status_t status;

    if (event_out == NULL) {
        return TAVRN_ROUTER_EVENT_INVALID;
    }
    memset(event_out, 0, sizeof(*event_out));
    event_out->type = TAVRN_ROUTER_DISPATCH_EVENT_NONE;
    status = tavrn_router_dispatch_trace_ex(router, now_ms, &trace);
    *event_out = trace.detail.dispatch.dispatch_event;
    return status;
}

tavrn_router_event_status_t tavrn_router_service_link_ex(
    tavrn_router_t *router, uint32_t now_ms,
    tavrn_router_phase_trace_t *trace_out)
{
    tavrn_router_event_status_t status;
    tavrn_router_link_service_trace_t *service_trace = NULL;

    begin_phase_trace(trace_out, TAVRN_ROUTER_TRACE_LINK_SERVICE, now_ms);
    if (trace_out != NULL) {
        service_trace = &trace_out->detail.link_service;
    }
    status = router_service_link(router, now_ms, service_trace);
    if (trace_out != NULL) {
        trace_out->detail.link_service.status = status;
    }
    finish_phase_trace(trace_out, router, now_ms);
    return status;
}

tavrn_router_event_status_t tavrn_router_service_link(tavrn_router_t *router,
                                                         uint32_t now_ms)
{
    return tavrn_router_service_link_ex(router, now_ms, NULL);
}

tavrn_router_incarnation_status_t tavrn_router_incarnation_snapshot(
    const tavrn_router_t *router,
    tavrn_router_incarnation_snapshot_t *snapshot_out)
{
    if (router == NULL || snapshot_out == NULL || router->incarnation.enabled == 0u) {
        return TAVRN_ROUTER_INCARNATION_INVALID;
    }
    *snapshot_out = router->incarnation.snapshot;
    return TAVRN_ROUTER_INCARNATION_OK;
}

tavrn_router_incarnation_status_t tavrn_router_incarnation_peer_snapshot(
    const tavrn_router_t *router, const tavrn_adva_t *peer_adva,
    tavrn_router_incarnation_peer_snapshot_t *snapshot_out)
{
    const tavrn_router_incarnation_peer_t *record;
    tavrn_link_peer_incarnation_snapshot_t link_snapshot;
    aodv_peer_incarnation_snapshot_t aodv_snapshot;
    tavrn_direct_peer_t active_peer;
    int index;

    if (router == NULL || peer_adva == NULL || snapshot_out == NULL ||
        router->incarnation.enabled == 0u) {
        return TAVRN_ROUTER_INCARNATION_INVALID;
    }
    index = incarnation_peer_index(router, peer_adva);
    if (index < 0) {
        return TAVRN_ROUTER_INCARNATION_NOT_FOUND;
    }
    record = &router->incarnation.peers[(uint8_t)index];
    memset(snapshot_out, 0, sizeof(*snapshot_out));
    snapshot_out->direct_peer = record->direct_peer;
    snapshot_out->boot_nonce = record->boot_nonce;
    snapshot_out->direct_binding_valid = record->valid;
    snapshot_out->route_barred = record->route_barred;
    if (!direct_peer_at_active_width(router, &record->direct_peer, &active_peer)) {
        return TAVRN_ROUTER_INCARNATION_INVALID;
    }
    snapshot_out->reset_pending = router->incarnation.pending_reset.valid != 0u &&
        direct_peer_equal(&router->incarnation.pending_reset.direct_peer,
                          &active_peer);
    if (tavrn_link_v2_peer_incarnation_snapshot(router->link, &active_peer,
                                                 &link_snapshot) !=
            TAVRN_LINK_RESOLVE_OK ||
        aodv_core_peer_incarnation_snapshot(router->aodv, &active_peer,
                                             &aodv_snapshot) != AODV_FAILURE_OK) {
        return TAVRN_ROUTER_INCARNATION_INVALID;
    }
    snapshot_out->pending_custody_count = link_snapshot.pending_custody_count;
    snapshot_out->data_dedupe_count = link_snapshot.data_dedupe_count;
    snapshot_out->control_dedupe_count = record->control_dedupe_count;
    snapshot_out->freshness_count = (uint8_t)(record->freshness_count +
                                               aodv_snapshot.freshness_count);
    snapshot_out->route_count = aodv_snapshot.valid_route_count;
    return TAVRN_ROUTER_INCARNATION_OK;
}

const tavrn_router_incarnation_counters_t *tavrn_router_incarnation_counters(
    const tavrn_router_t *router)
{
    return router == NULL || router->incarnation.enabled == 0u ? NULL :
        &router->incarnation.counters;
}

tavrn_router_incarnation_status_t tavrn_router_reconfigure_identity(
    tavrn_router_t *router, const tavrn_direct_peer_t *local_peer,
    uint32_t now_ms)
{
    tavrn_router_incarnation_t incarnation;
    tavrn_router_application_hooks_t application;
    tavrn_router_augmentation_hooks_t augmentation;

    if (!router_is_usable(router) || !direct_peer_is_valid(local_peer)) {
        return TAVRN_ROUTER_INCARNATION_INVALID;
    }
    if (router->link->config.local_peer.logical_id.width ==
            local_peer->logical_id.width &&
        direct_peer_equal(&router->link->config.local_peer, local_peer)) {
        return TAVRN_ROUTER_INCARNATION_OK;
    }
    incarnation = router->incarnation;
    application = router->application;
    augmentation = router->augmentation;
    if (tavrn_link_v2_reconfigure_identity(router->link, local_peer, now_ms) !=
        TAVRN_LINK_RESOLVE_OK) {
        return TAVRN_ROUTER_INCARNATION_INVALID;
    }
    if (aodv_core_reconfigure_identity(router->aodv, local_peer, now_ms) !=
        AODV_INIT_OK) {
        latch_router_fault(router, TAVRN_ROUTER_FAULT_CONTROL_CANCEL_INVALID);
        return TAVRN_ROUTER_INCARNATION_INVALID;
    }
    /* This is one object graph.  Only state keyed by the previous width is
     * discarded before the new local width is observable by callers. */
    memset(&router->delivery, 0, sizeof(router->delivery));
    memset(&router->pending_ingest, 0, sizeof(router->pending_ingest));
    memset(router->failures, 0, sizeof(router->failures));
    memset(&router->failure_overflow, 0, sizeof(router->failure_overflow));
    memset(&router->retained_action, 0, sizeof(router->retained_action));
    router->retained_action_valid = 0u;
    router->next_failure_order = 0u;
    router->application = application;
    router->augmentation = augmentation;
    router->incarnation = incarnation;
    /* Returning a FULL node to SID16 invalidates its compressed context.  The
     * common incarnation owner must resume N=1 announcements; mentorship only
     * decides when a later SID8 handover is authorized. */
    if (router->incarnation.enabled != 0u &&
        router->incarnation.config.feature_level == TAVRN_ROUTER_FEATURE_FULL_TAVRN &&
        local_peer->logical_id.width == TAVRN_IDENTITY_SID16) {
        router->incarnation.snapshot.state = TAVRN_ROUTER_INCARNATION_REJOINING;
        router->incarnation.snapshot.bootstrap_tx_done_seen = 0u;
        router->incarnation.next_bootstrap_announce_ms = now_ms;
    }
    return TAVRN_ROUTER_INCARNATION_OK;
}

tavrn_router_incarnation_status_t tavrn_router_complete_full_bootstrap(
    tavrn_router_t *router, uint32_t now_ms)
{
    (void)now_ms;
    if (!router_is_usable(router) || router->incarnation.enabled == 0u ||
        router->incarnation.config.feature_level != TAVRN_ROUTER_FEATURE_FULL_TAVRN) {
        return TAVRN_ROUTER_INCARNATION_INVALID;
    }
    router->incarnation.snapshot.state = TAVRN_ROUTER_INCARNATION_ESTABLISHED;
    return TAVRN_ROUTER_INCARNATION_OK;
}
