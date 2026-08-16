#include "mind_event_forwarder.h"

#include <stdio.h>
#include <string.h>

#include "tron_mesh_packet.h"

static unsigned failures;

static void check(int condition, const char *message)
{
    if (!condition) {
        failures++;
        printf("FAIL: %s\n", message);
    }
}

static tavrn_adva_t identity(uint8_t value)
{
    tavrn_adva_t result = { { value, (uint8_t)(value + 1u), 0x54u, 0x56u,
                               0x78u, 0xc0u } };
    return result;
}

static void make_direct_event(ble_mesh_sched_event_t *event, uint8_t wearable,
                              uint32_t packet_id24, uint8_t event_type)
{
    uint8_t *adv;

    memset(event, 0, sizeof(*event));
    event->type = BLE_MESH_SCHED_EVENT_RX_ADV;
    event->adv_len = MIND_APPLICATION_INGRESS_FRAME_BYTES;
    event->adv_addr[4] = wearable;
    event->adv_addr[5] = 0xc0u;
    adv = event->adv_data;
    adv[0] = TRON_MESH_FLAGS_AD_LEN;
    adv[1] = TRON_MESH_FLAGS_AD_TYPE;
    adv[2] = TRON_MESH_FLAGS_VALUE;
    adv[3] = (uint8_t)(TRON_MESH_MANUF_BASE_LEN + MIND_PAYLOAD_SIZE);
    adv[4] = TRON_MESH_MANUF_AD_TYPE;
    adv[5] = 0xffu;
    adv[6] = 0xffu;
    adv[7] = (uint8_t)TRON_MESH_MAGIC0;
    adv[8] = (uint8_t)TRON_MESH_MAGIC1;
    adv[9] = TRON_MESH_VERSION;
    adv[10] = TRON_MESH_MSG_TYPE_MIND_EVENT;
    adv[11] = 7u;
    adv[12] = 0u;
    adv[13] = wearable;
    adv[14] = 0x01u;
    adv[15] = (uint8_t)(packet_id24 & 0xffu);
    adv[16] = (uint8_t)((packet_id24 >> 8) & 0xffu);
    adv[17] = (uint8_t)((packet_id24 >> 16) & 0xffu);
    adv[18] = MIND_PAYLOAD_SIZE;
    adv[19] = MIND_SCHEMA_VERSION;
    adv[20] = event_type;
    adv[21] = event_type == MIND_EVT_HEARTBEAT ? 0u : 73u;
    adv[22] = 0x1fu;
    adv[23] = 0x1au;
    adv[24] = event_type == MIND_EVT_HEARTBEAT ? 0u : 0x7du;
    adv[25] = (uint8_t)(packet_id24 & 0xffu);
}

static int admit(mind_application_ingress_t *ingress, uint8_t wearable,
                 uint32_t packet_id24, uint8_t event_type, uint32_t now)
{
    ble_mesh_sched_event_t event;

    make_direct_event(&event, wearable, packet_id24, event_type);
    return mind_application_ingress_receive(ingress, &event, now).outcome ==
        MIND_APPLICATION_INGRESS_CONSUMED_ACCEPTED;
}

static mind_application_ingress_take_status_t take_ingress(
    void *context, mind_application_ingress_event_t *event_out)
{
    return mind_application_ingress_take((mind_application_ingress_t *)context, event_out);
}

typedef struct event_stream {
    mind_application_ingress_event_t events[MIND_APP_EVENT_CAPACITY + 1u];
    uint8_t next;
    uint8_t count;
} event_stream_t;

static mind_application_ingress_take_status_t take_stream(
    void *context, mind_application_ingress_event_t *event_out)
{
    event_stream_t *stream = context;

    if (stream == NULL || event_out == NULL) {
        return MIND_APPLICATION_INGRESS_TAKE_INVALID;
    }
    if (stream->next == stream->count) {
        return MIND_APPLICATION_INGRESS_TAKE_EMPTY;
    }
    *event_out = stream->events[stream->next++];
    return MIND_APPLICATION_INGRESS_TAKE_OK;
}

