/*
 * Dedicated Phase 1 link-v2 proving-ground firmware.
 *
 * This target deliberately has no router, AODV state, route selection, or
 * TAVRN feature level. It only drives the production bounded radio,
 * scheduler, queue, wire-v2, and custody-link modules for bench evidence.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include <stdint.h>
#include <string.h>

#include "ble_mesh_scheduler.h"
#include "ble_mesh_tx_queue.h"
#include "ble_radio.h"
#include "link_testbed_state.h"
#include "tavrn_link_v2.h"
#include "tron_build_config.h"
#include "tron_build_info.h"
#include "tron_timer_config.h"

typedef enum link_test_diag_kind {
    LINK_TEST_DIAG_BOOT = 0,
    LINK_TEST_DIAG_LINK_EVENT,
    LINK_TEST_DIAG_HOOK,
    LINK_TEST_DIAG_DELIVERY,
    LINK_TEST_DIAG_TX_SUBMIT,
    LINK_TEST_DIAG_RX_DECISION,
    LINK_TEST_DIAG_RESOLVE_FAILURE,
    LINK_TEST_DIAG_SCHEDULER_FAULT,
} link_test_diag_kind_t;

typedef struct link_test_diag {
    link_test_diag_kind_t kind;
    uint32_t now_ms;
    uint32_t value;
    tavrn_link_event_t link_event;
    ble_mesh_sched_event_t scheduler_event;
} link_test_diag_t;

typedef struct link_test_final_delivery {
    tavrn_direct_peer_t transmitter;
    tavrn_link_data_t data;
    uint32_t received_at_ms;
} link_test_final_delivery_t;

typedef struct link_test_hook_counters {
    uint32_t rx_blocked;
    uint32_t hack_suppressed;
    uint32_t busy_forced;
    uint32_t diagnostics_dropped;
} link_test_hook_counters_t;

typedef struct link_test_counters {
    uint32_t submission_accepted;
    uint32_t submission_rejected;
    uint32_t submission_no_slot;
    uint32_t submission_faulted;
    uint32_t custody_transferred;
    uint32_t local_tx_not_attempted;
    uint32_t custody_busy_expired;
    uint32_t custody_rejected;
    uint32_t retry_exhausted;
    uint32_t radio_fault_terminal;
    uint32_t service_fault_terminal;
    uint32_t scheduler_radio_fault;
    uint32_t scheduler_service_fault;
    uint32_t final_deliveries;
} link_test_counters_t;

typedef struct link_test_summary_snapshot {
    link_test_counters_t counters;
    link_test_hook_counters_t hooks;
    tavrn_link_counters_t link;
    uint8_t final_queue_count;
    uint8_t final_provisional;
    uint8_t diagnostic_queue_count;
    uint8_t fault_latched;
} link_test_summary_snapshot_t;

typedef char link_test_final_delivery_storage_must_be_eight[
    (LINK_TESTBED_DELIVERY_CAPACITY == 8u) ? 1 : -1];
typedef char link_test_diagnostic_storage_must_be_eight[
    (LINK_TESTBED_DIAGNOSTIC_CAPACITY == 8u) ? 1 : -1];
typedef char link_test_mesh_priority_must_exceed_logger_priority[
    (LINK_TESTBED_MESH_TASK_PRIORITY < LINK_TESTBED_LOGGER_TASK_PRIORITY) ? 1 : -1];

static ble_mesh_scheduler_t link_scheduler;
static tavrn_link_v2_t link_instance;
static tavrn_direct_peer_t local_peer;
static UB local_adva[6];
static UB runtime_ficr_adva[6];
static UB scheduler_adva[6];
static link_test_final_delivery_t final_deliveries[LINK_TESTBED_DELIVERY_CAPACITY];
static link_testbed_delivery_state_t final_delivery_state;
static link_test_diag_t diagnostics[LINK_TESTBED_DIAGNOSTIC_CAPACITY];
static link_testbed_ring_state_t diagnostic_state;
static link_test_hook_counters_t hook_counters;
static link_test_counters_t test_counters;
static uint32_t forced_busy_remaining = TRON_BUILD_BUSY_ADMISSION_COUNT;
static uint32_t suppressed_hacks_remaining = TRON_BUILD_HACK_DROP_COUNT;
static uint16_t next_data_seq = 1u;
static uint32_t sent_transactions;
static uint8_t mesh_fault_latched;
static uint32_t mesh_yield_delay_ms;

static const UB configured_override[6] = TRON_BUILD_ADVA_OVERRIDE_BYTES;
static const UB configured_peer_adva[6] = TRON_BUILD_LINK_PEER_ADVA_BYTES;
static const UB configured_rx_block_adva[6] = TRON_BUILD_RX_BLOCK_ADVA_BYTES;
static const UB configured_hack_drop_adva[6] = TRON_BUILD_HACK_DROP_ADVA_BYTES;

static uint32_t now_ms(void)
{
    SYSTIM time;

    tk_get_otm(&time);
    return time.lo;
}

/* Both queues have exactly one task-level producer and consumer. No ISR
 * accesses either queue. The μT-Kernel dispatch gate therefore makes each
 * copied ring mutation atomic with respect to the other task, without blocking
 * the mesh task or allocating a kernel buffer. These calls are never nested. */
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

