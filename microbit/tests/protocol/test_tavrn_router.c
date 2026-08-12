#include "tavrn_router.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned int failures;
static const char *reported[8];
static unsigned int reported_count;

static int first_for(const char *requirement)
{
    unsigned int i;

    for (i = 0u; i < reported_count; i++) {
        if (strcmp(reported[i], requirement) == 0) {
            return 0;
        }
    }
    reported[reported_count++] = requirement;
    return 1;
}

#define CHECK(requirement, expression) \
    do { \
        if (!(expression)) { \
            if (first_for(requirement)) { \
                printf("FAIL %s: %s:%d: assertion failed: %s\n", \
                       (requirement), __FILE__, __LINE__, #expression); \
            } \
            failures++; \
        } \
    } while (0)

static const uint8_t adva_a[TAVRN_ADVA_LEN] = {
    0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu,
};
static const uint8_t adva_b[TAVRN_ADVA_LEN] = {
    0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u,
};
static const uint8_t adva_c[TAVRN_ADVA_LEN] = {
    0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0xc1u,
};
static const uint8_t adva_d[TAVRN_ADVA_LEN] = {
    0x12u, 0x23u, 0x34u, 0x45u, 0x56u, 0xc2u,
};

typedef struct observation_recorder {
    tavrn_router_observation_t observations[8];
    uint8_t count;
    uint8_t scope_calls;
} observation_recorder_t;

typedef struct observation_case {
    tavrn_wire_type_t type;
    tavrn_hack_status_t hack_status;
    uint8_t subject_count;
    tavrn_router_evidence_role_t roles[TAVRN_ROUTER_EVIDENCE_CAPACITY];
    tavrn_router_evidence_serial_kind_t
        serial_kinds[TAVRN_ROUTER_EVIDENCE_CAPACITY];
} observation_case_t;

static tavrn_direct_peer_t make_peer(const uint8_t adva[TAVRN_ADVA_LEN])
{
    tavrn_direct_peer_t peer;

    memset(&peer, 0, sizeof(peer));
    peer.logical_id.width = TAVRN_IDENTITY_SID16;
    peer.logical_id.value = (uint16_t)adva[0] | ((uint16_t)adva[1] << 8);
    memcpy(peer.adva.bytes, adva, TAVRN_ADVA_LEN);
    return peer;
}

static aodv_core_config_t make_core_config(void)
{
    aodv_core_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_peer(adva_a);
    config.network_id = 0x2au;
    config.net_diameter = 15u;
    config.rreq_retries = 2u;
    config.rreq_rate_per_second = 10u;
    config.rerr_rate_per_second = 10u;
    config.request_rrep_ack = 1u;
    config.initial_origin_sequence = 1u;
    config.initial_request_id = 1u;
    config.initial_rerr_sequence = 1u;
    config.node_traversal_ms = 10u;
    config.path_discovery_ms = 600u;
    config.rreq_seen_ms = 10000u;
    config.active_route_ms = 36000u;
    config.pending_data_ms = 5000u;
    config.blacklist_ms = 600u;
    config.rrep_dedupe_ms = 10000u;
    config.rerr_dedupe_ms = 10000u;
    config.rrep_ack_wait_ms = 250u;
    return config;
}

static void record_observation(void *context,
                               const tavrn_router_observation_t *observation,
                               uint32_t now_ms)
{
    observation_recorder_t *recorder = context;

    (void)now_ms;
    if (recorder != NULL && observation != NULL &&
        recorder->count < (uint8_t)(sizeof(recorder->observations) /
                                    sizeof(recorder->observations[0]))) {
        recorder->observations[recorder->count++] = *observation;
    }
}

static tavrn_router_scope_hint_t give_scope(void *context,
                                             const tavrn_logical_id_t *destination,
                                             uint8_t full_scope,
                                             uint32_t now_ms)
{
    observation_recorder_t *recorder = context;
    tavrn_router_scope_hint_t hint;

    (void)destination;
    (void)full_scope;
    (void)now_ms;
    if (recorder != NULL) {
        recorder->scope_calls++;
    }
    hint.has_hint = 1u;
    hint.initial_scope = 4u;
    return hint;
}

static tavrn_router_delivery_status_t reserve_test_delivery(
    void *context, const tavrn_direct_peer_t *transmitter,
    const tavrn_link_data_t *data, tavrn_router_delivery_token_t *token_out,
    uint32_t now_ms)
{
    (void)context;
    (void)transmitter;
    (void)data;
    (void)now_ms;
    if (token_out == NULL) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    *token_out = 1u;
    return TAVRN_ROUTER_DELIVERY_OK;
}

static tavrn_router_delivery_status_t commit_test_delivery(
    void *context, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t now_ms)
{
    (void)context;
    (void)transmitter;
    (void)data;
    (void)now_ms;
    return token == 1u ? TAVRN_ROUTER_DELIVERY_OK : TAVRN_ROUTER_DELIVERY_INVALID;
}

static tavrn_router_delivery_status_t cancel_test_delivery(
    void *context, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t now_ms)
{
    return commit_test_delivery(context, token, transmitter, data, now_ms);
}

static int init_router(tavrn_router_t *router, tavrn_link_v2_t *link,
                       aodv_core_t *core,
                       const tavrn_router_augmentation_hooks_t *hooks_or_null)
{
    tavrn_router_application_hooks_t application_hooks;

    memset(&application_hooks, 0, sizeof(application_hooks));
    application_hooks.reserve = reserve_test_delivery;
    application_hooks.commit = commit_test_delivery;
    application_hooks.cancel = cancel_test_delivery;
    return tavrn_router_init(router, link, core, hooks_or_null) ==
               TAVRN_ROUTER_INIT_OK &&
           tavrn_router_set_application_hooks(router, &application_hooks) ==
               TAVRN_ROUTER_APPLICATION_HOOK_OK;
}

static tavrn_router_frame_observation_t make_frame(
    tavrn_router_frame_admission_t admission, tavrn_wire_type_t type,
    tavrn_hack_status_t hack_status)
{
    tavrn_router_frame_observation_t observation;

    memset(&observation, 0, sizeof(observation));
    observation.admission = admission;
    observation.frame_type = type;
    observation.transmitter = make_peer(adva_b);
    observation.hack_status = hack_status;
    return observation;
}

static void set_subject(tavrn_router_frame_observation_t *observation,
                        uint8_t index, const uint8_t adva[TAVRN_ADVA_LEN],
                        tavrn_router_evidence_role_t role,
                        tavrn_router_evidence_serial_kind_t serial_kind)
{
    tavrn_router_evidence_subject_t *subject = &observation->subjects[index];

    subject->logical_id = make_peer(adva).logical_id;
    subject->role = role;
    subject->serial_kind = serial_kind;
    subject->serial = (uint16_t)(0x0304u + index);
    subject->hop_count = (uint8_t)(2u + index);
    subject->hop_present = 1u;
}

static void populate_subjects(tavrn_router_frame_observation_t *observation,
                              const observation_case_t *test_case)
{
    static const uint8_t *const identities[TAVRN_ROUTER_EVIDENCE_CAPACITY] = {
        adva_c,
        adva_d,
    };
    uint8_t i;

    observation->subject_count = test_case->subject_count;
    for (i = 0u; i < test_case->subject_count; i++) {
        set_subject(observation, i, identities[i], test_case->roles[i],
                    test_case->serial_kinds[i]);
    }
}

static void pdu_put_u16(uint8_t *pdu, uint8_t offset, uint16_t value)
{
    pdu[offset] = (uint8_t)value;
    pdu[(uint8_t)(offset + 1u)] = (uint8_t)(value >> 8);
}

static int install_expiring_route(aodv_core_t *core)
{
    tron_application_data_t application;
    tavrn_validated_control_t reply;
    aodv_action_t action;
    aodv_control_input_t input;
    tavrn_direct_peer_t peer_c = make_peer(adva_c);

    memset(&application, 0, sizeof(application));
    application.final_destination = peer_c.logical_id;
    application.app_kind = 0x7fu;
    application.app_len = 1u;
    application.app_bytes[0] = 0xa5u;
    if (aodv_core_submit_application(core, &application, 0u) != AODV_STATUS_QUEUED ||
        aodv_core_poll_action(core, &action) != AODV_ACTION_POLL_OK ||
        action.type != AODV_ACTION_SEND_RREQ) {
        return 0;
    }
    memset(&reply, 0, sizeof(reply));
    reply.type = TAVRN_WIRE_E_RREP;
    reply.pdu_len = 19u;
    reply.pdu[0] = 0x54u;
    reply.pdu[1] = 0x52u;
    reply.pdu[2] = 0x02u;
    reply.pdu[3] = 0x2au;
    reply.pdu[4] = TAVRN_WIRE_E_RREP;
    reply.pdu[6] = 0x10u;
    pdu_put_u16(reply.pdu, 7u, make_peer(adva_a).logical_id.value);
    pdu_put_u16(reply.pdu, 9u, peer_c.logical_id.value);
    pdu_put_u16(reply.pdu, 11u, 1u);
    pdu_put_u16(reply.pdu, 13u, make_peer(adva_a).logical_id.value);
    reply.pdu[15] = action.detail.control.control.pdu[9];
    reply.pdu[16] = action.detail.control.control.pdu[10];
    pdu_put_u16(reply.pdu, 17u, 10u);
    memset(&input, 0, sizeof(input));
    input.transmitter = peer_c;
    input.control = reply;
    return aodv_core_ingest_control(core, &input, 1u) == AODV_STATUS_OK;
}

static tavrn_link_config_t make_link_config(void)
{
    tavrn_link_config_t config;

    memset(&config, 0, sizeof(config));
    config.local_peer = make_peer(adva_a);
    config.network_id = 0x2au;
    config.hack_max_attempts = 3u;
    config.busy_max_responses = 3u;
    config.hack_response_ms = 250u;
    config.hack_turnaround_ms = 8u;
    config.retry_backoff_ms = 1u;
    config.busy_backoff_ms = 500u;
    config.data_forward_deadline_ms = 5000u;
    config.candidate_resolve_ms = 10u;
    config.data_dedupe_ms = 10000u;
    config.flood_dedupe_ms = 10000u;
    config.flood_jitter_min_ms = 20u;
    config.flood_jitter_max_ms = 120u;
    return config;
}

static int setup_live_router(tavrn_router_t *router, tavrn_link_v2_t *link,
                             ble_mesh_scheduler_t *scheduler, aodv_core_t *core)
{
    tavrn_link_config_t link_config = make_link_config();
    aodv_core_config_t core_config = make_core_config();

    memset(scheduler, 0, sizeof(*scheduler));
    ble_mesh_scheduler_init(scheduler, 0u, adva_a);
    return tavrn_link_v2_init(link, scheduler, &link_config, 0u) ==
               TAVRN_LINK_INIT_OK &&
           aodv_core_init(core, &core_config, 0u) == AODV_INIT_OK &&
           init_router(router, link, core, NULL);
}

static tavrn_link_data_t make_transit_data(uint16_t sequence,
                                           const uint8_t destination[TAVRN_ADVA_LEN])
{
    tavrn_link_data_t data;

    memset(&data, 0, sizeof(data));
    data.origin = make_peer(adva_b).logical_id;
    data.final_destination = make_peer(destination).logical_id;
    data.data_seq = sequence;
    data.ttl = 7u;
    data.hops = 2u;
    data.app_kind = 0x71u;
    data.app_source = 0x31u;
    data.app_len = 3u;
    data.app_bytes[0] = 0xa1u;
    data.app_bytes[1] = 0xb2u;
    data.app_bytes[2] = 0xc3u;
    data.ownership = TAVRN_DATA_TRANSIT;
    return data;
}

static tavrn_link_event_t seed_candidate(tavrn_link_v2_t *link,
                                         const tavrn_link_data_t *data,
                                         uint16_t token)
{
    tavrn_link_event_t event;

    memset(&event, 0, sizeof(event));
    link->candidate.token = token;
    link->candidate.transmitter = make_peer(adva_b);
    link->candidate.rssi_magnitude_db = 42u;
    link->candidate.data = *data;
    link->candidate.deadline_ms = 100u;
    link->candidate_valid = 1u;
    event.type = TAVRN_LINK_EVENT_RX_DATA_CANDIDATE;
    event.detail.candidate = link->candidate;
    return event;
}

static void pin_transit_data(tavrn_link_v2_t *link, const tavrn_link_data_t *data)
{
    tavrn_data_dedupe_entry_t *entry = &link->data_dedupe[0];

    memset(entry, 0, sizeof(*entry));
    entry->valid = 1u;
    entry->custody_pinned = 1u;
    entry->origin = data->origin;
    entry->final_destination = data->final_destination;
    entry->data_seq = data->data_seq;
    entry->app_kind = data->app_kind;
    entry->app_source = data->app_source;
    entry->expires_at_ms = 10000u;
}

static int transit_data_is_pinned(const tavrn_link_v2_t *link,
                                  const tavrn_link_data_t *data)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_LINK_DATA_DEDUPE_CAPACITY; index++) {
        const tavrn_data_dedupe_entry_t *entry = &link->data_dedupe[index];

        if (entry->valid != 0u && entry->origin.width == data->origin.width &&
            entry->origin.value == data->origin.value &&
            entry->data_seq == data->data_seq && entry->app_kind == data->app_kind &&
            entry->app_source == data->app_source) {
            return entry->custody_pinned != 0u;
        }
    }
    return 0;
}