static int make_stream_event(mind_application_ingress_event_t *event,
                             uint32_t packet_id24)
{
    uint8_t payload[MIND_PAYLOAD_SIZE] = { MIND_SCHEMA_VERSION,
        MIND_EVT_FALL_AND_SHOUT, 73u, 0x1fu, 0x1au, 0x7du,
        (uint8_t)(packet_id24 & 0xffu) };

    if (event == NULL) {
        return 0;
    }
    memset(event, 0, sizeof(*event));
    return mind_application_wire_pack_report(&event->report, 9u, packet_id24,
                                              payload) == MIND_APPLICATION_WIRE_OK;
}

static tavrn_router_delivery_status_t publish_local(
    void *context, const mind_application_wire_record_t *record,
    const tavrn_adva_t *observer, uint32_t now)
{
    return mind_root_inbox_publish_local((mind_root_inbox_t *)context, record,
                                         observer, now);
}

static mind_application_root_state_t active_state(uint16_t nonce)
{
    mind_application_root_state_t state;

    state.active = 1u;
    state.nonce = nonce;
    state.generation = 1u;
    return state;
}

static void add_remote_root(mind_root_plane_t *plane, uint8_t value)
{
    tavrn_adva_t remote = identity(value);
    mind_application_root_state_t state = active_state((uint16_t)(value + 10u));

    check(mind_root_plane_receive_root_state(plane, &remote, &state) ==
              MIND_ROOT_APPLY_ACCEPTED,
          "remote root is admitted into the production root registry");
}

static void complete_next_remote(mind_event_forwarder_t *forwarder)
{
    mind_root_submission_t submission;

    check(mind_event_forwarder_prepare_submission(forwarder, 1u, &submission) &&
              submission.kind == MIND_ROOT_SUBMISSION_EVENT &&
              mind_event_forwarder_submission_resolved(
                  forwarder, &submission, MIND_ROOT_RESOLVER_UNIQUE),
          "remote urgent target resolves through the production event owner");
    mind_event_forwarder_submission_complete(forwarder, &submission, 1u);
}

static void test_rootless_and_local_delivery(void)
{
    tavrn_adva_t local = identity(0x80u);
    mind_root_plane_t plane;
    mind_event_forwarder_t forwarder;
    mind_application_ingress_t ingress;
    mind_root_inbox_t inbox;
    mind_root_inbox_entry_t entry;

    mind_root_plane_init(&plane, &local, 1u);
    mind_event_forwarder_init(&forwarder, &local);
    mind_application_ingress_init(&ingress, 7u);
    check(admit(&ingress, 4u, 0x111111u, MIND_EVT_FALL_AND_SHOUT, 1u) &&
              mind_event_forwarder_consume_ingress(&forwarder, &ingress,
                                                   take_ingress, &plane, 1u) &&
              ingress.queue_count == 0u && forwarder.counters.rootless_drop == 1u,
          "rootless ingress consumes silently without a retained delivery item");

    check(mind_root_plane_set_local(&plane, 1u, 2u) == MIND_ROOT_APPLY_ACCEPTED,
          "local root activates for direct-local fanout");
    mind_root_inbox_init(&inbox);
    check(admit(&ingress, 5u, 0x222222u, MIND_EVT_FALL_AND_SHOUT, 3u) &&
              mind_event_forwarder_consume_ingress(&forwarder, &ingress,
                                                   take_ingress, &plane, 3u),
          "local-root ingress snapshots one local target");
    mind_event_forwarder_service_local(&forwarder, &inbox, publish_local, 3u);
    check(mind_root_inbox_peek(&inbox, &entry) &&
              entry.path == MIND_ROOT_INBOX_PATH_LOCAL &&
              entry.data.app_kind == MIND_REPORT &&
              entry.observer.bytes[0] == local.bytes[0] &&
              forwarder.counters.local_published == 1u,
          "local target publishes through the shared final inbox without self-route");
}

