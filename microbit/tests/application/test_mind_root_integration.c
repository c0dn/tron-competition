#include "mind_root_coordinator.h"
#include "mind_uart.h"

#include <stdio.h>
#include <string.h>

static unsigned failures;

/* The lifecycle tests inject at mind_uart_isr_push() and do not install the
 * vector, but these host-only shims close the complete UART translation unit. */
uint32_t mind_uart_host_in_w(uint32_t address)
{
    (void)address;
    return 0u;
}

void mind_uart_host_out_w(uint32_t address, uint32_t value)
{
    (void)address;
    (void)value;
}

int mind_uart_host_def_int(unsigned int intno, const void *definition)
{
    (void)intno;
    (void)definition;
    return 0;
}

void mind_uart_host_enable_int(unsigned int intno, int priority)
{
    (void)intno;
    (void)priority;
}

typedef struct fake_binding {
    routed_cycle_gtt_snapshot_t snapshot;
    mind_uart_t uart;
    mind_application_ingress_t ingress;
    mind_root_inbox_t inbox;
    mind_log_queue_t logs;
    tavrn_adva_t incoming_identity;
    tavrn_adva_t outgoing_first_identity;
    tavrn_adva_t outgoing_second_identity;
    mind_root_resolver_status_t outgoing_status;
    mind_root_resolver_status_t outgoing_first_status;
    mind_root_resolver_status_t outgoing_second_status;
    mind_root_resolver_status_t incoming_status;
    mind_root_coordinator_submit_status_t submit_status;
    uint32_t submit_calls;
    uint32_t outgoing_resolve_calls;
    uint32_t incoming_resolve_calls;
    uint32_t incoming_resolve_now_ms;
    uint32_t ui_calls;
    uint32_t gtt_claim_calls;
    uint32_t gtt_commit_calls;
    uint32_t gtt_cancel_calls;
    uint8_t sid8_ready;
    uint8_t incoming_identity_by_sid8;
    uint8_t outgoing_identity_statuses;
    uint8_t snapshot_ok;
    uint8_t ui_local_active;
    uint8_t ui_active_roots;
    uint8_t gtt_available;
    uint8_t gtt_claimed;
    uint8_t log_commit_fails;
} fake_binding_t;

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

static mind_application_root_state_t root_state(uint8_t active, uint16_t nonce,
                                                 uint16_t generation)
{
    mind_application_root_state_t result;

    result.active = active;
    result.nonce = nonce;
    result.generation = generation;
    return result;
}

static int fake_snapshot(void *context, uint32_t now,
                         routed_cycle_gtt_snapshot_t *snapshot_out)
{
    fake_binding_t *binding = context;

    (void)now;
    if (binding == NULL || snapshot_out == NULL || binding->snapshot_ok == 0u) {
        return 0;
    }
    *snapshot_out = binding->snapshot;
    return 1;
}

static uint8_t fake_sid8_ready(void *context)
{
    return ((fake_binding_t *)context)->sid8_ready;
}

static mind_root_resolver_status_t fake_resolve_outgoing(
    void *context, const tavrn_adva_t *canonical, tavrn_logical_id_t *sid8_out)
{
    fake_binding_t *binding = context;
    mind_root_resolver_status_t status;

    binding->outgoing_resolve_calls++;
    status = binding->outgoing_status;
    if (binding->outgoing_identity_statuses != 0u && canonical != NULL) {
        if (memcmp(canonical->bytes, binding->outgoing_first_identity.bytes,
                   TAVRN_ADVA_LEN) == 0) {
            status = binding->outgoing_first_status;
        } else if (memcmp(canonical->bytes, binding->outgoing_second_identity.bytes,
                          TAVRN_ADVA_LEN) == 0) {
            status = binding->outgoing_second_status;
        }
    }
    if (status == MIND_ROOT_RESOLVER_UNIQUE && sid8_out != NULL) {
        sid8_out->width = TAVRN_IDENTITY_SID8;
        sid8_out->value = 0x44u;
    }
    return status;
}

static mind_root_resolver_status_t fake_resolve_incoming(
    void *context, uint8_t sid8, uint32_t now, tavrn_adva_t *canonical_out)
{
    fake_binding_t *binding = context;

    binding->incoming_resolve_calls++;
    binding->incoming_resolve_now_ms = now;
    if (binding->incoming_status == MIND_ROOT_RESOLVER_UNIQUE && canonical_out != NULL) {
        *canonical_out = binding->incoming_identity_by_sid8 != 0u ?
            identity(sid8) : binding->incoming_identity;
    }
    return binding->incoming_status;
}

static int fake_command_peek(void *context, mind_command_attempt_t *attempt_out)
{
    return mind_uart_command_peek(&((fake_binding_t *)context)->uart, attempt_out);
}

static int fake_command_consume(void *context)
{
    return mind_uart_command_consume(&((fake_binding_t *)context)->uart);
}

static int fake_gtt_claim(void *context)
{
    fake_binding_t *binding = context;

    binding->gtt_claim_calls++;
    if (binding->gtt_available == 0u || binding->gtt_claimed != 0u) {
        return 0;
    }
    binding->gtt_claimed = 1u;
    return 1;
}

static void fake_gtt_commit(void *context)
{
    fake_binding_t *binding = context;

    binding->gtt_commit_calls++;
}

static void fake_gtt_cancel(void *context)
{
    fake_binding_t *binding = context;

    binding->gtt_cancel_calls++;
    binding->gtt_claimed = 0u;
}

static int fake_final_peek(void *context, mind_root_inbox_entry_t *entry_out)
{
    return mind_root_inbox_peek(&((fake_binding_t *)context)->inbox, entry_out);
}

static int fake_final_consume(void *context)
{
    return mind_root_inbox_consume(&((fake_binding_t *)context)->inbox);
}

static int fake_final_pin_observer(void *context, const tavrn_adva_t *observer)
{
    return mind_root_inbox_pin_observer(&((fake_binding_t *)context)->inbox, observer);
}

static mind_application_ingress_take_status_t fake_ingress_take(
    void *context, mind_application_ingress_event_t *event_out)
{
    return mind_application_ingress_take(&((fake_binding_t *)context)->ingress,
                                         event_out);
}

static tavrn_router_delivery_status_t fake_final_publish_local(
    void *context, const mind_application_wire_record_t *record,
    const tavrn_adva_t *local_observer, uint32_t now)
{
    return mind_root_inbox_publish_local(&((fake_binding_t *)context)->inbox, record,
                                         local_observer, now);
}

static int fake_log_reserve(void *context, uint8_t count,
                            mind_log_reservation_t *reservation_out)
{
    return mind_log_queue_reserve(&((fake_binding_t *)context)->logs, count,
                                  reservation_out);
}

static int fake_log_commit(void *context, mind_log_reservation_t *reservation,
                           const mind_log_record_t *records)
{
    fake_binding_t *binding = context;

    if (binding->log_commit_fails != 0u) {
        return 0;
    }
    return mind_log_queue_commit(&binding->logs, reservation, records);
}

static void fake_log_cancel(void *context, mind_log_reservation_t *reservation)
{
    (void)context;
    mind_log_queue_cancel(reservation);
}

static void fake_ui_publish(void *context, uint8_t local_active,
                            uint8_t active_roots)
{
    fake_binding_t *binding = context;

    binding->ui_calls++;
    binding->ui_local_active = local_active;
    binding->ui_active_roots = active_roots;
}

static mind_root_coordinator_submit_status_t fake_generic_submit(
    void *context, const tron_application_data_t *data, uint32_t now)
{
    fake_binding_t *binding = context;

    (void)data;
    (void)now;
    binding->submit_calls++;
    return binding->submit_status;
}

static void setup(mind_root_coordinator_t *coordinator, fake_binding_t *binding,
                  uint8_t with_target)
{
    mind_root_coordinator_operations_t operations;
    tavrn_adva_t local = identity(0x80u);

    memset(binding, 0, sizeof(*binding));
    binding->snapshot_ok = 1u;
    binding->sid8_ready = 1u;
    binding->gtt_available = 1u;
    binding->outgoing_status = MIND_ROOT_RESOLVER_UNIQUE;
    binding->incoming_status = MIND_ROOT_RESOLVER_UNIQUE;
    binding->submit_status = MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED;
    binding->incoming_identity = identity(1u);
    if (with_target != 0u) {
        binding->snapshot.entry_count = 1u;
        binding->snapshot.entries[0].canonical_adva = binding->incoming_identity;
        binding->snapshot.entries[0].departed = ROUTED_CYCLE_BOOLEAN_FALSE;
    }
    mind_uart_init_state(&binding->uart);
    mind_application_ingress_init(&binding->ingress, 1u);
    mind_root_inbox_init(&binding->inbox);
    mind_log_queue_init(&binding->logs);
    memset(&operations, 0, sizeof(operations));
    operations.context = binding;
    operations.snapshot = fake_snapshot;
    operations.sid8_ready = fake_sid8_ready;
    operations.resolve_outgoing = fake_resolve_outgoing;
    operations.resolve_incoming = fake_resolve_incoming;
    operations.command_peek = fake_command_peek;
    operations.command_consume = fake_command_consume;
    operations.gtt_claim = fake_gtt_claim;
    operations.gtt_commit = fake_gtt_commit;
    operations.gtt_cancel = fake_gtt_cancel;
    operations.final_peek = fake_final_peek;
    operations.final_consume = fake_final_consume;
    operations.final_pin_observer = fake_final_pin_observer;
    operations.ingress_take = fake_ingress_take;
    operations.final_publish_local = fake_final_publish_local;
    operations.log_reserve = fake_log_reserve;
    operations.log_commit = fake_log_commit;
    operations.log_cancel = fake_log_cancel;
    operations.ui_publish = fake_ui_publish;
    operations.generic_submit = fake_generic_submit;
    check(mind_root_coordinator_init(coordinator, &local, 7u, 1u, &operations),
          "production coordinator accepts complete typed binding");
}

