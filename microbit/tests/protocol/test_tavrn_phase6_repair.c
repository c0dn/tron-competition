#include "tavrn_phase6_repair_contract.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FAST_PATH_DISCOVERY_MS 600u
#define FAST_REPAIR_TIMEOUT_MS 1700u
#define REPAIR_COOLDOWN_MS     1000u
#define NET_DIAMETER           15u

static unsigned int failures;
static const char *reported[9];
static unsigned int reported_count;

static const uint8_t adva_a[TAVRN_ADVA_LEN] =
    { 0x18u, 0x42u, 0xdeu, 0x52u, 0x4au, 0xddu };
static const uint8_t adva_h[TAVRN_ADVA_LEN] =
    { 0xdcu, 0x4bu, 0x0au, 0x06u, 0x03u, 0xf8u };
static const uint8_t adva_d[TAVRN_ADVA_LEN] =
    { 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0xc1u };
static const uint8_t adva_e[TAVRN_ADVA_LEN] =
    { 0x34u, 0x56u, 0x78u, 0x9au, 0xbcu, 0xc2u };

static int first_for(const char *requirement)
{
    unsigned int index;

    for (index = 0u; index < reported_count; index++) {
        if (strcmp(reported[index], requirement) == 0) {
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

#define REACHED(name) printf("REACHED %s\n", (name))

static tavrn_logical_id_t sid16(uint16_t value)
{
    tavrn_logical_id_t id;

    id.width = TAVRN_IDENTITY_SID16;
    id.value = value;
    return id;
}

static tavrn_direct_peer_t peer(const uint8_t bytes[TAVRN_ADVA_LEN])
{
    tavrn_direct_peer_t result;

    memset(&result, 0, sizeof(result));
    result.logical_id = sid16((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
    memcpy(result.adva.bytes, bytes, TAVRN_ADVA_LEN);
    return result;
}

typedef struct repair_ops_fake {
    aodv_failure_status_t begin_status;
    aodv_failure_status_t finish_status;
    aodv_single_rreq_status_t create_status;
    tavrn_repair_high_token_status_t reserve_status;
    tavrn_repair_high_token_status_t release_status;
    tavrn_repair_failed_hop_verification_status_t failed_hop_verification_status;
    uint16_t next_request_id;
    uint16_t next_high_token;
    uint32_t begin_calls;
    uint32_t finish_calls;
    uint32_t create_calls;
    uint32_t reserve_calls;
    uint32_t release_calls;
    uint32_t failed_hop_verification_calls;
    tavrn_direct_peer_t last_failed_hop;
    tavrn_logical_id_t last_destination;
    tavrn_repair_rreq_purpose_t last_purpose;
    uint16_t last_high_token;
    aodv_deferred_rerr_decision_t last_finish_decision;
} repair_ops_fake_t;

static void repair_ops_fake_init(repair_ops_fake_t *fake)
{
    memset(fake, 0, sizeof(*fake));
    fake->begin_status = AODV_FAILURE_OK;
    fake->finish_status = AODV_FAILURE_OK;
    fake->create_status = AODV_SINGLE_RREQ_OK;
    fake->reserve_status = TAVRN_REPAIR_HIGH_TOKEN_OK;
    fake->release_status = TAVRN_REPAIR_HIGH_TOKEN_OK;
    fake->failed_hop_verification_status = TAVRN_REPAIR_FAILED_HOP_VERIFICATION_OK;
    fake->next_request_id = 0x1200u;
    fake->next_high_token = 0x8000u;
}

static aodv_failure_status_t fake_begin_deferred_rerr(
    void *context, const tavrn_direct_peer_t *failed_next_hop,
    const tavrn_logical_id_t *destination, uint32_t now_ms)
{
    repair_ops_fake_t *fake = context;

    (void)now_ms;
    fake->begin_calls++;
    if (failed_next_hop != NULL) {
        fake->last_failed_hop = *failed_next_hop;
    }
    if (destination != NULL) {
        fake->last_destination = *destination;
    }
    return fake->begin_status;
}

static aodv_failure_status_t fake_finish_deferred_rerr(
    void *context, const tavrn_logical_id_t *destination,
    aodv_deferred_rerr_decision_t decision, uint32_t now_ms)
{
    repair_ops_fake_t *fake = context;

    (void)now_ms;
    fake->finish_calls++;
    fake->last_finish_decision = decision;
    if (destination != NULL) {
        fake->last_destination = *destination;
    }
    return fake->finish_status;
}

static aodv_single_rreq_status_t fake_create_single_rreq(
    void *context, const tavrn_logical_id_t *destination, uint8_t scope,
    uint32_t now_ms, aodv_action_t *action_out)
{
    repair_ops_fake_t *fake = context;
    aodv_rreq_attempt_t attempt;

    fake->create_calls++;
    if (destination != NULL) {
        fake->last_destination = *destination;
    }
    if (fake->create_status != AODV_SINGLE_RREQ_OK) {
        return fake->create_status;
    }
    if (action_out == NULL || destination == NULL) {
        return AODV_SINGLE_RREQ_INVALID;
    }
    memset(&attempt, 0, sizeof(attempt));
    attempt.discovery_correlation = fake->create_calls;
    attempt.origin = sid16((uint16_t)adva_a[0] | ((uint16_t)adva_a[1] << 8));
    attempt.destination = *destination;
    attempt.request_id = fake->next_request_id++;
    attempt.initial_scope = scope;
    attempt.current_scope = scope;
    /* aodv_core_create_single_rreq() emits its one-shot attempt at ordinal 0. */
    attempt.ring_ordinal = 0u;
    memset(action_out, 0, sizeof(*action_out));
    action_out->type = AODV_ACTION_SEND_RREQ;
    action_out->detail.control.rreq_attempt_present = AODV_RREQ_ATTEMPT_PRESENT;
    action_out->detail.control.rreq_attempt = attempt;
    (void)now_ms;
    return AODV_SINGLE_RREQ_OK;
}

static tavrn_repair_high_token_status_t fake_reserve_high_token(
    void *context, tavrn_repair_high_token_owner_t owner,
    tavrn_repair_rreq_purpose_t purpose, uint16_t *token_out)
{
    repair_ops_fake_t *fake = context;

    fake->reserve_calls++;
    fake->last_purpose = purpose;
    if (owner != TAVRN_REPAIR_HIGH_TOKEN_OWNER_REPAIR ||
        fake->reserve_status != TAVRN_REPAIR_HIGH_TOKEN_OK) {
        return fake->reserve_status;
    }
    if (token_out == NULL) {
        return TAVRN_REPAIR_HIGH_TOKEN_INVALID;
    }
    if (fake->next_high_token < 0x8000u) {
        fake->next_high_token = 0x8000u;
    }
    *token_out = fake->next_high_token++;
    fake->last_high_token = *token_out;
    return TAVRN_REPAIR_HIGH_TOKEN_OK;
}

static tavrn_repair_high_token_status_t fake_release_high_token(
    void *context, tavrn_repair_high_token_owner_t owner,
    tavrn_repair_rreq_purpose_t purpose, uint16_t token)
{
    repair_ops_fake_t *fake = context;

    fake->release_calls++;
    fake->last_purpose = purpose;
    fake->last_high_token = token;
    if (owner != TAVRN_REPAIR_HIGH_TOKEN_OWNER_REPAIR) {
        return TAVRN_REPAIR_HIGH_TOKEN_INVALID;
    }
    return fake->release_status;
}

static tavrn_repair_failed_hop_verification_status_t
fake_schedule_held_failed_hop_verification(
    void *context, const tavrn_direct_peer_t *failed_next_hop, uint32_t now_ms)
{
    repair_ops_fake_t *fake = context;

    (void)now_ms;
    fake->failed_hop_verification_calls++;
    if (failed_next_hop != NULL) {
        fake->last_failed_hop = *failed_next_hop;
    }
    return fake->failed_hop_verification_status;
}

static tavrn_repair_dependency_ops_t repair_operations(repair_ops_fake_t *fake)
{
    tavrn_repair_dependency_ops_t operations;

    memset(&operations, 0, sizeof(operations));
    operations.context = fake;
    operations.begin_deferred_rerr = fake_begin_deferred_rerr;
    operations.finish_deferred_rerr = fake_finish_deferred_rerr;
    operations.create_single_rreq = fake_create_single_rreq;
    operations.reserve_high_token = fake_reserve_high_token;
    operations.release_high_token = fake_release_high_token;
    operations.schedule_held_failed_hop_verification =
        fake_schedule_held_failed_hop_verification;
    return operations;
}

static tavrn_repair_config_t repair_config(
    uint8_t enabled, const tavrn_repair_dependency_ops_t *operations)
{
    tavrn_repair_config_t config;

    memset(&config, 0, sizeof(config));
    config.enabled = enabled;
    config.net_diameter = NET_DIAMETER;
    config.data_capacity = TAVRN_REPAIR_BUFFER_CAPACITY;
    config.path_discovery_ms = FAST_PATH_DISCOVERY_MS;
    config.repair_timeout_ms = FAST_REPAIR_TIMEOUT_MS;
    config.repair_cooldown_ms = REPAIR_COOLDOWN_MS;
    config.operations = operations;
    return config;
}

static tavrn_owned_data_event_t owned_data(uint16_t sequence, uint16_t destination)
{
    tavrn_owned_data_event_t event;

    memset(&event, 0, sizeof(event));
    event.next_hop = peer(adva_h);
    event.data.origin = sid16((uint16_t)adva_a[0] | ((uint16_t)adva_a[1] << 8));
    event.data.final_destination = sid16(destination);
    event.data.data_seq = sequence;
    event.data.ttl = 7u;
    event.data.hops = 2u;
    event.data.app_kind = 0x7fu;
    event.data.app_source = 0x31u;
    event.data.app_len = 3u;
    event.data.app_bytes[0] = 0xa1u;
    event.data.app_bytes[1] = 0xb2u;
    event.data.app_bytes[2] = 0xc3u;
    event.data.ownership = TAVRN_DATA_TRANSIT;
    event.requested_channel_mask = 0x07u;
    event.completed_channel_mask = 0x07u;
    event.attempt_count = 3u;
    event.first_tx_ms = 10u;
    event.last_tx_ms = 510u;
    event.final_deadline_ms = 760u;
    return event;
}

static tavrn_repair_retry_input_t repair_input(
    const tavrn_owned_data_event_t *owned, uint8_t smart_ttl_scope,
    const tavrn_link_data_t *existing_or_null)
{
    tavrn_repair_retry_input_t input;

    memset(&input, 0, sizeof(input));
    input.trigger = TAVRN_LINK_EVENT_RETRY_EXHAUSTED;
    input.owned = *owned;
    input.smart_ttl_scope = smart_ttl_scope;
    if (existing_or_null != NULL) {
        input.same_destination_link_custody.count = 1u;
        input.same_destination_link_custody.records[0].next_hop = owned->next_hop;
        input.same_destination_link_custody.records[0].data = *existing_or_null;
    }
    return input;
}

static tavrn_repair_link_custody_terminal_input_t custody_data(
    const tavrn_link_data_t *data)
{
    tavrn_repair_link_custody_terminal_input_t input;

    memset(&input, 0, sizeof(input));
    input.next_hop = peer(adva_h);
    input.data = *data;
    return input;
}

static tavrn_repair_link_custody_terminal_input_t custody_retry(
    const tavrn_owned_data_event_t *owned)
{
    tavrn_repair_link_custody_terminal_input_t input;

    memset(&input, 0, sizeof(input));
    input.next_hop = owned->next_hop;
    input.data = owned->data;
    input.owned = *owned;
    input.owned_present = 1u;
    return input;
}

static tavrn_repair_rreq_enqueue_t enqueue_for(
    const tavrn_repair_action_t *action, tavrn_repair_rreq_enqueue_status_t status)
{
    tavrn_repair_rreq_enqueue_t enqueue;

    memset(&enqueue, 0, sizeof(enqueue));
    enqueue.status = status;
    enqueue.purpose = action->purpose;
    enqueue.token_owner = action->token_owner;
    enqueue.rreq_attempt = action->rreq_attempt;
    enqueue.token = action->token;
    return enqueue;
}

static tavrn_repair_completion_t rreq_done(
    const tavrn_repair_action_t *action, const tavrn_repair_rreq_enqueue_t *enqueue)
{
    tavrn_repair_completion_t completion;

    memset(&completion, 0, sizeof(completion));
    completion.kind = TAVRN_REPAIR_COMPLETION_RREQ_TX_DONE;
    completion.purpose = action->purpose;
    completion.token_owner = action->token_owner;
    completion.rreq_attempt = action->rreq_attempt;
    completion.token = enqueue->token;
    completion.completed_channel_mask = 0x01u;
    return completion;
}

static tavrn_repair_completion_t data_completion(
    const tavrn_repair_action_t *action, tavrn_repair_completion_kind_t kind)
{
    tavrn_repair_completion_t completion;

    memset(&completion, 0, sizeof(completion));
    completion.kind = kind;
    completion.token = action->token;
    completion.buffer_index = action->buffer_index;
    return completion;
}

static tavrn_repair_rrep_completion_t matching_rrep(
    const tavrn_repair_action_t *action)
{
    tavrn_repair_rrep_completion_t completion;

    memset(&completion, 0, sizeof(completion));
    completion.purpose = action->purpose;
    completion.token_owner = action->token_owner;
    completion.rreq_attempt = action->rreq_attempt;
    completion.token = action->token;
    completion.transmitter = peer(adva_d);
    completion.installed_next_hop = peer(adva_d);
    completion.router_admitted = 1u;
    completion.route_installed = 1u;
    return completion;
}

static void test_strict_config_and_capacity(void)
{
    repair_ops_fake_t fake;
    tavrn_repair_dependency_ops_t operations;
    tavrn_repair_t repair;
    tavrn_repair_config_t config;
    tavrn_repair_config_t invalid;
    tavrn_repair_snapshot_t snapshot;
    tavrn_repair_status_t init_status;
    tavrn_repair_status_t invalid_status;
    tavrn_repair_status_t snapshot_status;

    REACHED("strict-config-capacity");
    repair_ops_fake_init(&fake);
    operations = repair_operations(&fake);
    config = repair_config(1u, &operations);
    memset(&repair, 0, sizeof(repair));
    memset(&snapshot, 0, sizeof(snapshot));
    init_status = phase6_repair_init(&repair, &config);
    invalid = config;
    invalid.operations = NULL;
    invalid_status = phase6_repair_init(&repair, &invalid);
    snapshot_status = phase6_repair_snapshot(&repair, &snapshot);

    CHECK("REPAIR-01", TAVRN_REPAIR_CONTEXT_CAPACITY == 1u &&
                         TAVRN_REPAIR_BUFFER_CAPACITY == 4u &&
                         config.data_capacity == TAVRN_REPAIR_BUFFER_CAPACITY &&
                         config.repair_timeout_ms ==
                             2u * config.path_discovery_ms + 500u &&
                         config.operations == &operations &&
                         init_status == TAVRN_REPAIR_OK &&
                         invalid_status == TAVRN_REPAIR_INVALID &&
                         snapshot_status == TAVRN_REPAIR_OK &&
                         snapshot.state == TAVRN_REPAIR_STATE_IDLE &&
                         snapshot.buffered_count == 0u &&
                         fake.begin_calls == 0u && fake.create_calls == 0u);
}

static void test_owned_transit_and_negative_triggers(void)
{
    repair_ops_fake_t fake;
    tavrn_repair_dependency_ops_t operations;
    tavrn_repair_t repair;
    tavrn_owned_data_event_t owned = owned_data(0x8101u, peer(adva_d).logical_id.value);
    tavrn_repair_retry_input_t input = repair_input(&owned, 4u, NULL);
    tavrn_repair_setup_result_t owned_result;
    tavrn_repair_setup_result_t negative_result;
    tavrn_repair_status_t init_status;
    tavrn_repair_status_t owned_status;
    tavrn_repair_status_t local_status;
    tavrn_repair_status_t busy_status;
    tavrn_repair_status_t control_status;

    REACHED("owned-transit-and-negative-triggers");
    repair_ops_fake_init(&fake);
    operations = repair_operations(&fake);
    memset(&repair, 0, sizeof(repair));
    memset(&owned_result, 0, sizeof(owned_result));
    memset(&negative_result, 0, sizeof(negative_result));
    init_status = phase6_repair_init(&repair, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS, &operations
    });
    owned_status = phase6_repair_retry_exhausted(&repair, &input, 100u, &owned_result);
    input.owned.data.ownership = TAVRN_DATA_ORIGINATED;
    local_status = phase6_repair_retry_exhausted(&repair, &input, 101u, &negative_result);
    input.owned.data.ownership = TAVRN_DATA_TRANSIT;
    input.trigger = TAVRN_LINK_EVENT_CUSTODY_BUSY_EXPIRED;
    busy_status = phase6_repair_retry_exhausted(&repair, &input, 102u, &negative_result);
    input.trigger = TAVRN_LINK_EVENT_RX_CONTROL;
    control_status = phase6_repair_retry_exhausted(&repair, &input, 103u, &negative_result);

    CHECK("ACK-006", input.owned.attempt_count == 3u &&
                     input.owned.completed_channel_mask != 0u &&
                     input.owned.data.app_len == 3u &&
                     owned_status == TAVRN_REPAIR_OK &&
                     owned_result.disposition == TAVRN_REPAIR_RETRY_OWNED &&
                     owned_result.data_copied == 1u &&
                     fake.begin_calls == 1u);
    CHECK("REPAIR-02", init_status == TAVRN_REPAIR_OK &&
                        local_status == TAVRN_REPAIR_OK &&
                        busy_status == TAVRN_REPAIR_OK &&
                        control_status == TAVRN_REPAIR_OK &&
                        negative_result.disposition == TAVRN_REPAIR_RETRY_DECLINED &&
                        negative_result.state == TAVRN_REPAIR_SETUP_NORMAL_FAILURE &&
                        negative_result.failure_mode == AODV_LINK_FAILURE_IMMEDIATE_RERR &&
                        fake.begin_calls == 1u && fake.create_calls == 0u);
}

static void test_atomic_setup_and_deferred_obligations(void)
{
    repair_ops_fake_t fake;
    repair_ops_fake_t busy_fake;
    repair_ops_fake_t invalid_fake;
    repair_ops_fake_t malformed_fake;
    tavrn_repair_dependency_ops_t operations;
    tavrn_repair_dependency_ops_t busy_operations;
    tavrn_repair_dependency_ops_t invalid_operations;
    tavrn_repair_t repair;
    tavrn_repair_t busy_repair;
    tavrn_repair_t invalid_repair;
    tavrn_repair_t malformed_repair;
    tavrn_owned_data_event_t owned = owned_data(0x8102u, peer(adva_d).logical_id.value);
    tavrn_link_data_t existing = owned.data;
    tavrn_repair_retry_input_t input;
    tavrn_repair_setup_result_t result;
    tavrn_repair_setup_result_t blocked;
    tavrn_repair_setup_result_t malformed_result;
    tavrn_repair_snapshot_t snapshot;
    tavrn_repair_snapshot_t malformed_snapshot;
    tavrn_repair_status_t statuses[6];
    tavrn_repair_status_t malformed_statuses[4];

    REACHED("atomic-setup-held-obligations");
    existing.data_seq++;
    input = repair_input(&owned, 4u, &existing);
    repair_ops_fake_init(&fake);
    repair_ops_fake_init(&busy_fake);
    repair_ops_fake_init(&invalid_fake);
    repair_ops_fake_init(&malformed_fake);
    busy_fake.begin_status = AODV_FAILURE_BUSY;
    invalid_fake.begin_status = AODV_FAILURE_INVALID;
    operations = repair_operations(&fake);
    busy_operations = repair_operations(&busy_fake);
    invalid_operations = repair_operations(&invalid_fake);
    {
        tavrn_repair_dependency_ops_t malformed_operations =
            repair_operations(&malformed_fake);

        memset(&malformed_repair, 0, sizeof(malformed_repair));
        memset(&malformed_result, 0, sizeof(malformed_result));
        memset(&malformed_snapshot, 0, sizeof(malformed_snapshot));
        malformed_statuses[0] = phase6_repair_init(&malformed_repair,
            &(tavrn_repair_config_t){
                1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
                FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS,
                &malformed_operations
            });
        input.smart_ttl_scope = (uint8_t)(NET_DIAMETER + 1u);
        malformed_statuses[1] = phase6_repair_retry_exhausted(
            &malformed_repair, &input, 199u, &malformed_result);
        input.smart_ttl_scope = 4u;
        input.same_destination_link_custody.count = 2u;
        input.same_destination_link_custody.records[0].data = existing;
        input.same_destination_link_custody.records[1].next_hop = peer(adva_e);
        input.same_destination_link_custody.records[1].data = existing;
        malformed_statuses[2] = phase6_repair_retry_exhausted(
            &malformed_repair, &input, 199u, &malformed_result);
        input.same_destination_link_custody.count = 1u;
        input.same_destination_link_custody.records[0].data = owned.data;
        malformed_statuses[3] = phase6_repair_retry_exhausted(
            &malformed_repair, &input, 199u, &malformed_result);
        (void)phase6_repair_snapshot(&malformed_repair, &malformed_snapshot);
        input = repair_input(&owned, 4u, &existing);
    }
    memset(&repair, 0, sizeof(repair));
    memset(&busy_repair, 0, sizeof(busy_repair));
    memset(&invalid_repair, 0, sizeof(invalid_repair));
    memset(&result, 0, sizeof(result));
    memset(&blocked, 0, sizeof(blocked));
    memset(&snapshot, 0, sizeof(snapshot));
    statuses[0] = phase6_repair_init(&repair, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS, &operations
    });
    statuses[1] = phase6_repair_retry_exhausted(&repair, &input, 200u, &result);
    statuses[2] = phase6_repair_snapshot(&repair, &snapshot);
    statuses[3] = phase6_repair_init(&busy_repair, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS, &busy_operations
    });
    statuses[4] = phase6_repair_retry_exhausted(&busy_repair, &input, 201u, &blocked);
    statuses[5] = phase6_repair_init(&invalid_repair, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS, &invalid_operations
    });
    (void)phase6_repair_retry_exhausted(&invalid_repair, &input, 202u, &blocked);

    CHECK("AODV-08", statuses[0] == TAVRN_REPAIR_OK &&
                       statuses[1] == TAVRN_REPAIR_OK &&
                       statuses[2] == TAVRN_REPAIR_OK &&
                       statuses[3] == TAVRN_REPAIR_OK &&
                       statuses[4] == TAVRN_REPAIR_OK &&
                       statuses[5] == TAVRN_REPAIR_OK &&
                       result.state == TAVRN_REPAIR_SETUP_ATOMIC_OWNED &&
                       result.disposition == TAVRN_REPAIR_RETRY_OWNED &&
                       result.data_copied == 1u &&
                       result.deferred_rerr_started == 1u &&
                        result.held_failed_hop_verification_created == 1u &&
                       result.failure_mode ==
                           AODV_LINK_FAILURE_DEFER_REPAIR_DESTINATION_RERR &&
                       result.deferred_rerr_status == AODV_FAILURE_OK &&
                       result.reserved_link_slots == 1u &&
                       fake.begin_calls == 1u &&
                       memcmp(fake.last_failed_hop.adva.bytes, adva_h, TAVRN_ADVA_LEN) == 0 &&
                       fake.last_destination.value == owned.data.final_destination.value &&
                        fake.create_calls == 0u && fake.failed_hop_verification_calls == 0u &&
                       snapshot.deferred_rerr_active == 1u &&
                        snapshot.failed_hop_verification_state ==
                            TAVRN_REPAIR_FAILED_HOP_VERIFICATION_HELD &&
                        busy_fake.begin_calls == 1u && busy_fake.create_calls == 0u &&
                        busy_fake.reserve_calls == 0u &&
                            busy_fake.failed_hop_verification_calls == 0u &&
                        invalid_fake.begin_calls == 1u && invalid_fake.create_calls == 0u &&
                         invalid_fake.reserve_calls == 0u &&
                             invalid_fake.failed_hop_verification_calls == 0u &&
                        blocked.disposition == TAVRN_REPAIR_RETRY_DECLINED &&
                        blocked.data_copied == 0u && blocked.deferred_rerr_started == 0u &&
                        malformed_statuses[0] == TAVRN_REPAIR_OK &&
                        malformed_statuses[1] == TAVRN_REPAIR_INVALID &&
                        malformed_statuses[2] == TAVRN_REPAIR_INVALID &&
                        malformed_statuses[3] == TAVRN_REPAIR_INVALID &&
                        malformed_result.disposition == TAVRN_REPAIR_RETRY_INVALID &&
                        malformed_snapshot.state == TAVRN_REPAIR_STATE_IDLE &&
                        malformed_snapshot.buffered_count == 0u &&
                        malformed_fake.begin_calls == 0u);
}

static void test_same_destination_capacity_and_reservations(void)
{
    repair_ops_fake_t fake;
    repair_ops_fake_t converted_fake;
    tavrn_repair_dependency_ops_t operations;
    tavrn_repair_dependency_ops_t converted_operations;
    tavrn_repair_t repair;
    tavrn_repair_t converted_repair;
    tavrn_owned_data_event_t owned = owned_data(0x8103u, peer(adva_d).logical_id.value);
    tavrn_link_data_t existing = owned.data;
    tavrn_link_data_t unrelated;
    tavrn_link_data_t candidate;
    tavrn_link_data_t different;
    tavrn_owned_data_event_t existing_terminal;
    tavrn_repair_retry_input_t input;
    tavrn_repair_setup_result_t setup;
    tavrn_repair_candidate_result_t first;
    tavrn_repair_candidate_result_t rolled_back;
    tavrn_repair_candidate_result_t third;
    tavrn_repair_candidate_result_t fourth;
    tavrn_repair_candidate_result_t fifth;
    tavrn_repair_candidate_result_t other;
    tavrn_repair_link_custody_result_t ignored_result;
    tavrn_repair_link_custody_result_t released_result;
    tavrn_repair_link_custody_result_t converted_result;
    tavrn_repair_status_t statuses[15];

    REACHED("same-destination-reserve-commit-rollback");
    existing.data_seq++;
    existing_terminal = owned;
    existing_terminal.data = existing;
    unrelated = existing;
    unrelated.data_seq++;
    candidate = owned.data;
    different = owned.data;
    input = repair_input(&owned, 4u, &existing);
    repair_ops_fake_init(&fake);
    repair_ops_fake_init(&converted_fake);
    operations = repair_operations(&fake);
    converted_operations = repair_operations(&converted_fake);
    memset(&repair, 0, sizeof(repair));
    memset(&converted_repair, 0, sizeof(converted_repair));
    memset(&setup, 0, sizeof(setup));
    memset(&first, 0, sizeof(first));
    memset(&rolled_back, 0, sizeof(rolled_back));
    memset(&third, 0, sizeof(third));
    memset(&fourth, 0, sizeof(fourth));
    memset(&fifth, 0, sizeof(fifth));
    memset(&other, 0, sizeof(other));
    memset(&ignored_result, 0, sizeof(ignored_result));
    memset(&released_result, 0, sizeof(released_result));
    memset(&converted_result, 0, sizeof(converted_result));
    statuses[0] = phase6_repair_init(&repair, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS, &operations
    });
    statuses[1] = phase6_repair_retry_exhausted(&repair, &input, 300u, &setup);
    statuses[2] = phase6_repair_observe_link_custody(
        &repair, &(tavrn_repair_link_custody_terminal_input_t){
            .next_hop = peer(adva_h),
            .data = unrelated
        }, TAVRN_REPAIR_LINK_CUSTODY_TRANSFERRED, 301u, &ignored_result);
    statuses[3] = phase6_repair_observe_link_custody(
        &repair, &(tavrn_repair_link_custody_terminal_input_t){
            .next_hop = peer(adva_h),
            .data = existing
        }, TAVRN_REPAIR_LINK_CUSTODY_TRANSFERRED, 302u, &released_result);
    candidate.data_seq += 3u;
    statuses[4] = phase6_repair_reserve_candidate(&repair, &candidate, 303u, &first);
    statuses[5] = phase6_repair_commit_candidate(&repair, &first.reservation, &candidate, 303u);
    candidate.data_seq++;
    statuses[6] = phase6_repair_reserve_candidate(&repair, &candidate, 304u, &rolled_back);
    statuses[7] = phase6_repair_rollback_candidate(&repair, &rolled_back.reservation, 304u);
    statuses[8] = phase6_repair_reserve_candidate(&repair, &candidate, 305u, &third);
    statuses[9] = phase6_repair_commit_candidate(&repair, &third.reservation, &candidate, 305u);
    candidate.data_seq++;
    statuses[10] = phase6_repair_reserve_candidate(&repair, &candidate, 306u, &fourth);
    statuses[11] = phase6_repair_commit_candidate(&repair, &fourth.reservation, &candidate, 306u);
    candidate.data_seq++;
    statuses[12] = phase6_repair_reserve_candidate(&repair, &candidate, 307u, &fifth);
    different.final_destination = peer(adva_e).logical_id;
    statuses[13] = phase6_repair_reserve_candidate(&repair, &different, 307u, &other);
    statuses[14] = phase6_repair_init(&converted_repair, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS,
        &converted_operations
    });
    (void)phase6_repair_retry_exhausted(&converted_repair, &input, 310u, &setup);
    (void)phase6_repair_observe_link_custody(
        &converted_repair, &(tavrn_repair_link_custody_terminal_input_t){
            .next_hop = peer(adva_h),
            .data = existing,
            .owned = existing_terminal,
            .owned_present = 1u
        }, TAVRN_REPAIR_LINK_CUSTODY_RETRY_EXHAUSTED,
        311u, &converted_result);

    CHECK("REPAIR-04", statuses[0] == TAVRN_REPAIR_OK &&
                        statuses[1] == TAVRN_REPAIR_OK &&
                        statuses[2] == TAVRN_REPAIR_IGNORED &&
                        statuses[3] == TAVRN_REPAIR_OK &&
                        statuses[4] == TAVRN_REPAIR_OK &&
                        statuses[5] == TAVRN_REPAIR_OK &&
                        statuses[6] == TAVRN_REPAIR_OK &&
                        statuses[7] == TAVRN_REPAIR_OK &&
                        statuses[8] == TAVRN_REPAIR_OK &&
                        statuses[9] == TAVRN_REPAIR_OK &&
                        statuses[10] == TAVRN_REPAIR_OK &&
                        statuses[11] == TAVRN_REPAIR_OK &&
                        statuses[12] == TAVRN_REPAIR_OK &&
                        statuses[13] == TAVRN_REPAIR_OK &&
                        statuses[14] == TAVRN_REPAIR_OK &&
                        input.same_destination_link_custody.count == 1u &&
                        memcmp(&input.same_destination_link_custody.records[0].data,
                               &existing, sizeof(existing)) == 0 &&
                        ignored_result.released_reservation == 0u &&
                        released_result.released_reservation == 1u &&
                        first.status == TAVRN_REPAIR_CANDIDATE_RESERVED &&
                        first.reservation.valid == 1u &&
                        first.reservation.token != TAVRN_REPAIR_RESERVATION_NONE &&
                        rolled_back.status == TAVRN_REPAIR_CANDIDATE_RESERVED &&
                        third.status == TAVRN_REPAIR_CANDIDATE_RESERVED &&
                        fourth.status == TAVRN_REPAIR_CANDIDATE_RESERVED &&
                        fifth.status == TAVRN_REPAIR_CANDIDATE_BUSY &&
                        other.status == TAVRN_REPAIR_CANDIDATE_NOT_APPLICABLE &&
                        converted_result.converted_to_buffer == 1u &&
                        converted_result.buffered_count == 2u &&
                        converted_result.reserved_link_slots == 0u);
}