static void test_target_matrix_and_independent_progress(void)
{
    tavrn_adva_t local = identity(0x80u);
    mind_root_plane_t plane;
    mind_event_forwarder_t forwarder;
    mind_application_ingress_t ingress;
    mind_root_inbox_t inbox;
    uint8_t value;

    mind_root_plane_init(&plane, &local, 1u);
    mind_event_forwarder_init(&forwarder, &local);
    mind_application_ingress_init(&ingress, 7u);
    mind_root_inbox_init(&inbox);
    check(mind_root_plane_set_local(&plane, 1u, 0u) == MIND_ROOT_APPLY_ACCEPTED,
          "local root is one member of the all-root snapshot");
    add_remote_root(&plane, 1u);
    check(admit(&ingress, 1u, 0x333333u, MIND_EVT_FALL_AND_SHOUT, 1u) &&
              mind_event_forwarder_consume_ingress(&forwarder, &ingress,
                                                   take_ingress, &plane, 1u) &&
              forwarder.events[0].target_count == 2u,
          "local plus one remote creates two independent target obligations");
    complete_next_remote(&forwarder);
    check(forwarder.events[0].occupied != 0u &&
              forwarder.events[0].targets[0].state == MIND_EVENT_TARGET_PENDING,
          "remote handoff completes independently while local BUSY target remains");
    mind_event_forwarder_service_local(&forwarder, &inbox, publish_local, 2u);
    check(forwarder.counters.remote_handoff == 1u &&
              forwarder.counters.local_published == 1u &&
              forwarder.events[0].occupied == 0u,
          "local and remote targets retire only after both independent paths complete");

    mind_root_plane_init(&plane, &local, 2u);
    mind_event_forwarder_init(&forwarder, &local);
    mind_application_ingress_init(&ingress, 7u);
    mind_root_inbox_init(&inbox);
    check(mind_root_plane_set_local(&plane, 1u, 0u) == MIND_ROOT_APPLY_ACCEPTED,
          "sixteen-root fixture enables the local root");
    for (value = 1u; value < MIND_APP_ROOT_CAPACITY; value++) {
        add_remote_root(&plane, value);
    }
    check(mind_root_plane_active_count(&plane) == MIND_APP_ROOT_CAPACITY &&
              admit(&ingress, 2u, 0x444444u, MIND_EVT_FALL_AND_SHOUT, 1u) &&
              mind_event_forwarder_consume_ingress(&forwarder, &ingress,
                                                   take_ingress, &plane, 1u) &&
              forwarder.events[0].target_count == MIND_APP_ROOT_CAPACITY,
          "all sixteen active Layer-7 roots are captured without a TAVRN bound");
    mind_event_forwarder_service_local(&forwarder, &inbox, publish_local, 2u);
    for (value = 1u; value < MIND_APP_ROOT_CAPACITY; value++) {
        complete_next_remote(&forwarder);
    }
    check(forwarder.counters.local_published == 1u &&
              forwarder.counters.remote_handoff == MIND_APP_ROOT_CAPACITY - 1u &&
              forwarder.counters.completed_events == 1u,
          "all sixteen target paths complete without dropping or evicting the item");
}

