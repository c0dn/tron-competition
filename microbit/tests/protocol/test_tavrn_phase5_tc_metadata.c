#include "tavrn_gtt.h"
#include "tavrn_full.h"
#include "tavrn_full_maintenance_binding.h"
#include "tavrn_link_v2.h"
#include "tavrn_mentorship.h"
#include "tavrn_phase5_tc_metadata_contract.h"
#include "tavrn_router.h"
#include "tavrn_wire_v2.h"

#include "aodv_core.h"
#include "ble_mesh_scheduler.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define NETWORK_ID 0x2au

static unsigned int failures;
static unsigned int structural_failures;
static const char *reported[16];
static unsigned int reported_count;

static const uint8_t adva_a[TAVRN_ADVA_LEN] = { 0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu };
static const uint8_t adva_b[TAVRN_ADVA_LEN] = { 0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u };
static const uint8_t adva_c[TAVRN_ADVA_LEN] = { 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0xc1u };
static const uint8_t adva_d[TAVRN_ADVA_LEN] = { 0x34u, 0x56u, 0x78u, 0x9au, 0xbcu, 0xc2u };
static const uint8_t adva_e[TAVRN_ADVA_LEN] = { 0x55u, 0x66u, 0x77u, 0x88u, 0x99u, 0xc3u };
static const uint8_t adva_f[TAVRN_ADVA_LEN] = { 0x66u, 0x77u, 0x88u, 0x99u, 0xaau, 0xc4u };

static int first_for(const char *requirement)
{
    unsigned int index;

    for (index = 0u; index < reported_count; index++) {
        if (strcmp(reported[index], requirement) == 0) return 0;
    }
    reported[reported_count++] = requirement;
    return 1;
}