static void test_exact_custody_next_hop_and_reconciliation(void)
{
    repair_ops_fake_t fake;
    repair_ops_fake_t reconciliation_fake;
    tavrn_repair_dependency_ops_t operations;
    tavrn_repair_dependency_ops_t reconciliation_operations;
    tavrn_repair_t repair;
    tavrn_repair_t reconciliation;
    tavrn_owned_data_event_t owned = owned_data(0x8110u, peer(adva_d).logical_id.value);
    tavrn_owned_data_event_t second_owned;
    tavrn_link_data_t first = owned.data;
    tavrn_link_data_t second = owned.data;
    tavrn_repair_retry_input_t input;
    tavrn_repair_setup_result_t setup;
    tavrn_repair_link_custody_terminal_input_t wrong_first;
    tavrn_repair_link_custody_terminal_input_t exact_first;
    tavrn_repair_link_custody_terminal_input_t exact_second_retry;
    tavrn_repair_link_custody_result_t wrong_result;
    tavrn_repair_link_custody_result_t released_result;
    tavrn_repair_link_custody_result_t converted_result;
    tavrn_repair_link_custody_snapshot_t current;
    tavrn_repair_link_custody_reconcile_result_t first_reconciliation;
    tavrn_repair_link_custody_reconcile_result_t final_reconciliation;
    tavrn_repair_action_t rreq;
    tavrn_repair_action_t flush;
    tavrn_repair_rreq_enqueue_t enqueue;
    tavrn_repair_completion_t completion;
    tavrn_repair_rrep_completion_t rrep;
    tavrn_repair_snapshot_t snapshot;
    tavrn_repair_status_t init_status;
    tavrn_repair_status_t retry_status;
    tavrn_repair_status_t wrong_status;
    tavrn_repair_status_t release_status;
    tavrn_repair_status_t convert_status;
    tavrn_repair_status_t malformed_retry_status;
    tavrn_repair_status_t reconciliation_init_status;
    tavrn_repair_status_t reconciliation_retry_status;
    tavrn_repair_status_t rreq_status;
    tavrn_repair_status_t enqueue_status;
    tavrn_repair_status_t done_status;
    tavrn_repair_status_t rrep_status;
    tavrn_repair_status_t flush_status;
    tavrn_repair_status_t flush_complete_status;
    tavrn_repair_status_t first_reconciliation_status;
    tavrn_repair_status_t final_reconciliation_status;

    first.data_seq++;
    second.data_seq += 2u;
    second_owned = owned;
    second_owned.data = second;
    repair_ops_fake_init(&fake);
    repair_ops_fake_init(&reconciliation_fake);
    operations = repair_operations(&fake);
    reconciliation_operations = repair_operations(&reconciliation_fake);
    input = repair_input(&owned, 4u, &first);
    input.same_destination_link_custody.count = 2u;
    input.same_destination_link_custody.records[1].next_hop = owned.next_hop;
    input.same_destination_link_custody.records[1].data = second;
    memset(&repair, 0, sizeof(repair));
    memset(&reconciliation, 0, sizeof(reconciliation));
    memset(&setup, 0, sizeof(setup));
    memset(&wrong_result, 0, sizeof(wrong_result));
    memset(&released_result, 0, sizeof(released_result));
    memset(&converted_result, 0, sizeof(converted_result));
    wrong_first = custody_data(&first);
    wrong_first.next_hop = peer(adva_e);
    exact_first = custody_data(&first);
    exact_second_retry = custody_retry(&second_owned);
    init_status = phase6_repair_init(&repair, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS, &operations
    });
    retry_status = phase6_repair_retry_exhausted(&repair, &input, 900u, &setup);
    wrong_status = phase6_repair_observe_link_custody(
        &repair, &wrong_first, TAVRN_REPAIR_LINK_CUSTODY_TRANSFERRED, 901u,
        &wrong_result);
    release_status = phase6_repair_observe_link_custody(
        &repair, &exact_first, TAVRN_REPAIR_LINK_CUSTODY_TRANSFERRED, 902u,
        &released_result);
    convert_status = phase6_repair_observe_link_custody(
        &repair, &exact_second_retry, TAVRN_REPAIR_LINK_CUSTODY_RETRY_EXHAUSTED,
        903u, &converted_result);
    exact_second_retry.next_hop = peer(adva_e);
    malformed_retry_status = phase6_repair_observe_link_custody(
        &repair, &exact_second_retry, TAVRN_REPAIR_LINK_CUSTODY_RETRY_EXHAUSTED,
        904u, &converted_result);

    reconciliation_init_status = phase6_repair_init(&reconciliation,
        &(tavrn_repair_config_t){
            1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
            FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS,
            &reconciliation_operations
        });
    reconciliation_retry_status = phase6_repair_retry_exhausted(
        &reconciliation, &input, 1000u, &setup);
    memset(&rreq, 0, sizeof(rreq));
    memset(&flush, 0, sizeof(flush));
    rreq_status = phase6_repair_owner_tick(&reconciliation, 1000u, &rreq);
    enqueue = enqueue_for(&rreq, TAVRN_REPAIR_RREQ_ENQUEUE_ADMITTED);
    enqueue_status = phase6_repair_rreq_enqueue(&reconciliation, &enqueue, 1000u);
    completion = rreq_done(&rreq, &enqueue);
    done_status = phase6_repair_complete(&reconciliation, &completion, 1001u, NULL);
    rrep = matching_rrep(&rreq);
    rrep_status = phase6_repair_receive_rrep(&reconciliation, &rrep, 1002u, NULL);
    flush_status = phase6_repair_owner_tick(&reconciliation, 1002u, &flush);
    completion = data_completion(&flush, TAVRN_REPAIR_COMPLETION_DATA_FLUSHED);
    flush_complete_status = phase6_repair_complete(&reconciliation, &completion,
                                                    1002u, NULL);
    memset(&current, 0, sizeof(current));
    current.count = 1u;
    current.records[0].next_hop = owned.next_hop;
    current.records[0].data = first;
    first_reconciliation_status = phase6_repair_reconcile_link_custody(
        &reconciliation, &current, 1003u, &first_reconciliation);
    memset(&current, 0, sizeof(current));
    final_reconciliation_status = phase6_repair_reconcile_link_custody(
        &reconciliation, &current, 1004u, &final_reconciliation);
    (void)phase6_repair_snapshot(&reconciliation, &snapshot);

    CHECK("REPAIR-04", init_status == TAVRN_REPAIR_OK &&
                         retry_status == TAVRN_REPAIR_OK &&
                         wrong_status == TAVRN_REPAIR_IGNORED &&
                         wrong_result.released_reservation == 0u &&
                         release_status == TAVRN_REPAIR_OK &&
                         released_result.released_reservation == 1u &&
                         convert_status == TAVRN_REPAIR_OK &&
                         converted_result.converted_to_buffer == 1u &&
                         malformed_retry_status == TAVRN_REPAIR_INVALID &&
                         reconciliation_init_status == TAVRN_REPAIR_OK &&
                         reconciliation_retry_status == TAVRN_REPAIR_OK &&
                         rreq_status == TAVRN_REPAIR_OK &&
                         enqueue_status == TAVRN_REPAIR_OK &&
                         done_status == TAVRN_REPAIR_OK &&
                         rrep_status == TAVRN_REPAIR_OK &&
                         flush_status == TAVRN_REPAIR_OK &&
                         flush.type == TAVRN_REPAIR_ACTION_FLUSH_DATA &&
                         flush_complete_status == TAVRN_REPAIR_OK &&
                         first_reconciliation_status == TAVRN_REPAIR_OK &&
                         first_reconciliation.released_count == 1u &&
                         first_reconciliation.buffered_count == 0u &&
                         first_reconciliation.reserved_link_slots == 1u &&
                         final_reconciliation_status == TAVRN_REPAIR_OK &&
                         final_reconciliation.released_count == 1u &&
                         final_reconciliation.buffered_count == 0u &&
                         final_reconciliation.reserved_link_slots == 0u &&
                         snapshot.state == TAVRN_REPAIR_STATE_COOLDOWN);
}