static void test_busy_resolver_and_cancellation(void)
{
    tavrn_adva_t local = identity(0x80u);
    tavrn_adva_t first = identity(1u);
    tavrn_adva_t second = identity(2u);
    mind_root_plane_t plane;
    mind_event_forwarder_t forwarder;
    mind_application_ingress_t ingress;
    mind_root_inbox_t inbox;
    mind_application_wire_record_t report;
    mind_root_submission_t submission;
    uint8_t payload[MIND_PAYLOAD_SIZE] = { 1u, MIND_EVT_FALL_AND_SHOUT, 1u,
                                           1u, 0u, 1u, 0x55u };
    uint8_t index;

    check(mind_application_wire_pack_report(&report, 9u, 0x55u, payload) ==
              MIND_APPLICATION_WIRE_OK,
          "busy fixture creates a valid copied report");
    mind_root_plane_init(&plane, &local, 1u);
    mind_event_forwarder_init(&forwarder, &local);
    mind_application_ingress_init(&ingress, 7u);
    mind_root_inbox_init(&inbox);
    check(mind_root_plane_set_local(&plane, 1u, 0u) == MIND_ROOT_APPLY_ACCEPTED,
          "local busy fixture activates one root");
    for (index = 0u; index < MIND_ROOT_INBOX_CAPACITY; index++) {
        check(mind_root_inbox_publish_local(&inbox, &report, &local, index) ==
                  TAVRN_ROUTER_DELIVERY_OK,
              "final inbox fills through its production local publication seam");
    }
    check(admit(&ingress, 3u, 0x555555u, MIND_EVT_FALL_AND_SHOUT, 1u) &&
              mind_event_forwarder_consume_ingress(&forwarder, &ingress,
                                                   take_ingress, &plane, 1u),
          "busy local fixture creates a retained target");
    mind_event_forwarder_service_local(&forwarder, &inbox, publish_local, 1u);
    check(forwarder.counters.local_busy == 1u && forwarder.events[0].occupied != 0u,
          "full final inbox retains rather than losing local target progress");
    check(mind_root_inbox_consume(&inbox), "one final inbox slot is released");
    mind_event_forwarder_service_local(&forwarder, &inbox, publish_local, 2u);
    check(forwarder.counters.local_published == 1u && forwarder.events[0].occupied == 0u,
          "local BUSY target retries after final inbox recovery");

    mind_root_plane_init(&plane, &local, 2u);
    mind_event_forwarder_init(&forwarder, &local);
    mind_application_ingress_init(&ingress, 7u);
    add_remote_root(&plane, 1u);
    add_remote_root(&plane, 2u);
    check(admit(&ingress, 4u, 0x666666u, MIND_EVT_FALL_AND_SHOUT, 1u) &&
              mind_event_forwarder_consume_ingress(&forwarder, &ingress,
                                                   take_ingress, &plane, 1u) &&
              mind_event_forwarder_prepare_submission(&forwarder, 1u, &submission),
          "remote resolver fixture selects the first independent target");
    check(!mind_event_forwarder_submission_resolved(
                  &forwarder, &submission, MIND_ROOT_RESOLVER_UNKNOWN) &&
              forwarder.events[0].targets[submission.target_slot].resolver_blocked != 0u,
          "UNKNOWN resolver retains the selected event target");
    mind_event_forwarder_note_topology_observed(&forwarder);
    check(mind_event_forwarder_prepare_submission(&forwarder, 1u, &submission) &&
              !mind_event_forwarder_submission_resolved(
                  &forwarder, &submission, MIND_ROOT_RESOLVER_RESERVED) &&
              forwarder.events[0].targets[submission.target_slot].resolver_failed_closed != 0u,
          "RESERVED resolver fails closed with retained application evidence");
    check(mind_event_forwarder_prepare_submission(&forwarder, 1u, &submission) &&
              memcmp(submission.target.bytes, first.bytes, TAVRN_ADVA_LEN) == 0,
          "failed-closed second target does not erase the other remote target");
    mind_event_forwarder_clear_target(&forwarder, &second);
    check(forwarder.events[0].occupied != 0u &&
              forwarder.events[0].targets[1].state == MIND_EVENT_TARGET_CANCELLED,
          "matching OFF/departure cancellation clears only that retained target");
    mind_event_forwarder_clear_target(&forwarder, &first);
    check(forwarder.events[0].occupied == 0u,
          "matching OFF/departure clears the remaining failed-closed target only");
}