static int transit_data_has_external_custody_owner(const tavrn_link_v2_t *link,
                                                   const tavrn_link_data_t *data)
{
    uint8_t index;

    if (link == NULL || data == NULL) {
        return 0;
    }
    for (index = 0u; index < TAVRN_LINK_DATA_DEDUPE_CAPACITY; index++) {
        const tavrn_data_dedupe_entry_t *entry = &link->data_dedupe[index];

        if (entry->valid != 0u && entry->origin.width == data->origin.width &&
            entry->origin.value == data->origin.value &&
            entry->final_destination.width == data->final_destination.width &&
            entry->final_destination.value == data->final_destination.value &&
            entry->data_seq == data->data_seq && entry->app_kind == data->app_kind &&
            entry->app_source == data->app_source) {
            return entry->external_custody_owner != 0u;
        }
    }
    return 0;
}

typedef struct candidate_port_recorder {
    tavrn_router_candidate_reservation_status_t reserve_status;
    tavrn_router_candidate_completion_status_t commit_status;
    tavrn_router_candidate_completion_status_t rollback_status;
    tavrn_rx_data_candidate_t reserved;
    tavrn_router_candidate_reservation_token_t token;
    uint8_t reserve_calls;
    uint8_t commit_calls;
    uint8_t rollback_calls;
    uint8_t commit_exact;
    uint8_t rollback_exact;
} candidate_port_recorder_t;