static void test_smart_ttl_full_scope_and_fresh_ids(void)
{
    repair_ops_fake_t fake;
    repair_ops_fake_t reserve_busy_fake;
    repair_ops_fake_t create_invalid_fake;
    repair_ops_fake_t release_busy_fake;
    repair_ops_fake_t local_failure_fake;
    tavrn_repair_dependency_ops_t operations;
    tavrn_repair_dependency_ops_t reserve_busy_operations;
    tavrn_repair_dependency_ops_t create_invalid_operations;
    tavrn_repair_dependency_ops_t release_busy_operations;
    tavrn_repair_dependency_ops_t local_failure_operations;
    tavrn_repair_t repair;
    tavrn_repair_t reserve_busy_repair;
    tavrn_repair_t create_invalid_repair;
    tavrn_repair_t release_busy_repair;
    tavrn_repair_t local_failure_repair;
    tavrn_owned_data_event_t owned = owned_data(0x8104u, peer(adva_d).logical_id.value);
    tavrn_repair_retry_input_t input = repair_input(&owned, 4u, NULL);
    tavrn_repair_setup_result_t setup;
    tavrn_repair_action_t smart;
    tavrn_repair_action_t smart_reissued;
    tavrn_repair_action_t full;
    tavrn_repair_action_t rejected_action;
    tavrn_repair_action_t release_busy_smart;
    tavrn_repair_action_t release_busy_full;
    tavrn_repair_action_t local_smart;
    tavrn_repair_action_t local_full;
    tavrn_repair_action_t local_drop;
    tavrn_repair_rreq_enqueue_t smart_enqueue;
    tavrn_repair_completion_t smart_done;
    tavrn_repair_snapshot_t snapshot;
    tavrn_repair_snapshot_t release_busy_snapshot;
    tavrn_repair_status_t statuses[12];
    tavrn_repair_status_t reissue_status;
    tavrn_repair_status_t admitted_status;
    tavrn_repair_status_t release_busy_status;
    tavrn_repair_status_t release_accept_status;
    tavrn_repair_status_t local_not_attempted_status;
    tavrn_repair_status_t local_full_invalid_status;
    tavrn_repair_status_t local_drop_status;

    REACHED("smart-ttl-full-scope-fresh-ids-no-inner-retry");
    repair_ops_fake_init(&fake);
    repair_ops_fake_init(&reserve_busy_fake);
    repair_ops_fake_init(&create_invalid_fake);
    repair_ops_fake_init(&release_busy_fake);
    repair_ops_fake_init(&local_failure_fake);
    reserve_busy_fake.reserve_status = TAVRN_REPAIR_HIGH_TOKEN_BUSY;
    create_invalid_fake.create_status = AODV_SINGLE_RREQ_INVALID;
    operations = repair_operations(&fake);
    reserve_busy_operations = repair_operations(&reserve_busy_fake);
    create_invalid_operations = repair_operations(&create_invalid_fake);
    release_busy_operations = repair_operations(&release_busy_fake);
    local_failure_operations = repair_operations(&local_failure_fake);
    memset(&repair, 0, sizeof(repair));
    memset(&reserve_busy_repair, 0, sizeof(reserve_busy_repair));
    memset(&create_invalid_repair, 0, sizeof(create_invalid_repair));
    memset(&release_busy_repair, 0, sizeof(release_busy_repair));
    memset(&local_failure_repair, 0, sizeof(local_failure_repair));
    memset(&setup, 0, sizeof(setup));
    memset(&smart, 0, sizeof(smart));
    memset(&smart_reissued, 0, sizeof(smart_reissued));
    memset(&full, 0, sizeof(full));
    memset(&rejected_action, 0, sizeof(rejected_action));
    memset(&release_busy_smart, 0, sizeof(release_busy_smart));
    memset(&release_busy_full, 0, sizeof(release_busy_full));
    memset(&local_smart, 0, sizeof(local_smart));
    memset(&local_full, 0, sizeof(local_full));
    memset(&local_drop, 0, sizeof(local_drop));
    memset(&snapshot, 0, sizeof(snapshot));
    memset(&release_busy_snapshot, 0, sizeof(release_busy_snapshot));
    statuses[0] = phase6_repair_init(&repair, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS, &operations
    });
    statuses[1] = phase6_repair_retry_exhausted(&repair, &input, 400u, &setup);
    statuses[2] = phase6_repair_owner_tick(&repair, 400u, &smart);
    smart_enqueue = enqueue_for(&smart, TAVRN_REPAIR_RREQ_ENQUEUE_BUSY);
    statuses[3] = phase6_repair_rreq_enqueue(&repair, &smart_enqueue, 400u);
    reissue_status = phase6_repair_owner_tick(&repair, 401u, &smart_reissued);
    smart_enqueue = enqueue_for(&smart_reissued, TAVRN_REPAIR_RREQ_ENQUEUE_ADMITTED);
    admitted_status = phase6_repair_rreq_enqueue(&repair, &smart_enqueue, 401u);
    smart_done = rreq_done(&smart_reissued, &smart_enqueue);
    statuses[4] = phase6_repair_complete(&repair, &smart_done, 400u, NULL);
    statuses[5] = phase6_repair_owner_tick(
        &repair, 400u + FAST_PATH_DISCOVERY_MS, &full);
    statuses[6] = phase6_repair_snapshot(&repair, &snapshot);
    statuses[7] = phase6_repair_init(&reserve_busy_repair, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS,
        &reserve_busy_operations
    });
    (void)phase6_repair_retry_exhausted(&reserve_busy_repair, &input, 500u, &setup);
    statuses[8] = phase6_repair_owner_tick(&reserve_busy_repair, 500u, &rejected_action);
    statuses[9] = phase6_repair_init(&create_invalid_repair, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS,
        &create_invalid_operations
    });
    (void)phase6_repair_retry_exhausted(&create_invalid_repair, &input, 600u, &setup);
    statuses[10] = phase6_repair_owner_tick(&create_invalid_repair, 600u, &rejected_action);
    statuses[11] = phase6_repair_snapshot(&create_invalid_repair, &snapshot);

    (void)phase6_repair_init(&release_busy_repair, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS,
        &release_busy_operations
    });
    (void)phase6_repair_retry_exhausted(&release_busy_repair, &input, 700u, &setup);
    (void)phase6_repair_owner_tick(&release_busy_repair, 700u, &release_busy_smart);
    smart_enqueue = enqueue_for(&release_busy_smart,
                                TAVRN_REPAIR_RREQ_ENQUEUE_ADMITTED);
    (void)phase6_repair_rreq_enqueue(&release_busy_repair, &smart_enqueue, 700u);
    smart_done = rreq_done(&release_busy_smart, &smart_enqueue);
    (void)phase6_repair_complete(&release_busy_repair, &smart_done, 700u, NULL);
    release_busy_fake.release_status = TAVRN_REPAIR_HIGH_TOKEN_BUSY;
    release_busy_status = phase6_repair_owner_tick(
        &release_busy_repair, 700u + FAST_PATH_DISCOVERY_MS, &release_busy_full);
    (void)phase6_repair_snapshot(&release_busy_repair, &release_busy_snapshot);
    release_busy_fake.release_status = TAVRN_REPAIR_HIGH_TOKEN_OK;
    release_accept_status = phase6_repair_owner_tick(
        &release_busy_repair, 700u + FAST_PATH_DISCOVERY_MS + 1u, &release_busy_full);

    (void)phase6_repair_init(&local_failure_repair, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS,
        &local_failure_operations
    });
    (void)phase6_repair_retry_exhausted(&local_failure_repair, &input, 800u, &setup);
    (void)phase6_repair_owner_tick(&local_failure_repair, 800u, &local_smart);
    smart_enqueue = enqueue_for(&local_smart,
                                TAVRN_REPAIR_RREQ_ENQUEUE_LOCAL_NOT_ATTEMPTED);
    local_not_attempted_status = phase6_repair_rreq_enqueue(
        &local_failure_repair, &smart_enqueue, 800u);
    (void)phase6_repair_owner_tick(&local_failure_repair, 801u, &local_full);
    smart_enqueue = enqueue_for(&local_full, TAVRN_REPAIR_RREQ_ENQUEUE_INVALID);
    local_full_invalid_status = phase6_repair_rreq_enqueue(
        &local_failure_repair, &smart_enqueue, 801u);
    local_drop_status = phase6_repair_owner_tick(&local_failure_repair, 802u,
                                                  &local_drop);

    CHECK("REPAIR-05", statuses[0] == TAVRN_REPAIR_OK &&
                        statuses[1] == TAVRN_REPAIR_OK &&
                        statuses[2] == TAVRN_REPAIR_OK &&
                        statuses[3] == TAVRN_REPAIR_BUSY &&
                        statuses[4] == TAVRN_REPAIR_OK &&
                        statuses[5] == TAVRN_REPAIR_OK &&
                        statuses[6] == TAVRN_REPAIR_OK &&
                        statuses[7] == TAVRN_REPAIR_OK &&
                        statuses[8] == TAVRN_REPAIR_BUSY &&
                        statuses[9] == TAVRN_REPAIR_OK &&
                        statuses[10] == TAVRN_REPAIR_INVALID &&
                        statuses[11] == TAVRN_REPAIR_OK &&
                        smart.type == TAVRN_REPAIR_ACTION_RREQ_CREATED &&
                        smart.rreq_created == 1u &&
                        smart.purpose == TAVRN_REPAIR_RREQ_SMART_TTL &&
                        smart.token_owner == TAVRN_REPAIR_HIGH_TOKEN_OWNER_REPAIR &&
                        smart.aodv_action.type == AODV_ACTION_SEND_RREQ &&
                        smart.rreq_attempt.current_scope == 4u &&
                        smart.rreq_attempt.ring_ordinal == 0u &&
                        smart.token >= 0x8000u &&
                        reissue_status == TAVRN_REPAIR_OK &&
                        admitted_status == TAVRN_REPAIR_OK &&
                        smart_reissued.type == TAVRN_REPAIR_ACTION_RREQ_CREATED &&
                        smart_reissued.token == smart.token &&
                        memcmp(&smart_reissued.aodv_action, &smart.aodv_action,
                               sizeof(smart.aodv_action)) == 0 &&
                        memcmp(&smart_reissued.rreq_attempt, &smart.rreq_attempt,
                               sizeof(smart.rreq_attempt)) == 0 &&
                        smart_done.token == smart.token &&
                        full.type == TAVRN_REPAIR_ACTION_RREQ_CREATED &&
                        full.purpose == TAVRN_REPAIR_RREQ_FULL_SCOPE &&
                        full.rreq_attempt.current_scope == NET_DIAMETER &&
                        full.rreq_attempt.request_id != smart.rreq_attempt.request_id &&
                        full.token != smart.token &&
                        fake.create_calls == 2u && fake.reserve_calls == 2u &&
                        fake.release_calls == 1u &&
                        snapshot.inner_retry_count == 0u &&
                        reserve_busy_fake.reserve_calls == 1u &&
                        reserve_busy_fake.create_calls == 0u &&
                        create_invalid_fake.reserve_calls == 1u &&
                        create_invalid_fake.create_calls == 1u &&
                        create_invalid_fake.release_calls == 1u &&
                        rejected_action.type == TAVRN_REPAIR_ACTION_NONE &&
                        release_busy_status == TAVRN_REPAIR_BUSY &&
                        release_busy_snapshot.state ==
                            TAVRN_REPAIR_STATE_WAIT_SMART_TTL_RREP &&
                        release_busy_snapshot.active_token == release_busy_smart.token &&
                        release_busy_full.type == TAVRN_REPAIR_ACTION_RREQ_CREATED &&
                        release_accept_status == TAVRN_REPAIR_OK &&
                        release_busy_full.purpose == TAVRN_REPAIR_RREQ_FULL_SCOPE &&
                        release_busy_full.token != release_busy_smart.token &&
                        release_busy_fake.create_calls == 2u &&
                        release_busy_fake.reserve_calls == 2u &&
                        release_busy_fake.release_calls == 2u &&
                        local_not_attempted_status == TAVRN_REPAIR_DECLINED &&
                        local_full_invalid_status == TAVRN_REPAIR_INVALID &&
                        local_full.type == TAVRN_REPAIR_ACTION_RREQ_CREATED &&
                        local_full.purpose == TAVRN_REPAIR_RREQ_FULL_SCOPE &&
                        local_full.rreq_attempt.request_id != local_smart.rreq_attempt.request_id &&
                        local_full.token != local_smart.token &&
                        local_drop_status == TAVRN_REPAIR_OK &&
                        local_drop.type == TAVRN_REPAIR_ACTION_DROP_DATA &&
                        local_failure_fake.create_calls == 2u &&
                        local_failure_fake.reserve_calls == 2u &&
                        local_failure_fake.release_calls == 2u &&
                        local_failure_fake.finish_calls == 1u);
}