static int enqueue_command(fake_binding_t *binding, mind_command_kind_t command,
                           mind_command_status_t status)
{
    mind_command_attempt_t attempt;

    attempt.command = command;
    attempt.status = status;
    return mind_command_mailbox_offer(&binding->uart.mailbox, &attempt);
}

static void uart_isr_feed(mind_uart_t *uart, const char *text)
{
    size_t index;

    if (uart == NULL || text == NULL) {
        return;
    }
    for (index = 0u; text[index] != '\0'; index++) {
        mind_uart_isr_push(uart, (uint8_t)text[index]);
    }
}

static void drain_logs(mind_log_queue_t *logs)
{
    mind_log_record_t record;

    while (mind_log_queue_take(logs, &record)) {
    }
}

static int enqueue_record_at_from_sid8(
    fake_binding_t *binding, const mind_application_wire_record_t *record,
    tavrn_identity_width_t origin_width, uint8_t origin_sid8,
    uint32_t delivered_at_ms)
{
    tavrn_direct_peer_t transmitter;
    tavrn_link_data_t data;
    tavrn_router_delivery_token_t token;

    if (binding == NULL || record == NULL) {
        return 0;
    }
    memset(&transmitter, 0, sizeof(transmitter));
    memset(&data, 0, sizeof(data));
    data.origin.width = origin_width;
    data.origin.value = origin_sid8;
    data.app_kind = record->app_kind;
    data.app_source = record->app_source;
    data.urgent = record->urgent;
    data.app_len = record->app_len;
    memcpy(data.app_bytes, record->app_bytes, sizeof(data.app_bytes));
    return mind_root_inbox_reserve(&binding->inbox, &transmitter, &data, &token) ==
            TAVRN_ROUTER_DELIVERY_OK &&
        mind_root_inbox_commit(&binding->inbox, token, &transmitter, &data,
                               delivered_at_ms) ==
            TAVRN_ROUTER_DELIVERY_OK;
}

static int enqueue_record_at(fake_binding_t *binding,
                             const mind_application_wire_record_t *record,
                             tavrn_identity_width_t origin_width,
                             uint32_t delivered_at_ms)
{
    return enqueue_record_at_from_sid8(binding, record, origin_width, 0x44u,
                                       delivered_at_ms);
}

static int enqueue_record(fake_binding_t *binding,
                           const mind_application_wire_record_t *record,
                           tavrn_identity_width_t origin_width)
{
    return enqueue_record_at(binding, record, origin_width, 0u);
}

static int enqueue_record_from_sid8(fake_binding_t *binding,
                                    const mind_application_wire_record_t *record,
                                    uint8_t origin_sid8)
{
    return enqueue_record_at_from_sid8(binding, record, TAVRN_IDENTITY_SID8,
                                       origin_sid8, 0u);
}

static int enqueue_report_ingress(fake_binding_t *binding, uint8_t wearable,
                                  uint32_t packet_id24, uint8_t event_type)
{
    mind_application_ingress_event_t event;
    uint8_t payload[MIND_PAYLOAD_SIZE] = {
        MIND_SCHEMA_VERSION, event_type,
        event_type == MIND_EVT_HEARTBEAT ? 0u : 73u,
        0x1fu, 0x1au, event_type == MIND_EVT_HEARTBEAT ? 0u : 0x7du,
        (uint8_t)(packet_id24 & 0xffu)
    };

    if (binding == NULL || binding->ingress.queue_count ==
            MIND_APPLICATION_INGRESS_QUEUE_CAPACITY) {
        return 0;
    }
    memset(&event, 0, sizeof(event));
    if (mind_application_wire_pack_report(&event.report, wearable, packet_id24,
                                            payload) != MIND_APPLICATION_WIRE_OK) {
        return 0;
    }
    event.rssi_magnitude_db = 41u;
    binding->ingress.queue[binding->ingress.queue_tail] = event;
    binding->ingress.queue_tail = (uint8_t)((binding->ingress.queue_tail + 1u) %
        MIND_APPLICATION_INGRESS_QUEUE_CAPACITY);
    binding->ingress.queue_count++;
    return 1;
}

static int make_report(mind_application_wire_record_t *record, uint8_t wearable,
                       uint32_t packet_id24, uint8_t event_type)
{
    uint8_t payload[MIND_PAYLOAD_SIZE] = {
        MIND_SCHEMA_VERSION, event_type,
        event_type == MIND_EVT_HEARTBEAT ? 0u : 73u,
        0x1fu, 0x1au, event_type == MIND_EVT_HEARTBEAT ? 0u : 0x7du,
        (uint8_t)(packet_id24 & 0xffu)
    };

    return mind_application_wire_pack_report(record, wearable, packet_id24, payload) ==
        MIND_APPLICATION_WIRE_OK;
}

static int make_observed_report(mind_application_wire_record_t *record,
                                uint8_t wearable, uint32_t packet_id24,
                                uint8_t event_type, uint8_t rssi_magnitude_db)
{
    uint8_t payload[MIND_PAYLOAD_SIZE] = {
        MIND_SCHEMA_VERSION, event_type,
        event_type == MIND_EVT_HEARTBEAT ? 0u : 73u,
        0x1fu, 0x1au, event_type == MIND_EVT_HEARTBEAT ? 0u : 0x7du,
        (uint8_t)(packet_id24 & 0xffu)
    };

    return mind_application_wire_pack_observed_report(
        record, wearable, packet_id24, payload, rssi_magnitude_db) ==
        MIND_APPLICATION_WIRE_OK;
}

static int prepare_due(mind_root_coordinator_t *coordinator,
                       mind_root_coordinator_request_t *request_out)
{
    mind_root_coordinator_request_t discarded;

    (void)mind_root_coordinator_prepare(coordinator, 0u, &discarded);
    return mind_root_coordinator_prepare(coordinator, 200u, request_out);
}

static void set_outgoing_identity_statuses(
    fake_binding_t *binding, const tavrn_adva_t *first,
    mind_root_resolver_status_t first_status, const tavrn_adva_t *second,
    mind_root_resolver_status_t second_status)
{
    if (binding == NULL || first == NULL || second == NULL) {
        return;
    }
    binding->outgoing_first_identity = *first;
    binding->outgoing_second_identity = *second;
    binding->outgoing_first_status = first_status;
    binding->outgoing_second_status = second_status;
    binding->outgoing_identity_statuses = 1u;
}

static int enqueue_active_root_state(fake_binding_t *binding, uint8_t sid8)
{
    mind_application_wire_record_t record;
    mind_application_root_state_t state = root_state(1u, (uint16_t)(sid8 + 9u), 1u);

    return mind_application_wire_pack_root_state(&record, &state) ==
            MIND_APPLICATION_WIRE_OK &&
        enqueue_record_from_sid8(binding, &record, sid8);
}

static int activate_remote_root(mind_root_coordinator_t *coordinator,
                                 fake_binding_t *binding, uint8_t sid8,
                                 uint32_t now_ms)
{
    mind_root_coordinator_request_t request;
    mind_application_wire_record_t record;
    mind_application_root_state_t state = root_state(1u, (uint16_t)(sid8 + 9u),
                                                      1u);

    if (coordinator == NULL || binding == NULL ||
        mind_application_wire_pack_root_state(&record, &state) !=
            MIND_APPLICATION_WIRE_OK) {
        return 0;
    }
    binding->incoming_identity_by_sid8 = 1u;
    return enqueue_record_from_sid8(binding, &record, sid8) &&
        mind_root_coordinator_prepare(coordinator, now_ms, &request) &&
        request.token.submission.kind == MIND_ROOT_SUBMISSION_ACK &&
        mind_root_coordinator_submit(coordinator, &request.token, &request.data, now_ms) ==
            MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED;
}

static void test_uart_immediate_off_lifecycle(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;
    mind_uart_snapshot_t snapshot;
    mind_log_record_t record;

    setup(&coordinator, &binding, 0u);
    uart_isr_feed(&binding.uart, "ROOT OFF\r");
    mind_uart_service(&binding.uart);
    mind_uart_snapshot(&binding.uart, &snapshot);
    check(snapshot.bytes_received == 9u && snapshot.commands_parsed == 1u &&
          snapshot.mailbox_publications == 1u && snapshot.production_peeks == 0u &&
          snapshot.production_consumes == 0u && snapshot.error_count == 0u &&
          snapshot.overflow_count == 0u && snapshot.mailbox_full_count == 0u &&
          snapshot.ring_depth == 0u && snapshot.mailbox_depth == 1u &&
          snapshot.pending_valid == 0u,
          "real UART ISR/parser publishes immediate ROOT OFF before coordinator ownership");
    check(!mind_root_coordinator_prepare(&coordinator, 1u, &request) &&
          mind_root_plane_local_active(&coordinator.plane) == 0u,
          "production coordinator consumes immediate ROOT OFF without changing inactive role");
    mind_uart_snapshot(&binding.uart, &snapshot);
    check(snapshot.production_peeks == 1u && snapshot.production_consumes == 1u &&
          snapshot.mailbox_depth == 0u && snapshot.pending_valid == 0u &&
          mind_log_queue_take(&binding.logs, &record) &&
          record.kind == MIND_LOG_COMMAND && record.detail.command.command == MIND_COMMAND_OFF &&
          record.detail.command.status == MIND_COMMAND_DUPLICATE,
          "immediate inactive OFF is transactionally consumed and logged as duplicate");
}