static tavrn_router_candidate_reservation_status_t reserve_candidate_port(
    void *context, const tavrn_rx_data_candidate_t *candidate,
    tavrn_router_candidate_reservation_token_t *token_out, uint32_t now_ms)
{
    candidate_port_recorder_t *recorder = context;

    (void)now_ms;
    if (recorder == NULL || candidate == NULL || token_out == NULL) {
        return TAVRN_ROUTER_CANDIDATE_INVALID;
    }
    recorder->reserve_calls++;
    recorder->reserved = *candidate;
    *token_out = recorder->reserve_status == TAVRN_ROUTER_CANDIDATE_RESERVED ?
        recorder->token : TAVRN_ROUTER_CANDIDATE_RESERVATION_TOKEN_NONE;
    return recorder->reserve_status;
}

static tavrn_router_candidate_completion_status_t commit_candidate_port(
    void *context, tavrn_router_candidate_reservation_token_t token,
    const tavrn_rx_data_candidate_t *candidate, uint32_t now_ms)
{
    candidate_port_recorder_t *recorder = context;

    (void)now_ms;
    if (recorder == NULL || candidate == NULL) {
        return TAVRN_ROUTER_CANDIDATE_COMPLETION_INVALID;
    }
    recorder->commit_calls++;
    recorder->commit_exact = token == recorder->token &&
        candidate->token == recorder->reserved.token &&
        memcmp(&candidate->data, &recorder->reserved.data,
               sizeof(candidate->data)) == 0 &&
        memcmp(&candidate->transmitter, &recorder->reserved.transmitter,
               sizeof(candidate->transmitter)) == 0;
    return recorder->commit_status;
}

static tavrn_router_candidate_completion_status_t rollback_candidate_port(
    void *context, tavrn_router_candidate_reservation_token_t token,
    const tavrn_rx_data_candidate_t *candidate, uint32_t now_ms)
{
    candidate_port_recorder_t *recorder = context;

    (void)now_ms;
    if (recorder == NULL || candidate == NULL) {
        return TAVRN_ROUTER_CANDIDATE_COMPLETION_INVALID;
    }
    recorder->rollback_calls++;
    recorder->rollback_exact = token == recorder->token &&
        candidate->token == recorder->reserved.token &&
        memcmp(&candidate->data, &recorder->reserved.data,
               sizeof(candidate->data)) == 0;
    return recorder->rollback_status;
}

static tavrn_router_candidate_reservation_port_t candidate_port(
    candidate_port_recorder_t *recorder)
{
    tavrn_router_candidate_reservation_port_t port;

    memset(&port, 0, sizeof(port));
    port.context = recorder;
    port.reserve = reserve_candidate_port;
    port.commit = commit_candidate_port;
    port.rollback = rollback_candidate_port;
    return port;
}

typedef struct terminal_hook_recorder {
    tavrn_router_data_terminal_disposition_t disposition;
    tavrn_link_event_t event;
    uint8_t calls;
} terminal_hook_recorder_t;

static tavrn_router_data_terminal_disposition_t record_data_terminal(
    void *context, const tavrn_link_event_t *event, uint32_t now_ms)
{
    terminal_hook_recorder_t *recorder = context;

    (void)now_ms;
    if (recorder == NULL || event == NULL) {
        return TAVRN_ROUTER_DATA_TERMINAL_INVALID;
    }
    recorder->calls++;
    recorder->event = *event;
    return recorder->disposition;
}

static void test_gtt_01_generic_hook_isolation(void)
{
    aodv_core_t core;
    tavrn_link_v2_t link;
    aodv_core_config_t config = make_core_config();
    tavrn_router_t router;
    tavrn_router_t aodv_only;
    observation_recorder_t recorder;
    tavrn_router_augmentation_hooks_t hooks;
    tavrn_router_scope_hint_t hint;
    tavrn_logical_id_t destination = make_peer(adva_c).logical_id;

    memset(&recorder, 0, sizeof(recorder));
    memset(&link, 0, sizeof(link));
    memset(&hooks, 0, sizeof(hooks));
    hooks.context = &recorder;
    hooks.observe = record_observation;
    hooks.initial_scope = give_scope;
    if (aodv_core_init(&core, &config, 0u) != AODV_INIT_OK) {
        CHECK("GTT-01", 0);
        return;
    }
    CHECK("GTT-01", init_router(&router, &link, &core, &hooks) &&
                         tavrn_router_initial_scope(&router, &destination, 15u, 0u,
                                                    &hint) == TAVRN_ROUTER_SCOPE_OK &&
                        hint.has_hint != 0u && hint.initial_scope == 4u &&
                        recorder.scope_calls == 1u);
    CHECK("GTT-01", init_router(&aodv_only, &link, &core, NULL) &&
                         tavrn_router_initial_scope(&aodv_only, &destination, 15u, 0u,
                                                    &hint) == TAVRN_ROUTER_SCOPE_OK &&
                        hint.has_hint == 0u && hint.initial_scope == 15u);
}