static void test_matching_alternate_rrep_and_rejections(void)
{
    repair_ops_fake_t fake;
    repair_ops_fake_t rejected_fake;
    tavrn_repair_dependency_ops_t operations;
    tavrn_repair_dependency_ops_t rejected_operations;
    tavrn_repair_t repaired;
    tavrn_repair_t rejected;
    tavrn_owned_data_event_t owned = owned_data(0x8105u, peer(adva_d).logical_id.value);
    tavrn_repair_retry_input_t input = repair_input(&owned, 4u, NULL);
    tavrn_repair_setup_result_t setup;
    tavrn_repair_action_t action;
    tavrn_repair_rreq_enqueue_t enqueue;
    tavrn_repair_completion_t done;
    tavrn_repair_rrep_completion_t good;
    tavrn_repair_rrep_completion_t wrong;
    tavrn_repair_rrep_completion_t wrong_transmitter;
    tavrn_repair_rrep_completion_t failed_hop;
    tavrn_repair_rrep_completion_t late;
    tavrn_repair_terminal_t terminal;
    tavrn_repair_status_t statuses[14];
    uint32_t releases_after_busy;

    REACHED("matching-alternate-rrep-and-rejections");
    repair_ops_fake_init(&fake);
    repair_ops_fake_init(&rejected_fake);
    operations = repair_operations(&fake);
    rejected_operations = repair_operations(&rejected_fake);
    memset(&repaired, 0, sizeof(repaired));
    memset(&rejected, 0, sizeof(rejected));
    memset(&setup, 0, sizeof(setup));
    memset(&action, 0, sizeof(action));
    memset(&terminal, 0, sizeof(terminal));
    statuses[0] = phase6_repair_init(&repaired, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS, &operations
    });
    statuses[1] = phase6_repair_retry_exhausted(&repaired, &input, 500u, &setup);
    statuses[2] = phase6_repair_owner_tick(&repaired, 500u, &action);
    enqueue = enqueue_for(&action, TAVRN_REPAIR_RREQ_ENQUEUE_ADMITTED);
    statuses[3] = phase6_repair_rreq_enqueue(&repaired, &enqueue, 500u);
    done = rreq_done(&action, &enqueue);
    statuses[4] = phase6_repair_complete(&repaired, &done, 500u, NULL);
    good = matching_rrep(&action);
    fake.finish_status = AODV_FAILURE_BUSY;
    statuses[5] = phase6_repair_receive_rrep(&repaired, &good, 501u, &terminal);
    releases_after_busy = fake.release_calls;
    fake.finish_status = AODV_FAILURE_OK;
    statuses[6] = phase6_repair_receive_rrep(&repaired, &good, 502u, &terminal);

    statuses[7] = phase6_repair_init(&rejected, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS, &rejected_operations
    });
    statuses[8] = phase6_repair_retry_exhausted(&rejected, &input, 600u, &setup);
    statuses[9] = phase6_repair_owner_tick(&rejected, 600u, &action);
    enqueue = enqueue_for(&action, TAVRN_REPAIR_RREQ_ENQUEUE_ADMITTED);
    (void)phase6_repair_rreq_enqueue(&rejected, &enqueue, 600u);
    done = rreq_done(&action, &enqueue);
    (void)phase6_repair_complete(&rejected, &done, 600u, NULL);
    wrong = matching_rrep(&action);
    wrong.rreq_attempt.request_id++;
    statuses[10] = phase6_repair_receive_rrep(&rejected, &wrong, 601u, NULL);
    wrong_transmitter = matching_rrep(&action);
    wrong_transmitter.transmitter = peer(adva_e);
    statuses[11] = phase6_repair_receive_rrep(
        &rejected, &wrong_transmitter, 601u, NULL);
    failed_hop = matching_rrep(&action);
    failed_hop.installed_next_hop = owned.next_hop;
    statuses[12] = phase6_repair_receive_rrep(&rejected, &failed_hop, 601u, NULL);
    late = matching_rrep(&action);
    statuses[13] = phase6_repair_receive_rrep(
        &rejected, &late, 600u + FAST_REPAIR_TIMEOUT_MS, NULL);

    CHECK("REPAIR-03", statuses[0] == TAVRN_REPAIR_OK &&
                        statuses[1] == TAVRN_REPAIR_OK &&
                        statuses[2] == TAVRN_REPAIR_OK &&
                        statuses[3] == TAVRN_REPAIR_OK &&
                        statuses[4] == TAVRN_REPAIR_OK &&
                        statuses[5] == TAVRN_REPAIR_BUSY &&
                        statuses[6] == TAVRN_REPAIR_OK &&
                        statuses[7] == TAVRN_REPAIR_OK &&
                        statuses[8] == TAVRN_REPAIR_OK &&
                        statuses[9] == TAVRN_REPAIR_OK &&
                        statuses[10] == TAVRN_REPAIR_IGNORED &&
                        statuses[11] == TAVRN_REPAIR_IGNORED &&
                        statuses[12] == TAVRN_REPAIR_IGNORED &&
                        statuses[13] == TAVRN_REPAIR_IGNORED &&
                        fake.finish_calls == 2u &&
                        fake.last_finish_decision == AODV_DEFERRED_RERR_ROUTE_REPAIRED &&
                        releases_after_busy == 0u && fake.release_calls == 1u &&
                        fake.last_high_token == good.token && good.token >= 0x8000u &&
                         fake.failed_hop_verification_calls == 0u &&
                        terminal.kind == TAVRN_REPAIR_TERMINAL_REPAIRED &&
                         terminal.failed_hop_verification_state ==
                             TAVRN_REPAIR_FAILED_HOP_VERIFICATION_CANCELED &&
                        memcmp(terminal.failed_next_hop.adva.bytes, adva_h,
                               TAVRN_ADVA_LEN) == 0 &&
                        memcmp(good.installed_next_hop.adva.bytes, adva_h,
                               TAVRN_ADVA_LEN) != 0 &&
                         rejected_fake.finish_calls == 0u &&
                             rejected_fake.failed_hop_verification_calls == 0u);
}