#define CHECK(requirement, expression) \
    do { \
        if (!(expression)) { \
            if (first_for(requirement)) { \
                printf("FAIL %s: %s:%d: assertion failed: %s\n", \
                       requirement, __FILE__, __LINE__, #expression); \
            } \
            failures++; \
        } \
    } while (0)

#define STRUCTURAL(label, expression) \
    do { \
        if (!(expression)) { \
            printf("STRUCTURAL %s: %s:%d: %s\n", label, __FILE__, __LINE__, #expression); \
            structural_failures++; \
        } \
    } while (0)

#define REACHED(name) printf("REACHED %s\n", name)

static tavrn_adva_t adva(const uint8_t bytes[TAVRN_ADVA_LEN])
{
    tavrn_adva_t value;

    memcpy(value.bytes, bytes, TAVRN_ADVA_LEN);
    return value;
}

#if defined(TAVRN_PHASE5_TC_METADATA_CORRECTIVE_MODE)
static tavrn_adva_t generated_adva(uint8_t suffix)
{
    tavrn_adva_t value;

    memset(&value, 0, sizeof(value));
    value.bytes[0] = suffix;
    value.bytes[1] = 0xa5u;
    value.bytes[2] = 0x5au;
    value.bytes[3] = 0x3cu;
    value.bytes[4] = 0xc3u;
    value.bytes[5] = 0xc5u;
    return value;
}
#endif

static void put_u16(uint8_t pdu[24], uint8_t offset, uint16_t value)
{
    pdu[offset] = (uint8_t)value;
    pdu[(uint8_t)(offset + 1u)] = (uint8_t)(value >> 8);
}

static void make_tc(uint8_t pdu[24], const uint8_t origin[TAVRN_ADVA_LEN],
                    const uint8_t subject[TAVRN_ADVA_LEN], uint16_t sequence,
                    uint8_t ttl, uint8_t hops, tavrn_tc_event_t event)
{
    memset(pdu, 0, 24u);
    pdu[0] = 0x54u;
    pdu[1] = 0x52u;
    pdu[2] = 0x02u;
    pdu[3] = NETWORK_ID;
    pdu[4] = TAVRN_WIRE_TC_UPDATE;
    pdu[6] = (uint8_t)((ttl << 4) | hops);
    memcpy(&pdu[7], origin, TAVRN_ADVA_LEN);
    put_u16(pdu, 13u, sequence);
    memcpy(&pdu[15], subject, TAVRN_ADVA_LEN);
    pdu[21] = (uint8_t)event;
    put_u16(pdu, 22u, 0x1234u);
}

static tavrn_tc_metadata_config_t config(void)
{
    tavrn_tc_metadata_config_t value;

    memset(&value, 0, sizeof(value));
    value.local_identity = adva(adva_a);
    value.initial_tc_sequence = 0xffffu;
    value.tc_uuid_ms = 30000u;
    value.tc_subject_ms = 1000u;
    value.metadata_cooldown_ms = 5000u;
    return value;
}

static tavrn_metadata_candidate_t candidate(const uint8_t subject[TAVRN_ADVA_LEN],
                                             tavrn_metadata_kind_t kind,
                                             uint8_t bucket, uint8_t request)
{
    tavrn_metadata_candidate_t value;

    memset(&value, 0, sizeof(value));
    value.subject = adva(subject);
    value.subject_sid8 = subject[0];
    value.ttl_bucket = bucket;
    value.freshness_request = request;
    value.kind = kind;
    return value;
}

static tavrn_metadata_selection_t general_selection(uint8_t count)
{
    tavrn_metadata_selection_t selection;

    memset(&selection, 0, sizeof(selection));
    selection.valid = 1u;
    selection.count = count;
    selection.entries[0] = candidate(adva_b, TAVRN_METADATA_SOFT_REQUEST, 2u, 1u);
    selection.entries[1] = candidate(adva_c, TAVRN_METADATA_SUBJECT_SELF_ANSWER, 4u, 0u);
    selection.entries[2] = candidate(adva_d, TAVRN_METADATA_INTERMEDIARY_ANSWER, 5u, 0u);
    selection.entries[3] = candidate(adva_e, TAVRN_METADATA_SOFT_REQUEST, 3u, 1u);
    return selection;
}

static tavrn_validated_control_t route_control(tavrn_metadata_frame_kind_t kind,
                                               uint8_t unreachable_count)
{
    tavrn_validated_control_t control;
    uint8_t index;

    memset(&control, 0, sizeof(control));
    control.pdu[0] = 0x54u;
    control.pdu[1] = 0x52u;
    control.pdu[2] = 0x02u;
    control.pdu[3] = NETWORK_ID;
    control.pdu[6] = 0xf0u;
    switch (kind) {
    case TAVRN_METADATA_FRAME_RREQ8:
        control.type = TAVRN_WIRE_E_RREQ;
        control.pdu_len = 15u;
        control.pdu[4] = control.type;
        control.pdu[5] = 0x80u;
        control.pdu[7] = adva_a[0];
        control.pdu[8] = 1u;
        control.pdu[10] = adva_b[0];
        control.pdu[11] = 1u;
        control.pdu[13] = 1u;
        break;
    case TAVRN_METADATA_FRAME_RREP8:
        control.type = TAVRN_WIRE_E_RREP;
        control.pdu_len = 16u;
        control.pdu[4] = control.type;
        control.pdu[5] = 0x80u;
        control.pdu[7] = adva_a[0];
        control.pdu[8] = adva_b[0];
        control.pdu[9] = 1u;
        control.pdu[11] = adva_c[0];
        control.pdu[12] = 1u;
        control.pdu[14] = 1u;
        break;
    case TAVRN_METADATA_FRAME_RERR8:
        control.type = TAVRN_WIRE_E_RERR;
        control.pdu_len = (uint8_t)(11u + 3u * unreachable_count);
        control.pdu[4] = control.type;
        control.pdu[5] = 0x80u;
        control.pdu[7] = adva_a[0];
        control.pdu[8] = 1u;
        control.pdu[10] = unreachable_count;
        for (index = 0u; index < unreachable_count; index++) {
            uint8_t offset = (uint8_t)(11u + 3u * index);

            control.pdu[offset] = (uint8_t)(0x20u + index);
            control.pdu[(uint8_t)(offset + 1u)] = 1u;
        }
        break;
    case TAVRN_METADATA_FRAME_RREQ16:
        control.type = TAVRN_WIRE_E_RREQ;
        control.pdu_len = 17u;
        control.pdu[4] = control.type;
        control.pdu[7] = adva_a[0];
        control.pdu[8] = adva_a[1];
        control.pdu[9] = 1u;
        control.pdu[11] = adva_b[0];
        control.pdu[12] = adva_b[1];
        control.pdu[13] = 1u;
        control.pdu[15] = 1u;
        break;
    case TAVRN_METADATA_FRAME_RREP_ACK:
        control.type = TAVRN_WIRE_E_RREP_ACK;
        control.pdu_len = 13u;
        control.pdu[4] = control.type;
        control.pdu[5] = 0x80u;
        control.pdu[6] = adva_a[0];
        control.pdu[7] = adva_b[0];
        control.pdu[9] = adva_c[0];
        control.pdu[10] = 1u;
        break;
    default:
        break;
    }
    return control;
}

static int control_round_trip_identity_admission(
    void *context, const tavrn_logical_id_t *logical_id,
    const tavrn_adva_t *direct_adva_or_null)
{
    (void)context;
    if (logical_id == NULL || logical_id->width != TAVRN_IDENTITY_SID8 ||
        logical_id->value == 0u || logical_id->value > 0xfeu) {
        return 1;
    }
    return direct_adva_or_null != NULL &&
        direct_adva_or_null->bytes[0] != (uint8_t)logical_id->value;
}

static int control_round_trip(const tavrn_validated_control_t *control,
                              const uint8_t outer_adva[TAVRN_ADVA_LEN],
                              tavrn_rx_control_event_t *event_out)
{
    tavrn_codec_config_t codec;
    tavrn_decoded_frame_t frame;
    tavrn_decoded_frame_t decoded;
    uint8_t adv_data[31];
    size_t adv_len = 0u;

    if (control == NULL || outer_adva == NULL || event_out == NULL) return 0;
    memset(&codec, 0, sizeof(codec));
    codec.network_id = NETWORK_ID;
    codec.local_peer.logical_id.width = TAVRN_IDENTITY_SID8;
    codec.local_peer.logical_id.value = adva_a[0];
    codec.local_peer.adva = adva(adva_a);
    codec.identity_conflict = control_round_trip_identity_admission;
    memset(&frame, 0, sizeof(frame));
    frame.type = control->type;
    frame.network_id = NETWORK_ID;
    frame.transmitter.logical_id.width = TAVRN_IDENTITY_SID8;
    frame.transmitter.logical_id.value = outer_adva[0];
    frame.transmitter.adva = adva(outer_adva);
    frame.detail.control = *control;
    if (tavrn_wire_v2_encode(&codec, &frame, adv_data, sizeof(adv_data), &adv_len) !=
            TAVRN_CODEC_OK ||
        tavrn_wire_v2_decode(&codec, outer_adva, adv_data, adv_len, &decoded) !=
            TAVRN_CODEC_OK || decoded.type != control->type) {
        return 0;
    }
    memset(event_out, 0, sizeof(*event_out));
    event_out->transmitter = decoded.transmitter;
    event_out->control = decoded.detail.control;
    return 1;
}

static void verify_existing_join_and_tc_codec(void)
{
    tavrn_mentorship_t mentorship;
    tavrn_gtt_t gtt;
    tavrn_router_t router;
    tavrn_link_v2_t link;
    tavrn_validated_control_t join;
    tavrn_decoded_frame_t frame;
    tavrn_decoded_frame_t decoded;
    tavrn_codec_config_t codec;
    uint8_t adv_data[31];
    size_t adv_len = 0u;

    /* This is production code, not a RED stand-in: preserve Phase 4 JOIN bytes. */
    memset(&mentorship, 0, sizeof(mentorship));
    memset(&gtt, 0, sizeof(gtt));
    memset(&router, 0, sizeof(router));
    memset(&link, 0, sizeof(link));
    gtt.config.local_identity = adva(adva_a);
    link.config.network_id = NETWORK_ID;
    router.link = &link;
    mentorship.gtt = &gtt;
    mentorship.router = &router;
    STRUCTURAL("legacy-join-build",
               tavrn_mentorship_build_join(&mentorship, 0xffffu, &join) ==
                   TAVRN_MENTORSHIP_OK);
    STRUCTURAL("legacy-join-exact",
               join.type == TAVRN_WIRE_TC_UPDATE && join.pdu_len == 24u &&
               join.pdu[5] == 0u && join.pdu[6] == 0xf0u &&
               memcmp(&join.pdu[7], adva_a, TAVRN_ADVA_LEN) == 0 &&
               join.pdu[13] == 0xffu && join.pdu[14] == 0xffu &&
               memcmp(&join.pdu[15], adva_a, TAVRN_ADVA_LEN) == 0 &&
               join.pdu[21] == TAVRN_TC_EVENT_JOIN);

    memset(&codec, 0, sizeof(codec));
    codec.network_id = NETWORK_ID;
    codec.local_peer.logical_id.width = TAVRN_IDENTITY_SID16;
    codec.local_peer.logical_id.value = 0x4218u;
    codec.local_peer.adva = adva(adva_a);
    memset(&frame, 0, sizeof(frame));
    frame.type = TAVRN_WIRE_TC_UPDATE;
    frame.network_id = NETWORK_ID;
    frame.transmitter = codec.local_peer;
    frame.detail.control = join;
    STRUCTURAL("legacy-join-wire-encode",
               tavrn_wire_v2_encode(&codec, &frame, adv_data, sizeof(adv_data), &adv_len) ==
                   TAVRN_CODEC_OK && adv_len == 31u);
    STRUCTURAL("legacy-join-wire-decode",
               tavrn_wire_v2_decode(&codec, adva_a, adv_data, adv_len, &decoded) ==
                   TAVRN_CODEC_OK && decoded.type == TAVRN_WIRE_TC_UPDATE &&
               decoded.detail.control.pdu[6] == 0xf0u);
}

static void test_confirmed_origin_and_shared_sequence(void)
{
    static const tavrn_tc_origin_cause_t negative_causes[] = {
        TAVRN_TC_ORIGIN_NO_DEMAND_EXPIRY,
        TAVRN_TC_ORIGIN_RREQ_EXHAUSTED,
        TAVRN_TC_ORIGIN_BUSY,
        TAVRN_TC_ORIGIN_REJECTED,
        TAVRN_TC_ORIGIN_ENQUEUE_FAILURE,
        TAVRN_TC_ORIGIN_ZERO_CHANNEL_FAILURE,
        TAVRN_TC_ORIGIN_DEADLINE_EXPIRY,
        TAVRN_TC_ORIGIN_RREP_ACK_TIMEOUT,
        TAVRN_TC_ORIGIN_RADIO_FAULT,
        TAVRN_TC_ORIGIN_SERVICE_FAULT,
        TAVRN_TC_ORIGIN_REPAIR_FAILURE,
    };
    tavrn_tc_metadata_state_t state;
    tavrn_tc_metadata_action_t action;
    tavrn_tc_metadata_action_t retained;
    tavrn_tc_metadata_snapshot_t snapshot;
    tavrn_tc_sequence_ticket_t ticket;
    tavrn_tc_sequence_ticket_t retained_ticket;
    tavrn_tc_metadata_config_t settings = config();
    unsigned int index;

    REACHED("confirmed-origin-shared-sequence");
    memset(&state, 0, sizeof(state));
    (void)phase5_tc_metadata_init(&state, &settings);
    memset(&action, 0, sizeof(action));
    CHECK("MAINT-05", phase5_tc_prepare_origin(
                              &state, TAVRN_TC_ORIGIN_VERIFICATION_TERMINAL_DEPARTURE,
                              &(tavrn_adva_t){ { 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0xc1u } },
                              NULL, NULL, 10u, &action) == TAVRN_TC_METADATA_PREPARED &&
                          action.valid != 0u && action.event == TAVRN_TC_EVENT_LEAVE);
    CHECK("MAINT-08", phase5_tc_prepare_origin(
                              &state, TAVRN_TC_ORIGIN_DIRECT_TIMEOUT_DEPARTURE,
                              &(tavrn_adva_t){ { 0x34u, 0x56u, 0x78u, 0x9au, 0xbcu, 0xc2u } },
                              NULL, NULL, 11u, &action) == TAVRN_TC_METADATA_PREPARED &&
                          action.event == TAVRN_TC_EVENT_LEAVE);
    CHECK("MAINT-08", phase5_tc_prepare_origin(
                              &state, TAVRN_TC_ORIGIN_DATA_RETRY_EXHAUSTED,
                              &(tavrn_adva_t){ { 0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u } },
                              &(tavrn_adva_t){ { 0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u } },
                              &(tavrn_adva_t){ { 0x55u, 0x66u, 0x77u, 0x88u, 0x99u, 0xc3u } },
                              12u, &action) == TAVRN_TC_METADATA_PREPARED &&
                          memcmp(action.subject.bytes, adva_b, TAVRN_ADVA_LEN) == 0 &&
                          memcmp(action.subject.bytes, adva_e, TAVRN_ADVA_LEN) != 0);
    for (index = 0u; index < sizeof(negative_causes) / sizeof(negative_causes[0]); index++) {
        CHECK("MAINT-08", phase5_tc_prepare_origin(&state, negative_causes[index],
                                                      &settings.local_identity, NULL, NULL,
                                                      20u + index, &action) ==
                              TAVRN_TC_METADATA_IGNORED);
    }

    memset(&ticket, 0, sizeof(ticket));
    CHECK("SERIAL-01", phase5_tc_sequence_prepare(&state, TAVRN_TC_EVENT_JOIN, &ticket) ==
                            TAVRN_TC_METADATA_PREPARED && ticket.valid != 0u &&
                            ticket.sequence == 0xffffu && ticket.event == TAVRN_TC_EVENT_JOIN &&
                            phase5_tc_sequence_commit(&state, &ticket,
                                                      TAVRN_TC_ADMISSION_BUSY, 40u) ==
                                TAVRN_TC_METADATA_RETAINED);
    retained_ticket = ticket;
    CHECK("SERIAL-01", phase5_tc_sequence_prepare(&state, TAVRN_TC_EVENT_JOIN, &ticket) ==
                            TAVRN_TC_METADATA_RETAINED &&
                            memcmp(&ticket, &retained_ticket, sizeof(ticket)) == 0 &&
                            phase5_tc_sequence_commit(&state, &ticket,
                                                      TAVRN_TC_ADMISSION_ADMITTED, 41u) ==
                                TAVRN_TC_METADATA_OK &&
                            phase5_tc_sequence_prepare(&state, TAVRN_TC_EVENT_LEAVE, &ticket) ==
                                TAVRN_TC_METADATA_PREPARED && ticket.sequence == 1u &&
                            phase5_tc_sequence_commit(&state, &ticket,
                                                      TAVRN_TC_ADMISSION_ADMITTED, 42u) ==
                                TAVRN_TC_METADATA_OK);
    CHECK("SERIAL-01", phase5_tc_prepare_origin(
                               &state, TAVRN_TC_ORIGIN_MENTORSHIP_SELF_JOIN,
                               &settings.local_identity, NULL, NULL, 43u, &action) ==
                           TAVRN_TC_METADATA_PREPARED && action.tc_sequence == 2u &&
                           action.event == TAVRN_TC_EVENT_JOIN &&
                           phase5_tc_commit_origin(&state, &action, TAVRN_TC_ADMISSION_BUSY,
                                                   44u) == TAVRN_TC_METADATA_RETAINED);
    retained = action;
    CHECK("SERIAL-01", phase5_tc_prepare_origin(
                               &state, TAVRN_TC_ORIGIN_MENTORSHIP_SELF_JOIN,
                               &settings.local_identity, NULL, NULL, 45u, &action) ==
                           TAVRN_TC_METADATA_RETAINED &&
                           memcmp(&action, &retained, sizeof(action)) == 0 &&
                           phase5_tc_commit_origin(&state, &action,
                                                   TAVRN_TC_ADMISSION_ADMITTED, 46u) ==
                               TAVRN_TC_METADATA_OK &&
                           phase5_tc_metadata_snapshot(&state, &snapshot) ==
                               TAVRN_TC_METADATA_OK && snapshot.next_tc_sequence == 3u);
}

static void test_tc_receive_relay_and_atomicity(void)
{
    tavrn_tc_metadata_state_t state;
    tavrn_tc_metadata_config_t settings = config();
    tavrn_tc_metadata_action_t relay;
    tavrn_tc_metadata_action_t retained;
    tavrn_tc_metadata_snapshot_t before;
    tavrn_tc_metadata_snapshot_t after;
    uint8_t pdu[24];

    REACHED("tc-uuid-subject-ttl-relay");
    memset(&state, 0, sizeof(state));
    (void)phase5_tc_metadata_init(&state, &settings);
    /* Complete validation precedes UUID retention: this UUID has never appeared. */
    (void)phase5_tc_metadata_snapshot(&state, &before);
    make_tc(pdu, adva_e, adva_c, 0x1234u, 15u, 0u, TAVRN_TC_EVENT_JOIN);
    pdu[5] = 1u;
    CHECK("GTT-03", phase5_tc_receive(&state, pdu, sizeof(pdu),
                                        &(tavrn_adva_t){ { 0x34u, 0x56u, 0x78u, 0x9au, 0xbcu, 0xc2u } },
                                        99u, &relay) == TAVRN_TC_METADATA_INVALID &&
                     phase5_tc_metadata_snapshot(&state, &after) == TAVRN_TC_METADATA_OK &&
                     memcmp(&before, &after, sizeof(before)) == 0);

    /* TTL zero applies once but cannot create a relay obligation. */
    make_tc(pdu, adva_b, adva_c, 0xffffu, 0u, 0u, TAVRN_TC_EVENT_JOIN);
    CHECK("GTT-03", phase5_tc_receive(&state, pdu, sizeof(pdu),
                                        &(tavrn_adva_t){ { 0x34u, 0x56u, 0x78u, 0x9au, 0xbcu, 0xc2u } },
                                        100u, &relay) == TAVRN_TC_METADATA_APPLIED &&
                      relay.valid == 0u && phase5_tc_metadata_snapshot(&state, &after) ==
                          TAVRN_TC_METADATA_OK && after.imported_non_direct_count == 1u &&
                      after.uuid_entries == 1u && after.subject_entries == 1u &&
                      after.local_apply_count == 1u && after.relay_count == 0u);

    /* A fresh UUID is retained before subject suppression; same JOIN is silent. */
    make_tc(pdu, adva_b, adva_c, 1u, 1u, 0u, TAVRN_TC_EVENT_JOIN);
    CHECK("MAINT-08", phase5_tc_receive(&state, pdu, sizeof(pdu),
                                           &(tavrn_adva_t){ { 0x34u, 0x56u, 0x78u, 0x9au, 0xbcu, 0xc2u } },
                                           101u, &relay) == TAVRN_TC_METADATA_IGNORED &&
                        relay.valid == 0u && phase5_tc_metadata_snapshot(&state, &after) ==
                            TAVRN_TC_METADATA_OK && after.uuid_entries == 2u &&
                        after.subject_entries == 1u && after.local_apply_count == 1u &&
                        after.relay_count == 0u);

    /* Event mutation under the same UUID stays UUID-deduped, never a new LEAVE. */
    pdu[21] = TAVRN_TC_EVENT_LEAVE;
    CHECK("GTT-04", phase5_tc_receive(&state, pdu, sizeof(pdu),
                                        &(tavrn_adva_t){ { 0x34u, 0x56u, 0x78u, 0x9au, 0xbcu, 0xc2u } },
                                        102u, &relay) == TAVRN_TC_METADATA_IGNORED &&
                     relay.valid == 0u && phase5_tc_metadata_snapshot(&state, &after) ==
                         TAVRN_TC_METADATA_OK && after.uuid_entries == 2u &&
                     after.subject_entries == 1u && after.local_apply_count == 1u);

    /* A distinct UUID with JOIN versus LEAVE is a distinct subject/event key. */
    make_tc(pdu, adva_b, adva_c, 2u, 15u, 0u, TAVRN_TC_EVENT_LEAVE);
    CHECK("MAINT-08", phase5_tc_receive(&state, pdu, sizeof(pdu),
                                           &(tavrn_adva_t){ { 0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u } },
                                           103u, &relay) == TAVRN_TC_METADATA_APPLIED &&
                       relay.ttl == 14u && relay.hops == 1u);
    retained = relay;
    (void)phase5_tc_metadata_snapshot(&state, &before);
    CHECK("MAINT-08", phase5_tc_commit_relay(&state, &relay, TAVRN_TC_ADMISSION_BUSY,
                                               104u) == TAVRN_TC_METADATA_RETAINED &&
                       phase5_tc_receive(&state, pdu, sizeof(pdu),
                                         &(tavrn_adva_t){ { 0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u } },
                                         105u, &relay) == TAVRN_TC_METADATA_IGNORED &&
                       phase5_tc_commit_relay(&state, &retained, TAVRN_TC_ADMISSION_BUSY,
                                               106u) == TAVRN_TC_METADATA_RETAINED &&
                       retained.valid != 0u && retained.tc_sequence == 2u &&
                       retained.ttl == 14u && retained.hops == 1u &&
                        phase5_tc_metadata_snapshot(&state, &after) ==
                            TAVRN_TC_METADATA_OK &&
                        memcmp(&before, &after, sizeof(before)) == 0);

    /* TTL one is relayed as TTL zero for a different non-suppressed subject. */
    make_tc(pdu, adva_b, adva_d, 3u, 1u, 0u, TAVRN_TC_EVENT_JOIN);
    CHECK("GTT-04", phase5_tc_receive(&state, pdu, sizeof(pdu),
                                        &(tavrn_adva_t){ { 0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u } },
                                        107u, &relay) == TAVRN_TC_METADATA_APPLIED &&
                     relay.valid != 0u && relay.ttl == 0u && relay.hops == 1u &&
                     memcmp(&relay.pdu[7], adva_b, TAVRN_ADVA_LEN) == 0 &&
                     memcmp(&relay.pdu[15], adva_d, TAVRN_ADVA_LEN) == 0 &&
                     relay.pdu[21] == TAVRN_TC_EVENT_JOIN && relay.pdu[22] == 0x34u &&
                     relay.pdu[23] == 0x12u);
}

static void test_metadata_pool_selection_and_capacity(void)
{
    tavrn_tc_metadata_state_t state;
    tavrn_tc_metadata_config_t settings = config();
    tavrn_metadata_candidate_t request = candidate(adva_b, TAVRN_METADATA_SOFT_REQUEST, 2u, 1u);
    tavrn_metadata_candidate_t self = candidate(adva_c, TAVRN_METADATA_SUBJECT_SELF_ANSWER, 4u, 0u);
    tavrn_metadata_candidate_t intermediary = candidate(adva_d,
        TAVRN_METADATA_INTERMEDIARY_ANSWER, 5u, 0u);
    tavrn_metadata_candidate_t delayed = candidate(adva_e,
        TAVRN_METADATA_DELAYED_TARGETED_RESERVATION, 0u, 0u);
    tavrn_metadata_candidate_t fifth = candidate(adva_f, TAVRN_METADATA_SOFT_REQUEST, 3u, 1u);
    tavrn_metadata_selection_t selection;
    tavrn_metadata_selection_t maximum = general_selection(4u);
    tavrn_metadata_selection_t illegal_delayed = general_selection(1u);
    tavrn_metadata_attached_control_t attached;
    tavrn_validated_control_t base;
    tavrn_tc_metadata_snapshot_t before;
    tavrn_tc_metadata_snapshot_t after;
    static const tavrn_wire_type_t zero_slot_types[] = {
        TAVRN_WIRE_DATA, TAVRN_WIRE_HELLO, TAVRN_WIRE_E_RREP_ACK,
        TAVRN_WIRE_SYNC_DATA, TAVRN_WIRE_TC_UPDATE,
    };
    unsigned int index;

    REACHED("shared-four-slot-priority-cooldown");
    memset(&state, 0, sizeof(state));
    (void)phase5_tc_metadata_init(&state, &settings);
    CHECK("BUILD-01", phase5_metadata_create(&state, &request, 200u) == TAVRN_TC_METADATA_OK &&
                          phase5_metadata_create(&state, &self, 200u) == TAVRN_TC_METADATA_OK &&
                          phase5_metadata_create(&state, &intermediary, 200u) ==
                              TAVRN_TC_METADATA_OK &&
                          phase5_metadata_create(&state, &delayed, 200u) ==
                              TAVRN_TC_METADATA_OK &&
                           phase5_tc_metadata_snapshot(&state, &after) == TAVRN_TC_METADATA_OK &&
                           after.candidate_count == 4u &&
                           phase5_metadata_create(&state, &request, 201u) ==
                               TAVRN_TC_METADATA_OK &&
                           phase5_tc_metadata_snapshot(&state, &after) == TAVRN_TC_METADATA_OK &&
                           after.candidate_count == 4u &&
                           phase5_metadata_create(&state, &fifth, 201u) ==
                               TAVRN_TC_METADATA_BUSY &&
                           phase5_tc_metadata_snapshot(&state, &after) == TAVRN_TC_METADATA_OK &&
                           after.candidate_count == 4u);
    CHECK("META-01", phase5_metadata_select(&state, TAVRN_METADATA_FRAME_RREQ8, 0u,
                                                202u, &selection) == TAVRN_TC_METADATA_OK &&
                        selection.valid != 0u && selection.count == 3u &&
                        selection.entries[0].kind == TAVRN_METADATA_SOFT_REQUEST &&
                        selection.entries[1].kind == TAVRN_METADATA_SUBJECT_SELF_ANSWER &&
                        selection.entries[2].kind == TAVRN_METADATA_INTERMEDIARY_ANSWER &&
                        selection.entries[0].kind !=
                            TAVRN_METADATA_DELAYED_TARGETED_RESERVATION &&
                        selection.entries[1].kind !=
                            TAVRN_METADATA_DELAYED_TARGETED_RESERVATION &&
                        selection.entries[2].kind !=
                            TAVRN_METADATA_DELAYED_TARGETED_RESERVATION);
    (void)phase5_tc_metadata_snapshot(&state, &before);
    CHECK("META-01", phase5_metadata_select(&state, TAVRN_METADATA_FRAME_RREQ8, 0u,
                                               203u, &selection) == TAVRN_TC_METADATA_OK &&
                       phase5_tc_metadata_snapshot(&state, &after) == TAVRN_TC_METADATA_OK &&
                       memcmp(&before, &after, sizeof(before)) == 0 &&
                       phase5_metadata_commit(&state, &selection, TAVRN_TC_ADMISSION_BUSY,
                                              204u) == TAVRN_TC_METADATA_RETAINED &&
                       phase5_tc_metadata_snapshot(&state, &after) == TAVRN_TC_METADATA_OK &&
                       memcmp(&before, &after, sizeof(before)) == 0 &&
                       phase5_metadata_commit(&state, &selection,
                                              TAVRN_TC_ADMISSION_ZERO_CHANNEL_FAILED,
                                              204u) == TAVRN_TC_METADATA_RETAINED &&
                       phase5_metadata_commit(&state, &selection,
                                              TAVRN_TC_ADMISSION_LOCAL_NOT_ATTEMPTED,
                                              204u) == TAVRN_TC_METADATA_RETAINED &&
                       phase5_tc_metadata_snapshot(&state, &after) == TAVRN_TC_METADATA_OK &&
                       memcmp(&before, &after, sizeof(before)) == 0);
    CHECK("META-01", phase5_metadata_commit(&state, &selection,
                                               TAVRN_TC_ADMISSION_ADMITTED, 205u) ==
                            TAVRN_TC_METADATA_OK &&
                        phase5_tc_metadata_snapshot(&state, &after) == TAVRN_TC_METADATA_OK &&
                        after.candidate_count == 1u && after.cooldown_count == 3u &&
                        after.round_robin_cursor != before.round_robin_cursor);

    REACHED("metadata-frame-budgets-no-truncation");
    illegal_delayed.entries[0] = delayed;
    base = route_control(TAVRN_METADATA_FRAME_RREQ8, 0u);
    CHECK("META-01", phase5_metadata_attach_control(&base, &illegal_delayed, &attached) ==
                           TAVRN_TC_METADATA_INVALID);
    base = route_control(TAVRN_METADATA_FRAME_RREQ8, 0u);
    CHECK("META-02", phase5_metadata_attach_control(&base, &maximum, &attached) ==
                           TAVRN_TC_METADATA_OK && attached.control.pdu_len == 24u &&
                        (attached.control.pdu[5] & 0x08u) != 0u && attached.emitted.count == 4u);
    base = route_control(TAVRN_METADATA_FRAME_RREP8, 0u);
    CHECK("META-02", phase5_metadata_attach_control(&base, &maximum, &attached) ==
                           TAVRN_TC_METADATA_OK && attached.control.pdu_len == 23u &&
                        attached.emitted.count == 3u);
    base = route_control(TAVRN_METADATA_FRAME_RERR8, 1u);
    CHECK("META-02", phase5_metadata_attach_control(&base, &maximum, &attached) ==
                           TAVRN_TC_METADATA_OK && attached.control.pdu_len == 23u &&
                        attached.emitted.count == 4u);
    base = route_control(TAVRN_METADATA_FRAME_RERR8, 2u);
    CHECK("META-02", phase5_metadata_attach_control(&base, &maximum, &attached) ==
                           TAVRN_TC_METADATA_OK && attached.control.pdu_len == 24u &&
                        attached.emitted.count == 3u);
    base = route_control(TAVRN_METADATA_FRAME_RERR8, 3u);
    CHECK("META-02", phase5_metadata_attach_control(&base, &maximum, &attached) ==
                           TAVRN_TC_METADATA_OK && attached.control.pdu_len == 23u &&
                        attached.emitted.count == 1u);
    base = route_control(TAVRN_METADATA_FRAME_RERR8, 4u);
    CHECK("META-02", phase5_metadata_attach_control(&base, &maximum, &attached) ==
                           TAVRN_TC_METADATA_OK && attached.control.pdu_len == 23u &&
                        (attached.control.pdu[5] & 0x08u) == 0u && attached.emitted.count == 0u);
    base = route_control(TAVRN_METADATA_FRAME_RREQ16, 0u);
    CHECK("META-03", phase5_metadata_attach_control(&base, &maximum, &attached) ==
                           TAVRN_TC_METADATA_INVALID);
    base = route_control(TAVRN_METADATA_FRAME_RREP_ACK, 0u);
    CHECK("META-03", phase5_metadata_attach_control(&base, &maximum, &attached) ==
                           TAVRN_TC_METADATA_INVALID);
    for (index = 0u; index < sizeof(zero_slot_types) / sizeof(zero_slot_types[0]); index++) {
        base = route_control(TAVRN_METADATA_FRAME_RREP_ACK, 0u);
        base.type = zero_slot_types[index];
        base.pdu[4] = (uint8_t)base.type;
        CHECK("META-03", phase5_metadata_attach_control(&base, &maximum, &attached) ==
                               TAVRN_TC_METADATA_INVALID);
    }
}

#if !defined(TAVRN_PHASE5_TC_METADATA_RED_MODE)
static void test_metadata_cooldown_semantics(void)
{
    tavrn_tc_metadata_state_t state;
    tavrn_tc_metadata_config_t settings = config();
    tavrn_metadata_candidate_t request = candidate(
        adva_b, TAVRN_METADATA_SOFT_REQUEST, 2u, 1u);
    tavrn_metadata_candidate_t changed = request;
    tavrn_metadata_selection_t selection;
    tavrn_tc_metadata_snapshot_t snapshot;

    REACHED("metadata-cooldown-equality-change");
    memset(&state, 0, sizeof(state));
    (void)phase5_tc_metadata_init(&state, &settings);
    CHECK("META-01", phase5_metadata_create(&state, &request, 1u) ==
                           TAVRN_TC_METADATA_OK &&
                           phase5_metadata_select(&state, TAVRN_METADATA_FRAME_RREQ8,
                                                  0u, 2u, &selection) ==
                               TAVRN_TC_METADATA_OK && selection.count == 1u &&
                           phase5_metadata_commit(&state, &selection,
                                                  TAVRN_TC_ADMISSION_ADMITTED, 10u) ==
                               TAVRN_TC_METADATA_OK &&
                           phase5_metadata_create(&state, &request, 5009u) ==
                               TAVRN_TC_METADATA_OK &&
                           phase5_tc_metadata_snapshot(&state, &snapshot) ==
                               TAVRN_TC_METADATA_OK && snapshot.candidate_count == 0u &&
                           snapshot.cooldown_count == 1u &&
                           phase5_metadata_create(&state, &request, 5010u) ==
                               TAVRN_TC_METADATA_OK &&
                           phase5_tc_metadata_snapshot(&state, &snapshot) ==
                               TAVRN_TC_METADATA_OK && snapshot.candidate_count == 1u &&
                           snapshot.cooldown_count == 0u &&
                           phase5_metadata_select(&state, TAVRN_METADATA_FRAME_RREQ8,
                                                  0u, 5011u, &selection) ==
                               TAVRN_TC_METADATA_OK && selection.count == 1u &&
                           phase5_metadata_commit(&state, &selection,
                                                  TAVRN_TC_ADMISSION_ADMITTED, 5012u) ==
                               TAVRN_TC_METADATA_OK);
    changed.ttl_bucket = 3u;
    CHECK("META-01", phase5_metadata_create(&state, &changed, 5013u) ==
                           TAVRN_TC_METADATA_OK &&
                           phase5_tc_metadata_snapshot(&state, &snapshot) ==
                               TAVRN_TC_METADATA_OK && snapshot.candidate_count == 1u &&
                           snapshot.cooldown_count == 0u);
}
#endif

static void test_metadata_attribution_merge_and_atomic_reject(void)
{
    tavrn_tc_metadata_state_t state;
    tavrn_tc_metadata_config_t settings = config();
    tavrn_metadata_rx_t rx;
    tavrn_metadata_selection_t one = general_selection(1u);
    tavrn_metadata_attached_control_t attached;
    tavrn_validated_control_t base;
    tavrn_rx_control_event_t control_event;
    tavrn_tc_metadata_snapshot_t before;
    tavrn_tc_metadata_snapshot_t after;

    REACHED("metadata-attribution-atomic-merge");
    memset(&state, 0, sizeof(state));
    (void)phase5_tc_metadata_init(&state, &settings);
    base = route_control(TAVRN_METADATA_FRAME_RREQ8, 0u);
    memset(&control_event, 0, sizeof(control_event));
    CHECK("META-04", phase5_metadata_attach_control(&base, &one, &attached) ==
                           TAVRN_TC_METADATA_OK && attached.control.pdu_len == 18u &&
                       attached.emitted.count == 1u &&
                       control_round_trip(&attached.control, adva_b, &control_event) &&
                       phase5_metadata_receive_control(&state, &control_event, 299u) ==
                           TAVRN_TC_METADATA_APPLIED &&
                       phase5_tc_metadata_snapshot(&state, &after) ==
                           TAVRN_TC_METADATA_OK && after.imported_non_direct_count == 1u);
    memset(&rx, 0, sizeof(rx));
    rx.frame_kind = TAVRN_METADATA_FRAME_RREQ8;
    rx.outer_transmitter = adva(adva_b);
    rx.sid8 = 1u;
    rx.metadata_flag = 1u;
    rx.count = 1u;
    rx.entries[0] = candidate(adva_c, TAVRN_METADATA_SUBJECT_SELF_ANSWER, 3u, 0u);
    CHECK("META-04", phase5_metadata_create(&state,
                                              &(tavrn_metadata_candidate_t){
                                                  .subject = { { 0x11u, 0x22u, 0x33u,
                                                                 0x44u, 0x55u, 0xc1u } },
                                                  .subject_sid8 = 0x11u,
                                                  .ttl_bucket = 2u,
                                                  .freshness_request = 1u,
                                                  .kind = TAVRN_METADATA_SOFT_REQUEST,
                                               }, 300u) == TAVRN_TC_METADATA_OK);
    CHECK("META-04", phase5_metadata_receive(&state, &rx, 301u) == TAVRN_TC_METADATA_APPLIED &&
                       phase5_tc_metadata_snapshot(&state, &after) == TAVRN_TC_METADATA_OK &&
                       after.imported_non_direct_count == 1u && after.candidate_count == 0u);
    rx.entries[0] = candidate(adva_d, TAVRN_METADATA_SOFT_REQUEST, 2u, 1u);
    CHECK("META-01", phase5_metadata_receive(&state, &rx, 302u) == TAVRN_TC_METADATA_APPLIED &&
                       phase5_tc_metadata_snapshot(&state, &after) == TAVRN_TC_METADATA_OK &&
                       after.candidate_count == 1u);
    (void)phase5_tc_metadata_snapshot(&state, &before);
    rx.entries[0].subject_sid8 = 0xffu;
    CHECK("META-04", phase5_metadata_receive(&state, &rx, 301u) == TAVRN_TC_METADATA_INVALID &&
                       phase5_tc_metadata_snapshot(&state, &after) == TAVRN_TC_METADATA_OK &&
                       memcmp(&before, &after, sizeof(before)) == 0);
    rx.entries[0] = candidate(adva_d, TAVRN_METADATA_SOFT_REQUEST, 2u, 1u);
    rx.count = 5u;
    CHECK("META-04", phase5_metadata_receive(&state, &rx, 302u) == TAVRN_TC_METADATA_INVALID &&
                       phase5_tc_metadata_snapshot(&state, &after) == TAVRN_TC_METADATA_OK &&
                       memcmp(&before, &after, sizeof(before)) == 0);
    rx.count = 1u;
    rx.metadata_flag = 0u;
    CHECK("META-04", phase5_metadata_receive(&state, &rx, 303u) == TAVRN_TC_METADATA_INVALID &&
                       phase5_tc_metadata_snapshot(&state, &after) == TAVRN_TC_METADATA_OK &&
                       memcmp(&before, &after, sizeof(before)) == 0);
    rx.metadata_flag = 1u;
    rx.entries[0] = candidate(adva_d, TAVRN_METADATA_SOFT_REQUEST, 2u, 1u);
    rx.stale = 1u;
    CHECK("META-04", phase5_metadata_receive(&state, &rx, 304u) == TAVRN_TC_METADATA_INVALID &&
                       phase5_tc_metadata_snapshot(&state, &after) == TAVRN_TC_METADATA_OK &&
                       memcmp(&before, &after, sizeof(before)) == 0);
}

#if defined(TAVRN_PHASE5_TC_METADATA_CORRECTIVE_MODE)

typedef struct corrective_fixture {
    ble_mesh_scheduler_t scheduler;
    tavrn_link_v2_t link;
    aodv_core_t aodv;
    tavrn_gtt_t gtt;
    tavrn_gtt_storage_t gtt_storage;
    tavrn_full_t full;
    tavrn_router_t router;
    tavrn_mentorship_t mentorship;
    tavrn_maintenance_t maintenance;
} corrective_fixture_t;

static tavrn_direct_peer_t corrective_peer(const uint8_t bytes[TAVRN_ADVA_LEN],
                                           tavrn_identity_width_t width)
{
    tavrn_direct_peer_t value;

    memset(&value, 0, sizeof(value));
    value.logical_id.width = width;
    value.logical_id.value = width == TAVRN_IDENTITY_SID8 ? bytes[0] :
        (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
    value.adva = adva(bytes);
    return value;
}

static tavrn_link_config_t corrective_link_config(void)
{
    tavrn_link_config_t value;

    memset(&value, 0, sizeof(value));
    value.local_peer = corrective_peer(adva_a, TAVRN_IDENTITY_SID16);
    value.network_id = NETWORK_ID;
    value.hack_max_attempts = 3u;
    value.busy_max_responses = 3u;
    value.hack_response_ms = 250u;
    value.hack_turnaround_ms = 8u;
    value.busy_backoff_ms = 500u;
    value.data_forward_deadline_ms = 5000u;
    value.candidate_resolve_ms = 10u;
    value.data_dedupe_ms = 10000u;
    value.flood_dedupe_ms = 10000u;
    value.flood_jitter_min_ms = 20u;
    value.flood_jitter_max_ms = 120u;
    return value;
}

static aodv_core_config_t corrective_aodv_config(void)
{
    aodv_core_config_t value;

    memset(&value, 0, sizeof(value));
    value.local_peer = corrective_peer(adva_a, TAVRN_IDENTITY_SID16);
    value.network_id = NETWORK_ID;
    value.net_diameter = 15u;
    value.rreq_retries = 2u;
    value.rreq_rate_per_second = 10u;
    value.rerr_rate_per_second = 10u;
    value.request_rrep_ack = 1u;
    value.initial_origin_sequence = 1u;
    value.initial_request_id = 1u;
    value.initial_rerr_sequence = 1u;
    value.node_traversal_ms = 10u;
    value.path_discovery_ms = 60u;
    value.rreq_seen_ms = 10000u;
    value.active_route_ms = 36000u;
    value.pending_data_ms = 5000u;
    value.blacklist_ms = 600u;
    value.rrep_dedupe_ms = 10000u;
    value.rerr_dedupe_ms = 10000u;
    value.rrep_ack_wait_ms = 250u;
    return value;
}

static int corrective_observe(corrective_fixture_t *fixture,
                              const uint8_t identity[TAVRN_ADVA_LEN],
                              uint16_t serial, uint8_t serial_present,
                              tavrn_gtt_provenance_t provenance, uint32_t now_ms)
{
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_expiry_observe_status_t status;

    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = adva(identity);
    evidence.serial = serial;
    evidence.serial_present = serial_present;
    evidence.hop_count = 1u;
    evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    status = tavrn_gtt_observe_with_provenance(&fixture->gtt, &evidence,
                                                provenance, now_ms, NULL);
    return status == TAVRN_GTT_EXPIRY_OBSERVE_ADDED ||
        status == TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED;
}

static void corrective_clear_queue(corrective_fixture_t *fixture)
{
    uint8_t index;

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        if (fixture->scheduler.routed_tx_queue.entries[index].occupied != 0u) {
            (void)ble_mesh_tx_queue_remove(&fixture->scheduler.routed_tx_queue,
                                           index, NULL);
        }
    }
}

static int corrective_queue_contains_control(const corrective_fixture_t *fixture,
                                             const tavrn_validated_control_t *control)
{
    uint8_t index;

    if (fixture == NULL || control == NULL) return 0;
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry =
            &fixture->scheduler.routed_tx_queue.entries[index];

        if (entry->occupied != 0u &&
            entry->item.adv_len == (uint8_t)(control->pdu_len + 7u) &&
            memcmp(&entry->item.adv_data[7], control->pdu, control->pdu_len) == 0) {
            return 1;
        }
    }
    return 0;
}

static int corrective_seed_evictable_transit(corrective_fixture_t *fixture,
                                              tavrn_link_data_t *data_out)
{
    tavrn_link_data_t data;
    ble_mesh_tx_item_t item;
    uint8_t index;

    if (fixture == NULL || data_out == NULL) return 0;
    memset(&data, 0, sizeof(data));
    data.origin.width = TAVRN_IDENTITY_SID8;
    data.origin.value = adva_d[0];
    data.final_destination.width = TAVRN_IDENTITY_SID8;
    data.final_destination.value = adva_c[0];
    data.data_seq = 0x4a01u;
    data.ttl = 4u;
    data.hops = 1u;
    data.app_kind = 0x7eu;
    data.app_len = 1u;
    data.app_bytes[0] = 0x5au;
    data.ownership = TAVRN_DATA_TRANSIT;
    memset(&fixture->link.custody[0], 0, sizeof(fixture->link.custody[0]));
    fixture->link.custody[0].phase = TAVRN_CUSTODY_ACTIVE_QUEUED;
    fixture->link.custody[0].next_hop = corrective_peer(adva_b, TAVRN_IDENTITY_SID8);
    fixture->link.custody[0].data = data;
    fixture->link.custody[0].scheduler_token = 0x31u;
    fixture->link.custody[0].transaction_deadline_ms = 1000u;
    fixture->link.active_custody_index = 0u;
    memset(&fixture->link.data_dedupe[0], 0, sizeof(fixture->link.data_dedupe[0]));
    fixture->link.data_dedupe[0].valid = 1u;
    fixture->link.data_dedupe[0].custody_pinned = 1u;
    fixture->link.data_dedupe[0].origin = data.origin;
    fixture->link.data_dedupe[0].final_destination = data.final_destination;
    fixture->link.data_dedupe[0].data_seq = data.data_seq;
    fixture->link.data_dedupe[0].app_kind = data.app_kind;
    fixture->link.data_dedupe[0].app_source = data.app_source;
    fixture->link.data_dedupe[0].expires_at_ms = 1000u;
    corrective_clear_queue(fixture);
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_enqueue_result_t enqueue;

        memset(&item, 0, sizeof(item));
        item.adv_len = 1u;
        item.adv_data[0] = index;
        item.channel_mask = BLE_RADIO_ADV_CH_ALL;
        item.priority = BLE_MESH_TX_PRIORITY_DATA;
        item.service_class = index == 0u ? BLE_MESH_TX_SERVICE_CUSTODY_DATA :
                                          BLE_MESH_TX_SERVICE_BEST_EFFORT;
        item.token = index == 0u ? 0x31u : BLE_MESH_TX_TOKEN_NONE;
        enqueue = ble_mesh_tx_queue_enqueue(&fixture->scheduler.routed_tx_queue, &item);
        if (enqueue.status != BLE_MESH_TX_ENQUEUE_OK) return 0;
    }
    *data_out = data;
    return 1;
}