static void test_gtt_03_frame_role_admission(void)
{
    static const observation_case_t positive[] = {
        { TAVRN_WIRE_DATA, TAVRN_HACK_ACCEPTED, 1u,
          { TAVRN_ROUTER_EVIDENCE_ORIGIN },
          { TAVRN_ROUTER_EVIDENCE_SERIAL_NONE } },
        { TAVRN_WIRE_HACK, TAVRN_HACK_ACCEPTED, 0u, { 0 }, { 0 } },
        { TAVRN_WIRE_HACK, TAVRN_HACK_DUPLICATE, 0u, { 0 }, { 0 } },
        { TAVRN_WIRE_FLOOD, TAVRN_HACK_ACCEPTED, 1u,
          { TAVRN_ROUTER_EVIDENCE_ORIGIN },
          { TAVRN_ROUTER_EVIDENCE_SERIAL_NONE } },
        { TAVRN_WIRE_E_RREQ, TAVRN_HACK_ACCEPTED, 1u,
          { TAVRN_ROUTER_EVIDENCE_ORIGIN },
          { TAVRN_ROUTER_EVIDENCE_SERIAL_NODE_SEQUENCE } },
        { TAVRN_WIRE_E_RREP, TAVRN_HACK_ACCEPTED, 1u,
          { TAVRN_ROUTER_EVIDENCE_ROUTE_DESTINATION },
          { TAVRN_ROUTER_EVIDENCE_SERIAL_DESTINATION_SEQUENCE } },
        { TAVRN_WIRE_E_RERR, TAVRN_HACK_ACCEPTED, 1u,
          { TAVRN_ROUTER_EVIDENCE_REPORTER },
          { TAVRN_ROUTER_EVIDENCE_SERIAL_NONE } },
        { TAVRN_WIRE_E_RREP_ACK, TAVRN_HACK_ACCEPTED, 0u, { 0 }, { 0 } },
    };
    static const observation_case_t negative[] = {
        { TAVRN_WIRE_DATA, TAVRN_HACK_ACCEPTED, 0u, { 0 }, { 0 } },
        { TAVRN_WIRE_DATA, TAVRN_HACK_ACCEPTED, 0u, { 0 }, { 0 } },
        { TAVRN_WIRE_HACK, TAVRN_HACK_BUSY, 0u, { 0 }, { 0 } },
        { TAVRN_WIRE_HACK, TAVRN_HACK_REJECTED, 0u, { 0 }, { 0 } },
        { TAVRN_WIRE_E_RREQ, TAVRN_HACK_ACCEPTED, 0u, { 0 }, { 0 } },
        { TAVRN_WIRE_E_RREP, TAVRN_HACK_ACCEPTED, 0u, { 0 }, { 0 } },
        { TAVRN_WIRE_HELLO, TAVRN_HACK_ACCEPTED, 0u, { 0 }, { 0 } },
        { TAVRN_WIRE_FLOOD, TAVRN_HACK_ACCEPTED, 0u, { 0 }, { 0 } },
    };
    static const tavrn_router_frame_admission_t negative_admission[] = {
        TAVRN_ROUTER_FRAME_UNRESOLVED_DATA_CANDIDATE,
        TAVRN_ROUTER_FRAME_OVERHEARD_ADDRESSED_DATA,
        TAVRN_ROUTER_FRAME_COMMITTED,
        TAVRN_ROUTER_FRAME_COMMITTED,
        TAVRN_ROUTER_FRAME_MALFORMED,
        TAVRN_ROUTER_FRAME_AMBIGUOUS,
        TAVRN_ROUTER_FRAME_COMMITTED,
        TAVRN_ROUTER_FRAME_REJECTED,
    };
    aodv_core_t core;
    tavrn_link_v2_t link;
    aodv_core_config_t config = make_core_config();
    tavrn_router_t router;
    observation_recorder_t recorder;
    tavrn_router_augmentation_hooks_t hooks;
    tavrn_router_frame_observation_t frame;
    uint8_t i;
    uint8_t j;

    memset(&recorder, 0, sizeof(recorder));
    memset(&link, 0, sizeof(link));
    memset(&hooks, 0, sizeof(hooks));
    hooks.context = &recorder;
    hooks.observe = record_observation;
    if (aodv_core_init(&core, &config, 0u) != AODV_INIT_OK ||
        !init_router(&router, &link, &core, &hooks)) {
        CHECK("GTT-03", 0);
        return;
    }
    for (i = 0u; i < sizeof(positive) / sizeof(positive[0]); i++) {
        frame = make_frame(TAVRN_ROUTER_FRAME_COMMITTED, positive[i].type,
                           positive[i].hack_status);
        populate_subjects(&frame, &positive[i]);
        CHECK("GTT-03", tavrn_router_observe_frame(&router, &frame, i) ==
                             TAVRN_ROUTER_OBSERVE_REPORTED && recorder.count == i + 1u &&
                             recorder.observations[i].frame_type == positive[i].type &&
                             recorder.observations[i].subject_count ==
                                 positive[i].subject_count &&
                             memcmp(recorder.observations[i].transmitter.adva.bytes,
                                    adva_b, TAVRN_ADVA_LEN) == 0);
        for (j = 0u; j < positive[i].subject_count; j++) {
            tavrn_logical_id_t expected_logical =
                make_peer(j == 0u ? adva_c : adva_d).logical_id;

            CHECK("GTT-03", recorder.observations[i].subjects[j].role ==
                                positive[i].roles[j] &&
                                recorder.observations[i].subjects[j].serial_kind ==
                                    positive[i].serial_kinds[j] &&
                                recorder.observations[i].subjects[j].logical_id.width ==
                                    expected_logical.width &&
                                recorder.observations[i].subjects[j].logical_id.value ==
                                    expected_logical.value &&
                                recorder.observations[i].subjects[j].canonical_present == 0u &&
                                recorder.observations[i].subjects[j].hop_present != 0u);
        }
    }
    for (i = 0u; i < sizeof(negative) / sizeof(negative[0]); i++) {
        frame = make_frame(negative_admission[i], negative[i].type,
                           negative[i].hack_status);
        CHECK("GTT-03", tavrn_router_observe_frame(
                             &router, &frame, (uint32_t)(100u + i)) ==
                             TAVRN_ROUTER_OBSERVE_IGNORED && recorder.count ==
                             sizeof(positive) / sizeof(positive[0]));
    }

    frame = make_frame(TAVRN_ROUTER_FRAME_COMMITTED, TAVRN_WIRE_E_RERR,
                       TAVRN_HACK_ACCEPTED);
    frame.subject_count = 1u;
    set_subject(&frame, 0u, adva_c,
                TAVRN_ROUTER_EVIDENCE_ROUTE_DESTINATION,
                TAVRN_ROUTER_EVIDENCE_SERIAL_DESTINATION_SEQUENCE);
    CHECK("GTT-03", tavrn_router_observe_frame(&router, &frame, 200u) ==
                        TAVRN_ROUTER_OBSERVE_INVALID &&
                        recorder.count == sizeof(positive) / sizeof(positive[0]));

    frame = make_frame(TAVRN_ROUTER_FRAME_COMMITTED, TAVRN_WIRE_E_RREQ,
                       TAVRN_HACK_ACCEPTED);
    frame.subject_count = 1u;
    set_subject(&frame, 0u, adva_c, TAVRN_ROUTER_EVIDENCE_ORIGIN,
                TAVRN_ROUTER_EVIDENCE_SERIAL_DESTINATION_SEQUENCE);
    CHECK("GTT-03", tavrn_router_observe_frame(&router, &frame, 201u) ==
                        TAVRN_ROUTER_OBSERVE_INVALID);

    frame = make_frame(TAVRN_ROUTER_FRAME_COMMITTED, TAVRN_WIRE_E_RREP_ACK,
                       TAVRN_HACK_ACCEPTED);
    frame.subject_count = 1u;
    set_subject(&frame, 0u, adva_c,
                TAVRN_ROUTER_EVIDENCE_ROUTE_DESTINATION,
                TAVRN_ROUTER_EVIDENCE_SERIAL_DESTINATION_SEQUENCE);
    CHECK("GTT-03", tavrn_router_observe_frame(&router, &frame, 202u) ==
                        TAVRN_ROUTER_OBSERVE_INVALID);

    frame = make_frame(TAVRN_ROUTER_FRAME_COMMITTED, TAVRN_WIRE_DATA,
                       TAVRN_HACK_ACCEPTED);
    frame.subject_count = 1u;
    set_subject(&frame, 0u, adva_c, TAVRN_ROUTER_EVIDENCE_ORIGIN,
                TAVRN_ROUTER_EVIDENCE_SERIAL_DESTINATION_SEQUENCE);
    CHECK("GTT-03", tavrn_router_observe_frame(&router, &frame, 203u) ==
                        TAVRN_ROUTER_OBSERVE_INVALID);

    frame = make_frame(TAVRN_ROUTER_FRAME_COMMITTED, TAVRN_WIRE_FLOOD,
                       TAVRN_HACK_ACCEPTED);
    frame.subject_count = 1u;
    set_subject(&frame, 0u, adva_c, TAVRN_ROUTER_EVIDENCE_ORIGIN,
                TAVRN_ROUTER_EVIDENCE_SERIAL_NODE_SEQUENCE);
    CHECK("GTT-03", tavrn_router_observe_frame(&router, &frame, 204u) ==
                        TAVRN_ROUTER_OBSERVE_INVALID);

    frame = make_frame(TAVRN_ROUTER_FRAME_COMMITTED, TAVRN_WIRE_E_RERR,
                       TAVRN_HACK_ACCEPTED);
    frame.subject_count = 1u;
    set_subject(&frame, 0u, adva_c, TAVRN_ROUTER_EVIDENCE_REPORTER,
                TAVRN_ROUTER_EVIDENCE_SERIAL_NODE_SEQUENCE);
    CHECK("GTT-03", tavrn_router_observe_frame(&router, &frame, 205u) ==
                        TAVRN_ROUTER_OBSERVE_INVALID);

    frame = make_frame(TAVRN_ROUTER_FRAME_COMMITTED, TAVRN_WIRE_HACK,
                       TAVRN_HACK_ACCEPTED);
    frame.subject_count = 1u;
    set_subject(&frame, 0u, adva_c, TAVRN_ROUTER_EVIDENCE_ORIGIN,
                TAVRN_ROUTER_EVIDENCE_SERIAL_NONE);
    CHECK("GTT-03", tavrn_router_observe_frame(&router, &frame, 206u) ==
                        TAVRN_ROUTER_OBSERVE_INVALID);

    frame = make_frame(TAVRN_ROUTER_FRAME_COMMITTED, TAVRN_WIRE_DATA,
                       TAVRN_HACK_ACCEPTED);
    frame.subject_count = TAVRN_ROUTER_EVIDENCE_CAPACITY + 1u;
    CHECK("GTT-03", tavrn_router_observe_frame(&router, &frame, 207u) ==
                        TAVRN_ROUTER_OBSERVE_INVALID);
}