static void test_uart_delayed_off_backpressure_and_duplicate_lifecycle(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;
    mind_uart_snapshot_t snapshot;
    mind_log_reservation_t reservation;
    mind_log_record_t records[MIND_LOG_CAPACITY];
    mind_log_record_t record;

    setup(&coordinator, &binding, 1u);
    uart_isr_feed(&binding.uart, "ROOT ON\r");
    mind_uart_service(&binding.uart);
    check(!mind_root_coordinator_prepare(&coordinator, 1u, &request) &&
          mind_root_plane_local_active(&coordinator.plane) != 0u,
          "real UART ROOT ON reaches the coordinator before delayed OFF");
    mind_uart_snapshot(&binding.uart, &snapshot);
    check(snapshot.bytes_received == 8u && snapshot.commands_parsed == 1u &&
          snapshot.mailbox_publications == 1u && snapshot.production_peeks == 1u &&
          snapshot.production_consumes == 1u && snapshot.mailbox_depth == 0u,
          "ROOT ON lifecycle records parser publication and production transaction once");

    binding.submit_status = MIND_ROOT_COORDINATOR_SUBMIT_BUSY;
    check(mind_root_coordinator_prepare(&coordinator, 200u, &request) &&
          request.token.submission.kind == MIND_ROOT_SUBMISSION_STATE &&
          mind_root_coordinator_submit(&coordinator, &request.token, &request.data, 200u) ==
              MIND_ROOT_COORDINATOR_SUBMIT_BUSY,
          "a BUSY root-state campaign retains its exact owner before delayed OFF");

    drain_logs(&binding.logs);
    memset(records, 0, sizeof(records));
    check(mind_log_queue_reserve(&binding.logs, MIND_LOG_CAPACITY - 1u, &reservation) &&
          mind_log_queue_commit(&binding.logs, &reservation, records),
          "logger fixture leaves one slot, insufficient for a ROOT OFF transaction");
    uart_isr_feed(&binding.uart, "ROOT OFF\r");
    mind_uart_service(&binding.uart);
    mind_uart_snapshot(&binding.uart, &snapshot);
    check(snapshot.bytes_received == 17u && snapshot.commands_parsed == 2u &&
          snapshot.mailbox_publications == 2u && snapshot.production_peeks == 1u &&
          snapshot.production_consumes == 1u && snapshot.ring_depth == 0u &&
          snapshot.mailbox_depth == 1u && snapshot.pending_valid == 0u,
          "delayed OFF is published once and remains at the production mailbox head");
    check(mind_root_coordinator_prepare(&coordinator, 300u, &request) &&
          request.token.submission.kind == MIND_ROOT_SUBMISSION_STATE &&
          mind_root_coordinator_submit(&coordinator, &request.token, &request.data, 300u) ==
              MIND_ROOT_COORDINATOR_SUBMIT_BUSY &&
          mind_root_plane_local_active(&coordinator.plane) != 0u,
          "logger pressure retains delayed OFF while independently due BUSY work completes");
    mind_uart_snapshot(&binding.uart, &snapshot);
    check(snapshot.production_peeks == 2u && snapshot.production_consumes == 1u &&
          snapshot.mailbox_depth == 1u,
          "blocked logger retry adds peek evidence without consuming delayed OFF");

    check(mind_log_queue_take(&binding.logs, &record),
          "logger fixture releases exactly one reservation slot");
    check(!mind_root_coordinator_prepare(&coordinator, 301u, &request) &&
          mind_root_plane_local_active(&coordinator.plane) == 0u,
          "released logger capacity commits delayed ROOT OFF exactly once");
    mind_uart_snapshot(&binding.uart, &snapshot);
    check(snapshot.production_peeks == 3u && snapshot.production_consumes == 2u &&
          snapshot.mailbox_depth == 0u && snapshot.pending_valid == 0u,
          "recovered delayed OFF has matching production peek/consume evidence");

    drain_logs(&binding.logs);
    binding.submit_status = MIND_ROOT_COORDINATOR_SUBMIT_NOT_READY;
    check(mind_root_coordinator_prepare(&coordinator, 1000u, &request) &&
          request.token.submission.kind == MIND_ROOT_SUBMISSION_STATE &&
          mind_root_coordinator_submit(&coordinator, &request.token, &request.data, 1000u) ==
              MIND_ROOT_COORDINATOR_SUBMIT_NOT_READY,
          "NOT_READY campaign work retains the new OFF state after UART consumption");

    uart_isr_feed(&binding.uart, "ROOT OFF\r");
    mind_uart_service(&binding.uart);
    check(!mind_root_coordinator_prepare(&coordinator, 1001u, &request) &&
          mind_root_plane_local_active(&coordinator.plane) == 0u,
          "repeated OFF stays inactive while the campaign remains NOT_READY");
    mind_uart_snapshot(&binding.uart, &snapshot);
    check(snapshot.bytes_received == 26u && snapshot.commands_parsed == 3u &&
          snapshot.mailbox_publications == 3u && snapshot.production_peeks == 4u &&
          snapshot.production_consumes == 3u && snapshot.error_count == 0u &&
          snapshot.overflow_count == 0u && snapshot.mailbox_full_count == 0u &&
          snapshot.ring_depth == 0u && snapshot.mailbox_depth == 0u &&
          snapshot.pending_valid == 0u,
          "repeated OFF leaves bounded lifecycle evidence with no UART fault state");
    check(mind_log_queue_take(&binding.logs, &record) &&
          record.kind == MIND_LOG_COMMAND && record.detail.command.command == MIND_COMMAND_OFF &&
          record.detail.command.status == MIND_COMMAND_DUPLICATE,
          "repeated inactive OFF is logged as duplicate without a ROOT OFF behavior fix");
}

static void test_snapshot_and_pre_sid8_command(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;

    setup(&coordinator, &binding, 1u);
    binding.snapshot_ok = 0u;
    check(!mind_root_coordinator_prepare(&coordinator, 1u, &request) &&
          coordinator.plane.campaigns[0].occupied == 0u &&
          coordinator.plane.counters.snapshot_failures == 1u,
          "non-OK copied topology preserves Layer-7 state");
    binding.snapshot_ok = 1u;
    binding.sid8_ready = 0u;
    check(enqueue_command(&binding, MIND_COMMAND_ON, MIND_COMMAND_ACCEPTED) &&
          !mind_root_coordinator_prepare(&coordinator, 2u, &request) &&
          mind_root_plane_local_active(&coordinator.plane) != 0u &&
          coordinator.plane.campaigns[0].occupied != 0u &&
          binding.ui_local_active != 0u,
          "pre-SID8 ROOT ON changes local role after topology copy but defers campaign");
    binding.sid8_ready = 1u;
    check(prepare_due(&coordinator, &request) &&
          request.token.submission.kind == MIND_ROOT_SUBMISSION_STATE,
          "later SID8 readiness prepares the retained local campaign");
}

static void test_logger_transaction_and_status(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;
    mind_log_reservation_t reservation;
    mind_log_record_t records[MIND_LOG_CAPACITY];
    mind_log_record_t record;

    setup(&coordinator, &binding, 0u);
    memset(records, 0, sizeof(records));
    check(enqueue_command(&binding, MIND_COMMAND_ON, MIND_COMMAND_ACCEPTED) &&
          mind_log_queue_reserve(&binding.logs, MIND_LOG_CAPACITY - 1u, &reservation) &&
          mind_log_queue_commit(&binding.logs, &reservation, records) &&
          !mind_root_coordinator_prepare(&coordinator, 1u, &request) &&
           mind_uart_command_peek(&binding.uart, &(mind_command_attempt_t){ 0 }) &&
          !mind_root_plane_local_active(&coordinator.plane),
          "logger BUSY retains ROOT ON without state or cue publication");
    check(mind_log_queue_take(&binding.logs, &record) &&
          mind_log_queue_take(&binding.logs, &record) &&
          !mind_root_coordinator_prepare(&coordinator, 2u, &request) &&
          mind_root_plane_local_active(&coordinator.plane) != 0u,
          "released logger capacity commits ROOT ON transactionally");
    while (mind_log_queue_take(&binding.logs, &record)) {
    }
    check(enqueue_command(&binding, MIND_COMMAND_STATUS, MIND_COMMAND_ACCEPTED) &&
          !mind_root_coordinator_prepare(&coordinator, 3u, &request) &&
          mind_log_queue_take(&binding.logs, &record) &&
          record.kind == MIND_LOG_COMMAND &&
          record.detail.command.command == MIND_COMMAND_STATUS &&
          record.detail.command.status == MIND_COMMAND_ACCEPTED &&
          mind_log_queue_take(&binding.logs, &record) && record.kind == MIND_LOG_ROOT,
          "ROOT STATUS produces exact command and root records without changing role");
}