static int corrective_set_hard_deadline(corrective_fixture_t *fixture,
                                        const uint8_t identity[TAVRN_ADVA_LEN],
                                        uint32_t deadline_ms)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        tavrn_gtt_entry_t *entry = &fixture->gtt_storage.entries[index];

        if (entry->occupied != 0u &&
            memcmp(entry->identity.bytes, identity, TAVRN_ADVA_LEN) == 0) {
            entry->hard_deadline_ms = deadline_ms;
            return 1;
        }
    }
    return 0;
}

static int corrective_fixture_init_with_join_binding(corrective_fixture_t *fixture,
                                                       uint8_t bind_before_activation,
                                                       uint8_t install_binding)
{
    tavrn_gtt_config_t gtt_config;
    tavrn_router_incarnation_config_t incarnation;
    tavrn_mentorship_config_t mentorship_config;
    tavrn_maintenance_config_t maintenance_config;
    tavrn_router_augmentation_hooks_t hooks;
    tavrn_mentorship_state_snapshot_t mentorship_state;
    tavrn_link_config_t link_config = corrective_link_config();
    aodv_core_config_t aodv_config = corrective_aodv_config();

    memset(fixture, 0, sizeof(*fixture));
    memset(&gtt_config, 0, sizeof(gtt_config));
    gtt_config.local_identity = adva(adva_a);
    gtt_config.soft_expiry_ms = 10u;
    gtt_config.hard_expiry_ms = 300000u;
    gtt_config.departed_retention_ms = 600000u;
    memset(&incarnation, 0, sizeof(incarnation));
    incarnation.feature_level = TAVRN_ROUTER_FEATURE_FULL_TAVRN;
    incarnation.boot_nonce = 0x5a01u;
    incarnation.reboot_announce_ms = 30u;
    memset(&mentorship_config, 0, sizeof(mentorship_config));
    mentorship_config.offer_window_ms = 10u;
    mentorship_config.page_timeout_ms = 30u;
    mentorship_config.self_bootstrap_ms = 20u;
    mentorship_config.offer_suppression_ms = 100u;
    mentorship_config.join_dedupe_ms = 100u;
    mentorship_config.sync_dedupe_ms = 100u;
    mentorship_config.rssi_weak_magnitude_db = 90u;
    mentorship_config.rssi_strong_magnitude_db = 30u;
    mentorship_config.rssi_weak_delay_ms = 500u;
    mentorship_config.rssi_strong_delay_ms = 10u;
    mentorship_config.jitter_max_ms = 50u;
    mentorship_config.page_attempts = 3u;
    memset(&maintenance_config, 0, sizeof(maintenance_config));
    maintenance_config.hello_change_ms = 10u;
    maintenance_config.hello_stable_ms = 20u;
    maintenance_config.hello_alpha_permille = 800u;
    maintenance_config.hello_snap_permille = 950u;
    maintenance_config.topology_sample_ms = 1u;
    maintenance_config.hello_dedupe_ms = 100u;
    maintenance_config.initial_node_sequence = 1u;
    maintenance_config.aodv_net_traversal_ms = 30u;
    maintenance_config.freshness_response_min_ms = 10u;
    maintenance_config.freshness_response_max_ms = 100u;
    maintenance_config.metadata_cooldown_ms = 5u;
    maintenance_config.tc_uuid_ms = 30u;
    maintenance_config.tc_subject_ms = 5u;
    ble_mesh_scheduler_init(&fixture->scheduler, 0u, adva_a);
    if (tavrn_link_v2_init(&fixture->link, &fixture->scheduler, &link_config, 0u) !=
            TAVRN_LINK_INIT_OK ||
        aodv_core_init(&fixture->aodv, &aodv_config, 0u) != AODV_INIT_OK ||
        tavrn_gtt_init(&fixture->gtt, &fixture->gtt_storage, &gtt_config, 0u) !=
            TAVRN_GTT_INIT_OK ||
        tavrn_full_init(&fixture->full, &fixture->gtt) != TAVRN_FULL_INIT_OK) {
        return 0;
    }
    hooks = tavrn_full_router_hooks(&fixture->full);
    if (tavrn_router_init_with_incarnation(&fixture->router, &fixture->link,
                                            &fixture->aodv, &hooks, &incarnation, 0u) !=
            TAVRN_ROUTER_INCARNATION_OK ||
        tavrn_mentorship_init(&fixture->mentorship, &fixture->router, &fixture->gtt,
                              &mentorship_config, 0u) != TAVRN_MENTORSHIP_OK ||
        tavrn_maintenance_init(&fixture->maintenance, &fixture->router, &fixture->gtt,
                               &maintenance_config) != TAVRN_MAINTENANCE_OK ||
        !corrective_observe(fixture, adva_b, 1u, 1u,
                            TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 1u)) {
        return 0;
    }
    if (install_binding != 0u && bind_before_activation != 0u &&
        tavrn_full_maintenance_binding_install(&fixture->router, &fixture->mentorship,
                                               &fixture->maintenance) !=
            TAVRN_FULL_MAINTENANCE_BINDING_OK) {
        return 0;
    }
    if (tavrn_mentorship_tick(&fixture->mentorship, 20u) !=
            TAVRN_MENTORSHIP_SELF_BOOTSTRAPPED ||
        tavrn_mentorship_state_snapshot(&fixture->mentorship, &mentorship_state) !=
            TAVRN_MENTORSHIP_OK ||
        mentorship_state.state != TAVRN_MENTORSHIP_SID8_ACTIVE ||
        tavrn_maintenance_activate(&fixture->maintenance, 20u) != TAVRN_MAINTENANCE_OK) {
        return 0;
    }
    if (install_binding != 0u && bind_before_activation == 0u &&
        tavrn_full_maintenance_binding_install(&fixture->router, &fixture->mentorship,
                                               &fixture->maintenance) !=
            TAVRN_FULL_MAINTENANCE_BINDING_OK) {
        return 0;
    }
    corrective_clear_queue(fixture);
    if (bind_before_activation == 0u) {
        memset(fixture->mentorship.join_obligations, 0,
               sizeof(fixture->mentorship.join_obligations));
    }
    return 1;
}