static void test_gtt_05_observation_never_mutates_routes(void)
{
    aodv_core_t core;
    tavrn_link_v2_t link;
    aodv_core_t before;
    aodv_core_config_t config = make_core_config();
    tavrn_router_t router;
    observation_recorder_t recorder;
    tavrn_router_augmentation_hooks_t hooks;
    tavrn_router_frame_observation_t frame;
    aodv_route_snapshot_t route;
    tavrn_logical_id_t destination = make_peer(adva_c).logical_id;

    memset(&recorder, 0, sizeof(recorder));
    memset(&link, 0, sizeof(link));
    memset(&hooks, 0, sizeof(hooks));
    hooks.context = &recorder;
    hooks.observe = record_observation;
    if (aodv_core_init(&core, &config, 0u) != AODV_INIT_OK ||
        !install_expiring_route(&core) ||
        !init_router(&router, &link, &core, &hooks)) {
        CHECK("GTT-05", 0);
        return;
    }
    before = core;
    frame = make_frame(TAVRN_ROUTER_FRAME_COMMITTED, TAVRN_WIRE_E_RREP,
                       TAVRN_HACK_ACCEPTED);
    CHECK("GTT-05", tavrn_router_observe_frame(&router, &frame, 2u) ==
                        TAVRN_ROUTER_OBSERVE_REPORTED && recorder.count == 1u &&
                        memcmp(&core, &before, sizeof(core)) == 0);
    CHECK("GTT-05", aodv_core_tick(&core, 11u) == AODV_STATUS_OK &&
                        aodv_core_route_snapshot(&core, &destination, &route) ==
                            AODV_ROUTE_QUERY_FOUND &&
                        route.state == AODV_ROUTE_INVALID && recorder.count == 1u);
}