static void diagnostic_push(const link_test_diag_t *diagnostic)
{
    uint8_t index;

    if (diagnostic == NULL || !queue_guard_begin()) {
        hook_counters.diagnostics_dropped++;
        return;
    }
    if (!link_testbed_ring_push(&diagnostic_state,
                                LINK_TESTBED_DIAGNOSTIC_CAPACITY, &index)) {
        hook_counters.diagnostics_dropped++;
        queue_guard_end();
        return;
    }
    diagnostics[index] = *diagnostic;
    queue_guard_end();
}

static void diagnostic_push_simple(link_test_diag_kind_t kind, uint32_t now,
                                   uint32_t value)
{
    link_test_diag_t diagnostic;

    memset(&diagnostic, 0, sizeof(diagnostic));
    diagnostic.kind = kind;
    diagnostic.now_ms = now;
    diagnostic.value = value;
    diagnostic_push(&diagnostic);
}

static void diagnostic_push_link_event(uint32_t now,
                                       const tavrn_link_event_t *event)
{
    link_test_diag_t diagnostic;

    if (event == NULL || event->type == TAVRN_LINK_EVENT_NONE) {
        return;
    }
    memset(&diagnostic, 0, sizeof(diagnostic));
    diagnostic.kind = LINK_TEST_DIAG_LINK_EVENT;
    diagnostic.now_ms = now;
    diagnostic.link_event = *event;
    diagnostic_push(&diagnostic);
}

static void diagnostic_push_scheduler_fault(uint32_t now,
                                            const ble_mesh_sched_event_t *event)
{
    link_test_diag_t diagnostic;

    if (event == NULL) {
        return;
    }
    memset(&diagnostic, 0, sizeof(diagnostic));
    diagnostic.kind = LINK_TEST_DIAG_SCHEDULER_FAULT;
    diagnostic.now_ms = now;
    diagnostic.scheduler_event = *event;
    diagnostic_push(&diagnostic);
}

static int diagnostic_pop(link_test_diag_t *out)
{
    uint8_t index;
    int result;

    if (out == NULL || !queue_guard_begin()) {
        return 0;
    }
    result = link_testbed_ring_pop(&diagnostic_state,
                                   LINK_TESTBED_DIAGNOSTIC_CAPACITY, &index);
    if (result) {
        *out = diagnostics[index];
    }
    queue_guard_end();
    return result;
}

static int commit_final_delivery(void)
{
    int result;

    if (!queue_guard_begin()) {
        return 0;
    }
    result = link_testbed_delivery_commit(&final_delivery_state);
    queue_guard_end();
    return result;
}

static void cancel_final_delivery(void)
{
    if (queue_guard_begin()) {
        link_testbed_delivery_cancel(&final_delivery_state);
        queue_guard_end();
    }
}

static int final_delivery_pop(link_test_final_delivery_t *out)
{
    uint8_t index;
    int result;

    if (out == NULL || !queue_guard_begin()) {
        return 0;
    }
    result = link_testbed_delivery_pop(&final_delivery_state, &index);
    if (result) {
        *out = final_deliveries[index];
    }
    queue_guard_end();
    return result;
}

static void snapshot_summary(link_test_summary_snapshot_t *snapshot)
{
    const tavrn_link_counters_t *link_counters;

    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (!queue_guard_begin()) {
        return;
    }
    snapshot->counters = test_counters;
    snapshot->hooks = hook_counters;
    snapshot->final_queue_count = final_delivery_state.published.count;
    snapshot->final_provisional = final_delivery_state.provisional;
    snapshot->diagnostic_queue_count = diagnostic_state.count;
    snapshot->fault_latched = mesh_fault_latched;
    link_counters = tavrn_link_v2_counters(&link_instance);
    if (link_counters != NULL) {
        snapshot->link = *link_counters;
    }
    queue_guard_end();
}

static void log_adva_line(const UB *label, const UB adva[6])
{
    tm_printf((UB *)"linkv2 identity field=%s adva=%02x:%02x:%02x:%02x:%02x:%02x\n",
              (UB *)label, (UINT)adva[0], (UINT)adva[1], (UINT)adva[2],
              (UINT)adva[3], (UINT)adva[4], (UINT)adva[5]);
}