static int corrective_fixture_init(corrective_fixture_t *fixture)
{
    return corrective_fixture_init_with_join_binding(fixture, 0u, 1u);
}

static int corrective_fixture_init_unbound(corrective_fixture_t *fixture)
{
    return corrective_fixture_init_with_join_binding(fixture, 0u, 0u);
}

static int corrective_dispatch_decorated_control(
    tavrn_metadata_frame_kind_t kind, uint8_t unreachable_count,
    uint8_t metadata_count, uint8_t extra_flags, aodv_action_type_t action_type,
    uint8_t controlled_flood)
{
    corrective_fixture_t fixture;
    tavrn_metadata_candidate_t local_answer = candidate(
        adva_a, TAVRN_METADATA_SUBJECT_SELF_ANSWER, 15u, 0u);
    tavrn_metadata_selection_t selection = general_selection(metadata_count);
    tavrn_metadata_attached_control_t attached;
    tavrn_tc_metadata_snapshot_t before;
    tavrn_tc_metadata_snapshot_t after;
    tavrn_tc_metadata_telemetry_t telemetry_before;
    tavrn_tc_metadata_telemetry_t telemetry_after;
    tavrn_router_phase_trace_t trace;
    tavrn_validated_control_t base = route_control(kind, unreachable_count);
    aodv_action_t action;
    tavrn_router_event_status_t dispatch_status;
    tavrn_tc_metadata_status_t snapshot_status;
    tavrn_tc_metadata_status_t telemetry_status;
    int queued;

    base.pdu[5] |= extra_flags;
    if (kind == TAVRN_METADATA_FRAME_RREQ8 && (extra_flags & 0x10u) != 0u) {
        base.pdu[11] = 0u;
    }
    if (kind == TAVRN_METADATA_FRAME_RREP8) {
        base.pdu[7] = adva_b[0];
    }
    if (!corrective_fixture_init(&fixture)) {
        printf("META-07 diagnostic kind=%u setup=fixture\n", (unsigned int)kind);
        return 0;
    }
    if (tavrn_maintenance_metadata_attach_control(&base, &selection, &attached) !=
            TAVRN_TC_METADATA_OK) {
        printf("META-07 diagnostic kind=%u setup=attach count=%u flags=%u\n",
               (unsigned int)kind, (unsigned int)metadata_count,
               (unsigned int)base.pdu[5]);
        return 0;
    }
    if (tavrn_maintenance_metadata_create(&fixture.maintenance.tc_metadata,
                                          &local_answer, 40u) !=
            TAVRN_TC_METADATA_OK ||
        tavrn_maintenance_tc_metadata_snapshot(&fixture.maintenance.tc_metadata,
                                                &before) !=
            TAVRN_TC_METADATA_OK ||
        before.candidate_count == 0u ||
        tavrn_maintenance_tc_metadata_telemetry(&fixture.maintenance,
                                                &telemetry_before) !=
            TAVRN_TC_METADATA_OK) {
        printf("META-07 diagnostic kind=%u setup=state\n", (unsigned int)kind);
        return 0;
    }
    memset(&action, 0, sizeof(action));
    action.type = action_type;
    action.detail.control.control = attached.control;
    action.detail.control.controlled_flood = controlled_flood;
    if (controlled_flood == 0u) {
        action.detail.control.next_hop = corrective_peer(adva_b, TAVRN_IDENTITY_SID8);
    }
    fixture.router.retained_action = action;
    fixture.router.retained_action_valid = 1u;
    memset(&trace, 0, sizeof(trace));
    dispatch_status = tavrn_router_dispatch_trace_ex(&fixture.router, 41u, &trace);
    snapshot_status = tavrn_maintenance_tc_metadata_snapshot(
        &fixture.maintenance.tc_metadata, &after);
    telemetry_status = tavrn_maintenance_tc_metadata_telemetry(
        &fixture.maintenance, &telemetry_after);
    queued = corrective_queue_contains_control(&fixture, &attached.control);
    if (dispatch_status != TAVRN_ROUTER_EVENT_OK ||
        trace.phase != TAVRN_ROUTER_TRACE_DISPATCH ||
        trace.terminal_fault_present != TAVRN_ROUTER_TRACE_NOT_PRESENT ||
        trace.detail.dispatch.status != TAVRN_ROUTER_EVENT_OK ||
        trace.detail.dispatch.action_present != TAVRN_ROUTER_TRACE_PRESENT ||
        trace.detail.dispatch.action.type != action_type ||
        trace.detail.dispatch.link_send_present != TAVRN_ROUTER_TRACE_PRESENT ||
        trace.detail.dispatch.link_send_status != TAVRN_LINK_SEND_OK ||
        fixture.router.retained_action_valid != 0u ||
        fixture.maintenance.metadata_pending.valid != 0u ||
        !queued || snapshot_status != TAVRN_TC_METADATA_OK ||
        memcmp(&before, &after, sizeof(before)) != 0 ||
        telemetry_status != TAVRN_TC_METADATA_OK ||
        memcmp(&telemetry_before, &telemetry_after, sizeof(telemetry_before)) != 0) {
        printf("META-07 diagnostic kind=%u dispatch=%u phase=%u terminal=%u "
               "status=%u action=%u link_present=%u link_status=%u retained=%u "
               "pending=%u queued=%u snapshot=%u telemetry=%u\n",
               (unsigned int)kind, (unsigned int)dispatch_status,
               (unsigned int)trace.phase,
               (unsigned int)trace.terminal_fault_present,
               (unsigned int)trace.detail.dispatch.status,
               (unsigned int)trace.detail.dispatch.action.type,
               (unsigned int)trace.detail.dispatch.link_send_present,
               (unsigned int)trace.detail.dispatch.link_send_status,
               (unsigned int)fixture.router.retained_action_valid,
               (unsigned int)fixture.maintenance.metadata_pending.valid,
               (unsigned int)queued, (unsigned int)snapshot_status,
               (unsigned int)telemetry_status);
        return 0;
    }
    return 1;
}

static int corrective_gtt_snapshot_for(const corrective_fixture_t *fixture,
                                       const uint8_t identity[TAVRN_ADVA_LEN],
                                       uint32_t now_ms,
                                       tavrn_gtt_expiry_snapshot_t *snapshot_out)
{
    tavrn_adva_t subject = adva(identity);

    return tavrn_gtt_expiry_snapshot(&fixture->gtt, &subject, now_ms, snapshot_out) ==
        TAVRN_GTT_EXPIRY_QUERY_FOUND;
}

static int corrective_soft_request_exists(const tavrn_tc_metadata_state_t *state,
                                          const uint8_t subject[TAVRN_ADVA_LEN])
{
    uint8_t index;

    for (index = 0u; index < TAVRN_TC_METADATA_CANDIDATE_CAPACITY; index++) {
        const tavrn_tc_metadata_slot_t *slot = &state->slots[index];

        if (slot->valid != 0u && slot->candidate.kind == TAVRN_METADATA_SOFT_REQUEST &&
            memcmp(slot->candidate.subject.bytes, subject, TAVRN_ADVA_LEN) == 0) {
            return 1;
        }
    }
    return 0;
}

static int corrective_wrap_tc_event(const uint8_t pdu[24],
                                     const uint8_t outer[TAVRN_ADVA_LEN],
                                     ble_mesh_sched_event_t *event_out)
{
    if (pdu == NULL || outer == NULL || event_out == NULL) return 0;
    memset(event_out, 0, sizeof(*event_out));
    event_out->type = BLE_MESH_SCHED_EVENT_RX_ADV;
    memcpy(event_out->adv_addr, outer, TAVRN_ADVA_LEN);
    event_out->adv_len = 31u;
    event_out->adv_data[0] = 0x02u;
    event_out->adv_data[1] = 0x01u;
    event_out->adv_data[2] = 0x06u;
    event_out->adv_data[3] = 0x1bu;
    event_out->adv_data[4] = 0xffu;
    event_out->adv_data[5] = 0xffu;
    event_out->adv_data[6] = 0xffu;
    memcpy(&event_out->adv_data[7], pdu, 24u);
    return 1;
}

static int corrective_wrap_control_event(const tavrn_validated_control_t *control,
                                         const uint8_t outer[TAVRN_ADVA_LEN],
                                         ble_mesh_sched_event_t *event_out)
{
    tavrn_codec_config_t codec;
    tavrn_decoded_frame_t frame;
    size_t adv_len = 0u;

    if (control == NULL || outer == NULL || event_out == NULL) return 0;
    memset(&codec, 0, sizeof(codec));
    codec.network_id = NETWORK_ID;
    codec.local_peer = corrective_peer(adva_a, TAVRN_IDENTITY_SID8);
    memset(&frame, 0, sizeof(frame));
    frame.type = control->type;
    frame.network_id = NETWORK_ID;
    frame.transmitter = corrective_peer(outer, TAVRN_IDENTITY_SID8);
    frame.detail.control = *control;
    memset(event_out, 0, sizeof(*event_out));
    event_out->type = BLE_MESH_SCHED_EVENT_RX_ADV;
    memcpy(event_out->adv_addr, outer, TAVRN_ADVA_LEN);
    return tavrn_wire_v2_encode(&codec, &frame, event_out->adv_data,
                                sizeof(event_out->adv_data), &adv_len) == TAVRN_CODEC_OK &&
        adv_len <= UINT8_MAX && (event_out->adv_len = (uint8_t)adv_len, 1);
}

static int corrective_fill_control_queue(corrective_fixture_t *fixture,
                                         uint32_t now_ms)
{
    uint8_t index;

    (void)now_ms;
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        ble_mesh_tx_item_t item;
        ble_mesh_tx_enqueue_result_t enqueue;

        memset(&item, 0, sizeof(item));
        item.adv_len = 1u;
        item.channel_mask = 1u;
        item.priority = BLE_MESH_TX_PRIORITY_CONTROL;
        item.service_class = BLE_MESH_TX_SERVICE_BEST_EFFORT;
        enqueue = ble_mesh_tx_queue_enqueue(&fixture->scheduler.routed_tx_queue, &item);
        if (enqueue.status != BLE_MESH_TX_ENQUEUE_OK) {
            return 0;
        }
    }
    return ble_mesh_tx_queue_count(&fixture->scheduler.routed_tx_queue) ==
        BLE_MESH_TX_QUEUE_CAPACITY;
}

static unsigned int corrective_queued_tc_count(const corrective_fixture_t *fixture,
                                                uint16_t sequence,
                                                const uint8_t subject[TAVRN_ADVA_LEN])
{
    unsigned int count = 0u;
    uint8_t index;

    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry = &fixture->scheduler.routed_tx_queue.entries[index];
        const uint8_t *pdu;

        if (entry->occupied == 0u || entry->item.adv_len != 31u) continue;
        pdu = &entry->item.adv_data[7];
        if (pdu[4] == TAVRN_WIRE_TC_UPDATE &&
            ((uint16_t)pdu[13] | ((uint16_t)pdu[14] << 8)) == sequence &&
            memcmp(&pdu[15], subject, TAVRN_ADVA_LEN) == 0) {
            count++;
        }
    }
    return count;
}

static int corrective_queued_tc_sequence(const corrective_fixture_t *fixture,
                                         const uint8_t subject[TAVRN_ADVA_LEN],
                                         uint16_t *sequence_out)
{
    uint8_t index;

    if (sequence_out == NULL) return 0;
    for (index = 0u; index < BLE_MESH_TX_QUEUE_CAPACITY; index++) {
        const ble_mesh_tx_queue_entry_t *entry = &fixture->scheduler.routed_tx_queue.entries[index];
        const uint8_t *pdu;

        if (entry->occupied == 0u || entry->item.adv_len != 31u) continue;
        pdu = &entry->item.adv_data[7];
        if (pdu[4] == TAVRN_WIRE_TC_UPDATE &&
            memcmp(&pdu[15], subject, TAVRN_ADVA_LEN) == 0) {
            *sequence_out = (uint16_t)pdu[13] | ((uint16_t)pdu[14] << 8);
            return 1;
        }
    }
    return 0;
}