static void test_router_candidate_reservation_port(void)
{
    tavrn_router_t router;
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    aodv_core_t core;
    tavrn_link_event_t event;
    tavrn_link_data_t local_data = make_transit_data(0x8501u, adva_a);
    tavrn_link_data_t transit_data = make_transit_data(0x8502u, adva_c);
    candidate_port_recorder_t recorder;
    tavrn_router_candidate_reservation_port_t port;
    tavrn_direct_peer_t peer_c = make_peer(adva_c);

    if (!setup_live_router(&router, &link, &scheduler, &core)) {
        CHECK("GTT-05", 0);
        return;
    }
    event = seed_candidate(&link, &local_data, 1u);
    CHECK("GTT-05", tavrn_router_handle_link_event(&router, &event, 1u) ==
                        TAVRN_ROUTER_EVENT_OK &&
                        link.candidate_valid == 0u &&
                        link.counters.rx_candidate_accepted == 1u &&
                        tavrn_router_fault_reason(&router) == TAVRN_ROUTER_FAULT_NONE);

    if (!setup_live_router(&router, &link, &scheduler, &core) ||
        tavrn_router_set_candidate_reservation_port(&router, NULL) !=
            TAVRN_ROUTER_EVENT_OK) {
        CHECK("GTT-05", 0);
        return;
    }
    event = seed_candidate(&link, &local_data, 1u);
    CHECK("GTT-05", tavrn_router_handle_link_event(&router, &event, 1u) ==
                        TAVRN_ROUTER_EVENT_OK &&
                        link.candidate_valid == 0u &&
                        link.counters.rx_candidate_accepted == 1u &&
                        tavrn_router_fault_reason(&router) == TAVRN_ROUTER_FAULT_NONE);

    memset(&recorder, 0, sizeof(recorder));
    recorder.reserve_status = TAVRN_ROUTER_CANDIDATE_RESERVED;
    recorder.commit_status = TAVRN_ROUTER_CANDIDATE_COMPLETION_OK;
    recorder.rollback_status = TAVRN_ROUTER_CANDIDATE_COMPLETION_OK;
    recorder.token = 9u;
    port = candidate_port(&recorder);
    if (!setup_live_router(&router, &link, &scheduler, &core) ||
        tavrn_router_set_candidate_reservation_port(&router, &port) !=
            TAVRN_ROUTER_EVENT_OK) {
        CHECK("GTT-05", 0);
        return;
    }
    event = seed_candidate(&link, &transit_data, 2u);
    CHECK("GTT-05", tavrn_router_handle_link_event(&router, &event, 2u) ==
                        TAVRN_ROUTER_EVENT_OK && recorder.reserve_calls == 1u &&
                        recorder.commit_calls == 1u && recorder.rollback_calls == 0u &&
                         recorder.commit_exact != 0u && link.candidate_valid == 0u &&
                         link.counters.rx_candidate_accepted == 1u &&
                         transit_data_is_pinned(&link, &transit_data) &&
                         transit_data_has_external_custody_owner(&link, &transit_data) &&
                         aodv_core_poll_action(&core, &(aodv_action_t){0}) ==
                             AODV_ACTION_POLL_EMPTY);

    memset(&recorder, 0, sizeof(recorder));
    recorder.reserve_status = TAVRN_ROUTER_CANDIDATE_BUSY;
    recorder.commit_status = TAVRN_ROUTER_CANDIDATE_COMPLETION_OK;
    recorder.rollback_status = TAVRN_ROUTER_CANDIDATE_COMPLETION_OK;
    port = candidate_port(&recorder);
    if (!setup_live_router(&router, &link, &scheduler, &core) ||
        tavrn_router_set_candidate_reservation_port(&router, &port) !=
            TAVRN_ROUTER_EVENT_OK) {
        CHECK("GTT-05", 0);
        return;
    }
    event = seed_candidate(&link, &transit_data, 3u);
    CHECK("GTT-05", tavrn_router_handle_link_event(&router, &event, 3u) ==
                        TAVRN_ROUTER_EVENT_BUSY && recorder.reserve_calls == 1u &&
                        recorder.commit_calls == 0u && recorder.rollback_calls == 0u &&
                        link.candidate_valid == 0u && link.counters.rx_candidate_busy == 1u &&
                        !transit_data_is_pinned(&link, &transit_data));

    memset(&recorder, 0, sizeof(recorder));
    recorder.reserve_status = TAVRN_ROUTER_CANDIDATE_RESERVED;
    recorder.commit_status = TAVRN_ROUTER_CANDIDATE_COMPLETION_OK;
    recorder.rollback_status = TAVRN_ROUTER_CANDIDATE_COMPLETION_OK;
    recorder.token = 10u;
    port = candidate_port(&recorder);
    if (!setup_live_router(&router, &link, &scheduler, &core) ||
        tavrn_router_set_candidate_reservation_port(&router, &port) !=
            TAVRN_ROUTER_EVENT_OK) {
        CHECK("GTT-05", 0);
        return;
    }
    event = seed_candidate(&link, &transit_data, 4u);
    link.mesh_fault_latched = 1u;
    CHECK("GTT-05", tavrn_router_handle_link_event(&router, &event, 4u) ==
                        TAVRN_ROUTER_EVENT_INVALID && recorder.reserve_calls == 1u &&
                        recorder.commit_calls == 0u && recorder.rollback_calls == 1u &&
                        recorder.rollback_exact != 0u &&
                        tavrn_router_fault_reason(&router) == TAVRN_ROUTER_FAULT_NONE);

    memset(&recorder, 0, sizeof(recorder));
    recorder.reserve_status = TAVRN_ROUTER_CANDIDATE_RESERVED;
    recorder.commit_status = TAVRN_ROUTER_CANDIDATE_COMPLETION_OK;
    recorder.rollback_status = TAVRN_ROUTER_CANDIDATE_COMPLETION_INVALID;
    recorder.token = 12u;
    port = candidate_port(&recorder);
    if (!setup_live_router(&router, &link, &scheduler, &core) ||
        tavrn_router_set_candidate_reservation_port(&router, &port) !=
            TAVRN_ROUTER_EVENT_OK) {
        CHECK("GTT-05", 0);
        return;
    }
    event = seed_candidate(&link, &transit_data, 5u);
    link.mesh_fault_latched = 1u;
    CHECK("GTT-05", tavrn_router_handle_link_event(&router, &event, 5u) ==
                        TAVRN_ROUTER_EVENT_INVALID && recorder.reserve_calls == 1u &&
                        recorder.commit_calls == 0u && recorder.rollback_calls == 1u &&
                        recorder.rollback_exact != 0u &&
                        tavrn_router_fault_reason(&router) ==
                            TAVRN_ROUTER_FAULT_CANDIDATE_ROLLBACK_INVALID);

    memset(&recorder, 0, sizeof(recorder));
    recorder.reserve_status = TAVRN_ROUTER_CANDIDATE_RESERVED;
    recorder.commit_status = TAVRN_ROUTER_CANDIDATE_COMPLETION_INVALID;
    recorder.rollback_status = TAVRN_ROUTER_CANDIDATE_COMPLETION_OK;
    recorder.token = 11u;
    port = candidate_port(&recorder);
    if (!setup_live_router(&router, &link, &scheduler, &core) ||
        tavrn_router_set_candidate_reservation_port(&router, &port) !=
            TAVRN_ROUTER_EVENT_OK) {
        CHECK("GTT-05", 0);
        return;
    }
    event = seed_candidate(&link, &transit_data, 6u);
    CHECK("GTT-05", tavrn_router_handle_link_event(&router, &event, 6u) ==
                        TAVRN_ROUTER_EVENT_INVALID && recorder.reserve_calls == 1u &&
                        recorder.commit_calls == 1u && recorder.commit_exact != 0u &&
                         transit_data_is_pinned(&link, &transit_data) &&
                         tavrn_router_fault_reason(&router) ==
                             TAVRN_ROUTER_FAULT_CANDIDATE_COMMIT_INVALID);

    memset(&recorder, 0, sizeof(recorder));
    recorder.reserve_status = TAVRN_ROUTER_CANDIDATE_RESERVED;
    recorder.commit_status = TAVRN_ROUTER_CANDIDATE_COMPLETION_OK;
    recorder.rollback_status = TAVRN_ROUTER_CANDIDATE_COMPLETION_OK;
    recorder.token = 13u;
    port = candidate_port(&recorder);
    if (!setup_live_router(&router, &link, &scheduler, &core) ||
        tavrn_router_set_candidate_reservation_port(&router, &port) !=
            TAVRN_ROUTER_EVENT_OK) {
        CHECK("GTT-05", 0);
        return;
    }
    event = seed_candidate(&link, &transit_data, 7u);
    CHECK("GTT-05", tavrn_link_v2_send_unicast(&link, &peer_c, &transit_data, 7u,
                                                  &(tavrn_link_event_t){0}) ==
                         TAVRN_LINK_SEND_OK &&
                         tavrn_router_handle_link_event(&router, &event, 7u) ==
                             TAVRN_ROUTER_EVENT_INVALID &&
                         recorder.reserve_calls == 1u && recorder.commit_calls == 1u &&
                         transit_data_is_pinned(&link, &transit_data) &&
                         !transit_data_has_external_custody_owner(&link,
                                                                    &transit_data) &&
                         tavrn_router_fault_reason(&router) ==
                             TAVRN_ROUTER_FAULT_CANDIDATE_COMMIT_INVALID);
}