static void log_startup_record(const UB *configured_runtime_equal,
                               const UB *effective_runtime_equal,
                               const UB *effective_scheduler_equal)
{
    tm_printf((UB *)"linkv2 startup %s runtime_ficr=%02x:%02x:%02x:%02x:%02x:%02x effective_local=%02x:%02x:%02x:%02x:%02x:%02x scheduler_adva=%02x:%02x:%02x:%02x:%02x:%02x configured_runtime_equal=%s effective_runtime_equal=%s effective_scheduler_equal=%s sid16=%u sid8=%u fixed_k=disabled mesh_yield_ms=%lu %s %s\n",
              (UB *)TRON_BUILD_RUNTIME_CONFIG_EVIDENCE,
              (UINT)runtime_ficr_adva[0], (UINT)runtime_ficr_adva[1],
              (UINT)runtime_ficr_adva[2], (UINT)runtime_ficr_adva[3],
              (UINT)runtime_ficr_adva[4], (UINT)runtime_ficr_adva[5],
              (UINT)local_adva[0], (UINT)local_adva[1], (UINT)local_adva[2],
              (UINT)local_adva[3], (UINT)local_adva[4], (UINT)local_adva[5],
              (UINT)scheduler_adva[0], (UINT)scheduler_adva[1],
              (UINT)scheduler_adva[2], (UINT)scheduler_adva[3],
              (UINT)scheduler_adva[4], (UINT)scheduler_adva[5],
              (UB *)configured_runtime_equal, (UB *)effective_runtime_equal,
              (UB *)effective_scheduler_equal,
              (UINT)local_peer.logical_id.value, (UINT)local_adva[0],
              (UW)mesh_yield_delay_ms,
              (UB *)TRON_BUILD_RUNTIME_HOOK_EVIDENCE,
              (UB *)TRON_BUILD_RUNTIME_TIMER_EVIDENCE);
}

static void log_link_event(uint32_t now, const tavrn_link_event_t *event)
{
    uint16_t sequence = 0u;
    uint8_t attempts = 0u;
    uint8_t requested = 0u;
    uint8_t completed = 0u;
    uint8_t status = 0xffu;
    uint8_t local_reason = 0xffu;
    uint8_t mesh_reason = 0xffu;
    uint16_t origin = 0u;
    uint16_t destination = 0u;

    if (event == NULL) {
        return;
    }
    if (event->type == TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED) {
        sequence = event->detail.transferred_data.data.data_seq;
        attempts = event->detail.transferred_data.attempt_count;
        requested = event->detail.transferred_data.requested_channel_mask;
        completed = event->detail.transferred_data.completed_channel_mask;
        status = (uint8_t)event->detail.transferred_data.status;
        origin = event->detail.transferred_data.data.origin.value;
        destination = event->detail.transferred_data.data.final_destination.value;
    } else if (event->type >= TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED &&
               event->type <= TAVRN_LINK_EVENT_SERVICE_FAULT_TERMINAL) {
        sequence = event->detail.owned_data.data.data_seq;
        attempts = event->detail.owned_data.attempt_count;
        requested = event->detail.owned_data.requested_channel_mask;
        completed = event->detail.owned_data.completed_channel_mask;
        local_reason = (uint8_t)event->detail.owned_data.local_reason;
        mesh_reason = (uint8_t)event->detail.owned_data.mesh_fault_reason;
        origin = event->detail.owned_data.data.origin.value;
        destination = event->detail.owned_data.data.final_destination.value;
    } else if (event->type == TAVRN_LINK_EVENT_RX_DATA_CANDIDATE) {
        sequence = event->detail.candidate.data.data_seq;
        origin = event->detail.candidate.data.origin.value;
        destination = event->detail.candidate.data.final_destination.value;
    }
    tm_printf((UB *)"linkv2 event now=%lu type=%u seq=%u attempts=%u requested_mask=0x%02x completed_mask=0x%02x hack_status=%u local_reason=%u mesh_reason=%u origin=0x%04x destination=0x%04x\n",
              (UW)now, (UINT)event->type, (UINT)sequence, (UINT)attempts,
              (UINT)requested, (UINT)completed, (UINT)status,
              (UINT)local_reason, (UINT)mesh_reason, (UINT)origin,
              (UINT)destination);
}

static void log_diagnostic(const link_test_diag_t *diagnostic)
{
    if (diagnostic == NULL) {
        return;
    }
    if (diagnostic->kind == LINK_TEST_DIAG_LINK_EVENT) {
        log_link_event(diagnostic->now_ms, &diagnostic->link_event);
        return;
    }
    if (diagnostic->kind == LINK_TEST_DIAG_SCHEDULER_FAULT) {
        const ble_mesh_sched_event_t *event = &diagnostic->scheduler_event;

        tm_printf((UB *)"linkv2 scheduler_fault now=%lu type=%u token=%u requested_mask=0x%02x completed_mask=0x%02x reason=%u\n",
                  (UW)diagnostic->now_ms, (UINT)event->type,
                  (UINT)event->tx_token, (UINT)event->tx_requested_channel_mask,
                  (UINT)event->tx_completed_channel_mask, (UINT)event->fault);
        return;
    }
    tm_printf((UB *)"linkv2 diag now=%lu kind=%u value=%lu\n",
              (UW)diagnostic->now_ms, (UINT)diagnostic->kind,
              (UW)diagnostic->value);
}