static void test_active_rreq_snapshot_and_data_retry(void)
{
    repair_ops_fake_t fake;
    tavrn_repair_dependency_ops_t operations;
    tavrn_repair_t repair;
    tavrn_owned_data_event_t owned = owned_data(0x8120u, peer(adva_d).logical_id.value);
    tavrn_repair_retry_input_t input = repair_input(&owned, 4u, NULL);
    tavrn_repair_setup_result_t setup;
    tavrn_repair_action_t rreq;
    tavrn_repair_action_t flush;
    tavrn_repair_action_t retry;
    tavrn_repair_rreq_enqueue_t enqueue;
    tavrn_repair_completion_t completion;
    tavrn_repair_rrep_completion_t rrep;
    tavrn_repair_active_rreq_snapshot_t active;
    tavrn_repair_status_t statuses[10];

    REACHED("active-rreq-snapshot-data-not-admitted");
    repair_ops_fake_init(&fake);
    operations = repair_operations(&fake);
    memset(&repair, 0, sizeof(repair));
    memset(&setup, 0, sizeof(setup));
    memset(&rreq, 0, sizeof(rreq));
    memset(&flush, 0, sizeof(flush));
    memset(&retry, 0, sizeof(retry));
    memset(&active, 0, sizeof(active));
    statuses[0] = phase6_repair_init(&repair, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS, &operations
    });
    statuses[1] = phase6_repair_retry_exhausted(&repair, &input, 100u, &setup);
    statuses[2] = phase6_repair_owner_tick(&repair, 100u, &rreq);
    statuses[3] = phase6_repair_active_rreq_snapshot(&repair, &active);
    enqueue = enqueue_for(&rreq, TAVRN_REPAIR_RREQ_ENQUEUE_ADMITTED);
    statuses[4] = phase6_repair_rreq_enqueue(&repair, &enqueue, 100u);
    completion = rreq_done(&rreq, &enqueue);
    statuses[5] = phase6_repair_complete(&repair, &completion, 101u, NULL);
    statuses[6] = phase6_repair_active_rreq_snapshot(&repair, &active);
    rrep = matching_rrep(&rreq);
    statuses[7] = phase6_repair_receive_rrep(&repair, &rrep, 102u, NULL);
    statuses[8] = phase6_repair_owner_tick(&repair, 102u, &flush);
    statuses[9] = phase6_repair_data_action_not_admitted(&repair, &flush, 102u);
    (void)phase6_repair_owner_tick(&repair, 103u, &retry);

    CHECK("REPAIR-05", statuses[0] == TAVRN_REPAIR_OK &&
                         statuses[1] == TAVRN_REPAIR_OK &&
                         statuses[2] == TAVRN_REPAIR_OK &&
                         statuses[3] == TAVRN_REPAIR_OK &&
                         statuses[4] == TAVRN_REPAIR_OK &&
                         statuses[5] == TAVRN_REPAIR_OK &&
                         statuses[6] == TAVRN_REPAIR_OK &&
                         statuses[7] == TAVRN_REPAIR_OK &&
                         statuses[8] == TAVRN_REPAIR_OK &&
                         statuses[9] == TAVRN_REPAIR_OK &&
                         active.active == 1u && active.tx_done == 1u &&
                         active.state == TAVRN_REPAIR_STATE_WAIT_SMART_TTL_RREP &&
                         active.purpose == TAVRN_REPAIR_RREQ_SMART_TTL &&
                         active.token == rreq.token &&
                         memcmp(&active.attempt, &rreq.rreq_attempt,
                                sizeof(active.attempt)) == 0 &&
                         memcmp(&active.failed_next_hop, &owned.next_hop,
                                sizeof(active.failed_next_hop)) == 0 &&
                         active.destination.value == owned.data.final_destination.value &&
                         active.total_deadline_ms == 100u + FAST_REPAIR_TIMEOUT_MS &&
                         active.stage_response_deadline_ms ==
                             101u + FAST_PATH_DISCOVERY_MS &&
                         flush.type == TAVRN_REPAIR_ACTION_FLUSH_DATA &&
                         retry.type == TAVRN_REPAIR_ACTION_FLUSH_DATA &&
                         retry.token != flush.token &&
                         memcmp(&retry.owned, &flush.owned, sizeof(retry.owned)) == 0);
}

