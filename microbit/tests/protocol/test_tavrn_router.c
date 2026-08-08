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

int main(void)
{
    test_gtt_01_generic_hook_isolation();
    test_gtt_03_frame_role_admission();
    test_gtt_05_observation_never_mutates_routes();
    if (failures != 0u) {
        printf("tavrn_router RED tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_router tests passed\n");
    return 0;
}