static int corrective_begin_active_rfi(corrective_fixture_t *fixture,
                                       uint32_t now_ms)
{
    tavrn_adva_t mentor = adva(adva_b);

    return fixture != NULL &&
        tavrn_mentorship_begin_active_sync(&fixture->mentorship, &mentor, 2u,
                                           now_ms) == TAVRN_MENTORSHIP_OK &&
        tavrn_mentorship_tick(&fixture->mentorship, now_ms + 1u) ==
            TAVRN_MENTORSHIP_OK &&
        fixture->mentorship.active_sync.valid != 0u &&
        fixture->mentorship.active_sync.queued_pull_valid != 0u &&
        corrective_queue_contains_control(
            fixture, &fixture->mentorship.active_sync.queued_pull);
}

static int corrective_busy_retains_soft_request(corrective_fixture_t *fixture,
                                                 const tavrn_validated_control_t *base,
                                                 uint32_t now_ms)
{
    tavrn_validated_control_t augmented;
    tavrn_tc_metadata_snapshot_t before;
    tavrn_tc_metadata_snapshot_t after;

    if (fixture->router.control_augmentation.prepare(
            fixture->router.control_augmentation.context, base, &augmented, now_ms) !=
            TAVRN_ROUTER_CONTROL_AUGMENTATION_OK ||
        augmented.pdu_len < 18u || (augmented.pdu[5] & 0x08u) == 0u ||
        tavrn_maintenance_tc_metadata_snapshot(&fixture->maintenance.tc_metadata, &before) !=
            TAVRN_TC_METADATA_OK) {
        return 0;
    }
    fixture->router.control_augmentation.admitted(
        fixture->router.control_augmentation.context, base, &augmented,
        NULL, 1u, TAVRN_LINK_SEND_BUSY, BLE_MESH_TX_TOKEN_NONE, now_ms);
    return tavrn_maintenance_tc_metadata_snapshot(&fixture->maintenance.tc_metadata, &after) ==
            TAVRN_TC_METADATA_OK &&
        memcmp(&before, &after, sizeof(before)) == 0;
}