static void test_timeout_cooldown_and_wrap(void)
{
    repair_ops_fake_t fake;
    tavrn_repair_dependency_ops_t operations;
    tavrn_repair_t repair;
    tavrn_owned_data_event_t owned = owned_data(0x8106u, peer(adva_d).logical_id.value);
    tavrn_repair_retry_input_t input = repair_input(&owned, 4u, NULL);
    tavrn_repair_setup_result_t setup;
    tavrn_repair_setup_result_t during_cooldown;
    tavrn_repair_action_t action;
    tavrn_repair_rreq_enqueue_t enqueue;
    tavrn_repair_completion_t completion;
    tavrn_repair_snapshot_t snapshot;
    tavrn_repair_terminal_t terminal;
    uint32_t started_at = 0xfffffff0u;
    uint32_t deadline = 0x00000694u;
    uint32_t cooldown_deadline = 0x00000a7cu;
    tavrn_repair_status_t statuses[11];

    REACHED("total-timeout-cooldown-wrap");
    repair_ops_fake_init(&fake);
    operations = repair_operations(&fake);
    memset(&repair, 0, sizeof(repair));
    memset(&setup, 0, sizeof(setup));
    memset(&during_cooldown, 0, sizeof(during_cooldown));
    memset(&action, 0, sizeof(action));
    memset(&snapshot, 0, sizeof(snapshot));
    memset(&terminal, 0, sizeof(terminal));
    statuses[0] = phase6_repair_init(&repair, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS, &operations
    });
    statuses[1] = phase6_repair_retry_exhausted(&repair, &input, started_at, &setup);
    statuses[2] = phase6_repair_owner_tick(&repair, started_at, &action);
    enqueue = enqueue_for(&action, TAVRN_REPAIR_RREQ_ENQUEUE_ADMITTED);
    statuses[3] = phase6_repair_rreq_enqueue(&repair, &enqueue, started_at);
    completion = rreq_done(&action, &enqueue);
    statuses[4] = phase6_repair_complete(&repair, &completion, started_at, NULL);
    statuses[5] = phase6_repair_owner_tick(
        &repair, started_at + FAST_PATH_DISCOVERY_MS, &action);
    enqueue = enqueue_for(&action, TAVRN_REPAIR_RREQ_ENQUEUE_ADMITTED);
    (void)phase6_repair_rreq_enqueue(&repair, &enqueue, started_at + FAST_PATH_DISCOVERY_MS);
    completion = rreq_done(&action, &enqueue);
    (void)phase6_repair_complete(&repair, &completion, started_at + FAST_PATH_DISCOVERY_MS, NULL);
    statuses[6] = phase6_repair_owner_tick(&repair, deadline, &action);
    completion = data_completion(&action, TAVRN_REPAIR_COMPLETION_DATA_DROPPED);
    statuses[7] = phase6_repair_complete(&repair, &completion, deadline, &terminal);
    statuses[8] = phase6_repair_snapshot(&repair, &snapshot);
    statuses[9] = phase6_repair_retry_exhausted(
        &repair, &input, cooldown_deadline - 1u, &during_cooldown);
    statuses[10] = phase6_repair_retry_exhausted(
        &repair, &input, cooldown_deadline, &setup);

    CHECK("SERIAL-05", statuses[0] == TAVRN_REPAIR_OK &&
                       statuses[1] == TAVRN_REPAIR_OK &&
                       statuses[2] == TAVRN_REPAIR_OK &&
                       statuses[3] == TAVRN_REPAIR_OK &&
                       statuses[4] == TAVRN_REPAIR_OK &&
                       statuses[5] == TAVRN_REPAIR_OK &&
                       statuses[6] == TAVRN_REPAIR_OK &&
                       statuses[7] == TAVRN_REPAIR_OK &&
                       statuses[8] == TAVRN_REPAIR_OK &&
                       statuses[9] == TAVRN_REPAIR_OK &&
                       statuses[10] == TAVRN_REPAIR_OK &&
                       action.type == TAVRN_REPAIR_ACTION_DROP_DATA &&
                       snapshot.state == TAVRN_REPAIR_STATE_COOLDOWN &&
                       snapshot.deadline_ms == deadline &&
                       snapshot.cooldown_deadline_ms == cooldown_deadline &&
                       during_cooldown.disposition == TAVRN_REPAIR_RETRY_DECLINED &&
                       setup.disposition == TAVRN_REPAIR_RETRY_OWNED &&
                       fake.finish_calls == 1u &&
                       fake.last_finish_decision == AODV_DEFERRED_RERR_REPAIR_FAILED &&
                        fake.failed_hop_verification_calls == 1u);
}