static void log_summary(uint32_t now, const UB *phase)
{
    link_test_summary_snapshot_t snapshot;

    snapshot_summary(&snapshot);
    tm_printf((UB *)"linkv2 summary phase=%s now=%lu submit_accepted=%lu submit_rejected=%lu submit_no_slot=%lu submit_faulted=%lu custody_transferred=%lu local_tx_not_attempted=%lu custody_busy_expired=%lu custody_rejected=%lu retry_exhausted=%lu radio_fault_terminal=%lu service_fault_terminal=%lu final_deliveries=%lu final_duplicates=%lu scheduler_radio_fault=%lu scheduler_service_fault=%lu hook_rx_blocked=%lu hook_hack_suppressed=%lu hook_busy_forced=%lu diagnostic_dropped=%lu diag_queue=%u delivery_queue=%u delivery_provisional=%u fault_latched=%u link_hack_enqueue_failed=%lu\n",
              (UB *)phase, (UW)now,
              (UW)snapshot.counters.submission_accepted,
              (UW)snapshot.counters.submission_rejected,
              (UW)snapshot.counters.submission_no_slot,
              (UW)snapshot.counters.submission_faulted,
              (UW)snapshot.counters.custody_transferred,
              (UW)snapshot.counters.local_tx_not_attempted,
              (UW)snapshot.counters.custody_busy_expired,
              (UW)snapshot.counters.custody_rejected,
              (UW)snapshot.counters.retry_exhausted,
              (UW)snapshot.counters.radio_fault_terminal,
              (UW)snapshot.counters.service_fault_terminal,
              (UW)snapshot.counters.final_deliveries,
              (UW)snapshot.link.rx_committed_duplicate,
              (UW)snapshot.counters.scheduler_radio_fault,
              (UW)snapshot.counters.scheduler_service_fault,
              (UW)snapshot.hooks.rx_blocked,
              (UW)snapshot.hooks.hack_suppressed,
              (UW)snapshot.hooks.busy_forced,
              (UW)snapshot.hooks.diagnostics_dropped,
              (UINT)snapshot.diagnostic_queue_count,
              (UINT)snapshot.final_queue_count,
              (UINT)snapshot.final_provisional,
              (UINT)snapshot.fault_latched,
              (UW)snapshot.link.hack_enqueue_failed);
}

static void process_link_event(const tavrn_link_event_t *event, uint32_t now)
{
    if (event == NULL || event->type == TAVRN_LINK_EVENT_NONE) {
        return;
    }
    if (event->type == TAVRN_LINK_EVENT_RX_DATA_CANDIDATE) {
        return;
    }
    if (event->type == TAVRN_LINK_EVENT_CUSTODY_TRANSFERRED) {
        test_counters.custody_transferred++;
    } else if (event->type == TAVRN_LINK_EVENT_LOCAL_TX_NOT_ATTEMPTED) {
        test_counters.local_tx_not_attempted++;
    } else if (event->type == TAVRN_LINK_EVENT_CUSTODY_BUSY_EXPIRED) {
        test_counters.custody_busy_expired++;
    } else if (event->type == TAVRN_LINK_EVENT_CUSTODY_REJECTED) {
        test_counters.custody_rejected++;
    } else if (event->type == TAVRN_LINK_EVENT_RETRY_EXHAUSTED) {
        test_counters.retry_exhausted++;
    } else if (event->type == TAVRN_LINK_EVENT_RADIO_FAULT_TERMINAL) {
        test_counters.radio_fault_terminal++;
    } else if (event->type == TAVRN_LINK_EVENT_SERVICE_FAULT_TERMINAL) {
        test_counters.service_fault_terminal++;
    }
    diagnostic_push_link_event(now, event);
}

static void suppress_captured_hack(const link_testbed_hack_capture_t *capture,
                                   uint32_t now)
{
    if (capture == NULL || capture->armed == 0u ||
        suppressed_hacks_remaining == 0u) {
        return;
    }
    if (link_testbed_remove_captured_hack(&link_scheduler.routed_tx_queue,
                                          &local_peer, capture)) {
        suppressed_hacks_remaining--;
        hook_counters.hack_suppressed++;
        diagnostic_push_simple(LINK_TEST_DIAG_HOOK, now, 2u);
    }
}

static void record_scheduler_fault(const ble_mesh_sched_event_t *event, uint32_t now)
{
    if (event == NULL) {
        return;
    }
    if (event->type == BLE_MESH_SCHED_EVENT_RADIO_FAULT) {
        test_counters.scheduler_radio_fault++;
    } else if (event->type == BLE_MESH_SCHED_EVENT_SERVICE_FAULT) {
        test_counters.scheduler_service_fault++;
    } else {
        return;
    }
    diagnostic_push_scheduler_fault(now, event);
}