static void test_event_item_target_cursor_fairness(void)
{
    tavrn_adva_t local = identity(0x80u);
    tavrn_adva_t first = identity(1u);
    tavrn_adva_t second = identity(2u);
    mind_root_plane_t plane;
    mind_event_forwarder_t forwarder;
    mind_application_ingress_t ingress;
    mind_root_submission_t submission;

    mind_root_plane_init(&plane, &local, 1u);
    mind_event_forwarder_init(&forwarder, &local);
    mind_application_ingress_init(&ingress, 7u);
    add_remote_root(&plane, 1u);
    add_remote_root(&plane, 2u);
    check(admit(&ingress, 1u, 0x710001u, MIND_EVT_FALL_AND_SHOUT, 1u) &&
              mind_event_forwarder_consume_ingress(&forwarder, &ingress,
                                                   take_ingress, &plane, 1u) &&
              admit(&ingress, 2u, 0x710002u, MIND_EVT_FALL_AND_SHOUT, 2u) &&
              mind_event_forwarder_consume_ingress(&forwarder, &ingress,
                                                   take_ingress, &plane, 2u) &&
              forwarder.events[0].target_count == 2u &&
              forwarder.events[1].target_count == 2u,
          "two retained events snapshot two independent remote targets each");

    check(mind_event_forwarder_prepare_submission(&forwarder, 1u, &submission) &&
              submission.slot == 0u &&
              memcmp(submission.target.bytes, first.bytes, TAVRN_ADVA_LEN) == 0 &&
              mind_event_forwarder_submission_resolved(
                  &forwarder, &submission, MIND_ROOT_RESOLVER_UNIQUE),
          "first event initially selects its first target");
    mind_event_forwarder_submission_complete(&forwarder, &submission, 0u);
    check(forwarder.events[0].target_cursor == 1u &&
              forwarder.events[0].targets[0].state == MIND_EVENT_TARGET_PENDING,
          "BUSY advances only the first event's retained target cursor");

    check(mind_event_forwarder_prepare_submission(&forwarder, 1u, &submission) &&
              submission.slot == 1u &&
              memcmp(submission.target.bytes, first.bytes, TAVRN_ADVA_LEN) == 0 &&
              mind_event_forwarder_submission_resolved(
                  &forwarder, &submission, MIND_ROOT_RESOLVER_UNIQUE),
          "global event fairness still selects the second event first target");
    mind_event_forwarder_submission_complete(&forwarder, &submission, 0u);
    check(forwarder.events[1].target_cursor == 1u &&
              forwarder.events[1].targets[0].state == MIND_EVENT_TARGET_PENDING,
          "second BUSY advances only the second event's retained target cursor");

    check(mind_event_forwarder_prepare_submission(&forwarder, 1u, &submission) &&
              submission.slot == 0u && submission.target_slot == 1u &&
              memcmp(submission.target.bytes, second.bytes, TAVRN_ADVA_LEN) == 0 &&
              mind_event_forwarder_submission_resolved(
                  &forwarder, &submission, MIND_ROOT_RESOLVER_UNIQUE),
          "first event reaches its healthy alternate target after its BUSY target");
    mind_event_forwarder_submission_complete(&forwarder, &submission, 1u);
    check(mind_event_forwarder_prepare_submission(&forwarder, 1u, &submission) &&
              submission.slot == 1u && submission.target_slot == 1u &&
              memcmp(submission.target.bytes, second.bytes, TAVRN_ADVA_LEN) == 0 &&
              mind_event_forwarder_submission_resolved(
                  &forwarder, &submission, MIND_ROOT_RESOLVER_UNIQUE),
          "second event reaches its healthy alternate target without cross-starvation");
    mind_event_forwarder_submission_complete(&forwarder, &submission, 1u);
    check(forwarder.events[0].target_count == 2u &&
              forwarder.events[1].target_count == 2u &&
              forwarder.events[0].target_cursor == 0u &&
              forwarder.events[1].target_cursor == 0u &&
              forwarder.events[0].targets[0].state == MIND_EVENT_TARGET_PENDING &&
              forwarder.events[0].targets[1].state == MIND_EVENT_TARGET_COMPLETE &&
              forwarder.events[1].targets[0].state == MIND_EVENT_TARGET_PENDING &&
              forwarder.events[1].targets[1].state == MIND_EVENT_TARGET_COMPLETE &&
              forwarder.counters.remote_busy == 2u &&
              forwarder.counters.remote_handoff == 2u,
          "two-event BUSY retries retain blocked targets while both alternate paths progress");
}