static void test_deferred_rerr_leave_and_exact_once_terminals(void)
{
    repair_ops_fake_t success_fake;
    repair_ops_fake_t failure_fake;
    repair_ops_fake_t reservation_fake;
    tavrn_repair_dependency_ops_t success_operations;
    tavrn_repair_dependency_ops_t failure_operations;
    tavrn_repair_dependency_ops_t reservation_operations;
    tavrn_repair_t repaired;
    tavrn_repair_t failed;
    tavrn_repair_t reservation_repaired;
    tavrn_owned_data_event_t owned = owned_data(0x8107u, peer(adva_d).logical_id.value);
    tavrn_repair_retry_input_t input = repair_input(&owned, 4u, NULL);
    tavrn_link_data_t second = owned.data;
    tavrn_owned_data_event_t reserved_transferred =
        owned_data(0x8117u, peer(adva_d).logical_id.value);
    tavrn_owned_data_event_t reserved_rejected =
        owned_data(0x8118u, peer(adva_d).logical_id.value);
    tavrn_owned_data_event_t reserved_retry =
        owned_data(0x8119u, peer(adva_d).logical_id.value);
    tavrn_repair_retry_input_t reservation_input;
    tavrn_repair_setup_result_t setup;
    tavrn_repair_candidate_result_t reservation;
    tavrn_repair_action_t action;
    tavrn_repair_action_t first_flush;
    tavrn_repair_action_t first_drop;
    tavrn_repair_action_t reservation_flush;
    tavrn_repair_rreq_enqueue_t enqueue;
    tavrn_repair_completion_t completion;
    tavrn_repair_rrep_completion_t rrep;
    tavrn_repair_terminal_t success_terminal;
    tavrn_repair_terminal_t failure_terminal;
    tavrn_repair_snapshot_t success_snapshot;
    tavrn_repair_snapshot_t leave_busy_snapshot;
    tavrn_repair_snapshot_t failure_snapshot;
    tavrn_repair_snapshot_t reservation_holding_snapshot;
    tavrn_repair_snapshot_t reservation_final_snapshot;
    tavrn_repair_link_custody_result_t reservation_released;
    tavrn_repair_link_custody_result_t reservation_rejected_result;
    tavrn_repair_link_custody_result_t reservation_converted;
    tavrn_repair_link_custody_terminal_input_t reservation_transfer_input;
    tavrn_repair_link_custody_terminal_input_t reservation_rejected_input;
    tavrn_repair_link_custody_terminal_input_t reservation_retry_input;
    tavrn_repair_status_t statuses[19];
    tavrn_repair_status_t reservation_statuses[13];

    REACHED("deferred-rerr-held-leave-exactly-once");
    second.data_seq++;
    repair_ops_fake_init(&success_fake);
    repair_ops_fake_init(&failure_fake);
    repair_ops_fake_init(&reservation_fake);
    failure_fake.failed_hop_verification_status =
        TAVRN_REPAIR_FAILED_HOP_VERIFICATION_BUSY;
    success_operations = repair_operations(&success_fake);
    failure_operations = repair_operations(&failure_fake);
    reservation_operations = repair_operations(&reservation_fake);
    memset(&repaired, 0, sizeof(repaired));
    memset(&failed, 0, sizeof(failed));
    memset(&reservation_repaired, 0, sizeof(reservation_repaired));
    memset(&setup, 0, sizeof(setup));
    memset(&reservation, 0, sizeof(reservation));
    memset(&action, 0, sizeof(action));
    memset(&first_flush, 0, sizeof(first_flush));
    memset(&first_drop, 0, sizeof(first_drop));
    memset(&reservation_flush, 0, sizeof(reservation_flush));
    memset(&success_terminal, 0, sizeof(success_terminal));
    memset(&failure_terminal, 0, sizeof(failure_terminal));
    memset(&success_snapshot, 0, sizeof(success_snapshot));
    memset(&leave_busy_snapshot, 0, sizeof(leave_busy_snapshot));
    memset(&failure_snapshot, 0, sizeof(failure_snapshot));
    memset(&reservation_holding_snapshot, 0, sizeof(reservation_holding_snapshot));
    memset(&reservation_final_snapshot, 0, sizeof(reservation_final_snapshot));
    memset(&reservation_released, 0, sizeof(reservation_released));
    memset(&reservation_rejected_result, 0, sizeof(reservation_rejected_result));
    memset(&reservation_converted, 0, sizeof(reservation_converted));
    memset(&reservation_transfer_input, 0, sizeof(reservation_transfer_input));
    memset(&reservation_rejected_input, 0, sizeof(reservation_rejected_input));
    memset(&reservation_retry_input, 0, sizeof(reservation_retry_input));
    statuses[0] = phase6_repair_init(&repaired, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS,
        &success_operations
    });
    statuses[1] = phase6_repair_retry_exhausted(&repaired, &input, 700u, &setup);
    statuses[2] = phase6_repair_reserve_candidate(&repaired, &second, 701u, &reservation);
    statuses[3] = phase6_repair_commit_candidate(
        &repaired, &reservation.reservation, &second, 701u);
    statuses[4] = phase6_repair_owner_tick(&repaired, 702u, &action);
    enqueue = enqueue_for(&action, TAVRN_REPAIR_RREQ_ENQUEUE_ADMITTED);
    (void)phase6_repair_rreq_enqueue(&repaired, &enqueue, 702u);
    completion = rreq_done(&action, &enqueue);
    (void)phase6_repair_complete(&repaired, &completion, 702u, NULL);
    rrep = matching_rrep(&action);
    statuses[5] = phase6_repair_receive_rrep(&repaired, &rrep, 703u, &success_terminal);
    statuses[6] = phase6_repair_owner_tick(&repaired, 703u, &action);
    first_flush = action;
    completion = data_completion(&action, TAVRN_REPAIR_COMPLETION_DATA_FLUSHED);
    statuses[7] = phase6_repair_complete(&repaired, &completion, 703u, NULL);
    statuses[8] = phase6_repair_complete(&repaired, &completion, 703u, NULL);
    (void)phase6_repair_owner_tick(&repaired, 704u, &action);
    completion = data_completion(&action, TAVRN_REPAIR_COMPLETION_DATA_FLUSHED);
    (void)phase6_repair_complete(&repaired, &completion, 704u, NULL);
    (void)phase6_repair_snapshot(&repaired, &success_snapshot);

    reservation_input = repair_input(&owned, 4u, &reserved_transferred.data);
    reservation_input.same_destination_link_custody.count = 3u;
    reservation_input.same_destination_link_custody.records[1].next_hop =
        reserved_rejected.next_hop;
    reservation_input.same_destination_link_custody.records[1].data =
        reserved_rejected.data;
    reservation_input.same_destination_link_custody.records[2].next_hop =
        reserved_retry.next_hop;
    reservation_input.same_destination_link_custody.records[2].data =
        reserved_retry.data;
    reservation_statuses[0] = phase6_repair_init(&reservation_repaired,
        &(tavrn_repair_config_t){
            1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
            FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS,
            &reservation_operations
        });
    reservation_statuses[1] = phase6_repair_retry_exhausted(
        &reservation_repaired, &reservation_input, 750u, &setup);
    reservation_statuses[2] = phase6_repair_owner_tick(
        &reservation_repaired, 750u, &action);
    enqueue = enqueue_for(&action, TAVRN_REPAIR_RREQ_ENQUEUE_ADMITTED);
    reservation_statuses[3] = phase6_repair_rreq_enqueue(
        &reservation_repaired, &enqueue, 750u);
    completion = rreq_done(&action, &enqueue);
    reservation_statuses[4] = phase6_repair_complete(
        &reservation_repaired, &completion, 750u, NULL);
    rrep = matching_rrep(&action);
    reservation_statuses[5] = phase6_repair_receive_rrep(
        &reservation_repaired, &rrep, 751u, NULL);
    (void)phase6_repair_snapshot(&reservation_repaired,
                                 &reservation_holding_snapshot);
    reservation_statuses[6] = phase6_repair_owner_tick(
        &reservation_repaired, 751u, &action);
    completion = data_completion(&action, TAVRN_REPAIR_COMPLETION_DATA_FLUSHED);
    reservation_statuses[7] = phase6_repair_complete(
        &reservation_repaired, &completion, 751u, NULL);
    reservation_transfer_input = custody_data(&reserved_transferred.data);
    reservation_statuses[8] = phase6_repair_observe_link_custody(
        &reservation_repaired, &reservation_transfer_input,
        TAVRN_REPAIR_LINK_CUSTODY_TRANSFERRED, 752u, &reservation_released);
    reservation_rejected_input = custody_data(&reserved_rejected.data);
    reservation_retry_input = custody_retry(&reserved_retry);
    reservation_statuses[9] = phase6_repair_observe_link_custody(
        &reservation_repaired, &reservation_rejected_input,
        TAVRN_REPAIR_LINK_CUSTODY_REJECTED, 753u, &reservation_rejected_result);
    reservation_statuses[10] = phase6_repair_observe_link_custody(
        &reservation_repaired, &reservation_retry_input,
        TAVRN_REPAIR_LINK_CUSTODY_RETRY_EXHAUSTED, 754u, &reservation_converted);
    reservation_statuses[11] = phase6_repair_owner_tick(
        &reservation_repaired, 754u, &reservation_flush);
    completion = data_completion(&reservation_flush,
                                 TAVRN_REPAIR_COMPLETION_DATA_FLUSHED);
    reservation_statuses[12] = phase6_repair_complete(
        &reservation_repaired, &completion, 754u, NULL);
    (void)phase6_repair_snapshot(&reservation_repaired,
                                 &reservation_final_snapshot);

    statuses[9] = phase6_repair_init(&failed, &(tavrn_repair_config_t){
        1u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS,
        &failure_operations
    });
    statuses[10] = phase6_repair_retry_exhausted(&failed, &input, 800u, &setup);
    statuses[11] = phase6_repair_reserve_candidate(&failed, &second, 801u, &reservation);
    statuses[12] = phase6_repair_commit_candidate(
        &failed, &reservation.reservation, &second, 801u);
    statuses[13] = phase6_repair_owner_tick(&failed, 800u + FAST_REPAIR_TIMEOUT_MS,
                                             &action);
    first_drop = action;
    completion = data_completion(&action, TAVRN_REPAIR_COMPLETION_DATA_DROPPED);
    statuses[14] = phase6_repair_complete(
        &failed, &completion, 800u + FAST_REPAIR_TIMEOUT_MS, NULL);
    (void)phase6_repair_owner_tick(&failed, 800u + FAST_REPAIR_TIMEOUT_MS, &action);
    completion = data_completion(&action, TAVRN_REPAIR_COMPLETION_DATA_DROPPED);
    statuses[15] = phase6_repair_complete(
        &failed, &completion, 800u + FAST_REPAIR_TIMEOUT_MS, &failure_terminal);
    statuses[16] = phase6_repair_snapshot(&failed, &leave_busy_snapshot);
    failure_fake.failed_hop_verification_status =
        TAVRN_REPAIR_FAILED_HOP_VERIFICATION_OK;
    statuses[17] = phase6_repair_owner_tick(
        &failed, 800u + FAST_REPAIR_TIMEOUT_MS + 1u, &action);
    statuses[18] = phase6_repair_snapshot(&failed, &failure_snapshot);

    CHECK("MAINT-08", statuses[0] == TAVRN_REPAIR_OK &&
                       statuses[1] == TAVRN_REPAIR_OK &&
                       statuses[2] == TAVRN_REPAIR_OK &&
                       statuses[3] == TAVRN_REPAIR_OK &&
                       statuses[4] == TAVRN_REPAIR_OK &&
                       statuses[5] == TAVRN_REPAIR_OK &&
                       statuses[6] == TAVRN_REPAIR_OK &&
                       statuses[7] == TAVRN_REPAIR_OK &&
                       statuses[8] == TAVRN_REPAIR_IGNORED &&
                       statuses[9] == TAVRN_REPAIR_OK &&
                       statuses[10] == TAVRN_REPAIR_OK &&
                       statuses[11] == TAVRN_REPAIR_OK &&
                       statuses[12] == TAVRN_REPAIR_OK &&
                       statuses[13] == TAVRN_REPAIR_OK &&
                       statuses[14] == TAVRN_REPAIR_OK &&
                       statuses[15] == TAVRN_REPAIR_BUSY &&
                       statuses[16] == TAVRN_REPAIR_OK &&
                       statuses[17] == TAVRN_REPAIR_OK &&
                       statuses[18] == TAVRN_REPAIR_OK &&
                       success_fake.finish_calls == 1u &&
                       success_fake.last_finish_decision ==
                           AODV_DEFERRED_RERR_ROUTE_REPAIRED &&
                        success_fake.failed_hop_verification_calls == 0u &&
                       success_terminal.kind == TAVRN_REPAIR_TERMINAL_REPAIRED &&
                        success_terminal.failed_hop_verification_state ==
                            TAVRN_REPAIR_FAILED_HOP_VERIFICATION_CANCELED &&
                       first_flush.type == TAVRN_REPAIR_ACTION_FLUSH_DATA &&
                       memcmp(&first_flush.owned.data, &owned.data,
                              sizeof(owned.data)) == 0 &&
                        success_snapshot.flush_count == 2u &&
                        success_snapshot.drop_count == 0u &&
                        reservation_statuses[0] == TAVRN_REPAIR_OK &&
                        reservation_statuses[1] == TAVRN_REPAIR_OK &&
                        reservation_statuses[2] == TAVRN_REPAIR_OK &&
                        reservation_statuses[3] == TAVRN_REPAIR_OK &&
                        reservation_statuses[4] == TAVRN_REPAIR_OK &&
                        reservation_statuses[5] == TAVRN_REPAIR_OK &&
                        reservation_statuses[6] == TAVRN_REPAIR_OK &&
                        reservation_statuses[7] == TAVRN_REPAIR_OK &&
                        reservation_statuses[8] == TAVRN_REPAIR_OK &&
                        reservation_statuses[9] == TAVRN_REPAIR_OK &&
                        reservation_statuses[10] == TAVRN_REPAIR_OK &&
                        reservation_statuses[11] == TAVRN_REPAIR_OK &&
                        reservation_statuses[12] == TAVRN_REPAIR_OK &&
                        reservation_holding_snapshot.state ==
                            TAVRN_REPAIR_STATE_FLUSHING &&
                        reservation_holding_snapshot.buffered_count == 1u &&
                        reservation_holding_snapshot.reserved_link_slots == 3u &&
                        reservation_released.released_reservation == 1u &&
                        reservation_rejected_result.released_reservation == 1u &&
                        reservation_converted.converted_to_buffer == 1u &&
                        reservation_flush.type == TAVRN_REPAIR_ACTION_FLUSH_DATA &&
                        memcmp(&reservation_flush.owned, &reserved_retry,
                               sizeof(reserved_retry)) == 0 &&
                        reservation_final_snapshot.state == TAVRN_REPAIR_STATE_COOLDOWN &&
                        reservation_final_snapshot.buffered_count == 0u &&
                        reservation_final_snapshot.reserved_link_slots == 0u &&
                        reservation_final_snapshot.flush_count == 2u &&
                        failure_fake.finish_calls == 1u &&
                       failure_fake.last_finish_decision ==
                           AODV_DEFERRED_RERR_REPAIR_FAILED &&
                        failure_fake.failed_hop_verification_calls == 2u &&
                       failure_terminal.kind == TAVRN_REPAIR_TERMINAL_FAILED &&
                        failure_terminal.failed_hop_verification_state ==
                            TAVRN_REPAIR_FAILED_HOP_VERIFICATION_HELD &&
                        leave_busy_snapshot.state == TAVRN_REPAIR_STATE_DROPPING &&
                        leave_busy_snapshot.failed_hop_verification_state ==
                            TAVRN_REPAIR_FAILED_HOP_VERIFICATION_HELD &&
                       first_drop.type == TAVRN_REPAIR_ACTION_DROP_DATA &&
                       memcmp(&first_drop.owned.data, &owned.data,
                              sizeof(owned.data)) == 0 &&
                       failure_snapshot.state == TAVRN_REPAIR_STATE_COOLDOWN &&
                        failure_snapshot.failed_hop_verification_state ==
                            TAVRN_REPAIR_FAILED_HOP_VERIFICATION_SCHEDULED &&
                       failure_snapshot.flush_count == 0u && failure_snapshot.drop_count == 2u);
}