static void test_pending_ownership_and_submit_results(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;
    mind_root_coordinator_token_t wrong;
    tron_application_data_t wrong_data;
    uint32_t due_at;

    setup(&coordinator, &binding, 1u);
    check(prepare_due(&coordinator, &request), "due topology target prepares one request");
    due_at = coordinator.plane.campaigns[0].due_at_ms;
    check(!mind_root_coordinator_prepare(&coordinator, 201u, &request) &&
          mind_root_coordinator_pending(&coordinator) != NULL,
          "one pending root token prevents a second application submit in the phase");
    wrong = request.token;
    wrong.submission.kind = MIND_ROOT_SUBMISSION_ACK;
    check(!mind_root_coordinator_complete(&coordinator, &wrong,
                                          MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED, 200u) &&
          mind_root_coordinator_pending(&coordinator) != NULL,
          "wrong completion kind cannot mutate another root obligation");
    wrong = request.token;
    wrong.submission.slot++;
    check(!mind_root_coordinator_complete(&coordinator, &wrong,
                                          MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED, 200u) &&
          mind_root_coordinator_pending(&coordinator) != NULL,
          "wrong completion slot cannot mutate another root obligation");
    wrong = request.token;
    wrong.submission.target_slot++;
    check(!mind_root_coordinator_complete(&coordinator, &wrong,
                                          MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED, 200u) &&
          mind_root_coordinator_pending(&coordinator) != NULL,
          "wrong completion target-slot cannot mutate another root obligation");
    wrong = request.token;
    wrong.submission.target = identity(9u);
    check(!mind_root_coordinator_complete(&coordinator, &wrong,
                                          MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED, 200u) &&
          mind_root_coordinator_pending(&coordinator) != NULL &&
          coordinator.plane.campaigns[0].due_at_ms == due_at,
          "wrong completion target cannot mutate another root obligation");
    wrong = request.token;
    wrong.submission.record.app_bytes[0] ^= 0x01u;
    check(!mind_root_coordinator_complete(&coordinator, &wrong,
                                          MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED, 200u) &&
          mind_root_coordinator_pending(&coordinator) != NULL,
          "wrong completion record cannot mutate another root obligation");
    wrong = request.token;
    wrong.sequence++;
    check(!mind_root_coordinator_complete(&coordinator, &wrong,
                                          MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED, 200u) &&
          mind_root_coordinator_pending(&coordinator) != NULL,
          "wrong completion sequence cannot mutate another root obligation");
    wrong_data = request.data;
    wrong_data.app_kind = ROOT_ACK;
    check(mind_root_coordinator_submit(&coordinator, &request.token, &wrong_data, 200u) ==
              MIND_ROOT_COORDINATOR_SUBMIT_INVALID &&
          binding.submit_calls == 0u && mind_root_coordinator_pending(&coordinator) != NULL,
          "wrong generic DATA cannot consume the exact pending owner");
    check(mind_root_coordinator_submit(&coordinator, &request.token, &request.data, 200u) ==
              MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED &&
          binding.submit_calls == 1u && mind_root_coordinator_pending(&coordinator) == NULL &&
          coordinator.plane.counters.submissions == 1u,
          "accepted generic completion owns and releases exactly one token");
    check(mind_root_coordinator_submit(&coordinator, &request.token, &request.data, 201u) ==
              MIND_ROOT_COORDINATOR_SUBMIT_INVALID && binding.submit_calls == 1u,
          "stale generic completion cannot submit a released token again");

    setup(&coordinator, &binding, 1u);
    binding.submit_status = MIND_ROOT_COORDINATOR_SUBMIT_BUSY;
    check(prepare_due(&coordinator, &request) &&
          mind_root_coordinator_submit(&coordinator, &request.token, &request.data, 200u) ==
              MIND_ROOT_COORDINATOR_SUBMIT_BUSY &&
          binding.submit_calls == 1u && coordinator.plane.counters.submission_busy == 1u &&
          coordinator.plane.counters.root_state_submit_invoked == 1u,
          "BUSY generic completion records one invoked submission and retains campaign");
}