static void test_event_resolver_colliding_and_invalid(void)
{
    tavrn_adva_t local = identity(0x80u);
    tavrn_adva_t first = identity(1u);
    tavrn_adva_t second = identity(2u);
    mind_root_plane_t plane;
    mind_event_forwarder_t forwarder;
    mind_application_ingress_t ingress;
    mind_root_submission_t submission;

    mind_root_plane_init(&plane, &local, 1u);
    mind_event_forwarder_init(&forwarder, &local);
    mind_application_ingress_init(&ingress, 7u);
    add_remote_root(&plane, 1u);
    add_remote_root(&plane, 2u);
    check(admit(&ingress, 3u, 0x720001u, MIND_EVT_FALL_AND_SHOUT, 1u) &&
              mind_event_forwarder_consume_ingress(&forwarder, &ingress,
                                                   take_ingress, &plane, 1u) &&
              mind_event_forwarder_prepare_submission(&forwarder, 1u, &submission) &&
              memcmp(submission.target.bytes, first.bytes, TAVRN_ADVA_LEN) == 0 &&
              !mind_event_forwarder_submission_resolved(
                  &forwarder, &submission, MIND_ROOT_RESOLVER_COLLIDING) &&
              forwarder.events[0].targets[0].state == MIND_EVENT_TARGET_PENDING &&
              forwarder.events[0].targets[0].resolver_blocked != 0u &&
              forwarder.events[0].targets[0].resolver_failed_closed == 0u &&
              forwarder.events[0].target_cursor == 1u,
          "COLLIDING retains the first event target for a later topology retry");
    mind_event_forwarder_note_topology_observed(&forwarder);
    check(mind_event_forwarder_prepare_submission(&forwarder, 1u, &submission) &&
              memcmp(submission.target.bytes, second.bytes, TAVRN_ADVA_LEN) == 0 &&
              !mind_event_forwarder_submission_resolved(
                  &forwarder, &submission, MIND_ROOT_RESOLVER_INVALID) &&
              forwarder.events[0].targets[1].state == MIND_EVENT_TARGET_PENDING &&
              forwarder.events[0].targets[1].resolver_failed_closed != 0u &&
              forwarder.events[0].targets[1].resolver_blocked == 0u &&
              forwarder.events[0].target_cursor == 0u,
          "INVALID fails closed without completing or clearing its event target");
    mind_event_forwarder_note_topology_observed(&forwarder);
    check(mind_event_forwarder_prepare_submission(&forwarder, 1u, &submission) &&
              memcmp(submission.target.bytes, first.bytes, TAVRN_ADVA_LEN) == 0 &&
              mind_event_forwarder_submission_resolved(
                  &forwarder, &submission, MIND_ROOT_RESOLVER_UNIQUE),
          "topology observation retries COLLIDING while skipping the fail-closed target");
    mind_event_forwarder_submission_complete(&forwarder, &submission, 1u);
    check(forwarder.events[0].occupied != 0u &&
              forwarder.events[0].targets[0].state == MIND_EVENT_TARGET_COMPLETE &&
              forwarder.events[0].targets[1].state == MIND_EVENT_TARGET_PENDING &&
              forwarder.events[0].targets[1].resolver_failed_closed != 0u &&
              forwarder.counters.resolver_colliding == 1u &&
              forwarder.counters.resolver_invalid == 1u,
          "fail-closed INVALID evidence survives a successful COLLIDING retry");
}

static void test_final_inbox_reserve_commit_cancel_and_full(void)
{
    mind_root_inbox_t inbox;
    tavrn_direct_peer_t transmitter;
    tavrn_link_data_t data;
    tavrn_router_delivery_token_t token;
    uint8_t index;

    memset(&transmitter, 0, sizeof(transmitter));
    memset(&data, 0, sizeof(data));
    data.app_kind = 0x7fu;
    mind_root_inbox_init(&inbox);
    check(mind_root_inbox_reserve(&inbox, &transmitter, &data, &token) ==
              TAVRN_ROUTER_DELIVERY_OK &&
              mind_root_inbox_cancel(&inbox, token, &transmitter, &data) ==
                  TAVRN_ROUTER_DELIVERY_OK && inbox.count == 0u,
          "cancelled router reservation has no accepted final-data side effect");
    for (index = 0u; index < MIND_ROOT_INBOX_CAPACITY; index++) {
        check(mind_root_inbox_reserve(&inbox, &transmitter, &data, &token) ==
                  TAVRN_ROUTER_DELIVERY_OK &&
                  mind_root_inbox_commit(&inbox, token, &transmitter, &data, index) ==
                      TAVRN_ROUTER_DELIVERY_OK,
              "generic final DATA reserves before its transport acceptance");
    }
    check(mind_root_inbox_reserve(&inbox, &transmitter, &data, &token) ==
              TAVRN_ROUTER_DELIVERY_BUSY,
          "full eight-slot final inbox returns BUSY before a new transport acceptance");
}