static void test_repair_off_equivalence(void)
{
    repair_ops_fake_t fake;
    tavrn_repair_dependency_ops_t operations;
    tavrn_repair_t repair;
    tavrn_owned_data_event_t owned = owned_data(0x8108u, peer(adva_d).logical_id.value);
    tavrn_owned_data_event_t original = owned;
    tavrn_repair_retry_input_t input = repair_input(&owned, 4u, NULL);
    tavrn_repair_setup_result_t result;
    tavrn_repair_snapshot_t snapshot;
    tavrn_repair_status_t init_status;
    tavrn_repair_status_t retry_status;
    tavrn_repair_status_t snapshot_status;

    REACHED("repair-off-equivalence");
    repair_ops_fake_init(&fake);
    operations = repair_operations(&fake);
    memset(&repair, 0, sizeof(repair));
    memset(&result, 0, sizeof(result));
    memset(&snapshot, 0, sizeof(snapshot));
    init_status = phase6_repair_init(&repair, &(tavrn_repair_config_t){
        0u, NET_DIAMETER, TAVRN_REPAIR_BUFFER_CAPACITY,
        FAST_PATH_DISCOVERY_MS, FAST_REPAIR_TIMEOUT_MS, REPAIR_COOLDOWN_MS, &operations
    });
    retry_status = phase6_repair_retry_exhausted(&repair, &input, 900u, &result);
    snapshot_status = phase6_repair_snapshot(&repair, &snapshot);

    CHECK("REPAIR-01", init_status == TAVRN_REPAIR_OK &&
                         retry_status == TAVRN_REPAIR_OK &&
                         snapshot_status == TAVRN_REPAIR_OK &&
                         result.disposition == TAVRN_REPAIR_RETRY_DECLINED &&
                         result.state == TAVRN_REPAIR_SETUP_NORMAL_FAILURE &&
                         result.failure_mode == AODV_LINK_FAILURE_IMMEDIATE_RERR &&
                         result.data_copied == 0u &&
                         result.deferred_rerr_started == 0u &&
                          result.held_failed_hop_verification_created == 0u &&
                         memcmp(&input.owned, &original, sizeof(original)) == 0 &&
                         snapshot.state == TAVRN_REPAIR_STATE_IDLE &&
                         snapshot.buffered_count == 0u &&
                         fake.begin_calls == 0u && fake.create_calls == 0u &&
                          fake.reserve_calls == 0u &&
                              fake.failed_hop_verification_calls == 0u);
}

int main(void)
{
    test_strict_config_and_capacity();
    test_owned_transit_and_negative_triggers();
    test_atomic_setup_and_deferred_obligations();
    test_same_destination_capacity_and_reservations();
    test_exact_custody_next_hop_and_reconciliation();
    test_smart_ttl_full_scope_and_fresh_ids();
    test_matching_alternate_rrep_and_rejections();
    test_active_rreq_snapshot_and_data_retry();
    test_timeout_cooldown_and_wrap();
    test_deferred_rerr_leave_and_exact_once_terminals();
    test_repair_off_equivalence();
    if (failures != 0u) {
        printf("tavrn_phase6_repair tests failed: %u assertion(s)\n", failures);
        return 1;
    }
    printf("tavrn_phase6_repair tests passed\n");
    return 0;
}