static void test_router_data_terminal_ownership_port(void)
{
    tavrn_router_t router;
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    aodv_core_t core;
    tavrn_link_data_t data = make_transit_data(0x8601u, adva_c);
    tavrn_link_event_t event;
    terminal_hook_recorder_t recorder;
    tavrn_router_data_terminal_hook_t hook;

    memset(&recorder, 0, sizeof(recorder));
    recorder.disposition = TAVRN_ROUTER_DATA_TERMINAL_OWNED;
    memset(&hook, 0, sizeof(hook));
    hook.context = &recorder;
    hook.handle = record_data_terminal;
    if (!setup_live_router(&router, &link, &scheduler, &core) ||
        tavrn_router_set_data_terminal_hook(&router, &hook) != TAVRN_ROUTER_EVENT_OK) {
        CHECK("GTT-05", 0);
        return;
    }
    pin_transit_data(&link, &data);
    memset(&event, 0, sizeof(event));
    event.type = TAVRN_LINK_EVENT_RETRY_EXHAUSTED;
    event.detail.owned_data.next_hop = make_peer(adva_c);
    event.detail.owned_data.data = data;
    event.detail.owned_data.attempt_count = 3u;
    CHECK("GTT-05", tavrn_router_handle_link_event(&router, &event, 6u) ==
                        TAVRN_ROUTER_EVENT_OK && recorder.calls == 1u &&
                         recorder.event.type == TAVRN_LINK_EVENT_RETRY_EXHAUSTED &&
                         memcmp(&recorder.event.detail.owned_data, &event.detail.owned_data,
                                sizeof(event.detail.owned_data)) == 0 &&
                         transit_data_is_pinned(&link, &data) &&
                         transit_data_has_external_custody_owner(&link, &data) &&
                         tavrn_router_fault_reason(&router) == TAVRN_ROUTER_FAULT_NONE);

    memset(&recorder, 0, sizeof(recorder));
    recorder.disposition = TAVRN_ROUTER_DATA_TERMINAL_OWNED;
    hook.context = &recorder;
    if (!setup_live_router(&router, &link, &scheduler, &core) ||
        tavrn_router_set_data_terminal_hook(&router, &hook) != TAVRN_ROUTER_EVENT_OK) {
        CHECK("GTT-05", 0);
        return;
    }
    pin_transit_data(&link, &data);
    memset(&event, 0, sizeof(event));
    event.type = TAVRN_LINK_EVENT_RETRY_EXHAUSTED;
    event.detail.owned_data.next_hop = make_peer(adva_c);
    event.detail.owned_data.data = data;
    CHECK("GTT-05", tavrn_link_v2_transfer_rx_custody_to_external(
                         &link, &data, 7u) == TAVRN_LINK_RESOLVE_OK &&
                         tavrn_router_handle_link_event(&router, &event, 7u) ==
                             TAVRN_ROUTER_EVENT_INVALID &&
                         recorder.calls == 1u && transit_data_is_pinned(&link, &data) &&
                         transit_data_has_external_custody_owner(&link, &data) &&
                         tavrn_router_fault_reason(&router) ==
                             TAVRN_ROUTER_FAULT_DATA_TERMINAL_HOOK_INVALID);

    memset(&recorder, 0, sizeof(recorder));
    recorder.disposition = TAVRN_ROUTER_DATA_TERMINAL_OBSERVED;
    hook.context = &recorder;
    if (!setup_live_router(&router, &link, &scheduler, &core) ||
        tavrn_router_set_data_terminal_hook(&router, &hook) != TAVRN_ROUTER_EVENT_OK) {
        CHECK("GTT-05", 0);
        return;
    }
    pin_transit_data(&link, &data);
    memset(&event, 0, sizeof(event));
    event.type = TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED;
    event.detail.transferred_data.next_hop = make_peer(adva_c);
    event.detail.transferred_data.data = data;
    event.detail.transferred_data.status = TAVRN_HACK_ACCEPTED;
    CHECK("GTT-05", tavrn_router_handle_link_event(&router, &event, 7u) ==
                        TAVRN_ROUTER_EVENT_OK && recorder.calls == 1u &&
                        recorder.event.type == TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED &&
                        !transit_data_is_pinned(&link, &data));

    pin_transit_data(&link, &data);
    memset(&event, 0, sizeof(event));
    event.type = TAVRN_LINK_EVENT_RETRY_EXHAUSTED;
    event.detail.owned_data.next_hop = make_peer(adva_c);
    event.detail.owned_data.data = data;
    CHECK("GTT-05", tavrn_router_handle_link_event(&router, &event, 8u) ==
                        TAVRN_ROUTER_EVENT_OK && recorder.calls == 2u &&
                        !transit_data_is_pinned(&link, &data) &&
                        router.failures[0].valid == 0u &&
                        tavrn_router_fault_reason(&router) == TAVRN_ROUTER_FAULT_NONE);

    memset(&recorder, 0, sizeof(recorder));
    recorder.disposition = TAVRN_ROUTER_DATA_TERMINAL_OWNED;
    hook.context = &recorder;
    if (!setup_live_router(&router, &link, &scheduler, &core) ||
        tavrn_router_set_data_terminal_hook(&router, &hook) != TAVRN_ROUTER_EVENT_OK) {
        CHECK("GTT-05", 0);
        return;
    }
    pin_transit_data(&link, &data);
    memset(&event, 0, sizeof(event));
    event.type = TAVRN_LINK_EVENT_CUSTODY_REJECTED;
    event.detail.owned_data.next_hop = make_peer(adva_c);
    event.detail.owned_data.data = data;
    CHECK("GTT-05", tavrn_router_handle_link_event(&router, &event, 9u) ==
                        TAVRN_ROUTER_EVENT_INVALID && recorder.calls == 1u &&
                        transit_data_is_pinned(&link, &data) &&
                        tavrn_router_fault_reason(&router) ==
                            TAVRN_ROUTER_FAULT_DATA_TERMINAL_HOOK_INVALID);
}