static void test_round_robin_and_full_retention(void)
{
    tavrn_adva_t local = identity(0x80u);
    tavrn_adva_t first = identity(1u);
    tavrn_adva_t second = identity(2u);
    mind_root_plane_t plane;
    mind_event_forwarder_t forwarder;
    mind_application_ingress_t ingress;
    mind_root_submission_t submission;
    event_stream_t stream;
    uint8_t index;

    mind_root_plane_init(&plane, &local, 1u);
    mind_event_forwarder_init(&forwarder, &local);
    mind_application_ingress_init(&ingress, 7u);
    add_remote_root(&plane, 1u);
    add_remote_root(&plane, 2u);
    check(admit(&ingress, 7u, 0x777777u, MIND_EVT_FALL_AND_SHOUT, 1u) &&
              mind_event_forwarder_consume_ingress(&forwarder, &ingress,
                                                   take_ingress, &plane, 1u) &&
              mind_event_forwarder_prepare_submission(&forwarder, 1u, &submission) &&
              memcmp(submission.target.bytes, first.bytes, TAVRN_ADVA_LEN) == 0 &&
              mind_event_forwarder_submission_resolved(
                  &forwarder, &submission, MIND_ROOT_RESOLVER_UNIQUE),
          "round-robin fixture begins with the first remote target");
    mind_event_forwarder_submission_complete(&forwarder, &submission, 0u);
    check(mind_event_forwarder_prepare_submission(&forwarder, 1u, &submission) &&
              memcmp(submission.target.bytes, second.bytes, TAVRN_ADVA_LEN) == 0 &&
              mind_event_forwarder_submission_resolved(
                  &forwarder, &submission, MIND_ROOT_RESOLVER_UNIQUE),
          "BUSY first target advances to the second remote target");
    mind_event_forwarder_submission_complete(&forwarder, &submission, 1u);
    check(mind_event_forwarder_prepare_submission(&forwarder, 1u, &submission) &&
              memcmp(submission.target.bytes, first.bytes, TAVRN_ADVA_LEN) == 0 &&
              mind_event_forwarder_submission_resolved(
                  &forwarder, &submission, MIND_ROOT_RESOLVER_UNIQUE),
          "completed second target leaves the retained first target eligible later");
    mind_event_forwarder_submission_complete(&forwarder, &submission, 1u);
    check(forwarder.counters.remote_busy == 1u &&
              forwarder.counters.remote_handoff == 2u &&
              forwarder.counters.completed_events == 1u,
          "two remote targets make fair independent progress across BUSY and retry");

    mind_root_plane_init(&plane, &local, 2u);
    mind_event_forwarder_init(&forwarder, &local);
    add_remote_root(&plane, 1u);
    memset(&stream, 0, sizeof(stream));
    stream.count = MIND_APP_EVENT_CAPACITY + 1u;
    for (index = 0u; index < stream.count; index++) {
        check(make_stream_event(&stream.events[index], (uint32_t)(0x800000u + index)),
              "full-retention fixture creates a valid production ingress event");
    }
    for (index = 0u; index < MIND_APP_EVENT_CAPACITY; index++) {
        check(mind_event_forwarder_consume_ingress(&forwarder, &stream, take_stream,
                                                   &plane, index),
              "forwarder admits every fixed event item through exact capacity");
    }
    check(!mind_event_forwarder_consume_ingress(&forwarder, &stream, take_stream,
                                                &plane, 99u) && stream.next ==
              MIND_APP_EVENT_CAPACITY && forwarder.events[0].occupied != 0u &&
              forwarder.events[MIND_APP_EVENT_CAPACITY - 1u].occupied != 0u &&
              forwarder.counters.event_capacity_busy == 1u,
          "full plus one retains every live event and leaves ingress unconsumed");
}

int main(void)
{
    test_rootless_and_local_delivery();
    test_target_matrix_and_independent_progress();
    test_busy_resolver_and_cancellation();
    test_event_item_target_cursor_fairness();
    test_event_resolver_colliding_and_invalid();
    test_final_inbox_reserve_commit_cancel_and_full();
    test_round_robin_and_full_retention();
    if (failures != 0u) {
        printf("mind_event_forwarder failures=%u\n", failures);
        return 1;
    }
    printf("mind_event_forwarder tests passed\n");
    return 0;
}