static void capture_duplicate_hack(const ble_mesh_sched_event_t *event,
                                   link_testbed_hack_capture_t *capture)
{
    tavrn_codec_config_t config;
    tavrn_decoded_frame_t frame;

    memset(capture, 0, sizeof(*capture));
    if (TRON_BUILD_TEST_HOOKS == 0 || suppressed_hacks_remaining == 0u ||
        TRON_BUILD_HACK_DROP_ADVA_ENABLED == 0 || event == NULL ||
        event->type != BLE_MESH_SCHED_EVENT_RX_ADV) {
        return;
    }
    memset(&config, 0, sizeof(config));
    config.network_id = TRON_BUILD_NETWORK_ID;
    config.local_peer = local_peer;
    if (tavrn_wire_v2_decode(&config, event->adv_addr, event->adv_data,
                             event->adv_len, &frame) != TAVRN_CODEC_OK ||
        frame.type != TAVRN_WIRE_DATA) {
        return;
    }
    link_testbed_capture_hack(capture, &link_scheduler.routed_tx_queue,
                               configured_hack_drop_adva, &frame.transmitter,
                               &frame.detail.data.data, TAVRN_HACK_DUPLICATE,
                               TRON_BUILD_NETWORK_ID);
}

static void resolve_candidate(const tavrn_rx_data_candidate_t *candidate, uint32_t now)
{
    tavrn_rx_decision_t decision;
    tavrn_link_event_t outcome;
    link_testbed_hack_capture_t capture;
    tavrn_link_resolve_status_t resolved;
    uint32_t forced_before;
    uint8_t reserved_index = 0u;

    if (candidate == NULL) {
        return;
    }
    forced_before = forced_busy_remaining;
    if (!queue_guard_begin()) {
        decision = TAVRN_RX_BUSY;
    } else {
        decision = link_testbed_admit_final(&candidate->data,
                                             &local_peer.logical_id,
                                             &final_delivery_state,
                                             &forced_busy_remaining,
                                             &reserved_index);
        if (decision == TAVRN_RX_ACCEPTED) {
            memset(&final_deliveries[reserved_index], 0,
                   sizeof(final_deliveries[reserved_index]));
            final_deliveries[reserved_index].transmitter = candidate->transmitter;
            final_deliveries[reserved_index].data = candidate->data;
            final_deliveries[reserved_index].received_at_ms = now;
        }
        queue_guard_end();
    }
    if (forced_busy_remaining != forced_before) {
        hook_counters.busy_forced++;
        diagnostic_push_simple(LINK_TEST_DIAG_HOOK, now, 3u);
    }
    diagnostic_push_simple(LINK_TEST_DIAG_RX_DECISION, now, (uint32_t)decision);
    memset(&capture, 0, sizeof(capture));
    if (TRON_BUILD_TEST_HOOKS != 0 && suppressed_hacks_remaining != 0u &&
        TRON_BUILD_HACK_DROP_ADVA_ENABLED != 0) {
        link_testbed_capture_hack(&capture, &link_scheduler.routed_tx_queue,
                                   configured_hack_drop_adva,
                                    &candidate->transmitter, &candidate->data,
                                    decision == TAVRN_RX_ACCEPTED ? TAVRN_HACK_ACCEPTED :
                                    decision == TAVRN_RX_BUSY ? TAVRN_HACK_BUSY :
                                    TAVRN_HACK_REJECTED,
                                    TRON_BUILD_NETWORK_ID);
    }
    resolved = tavrn_link_v2_resolve_rx(&link_instance, candidate->token, decision,
                                        now, &outcome);
    /* This call is immediately adjacent to resolve_rx: only the HACK generated
     * for this candidate can own capture.ordinal. */
    suppress_captured_hack(&capture, now);
    if (resolved != TAVRN_LINK_RESOLVE_OK) {
        if (decision == TAVRN_RX_ACCEPTED) {
            cancel_final_delivery();
        }
        diagnostic_push_simple(LINK_TEST_DIAG_RESOLVE_FAILURE, now,
                               (uint32_t)resolved);
        return;
    }
    if (decision == TAVRN_RX_ACCEPTED) {
        if (!commit_final_delivery()) {
            /* resolve_rx has committed link custody. This impossible local
             * publication failure is fail-closed: release private storage,
             * never claim a delivery, and stop the mesh path visibly. */
            cancel_final_delivery();
            mesh_fault_latched = 1u;
            diagnostic_push_simple(LINK_TEST_DIAG_RESOLVE_FAILURE, now,
                                   (uint32_t)TAVRN_MESH_FAULT_INTERNAL_STATE);
        } else {
            test_counters.final_deliveries++;
            diagnostic_push_simple(LINK_TEST_DIAG_DELIVERY, now,
                                   candidate->data.data_seq);
        }
    }
    process_link_event(&outcome, now);
}

