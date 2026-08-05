/*
 * Minimal AODV_ONLY routed-node proving ground.  The mesh task owns exactly
 * one link-v2 instance and one AODV core; UART output is confined to the
 * lower-priority logger task.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include <stdint.h>
#include <string.h>

#include "aodv_core.h"
#include "ble_mesh_scheduler.h"
#include "ble_radio.h"
#include "tavrn_link_v2.h"
#include "tron_build_config.h"
#include "tron_build_info.h"
#include "tron_timer_config.h"

#define ROUTED_DELIVERY_CAPACITY 8u
#define ROUTED_MESH_TASK_PRIORITY 10u
#define ROUTED_LOGGER_TASK_PRIORITY 11u

typedef struct routed_ring {
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} routed_ring_t;

typedef struct routed_delivery_state {
    routed_ring_t published;
    uint8_t provisional;
    uint8_t reserved_index;
    tavrn_direct_peer_t transmitter;
} routed_delivery_state_t;

typedef struct routed_delivery {
    tavrn_direct_peer_t transmitter;
    tavrn_link_data_t data;
    uint32_t delivered_at_ms;
} routed_delivery_t;

typedef struct routed_poll_gate {
    uint32_t last_poll_return_ms;
    uint8_t have_poll_return;
    uint8_t fault_latched;
} routed_poll_gate_t;

typedef struct routed_counters {
    uint32_t submitted;
    uint32_t delivered;
    uint32_t forwarded;
    uint32_t control_rreq;
    uint32_t control_rrep;
    uint32_t control_rerr;
    uint32_t control_rrep_ack;
    uint32_t custody_transferred;
    uint32_t retry_exhausted;
    uint32_t local_custody_fault;
    uint32_t scheduler_fault;
    uint32_t rx_blocked;
    uint32_t retained_action;
    uint32_t pending_failed;
} routed_counters_t;

typedef struct routed_snapshot {
    routed_counters_t routed;
    tavrn_link_counters_t link;
    aodv_counters_t aodv;
    uint8_t delivery_queue_count;
    uint8_t delivery_provisional;
    uint8_t mesh_fault_latched;
} routed_snapshot_t;

typedef struct routed_pending_failure {
    tavrn_direct_peer_t failed_next_hop;
    uint8_t active;
} routed_pending_failure_t;

typedef char routed_delivery_capacity_must_be_eight[
    (ROUTED_DELIVERY_CAPACITY == 8u) ? 1 : -1];
typedef char routed_logger_must_be_lower_priority[
    (ROUTED_MESH_TASK_PRIORITY < ROUTED_LOGGER_TASK_PRIORITY) ? 1 : -1];

static ble_mesh_scheduler_t routed_scheduler;
static tavrn_link_v2_t routed_link;
static aodv_core_t routed_aodv;
static tavrn_direct_peer_t local_peer;
static tavrn_direct_peer_t configured_destination;
static UB local_adva[6];
static UB runtime_ficr_adva[6];
static routed_delivery_t deliveries[ROUTED_DELIVERY_CAPACITY];
static routed_delivery_state_t delivery_state;
static routed_counters_t routed_counters;
static aodv_action_t retained_action;
static uint8_t retained_action_valid;
static routed_pending_failure_t pending_failure;
static uint8_t mesh_fault_latched;

static const UB configured_override[6] = TRON_BUILD_ADVA_OVERRIDE_BYTES;
static const UB configured_peer_adva[6] = TRON_BUILD_LINK_PEER_ADVA_BYTES;
static const UB configured_rx_block_adva[6] = TRON_BUILD_RX_BLOCK_ADVA_BYTES;

static uint32_t now_ms(void)
{
    SYSTIM time;

    tk_get_otm(&time);
    return time.lo;
}

static int queue_guard_begin(void)
{
    return tk_dis_dsp() == E_OK;
}

static void queue_guard_end(void)
{
    (void)tk_ena_dsp();
}

static int adva_equal(const UB left[6], const UB right[6])
{
    return memcmp(left, right, 6u) == 0;
}

static uint16_t sid16_from_adva(const UB adva[6])
{
    return (uint16_t)adva[0] | ((uint16_t)adva[1] << 8);
}

static int logical_id_equal(const tavrn_logical_id_t *left,
                            const tavrn_logical_id_t *right)
{
    return left != NULL && right != NULL && left->width == right->width &&
           left->value == right->value;
}

static void ring_init(routed_ring_t *ring)
{
    if (ring != NULL) {
        memset(ring, 0, sizeof(*ring));
    }
}

static int ring_push(routed_ring_t *ring, uint8_t *index_out)
{
    if (ring == NULL || index_out == NULL || ring->count >= ROUTED_DELIVERY_CAPACITY) {
        return 0;
    }
    *index_out = ring->tail;
    ring->tail = (uint8_t)((ring->tail + 1u) % ROUTED_DELIVERY_CAPACITY);
    ring->count++;
    return 1;
}

static int ring_pop(routed_ring_t *ring, uint8_t *index_out)
{
    if (ring == NULL || index_out == NULL || ring->count == 0u) {
        return 0;
    }
    *index_out = ring->head;
    ring->head = (uint8_t)((ring->head + 1u) % ROUTED_DELIVERY_CAPACITY);
    ring->count--;
    return 1;
}

static int delivery_reserve(const tavrn_direct_peer_t *transmitter)
{
    int result = 0;

    if (transmitter == NULL || !queue_guard_begin()) {
        return 0;
    }
    if (delivery_state.provisional == 0u &&
        delivery_state.published.count < ROUTED_DELIVERY_CAPACITY) {
        delivery_state.provisional = 1u;
        delivery_state.reserved_index = delivery_state.published.tail;
        delivery_state.transmitter = *transmitter;
        result = 1;
    }
    queue_guard_end();
    return result;
}

static void delivery_cancel(void)
{
    if (queue_guard_begin()) {
        delivery_state.provisional = 0u;
        queue_guard_end();
    }
}

static int delivery_commit(const tavrn_link_data_t *data, uint32_t delivered_at_ms)
{
    uint8_t index;
    int result = 0;

    if (data == NULL || !queue_guard_begin()) {
        return 0;
    }
    if (delivery_state.provisional != 0u &&
        delivery_state.reserved_index == delivery_state.published.tail &&
        ring_push(&delivery_state.published, &index)) {
        memset(&deliveries[index], 0, sizeof(deliveries[index]));
        deliveries[index].transmitter = delivery_state.transmitter;
        deliveries[index].data = *data;
        deliveries[index].delivered_at_ms = delivered_at_ms;
        delivery_state.provisional = 0u;
        result = 1;
    }
    queue_guard_end();
    return result;
}

static int delivery_pop(routed_delivery_t *delivery)
{
    uint8_t index;
    int result = 0;

    if (delivery == NULL || !queue_guard_begin()) {
        return 0;
    }
    if (ring_pop(&delivery_state.published, &index)) {
        *delivery = deliveries[index];
        result = 1;
    }
    queue_guard_end();
    return result;
}

static uint32_t mesh_remaining_yield_ms(const routed_poll_gate_t *gate,
                                        uint32_t work_complete_ms)
{
    uint32_t voluntary_yield = tron_timer_config.scheduler_poll_max_ms > 1u ?
        tron_timer_config.scheduler_poll_max_ms - 1u : 0u;
    uint32_t elapsed;

    if (gate == NULL || gate->have_poll_return == 0u) {
        return 0u;
    }
    elapsed = (uint32_t)(work_complete_ms - gate->last_poll_return_ms);
    return elapsed < voluntary_yield ? voluntary_yield - elapsed : 0u;
}

static void snapshot(routed_snapshot_t *out)
{
    const tavrn_link_counters_t *link_counters;
    const aodv_counters_t *aodv_counters;

    if (out == NULL || !queue_guard_begin()) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->routed = routed_counters;
    out->delivery_queue_count = delivery_state.published.count;
    out->delivery_provisional = delivery_state.provisional;
    out->mesh_fault_latched = mesh_fault_latched;
    link_counters = tavrn_link_v2_counters(&routed_link);
    aodv_counters = aodv_core_counters(&routed_aodv);
    if (link_counters != NULL) {
        out->link = *link_counters;
    }
    if (aodv_counters != NULL) {
        out->aodv = *aodv_counters;
    }
    queue_guard_end();
}

static void log_startup(void)
{
    if (TRON_BUILD_LINK_PEER_ADVA_ENABLED != 0) {
        tm_printf((UB *)"routed startup sid16=0x%04x destination=0x%04x rx_block=%s\n",
                  (UINT)local_peer.logical_id.value,
                  (UINT)configured_destination.logical_id.value,
                  (UB *)(TRON_BUILD_RX_BLOCK_ADVA_ENABLED != 0 ? "enabled" : "off"));
    } else {
        tm_printf((UB *)"routed startup sid16=0x%04x destination=not_configured rx_block=%s\n",
                  (UINT)local_peer.logical_id.value,
                  (UB *)(TRON_BUILD_RX_BLOCK_ADVA_ENABLED != 0 ? "enabled" : "off"));
    }
}

static void log_summary(uint32_t now)
{
    routed_snapshot_t state;

    snapshot(&state);
    tm_printf((UB *)"routed stats now=%lu submitted=%lu delivered=%lu forwarded=%lu ctrl_rreq=%lu ctrl_rrep=%lu ctrl_rerr=%lu ctrl_rrep_ack=%lu custody=%lu retry_exhausted=%lu local_fault=%lu scheduler_fault=%lu retained=%lu aodv_backpressure=%lu pending_failed=%lu rx_blocked=%lu delivery_q=%u provisional=%u fault=%u\n",
              (UW)now, (UW)state.routed.submitted, (UW)state.routed.delivered,
              (UW)state.routed.forwarded, (UW)state.routed.control_rreq,
              (UW)state.routed.control_rrep, (UW)state.routed.control_rerr,
              (UW)state.routed.control_rrep_ack,
              (UW)state.routed.custody_transferred,
              (UW)state.routed.retry_exhausted,
              (UW)state.routed.local_custody_fault,
              (UW)state.routed.scheduler_fault,
              (UW)state.routed.retained_action,
              (UW)state.aodv.action_backpressure,
              (UW)state.routed.pending_failed,
              (UW)state.routed.rx_blocked,
              (UINT)state.delivery_queue_count,
              (UINT)state.delivery_provisional,
              (UINT)state.mesh_fault_latched);
}

static void release_transit_rx_custody(const tavrn_link_data_t *data, uint32_t now)
{
    if (data != NULL && data->ownership == TAVRN_DATA_TRANSIT) {
        (void)tavrn_link_v2_release_rx_custody(&routed_link, data, now);
    }
}

static void queue_link_failure(const tavrn_owned_data_event_t *owned, uint32_t now)
{
    aodv_failure_status_t status;

    if (owned == NULL) {
        return;
    }
    status = aodv_core_report_link_failure(&routed_aodv, &owned->next_hop, NULL,
                                           AODV_LINK_FAILURE_IMMEDIATE_RERR, now);
    if (status == AODV_FAILURE_BUSY) {
        pending_failure.failed_next_hop = owned->next_hop;
        pending_failure.active = 1u;
    } else if (status != AODV_FAILURE_OK) {
        routed_counters.local_custody_fault++;
    }
}

static void retry_pending_failure(uint32_t now)
{
    aodv_failure_status_t status;

    if (pending_failure.active == 0u) {
        return;
    }
    status = aodv_core_report_link_failure(&routed_aodv,
                                           &pending_failure.failed_next_hop, NULL,
                                           AODV_LINK_FAILURE_IMMEDIATE_RERR, now);
    if (status == AODV_FAILURE_OK) {
        pending_failure.active = 0u;
    } else if (status == AODV_FAILURE_INVALID) {
        pending_failure.active = 0u;
        routed_counters.local_custody_fault++;
    }
}

static void handle_terminal_link_event(const tavrn_link_event_t *event, uint32_t now)
{
    const tavrn_link_data_t *data;

    if (event == NULL || event->type == TAVRN_LINK_EVENT_NONE) {
        return;
    }
    if (event->type == TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED) {
        routed_counters.custody_transferred++;
        release_transit_rx_custody(&event->detail.transferred_data.data, now);
        return;
    }
    if (event->type < TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED ||
        event->type > TAVRN_LINK_EVENT_SERVICE_FAULT_TERMINAL) {
        return;
    }
    data = &event->detail.owned_data.data;
    release_transit_rx_custody(data, now);
    if (event->type == TAVRN_LINK_EVENT_RETRY_EXHAUSTED) {
        routed_counters.retry_exhausted++;
        queue_link_failure(&event->detail.owned_data, now);
    } else {
        routed_counters.local_custody_fault++;
    }
}

static void resolve_data_candidate(const tavrn_rx_data_candidate_t *candidate,
                                   uint32_t now)
{
    aodv_data_input_t input;
    aodv_status_t status;
    tavrn_rx_decision_t decision;
    tavrn_link_event_t outcome;
    tavrn_link_resolve_status_t resolved;
    int final_destination;

    if (candidate == NULL) {
        return;
    }
    memset(&input, 0, sizeof(input));
    input.transmitter = candidate->transmitter;
    input.data = candidate->data;
    final_destination = logical_id_equal(&candidate->data.final_destination,
                                         &local_peer.logical_id);
    if (final_destination && !delivery_reserve(&candidate->transmitter)) {
        decision = TAVRN_RX_BUSY;
    } else {
        status = aodv_core_ingest_data(&routed_aodv, &input, now);
        if (status == AODV_STATUS_OK) {
            decision = TAVRN_RX_ACCEPTED;
        } else if (status == AODV_STATUS_BUSY) {
            decision = TAVRN_RX_BUSY;
            if (final_destination) {
                delivery_cancel();
            }
        } else {
            decision = TAVRN_RX_REJECTED;
            if (final_destination) {
                delivery_cancel();
            }
        }
    }
    resolved = tavrn_link_v2_resolve_rx(&routed_link, candidate->token, decision,
                                        now, &outcome);
    if (resolved != TAVRN_LINK_RESOLVE_OK && final_destination &&
        decision == TAVRN_RX_ACCEPTED) {
        delivery_cancel();
        routed_counters.local_custody_fault++;
    }
    handle_terminal_link_event(&outcome, now);
}

static void handle_control_event(const tavrn_rx_control_event_t *control,
                                 uint32_t now)
{
    aodv_control_input_t input;
    aodv_status_t status;

    if (control == NULL) {
        return;
    }
    input.transmitter = control->transmitter;
    input.control = control->control;
    status = aodv_core_ingest_control(&routed_aodv, &input, now);
    if (status == AODV_STATUS_BUSY) {
        routed_counters.local_custody_fault++;
    }
}

static void handle_link_event(const tavrn_link_event_t *event, uint32_t now)
{
    if (event == NULL) {
        return;
    }
    if (event->type == TAVRN_LINK_EVENT_RX_DATA_CANDIDATE) {
        resolve_data_candidate(&event->detail.candidate, now);
    } else if (event->type == TAVRN_LINK_EVENT_RX_CONTROL) {
        handle_control_event(&event->detail.control, now);
    } else {
        handle_terminal_link_event(event, now);
    }
}

static int link_send_is_transient(tavrn_link_send_status_t status)
{
    return status == TAVRN_LINK_SEND_BUSY || status == TAVRN_LINK_SEND_NO_SLOT;
}

static void count_control(tavrn_wire_type_t type)
{
    if (type == TAVRN_WIRE_E_RREQ) {
        routed_counters.control_rreq++;
    } else if (type == TAVRN_WIRE_E_RREP) {
        routed_counters.control_rrep++;
    } else if (type == TAVRN_WIRE_E_RERR) {
        routed_counters.control_rerr++;
    } else if (type == TAVRN_WIRE_E_RREP_ACK) {
        routed_counters.control_rrep_ack++;
    }
}

static int dispatch_aodv_action(const aodv_action_t *action, uint32_t now)
{
    tavrn_link_event_t outcome;
    tavrn_link_send_status_t status;

    if (action == NULL) {
        return 0;
    }
    if (action->type == AODV_ACTION_SEND_RREQ ||
        action->type == AODV_ACTION_SEND_RREP ||
        action->type == AODV_ACTION_FORWARD_RREP ||
        action->type == AODV_ACTION_SEND_RERR ||
        action->type == AODV_ACTION_SEND_RREP_ACK) {
        const tavrn_direct_peer_t *next_hop =
            action->detail.control.controlled_flood != 0u ? NULL :
            &action->detail.control.next_hop;

        status = tavrn_link_v2_send_control(&routed_link,
                                            &action->detail.control.control,
                                            next_hop,
                                            action->detail.control.controlled_flood,
                                            now, &outcome);
        if (status == TAVRN_LINK_SEND_OK) {
            count_control(action->detail.control.control.type);
            if (action->detail.control.token != 0u &&
                aodv_core_mark_action_sent(&routed_aodv,
                                           action->detail.control.token, now) !=
                    AODV_STATUS_OK) {
                routed_counters.local_custody_fault++;
            }
            handle_terminal_link_event(&outcome, now);
            return 1;
        }
        if (link_send_is_transient(status)) {
            return 0;
        }
        routed_counters.local_custody_fault++;
        return 1;
    }
    if (action->type == AODV_ACTION_FORWARD_DATA) {
        status = tavrn_link_v2_send_unicast(&routed_link, &action->detail.data.next_hop,
                                            &action->detail.data.data, now, &outcome);
        if (status == TAVRN_LINK_SEND_OK) {
            routed_counters.forwarded++;
            handle_terminal_link_event(&outcome, now);
            return 1;
        }
        if (link_send_is_transient(status)) {
            return 0;
        }
        release_transit_rx_custody(&action->detail.data.data, now);
        routed_counters.local_custody_fault++;
        handle_terminal_link_event(&outcome, now);
        return 1;
    }
    if (action->type == AODV_ACTION_DELIVER_DATA) {
        if (!delivery_commit(&action->detail.data.data, now)) {
            mesh_fault_latched = 1u;
            routed_counters.local_custody_fault++;
        } else {
            routed_counters.delivered++;
        }
        return 1;
    }
    if (action->type == AODV_ACTION_PENDING_DATA_FAILED) {
        routed_counters.pending_failed++;
        return 1;
    }
    if (action->type == AODV_ACTION_BLACKLIST_NEIGHBOR) {
        routed_counters.local_custody_fault++;
        return 1;
    }
    return 1;
}

static void drain_one_aodv_action(uint32_t now)
{
    aodv_action_t action;

    if (retained_action_valid != 0u) {
        if (dispatch_aodv_action(&retained_action, now)) {
            retained_action_valid = 0u;
        }
        return;
    }
    if (aodv_core_poll_action(&routed_aodv, &action) != AODV_ACTION_POLL_OK) {
        return;
    }
    if (!dispatch_aodv_action(&action, now)) {
        retained_action = action;
        retained_action_valid = 1u;
        routed_counters.retained_action++;
    }
}

static void maybe_submit_initiator(uint32_t now, uint32_t *next_submit_at)
{
    tron_application_data_t data;
    aodv_status_t status;

    if (next_submit_at == NULL || TRON_BUILD_LINK_INITIATOR == 0 ||
        TRON_BUILD_LINK_PEER_ADVA_ENABLED == 0 || mesh_fault_latched != 0u ||
        routed_counters.submitted >= TRON_BUILD_LINK_TRANSACTION_TARGET ||
        (int32_t)(now - *next_submit_at) < 0) {
        return;
    }
    memset(&data, 0, sizeof(data));
    data.final_destination = configured_destination.logical_id;
    data.app_kind = 0x7fu;
    data.app_source = 0u;
    data.app_len = 4u;
    data.app_bytes[0] = (uint8_t)(routed_counters.submitted & 0xffu);
    data.app_bytes[1] = (uint8_t)((routed_counters.submitted >> 8) & 0xffu);
    data.app_bytes[2] = (uint8_t)((routed_counters.submitted >> 16) & 0xffu);
    data.app_bytes[3] = (uint8_t)((routed_counters.submitted >> 24) & 0xffu);
    status = aodv_core_submit_application(&routed_aodv, &data, now);
    if (status == AODV_STATUS_OK || status == AODV_STATUS_QUEUED) {
        routed_counters.submitted++;
    }
    *next_submit_at = now + TRON_BUILD_LINK_TX_INTERVAL_MS;
}

static void latch_scheduler_fault(uint32_t now)
{
    ble_mesh_sched_event_t fault;
    tavrn_link_event_t outcome;

    if (mesh_fault_latched != 0u) {
        return;
    }
    memset(&fault, 0, sizeof(fault));
    fault.type = BLE_MESH_SCHED_EVENT_SERVICE_FAULT;
    fault.fault = BLE_MESH_SCHED_FAULT_POLL_OVERRUN;
    mesh_fault_latched = 1u;
    routed_counters.scheduler_fault++;
    (void)tavrn_link_v2_on_scheduler_event(&routed_link, &fault, now, &outcome);
    handle_link_event(&outcome, now);
}

static void handle_scheduler_event(const ble_mesh_sched_event_t *event, uint32_t now)
{
    tavrn_link_event_t outcome;

    if (event == NULL) {
        return;
    }
    if (event->type == BLE_MESH_SCHED_EVENT_RADIO_FAULT ||
        event->type == BLE_MESH_SCHED_EVENT_SERVICE_FAULT) {
        mesh_fault_latched = 1u;
        routed_counters.scheduler_fault++;
    }
    if (TRON_BUILD_TEST_HOOKS != 0 && TRON_BUILD_RX_BLOCK_ADVA_ENABLED != 0 &&
        event->type == BLE_MESH_SCHED_EVENT_RX_ADV &&
        adva_equal(event->adv_addr, configured_rx_block_adva)) {
        routed_counters.rx_blocked++;
        return;
    }
    (void)tavrn_link_v2_on_scheduler_event(&routed_link, event, now, &outcome);
    handle_link_event(&outcome, now);
}

LOCAL void routed_mesh_task(INT stacd, void *exinf)
{
    routed_poll_gate_t poll_gate;
    ble_mesh_sched_event_t scheduler_event;
    tavrn_link_event_t link_event;
    uint32_t next_submit_at = now_ms();
    uint32_t start_rx_return;

    (void)stacd;
    (void)exinf;
    memset(&poll_gate, 0, sizeof(poll_gate));
    ble_mesh_scheduler_start_rx(&routed_scheduler, now_ms());
    start_rx_return = now_ms();
    poll_gate.last_poll_return_ms = start_rx_return;
    poll_gate.have_poll_return = 1u;
    while (1) {
        uint32_t poll_start;
        uint32_t poll_return;
        uint32_t yield_delay;

        if (mesh_fault_latched == 0u) {
            poll_start = now_ms();
            if (poll_gate.fault_latched != 0u ||
                (uint32_t)(poll_start - poll_gate.last_poll_return_ms) >
                    tron_timer_config.scheduler_poll_max_ms) {
                poll_gate.fault_latched = 1u;
                latch_scheduler_fault(poll_start);
            } else {
                memset(&scheduler_event, 0, sizeof(scheduler_event));
                (void)ble_mesh_scheduler_poll(&routed_scheduler, poll_start,
                                              &scheduler_event);
                poll_return = now_ms();
                poll_gate.last_poll_return_ms = poll_return;
                poll_gate.have_poll_return = 1u;
                if (scheduler_event.type != BLE_MESH_SCHED_EVENT_NONE) {
                    handle_scheduler_event(&scheduler_event, poll_return);
                }
            }
        }

        (void)tavrn_link_v2_tick(&routed_link, now_ms(), &link_event);
        handle_link_event(&link_event, now_ms());
        retry_pending_failure(now_ms());
        (void)aodv_core_tick(&routed_aodv, now_ms());
        if (mesh_fault_latched == 0u) {
            maybe_submit_initiator(now_ms(), &next_submit_at);
            drain_one_aodv_action(now_ms());
            (void)tavrn_link_v2_dispatch(&routed_link, now_ms(), &link_event);
            handle_link_event(&link_event, now_ms());
        }

        yield_delay = mesh_fault_latched == 0u ?
            mesh_remaining_yield_ms(&poll_gate, now_ms()) :
            (tron_timer_config.scheduler_poll_max_ms > 1u ?
             tron_timer_config.scheduler_poll_max_ms - 1u : 0u);
        if (yield_delay != 0u) {
            (void)tk_dly_tsk(yield_delay);
        }
    }
}

LOCAL void routed_logger_task(INT stacd, void *exinf)
{
    routed_delivery_t delivery;
    uint32_t next_summary_at = now_ms() + tron_timer_config.stats_ms;

    (void)stacd;
    (void)exinf;
    tm_printf((UB *)"%s\n", (UB *)tron_build_info_line());
    log_startup();
    while (1) {
        uint32_t now = now_ms();

        if (delivery_pop(&delivery)) {
            tm_printf((UB *)"routed final_data now=%lu origin=0x%04x destination=0x%04x seq=%u app_kind=0x%02x app_len=%u peer=%02x:%02x:%02x:%02x:%02x:%02x\n",
                      (UW)delivery.delivered_at_ms,
                      (UINT)delivery.data.origin.value,
                      (UINT)delivery.data.final_destination.value,
                      (UINT)delivery.data.data_seq, (UINT)delivery.data.app_kind,
                      (UINT)delivery.data.app_len,
                      (UINT)delivery.transmitter.adva.bytes[0],
                      (UINT)delivery.transmitter.adva.bytes[1],
                      (UINT)delivery.transmitter.adva.bytes[2],
                      (UINT)delivery.transmitter.adva.bytes[3],
                      (UINT)delivery.transmitter.adva.bytes[4],
                      (UINT)delivery.transmitter.adva.bytes[5]);
        } else if ((int32_t)(now - next_summary_at) >= 0) {
            log_summary(now);
            next_summary_at = now + tron_timer_config.stats_ms;
        }
        (void)tk_dly_tsk(1u);
    }
}

EXPORT INT usermain(void)
{
    T_CTSK mesh_task = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG3,
                         .task = (FP)routed_mesh_task,
                         .itskpri = ROUTED_MESH_TASK_PRIORITY, .stksz = 2304 };
    T_CTSK logger_task = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG3,
                           .task = (FP)routed_logger_task,
                           .itskpri = ROUTED_LOGGER_TASK_PRIORITY, .stksz = 1536 };
    tavrn_link_config_t link_config;
    aodv_core_config_t aodv_config;
    ID mesh_id;
    ID logger_id;

    ring_init(&delivery_state.published);
    if (!tron_timer_config_is_valid(&tron_timer_config) ||
        ble_radio_try_init(tron_timer_config.radio_state_timeout_ms) != BLE_RADIO_OP_OK ||
        ble_radio_read_default_adva(runtime_ficr_adva) != BLE_RADIO_OP_OK) {
        tm_printf((UB *)"routed bounded radio/FICR/timer initialization failed\n");
        return 1;
    }
    if (TRON_BUILD_ADVA_OVERRIDE_ENABLED != 0) {
        memcpy(local_adva, configured_override, sizeof(local_adva));
    } else {
        memcpy(local_adva, runtime_ficr_adva, sizeof(local_adva));
    }
    memset(&local_peer, 0, sizeof(local_peer));
    local_peer.logical_id.width = TAVRN_IDENTITY_SID16;
    local_peer.logical_id.value = sid16_from_adva(local_adva);
    memcpy(local_peer.adva.bytes, local_adva, sizeof(local_peer.adva.bytes));
    if (TRON_BUILD_LINK_PEER_ADVA_ENABLED != 0) {
        configured_destination.logical_id.width = TAVRN_IDENTITY_SID16;
        configured_destination.logical_id.value = sid16_from_adva(configured_peer_adva);
        memcpy(configured_destination.adva.bytes, configured_peer_adva,
               sizeof(configured_destination.adva.bytes));
    }
    ble_mesh_scheduler_init(&routed_scheduler, now_ms(), local_adva);
    memset(&link_config, 0, sizeof(link_config));
    link_config.local_peer = local_peer;
    link_config.network_id = TRON_BUILD_NETWORK_ID;
    link_config.hack_max_attempts = (uint8_t)tron_timer_config.link_max_attempts;
    link_config.busy_max_responses = (uint8_t)tron_timer_config.link_busy_max_responses;
    link_config.hack_response_ms = tron_timer_config.link_hack_timeout_ms;
    link_config.hack_turnaround_ms = tron_timer_config.radio_tx_event_bound_ms;
    link_config.retry_backoff_ms = tron_timer_config.link_retry_backoff_ms;
    link_config.busy_backoff_ms = tron_timer_config.link_busy_backoff_ms;
    link_config.data_forward_deadline_ms = tron_timer_config.link_data_deadline_ms;
    link_config.candidate_resolve_ms = tron_timer_config.link_candidate_resolve_ms;
    link_config.data_dedupe_ms = tron_timer_config.link_data_dedupe_ms;
    link_config.flood_dedupe_ms = tron_timer_config.link_flood_dedupe_ms;
    link_config.flood_jitter_min_ms = tron_timer_config.link_flood_jitter_min_ms;
    link_config.flood_jitter_max_ms = tron_timer_config.link_flood_jitter_max_ms;
    memset(&aodv_config, 0, sizeof(aodv_config));
    aodv_config.local_peer = local_peer;
    aodv_config.network_id = TRON_BUILD_NETWORK_ID;
    aodv_config.net_diameter = (uint8_t)tron_timer_config.aodv_net_diameter;
    aodv_config.rreq_retries = (uint8_t)tron_timer_config.aodv_rreq_retries;
    aodv_config.rreq_rate_per_second = (uint8_t)tron_timer_config.aodv_rreq_rate;
    aodv_config.rerr_rate_per_second = (uint8_t)tron_timer_config.aodv_rerr_rate;
    aodv_config.request_rrep_ack = 1u;
    aodv_config.initial_origin_sequence = 1u;
    aodv_config.initial_request_id = 1u;
    aodv_config.initial_rerr_sequence = 1u;
    aodv_config.node_traversal_ms = tron_timer_config.aodv_node_traversal_ms;
    aodv_config.path_discovery_ms = tron_timer_config.aodv_path_discovery_ms;
    aodv_config.rreq_seen_ms = tron_timer_config.aodv_rreq_seen_ms;
    aodv_config.active_route_ms = tron_timer_config.aodv_active_route_ms;
    aodv_config.pending_data_ms = tron_timer_config.aodv_pending_data_ms;
    aodv_config.blacklist_ms = tron_timer_config.aodv_blacklist_ms;
    aodv_config.rrep_dedupe_ms = tron_timer_config.aodv_rrep_dedupe_ms;
    aodv_config.rerr_dedupe_ms = tron_timer_config.aodv_rerr_dedupe_ms;
    aodv_config.rrep_ack_wait_ms = tron_timer_config.aodv_rrep_ack_wait_ms;
    if (tavrn_link_v2_init(&routed_link, &routed_scheduler, &link_config, now_ms()) !=
            TAVRN_LINK_INIT_OK ||
        aodv_core_init(&routed_aodv, &aodv_config, now_ms()) != AODV_INIT_OK) {
        tm_printf((UB *)"routed generated link/AODV configuration rejected\n");
        return 1;
    }
    mesh_id = tk_cre_tsk(&mesh_task);
    logger_id = tk_cre_tsk(&logger_task);
    if (mesh_id <= 0 || logger_id <= 0) {
        tm_printf((UB *)"routed task creation failed mesh=%d logger=%d\n", mesh_id,
                  logger_id);
        return 1;
    }
    tk_sta_tsk(logger_id, 0);
    tk_sta_tsk(mesh_id, 0);
    tk_slp_tsk(TMO_FEVR);
    return 0;
}