static void test_event_submit_status_paths_and_target_ownership(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;
    tavrn_adva_t first = identity(1u);
    tavrn_adva_t second = identity(2u);

    setup(&coordinator, &binding, 0u);
    binding.snapshot.entry_count = 2u;
    binding.snapshot.entries[0].canonical_adva = first;
    binding.snapshot.entries[0].departed = ROUTED_CYCLE_BOOLEAN_FALSE;
    binding.snapshot.entries[1].canonical_adva = second;
    binding.snapshot.entries[1].departed = ROUTED_CYCLE_BOOLEAN_FALSE;
    check(activate_remote_root(&coordinator, &binding, 1u, 1u) &&
              activate_remote_root(&coordinator, &binding, 2u, 2u) &&
              mind_root_plane_active_count(&coordinator.plane) == 2u,
          "production coordinator activates two copied remote roots for event fanout");
    binding.submit_calls = 0u;
    check(enqueue_report_ingress(&binding, 7u, 0x730001u, MIND_EVT_FALL_AND_SHOUT) &&
              enqueue_report_ingress(&binding, 8u, 0x730002u, MIND_EVT_FALL_AND_SHOUT),
          "two urgent ingress reports await one-submit coordinator ownership");

    binding.submit_status = MIND_ROOT_COORDINATOR_SUBMIT_BUSY;
    check(mind_root_coordinator_prepare(&coordinator, 200u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_EVENT &&
              request.token.submission.slot == 0u &&
              request.token.submission.target_slot == 0u &&
              memcmp(request.token.submission.target.bytes, first.bytes,
                     TAVRN_ADVA_LEN) == 0 &&
              mind_root_coordinator_submit(&coordinator, &request.token, &request.data,
                                           200u) == MIND_ROOT_COORDINATOR_SUBMIT_BUSY &&
              mind_root_coordinator_pending(&coordinator) == NULL &&
              coordinator.events.events[0].targets[0].state == MIND_EVENT_TARGET_PENDING &&
              coordinator.events.events[0].target_cursor == 1u,
          "BUSY releases only its exact event owner and advances that event target cursor");

    binding.submit_status = MIND_ROOT_COORDINATOR_SUBMIT_NOT_READY;
    check(mind_root_coordinator_prepare(&coordinator, 201u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_EVENT &&
              request.token.submission.slot == 1u &&
              request.token.submission.target_slot == 0u &&
              memcmp(request.token.submission.target.bytes, first.bytes,
                     TAVRN_ADVA_LEN) == 0 &&
              mind_root_coordinator_submit(&coordinator, &request.token, &request.data,
                                           201u) == MIND_ROOT_COORDINATOR_SUBMIT_NOT_READY &&
              mind_root_coordinator_pending(&coordinator) == NULL &&
              coordinator.events.events[1].targets[0].state == MIND_EVENT_TARGET_PENDING &&
              coordinator.events.events[1].target_cursor == 1u,
          "NOT_READY retains and advances the second event without changing the first owner");

    binding.submit_status = MIND_ROOT_COORDINATOR_SUBMIT_QUEUED;
    check(mind_root_coordinator_prepare(&coordinator, 202u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_EVENT &&
              request.token.submission.slot == 0u &&
              request.token.submission.target_slot == 1u &&
              memcmp(request.token.submission.target.bytes, second.bytes,
                     TAVRN_ADVA_LEN) == 0 &&
              mind_root_coordinator_submit(&coordinator, &request.token, &request.data,
                                           202u) == MIND_ROOT_COORDINATOR_SUBMIT_QUEUED &&
              mind_root_coordinator_pending(&coordinator) == NULL &&
              coordinator.events.events[0].targets[0].state == MIND_EVENT_TARGET_PENDING &&
              coordinator.events.events[0].targets[1].state == MIND_EVENT_TARGET_COMPLETE &&
              coordinator.events.events[0].target_cursor == 0u,
          "QUEUED completes the first event alternate target after BUSY retention");
    check(mind_root_coordinator_prepare(&coordinator, 203u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_EVENT &&
              request.token.submission.slot == 1u &&
              request.token.submission.target_slot == 1u &&
              memcmp(request.token.submission.target.bytes, second.bytes,
                     TAVRN_ADVA_LEN) == 0 &&
              mind_root_coordinator_submit(&coordinator, &request.token, &request.data,
                                           203u) == MIND_ROOT_COORDINATOR_SUBMIT_QUEUED &&
              mind_root_coordinator_pending(&coordinator) == NULL &&
              coordinator.events.events[1].targets[0].state == MIND_EVENT_TARGET_PENDING &&
              coordinator.events.events[1].targets[1].state == MIND_EVENT_TARGET_COMPLETE &&
              coordinator.events.events[1].target_cursor == 0u &&
              binding.submit_calls == 4u,
          "queued alternate completion preserves both retained BUSY/NOT_READY targets");
}

static void test_outgoing_resolver_matrix(void)
{
    const mind_root_resolver_status_t statuses[] = {
        MIND_ROOT_RESOLVER_UNIQUE,
        MIND_ROOT_RESOLVER_UNKNOWN,
        MIND_ROOT_RESOLVER_COLLIDING,
        MIND_ROOT_RESOLVER_RESERVED,
        MIND_ROOT_RESOLVER_INVALID,
    };
    uint8_t index;

    for (index = 0u; index < sizeof(statuses) / sizeof(statuses[0]); index++) {
        mind_root_coordinator_t coordinator;
        fake_binding_t binding;
        mind_root_coordinator_request_t request;

        setup(&coordinator, &binding, 1u);
        binding.outgoing_status = statuses[index];
        if (statuses[index] == MIND_ROOT_RESOLVER_UNIQUE) {
            check(prepare_due(&coordinator, &request),
                  "UNIQUE outgoing resolver permits coordinator submit preparation");
        } else {
            check(!prepare_due(&coordinator, &request) && binding.submit_calls == 0u &&
                  coordinator.plane.campaigns[0].occupied != 0u,
                  "non-unique/fail-closed resolver retains root campaign without submit");
            if (statuses[index] == MIND_ROOT_RESOLVER_UNKNOWN) {
                check(coordinator.plane.counters.resolver_unknown == 1u &&
                      coordinator.plane.campaigns[0].resolver_blocked != 0u,
                      "UNKNOWN outgoing resolution is retained for rediscovery");
            } else if (statuses[index] == MIND_ROOT_RESOLVER_COLLIDING) {
                check(coordinator.plane.counters.resolver_colliding == 1u &&
                      coordinator.plane.campaigns[0].resolver_blocked != 0u,
                      "COLLIDING outgoing resolution is retained for rediscovery");
            } else if (statuses[index] == MIND_ROOT_RESOLVER_RESERVED) {
                check(coordinator.plane.counters.resolver_reserved == 1u &&
                      coordinator.plane.campaigns[0].resolver_failed_closed != 0u,
                      "RESERVED outgoing resolution fails closed in Layer 7");
            } else {
                check(coordinator.plane.counters.resolver_invalid == 1u &&
                      coordinator.plane.campaigns[0].resolver_failed_closed != 0u,
                      "INVALID outgoing resolution fails closed in Layer 7");
            }
        }
    }
}

static void test_ack_resolution_skips_blocked_owner(void)
{
    const mind_root_resolver_status_t blocked_statuses[] = {
        MIND_ROOT_RESOLVER_UNKNOWN,
        MIND_ROOT_RESOLVER_COLLIDING,
    };
    uint8_t index;

    for (index = 0u; index < sizeof(blocked_statuses) / sizeof(blocked_statuses[0]);
         index++) {
        mind_root_coordinator_t coordinator;
        fake_binding_t binding;
        mind_root_coordinator_request_t request;
        tavrn_adva_t first = identity(1u);
        tavrn_adva_t second = identity(2u);

        setup(&coordinator, &binding, 0u);
        binding.snapshot.entry_count = 2u;
        binding.snapshot.entries[0].canonical_adva = first;
        binding.snapshot.entries[0].departed = ROUTED_CYCLE_BOOLEAN_FALSE;
        binding.snapshot.entries[1].canonical_adva = second;
        binding.snapshot.entries[1].departed = ROUTED_CYCLE_BOOLEAN_FALSE;
        binding.incoming_identity_by_sid8 = 1u;
        set_outgoing_identity_statuses(&binding, &first, blocked_statuses[index],
                                       &second, MIND_ROOT_RESOLVER_UNIQUE);
        check(enqueue_active_root_state(&binding, 1u) &&
                  enqueue_active_root_state(&binding, 2u) &&
                  mind_root_coordinator_prepare(&coordinator, 200u, &request) &&
                  request.token.submission.kind == MIND_ROOT_SUBMISSION_ACK &&
                  request.token.submission.slot == 1u &&
                  memcmp(request.token.submission.target.bytes, second.bytes,
                         TAVRN_ADVA_LEN) == 0 &&
                  coordinator.plane.acknowledgements[0].occupied != 0u &&
                  coordinator.plane.acknowledgements[0].resolver_blocked != 0u &&
                  coordinator.plane.acknowledgements[1].resolver_blocked == 0u &&
                  coordinator.plane.acknowledgements[1].resolver_failed_closed == 0u &&
                  binding.outgoing_resolve_calls == 2u && binding.submit_calls == 0u,
              "UNKNOWN/COLLIDING ACK owner cannot starve the next unique ACK owner");
    }
}

static void test_ack_resolution_skips_fail_closed_owner(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;
    tavrn_adva_t first = identity(1u);
    tavrn_adva_t second = identity(2u);

    setup(&coordinator, &binding, 0u);
    binding.snapshot.entry_count = 2u;
    binding.snapshot.entries[0].canonical_adva = first;
    binding.snapshot.entries[0].departed = ROUTED_CYCLE_BOOLEAN_FALSE;
    binding.snapshot.entries[1].canonical_adva = second;
    binding.snapshot.entries[1].departed = ROUTED_CYCLE_BOOLEAN_FALSE;
    binding.incoming_identity_by_sid8 = 1u;
    set_outgoing_identity_statuses(&binding, &first, MIND_ROOT_RESOLVER_RESERVED,
                                   &second, MIND_ROOT_RESOLVER_UNIQUE);
    check(enqueue_active_root_state(&binding, 1u) &&
              enqueue_active_root_state(&binding, 2u) &&
              mind_root_coordinator_prepare(&coordinator, 200u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_ACK &&
              request.token.submission.slot == 1u &&
              coordinator.plane.acknowledgements[0].resolver_failed_closed != 0u &&
              coordinator.plane.acknowledgements[0].resolver_blocked == 0u &&
              coordinator.plane.acknowledgements[1].resolver_failed_closed == 0u &&
              binding.outgoing_resolve_calls == 2u,
          "fail-closed ACK owner cannot starve the next unique ACK owner");
}

static void test_state_resolution_skips_blocked_owner_without_pacing_change(void)
{
    const mind_root_resolver_status_t blocked_statuses[] = {
        MIND_ROOT_RESOLVER_UNKNOWN,
        MIND_ROOT_RESOLVER_COLLIDING,
    };
    uint8_t index;

    for (index = 0u; index < sizeof(blocked_statuses) / sizeof(blocked_statuses[0]);
         index++) {
        mind_root_coordinator_t coordinator;
        fake_binding_t binding;
        mind_root_coordinator_request_t request;
        tavrn_adva_t first = identity(1u);
        tavrn_adva_t second = identity(2u);

        setup(&coordinator, &binding, 0u);
        binding.snapshot.entry_count = 2u;
        binding.snapshot.entries[0].canonical_adva = first;
        binding.snapshot.entries[0].departed = ROUTED_CYCLE_BOOLEAN_FALSE;
        binding.snapshot.entries[1].canonical_adva = second;
        binding.snapshot.entries[1].departed = ROUTED_CYCLE_BOOLEAN_FALSE;
        set_outgoing_identity_statuses(&binding, &first, blocked_statuses[index],
                                       &second, MIND_ROOT_RESOLVER_UNIQUE);
        check(enqueue_command(&binding, MIND_COMMAND_ON, MIND_COMMAND_ACCEPTED) &&
                  !mind_root_coordinator_prepare(&coordinator, 1u, &request) &&
                  mind_root_coordinator_prepare(&coordinator, 200u, &request) &&
                  request.token.submission.kind == MIND_ROOT_SUBMISSION_STATE &&
                  request.token.submission.slot == 1u &&
                  memcmp(request.token.submission.target.bytes, second.bytes,
                         TAVRN_ADVA_LEN) == 0 &&
                  coordinator.plane.campaigns[0].resolver_blocked != 0u &&
                  coordinator.plane.campaigns[1].resolver_blocked == 0u &&
                  coordinator.plane.root_state_submission_seen == 0u &&
                  coordinator.plane.last_root_state_invoked_ms == 0u &&
                  binding.outgoing_resolve_calls == 2u && binding.submit_calls == 0u,
              "UNKNOWN/COLLIDING state owner leaves pacing unchanged for next unique owner");
    }
}

static void test_root_resolution_scans_are_bounded(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;
    mind_application_root_state_t local;
    uint8_t index;

    setup(&coordinator, &binding, 0u);
    local = mind_root_plane_local_state(&coordinator.plane);
    for (index = 0u; index < MIND_APP_ROOT_ACK_CAPACITY; index++) {
        mind_root_ack_obligation_t *ack = &coordinator.plane.acknowledgements[index];

        ack->canonical_adva = identity((uint8_t)(index + 1u));
        ack->ack.state = local;
        ack->ack.status = MIND_APPLICATION_ROOT_ACK_ACCEPTED;
        ack->occupied = 1u;
    }
    binding.outgoing_status = MIND_ROOT_RESOLVER_UNKNOWN;
    check(!mind_root_coordinator_prepare(&coordinator, 1u, &request) &&
              mind_root_coordinator_pending(&coordinator) == NULL &&
              binding.outgoing_resolve_calls == MIND_APP_ROOT_ACK_CAPACITY &&
              coordinator.plane.acknowledgements[0].resolver_blocked != 0u &&
              coordinator.plane.acknowledgements[MIND_APP_ROOT_ACK_CAPACITY - 1u]
                  .resolver_blocked != 0u,
          "all unresolved ACK owners stop at their independent capacity");
    binding.snapshot_ok = 0u;
    check(!mind_root_coordinator_prepare(&coordinator, 2u, &request) &&
              binding.outgoing_resolve_calls == MIND_APP_ROOT_ACK_CAPACITY,
          "no eligible ACK owner returns cleanly without another resolution attempt");

    setup(&coordinator, &binding, 0u);
    check(!mind_root_coordinator_prepare(&coordinator, 0u, &request),
          "empty coordinator cycle establishes SID8 readiness before state bound test");
    for (index = 0u; index < MIND_APP_CAMPAIGN_CAPACITY; index++) {
        mind_root_campaign_target_t *target = &coordinator.plane.campaigns[index];

        target->canonical_adva = identity((uint8_t)(index + 1u));
        target->due_at_ms = 0u;
        target->occupied = 1u;
    }
    binding.outgoing_status = MIND_ROOT_RESOLVER_COLLIDING;
    check(!mind_root_coordinator_prepare(&coordinator, 1u, &request) &&
              mind_root_coordinator_pending(&coordinator) == NULL &&
              binding.outgoing_resolve_calls == MIND_APP_CAMPAIGN_CAPACITY &&
              coordinator.plane.campaigns[0].resolver_blocked != 0u &&
              coordinator.plane.campaigns[MIND_APP_CAMPAIGN_CAPACITY - 1u]
                  .resolver_blocked != 0u &&
              coordinator.plane.root_state_submission_seen == 0u,
          "all unresolved state owners stop at their independent capacity");
    check(!mind_root_coordinator_prepare(&coordinator, 2u, &request) &&
              binding.outgoing_resolve_calls == MIND_APP_CAMPAIGN_CAPACITY,
          "no eligible state owner returns cleanly without another resolution attempt");
}

static void test_final_validation_and_incoming_correlation(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;
    mind_application_wire_record_t record;
    mind_application_root_state_t state = root_state(1u, 9u, 1u);
    mind_application_root_ack_t ack;
    tavrn_adva_t wrong_sender = identity(2u);

    setup(&coordinator, &binding, 0u);
    memset(&record, 0, sizeof(record));
    record.app_kind = ROOT_STATE;
    record.app_len = 1u;
    check(enqueue_record(&binding, &record, TAVRN_IDENTITY_SID8),
          "malformed ROOT_STATE enters the generic final inbox");
    record.app_kind = ROOT_ACK;
    check(enqueue_record(&binding, &record, TAVRN_IDENTITY_SID8) &&
          !mind_root_coordinator_prepare(&coordinator, 1u, &request) &&
          coordinator.plane.acknowledgements[0].occupied == 0u &&
          mind_root_plane_active_count(&coordinator.plane) == 0u,
          "malformed ROOT_STATE and ACK consume without ACK or root mutation");
    check(mind_application_wire_pack_root_state(&record, &state) ==
              MIND_APPLICATION_WIRE_OK,
          "valid ROOT_STATE packs through the Layer-7 wire module");
    binding.incoming_status = MIND_ROOT_RESOLVER_UNKNOWN;
    check(enqueue_record(&binding, &record, TAVRN_IDENTITY_SID8) &&
              !mind_root_coordinator_prepare(&coordinator, 2u, &request) &&
              coordinator.plane.acknowledgements[0].occupied == 0u &&
              binding.inbox.count == 1u,
          "unresolved final ROOT_STATE retains accepted DATA without creating an ACK");
    binding.incoming_status = MIND_ROOT_RESOLVER_UNIQUE;
    binding.incoming_identity = identity(1u);
    check(mind_root_coordinator_prepare(&coordinator, 3u, &request) &&
          request.token.submission.kind == MIND_ROOT_SUBMISSION_ACK &&
          coordinator.plane.acknowledgements[0].occupied != 0u &&
          mind_root_plane_active_count(&coordinator.plane) == 1u,
          "resolved final ROOT_STATE applies only after injected sender resolution");

    setup(&coordinator, &binding, 1u);
    check(prepare_due(&coordinator, &request) &&
          mind_root_coordinator_submit(&coordinator, &request.token, &request.data, 200u) ==
              MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED,
          "campaign handoff precedes incoming ACK correlation");
    ack.state = mind_root_plane_local_state(&coordinator.plane);
    ack.status = MIND_APPLICATION_ROOT_ACK_ACCEPTED;
    check(mind_application_wire_pack_root_ack(&record, &ack) == MIND_APPLICATION_WIRE_OK,
          "valid ROOT_ACK packs through the Layer-7 wire module");
    binding.incoming_identity = wrong_sender;
    check(enqueue_record(&binding, &record, TAVRN_IDENTITY_SID8) &&
          !mind_root_coordinator_prepare(&coordinator, 201u, &request) &&
          coordinator.plane.campaigns[0].completed == 0u,
          "wrong resolved ACK sender cannot complete another campaign target");
    binding.incoming_identity = identity(1u);
    ack.state.generation++;
    check(mind_application_wire_pack_root_ack(&record, &ack) == MIND_APPLICATION_WIRE_OK &&
          enqueue_record(&binding, &record, TAVRN_IDENTITY_SID8) &&
          !mind_root_coordinator_prepare(&coordinator, 202u, &request) &&
          coordinator.plane.campaigns[0].completed == 0u,
          "wrong echoed ACK tuple cannot complete campaign ownership");
    ack.state = mind_root_plane_local_state(&coordinator.plane);
    check(mind_application_wire_pack_root_ack(&record, &ack) == MIND_APPLICATION_WIRE_OK &&
          enqueue_record(&binding, &record, TAVRN_IDENTITY_SID8) &&
          !mind_root_coordinator_prepare(&coordinator, 203u, &request) &&
          coordinator.plane.campaigns[0].completed != 0u,
          "valid resolved sender and exact ACK tuple complete production campaign");
}

static void test_event_final_logger_retention_and_observers(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;
    mind_application_wire_record_t report;
    mind_log_reservation_t reservation;
    mind_log_record_t records[MIND_LOG_CAPACITY];
    mind_log_record_t first;
    mind_log_record_t second;
    tavrn_adva_t observer_one = identity(1u);
    tavrn_adva_t observer_two = identity(2u);
    tavrn_direct_peer_t transmitter;
    tavrn_link_data_t unknown;
    tavrn_router_delivery_token_t token;
    uint32_t packet_id24;
    uint8_t index;

    setup(&coordinator, &binding, 0u);
    check(make_observed_report(&report, 7u, 0xa1b2c3u,
                               MIND_EVT_FALL_AND_SHOUT, 37u) &&
              enqueue_record_at(&binding, &report, TAVRN_IDENTITY_SID8, 10u) &&
              !mind_root_coordinator_prepare(&coordinator, 10u, &request) &&
              mind_log_queue_take(&binding.logs, &first) && first.kind == MIND_LOG_EVENT &&
               first.detail.event.wearable == 7u &&
               first.detail.event.report.packet_id24 == 0xa1b2c3u &&
               first.detail.event.report.rssi_magnitude_db == 37u &&
               first.detail.event.path_local == 0u &&
              first.now_ms == 10u && binding.incoming_resolve_now_ms == 10u &&
              memcmp(first.detail.event.observer.bytes, observer_one.bytes,
                     TAVRN_ADVA_LEN) == 0,
          "validated routed observed report reserves one logger record with RSSI evidence");
    packet_id24 = (uint32_t)report.app_bytes[0] |
        ((uint32_t)report.app_bytes[1] << 8) |
        ((uint32_t)report.app_bytes[2] << 16);

    memset(records, 0, sizeof(records));
    for (index = 0u; index < MIND_LOG_CAPACITY; index++) {
        records[index].kind = MIND_LOG_COMMAND;
    }
    check(mind_log_queue_reserve(&binding.logs, MIND_LOG_CAPACITY, &reservation) &&
              mind_log_queue_commit(&binding.logs, &reservation, records) &&
              enqueue_record_at(&binding, &report, TAVRN_IDENTITY_SID8, 55u) &&
              !mind_root_coordinator_prepare(&coordinator, 11u, &request) &&
              binding.inbox.count == 1u && coordinator.counters.final_logger_busy == 1u,
          "logger full retains committed final observed report without releasing it");
    binding.incoming_identity = observer_two;
    while (mind_log_queue_take(&binding.logs, &first)) {
    }
    check(!mind_root_coordinator_prepare(&coordinator, 1000u, &request) &&
              binding.inbox.count == 0u && mind_log_queue_take(&binding.logs, &first) &&
              first.kind == MIND_LOG_EVENT,
          "logger recovery processes the retained event record");
    check(first.now_ms == 55u,
          "logger recovery retains the original delivery timestamp");
    check(memcmp(first.detail.event.observer.bytes, observer_one.bytes,
                 TAVRN_ADVA_LEN) == 0,
          "logger recovery retains the first resolved observer");
    check(binding.incoming_resolve_calls == 2u,
          "logger recovery does not re-resolve a pinned routed observer");

    binding.incoming_status = MIND_ROOT_RESOLVER_UNKNOWN;
    check(enqueue_record(&binding, &report, TAVRN_IDENTITY_SID8) &&
              !mind_root_coordinator_prepare(&coordinator, 13u, &request) &&
              binding.inbox.count == 1u && !mind_log_queue_take(&binding.logs, &first),
          "wrong incoming resolver retains routed report with no event line");
    binding.incoming_status = MIND_ROOT_RESOLVER_UNIQUE;
    check(!mind_root_coordinator_prepare(&coordinator, 14u, &request) &&
              mind_log_queue_take(&binding.logs, &first) && first.kind == MIND_LOG_EVENT,
          "later resolver recovery processes the previously accepted report once");

    memset(&transmitter, 0, sizeof(transmitter));
    memset(&unknown, 0, sizeof(unknown));
    unknown.origin.width = TAVRN_IDENTITY_SID8;
    unknown.origin.value = 0x44u;
    unknown.app_kind = 0x7fu;
    check(mind_root_inbox_reserve(&binding.inbox, &transmitter, &unknown, &token) ==
              TAVRN_ROUTER_DELIVERY_OK &&
              mind_root_inbox_commit(&binding.inbox, token, &transmitter, &unknown, 0u) ==
                  TAVRN_ROUTER_DELIVERY_OK &&
              !mind_root_coordinator_prepare(&coordinator, 15u, &request) &&
              coordinator.counters.final_rejected != 0u &&
              !mind_log_queue_take(&binding.logs, &first),
          "unknown accepted generic final DATA has no root or event side effect");

    binding.incoming_identity = observer_one;
    check(enqueue_record(&binding, &report, TAVRN_IDENTITY_SID8) &&
              !mind_root_coordinator_prepare(&coordinator, 16u, &request) &&
              mind_log_queue_take(&binding.logs, &first) && first.kind == MIND_LOG_EVENT,
          "first observer receives a distinct application record");
    binding.incoming_identity = observer_two;
    check(enqueue_record(&binding, &report, TAVRN_IDENTITY_SID8) &&
              !mind_root_coordinator_prepare(&coordinator, 17u, &request) &&
              mind_log_queue_take(&binding.logs, &second) && second.kind == MIND_LOG_EVENT &&
              first.detail.event.report.packet_id24 == second.detail.event.report.packet_id24 &&
              memcmp(first.detail.event.observer.bytes, second.detail.event.observer.bytes,
                     TAVRN_ADVA_LEN) != 0,
          "same dashboard key from different observers retains two exact records");

    check(mind_root_inbox_publish_local(&binding.inbox, &report,
                                         &coordinator.plane.local_adva, 17u) ==
              TAVRN_ROUTER_DELIVERY_OK &&
              !mind_root_coordinator_prepare(&coordinator, 18u, &request) &&
              mind_log_queue_take(&binding.logs, &first) &&
              first.kind == MIND_LOG_EVENT && first.detail.event.path_local != 0u &&
              first.detail.event.report.packet_id24 == packet_id24,
          "direct-local and routed delivery share the validated report record contract");
}

static void test_malformed_observed_reports_reject_without_event_log(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;
    mind_application_wire_record_t report;
    mind_log_record_t logged;

    setup(&coordinator, &binding, 0u);
    check(make_observed_report(&report, 7u, 0x010203u,
                               MIND_EVT_FALL_AND_SHOUT, 41u),
          "malformed-observed schema fixture first packs a valid record");
    report.app_bytes[3] = (uint8_t)(MIND_SCHEMA_VERSION + 1u);
    check(enqueue_record_at(&binding, &report, TAVRN_IDENTITY_SID8, 10u) &&
              !mind_root_coordinator_prepare(&coordinator, 10u, &request) &&
              binding.inbox.count == 0u && coordinator.counters.final_rejected == 1u &&
              !mind_log_queue_take(&binding.logs, &logged),
          "malformed observed schema is consumed as final-rejected without an event or v2 log record");

    check(make_observed_report(&report, 8u, 0x040506u,
                               MIND_EVT_FALL_AND_SHOUT, 41u),
          "malformed-observed RSSI fixture first packs a valid record");
    report.app_bytes[9] = 128u;
    check(enqueue_record_at(&binding, &report, TAVRN_IDENTITY_SID8, 11u) &&
              !mind_root_coordinator_prepare(&coordinator, 11u, &request) &&
              binding.inbox.count == 0u && coordinator.counters.final_rejected == 2u &&
              !mind_log_queue_take(&binding.logs, &logged),
          "out-of-range observed RSSI is consumed as final-rejected without an event or v2 log record");
}

static void test_local_observed_and_legacy_report_logger_paths(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;
    mind_application_wire_record_t legacy;
    mind_log_record_t record;

    setup(&coordinator, &binding, 0u);
    check(mind_root_plane_set_local(&coordinator.plane, 1u, 1u) ==
              MIND_ROOT_APPLY_ACCEPTED &&
              enqueue_report_ingress(&binding, 7u, 0x010203u,
                                     MIND_EVT_FALL_AND_SHOUT) &&
              !mind_root_coordinator_prepare(&coordinator, 10u, &request) &&
              binding.inbox.count == 1u &&
              !mind_root_coordinator_prepare(&coordinator, 11u, &request) &&
              mind_log_queue_take(&binding.logs, &record) &&
              record.kind == MIND_LOG_EVENT && record.detail.event.path_local != 0u &&
              record.detail.event.report.packet_id24 == 0x010203u &&
              record.detail.event.report.schema_payload[6] == 0x03u &&
              record.detail.event.report.rssi_magnitude_db == 41u,
          "local ingress publishes one observed record through inbox and logger");

    setup(&coordinator, &binding, 0u);
    check(make_report(&legacy, 8u, 0x040506u, MIND_EVT_FALL_AND_SHOUT) &&
              enqueue_record_at(&binding, &legacy, TAVRN_IDENTITY_SID8, 12u) &&
              !mind_root_coordinator_prepare(&coordinator, 12u, &request) &&
              mind_log_queue_take(&binding.logs, &record) &&
              record.kind == MIND_LOG_EVENT && record.detail.event.path_local == 0u &&
              record.detail.event.report.packet_id24 == 0x040506u &&
              record.detail.event.report.rssi_magnitude_db == 0u,
          "legacy routed MIND_REPORT remains accepted with RSSI unavailable");
}

static const mind_event_target_t *find_event_target(
    const mind_event_item_t *item, const tavrn_adva_t *identity)
{
    uint8_t index;

    if (item == NULL || identity == NULL) {
        return NULL;
    }
    for (index = 0u; index < item->target_count; index++) {
        if (memcmp(item->targets[index].canonical_adva.bytes, identity->bytes,
                   TAVRN_ADVA_LEN) == 0) {
            return &item->targets[index];
        }
    }
    return NULL;
}

static void test_remote_off_and_departure_cancel_matching_event_targets(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;
    mind_application_wire_record_t root_record;
    mind_application_root_state_t active = root_state(1u, 9u, 1u);
    mind_application_root_state_t inactive = root_state(0u, 9u, 2u);
    tavrn_adva_t root_one = identity(1u);
    tavrn_adva_t root_two = identity(2u);
    const mind_event_target_t *target_one;
    const mind_event_target_t *target_two;

    setup(&coordinator, &binding, 1u);
    binding.incoming_identity_by_sid8 = 1u;
    check(mind_application_wire_pack_root_state(&root_record, &active) ==
              MIND_APPLICATION_WIRE_OK &&
              enqueue_record_from_sid8(&binding, &root_record, 1u) &&
              mind_root_coordinator_prepare(&coordinator, 1u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_ACK &&
              mind_root_coordinator_submit(&coordinator, &request.token, &request.data,
                                           1u) == MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED &&
              enqueue_report_ingress(&binding, 7u, 0x010203u,
                                     MIND_EVT_FALL_AND_SHOUT) &&
              mind_root_coordinator_prepare(&coordinator, 2u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_EVENT &&
              mind_root_coordinator_complete(&coordinator, &request.token,
                                             MIND_ROOT_COORDINATOR_SUBMIT_BUSY, 2u) &&
              mind_application_wire_pack_root_state(&root_record, &inactive) ==
                  MIND_APPLICATION_WIRE_OK &&
              enqueue_record_from_sid8(&binding, &root_record, 1u) &&
              mind_root_coordinator_prepare(&coordinator, 3u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_ACK &&
              coordinator.events.events[0].occupied == 0u &&
              coordinator.events.counters.cancelled_targets == 1u &&
              mind_root_plane_active_count(&coordinator.plane) == 0u,
          "accepted remote ROOT_STATE OFF cancels only its retained event target");

    setup(&coordinator, &binding, 0u);
    binding.incoming_identity_by_sid8 = 1u;
    binding.snapshot.entry_count = 2u;
    binding.snapshot.entries[0].canonical_adva = root_one;
    binding.snapshot.entries[0].departed = ROUTED_CYCLE_BOOLEAN_FALSE;
    binding.snapshot.entries[1].canonical_adva = root_two;
    binding.snapshot.entries[1].departed = ROUTED_CYCLE_BOOLEAN_FALSE;
    check(mind_application_wire_pack_root_state(&root_record, &active) ==
              MIND_APPLICATION_WIRE_OK &&
              enqueue_record_from_sid8(&binding, &root_record, 1u) &&
              enqueue_record_from_sid8(&binding, &root_record, 2u) &&
              mind_root_coordinator_prepare(&coordinator, 10u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_ACK &&
              mind_root_coordinator_submit(&coordinator, &request.token, &request.data,
                                           10u) == MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED &&
              mind_root_coordinator_prepare(&coordinator, 11u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_ACK &&
              mind_root_coordinator_submit(&coordinator, &request.token, &request.data,
                                           11u) == MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED &&
              enqueue_report_ingress(&binding, 8u, 0x010204u,
                                     MIND_EVT_FALL_AND_SHOUT) &&
              mind_root_coordinator_prepare(&coordinator, 12u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_EVENT &&
              mind_root_coordinator_complete(&coordinator, &request.token,
                                             MIND_ROOT_COORDINATOR_SUBMIT_BUSY, 12u),
          "two active copied roots retain one remote event target each");

    binding.snapshot.entries[0].departed = ROUTED_CYCLE_BOOLEAN_TRUE;
    check(mind_root_coordinator_prepare(&coordinator, 13u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_EVENT &&
              memcmp(request.token.submission.target.bytes, root_two.bytes,
                     TAVRN_ADVA_LEN) == 0,
          "copied departure cancels the departed target before the next fanout owner");
    target_one = find_event_target(&coordinator.events.events[0], &root_one);
    target_two = find_event_target(&coordinator.events.events[0], &root_two);
    check(target_one != NULL && target_one->state == MIND_EVENT_TARGET_CANCELLED &&
              target_two != NULL && target_two->state == MIND_EVENT_TARGET_PENDING &&
              coordinator.events.counters.cancelled_targets == 1u,
          "copied departure cancels only the matching retained event target");
}

static void test_arbiter_ack_before_urgent_report(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;
    mind_application_wire_record_t root_record;
    mind_application_root_state_t remote_state = root_state(1u, 9u, 1u);

    setup(&coordinator, &binding, 1u);
    check(mind_application_wire_pack_root_state(&root_record, &remote_state) ==
              MIND_APPLICATION_WIRE_OK &&
              enqueue_record(&binding, &root_record, TAVRN_IDENTITY_SID8) &&
              enqueue_report_ingress(&binding, 8u, 0x010203u,
                                     MIND_EVT_FALL_AND_SHOUT) &&
              mind_root_coordinator_prepare(&coordinator, 200u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_ACK,
          "single arbiter gives ROOT_ACK priority over an urgent event report");
    check(mind_root_coordinator_submit(&coordinator, &request.token, &request.data, 200u) ==
              MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED &&
              mind_root_coordinator_prepare(&coordinator, 201u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_EVENT &&
              request.data.urgent == 1u,
          "after exact ACK completion the next token belongs to the urgent report");
    {
        mind_root_coordinator_token_t wrong = request.token;

        wrong.submission.target_slot++;
        check(!mind_root_coordinator_complete(&coordinator, &wrong,
                                              MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED, 201u) &&
              mind_root_coordinator_pending(&coordinator) != NULL,
              "event completion with another target slot preserves the true owner");
        wrong = request.token;
        wrong.submission.slot = (uint8_t)(wrong.submission.slot + 1u);
        check(!mind_root_coordinator_complete(&coordinator, &wrong,
                                              MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED, 201u) &&
              mind_root_coordinator_pending(&coordinator) != NULL,
              "event completion with another event slot preserves the true owner");
        wrong = request.token;
        wrong.submission.kind = MIND_ROOT_SUBMISSION_STATE;
        check(!mind_root_coordinator_complete(&coordinator, &wrong,
                                              MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED, 201u) &&
              mind_root_coordinator_pending(&coordinator) != NULL,
              "event completion with another kind preserves the true owner");
    }
    check(mind_root_coordinator_submit(&coordinator, &request.token, &request.data, 201u) ==
              MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED &&
              enqueue_report_ingress(&binding, 9u, 0x010204u, MIND_EVT_HEARTBEAT) &&
              mind_root_coordinator_prepare(&coordinator, 400u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_STATE,
          "ROOT_STATE remains ahead of a heartbeat report in the same arbiter");
    check(mind_root_coordinator_submit(&coordinator, &request.token, &request.data, 400u) ==
              MIND_ROOT_COORDINATOR_SUBMIT_ACCEPTED &&
              mind_root_coordinator_prepare(&coordinator, 401u, &request) &&
              request.token.submission.kind == MIND_ROOT_SUBMISSION_EVENT &&
              request.data.urgent == 0u,
          "heartbeat report owns the token only after ACK, urgent, and ROOT_STATE work");
}

static void test_gtt_command_transaction(void)
{
    mind_root_coordinator_t coordinator;
    fake_binding_t binding;
    mind_root_coordinator_request_t request;
    mind_log_record_t record;
    mind_log_reservation_t reservation;
    mind_log_record_t records[MIND_LOG_CAPACITY];
    mind_application_root_state_t before;
    mind_application_root_state_t after;
    int transaction;

    setup(&coordinator, &binding, 0u);
    before = mind_root_plane_local_state(&coordinator.plane);
    transaction = enqueue_command(&binding, MIND_COMMAND_GTT, MIND_COMMAND_ACCEPTED) &&
        !mind_root_coordinator_prepare(&coordinator, 1u, &request) &&
        binding.gtt_claim_calls == 1u && binding.gtt_commit_calls == 1u &&
        binding.gtt_cancel_calls == 0u && binding.gtt_claimed != 0u &&
        mind_log_queue_take(&binding.logs, &record) &&
        record.kind == MIND_LOG_COMMAND &&
        record.detail.command.command == MIND_COMMAND_GTT &&
        record.detail.command.status == MIND_COMMAND_ACCEPTED &&
        !mind_log_queue_take(&binding.logs, &record);
    after = mind_root_plane_local_state(&coordinator.plane);
    check(transaction && memcmp(&before, &after, sizeof(before)) == 0,
          "accepted GTT claims after one log reservation without root state or root record");

    setup(&coordinator, &binding, 0u);
    binding.gtt_available = 0u;
    before = mind_root_plane_local_state(&coordinator.plane);
    transaction = enqueue_command(&binding, MIND_COMMAND_GTT, MIND_COMMAND_ACCEPTED) &&
        !mind_root_coordinator_prepare(&coordinator, 2u, &request) &&
        binding.gtt_claim_calls == 1u && binding.gtt_commit_calls == 0u &&
        binding.gtt_cancel_calls == 0u &&
        mind_log_queue_take(&binding.logs, &record) &&
        record.kind == MIND_LOG_COMMAND &&
        record.detail.command.command == MIND_COMMAND_GTT &&
        record.detail.command.status == MIND_COMMAND_BUSY &&
        !mind_log_queue_take(&binding.logs, &record);
    after = mind_root_plane_local_state(&coordinator.plane);
    check(transaction && memcmp(&before, &after, sizeof(before)) == 0,
          "busy GTT logs one busy command without local root mutation or root record");

    setup(&coordinator, &binding, 0u);
    memset(records, 0, sizeof(records));
    check(mind_log_queue_reserve(&binding.logs, MIND_LOG_CAPACITY, &reservation) &&
              mind_log_queue_commit(&binding.logs, &reservation, records) &&
              enqueue_command(&binding, MIND_COMMAND_GTT, MIND_COMMAND_ACCEPTED) &&
              !mind_root_coordinator_prepare(&coordinator, 3u, &request) &&
              binding.gtt_claim_calls == 0u && binding.gtt_commit_calls == 0u &&
              binding.gtt_cancel_calls == 0u &&
              mind_uart_command_peek(&binding.uart, &(mind_command_attempt_t){ 0 }),
          "logger-full GTT leaves the mailbox command intact without claiming the UI slot");

    setup(&coordinator, &binding, 0u);
    binding.log_commit_fails = 1u;
    check(enqueue_command(&binding, MIND_COMMAND_GTT, MIND_COMMAND_ACCEPTED) &&
              !mind_root_coordinator_prepare(&coordinator, 4u, &request) &&
              binding.gtt_claim_calls == 1u && binding.gtt_commit_calls == 0u &&
              binding.gtt_cancel_calls == 1u && binding.gtt_claimed == 0u &&
              mind_uart_command_peek(&binding.uart, &(mind_command_attempt_t){ 0 }) &&
              !mind_log_queue_take(&binding.logs, &record),
          "structural GTT log failure rolls back its claim and retains the mailbox command");
    binding.log_commit_fails = 0u;
    check(!mind_root_coordinator_prepare(&coordinator, 5u, &request) &&
              binding.gtt_claim_calls == 2u && binding.gtt_commit_calls == 1u &&
              binding.gtt_cancel_calls == 1u &&
              mind_log_queue_take(&binding.logs, &record) &&
              record.detail.command.command == MIND_COMMAND_GTT &&
              record.detail.command.status == MIND_COMMAND_ACCEPTED,
          "rolled-back GTT retries as the retained accepted mailbox command");
}

int main(void)
{
    test_uart_immediate_off_lifecycle();
    test_uart_delayed_off_backpressure_and_duplicate_lifecycle();
    test_snapshot_and_pre_sid8_command();
    test_logger_transaction_and_status();
    test_pending_ownership_and_submit_results();
    test_event_submit_status_paths_and_target_ownership();
    test_outgoing_resolver_matrix();
    test_ack_resolution_skips_blocked_owner();
    test_ack_resolution_skips_fail_closed_owner();
    test_state_resolution_skips_blocked_owner_without_pacing_change();
    test_root_resolution_scans_are_bounded();
    test_final_validation_and_incoming_correlation();
    test_event_final_logger_retention_and_observers();
    test_malformed_observed_reports_reject_without_event_log();
    test_local_observed_and_legacy_report_logger_paths();
    test_remote_off_and_departure_cancel_matching_event_targets();
    test_arbiter_ack_before_urgent_report();
    test_gtt_command_transaction();
    if (failures != 0u) {
        printf("mind_root_integration failures=%u\n", failures);
        return 1;
    }
    printf("mind_root_integration tests passed\n");
    return 0;
}
