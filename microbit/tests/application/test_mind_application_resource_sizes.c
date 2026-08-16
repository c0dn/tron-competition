/*
 * Target-ABI sizeof probe for the production Layer-7 fixed-state closure.
 *
 * The resource checker compiles this translation unit with the exact published
 * ARM command line, then reads these object-symbol sizes with arm-none-eabi-nm.
 * It is deliberately not linked into firmware.
 */

#include <stdint.h>

#include "aodv_core.h"
#include "mind_application_ingress.h"
#include "mind_event_forwarder.h"
#include "mind_log.h"
#include "mind_phase5_provenance.h"
#include "mind_root_coordinator.h"
#include "mind_root_inbox.h"
#include "mind_root_plane.h"
#include "mind_topology_adapter.h"
#include "mind_uart.h"
#include "mind_ui.h"

#define MIND_RESOURCE_SIZE(symbol, type) \
    const uint8_t mind_application_sizeof_##symbol[sizeof(type)] = { 0u }

typedef char mind_resource_root_capacity_guard[
    (MIND_APP_ROOT_CAPACITY == 16u) ? 1 : -1];
typedef char mind_resource_campaign_capacity_guard[
    (MIND_APP_CAMPAIGN_CAPACITY == 16u) ? 1 : -1];
typedef char mind_resource_ack_capacity_guard[
    (MIND_APP_ROOT_ACK_CAPACITY == 16u) ? 1 : -1];
typedef char mind_resource_event_capacity_guard[
    (MIND_APP_EVENT_CAPACITY == 16u) ? 1 : -1];
typedef char mind_resource_ingress_capacity_guard[
    (MIND_APPLICATION_INGRESS_QUEUE_CAPACITY == 8u) ? 1 : -1];
typedef char mind_resource_final_capacity_guard[
    (MIND_ROOT_INBOX_CAPACITY == 8u) ? 1 : -1];
typedef char mind_resource_log_capacity_guard[
    (MIND_LOG_CAPACITY == 8u) ? 1 : -1];

MIND_RESOURCE_SIZE(ingress_seen, mind_application_ingress_seen_t);
MIND_RESOURCE_SIZE(ingress_event, mind_application_ingress_event_t);
MIND_RESOURCE_SIZE(ingress_counters, mind_application_ingress_counters_t);
MIND_RESOURCE_SIZE(ingress, mind_application_ingress_t);

MIND_RESOURCE_SIZE(topology_entry, mind_topology_entry_t);
MIND_RESOURCE_SIZE(topology_counters, mind_topology_adapter_counters_t);
MIND_RESOURCE_SIZE(topology_adapter, mind_topology_adapter_t);

MIND_RESOURCE_SIZE(root_entry, mind_root_entry_t);
MIND_RESOURCE_SIZE(root_campaign_target, mind_root_campaign_target_t);
MIND_RESOURCE_SIZE(root_ack_obligation, mind_root_ack_obligation_t);
MIND_RESOURCE_SIZE(root_plane_counters, mind_root_plane_counters_t);
MIND_RESOURCE_SIZE(root_plane, mind_root_plane_t);

MIND_RESOURCE_SIZE(event_target, mind_event_target_t);
MIND_RESOURCE_SIZE(event_item, mind_event_item_t);
MIND_RESOURCE_SIZE(event_forwarder_counters, mind_event_forwarder_counters_t);
MIND_RESOURCE_SIZE(event_forwarder, mind_event_forwarder_t);

MIND_RESOURCE_SIZE(root_inbox_entry, mind_root_inbox_entry_t);
MIND_RESOURCE_SIZE(root_inbox, mind_root_inbox_t);

MIND_RESOURCE_SIZE(log_record, mind_log_record_t);
MIND_RESOURCE_SIZE(log_queue, mind_log_queue_t);
MIND_RESOURCE_SIZE(log_reservation, mind_log_reservation_t);

MIND_RESOURCE_SIZE(command_attempt, mind_command_attempt_t);
MIND_RESOURCE_SIZE(command_parser, mind_command_parser_t);
MIND_RESOURCE_SIZE(command_mailbox, mind_command_mailbox_t);
MIND_RESOURCE_SIZE(uart, mind_uart_t);
MIND_RESOURCE_SIZE(phase5_provenance, mind_phase5_provenance_snapshot_t);

MIND_RESOURCE_SIZE(audio, mind_audio_t);
MIND_RESOURCE_SIZE(ui, mind_ui_t);

MIND_RESOURCE_SIZE(root_coordinator_token, mind_root_coordinator_token_t);
MIND_RESOURCE_SIZE(root_coordinator_request, mind_root_coordinator_request_t);
MIND_RESOURCE_SIZE(root_coordinator_operations, mind_root_coordinator_operations_t);
MIND_RESOURCE_SIZE(root_coordinator_counters, mind_root_coordinator_counters_t);
MIND_RESOURCE_SIZE(root_coordinator, mind_root_coordinator_t);

/* Production-only static owners outside the nested coordinator object. */
MIND_RESOURCE_SIZE(aodv_status, aodv_status_t);
MIND_RESOURCE_SIZE(logger_dispatch_epoch, uint32_t);
MIND_RESOURCE_SIZE(logger_progress_wake_armed, uint8_t);
MIND_RESOURCE_SIZE(cycle_fault_logged, uint8_t);
MIND_RESOURCE_SIZE(pointer, void *);
MIND_RESOURCE_SIZE(audio_sample, uint16_t[1]);
MIND_RESOURCE_SIZE(display_framebuffer, uint8_t[5]);