static void latch_poll_overrun(uint32_t now)
{
    ble_mesh_sched_event_t fault;
    tavrn_link_event_t outcome;

    memset(&fault, 0, sizeof(fault));
    fault.type = BLE_MESH_SCHED_EVENT_SERVICE_FAULT;
    fault.fault = BLE_MESH_SCHED_FAULT_POLL_OVERRUN;
    mesh_fault_latched = 1u;
    test_counters.scheduler_service_fault++;
    diagnostic_push_scheduler_fault(now, &fault);
    (void)tavrn_link_v2_on_scheduler_event(&link_instance, &fault, now, &outcome);
    process_link_event(&outcome, now);
}

static void handle_scheduler_event(const ble_mesh_sched_event_t *event, uint32_t now)
{
    tavrn_link_event_t outcome;
    link_testbed_hack_capture_t capture;

    if (event == NULL) {
        return;
    }
    if (event->type == BLE_MESH_SCHED_EVENT_RADIO_FAULT) {
        record_scheduler_fault(event, now);
        mesh_fault_latched = 1u;
    } else if (event->type == BLE_MESH_SCHED_EVENT_SERVICE_FAULT) {
        record_scheduler_fault(event, now);
        mesh_fault_latched = 1u;
    }
    if (TRON_BUILD_TEST_HOOKS != 0 &&
        TRON_BUILD_RX_BLOCK_ADVA_ENABLED != 0 &&
        event->type == BLE_MESH_SCHED_EVENT_RX_ADV &&
        adva_equal(event->adv_addr, configured_rx_block_adva)) {
        hook_counters.rx_blocked++;
        diagnostic_push_simple(LINK_TEST_DIAG_HOOK, now, 1u);
        return;
    }
    capture_duplicate_hack(event, &capture);
    (void)tavrn_link_v2_on_scheduler_event(&link_instance, event, now, &outcome);
    /* This call is immediately adjacent to on_scheduler_event; an older HACK
     * cannot satisfy the captured insertion ordinal. */
    suppress_captured_hack(&capture, now);
    if (outcome.type == TAVRN_LINK_EVENT_RX_DATA_CANDIDATE) {
        resolve_candidate(&outcome.detail.candidate, now);
    } else {
        process_link_event(&outcome, now);
    }
}

static void maybe_submit_diagnostic_data(uint32_t now, uint32_t *next_tx_at)
{
    tavrn_direct_peer_t peer;
    tavrn_link_data_t data;
    tavrn_link_event_t outcome;
    tavrn_link_send_status_t result;

    if (TRON_BUILD_LINK_INITIATOR == 0 || TRON_BUILD_LINK_PEER_ADVA_ENABLED == 0 ||
        mesh_fault_latched != 0u || sent_transactions >= TRON_BUILD_LINK_TRANSACTION_TARGET ||
        (int32_t)(now - *next_tx_at) < 0) {
        return;
    }
    memset(&peer, 0, sizeof(peer));
    peer.logical_id.width = TAVRN_IDENTITY_SID16;
    peer.logical_id.value = sid16_from_adva(configured_peer_adva);
    memcpy(peer.adva.bytes, configured_peer_adva, sizeof(peer.adva.bytes));
    memset(&data, 0, sizeof(data));
    data.origin = local_peer.logical_id;
    data.final_destination = peer.logical_id;
    data.data_seq = next_data_seq++;
    data.ttl = 1u;
    data.app_kind = 0x7fu;
    data.app_source = 0u;
    data.app_len = 4u;
    data.app_bytes[0] = (uint8_t)(sent_transactions & 0xffu);
    data.app_bytes[1] = (uint8_t)((sent_transactions >> 8) & 0xffu);
    data.app_bytes[2] = (uint8_t)((sent_transactions >> 16) & 0xffu);
    data.app_bytes[3] = (uint8_t)((sent_transactions >> 24) & 0xffu);
    data.ownership = TAVRN_DATA_ORIGINATED;
    result = tavrn_link_v2_send_unicast(&link_instance, &peer, &data, now, &outcome);
    if (result == TAVRN_LINK_SEND_OK) {
        sent_transactions++;
        test_counters.submission_accepted++;
    } else if (result == TAVRN_LINK_SEND_NO_SLOT) {
        test_counters.submission_no_slot++;
    } else if (result == TAVRN_LINK_SEND_MESH_FAULTED ||
               result == TAVRN_LINK_SEND_LOCAL_NOT_ATTEMPTED) {
        test_counters.submission_faulted++;
    } else {
        test_counters.submission_rejected++;
    }
    diagnostic_push_simple(LINK_TEST_DIAG_TX_SUBMIT, now, (uint32_t)result);
    process_link_event(&outcome, now);
    *next_tx_at = now + TRON_BUILD_LINK_TX_INTERVAL_MS;
}