static void test_router_reforward_transit_data(void)
{
    tavrn_router_t router;
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    aodv_core_t core;
    aodv_core_t core_before;
    tavrn_link_data_t data = make_transit_data(0x8701u, adva_c);
    tavrn_direct_peer_t next_hop;

    if (!setup_live_router(&router, &link, &scheduler, &core) ||
        !install_expiring_route(&core)) {
        CHECK("GTT-05", 0);
        return;
    }
    core_before = core;
    CHECK("GTT-05", tavrn_router_reforward_transit_data(&router, &data, 2u,
                                                           &next_hop) ==
                        TAVRN_ROUTER_REFORWARD_OK &&
                        memcmp(next_hop.adva.bytes, adva_c, TAVRN_ADVA_LEN) == 0 &&
                        next_hop.logical_id.value == make_peer(adva_c).logical_id.value &&
                        link.custody[0].phase != TAVRN_CUSTODY_FREE &&
                        memcmp(&link.custody[0].data, &data, sizeof(data)) == 0 &&
                        link.custody[0].data.ttl == data.ttl &&
                        link.custody[0].data.hops == data.hops &&
                        memcmp(&core, &core_before, sizeof(core)) == 0);
}

static void test_router_reforward_gates_oldest_failure(void)
{
    tavrn_router_t router;
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    aodv_core_t core;
    tavrn_link_data_t data = make_transit_data(0x8702u, adva_c);

    if (!setup_live_router(&router, &link, &scheduler, &core) ||
        !install_expiring_route(&core)) {
        CHECK("GTT-05", 0);
        return;
    }
    router.failures[0].peer = make_peer(adva_c);
    router.failures[0].admission_order = 1u;
    router.failures[0].valid = 1u;
    router.failures[1].peer = make_peer(adva_d);
    router.failures[1].admission_order = 2u;
    router.failures[1].valid = 1u;
    core.config.rerr_rate_per_second = 0u;
    CHECK("GTT-05", tavrn_router_reforward_transit_data(&router, &data, 2u, NULL) ==
                         TAVRN_ROUTER_REFORWARD_BUSY &&
                         router.failures[0].valid != 0u &&
                         router.failures[1].valid != 0u &&
                         link.custody[0].phase == TAVRN_CUSTODY_FREE &&
                         link.custody[1].phase == TAVRN_CUSTODY_FREE);
}

static void test_router_transit_pin_requires_exact_data(void)
{
    tavrn_router_t router;
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    aodv_core_t core;
    tavrn_link_data_t data = make_transit_data(0x8703u, adva_c);
    tavrn_link_data_t wrong = data;

    wrong.data_seq++;
    if (!setup_live_router(&router, &link, &scheduler, &core)) {
        CHECK("GTT-05", 0);
        return;
    }
    pin_transit_data(&link, &data);
    CHECK("GTT-05", tavrn_router_release_transit_pin(&router, &wrong, 3u) ==
                         TAVRN_ROUTER_EVENT_INVALID &&
                         transit_data_is_pinned(&link, &data) &&
                         tavrn_router_fault_reason(&router) ==
                             TAVRN_ROUTER_FAULT_TRANSIT_CUSTODY_RELEASE_INVALID);
    CHECK("GTT-05", tavrn_router_release_transit_pin(&router, &data, 4u) ==
                         TAVRN_ROUTER_EVENT_OK &&
                         !transit_data_is_pinned(&link, &data));

    if (!setup_live_router(&router, &link, &scheduler, &core)) {
        CHECK("GTT-05", 0);
        return;
    }
    pin_transit_data(&link, &data);
    CHECK("GTT-05", tavrn_router_release_transit_pin(&router, &data, 5u) ==
                         TAVRN_ROUTER_EVENT_OK &&
                         !transit_data_is_pinned(&link, &data) &&
                         tavrn_router_release_transit_pin(&router, &data, 6u) ==
                             TAVRN_ROUTER_EVENT_INVALID &&
                         tavrn_router_fault_reason(&router) ==
                             TAVRN_ROUTER_FAULT_TRANSIT_CUSTODY_RELEASE_INVALID);
}

static void test_router_sid16_application_width_rejected_before_aodv(void)
{
    tavrn_router_t router;
    tavrn_link_v2_t link;
    ble_mesh_scheduler_t scheduler;
    aodv_core_t core;
    tron_application_data_t application;
    aodv_core_t core_before;
    tavrn_link_v2_t link_before;

    if (!setup_live_router(&router, &link, &scheduler, &core)) {
        CHECK("GTT-01", 0);
        return;
    }
    memset(&application, 0, sizeof(application));
    application.final_destination = make_peer(adva_c).logical_id;
    application.app_kind = 0x7fu;
    application.app_len = 8u;
    core_before = core;
    link_before = link;
    CHECK("GTT-01", tavrn_router_submit_application(&router, &application, 0u) ==
                         AODV_STATUS_INVALID &&
                         memcmp(&core, &core_before, sizeof(core)) == 0 &&
                         memcmp(&link, &link_before, sizeof(link)) == 0 &&
                         scheduler.routed_tx_queue.count == 0u &&
                         aodv_core_poll_action(&core, &(aodv_action_t){0}) ==
                             AODV_ACTION_POLL_EMPTY);
}

int main(void)
{
    test_gtt_01_generic_hook_isolation();
    test_gtt_03_frame_role_admission();
    test_gtt_05_observation_never_mutates_routes();
    test_router_candidate_reservation_port();
    test_router_data_terminal_ownership_port();
    test_router_reforward_transit_data();
    test_router_reforward_gates_oldest_failure();
    test_router_transit_pin_requires_exact_data();
    test_router_sid16_application_width_rejected_before_aodv();
    if (failures != 0u) {
        printf("tavrn_router RED tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_router tests passed\n");
    return 0;
}