static void test_corrective_tc_serial_domain(void)
{
    corrective_fixture_t fixture;
    tavrn_gtt_expiry_snapshot_t subject;
    tavrn_tc_metadata_action_t relay;
    tavrn_adva_t outer = adva(adva_b);
    uint8_t pdu[24];

    STRUCTURAL("corrective-tc-serial-fixture", corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    STRUCTURAL("corrective-tc-serial-subject",
               corrective_observe(&fixture, adva_c, 1u, 1u,
                                   TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 21u));
    if (structural_failures != 0u) return;
    make_tc(pdu, adva_b, adva_c, 2u, 0u, 0u, TAVRN_TC_EVENT_JOIN);
    memset(&relay, 0, sizeof(relay));
    CHECK("SERIAL-01", tavrn_maintenance_tc_receive(&fixture.maintenance.tc_metadata,
                                                        pdu, sizeof(pdu), &outer,
                                                        22u, &relay) ==
                           TAVRN_TC_METADATA_APPLIED &&
                           corrective_gtt_snapshot_for(&fixture, adva_c, 22u, &subject) &&
                           subject.serial_present != 0u && subject.serial == 1u);
    make_tc(pdu, adva_b, adva_c, 4u, 0u, 0u, TAVRN_TC_EVENT_LEAVE);
    memset(&relay, 0, sizeof(relay));
    CHECK("SERIAL-01", tavrn_maintenance_tc_receive(&fixture.maintenance.tc_metadata,
                                                        pdu, sizeof(pdu), &outer,
                                                        23u, &relay) ==
                           TAVRN_TC_METADATA_APPLIED &&
                           corrective_gtt_snapshot_for(&fixture, adva_c, 23u, &subject) &&
                           subject.serial_present != 0u && subject.serial == 1u &&
                           subject.freshness == TAVRN_GTT_FRESHNESS_DEPARTED);
    make_tc(pdu, adva_b, adva_d, 3u, 0u, 0u, TAVRN_TC_EVENT_JOIN);
    memset(&relay, 0, sizeof(relay));
    CHECK("GTT-03", tavrn_maintenance_tc_receive(&fixture.maintenance.tc_metadata,
                                                    pdu, sizeof(pdu), &outer,
                                                    24u, &relay) == TAVRN_TC_METADATA_APPLIED &&
                      corrective_gtt_snapshot_for(&fixture, adva_d, 24u, &subject) &&
                      subject.serial_present == 0u && subject.direct == 0u);
}

static void test_corrective_local_leave_fifo(void)
{
    corrective_fixture_t fixture;
    tavrn_tc_metadata_status_t first;
    tavrn_tc_metadata_status_t second;
    tavrn_adva_t first_hop = adva(adva_c);
    tavrn_adva_t second_hop = adva(adva_d);
    uint16_t first_sequence = 0u;
    uint16_t second_sequence = 0u;

    STRUCTURAL("corrective-local-leave-fifo-fixture", corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    STRUCTURAL("corrective-local-leave-fifo-queue",
               corrective_fill_control_queue(&fixture, 30u));
    if (structural_failures != 0u) return;
    first = tavrn_maintenance_tc_on_retry_exhausted(&fixture.maintenance, &first_hop,
                                                    NULL, 30u);
    second = tavrn_maintenance_tc_on_retry_exhausted(&fixture.maintenance, &second_hop,
                                                     NULL, 30u);
    (void)tavrn_maintenance_tc_owner_tick(&fixture.maintenance, 30u);
    corrective_clear_queue(&fixture);
    (void)tavrn_maintenance_tc_owner_tick(&fixture.maintenance, 31u);
    (void)tavrn_maintenance_tc_owner_tick(&fixture.maintenance, 32u);
    CHECK("TCQ-01", (first == TAVRN_TC_METADATA_PREPARED ||
                       first == TAVRN_TC_METADATA_RETAINED) &&
                       (second == TAVRN_TC_METADATA_PREPARED ||
                        second == TAVRN_TC_METADATA_RETAINED) &&
                       corrective_queued_tc_sequence(&fixture, adva_c, &first_sequence) &&
                       corrective_queued_tc_sequence(&fixture, adva_d, &second_sequence) &&
                       first_sequence != 0u && second_sequence != 0u &&
                       first_sequence < second_sequence);
}

static void test_corrective_relay_fifo(void)
{
    corrective_fixture_t fixture;
    tavrn_adva_t outer = adva(adva_b);
    tavrn_tc_metadata_action_t relay;
    uint8_t first_pdu[24];
    uint8_t second_pdu[24];

    STRUCTURAL("corrective-relay-fifo-fixture", corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    STRUCTURAL("corrective-relay-fifo-queue", corrective_fill_control_queue(&fixture, 30u));
    if (structural_failures != 0u) return;
    make_tc(first_pdu, adva_b, adva_c, 11u, 2u, 0u, TAVRN_TC_EVENT_JOIN);
    make_tc(second_pdu, adva_b, adva_d, 12u, 2u, 0u, TAVRN_TC_EVENT_JOIN);
    memset(&relay, 0, sizeof(relay));
    CHECK("TCQ-02", tavrn_maintenance_tc_receive(&fixture.maintenance.tc_metadata,
                                                       first_pdu, sizeof(first_pdu), &outer,
                                                       30u, &relay) == TAVRN_TC_METADATA_APPLIED &&
                      tavrn_maintenance_tc_receive(&fixture.maintenance.tc_metadata,
                                                    second_pdu, sizeof(second_pdu), &outer,
                                                    30u, &relay) == TAVRN_TC_METADATA_APPLIED &&
                      tavrn_maintenance_tc_owner_tick(&fixture.maintenance, 30u) ==
                          TAVRN_TC_METADATA_RETAINED &&
                      (corrective_clear_queue(&fixture), 1) &&
                      tavrn_maintenance_tc_owner_tick(&fixture.maintenance, 31u) ==
                          TAVRN_TC_METADATA_OK &&
                      tavrn_maintenance_tc_owner_tick(&fixture.maintenance, 32u) ==
                          TAVRN_TC_METADATA_OK &&
                      corrective_queued_tc_count(&fixture, 11u, adva_c) == 1u &&
                       corrective_queued_tc_count(&fixture, 12u, adva_d) == 1u);
}

static void test_corrective_tc_admission_consumes_evicted_data(void)
{
    corrective_fixture_t origin_fixture;
    corrective_fixture_t relay_fixture;
    tavrn_link_data_t origin_data;
    tavrn_link_data_t relay_data;
    tavrn_tc_metadata_action_t relay;
    tavrn_adva_t failed_hop = adva(adva_c);
    tavrn_adva_t outer = adva(adva_b);
    uint8_t pdu[24];

    STRUCTURAL("corrective-tc-eviction-origin-fixture",
               corrective_fixture_init(&origin_fixture));
    STRUCTURAL("corrective-tc-eviction-origin-data",
               corrective_seed_evictable_transit(&origin_fixture, &origin_data));
    if (structural_failures != 0u) return;
    {
        tavrn_tc_metadata_status_t origin_status = tavrn_maintenance_tc_on_retry_exhausted(
            &origin_fixture.maintenance, &failed_hop, NULL, 90u);

    CHECK("TCQ-05", (origin_status == TAVRN_TC_METADATA_PREPARED ||
                       origin_status == TAVRN_TC_METADATA_RETAINED) &&
                       tavrn_maintenance_tc_owner_tick(&origin_fixture.maintenance, 90u) ==
                           TAVRN_TC_METADATA_OK &&
                       origin_fixture.link.counters.local_tx_not_attempted == 1u &&
                       origin_fixture.link.custody[0].phase == TAVRN_CUSTODY_FREE &&
                       origin_fixture.link.data_dedupe[0].custody_pinned == 0u &&
                       tavrn_link_v2_release_rx_custody(&origin_fixture.link, &origin_data,
                                                        91u) == TAVRN_LINK_RESOLVE_TOKEN_INVALID &&
                       tavrn_maintenance_tc_owner_tick(&origin_fixture.maintenance, 91u) ==
                           TAVRN_TC_METADATA_OK &&
                       origin_fixture.link.counters.local_tx_not_attempted == 1u);
    }

    STRUCTURAL("corrective-tc-eviction-relay-fixture",
               corrective_fixture_init(&relay_fixture));
    STRUCTURAL("corrective-tc-eviction-relay-data",
               corrective_seed_evictable_transit(&relay_fixture, &relay_data));
    if (structural_failures != 0u) return;
    make_tc(pdu, adva_e, adva_d, 0x44u, 1u, 0u, TAVRN_TC_EVENT_JOIN);
    memset(&relay, 0, sizeof(relay));
    CHECK("TCQ-05", tavrn_maintenance_tc_receive(&relay_fixture.maintenance.tc_metadata,
                                                    pdu, sizeof(pdu), &outer, 92u, &relay) ==
                           TAVRN_TC_METADATA_APPLIED && relay.valid != 0u &&
                       tavrn_maintenance_tc_owner_tick(&relay_fixture.maintenance, 92u) ==
                           TAVRN_TC_METADATA_OK &&
                       relay_fixture.link.counters.local_tx_not_attempted == 1u &&
                       relay_fixture.link.custody[0].phase == TAVRN_CUSTODY_FREE &&
                       relay_fixture.link.data_dedupe[0].custody_pinned == 0u &&
                       tavrn_link_v2_release_rx_custody(&relay_fixture.link, &relay_data,
                                                        93u) == TAVRN_LINK_RESOLVE_TOKEN_INVALID);
}

static void test_corrective_retry_exhausted_leave_overflow(void)
{
    corrective_fixture_t fixture;
    tavrn_tc_metadata_state_t overflow_state;
    tavrn_tc_metadata_telemetry_t telemetry;
    tavrn_adva_t first = generated_adva(0x70u);
    tavrn_adva_t overflow = generated_adva(0x90u);
    tavrn_adva_t rejected = generated_adva(0xa0u);
    uint16_t first_sequence = 0u;
    uint16_t overflow_sequence = 0u;
    uint8_t index;
    uint8_t fifo_retained = 1u;
    uint8_t later_emitted = 1u;

    STRUCTURAL("corrective-retry-leave-overflow-fixture",
               corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    for (index = 0u; index < TAVRN_TC_METADATA_ORIGIN_CAPACITY; index++) {
        tavrn_adva_t subject = generated_adva((uint8_t)(0x70u + index));
        tavrn_tc_metadata_status_t status = tavrn_maintenance_tc_on_retry_exhausted(
            &fixture.maintenance, &subject, NULL, 60u);

        if (status != (index == 0u ? TAVRN_TC_METADATA_PREPARED :
                                     TAVRN_TC_METADATA_RETAINED)) {
            fifo_retained = 0u;
        }
    }
    CHECK("TCQ-04", fifo_retained != 0u &&
                        fixture.maintenance.tc_metadata.origin_fact_count ==
                            TAVRN_TC_METADATA_ORIGIN_CAPACITY &&
                        tavrn_maintenance_tc_on_retry_exhausted(
                            &fixture.maintenance, &overflow, NULL, 60u) ==
                            TAVRN_TC_METADATA_RETAINED &&
                        fixture.maintenance.tc_metadata.retry_exhausted_leave_overflow.valid !=
                            0u &&
                        memcmp(fixture.maintenance.tc_metadata.retry_exhausted_leave_overflow
                                   .failed_next_hop.bytes,
                               overflow.bytes, TAVRN_ADVA_LEN) == 0 &&
                        tavrn_maintenance_tc_on_retry_exhausted(
                            &fixture.maintenance, &overflow, NULL, 60u) ==
                            TAVRN_TC_METADATA_RETAINED &&
                        tavrn_maintenance_tc_metadata_telemetry(
                            &fixture.maintenance, &telemetry) == TAVRN_TC_METADATA_OK &&
                        telemetry.origin_queue_depth ==
                            TAVRN_TC_METADATA_ORIGIN_CAPACITY + 1u);
    overflow_state = fixture.maintenance.tc_metadata;
    CHECK("TCQ-04", tavrn_maintenance_tc_on_retry_exhausted(
                        &fixture.maintenance, &rejected, NULL, 60u) ==
                        TAVRN_TC_METADATA_INVALID &&
                        memcmp(&fixture.maintenance.tc_metadata, &overflow_state,
                               sizeof(overflow_state)) == 0);

    STRUCTURAL("corrective-retry-leave-overflow-busy-queue",
               corrective_fill_control_queue(&fixture, 61u));
    if (structural_failures != 0u) return;
    CHECK("TCQ-04", tavrn_maintenance_tc_owner_tick(&fixture.maintenance, 61u) ==
                        TAVRN_TC_METADATA_RETAINED &&
                        fixture.maintenance.tc_metadata.origin_pending.valid != 0u &&
                        memcmp(fixture.maintenance.tc_metadata.origin_pending.subject.bytes,
                               first.bytes, TAVRN_ADVA_LEN) == 0 &&
                        memcmp(fixture.maintenance.tc_metadata.origin_facts[0].subject.bytes,
                               first.bytes, TAVRN_ADVA_LEN) == 0 &&
                        fixture.maintenance.tc_metadata.retry_exhausted_leave_overflow.valid !=
                            0u);
    corrective_clear_queue(&fixture);
    CHECK("TCQ-04", tavrn_maintenance_tc_owner_tick(&fixture.maintenance, 62u) ==
                        TAVRN_TC_METADATA_OK &&
                        corrective_queued_tc_sequence(&fixture, first.bytes,
                                                       &first_sequence) &&
                        first_sequence != 0u &&
                        fixture.maintenance.tc_metadata.retry_exhausted_leave_overflow.valid ==
                            0u &&
                        fixture.maintenance.tc_metadata.origin_fact_count ==
                            TAVRN_TC_METADATA_ORIGIN_CAPACITY);
    for (index = 0u; index < TAVRN_TC_METADATA_ORIGIN_CAPACITY; index++) {
        corrective_clear_queue(&fixture);
        if (tavrn_maintenance_tc_owner_tick(&fixture.maintenance,
                                            (uint32_t)(63u + index)) !=
                TAVRN_TC_METADATA_OK) {
            later_emitted = 0u;
            break;
        }
    }
    CHECK("TCQ-04", later_emitted != 0u &&
                        corrective_queued_tc_sequence(&fixture, overflow.bytes,
                                                       &overflow_sequence) &&
                        overflow_sequence != 0u && overflow_sequence > first_sequence &&
                        corrective_queued_tc_count(&fixture, overflow_sequence,
                                                    overflow.bytes) == 1u &&
                        fixture.maintenance.tc_metadata.origin_fact_count == 0u);
}

static void test_corrective_obligation_capacity_and_hop_boundary(void)
{
    corrective_fixture_t fixture;
    tavrn_tc_metadata_state_t relay_state;
    tavrn_tc_metadata_state_t boundary_state;
    tavrn_tc_metadata_state_t before;
    tavrn_tc_metadata_config_t settings = config();
    tavrn_tc_metadata_action_t relay;
    tavrn_adva_t outer = adva(adva_b);
    tavrn_adva_t first_origin = generated_adva(0x70u);
    tavrn_adva_t duplicate_subject = generated_adva(0x40u);
    tavrn_adva_t overflow_subject = generated_adva(0x60u);
    uint8_t pdu[24];
    uint8_t index;
    uint8_t origin_retained = 1u;
    uint8_t relay_admitted = 1u;

    STRUCTURAL("corrective-obligation-capacity-fixture", corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    for (index = 0u; index < TAVRN_TC_METADATA_ORIGIN_CAPACITY; index++) {
        tavrn_adva_t subject = generated_adva((uint8_t)(0x70u + index));
        tavrn_tc_metadata_status_t status = tavrn_maintenance_tc_on_verified_departure(
            &fixture.maintenance, &subject, 60u);

        if (status != (index == 0u ? TAVRN_TC_METADATA_PREPARED :
                                     TAVRN_TC_METADATA_RETAINED)) {
            origin_retained = 0u;
        }
    }
    before = fixture.maintenance.tc_metadata;
    CHECK("TCQ-04", origin_retained != 0u &&
                       fixture.maintenance.tc_metadata.origin_fact_count ==
                           TAVRN_TC_METADATA_ORIGIN_CAPACITY &&
                       memcmp(fixture.maintenance.tc_metadata.origin_facts[0].subject.bytes,
                              first_origin.bytes, TAVRN_ADVA_LEN) == 0 &&
                        tavrn_maintenance_tc_on_verified_departure(
                            &fixture.maintenance, &first_origin, 60u) ==
                            TAVRN_TC_METADATA_RETAINED &&
                        tavrn_maintenance_tc_on_verified_departure(
                            &fixture.maintenance, &duplicate_subject, 60u) ==
                            TAVRN_TC_METADATA_BUSY &&
                        fixture.maintenance.tc_metadata.origin_busy_count ==
                            before.origin_busy_count + 1u);

    memset(&relay_state, 0, sizeof(relay_state));
    STRUCTURAL("corrective-relay-capacity-init",
               tavrn_maintenance_tc_metadata_init(&relay_state, &settings) ==
                   TAVRN_TC_METADATA_OK);
    if (structural_failures != 0u) return;
    for (index = 0u; index < TAVRN_TC_METADATA_RELAY_CAPACITY; index++) {
        tavrn_adva_t subject = generated_adva((uint8_t)(0x40u + index));

        make_tc(pdu, adva_b, subject.bytes, (uint16_t)(index + 1u), 1u, 0u,
                TAVRN_TC_EVENT_JOIN);
        memset(&relay, 0, sizeof(relay));
        if (tavrn_maintenance_tc_receive(&relay_state, pdu, sizeof(pdu), &outer,
                                         70u, &relay) != TAVRN_TC_METADATA_APPLIED ||
            relay.valid == 0u) {
            relay_admitted = 0u;
        }
    }
    before = relay_state;
    make_tc(pdu, adva_b, overflow_subject.bytes, 17u, 1u, 0u,
            TAVRN_TC_EVENT_JOIN);
    CHECK("TCQ-04", relay_admitted != 0u && relay_state.relay_queue_count ==
                           TAVRN_TC_METADATA_RELAY_CAPACITY &&
                       relay_state.relay_pending.tc_sequence == 1u &&
                       relay_state.relay_backlog[
                           TAVRN_TC_METADATA_RELAY_CAPACITY - 2u].tc_sequence == 16u &&
                        tavrn_maintenance_tc_receive(&relay_state, pdu, sizeof(pdu), &outer,
                                                     70u, &relay) == TAVRN_TC_METADATA_BUSY &&
                        (make_tc(pdu, adva_b, duplicate_subject.bytes, 1u, 1u, 0u,
                                 TAVRN_TC_EVENT_JOIN),
                         tavrn_maintenance_tc_receive(&relay_state, pdu, sizeof(pdu), &outer,
                                                      70u, &relay) ==
                             TAVRN_TC_METADATA_IGNORED) &&
                        relay_state.relay_busy_count == before.relay_busy_count + 1u);

    memset(&boundary_state, 0, sizeof(boundary_state));
    STRUCTURAL("corrective-relay-hop-boundary-init",
               tavrn_maintenance_tc_metadata_init(&boundary_state, &settings) ==
                   TAVRN_TC_METADATA_OK);
    if (structural_failures != 0u) return;
    make_tc(pdu, adva_b, adva_c, 1u, 1u, 15u, TAVRN_TC_EVENT_JOIN);
    memset(&relay, 0, sizeof(relay));
    CHECK("TCQ-04", tavrn_maintenance_tc_receive(&boundary_state, pdu, sizeof(pdu),
                                                    &outer, 71u, &relay) ==
                           TAVRN_TC_METADATA_APPLIED && relay.valid == 0u &&
                       boundary_state.relay_queue_count == 0u &&
                       boundary_state.local_apply_count == 1u);
}

static void test_corrective_join_leave_sequence_arbitration(void)
{
    corrective_fixture_t fixture;
    tavrn_tc_metadata_status_t leave;
    tavrn_adva_t failed_hop = adva(adva_c);
    uint16_t leave_sequence = 0u;

    STRUCTURAL("corrective-join-leave-sequence-fixture",
                corrective_fixture_init_with_join_binding(&fixture, 1u, 1u));
    if (structural_failures != 0u) return;
    leave = tavrn_maintenance_tc_on_retry_exhausted(&fixture.maintenance, &failed_hop,
                                                    NULL, 21u);
    CHECK("TCQ-03", fixture.mentorship.join_obligations[0].valid != 0u &&
                       fixture.mentorship.join_obligations[0].join_sequence == 1u &&
                       (leave == TAVRN_TC_METADATA_PREPARED ||
                        leave == TAVRN_TC_METADATA_RETAINED) &&
                       tavrn_mentorship_tick(&fixture.mentorship, 21u) == TAVRN_MENTORSHIP_OK &&
                       tavrn_maintenance_tc_owner_tick(&fixture.maintenance, 22u) ==
                           TAVRN_TC_METADATA_OK &&
                       corrective_queued_tc_sequence(&fixture, adva_c, &leave_sequence) &&
                        leave_sequence == 2u);
}

static void test_corrective_local_tc_preempts_rfi(void)
{
    corrective_fixture_t requester;
    corrective_fixture_t responder;
    corrective_fixture_t overflow;
    tavrn_validated_control_t unrelated_pull;
    tavrn_validated_control_t requester_pull;
    tavrn_validated_control_t responder_pull;
    tavrn_validated_control_t responder_data;
    tavrn_validated_control_t overflow_pull;
    tavrn_link_event_t outcome;
    ble_mesh_sched_event_t event;
    tavrn_adva_t first = adva(adva_c);
    tavrn_adva_t second = adva(adva_d);
    tavrn_adva_t overflow_subject = generated_adva(0x92u);
    uint8_t index;

    STRUCTURAL("corrective-local-tc-rfi-requester-fixture",
               corrective_fixture_init(&requester));
    if (structural_failures != 0u) return;
    STRUCTURAL("corrective-local-tc-rfi-requester-start",
               corrective_begin_active_rfi(&requester, 30u));
    if (structural_failures != 0u) return;
    requester_pull = requester.mentorship.active_sync.queued_pull;
    unrelated_pull = requester_pull;
    unrelated_pull.pdu[5] = 0u;
    unrelated_pull.pdu[18] ^= 0x5au;
    memset(&outcome, 0, sizeof(outcome));
    STRUCTURAL("corrective-local-tc-rfi-unrelated-pull",
               tavrn_link_v2_send_control(&requester.link, &unrelated_pull, NULL, 0u,
                                           32u, &outcome) == TAVRN_LINK_SEND_OK &&
               corrective_queue_contains_control(
                   &requester, &requester_pull) &&
               corrective_queue_contains_control(&requester, &unrelated_pull));
    if (structural_failures != 0u) return;
    CHECK("TCQ-06", tavrn_maintenance_tc_on_verified_departure(
                        &requester.maintenance, &first, 33u) ==
                            TAVRN_TC_METADATA_PREPARED &&
                        requester.maintenance.tc_metadata.origin_fact_count == 1u &&
                        requester.mentorship.active_sync.valid == 0u &&
                        !corrective_queue_contains_control(&requester, &requester_pull) &&
                        corrective_queue_contains_control(&requester, &unrelated_pull) &&
                        tavrn_maintenance_tc_on_verified_departure(
                            &requester.maintenance, &second, 34u) ==
                            TAVRN_TC_METADATA_RETAINED &&
                        (corrective_clear_queue(&requester), 1) &&
                        corrective_fill_control_queue(&requester, 131u) &&
                        tavrn_mentorship_begin_active_sync(
                            &requester.mentorship, &(tavrn_adva_t){
                                { adva_b[0], adva_b[1], adva_b[2], adva_b[3],
                                  adva_b[4], adva_b[5] }
                            }, 2u, 131u) == TAVRN_MENTORSHIP_BUSY &&
                        tavrn_maintenance_tc_owner_tick(&requester.maintenance, 132u) ==
                            TAVRN_TC_METADATA_RETAINED &&
                        requester.maintenance.tc_metadata.origin_fact_count == 2u &&
                        (corrective_clear_queue(&requester), 1) &&
                        tavrn_maintenance_tc_owner_tick(&requester.maintenance, 133u) ==
                            TAVRN_TC_METADATA_OK &&
                        requester.maintenance.tc_metadata.origin_fact_count == 1u &&
                        tavrn_mentorship_begin_active_sync(
                            &requester.mentorship, &(tavrn_adva_t){
                                { adva_b[0], adva_b[1], adva_b[2], adva_b[3],
                                  adva_b[4], adva_b[5] }
                            }, 2u, 134u) == TAVRN_MENTORSHIP_BUSY);

    STRUCTURAL("corrective-local-tc-rfi-responder-fixture",
               corrective_fixture_init(&responder));
    if (structural_failures != 0u) return;
    STRUCTURAL("corrective-local-tc-rfi-responder-pull",
               tavrn_mentorship_build_sync_pull(&(tavrn_adva_t){
                                                   { adva_c[0], adva_c[1], adva_c[2],
                                                     adva_c[3], adva_c[4], adva_c[5] }
                                               },
                                               &responder.gtt.config.local_identity,
                                               0x9123u, 0u, &responder_pull) ==
                   TAVRN_MENTORSHIP_OK &&
               ((responder_pull.pdu[5] = 0x01u), 1) &&
               corrective_wrap_control_event(&responder_pull, adva_c, &event) &&
               tavrn_mentorship_handle_scheduler_event(&responder.mentorship, &event,
                                                       40u, NULL) == TAVRN_MENTORSHIP_OK &&
               tavrn_mentorship_tick(&responder.mentorship, 41u) ==
                   TAVRN_MENTORSHIP_OK &&
               responder.mentorship.serving_active_rfi != 0u &&
               responder.mentorship.serving_rfi_data_valid != 0u);
    if (structural_failures != 0u) return;
    responder_data = responder.mentorship.serving_rfi_data;
    CHECK("TCQ-06", corrective_queue_contains_control(&responder, &responder_data) &&
                        tavrn_maintenance_tc_on_direct_timeout_departure(
                            &responder.maintenance, &first, 42u) ==
                            TAVRN_TC_METADATA_PREPARED &&
                        responder.mentorship.serving_session_valid == 0u &&
                        responder.mentorship.serving_active_rfi == 0u &&
                        !corrective_queue_contains_control(&responder, &responder_data));

    STRUCTURAL("corrective-local-tc-rfi-overflow-fixture",
               corrective_fixture_init(&overflow));
    STRUCTURAL("corrective-local-tc-rfi-overflow-start",
               corrective_begin_active_rfi(&overflow, 50u));
    if (structural_failures != 0u) return;
    overflow_pull = overflow.mentorship.active_sync.queued_pull;
    for (index = 0u; index < TAVRN_TC_METADATA_ORIGIN_CAPACITY; index++) {
        tavrn_tc_metadata_origin_fact_t *fact =
            &overflow.maintenance.tc_metadata.origin_facts[index];

        memset(fact, 0, sizeof(*fact));
        fact->subject = generated_adva((uint8_t)(0x60u + index));
        fact->event = TAVRN_TC_EVENT_LEAVE;
        fact->valid = 1u;
    }
    overflow.maintenance.tc_metadata.origin_fact_count =
        TAVRN_TC_METADATA_ORIGIN_CAPACITY;
    CHECK("TCQ-06", tavrn_maintenance_tc_on_retry_exhausted(
                        &overflow.maintenance, &overflow_subject, NULL, 52u) ==
                            TAVRN_TC_METADATA_RETAINED &&
                        overflow.maintenance.tc_metadata.retry_exhausted_leave_overflow
                            .valid != 0u &&
                        overflow.mentorship.active_sync.valid == 0u &&
                        !corrective_queue_contains_control(&overflow, &overflow_pull));
}

static void test_corrective_local_tc_port_is_optional(void)
{
    corrective_fixture_t bound;
    corrective_fixture_t unbound;
    tavrn_adva_t subject = adva(adva_c);
    tavrn_tc_metadata_status_t bound_status;
    tavrn_tc_metadata_status_t unbound_status;
    tavrn_tc_metadata_snapshot_t bound_snapshot;
    tavrn_tc_metadata_snapshot_t unbound_snapshot;

    STRUCTURAL("corrective-local-tc-port-bound-fixture",
               corrective_fixture_init(&bound));
    STRUCTURAL("corrective-local-tc-port-unbound-fixture",
               corrective_fixture_init_unbound(&unbound));
    if (structural_failures != 0u) return;
    bound_status = tavrn_maintenance_tc_on_verified_departure(
        &bound.maintenance, &subject, 70u);
    unbound_status = tavrn_maintenance_tc_on_verified_departure(
        &unbound.maintenance, &subject, 70u);
    CHECK("TCQ-06", bound.maintenance.local_tc_retained.callback != NULL &&
                        unbound.maintenance.local_tc_retained.callback == NULL &&
                        bound_status == TAVRN_TC_METADATA_PREPARED &&
                        unbound_status == bound_status &&
                        tavrn_maintenance_tc_metadata_snapshot(
                            &bound.maintenance.tc_metadata, &bound_snapshot) ==
                            TAVRN_TC_METADATA_OK &&
                        tavrn_maintenance_tc_metadata_snapshot(
                            &unbound.maintenance.tc_metadata, &unbound_snapshot) ==
                            TAVRN_TC_METADATA_OK &&
                        memcmp(&unbound_snapshot, &bound_snapshot,
                               sizeof(bound_snapshot)) == 0);
}

static void test_corrective_received_tc_preemption_filter(void)
{
    corrective_fixture_t fixture;
    ble_mesh_sched_event_t event;
    uint8_t pdu[24];

    STRUCTURAL("corrective-rx-tc-rfi-fixture", corrective_fixture_init(&fixture));
    STRUCTURAL("corrective-rx-tc-rfi-start", corrective_begin_active_rfi(&fixture, 60u));
    if (structural_failures != 0u) return;
    make_tc(pdu, adva_b, adva_c, 0x71u, 15u, 0u, TAVRN_TC_EVENT_JOIN);
    pdu[21] = 0xffu;
    STRUCTURAL("corrective-rx-tc-rfi-rejected",
               corrective_wrap_tc_event(pdu, adva_b, &event) &&
               tavrn_mentorship_handle_scheduler_event(&fixture.mentorship, &event,
                                                       62u, NULL) == TAVRN_MENTORSHIP_OK &&
               fixture.mentorship.active_sync.valid != 0u);
    if (structural_failures != 0u) return;
    make_tc(pdu, adva_b, adva_c, 0x72u, 15u, 0u, TAVRN_TC_EVENT_JOIN);
    fixture.maintenance.tc_metadata.uuid[0].origin = adva(adva_b);
    fixture.maintenance.tc_metadata.uuid[0].sequence = 0x72u;
    fixture.maintenance.tc_metadata.uuid[0].expires_at_ms = 100u;
    fixture.maintenance.tc_metadata.uuid[0].valid = 1u;
    CHECK("TCQ-06", corrective_wrap_tc_event(pdu, adva_b, &event) &&
                        tavrn_mentorship_handle_scheduler_event(&fixture.mentorship,
                                                                &event, 63u, NULL) ==
                            TAVRN_MENTORSHIP_OK &&
                        fixture.mentorship.active_sync.valid != 0u);
    make_tc(pdu, adva_b, adva_c, 0x73u, 15u, 0u, TAVRN_TC_EVENT_JOIN);
    fixture.maintenance.tc_metadata.subject[0].subject = adva(adva_c);
    fixture.maintenance.tc_metadata.subject[0].event = TAVRN_TC_EVENT_JOIN;
    fixture.maintenance.tc_metadata.subject[0].expires_at_ms = 100u;
    fixture.maintenance.tc_metadata.subject[0].valid = 1u;
    CHECK("TCQ-06", corrective_wrap_tc_event(pdu, adva_b, &event) &&
                        tavrn_mentorship_handle_scheduler_event(&fixture.mentorship,
                                                                &event, 64u, NULL) ==
                            TAVRN_MENTORSHIP_OK &&
                        fixture.mentorship.active_sync.valid != 0u);
    make_tc(pdu, adva_b, adva_d, 0x74u, 0u, 0u, TAVRN_TC_EVENT_JOIN);
    CHECK("TCQ-06", corrective_wrap_tc_event(pdu, adva_b, &event) &&
                        tavrn_mentorship_handle_scheduler_event(&fixture.mentorship,
                                                                &event, 65u, NULL) ==
                            TAVRN_MENTORSHIP_OK &&
                        fixture.mentorship.active_sync.valid == 0u);
}

static void test_corrective_metadata_answer_merge(void)
{
    corrective_fixture_t fixture;
    tavrn_metadata_candidate_t request = candidate(adva_c, TAVRN_METADATA_SOFT_REQUEST,
                                                    1u, 1u);
    tavrn_metadata_selection_t selection;
    tavrn_metadata_attached_control_t attached;
    tavrn_validated_control_t base;
    tavrn_rx_control_event_t event;
    tavrn_gtt_expiry_snapshot_t subject;
    tavrn_tc_metadata_snapshot_t metadata;

    STRUCTURAL("corrective-metadata-fixture", corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    STRUCTURAL("corrective-metadata-subject",
               corrective_observe(&fixture, adva_c, 7u, 1u,
                                   TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 30u));
    if (structural_failures != 0u) return;
    memset(&selection, 0, sizeof(selection));
    selection.valid = 1u;
    selection.count = 1u;
    selection.entries[0] = candidate(adva_c, TAVRN_METADATA_SUBJECT_SELF_ANSWER, 2u, 0u);
    base = route_control(TAVRN_METADATA_FRAME_RREQ8, 0u);
    CHECK("META-04", tavrn_maintenance_metadata_create(&fixture.maintenance.tc_metadata,
                                                          &request, 31u) ==
                           TAVRN_TC_METADATA_OK &&
                      tavrn_maintenance_metadata_attach_control(&base, &selection,
                                                                 &attached) ==
                           TAVRN_TC_METADATA_OK &&
                      control_round_trip(&attached.control, adva_b, &event) &&
                      tavrn_maintenance_metadata_receive_control(
                          &fixture.maintenance.tc_metadata, &event, 100u) ==
                          TAVRN_TC_METADATA_APPLIED &&
                      corrective_gtt_snapshot_for(&fixture, adva_c, 100u, &subject) &&
                      subject.serial_present != 0u && subject.serial == 7u &&
                      subject.direct == 0u && subject.hard_deadline_ms == 60100u &&
                       tavrn_maintenance_tc_metadata_snapshot(&fixture.maintenance.tc_metadata,
                                                               &metadata) ==
                           TAVRN_TC_METADATA_OK && metadata.candidate_count == 0u);
}

static void test_corrective_metadata_transaction_retention(void)
{
    corrective_fixture_t fixture;
    tavrn_metadata_candidate_t request = candidate(adva_c, TAVRN_METADATA_SOFT_REQUEST,
                                                    1u, 1u);
    tavrn_validated_control_t base;
    tavrn_validated_control_t mismatch;
    tavrn_validated_control_t first;
    tavrn_validated_control_t pass_through;
    tavrn_validated_control_t retry;
    tavrn_validated_control_t incoming_base;
    tavrn_metadata_selection_t incoming_selection;
    tavrn_metadata_attached_control_t incoming_attached;
    ble_mesh_sched_event_t completion;
    ble_mesh_sched_event_t incoming_event;
    tavrn_mentorship_event_trace_t incoming_trace;
    ble_mesh_tx_token_t retry_token;
    tavrn_tc_metadata_snapshot_t before;
    tavrn_tc_metadata_snapshot_t after;
    tavrn_tc_metadata_telemetry_t telemetry;
    tavrn_metadata_pending_transaction_t pending_before;

    STRUCTURAL("corrective-metadata-transaction-fixture", corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    STRUCTURAL("corrective-metadata-freeze-subject",
               corrective_observe(&fixture, adva_c, 7u, 1u,
                                   TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 80u));
    if (structural_failures != 0u) return;
    base = route_control(TAVRN_METADATA_FRAME_RREQ8, 0u);
    CHECK("META-06", tavrn_maintenance_metadata_create(&fixture.maintenance.tc_metadata,
                                                           &request, 80u) ==
                           TAVRN_TC_METADATA_OK &&
                       fixture.router.control_augmentation.prepare(
                           fixture.router.control_augmentation.context, &base, &first, 81u) ==
                           TAVRN_ROUTER_CONTROL_AUGMENTATION_OK &&
                        fixture.maintenance.metadata_pending.valid != 0u &&
                        tavrn_maintenance_tc_metadata_snapshot(&fixture.maintenance.tc_metadata,
                                                                &before) == TAVRN_TC_METADATA_OK);
    incoming_base = route_control(TAVRN_METADATA_FRAME_RREQ8, 0u);
    memset(&incoming_selection, 0, sizeof(incoming_selection));
    incoming_selection.valid = 1u;
    incoming_selection.count = 1u;
    incoming_selection.entries[0] = candidate(adva_c, TAVRN_METADATA_SUBJECT_SELF_ANSWER,
                                               2u, 0u);
    STRUCTURAL("corrective-metadata-freeze-attach",
               tavrn_maintenance_metadata_attach_control(
                    &incoming_base, &incoming_selection, &incoming_attached) ==
                    TAVRN_TC_METADATA_OK);
    pending_before = fixture.maintenance.metadata_pending;
    memset(&pass_through, 0, sizeof(pass_through));
    CHECK("META-07-PENDING",
          fixture.router.control_augmentation.prepare(
              fixture.router.control_augmentation.context, &incoming_attached.control,
              &pass_through, 81u) == TAVRN_ROUTER_CONTROL_AUGMENTATION_OK &&
              memcmp(&pass_through, &incoming_attached.control,
                     sizeof(pass_through)) == 0 &&
              memcmp(&pending_before, &fixture.maintenance.metadata_pending,
                     sizeof(pending_before)) == 0 &&
              tavrn_maintenance_tc_metadata_snapshot(&fixture.maintenance.tc_metadata,
                                                      &after) ==
                  TAVRN_TC_METADATA_OK &&
              memcmp(&before, &after, sizeof(before)) == 0);
    STRUCTURAL("corrective-metadata-freeze-wire",
               corrective_wrap_control_event(&incoming_attached.control, adva_b,
                                              &incoming_event));
    if (structural_failures != 0u) return;
    memset(&incoming_trace, 0, sizeof(incoming_trace));
    (void)tavrn_mentorship_handle_scheduler_event(&fixture.mentorship, &incoming_event,
                                                   81u, &incoming_trace);
    STRUCTURAL("corrective-metadata-freeze-decode",
               incoming_trace.wire_decode_result == TAVRN_CODEC_OK);
    if (structural_failures != 0u) return;
    STRUCTURAL("corrective-metadata-freeze-rx", incoming_trace.rx_control_present != 0u);
    if (structural_failures != 0u) return;
    CHECK("META-06", fixture.maintenance.metadata_pending.valid != 0u &&
                       fixture.maintenance.metadata_frozen_rx_count == 1u &&
                       tavrn_maintenance_tc_metadata_snapshot(&fixture.maintenance.tc_metadata,
                                                               &after) == TAVRN_TC_METADATA_OK &&
                       memcmp(&before, &after, sizeof(before)) == 0);
    mismatch = base;
    mismatch.pdu[10] = adva_d[0];
    fixture.router.control_augmentation.admitted(
        fixture.router.control_augmentation.context, &mismatch, &first,
        NULL, 1u, TAVRN_LINK_SEND_OK, 0x41u, 82u);
    CHECK("META-06", fixture.maintenance.metadata_pending.valid != 0u &&
                        tavrn_maintenance_tc_metadata_snapshot(&fixture.maintenance.tc_metadata,
                                                                &after) == TAVRN_TC_METADATA_OK &&
                        memcmp(&before, &after, sizeof(before)) == 0 &&
                        fixture.router.control_augmentation.prepare(
                            fixture.router.control_augmentation.context, &mismatch, &retry, 82u) ==
                            TAVRN_ROUTER_CONTROL_AUGMENTATION_OK &&
                        memcmp(&mismatch, &retry, sizeof(retry)) == 0 &&
                        fixture.router.control_augmentation.prepare(
                           fixture.router.control_augmentation.context, &base, &retry, 83u) ==
                           TAVRN_ROUTER_CONTROL_AUGMENTATION_OK &&
                       memcmp(&first, &retry, sizeof(first)) == 0);
    fixture.router.control_augmentation.admitted(
        fixture.router.control_augmentation.context, &base, &retry,
        NULL, 1u, TAVRN_LINK_SEND_BUSY, BLE_MESH_TX_TOKEN_NONE, 84u);
    CHECK("META-06", fixture.maintenance.metadata_pending.valid != 0u &&
                        fixture.router.control_augmentation.prepare(
                            fixture.router.control_augmentation.context, &base, &first, 85u) ==
                            TAVRN_ROUTER_CONTROL_AUGMENTATION_BUSY);
    fixture.router.control_augmentation.admitted(
        fixture.router.control_augmentation.context, &base, &first,
        NULL, 1u, TAVRN_LINK_SEND_NO_SLOT, BLE_MESH_TX_TOKEN_NONE, 86u);
    CHECK("META-06", fixture.maintenance.metadata_pending.valid != 0u &&
                        fixture.router.control_augmentation.prepare(
                            fixture.router.control_augmentation.context, &base, &retry, 87u) ==
                            TAVRN_ROUTER_CONTROL_AUGMENTATION_BUSY);
    fixture.router.control_augmentation.admitted(
        fixture.router.control_augmentation.context, &base, &first,
        NULL, 1u, TAVRN_LINK_SEND_OK, 0x42u, 88u);
    memset(&completion, 0, sizeof(completion));
    completion.type = BLE_MESH_SCHED_EVENT_TX_FAILED;
    completion.tx_token = 0x42u;
    completion.tx_completed_channel_mask = 0u;
    CHECK("META-06", fixture.maintenance.metadata_pending.valid != 0u &&
                        tavrn_router_handle_scheduler_event(&fixture.router, &completion, 89u) ==
                            TAVRN_ROUTER_EVENT_IGNORED &&
                        fixture.maintenance.metadata_pending.valid != 0u &&
                        fixture.maintenance.metadata_pending.scheduler_token ==
                            BLE_MESH_TX_TOKEN_NONE &&
                        fixture.maintenance.metadata_pending.retry_pending != 0u &&
                        tavrn_maintenance_tc_metadata_snapshot(&fixture.maintenance.tc_metadata,
                                                                &after) ==
                            TAVRN_TC_METADATA_OK &&
                        after.candidate_count == 1u && after.cooldown_count == 0u &&
                        fixture.maintenance.metadata_completion_retry_count == 1u &&
                        tavrn_maintenance_metadata_owner_tick(&fixture.maintenance, 90u) ==
                            TAVRN_TC_METADATA_OK &&
                        fixture.maintenance.metadata_pending.scheduler_token !=
                            BLE_MESH_TX_TOKEN_NONE);
    retry_token = fixture.maintenance.metadata_pending.scheduler_token;
    corrective_clear_queue(&fixture);
    CHECK("META-06", tavrn_maintenance_metadata_owner_tick(&fixture.maintenance, 91u) ==
                            TAVRN_TC_METADATA_OK &&
                        fixture.maintenance.metadata_pending.scheduler_token !=
                            BLE_MESH_TX_TOKEN_NONE &&
                        fixture.maintenance.metadata_pending.scheduler_token != retry_token &&
                        fixture.maintenance.metadata_completion_retry_count == 2u);
    retry_token = fixture.maintenance.metadata_pending.scheduler_token;
    memset(&completion, 0, sizeof(completion));
    completion.type = BLE_MESH_SCHED_EVENT_TX_DONE;
    completion.tx_token = retry_token;
    completion.tx_completed_channel_mask = 0x01u;
    CHECK("META-06", tavrn_router_handle_scheduler_event(&fixture.router, &completion, 92u) ==
                             TAVRN_ROUTER_EVENT_IGNORED &&
                        fixture.maintenance.metadata_pending.valid == 0u &&
                        tavrn_maintenance_tc_metadata_snapshot(&fixture.maintenance.tc_metadata,
                                                                &after) == TAVRN_TC_METADATA_OK &&
                        after.candidate_count == 0u && after.cooldown_count == 1u &&
                        fixture.maintenance.metadata_completion_commit_count == 1u &&
                        tavrn_router_handle_scheduler_event(&fixture.router, &completion, 93u) ==
                            TAVRN_ROUTER_EVENT_IGNORED &&
                        fixture.maintenance.metadata_completion_commit_count == 1u &&
                        tavrn_maintenance_tc_metadata_telemetry(&fixture.maintenance,
                                                                 &telemetry) ==
                            TAVRN_TC_METADATA_OK &&
                        telemetry.metadata_base_type == TAVRN_WIRE_E_RREQ &&
                        telemetry.metadata_emitted_count == 1u &&
                        telemetry.metadata_pdu_len == 18u &&
                        telemetry.metadata_transaction_active == 0u &&
                        telemetry.metadata_completion_commit_count == 1u &&
                        telemetry.metadata_completion_retry_count == 2u);
}

static void test_corrective_decorated_control_pass_through(void)
{
    CHECK("META-07-RREQ-ONE", corrective_dispatch_decorated_control(
                         TAVRN_METADATA_FRAME_RREQ8, 0u, 1u, 0x10u,
                         AODV_ACTION_SEND_RREQ, 1u));
    CHECK("META-07-RREQ-MAX", corrective_dispatch_decorated_control(
                         TAVRN_METADATA_FRAME_RREQ8, 0u, 4u, 0u,
                         AODV_ACTION_SEND_RREQ, 1u));
    CHECK("META-07-RREP-MAX", corrective_dispatch_decorated_control(
                         TAVRN_METADATA_FRAME_RREP8, 0u, 3u, 0x40u,
                         AODV_ACTION_SEND_RREP, 0u));
    CHECK("META-07-RERR-D1", corrective_dispatch_decorated_control(
                         TAVRN_METADATA_FRAME_RERR8, 1u, 4u, 0u,
                         AODV_ACTION_SEND_RERR, 1u));
    CHECK("META-07-RERR-D2", corrective_dispatch_decorated_control(
                         TAVRN_METADATA_FRAME_RERR8, 2u, 3u, 0u,
                         AODV_ACTION_SEND_RERR, 1u));
    CHECK("META-07-RERR-D3", corrective_dispatch_decorated_control(
                         TAVRN_METADATA_FRAME_RERR8, 3u, 1u, 0u,
                         AODV_ACTION_SEND_RERR, 1u));
}

static void test_corrective_metadata_multi_entry_atomic_failure(void)
{
    corrective_fixture_t fixture;
    tavrn_metadata_rx_t rx;
    tavrn_tc_metadata_state_t before_state;
    tavrn_gtt_storage_t before_gtt;
    tavrn_tc_metadata_status_t status;
    tavrn_metadata_candidate_t b_request = candidate(
        adva_b, TAVRN_METADATA_SOFT_REQUEST, 1u, 1u);
    tavrn_metadata_candidate_t c_request = candidate(
        adva_c, TAVRN_METADATA_SOFT_REQUEST, 1u, 1u);
    tavrn_metadata_candidate_t d_request = candidate(
        adva_d, TAVRN_METADATA_SOFT_REQUEST, 1u, 1u);

    STRUCTURAL("corrective-metadata-multi-atomic-fixture", corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    if (tavrn_maintenance_metadata_create(&fixture.maintenance.tc_metadata,
                                          &(tavrn_metadata_candidate_t){
                                              .subject = { { 0x11u, 0x22u, 0x33u,
                                                             0x44u, 0x55u, 0xc1u } },
                                              .subject_sid8 = 0x11u,
                                              .ttl_bucket = 1u,
                                              .freshness_request = 1u,
                                              .kind = TAVRN_METADATA_SOFT_REQUEST,
                                          }, 90u) != TAVRN_TC_METADATA_OK ||
         tavrn_maintenance_metadata_create(&fixture.maintenance.tc_metadata,
                                            &c_request, 90u) != TAVRN_TC_METADATA_OK ||
         tavrn_maintenance_metadata_create(&fixture.maintenance.tc_metadata,
                                            &d_request, 90u) != TAVRN_TC_METADATA_OK ||
         tavrn_maintenance_metadata_create(&fixture.maintenance.tc_metadata,
                                            &b_request, 90u) != TAVRN_TC_METADATA_OK) {
        CHECK("META-05", 0);
        return;
    }
    memset(&rx, 0, sizeof(rx));
    rx.frame_kind = TAVRN_METADATA_FRAME_RREQ8;
    rx.outer_transmitter = adva(adva_b);
    rx.metadata_flag = 1u;
    rx.count = 2u;
    rx.entries[0] = candidate(adva_e, TAVRN_METADATA_SOFT_REQUEST, 1u, 1u);
    rx.entries[1] = candidate(adva_f, TAVRN_METADATA_SOFT_REQUEST, 1u, 1u);
    before_state = fixture.maintenance.tc_metadata;
    before_gtt = fixture.gtt_storage;
    status = tavrn_maintenance_metadata_receive(&fixture.maintenance.tc_metadata, &rx, 91u);
    CHECK("META-05", status == TAVRN_TC_METADATA_BUSY);
    CHECK("META-05", memcmp(&before_state, &fixture.maintenance.tc_metadata,
                             sizeof(before_state)) == 0 &&
                       memcmp(&before_gtt, &fixture.gtt_storage, sizeof(before_gtt)) == 0);
}

static void test_corrective_soft_expiry_cooldown(void)
{
    corrective_fixture_t fixture;
    tavrn_maintenance_expiry_sweep_snapshot_t sweep;
    tavrn_metadata_selection_t selection;

    STRUCTURAL("corrective-soft-cooldown-fixture", corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    STRUCTURAL("corrective-soft-cooldown-subject",
               corrective_observe(&fixture, adva_e, 9u, 1u,
                                   TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 30u));
    if (structural_failures != 0u) return;
    memset(&sweep, 0, sizeof(sweep));
    (void)tavrn_maintenance_sweep_expiry(&fixture.maintenance, 30u, &sweep);
    memset(&sweep, 0, sizeof(sweep));
    (void)tavrn_maintenance_sweep_expiry(&fixture.maintenance, 41u, &sweep);
    CHECK("META-01", corrective_soft_request_exists(&fixture.maintenance.tc_metadata,
                                                       adva_e) &&
                       tavrn_maintenance_metadata_select(
                           &fixture.maintenance.tc_metadata, TAVRN_METADATA_FRAME_RREQ8,
                           0u, 42u, &selection) == TAVRN_TC_METADATA_OK &&
                       selection.count != 0u &&
                       tavrn_maintenance_metadata_commit(
                           &fixture.maintenance.tc_metadata, &selection,
                           TAVRN_TC_ADMISSION_ADMITTED, 42u) == TAVRN_TC_METADATA_OK &&
                       !corrective_soft_request_exists(&fixture.maintenance.tc_metadata,
                                                        adva_e) &&
                       (memset(&sweep, 0, sizeof(sweep)),
                        tavrn_maintenance_sweep_expiry(&fixture.maintenance, 43u, &sweep)) ==
                           TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
                       !corrective_soft_request_exists(&fixture.maintenance.tc_metadata,
                                                        adva_e));
}

static void test_corrective_tombstone_metadata_rejection(void)
{
    corrective_fixture_t fixture;
    tavrn_metadata_candidate_t request = candidate(adva_c, TAVRN_METADATA_SOFT_REQUEST,
                                                    1u, 1u);
    tavrn_metadata_selection_t selection;
    tavrn_metadata_attached_control_t attached;
    tavrn_validated_control_t base;
    tavrn_rx_control_event_t event;
    tavrn_tc_metadata_state_t before_state;
    tavrn_gtt_storage_t before_gtt;
    tavrn_gtt_evidence_t departure;
    tavrn_gtt_expiry_snapshot_t tombstone;

    STRUCTURAL("corrective-tombstone-metadata-fixture", corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    STRUCTURAL("corrective-tombstone-metadata-subject",
               corrective_observe(&fixture, adva_c, 7u, 1u,
                                   TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 30u));
    if (structural_failures != 0u) return;
    memset(&departure, 0, sizeof(departure));
    departure.identity = adva(adva_b);
    departure.serial = 1u;
    departure.serial_present = 1u;
    departure.hop_count = 1u;
    departure.kind = TAVRN_GTT_EVIDENCE_DEPARTED;
    if (tavrn_maintenance_metadata_create(&fixture.maintenance.tc_metadata, &request, 31u) !=
            TAVRN_TC_METADATA_OK ||
        tavrn_gtt_observe_with_provenance(&fixture.gtt, &departure,
                                          TAVRN_GTT_PROVENANCE_CONFIRMED_DEPARTURE, 32u,
                                          NULL) != TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED ||
        !corrective_gtt_snapshot_for(&fixture, adva_b, 32u, &tombstone) ||
        tombstone.freshness != TAVRN_GTT_FRESHNESS_DEPARTED) {
        CHECK("META-05", 0);
        return;
    }
    memset(&selection, 0, sizeof(selection));
    selection.valid = 1u;
    selection.count = 1u;
    selection.entries[0] = candidate(adva_c, TAVRN_METADATA_SUBJECT_SELF_ANSWER, 2u, 0u);
    base = route_control(TAVRN_METADATA_FRAME_RREQ8, 0u);
    if (tavrn_maintenance_metadata_attach_control(&base, &selection, &attached) !=
            TAVRN_TC_METADATA_OK ||
        !control_round_trip(&attached.control, adva_b, &event)) {
        CHECK("META-05", 0);
        return;
    }
    before_state = fixture.maintenance.tc_metadata;
    before_gtt = fixture.gtt_storage;
    CHECK("META-05", tavrn_maintenance_metadata_receive_control(
                         &fixture.maintenance.tc_metadata, &event, 40u) !=
                         TAVRN_TC_METADATA_APPLIED &&
                       memcmp(&before_state, &fixture.maintenance.tc_metadata,
                              sizeof(before_state)) == 0 &&
                       memcmp(&before_gtt, &fixture.gtt_storage, sizeof(before_gtt)) == 0);
}

static void test_corrective_delayed_reservation_ownership(void)
{
    corrective_fixture_t fixture;
    tavrn_metadata_reservation_handle_t first;
    tavrn_metadata_reservation_handle_t second;
    tavrn_tc_metadata_snapshot_t metadata;
    tavrn_adva_t subject = adva(adva_c);

    STRUCTURAL("corrective-delayed-fixture", corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    first = tavrn_maintenance_metadata_reserve_delayed(&fixture.maintenance,
                                                        &subject, 1u, 40u);
    second = tavrn_maintenance_metadata_reserve_delayed(&fixture.maintenance,
                                                         &subject, 2u, 40u);
    tavrn_maintenance_metadata_release_delayed(&fixture.maintenance, first);
    CHECK("META-01", first != 0u && second != 0u && first != second &&
                       tavrn_maintenance_tc_metadata_snapshot(&fixture.maintenance.tc_metadata,
                                                               &metadata) ==
                           TAVRN_TC_METADATA_OK && metadata.candidate_count == 1u &&
                       fixture.maintenance.tc_metadata.slots[
                           second & TAVRN_METADATA_RESERVATION_SLOT_MASK].valid != 0u &&
                       fixture.maintenance.tc_metadata.slots[
                           second & TAVRN_METADATA_RESERVATION_SLOT_MASK].targeted_context_index ==
                            2u);
}

static void test_corrective_stale_delayed_release(void)
{
    corrective_fixture_t fixture;
    tavrn_tc_metadata_snapshot_t metadata;
    tavrn_adva_t first_subject = adva(adva_c);
    tavrn_adva_t second_subject = adva(adva_d);
    tavrn_adva_t recycled_subject = adva(adva_e);
    tavrn_metadata_reservation_handle_t first;
    tavrn_metadata_reservation_handle_t second;
    tavrn_metadata_reservation_handle_t recycled;

    STRUCTURAL("corrective-stale-delayed-fixture", corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    first = tavrn_maintenance_metadata_reserve_delayed(&fixture.maintenance,
                                                        &first_subject, 1u, 40u);
    second = tavrn_maintenance_metadata_reserve_delayed(&fixture.maintenance,
                                                         &second_subject, 2u, 40u);
    tavrn_maintenance_metadata_release_delayed(&fixture.maintenance, first);
    recycled = tavrn_maintenance_metadata_reserve_delayed(&fixture.maintenance,
                                                           &recycled_subject, 3u, 41u);
    tavrn_maintenance_metadata_release_delayed(&fixture.maintenance, first);
    CHECK("META-06", first != 0u && second != 0u && recycled != first &&
                       (recycled & TAVRN_METADATA_RESERVATION_SLOT_MASK) ==
                           (first & TAVRN_METADATA_RESERVATION_SLOT_MASK) &&
                        tavrn_maintenance_tc_metadata_snapshot(&fixture.maintenance.tc_metadata,
                                                                &metadata) ==
                            TAVRN_TC_METADATA_OK && metadata.candidate_count == 2u &&
                        fixture.maintenance.tc_metadata.slots[
                            recycled & TAVRN_METADATA_RESERVATION_SLOT_MASK].valid != 0u &&
                        fixture.maintenance.tc_metadata.slots[
                            recycled & TAVRN_METADATA_RESERVATION_SLOT_MASK].targeted_context_index ==
                            3u &&
                        fixture.maintenance.tc_metadata.slots[
                            second & TAVRN_METADATA_RESERVATION_SLOT_MASK].valid != 0u &&
                        fixture.maintenance.tc_metadata.slots[
                            second & TAVRN_METADATA_RESERVATION_SLOT_MASK].targeted_context_index ==
                            2u);
}

static void test_corrective_soft_expiry_and_busy_retention(void)
{
    corrective_fixture_t fixture;
    tavrn_maintenance_expiry_sweep_snapshot_t sweep;
    tavrn_validated_control_t base;

    STRUCTURAL("corrective-soft-fixture", corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    STRUCTURAL("corrective-soft-subject",
               corrective_observe(&fixture, adva_e, 9u, 1u,
                                   TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, 30u));
    if (structural_failures != 0u) return;
    memset(&sweep, 0, sizeof(sweep));
    (void)tavrn_maintenance_sweep_expiry(&fixture.maintenance, 30u, &sweep);
    memset(&sweep, 0, sizeof(sweep));
    (void)tavrn_maintenance_sweep_expiry(&fixture.maintenance, 41u, &sweep);
    base = route_control(TAVRN_METADATA_FRAME_RREQ8, 0u);
    CHECK("MAINT-01", sweep.soft_selected_count != 0u &&
                       corrective_soft_request_exists(&fixture.maintenance.tc_metadata, adva_e) &&
                       corrective_busy_retains_soft_request(&fixture, &base, 41u));
}

static void test_corrective_ordinary_direct_timeout(void)
{
    corrective_fixture_t fixture;
    tavrn_full_maintenance_binding_input_t input;
    tavrn_full_maintenance_binding_result_t result;
    tavrn_gtt_expiry_snapshot_t subject;

    STRUCTURAL("corrective-direct-timeout-fixture", corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    STRUCTURAL("corrective-direct-timeout-subject",
               corrective_observe(&fixture, adva_f, 10u, 1u,
                                   TAVRN_GTT_PROVENANCE_DIRECT_HELLO, 21u) &&
               corrective_set_hard_deadline(&fixture, adva_f, 121u));
    if (structural_failures != 0u) return;
    memset(&input, 0, sizeof(input));
    input.router = &fixture.router;
    input.mentorship = &fixture.mentorship;
    input.maintenance = &fixture.maintenance;
    memset(&result, 0, sizeof(result));
    (void)tavrn_full_maintenance_binding_tick(input, 21u, &result);
    memset(&result, 0, sizeof(result));
    CHECK("MAINT-08", tavrn_full_maintenance_binding_tick(input, 121u, &result) ==
                           TAVRN_FULL_MAINTENANCE_BINDING_OK &&
                       corrective_gtt_snapshot_for(&fixture, adva_f, 121u, &subject) &&
                       subject.freshness == TAVRN_GTT_FRESHNESS_DEPARTED &&
                       fixture.maintenance.tc_metadata.origin_pending.valid != 0u &&
                       fixture.maintenance.tc_metadata.origin_pending.event ==
                           TAVRN_TC_EVENT_LEAVE &&
                       memcmp(fixture.maintenance.tc_metadata.origin_pending.subject.bytes,
                              adva_f, TAVRN_ADVA_LEN) == 0);
}

static void test_corrective_composed_tc_owner(void)
{
    corrective_fixture_t fixture;
    tavrn_gtt_expiry_snapshot_t outer;
    tavrn_gtt_expiry_snapshot_t subject;
    tavrn_mentorship_event_trace_t trace;
    ble_mesh_sched_event_t event;
    uint8_t pdu[24];

    STRUCTURAL("corrective-composed-owner-fixture", corrective_fixture_init(&fixture));
    if (structural_failures != 0u) return;
    make_tc(pdu, adva_b, adva_c, 11u, 15u, 0u, TAVRN_TC_EVENT_JOIN);
    memset(&trace, 0, sizeof(trace));
    CHECK("BUILD-01", corrective_wrap_tc_event(pdu, adva_b, &event) &&
                       tavrn_mentorship_handle_scheduler_event(&fixture.mentorship, &event,
                                                               50u, &trace) ==
                           TAVRN_MENTORSHIP_OK &&
                       fixture.maintenance.tc_metadata.relay_pending.valid != 0u &&
                       fixture.mentorship.join_obligations[0].valid == 0u &&
                       corrective_gtt_snapshot_for(&fixture, adva_b, 50u, &outer) &&
                       outer.direct != 0u && outer.hop_count == 1u &&
                       outer.serial_present != 0u && outer.serial == 1u &&
                       outer.last_direct_evidence_ms == 50u &&
                       corrective_gtt_snapshot_for(&fixture, adva_c, 50u, &subject) &&
                       subject.direct == 0u && subject.serial_present == 0u);

    make_tc(pdu, adva_b, adva_c, 12u, 15u, 0u, TAVRN_TC_EVENT_JOIN);
    pdu[21] = 0xffu;
    CHECK("GTT-03", corrective_wrap_tc_event(pdu, adva_e, &event) &&
                       tavrn_mentorship_handle_scheduler_event(&fixture.mentorship, &event,
                                                               51u, &trace) ==
                           TAVRN_MENTORSHIP_OK &&
                       !corrective_gtt_snapshot_for(&fixture, adva_e, 51u, &outer));
}

static void test_corrective_production_composition(void)
{
    test_corrective_tc_serial_domain();
    if (structural_failures == 0u) test_corrective_local_leave_fifo();
    if (structural_failures == 0u) test_corrective_relay_fifo();
    if (structural_failures == 0u) test_corrective_tc_admission_consumes_evicted_data();
    if (structural_failures == 0u) test_corrective_obligation_capacity_and_hop_boundary();
    if (structural_failures == 0u) test_corrective_retry_exhausted_leave_overflow();
    if (structural_failures == 0u) test_corrective_join_leave_sequence_arbitration();
    if (structural_failures == 0u) test_corrective_local_tc_preempts_rfi();
    if (structural_failures == 0u) test_corrective_local_tc_port_is_optional();
    if (structural_failures == 0u) test_corrective_received_tc_preemption_filter();
    if (structural_failures == 0u) test_corrective_metadata_answer_merge();
    if (structural_failures == 0u) test_corrective_metadata_transaction_retention();
    if (structural_failures == 0u) test_corrective_decorated_control_pass_through();
    if (structural_failures == 0u) test_corrective_metadata_multi_entry_atomic_failure();
    if (structural_failures == 0u) test_corrective_tombstone_metadata_rejection();
    if (structural_failures == 0u) test_corrective_delayed_reservation_ownership();
    if (structural_failures == 0u) test_corrective_stale_delayed_release();
    if (structural_failures == 0u) test_corrective_soft_expiry_and_busy_retention();
    if (structural_failures == 0u) test_corrective_soft_expiry_cooldown();
    if (structural_failures == 0u) test_corrective_ordinary_direct_timeout();
    if (structural_failures == 0u) test_corrective_composed_tc_owner();
}

#endif

int main(void)
{
    verify_existing_join_and_tc_codec();
    if (structural_failures == 0u) test_confirmed_origin_and_shared_sequence();
    if (structural_failures == 0u) test_tc_receive_relay_and_atomicity();
    if (structural_failures == 0u) test_metadata_pool_selection_and_capacity();
#if !defined(TAVRN_PHASE5_TC_METADATA_RED_MODE)
    if (structural_failures == 0u) test_metadata_cooldown_semantics();
#endif
    if (structural_failures == 0u) test_metadata_attribution_merge_and_atomic_reject();
#if defined(TAVRN_PHASE5_TC_METADATA_CORRECTIVE_MODE)
    if (structural_failures == 0u) test_corrective_production_composition();
#endif
    if (structural_failures != 0u) {
        printf("tavrn_phase5_tc_metadata structural failures: %u\n", structural_failures);
        return 2;
    }
    if (failures != 0u) {
        printf("tavrn_phase5_tc_metadata tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_phase5_tc_metadata tests passed\n");
    return 0;
}