LOCAL void link_mesh_task(INT stacd, void *exinf)
{
    ble_mesh_sched_event_t scheduler_event;
    tavrn_link_event_t link_event;
    uint32_t next_tx_at = now_ms();
    link_testbed_poll_gate_t poll_gate;

    (void)stacd;
    (void)exinf;
    ble_mesh_scheduler_start_rx(&link_scheduler, now_ms());
    link_testbed_poll_gate_init(&poll_gate);
    diagnostic_push_simple(LINK_TEST_DIAG_BOOT, now_ms(), 0u);
    while (1) {
        uint32_t poll_start;
        if (mesh_fault_latched == 0u) {
            /* The pre-poll gate is the only healthy-cycle work before poll.
             * It measures from the prior scheduler-operation return and faults
             * before a new radio operation could extend an overrun. */
            poll_start = now_ms();
            if (!link_testbed_poll_gate_begin(&poll_gate, poll_start,
                                               tron_timer_config.scheduler_poll_max_ms)) {
                latch_poll_overrun(poll_start);
            } else {
                memset(&scheduler_event, 0, sizeof(scheduler_event));
                (void)ble_mesh_scheduler_poll(&link_scheduler, poll_start,
                                               &scheduler_event);
                link_testbed_poll_gate_complete(&poll_gate, now_ms());
                if (scheduler_event.type != BLE_MESH_SCHED_EVENT_NONE) {
                    handle_scheduler_event(&scheduler_event, poll_start);
                }
            }
        }

        /* At most one due link transition is drained per cycle. Fault draining
         * therefore returns one owned terminal item on each later cycle. */
        (void)tavrn_link_v2_tick(&link_instance, now_ms(), &link_event);
        process_link_event(&link_event, now_ms());
        if (mesh_fault_latched == 0u) {
            /* The test initiator is one bounded local application submission;
             * no serial I/O or blocking callback runs in this task. */
            maybe_submit_diagnostic_data(now_ms(), &next_tx_at);
            (void)tavrn_link_v2_dispatch(&link_instance, now_ms(), &link_event);
            process_link_event(&link_event, now_ms());
        }

        /* Logger priority is lower, so its UART byte polling is preempted when
         * this bounded mesh wait expires. The next pre-poll gate remains the
         * authority if kernel wakeup latency nevertheless exceeds the budget. */
        (void)tk_dly_tsk(mesh_yield_delay_ms);
    }
}

LOCAL void link_logger_task(INT stacd, void *exinf)
{
    link_test_diag_t diagnostic;
    link_test_final_delivery_t delivery;
    uint32_t next_summary_at = now_ms() + tron_timer_config.stats_ms;
    uint8_t final_summary_emitted = 0u;

    (void)stacd;
    (void)exinf;
    while (1) {
        uint32_t now = now_ms();

        if (diagnostic_pop(&diagnostic)) {
            log_diagnostic(&diagnostic);
        } else if (final_delivery_pop(&delivery)) {
            tm_printf((UB *)"linkv2 final_delivery now=%lu origin=0x%04x destination=0x%04x seq=%u app_kind=0x%02x app_source=%u app_len=%u peer=%02x:%02x:%02x:%02x:%02x:%02x\n",
                      (UW)delivery.received_at_ms, (UINT)delivery.data.origin.value,
                      (UINT)delivery.data.final_destination.value,
                      (UINT)delivery.data.data_seq, (UINT)delivery.data.app_kind,
                      (UINT)delivery.data.app_source, (UINT)delivery.data.app_len,
                      (UINT)delivery.transmitter.adva.bytes[0],
                      (UINT)delivery.transmitter.adva.bytes[1],
                      (UINT)delivery.transmitter.adva.bytes[2],
                      (UINT)delivery.transmitter.adva.bytes[3],
                      (UINT)delivery.transmitter.adva.bytes[4],
                      (UINT)delivery.transmitter.adva.bytes[5]);
        } else if ((int32_t)(now - next_summary_at) >= 0) {
            log_summary(now, (const UB *)"periodic");
            next_summary_at = now + tron_timer_config.stats_ms;
        } else if (final_summary_emitted == 0u &&
                   (mesh_fault_latched != 0u ||
                    (TRON_BUILD_LINK_INITIATOR != 0 &&
                     test_counters.custody_transferred >=
                         TRON_BUILD_LINK_TRANSACTION_TARGET))) {
            log_summary(now, (const UB *)"final");
            final_summary_emitted = 1u;
        }
        (void)tk_dly_tsk(1u);
    }
}

EXPORT INT usermain(void)
{
    T_CTSK mesh_task = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG3,
                         .task = (FP)link_mesh_task,
                         .itskpri = LINK_TESTBED_MESH_TASK_PRIORITY, .stksz = 2048 };
    T_CTSK logger_task = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG3,
                           .task = (FP)link_logger_task,
                           .itskpri = LINK_TESTBED_LOGGER_TASK_PRIORITY, .stksz = 1536 };
    tavrn_link_config_t config;
    ID mesh_id;
    ID logger_id;
    const UB *configured_runtime_equal;
    const UB *effective_runtime_equal;

    link_testbed_delivery_init(&final_delivery_state);
    link_testbed_ring_init(&diagnostic_state);
    if (!tron_timer_config_is_valid(&tron_timer_config) ||
        (mesh_yield_delay_ms = link_testbed_mesh_yield_delay_ms(
             tron_timer_config.scheduler_poll_max_ms)) == 0u ||
        ble_radio_try_init(tron_timer_config.radio_state_timeout_ms) != BLE_RADIO_OP_OK ||
        ble_radio_read_default_adva(runtime_ficr_adva) != BLE_RADIO_OP_OK) {
        tm_printf((UB *)"linkv2 bounded radio/FICR/timer initialization failed\n");
        return 1;
    }
    if (TRON_BUILD_ADVA_OVERRIDE_ENABLED != 0) {
        memcpy(local_adva, configured_override, sizeof(local_adva));
        configured_runtime_equal = adva_equal(configured_override, runtime_ficr_adva) ?
            (const UB *)"yes" : (const UB *)"no";
        effective_runtime_equal = configured_runtime_equal;
        if (!adva_equal(local_adva, runtime_ficr_adva) &&
            TRON_BUILD_HARDWARE_CANDIDATE != 0) {
            tm_printf((UB *)"linkv2 candidate override/FICR mismatch\n");
            return 1;
        }
    } else {
        memcpy(local_adva, runtime_ficr_adva, sizeof(local_adva));
        configured_runtime_equal = (const UB *)"not_configured";
        effective_runtime_equal = (const UB *)"yes";
    }
    memset(&local_peer, 0, sizeof(local_peer));
    local_peer.logical_id.width = TAVRN_IDENTITY_SID16;
    local_peer.logical_id.value = sid16_from_adva(local_adva);
    memcpy(local_peer.adva.bytes, local_adva, sizeof(local_peer.adva.bytes));
    ble_mesh_scheduler_init(&link_scheduler, now_ms(), local_adva);
    if (!ble_mesh_scheduler_copy_local_adva(&link_scheduler, scheduler_adva) ||
        !adva_equal(local_adva, scheduler_adva)) {
        tm_printf((UB *)"linkv2 scheduler identity rejected\n");
        return 1;
    }
    memset(&config, 0, sizeof(config));
    config.local_peer = local_peer;
    config.network_id = TRON_BUILD_NETWORK_ID;
    config.hack_max_attempts = (uint8_t)tron_timer_config.link_max_attempts;
    config.busy_max_responses = (uint8_t)tron_timer_config.link_busy_max_responses;
    config.hack_response_ms = tron_timer_config.link_hack_timeout_ms;
    config.retry_backoff_ms = tron_timer_config.link_retry_backoff_ms;
    config.busy_backoff_ms = tron_timer_config.link_busy_backoff_ms;
    config.data_forward_deadline_ms = tron_timer_config.link_data_deadline_ms;
    config.candidate_resolve_ms = tron_timer_config.link_candidate_resolve_ms;
    config.data_dedupe_ms = tron_timer_config.link_data_dedupe_ms;
    config.flood_dedupe_ms = tron_timer_config.link_flood_dedupe_ms;
    config.flood_jitter_min_ms = tron_timer_config.link_flood_jitter_min_ms;
    config.flood_jitter_max_ms = tron_timer_config.link_flood_jitter_max_ms;
    if (tavrn_link_v2_init(&link_instance, &link_scheduler, &config, now_ms()) !=
        TAVRN_LINK_INIT_OK) {
        tm_printf((UB *)"linkv2 generated link configuration rejected\n");
        return 1;
    }
    tm_printf((UB *)"%s\n", (UB *)tron_build_info_line());
    log_adva_line((const UB *)"runtime_ficr", runtime_ficr_adva);
    log_adva_line((const UB *)"effective_local", local_adva);
    log_adva_line((const UB *)"scheduler_copy", scheduler_adva);
    log_startup_record(configured_runtime_equal, effective_runtime_equal,
                       (const UB *)"yes");
    mesh_id = tk_cre_tsk(&mesh_task);
    logger_id = tk_cre_tsk(&logger_task);
    if (mesh_id <= 0 || logger_id <= 0) {
        tm_printf((UB *)"linkv2 task creation failed mesh=%d logger=%d\n", mesh_id, logger_id);
        return 1;
    }
    tk_sta_tsk(logger_id, 0);
    tk_sta_tsk(mesh_id, 0);
    tk_slp_tsk(TMO_FEVR);
    return 0;
}
