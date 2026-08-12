/*
 * Routed firmware binding.  The mesh task owns one scheduler and one
 * tavrn_router; route/link/custody orchestration is intentionally not
 * duplicated here.
 */

#include <tk/tkernel.h>
#include <tm/tmonitor.h>

#include <stdint.h>
#include <string.h>

#include "aodv_core.h"
#include "ble_mesh_scheduler.h"
#include "ble_radio.h"
#include "routed_cycle.h"
#include "tavrn_link_v2.h"
#include "tavrn_router.h"
#include "tron_build_config.h"
#include "tron_build_info.h"
#include "tron_timer_config.h"

#if TRON_BUILD_BENCHMARK_MODE
#include "routed_benchmark.h"
#include "routed_benchmark_observer.h"
#endif

#if TRON_BUILD_BENCHMARK_MODE || TRON_BUILD_BENCH_IDENTIFY_DISPLAY
#include "display.h"
#endif

#if TRON_BUILD_ROUTED_FULL_TAVRN
#include "routed_full_telemetry.h"
#include "tavrn_mentorship.h"
#include "tavrn_maintenance.h"
#include "tavrn_full.h"
#include "tavrn_full_maintenance_binding.h"
#include "tavrn_gtt.h"
#if TRON_BUILD_BENCHMARK_MODE
#include "routed_benchmark_full.h"
#endif
#if TRON_BUILD_LOCAL_REPAIR
#include "tavrn_full_repair_binding.h"
#endif
#endif

#define ROUTED_DELIVERY_CAPACITY 8u
#define ROUTED_MESH_TASK_PRIORITY 10u
#define ROUTED_LOGGER_TASK_PRIORITY 11u
#define ROUTED_RELEASE_BIT 0x00000001u
#define ROUTED_MESH_TASK_STACK_BYTES TRON_BUILD_ROUTED_MESH_TASK_STACK_BYTES
#define ROUTED_LOGGER_TASK_STACK_BYTES TRON_BUILD_ROUTED_LOGGER_TASK_STACK_BYTES
#define ROUTED_RNG_BASE 0x4000D000UL
#define ROUTED_RNG_TASKS_START (ROUTED_RNG_BASE + 0x000UL)
#define ROUTED_RNG_TASKS_STOP (ROUTED_RNG_BASE + 0x004UL)
#define ROUTED_RNG_EVENTS_VALRDY (ROUTED_RNG_BASE + 0x100UL)
#define ROUTED_RNG_VALUE (ROUTED_RNG_BASE + 0x508UL)
#define ROUTED_BOOT_NONCE_RNG_POLLS 4096u
#define ROUTED_BOOT_NONCE_RNG_ATTEMPTS 4u
#define ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY 1u

typedef struct routed_ring {
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} routed_ring_t;

typedef struct routed_delivery_state {
    routed_ring_t published;
    tavrn_direct_peer_t transmitter;
    tavrn_link_data_t data;
    tavrn_router_delivery_token_t token;
    uint8_t provisional;
    uint8_t reserved_index;
    uint8_t callback_active;
} routed_delivery_state_t;

typedef struct routed_delivery {
    tavrn_direct_peer_t transmitter;
    tavrn_link_data_t data;
    uint32_t delivered_at_ms;
} routed_delivery_t;

typedef struct routed_local_event {
    tavrn_router_dispatch_event_t event;
} routed_local_event_t;

typedef struct routed_counters {
    uint32_t submitted;
    uint32_t delivered;
    uint32_t local_custody_fault;
    uint32_t scheduler_fault;
    uint32_t rx_blocked;
    uint32_t pending_failed;
    uint32_t blacklisted;
    uint32_t router_fault;
} routed_counters_t;

#if TRON_BUILD_ROUTED_FULL_TAVRN
typedef struct routed_expiry_sweep_record {
    uint32_t now_ms;
    uint32_t received_evidence_count;
    uint32_t pass;
    uint32_t complete_passes;
    uint32_t max_scheduler_gap_ms;
    uint32_t scheduler_fault_count;
    tavrn_router_fault_reason_t router_fault;
    uint8_t trace_count;
    uint8_t cursor;
    uint8_t soft_selected_count;
    uint8_t hard_selected_count;
    uint8_t demand_deferred_count;
    uint8_t unavailable_count;
    uint8_t departed_count;
    uint8_t purged_count;
    uint8_t maximum_copied_candidates;
    uint8_t targeted_control_count;
    uint8_t rreq_control_count;
    uint8_t expiry_tc_control_count;
    uint8_t mesh_fault_latched;
} routed_expiry_sweep_record_t;

typedef struct routed_expiry_sweep_queue {
    routed_expiry_sweep_record_t records[ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY];
    uint8_t head;
    uint8_t tail;
    uint8_t count;
} routed_expiry_sweep_queue_t;
#endif

typedef struct routed_snapshot {
    routed_counters_t routed;
    tavrn_link_counters_t link;
    aodv_counters_t aodv;
    tavrn_router_counters_t router;
    tavrn_router_fault_reason_t router_fault_reason;
    uint8_t delivery_queue_count;
    uint8_t delivery_provisional;
    uint8_t mesh_fault_latched;
    uint8_t diagnostic_queue_count;
    uint8_t rreq_queue_count;
    uint8_t retry_log_pending;
    uint32_t diagnostic_dropped;
    uint32_t diagnostic_evicted_healthy;
    uint32_t diagnostic_dropped_fault;
    uint32_t rreq_dropped;
    uint32_t retry_log_dropped;
    uint32_t phase_max_elapsed_ms[ROUTED_CYCLE_PHASE_UNKNOWN];
#if TRON_BUILD_ROUTED_FULL_TAVRN
    tavrn_mentorship_state_snapshot_t mentorship;
    tavrn_mentorship_counters_t mentorship_counters;
    tavrn_maintenance_snapshot_t maintenance;
    tavrn_maintenance_counters_t maintenance_counters;
    tavrn_tc_metadata_telemetry_t tc_metadata;
    tavrn_targeted_freshness_status_t targeted_owner_status;
    tavrn_targeted_freshness_action_t targeted_owner_action;
    tavrn_adva_t serving_mentee;
    uint16_t serving_boot_nonce;
    uint8_t pending_offer_valid;
    uint8_t serving_session_valid;
    uint8_t page_session_valid;
#endif
} routed_snapshot_t;

#if TRON_BUILD_BENCHMARK_MODE
typedef struct routed_benchmark_health_faults {
    uint32_t attempt_queue_faults;
    uint32_t guard_faults;
    uint32_t snapshot_faults;
} routed_benchmark_health_faults_t;

typedef struct routed_benchmark_snapshot {
    routed_benchmark_state_t state;
    routed_benchmark_attempt_queue_snapshot_t attempts;
    routed_benchmark_health_faults_t faults;
    uint32_t diagnostic_dropped;
    uint32_t rreq_dropped;
    uint32_t retry_log_dropped;
    tavrn_router_fault_reason_t router_fault;
    uint8_t final_queue_count;
    uint8_t mesh_fault_latched;
} routed_benchmark_snapshot_t;

typedef struct routed_benchmark_gtt_emission {
    uint32_t query_at_ms;
    uint8_t entry_cursor;
    uint8_t active;
    uint8_t invalid;
} routed_benchmark_gtt_emission_t;

/* Benchmark copies are consumed only by the single logger task.  Producers
 * populate them under queue_guard_begin(), then release the guard before the
 * logger formats a record.  Keeping this storage benchmark-only preserves the
 * normal routed image's state footprint while removing large logger frames. */
typedef struct routed_benchmark_logger_storage {
    routed_benchmark_observer_t observer;
    tavrn_link_counters_t link;
    aodv_counters_t aodv;
    tavrn_router_counters_t router;
    routed_benchmark_snapshot_t health;
    routed_delivery_t delivery;
    routed_local_event_t local_event;
} routed_benchmark_logger_storage_t;
#endif

#if TRON_BUILD_ROUTED_FULL_TAVRN && !TRON_BUILD_BENCHMARK_MODE
/* The logger never needs its summary and copied GTT snapshot at once. */
typedef union routed_full_logger_storage {
    routed_snapshot_t summary;
    routed_cycle_gtt_snapshot_t gtt_snapshot;
} routed_full_logger_storage_t;
#endif

/* Only the logger writes this scratch after each guarded pop and prints that
 * copy before selecting another logger record.  It is never shared with mesh
 * snapshots, which may be populated after their guard has been released. */
typedef union routed_logger_record {
    routed_cycle_trace_t diagnostic;
    tavrn_link_event_t retry_event;
} routed_logger_record_t;

typedef char routed_delivery_capacity_must_be_eight[
    (ROUTED_DELIVERY_CAPACITY == 8u) ? 1 : -1];
typedef char routed_logger_must_be_lower_priority[
    (ROUTED_MESH_TASK_PRIORITY < ROUTED_LOGGER_TASK_PRIORITY) ? 1 : -1];
#if TRON_BUILD_BENCHMARK_MODE
typedef char routed_benchmark_build_capacity_guard[
    (TRON_BUILD_BENCHMARK_ATTEMPT_QUEUE_CAPACITY ==
     ROUTED_BENCHMARK_ATTEMPT_QUEUE_CAPACITY) ? 1 : -1];
#endif

static ble_mesh_scheduler_t routed_scheduler;
static tavrn_link_v2_t routed_link;
static aodv_core_t routed_aodv;
static tavrn_router_t routed_router;
static routed_cycle_t routed_cycle_state;
static routed_cycle_operations_t routed_cycle_operations;
static routed_cycle_run_result_t routed_cycle_result_storage;
static routed_cycle_diagnostic_queue_t routed_diagnostic_queue;
static routed_cycle_rreq_queue_t routed_rreq_queue;
static routed_cycle_retry_log_mailbox_t routed_retry_log;
static routed_logger_record_t routed_logger_record;
static aodv_rreq_lifecycle_record_t routed_logged_rreq;
static tavrn_router_phase_trace_t routed_scheduler_fault_trace;
#if TRON_BUILD_ROUTED_FULL_TAVRN && !TRON_BUILD_BENCHMARK_MODE
static routed_full_logger_storage_t routed_full_logger_storage;
#elif !TRON_BUILD_BENCHMARK_MODE
static routed_snapshot_t routed_aodv_logger_summary;
#endif
static uint32_t routed_phase_max_elapsed_ms[ROUTED_CYCLE_PHASE_UNKNOWN];
#if !TRON_BUILD_BENCHMARK_MODE
static uint32_t routed_next_submit_at;
#endif
#if TRON_BUILD_BENCHMARK_MODE
static routed_benchmark_state_t routed_benchmark_state;
static routed_benchmark_attempt_queue_t routed_benchmark_attempt_queue;
static routed_benchmark_attempt_t routed_benchmark_logged_attempt;
static routed_benchmark_attempt_t routed_benchmark_pending_attempt;
static routed_benchmark_observer_t routed_benchmark_observer;
static routed_benchmark_health_faults_t routed_benchmark_health_faults;
static routed_benchmark_logger_storage_t routed_benchmark_logger_storage;
static uint64_t routed_benchmark_logged_offer_record_id;
static uint32_t routed_benchmark_logged_offer_identity;
static uint8_t routed_benchmark_logged_offer_valid;
static uint8_t routed_benchmark_pending_attempt_valid;
#if TRON_BUILD_ROUTED_FULL_TAVRN
static routed_benchmark_gtt_emission_t routed_benchmark_gtt_emission;
static routed_cycle_gtt_snapshot_t routed_benchmark_gtt_snapshot;
#endif
#endif
#if TRON_BUILD_ROUTED_FULL_TAVRN
static tavrn_gtt_storage_t routed_gtt_storage;
static tavrn_gtt_t routed_gtt;
static tavrn_full_t routed_full;
static tavrn_mentorship_t routed_mentorship;
static tavrn_maintenance_t routed_maintenance;
static tavrn_full_application_mailbox_t routed_application_mailbox;
static tavrn_full_maintenance_binding_input_t routed_binding_input_storage;
static tavrn_full_maintenance_binding_result_t routed_binding_result_storage;
static tavrn_gtt_application_command_t routed_binding_command_storage;
static tavrn_targeted_freshness_rx_input_t routed_targeted_rx_input_storage;
static tavrn_targeted_freshness_action_t routed_targeted_rx_action_storage;
static tavrn_targeted_freshness_status_t routed_targeted_rx_status;
static tavrn_maintenance_high_token_dispatch_result_t routed_high_token_dispatch;
#if TRON_BUILD_LOCAL_REPAIR
static tavrn_repair_t routed_repair;
static tavrn_full_repair_binding_t routed_repair_binding;
static tavrn_full_repair_binding_tick_result_t routed_repair_tick_result;
#endif
#if !TRON_BUILD_BENCHMARK_MODE
static uint32_t routed_gtt_snapshot_request_dropped;
static uint32_t routed_gtt_snapshot_failed;
static uint32_t routed_local_broadcast_generation_seen;
static uint8_t routed_gtt_snapshot_request_pending;
static uint8_t routed_gtt_snapshot_ready;
static uint8_t routed_full_logger_summary_active;
static routed_expiry_sweep_queue_t routed_expiry_sweep_queue;
static routed_expiry_sweep_record_t routed_logged_expiry_sweep;
#endif
#if !TRON_BUILD_BENCHMARK_MODE
static uint32_t routed_expiry_sweep_passes;
static uint32_t routed_expiry_complete_passes;
#endif
static uint8_t routed_router_tick_scheduler_return_requested;
#endif
static tavrn_direct_peer_t local_peer;
static tavrn_direct_peer_t configured_destination;
static UB local_adva[6];
static UB runtime_ficr_adva[6];
static routed_delivery_t deliveries[ROUTED_DELIVERY_CAPACITY];
static routed_local_event_t local_events[ROUTED_DELIVERY_CAPACITY];
static routed_delivery_state_t delivery_state;
static routed_ring_t local_event_ring;
static routed_counters_t routed_counters;
static tavrn_router_delivery_token_t next_delivery_token;
static tavrn_router_fault_reason_t pending_router_fault;
static tavrn_router_fault_reason_t logged_router_fault;
static uint8_t mesh_fault_latched;
static ID routed_release_flag_id;
static volatile uint32_t routed_logger_progress_epoch;
static uint32_t routed_max_scheduler_gap_ms;

static const UB configured_local_adva[6] = TRON_BUILD_LOCAL_ADVA_BYTES;
static const UB configured_peer_adva[6] = TRON_BUILD_LINK_PEER_ADVA_BYTES;
static const UB configured_rx_block_adva[6] = TRON_BUILD_RX_BLOCK_ADVA_BYTES;

static uint32_t now_ms(void)
{
    SYSTIM time;

    tk_get_otm(&time);
    return time.lo;
}

/* The nRF52833 RNG signals EVENTS_VALRDY for each byte in VALUE.  Startup
 * bounds both polling and zero retries, then fails closed rather than deriving
 * an incarnation discriminator from the static FICR identity. */
static int routed_boot_nonce(uint16_t *nonce_out)
{
    uint32_t attempt;

    if (nonce_out == NULL) {
        return 0;
    }
    *nonce_out = 0u;
    for (attempt = 0u; attempt < ROUTED_BOOT_NONCE_RNG_ATTEMPTS; attempt++) {
        uint16_t nonce = 0u;
        uint8_t byte_index;

        out_w(ROUTED_RNG_EVENTS_VALRDY, 0u);
        out_w(ROUTED_RNG_TASKS_START, 1u);
        for (byte_index = 0u; byte_index < 2u; byte_index++) {
            uint32_t polls;

            for (polls = 0u; polls < ROUTED_BOOT_NONCE_RNG_POLLS; polls++) {
                if (in_w(ROUTED_RNG_EVENTS_VALRDY) != 0u) {
                    break;
                }
            }
            if (polls == ROUTED_BOOT_NONCE_RNG_POLLS) {
                out_w(ROUTED_RNG_TASKS_STOP, 1u);
                return 0;
            }
            nonce |= (uint16_t)(in_w(ROUTED_RNG_VALUE) & 0xffu) <<
                (uint8_t)(8u * byte_index);
            out_w(ROUTED_RNG_EVENTS_VALRDY, 0u);
        }
        out_w(ROUTED_RNG_TASKS_STOP, 1u);
        if (nonce != 0u) {
            *nonce_out = nonce;
            return 1;
        }
    }
    return 0;
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

#if TRON_BUILD_ROUTED_FULL_TAVRN
static uint16_t ratio_to_permille(float ratio)
{
    uint16_t permille;

    if (ratio <= 0.0f) {
        return 1u;
    }
    if (ratio >= 1.0f) {
        return 1000u;
    }
    permille = (uint16_t)(ratio * 1000.0f + 0.5f);
    return permille == 0u ? 1u : permille;
}
#endif

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

static int delivery_callback_begin(void)
{
    if (!queue_guard_begin()) {
        return 0;
    }
    if (delivery_state.callback_active != 0u) {
        queue_guard_end();
        return 0;
    }
    delivery_state.callback_active = 1u;
    return 1;
}

static void delivery_callback_end(void)
{
    delivery_state.callback_active = 0u;
    queue_guard_end();
}

static tavrn_router_delivery_status_t router_delivery_reserve(
    void *context, const tavrn_direct_peer_t *transmitter,
    const tavrn_link_data_t *data, tavrn_router_delivery_token_t *token_out,
    uint32_t delivered_at_ms)
{
    (void)context;
    (void)delivered_at_ms;
    if (token_out != NULL) {
        *token_out = TAVRN_ROUTER_DELIVERY_TOKEN_NONE;
    }
    if (transmitter == NULL || data == NULL || token_out == NULL ||
        !delivery_callback_begin()) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    if (delivery_state.provisional != 0u ||
        delivery_state.published.count >= ROUTED_DELIVERY_CAPACITY) {
        delivery_callback_end();
        return TAVRN_ROUTER_DELIVERY_BUSY;
    }
    next_delivery_token++;
    if (next_delivery_token == TAVRN_ROUTER_DELIVERY_TOKEN_NONE) {
        next_delivery_token++;
    }
    delivery_state.provisional = 1u;
    delivery_state.reserved_index = delivery_state.published.tail;
    delivery_state.transmitter = *transmitter;
    delivery_state.data = *data;
    delivery_state.token = next_delivery_token;
    *token_out = next_delivery_token;
    delivery_callback_end();
    return TAVRN_ROUTER_DELIVERY_OK;
}

static tavrn_router_delivery_status_t router_delivery_commit(
    void *context, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t delivered_at_ms)
{
    uint8_t index;

    (void)context;
    if (transmitter == NULL || data == NULL || !delivery_callback_begin()) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    if (delivery_state.provisional == 0u || token == TAVRN_ROUTER_DELIVERY_TOKEN_NONE ||
        token != delivery_state.token ||
        memcmp(transmitter, &delivery_state.transmitter, sizeof(*transmitter)) != 0 ||
        memcmp(data, &delivery_state.data, sizeof(*data)) != 0 ||
        delivery_state.reserved_index != delivery_state.published.tail ||
        !ring_push(&delivery_state.published, &index)) {
        delivery_callback_end();
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    memset(&deliveries[index], 0, sizeof(deliveries[index]));
    deliveries[index].transmitter = *transmitter;
    deliveries[index].data = *data;
    deliveries[index].delivered_at_ms = delivered_at_ms;
    memset(&delivery_state.transmitter, 0, sizeof(delivery_state.transmitter));
    memset(&delivery_state.data, 0, sizeof(delivery_state.data));
    delivery_state.token = TAVRN_ROUTER_DELIVERY_TOKEN_NONE;
    delivery_state.provisional = 0u;
    routed_counters.delivered++;
    delivery_callback_end();
    return TAVRN_ROUTER_DELIVERY_OK;
}

static tavrn_router_delivery_status_t router_delivery_cancel(
    void *context, tavrn_router_delivery_token_t token,
    const tavrn_direct_peer_t *transmitter, const tavrn_link_data_t *data,
    uint32_t now)
{
    (void)context;
    (void)now;
    if (transmitter == NULL || data == NULL || !delivery_callback_begin()) {
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    if (delivery_state.provisional == 0u || token == TAVRN_ROUTER_DELIVERY_TOKEN_NONE ||
        token != delivery_state.token ||
        memcmp(transmitter, &delivery_state.transmitter, sizeof(*transmitter)) != 0 ||
        memcmp(data, &delivery_state.data, sizeof(*data)) != 0) {
        delivery_callback_end();
        return TAVRN_ROUTER_DELIVERY_INVALID;
    }
    memset(&delivery_state.transmitter, 0, sizeof(delivery_state.transmitter));
    memset(&delivery_state.data, 0, sizeof(delivery_state.data));
    delivery_state.token = TAVRN_ROUTER_DELIVERY_TOKEN_NONE;
    delivery_state.provisional = 0u;
    delivery_callback_end();
    return TAVRN_ROUTER_DELIVERY_OK;
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

#if TRON_BUILD_BENCHMARK_MODE
static void routed_benchmark_increment_saturating(uint32_t *value)
{
    if (value != NULL && *value != UINT32_MAX) {
        (*value)++;
    }
}

static void routed_benchmark_record_guard_fault(void)
{
    routed_benchmark_increment_saturating(
        &routed_benchmark_health_faults.guard_faults);
}

static void routed_benchmark_record_attempt_queue_fault(void)
{
    routed_benchmark_increment_saturating(
        &routed_benchmark_health_faults.attempt_queue_faults);
}

static void routed_benchmark_record_snapshot_fault(void)
{
    routed_benchmark_increment_saturating(
        &routed_benchmark_health_faults.snapshot_faults);
}

/* The mesh task only copies a completed source-attempt record.  The logger
 * owns dequeueing and every print, so serial backpressure cannot alter route or
 * application admission. */
static void routed_benchmark_offer_attempt(
    const routed_benchmark_attempt_t *attempt)
{
    routed_benchmark_attempt_queue_status_t status;

    if (attempt == NULL) {
        routed_benchmark_record_attempt_queue_fault();
        return;
    }
    if (!queue_guard_begin()) {
        routed_benchmark_record_guard_fault();
        return;
    }
    status = routed_benchmark_attempt_queue_offer(&routed_benchmark_attempt_queue,
                                                  attempt);
    queue_guard_end();
    if (status == ROUTED_BENCHMARK_ATTEMPT_QUEUE_INVALID) {
        routed_benchmark_record_attempt_queue_fault();
    }
}

static int routed_benchmark_attempt_pop(void)
{
    routed_benchmark_attempt_queue_status_t status;

    if (!queue_guard_begin()) {
        routed_benchmark_record_guard_fault();
        return 0;
    }
    status = routed_benchmark_attempt_queue_take(&routed_benchmark_attempt_queue,
                                                  &routed_benchmark_logged_attempt);
    queue_guard_end();
    if (status == ROUTED_BENCHMARK_ATTEMPT_QUEUE_INVALID) {
        routed_benchmark_record_attempt_queue_fault();
    }
    return status == ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK;
}

static routed_benchmark_schedule_status_t routed_benchmark_schedule(
    uint32_t now, routed_benchmark_slot_t *slot_out)
{
    routed_benchmark_schedule_status_t status = ROUTED_BENCHMARK_SCHEDULE_INVALID;

    if (!queue_guard_begin()) {
        routed_benchmark_record_guard_fault();
        return status;
    }
    status = routed_benchmark_schedule_due(&routed_benchmark_state, now, slot_out);
    queue_guard_end();
    return status;
}

static void routed_benchmark_record_submission_status(uint8_t accepted)
{
    if (!queue_guard_begin()) {
        routed_benchmark_record_guard_fault();
        return;
    }
    routed_benchmark_record_submission(&routed_benchmark_state, accepted);
    queue_guard_end();
}

static void routed_benchmark_record_not_ready_status(void)
{
    if (!queue_guard_begin()) {
        routed_benchmark_record_guard_fault();
        return;
    }
    routed_benchmark_record_not_ready(&routed_benchmark_state);
    queue_guard_end();
}

static int routed_benchmark_snapshot(routed_benchmark_snapshot_t *out)
{
    routed_benchmark_attempt_queue_status_t status;

    if (out == NULL) {
        routed_benchmark_record_snapshot_fault();
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (!queue_guard_begin()) {
        routed_benchmark_record_guard_fault();
        return 0;
    }
    out->state = routed_benchmark_state;
    status = routed_benchmark_attempt_queue_snapshot(&routed_benchmark_attempt_queue,
                                                       &out->attempts);
    if (status != ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK) {
        routed_benchmark_record_attempt_queue_fault();
    }
    out->final_queue_count = delivery_state.published.count;
    out->diagnostic_dropped = routed_diagnostic_queue.dropped_count;
    out->rreq_dropped = routed_rreq_queue.dropped_count;
    out->retry_log_dropped = routed_retry_log.dropped_count;
    out->router_fault = tavrn_router_fault_reason(&routed_router);
    out->mesh_fault_latched = mesh_fault_latched;
    out->faults = routed_benchmark_health_faults;
    queue_guard_end();
    return status == ROUTED_BENCHMARK_ATTEMPT_QUEUE_OK;
}
#endif

static int local_event_push(const tavrn_router_dispatch_event_t *event)
{
    uint8_t index;
    int result = 0;

    if (event == NULL || event->type == TAVRN_ROUTER_DISPATCH_EVENT_NONE ||
        !queue_guard_begin()) {
        return 0;
    }
    if (ring_push(&local_event_ring, &index)) {
        local_events[index].event = *event;
        result = 1;
    }
    queue_guard_end();
    return result;
}

static int local_event_pop(routed_local_event_t *event)
{
    uint8_t index;
    int result = 0;

    if (event == NULL || !queue_guard_begin()) {
        return 0;
    }
    if (ring_pop(&local_event_ring, &index)) {
        *event = local_events[index];
        result = 1;
    }
    queue_guard_end();
    return result;
}

#if !TRON_BUILD_BENCHMARK_MODE
static void snapshot(routed_snapshot_t *out)
{
    const tavrn_link_counters_t *link_counters;
    const aodv_counters_t *aodv_counters;
    const tavrn_router_counters_t *router_counters;
    routed_cycle_retry_log_snapshot_t retry_log_snapshot;

    if (out == NULL || !queue_guard_begin()) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->routed = routed_counters;
    out->delivery_queue_count = delivery_state.published.count;
    out->delivery_provisional = delivery_state.provisional;
    out->mesh_fault_latched = mesh_fault_latched;
    out->diagnostic_queue_count = routed_diagnostic_queue.count;
    out->rreq_queue_count = routed_rreq_queue.count;
    out->diagnostic_dropped = routed_diagnostic_queue.dropped_count;
    out->diagnostic_evicted_healthy =
        routed_diagnostic_queue.evicted_healthy_count;
    out->diagnostic_dropped_fault = routed_diagnostic_queue.dropped_fault_count;
    out->rreq_dropped = routed_rreq_queue.dropped_count;
    if (routed_cycle_retry_log_snapshot(&routed_retry_log, &retry_log_snapshot) ==
        ROUTED_CYCLE_RETRY_LOG_OK) {
        out->retry_log_pending = retry_log_snapshot.pending;
        out->retry_log_dropped = retry_log_snapshot.dropped_count;
    }
    memcpy(out->phase_max_elapsed_ms, routed_phase_max_elapsed_ms,
           sizeof(out->phase_max_elapsed_ms));
    out->router_fault_reason = tavrn_router_fault_reason(&routed_router);
    link_counters = tavrn_link_v2_counters(&routed_link);
    aodv_counters = aodv_core_counters(&routed_aodv);
    router_counters = tavrn_router_counters(&routed_router);
    if (link_counters != NULL) {
        out->link = *link_counters;
    }
    if (aodv_counters != NULL) {
        out->aodv = *aodv_counters;
    }
    if (router_counters != NULL) {
        out->router = *router_counters;
    }
#if TRON_BUILD_ROUTED_FULL_TAVRN
    {
        const tavrn_mentorship_counters_t *mentorship_counters =
            tavrn_mentorship_counters(&routed_mentorship);
        const tavrn_maintenance_counters_t *maintenance_counters =
            tavrn_maintenance_counters(&routed_maintenance);

        (void)tavrn_mentorship_state_snapshot(&routed_mentorship,
                                               &out->mentorship);
        (void)tavrn_maintenance_snapshot(&routed_maintenance,
                                         &out->maintenance);
        if (mentorship_counters != NULL) {
            out->mentorship_counters = *mentorship_counters;
        }
        if (maintenance_counters != NULL) {
            out->maintenance_counters = *maintenance_counters;
        }
        (void)tavrn_maintenance_tc_metadata_telemetry(&routed_maintenance,
                                                       &out->tc_metadata);
        out->targeted_owner_status =
            routed_binding_result_storage.targeted_owner_status;
        out->targeted_owner_action =
            routed_binding_result_storage.targeted_owner_action;
        out->serving_mentee = routed_mentorship.serving_mentee;
        out->serving_boot_nonce = routed_mentorship.serving_boot_nonce;
        out->pending_offer_valid = routed_mentorship.pending_offer_valid;
        out->serving_session_valid = routed_mentorship.serving_session_valid;
        out->page_session_valid = routed_mentorship.page_session_valid;
    }
#endif
    queue_guard_end();
}
#endif

#if TRON_BUILD_ROUTED_FULL_TAVRN && !TRON_BUILD_BENCHMARK_MODE
static const UB *mentorship_state_name(tavrn_mentorship_state_t state)
{
    switch (state) {
    case TAVRN_MENTORSHIP_REJOINING:
        return (const UB *)"REJOINING";
    case TAVRN_MENTORSHIP_COLLECTING_OFFERS:
        return (const UB *)"COLLECTING_OFFERS";
    case TAVRN_MENTORSHIP_SYNCING:
        return (const UB *)"SYNCING";
    case TAVRN_MENTORSHIP_SID8_ACTIVE:
        return (const UB *)"SID8_ACTIVE";
    case TAVRN_MENTORSHIP_IDENTITY_CONFLICT:
        return (const UB *)"IDENTITY_CONFLICT";
    default:
        return (const UB *)"INVALID";
    }
}

static const UB *identity_width_name(tavrn_identity_width_t width)
{
    if (width == TAVRN_IDENTITY_SID16) {
        return (const UB *)"SID16";
    }
    if (width == TAVRN_IDENTITY_SID8) {
        return (const UB *)"SID8";
    }
    return (const UB *)"INVALID";
}
#endif

static void log_startup(void)
{
    const UB *feature = TRON_BUILD_ROUTED_FULL_TAVRN != 0 ?
        (UB *)"FULL_TAVRN_ESC_K1_MENTORSHIP" : (UB *)"AODV_ONLY";

    if (TRON_BUILD_LINK_PEER_ADVA_ENABLED != 0) {
        tm_printf((UB *)"routed startup feature=%s role_number=%u identify_display=%u sid16=0x%04x destination=0x%04x rx_block=%s\n",
                   feature, (UINT)TRON_BUILD_BENCH_ROLE_NUMBER,
                   (UINT)TRON_BUILD_BENCH_IDENTIFY_DISPLAY,
                   (UINT)local_peer.logical_id.value,
                   (UINT)configured_destination.logical_id.value,
                   (UB *)(TRON_BUILD_RX_BLOCK_ADVA_ENABLED != 0 ? "enabled" : "off"));
    } else {
        tm_printf((UB *)"routed startup feature=%s role_number=%u identify_display=%u sid16=0x%04x destination=not_configured rx_block=%s\n",
                   feature, (UINT)TRON_BUILD_BENCH_ROLE_NUMBER,
                   (UINT)TRON_BUILD_BENCH_IDENTIFY_DISPLAY,
                   (UINT)local_peer.logical_id.value,
                   (UB *)(TRON_BUILD_RX_BLOCK_ADVA_ENABLED != 0 ? "enabled" : "off"));
    }
}

#if !TRON_BUILD_BENCHMARK_MODE
static void log_summary(uint32_t now)
{
#if TRON_BUILD_ROUTED_FULL_TAVRN
    routed_snapshot_t *state = &routed_full_logger_storage.summary;

    if (!queue_guard_begin()) {
        return;
    }
    routed_full_logger_summary_active = 1u;
    queue_guard_end();
#else
    routed_snapshot_t *state = &routed_aodv_logger_summary;
#endif

    snapshot(state);
    tm_printf((UB *)"routed stats now=%lu submitted=%lu delivered=%lu pending_failed=%lu blacklisted=%lu local_fault=%lu router_fault=%lu scheduler_fault=%lu rx_blocked=%lu router_delivery=%u router_failures=%lu overflow=%lu aodv_backpressure=%lu delivery_q=%u provisional=%u mesh_fault=%u router_fault_reason=%u diag_q=%u diag_dropped=%lu diag_evicted_healthy=%lu diag_dropped_fault=%lu rreq_q=%u rreq_dropped=%lu max_pre_poll_ms=%lu max_scheduler_poll_ms=%lu max_scheduler_event_ms=%lu max_router_tick_ms=%lu max_application_submit_ms=%lu max_dispatch_ms=%lu max_link_service_ms=%lu max_healthy_yield_ms=%lu max_fault_idle_ms=%lu\n",
              (UW)now, (UW)state->routed.submitted, (UW)state->routed.delivered,
              (UW)state->routed.pending_failed, (UW)state->routed.blacklisted,
              (UW)state->routed.local_custody_fault, (UW)state->routed.router_fault,
              (UW)state->routed.scheduler_fault, (UW)state->routed.rx_blocked,
              (UINT)tavrn_router_delivery_state(&routed_router),
              (UW)(state->router.failure_invariant + state->router.failure_set_overflow),
              (UW)state->router.failure_set_overflow,
              (UW)state->aodv.action_backpressure,
              (UINT)state->delivery_queue_count, (UINT)state->delivery_provisional,
              (UINT)state->mesh_fault_latched, (UINT)state->router_fault_reason,
              (UINT)state->diagnostic_queue_count, (UW)state->diagnostic_dropped,
              (UW)state->diagnostic_evicted_healthy,
              (UW)state->diagnostic_dropped_fault, (UINT)state->rreq_queue_count,
              (UW)state->rreq_dropped,
              (UW)state->phase_max_elapsed_ms[ROUTED_CYCLE_PHASE_PRE_POLL_BOUND],
              (UW)state->phase_max_elapsed_ms[ROUTED_CYCLE_PHASE_SCHEDULER_POLL],
              (UW)state->phase_max_elapsed_ms[ROUTED_CYCLE_PHASE_SCHEDULER_EVENT],
              (UW)state->phase_max_elapsed_ms[ROUTED_CYCLE_PHASE_ROUTER_TICK],
              (UW)state->phase_max_elapsed_ms[ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT],
              (UW)state->phase_max_elapsed_ms[ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH],
               (UW)state->phase_max_elapsed_ms[ROUTED_CYCLE_PHASE_LINK_SERVICE],
                (UW)state->phase_max_elapsed_ms[ROUTED_CYCLE_PHASE_HEALTHY_YIELD],
                (UW)state->phase_max_elapsed_ms[ROUTED_CYCLE_PHASE_FAULT_IDLE]);
    tm_printf((UB *)"routed link_stats rx_candidate=%lu rx_accepted=%lu rx_committed_duplicate=%lu hack_accepted=%lu hack_duplicate=%lu hack_busy=%lu hack_rejected=%lu hack_unmatched=%lu hack_enqueue_failed=%lu tx_admitted=%lu tx_done=%lu tx_partial_done=%lu tx_failed=%lu retry_due=%lu retry_exhausted=%lu custody_promoted=%lu custody_dispatch_blocked=%lu custody_transferred=%lu local_not_attempted=%lu busy_expired=%lu custody_rejected=%lu radio_terminal=%lu service_terminal=%lu retry_log_pending=%u retry_log_dropped=%lu\n",
              (UW)state->link.rx_candidate,
              (UW)state->link.rx_candidate_accepted,
              (UW)state->link.rx_committed_duplicate,
              (UW)state->link.hack_accepted,
              (UW)state->link.hack_duplicate,
              (UW)state->link.hack_busy,
              (UW)state->link.hack_rejected,
              (UW)state->link.hack_unmatched,
              (UW)state->link.hack_enqueue_failed,
              (UW)state->link.tx_admitted,
              (UW)state->link.tx_done,
              (UW)state->link.tx_partial_done,
              (UW)state->link.tx_failed,
              (UW)state->link.retry_due,
              (UW)state->link.retry_exhausted,
              (UW)state->link.custody_promoted,
              (UW)state->link.custody_dispatch_blocked,
              (UW)state->link.custody_transferred,
              (UW)state->link.local_tx_not_attempted,
              (UW)state->link.custody_busy_expired,
              (UW)state->link.custody_rejected,
              (UW)state->link.radio_fault_terminal,
              (UW)state->link.service_fault_terminal,
              (UINT)state->retry_log_pending,
              (UW)state->retry_log_dropped);
#if TRON_BUILD_ROUTED_FULL_TAVRN
    tm_printf((UB *)"routed mentorship state=%s active_width=%s gated=%u offers_active=%u selected_present=%u selected=%02x:%02x:%02x:%02x:%02x:%02x selected_snapshot=%u frozen=%u sync_pages=%u pull_attempts=%u restarts=%u pending_offer=%u page_session=%u serving_session=%u serving_mentee=%02x:%02x:%02x:%02x:%02x:%02x serving_nonce=%u\n",
              mentorship_state_name(state->mentorship.state),
              identity_width_name(state->mentorship.active_width),
              (UINT)state->mentorship.ordinary_traffic_gated,
              (UINT)state->mentorship.active_offer_count,
              (UINT)state->mentorship.selected_mentor_present,
              (UINT)state->mentorship.selected_mentor.bytes[0],
              (UINT)state->mentorship.selected_mentor.bytes[1],
              (UINT)state->mentorship.selected_mentor.bytes[2],
              (UINT)state->mentorship.selected_mentor.bytes[3],
              (UINT)state->mentorship.selected_mentor.bytes[4],
              (UINT)state->mentorship.selected_mentor.bytes[5],
              (UINT)state->mentorship.selected_snapshot_id,
              (UINT)state->mentorship.frozen_snapshot_count,
              (UINT)state->mentorship.sync_pages_committed,
              (UINT)state->mentorship.pull_attempts,
              (UINT)state->mentorship.restart_count,
              (UINT)state->pending_offer_valid, (UINT)state->page_session_valid,
              (UINT)state->serving_session_valid,
              (UINT)state->serving_mentee.bytes[0],
              (UINT)state->serving_mentee.bytes[1],
              (UINT)state->serving_mentee.bytes[2],
              (UINT)state->serving_mentee.bytes[3],
              (UINT)state->serving_mentee.bytes[4],
              (UINT)state->serving_mentee.bytes[5],
              (UINT)state->serving_boot_nonce);
    tm_printf((UB *)"routed mentorship_counters offer_collected=%lu offer_capacity_full=%lu offer_not_retained=%lu offer_scheduled=%lu offer_suppressed=%lu offer_selected=%lu sid8_unknown_drop=%lu sid8_collision_drop=%lu sid8_reserved_drop=%lu bootstrap_sid8=%lu gated_data=%lu transitions=%lu join_originated=%lu join_received=%lu join_duplicate=%lu join_dedupe_expired=%lu join_dedupe_replaced=%lu join_obligation_overflow=%lu join_relayed=%lu sync_dedupe_capacity_full=%lu\n",
              (UW)state->mentorship_counters.offer_collected,
              (UW)state->mentorship_counters.offer_capacity_full,
              (UW)state->mentorship_counters.offer_not_retained,
              (UW)state->mentorship_counters.offer_scheduled,
              (UW)state->mentorship_counters.offer_suppressed,
              (UW)state->mentorship_counters.offer_selected,
              (UW)state->mentorship_counters.sid8_unknown_drop,
              (UW)state->mentorship_counters.sid8_collision_drop,
              (UW)state->mentorship_counters.sid8_reserved_drop,
              (UW)state->mentorship_counters.bootstrap_admitted_while_sid8,
              (UW)state->mentorship_counters.gated_data_aodv,
              (UW)state->mentorship_counters.transition_cleared,
              (UW)state->mentorship_counters.join_originated,
              (UW)state->mentorship_counters.join_received,
              (UW)state->mentorship_counters.join_duplicate,
              (UW)state->mentorship_counters.join_dedupe_expired,
              (UW)state->mentorship_counters.join_dedupe_replaced,
              (UW)state->mentorship_counters.join_obligation_overflow,
               (UW)state->mentorship_counters.join_relayed,
               (UW)state->mentorship_counters.sync_dedupe_capacity_full);
    tm_printf((UB *)"routed maintenance armed=%u pending=%u next_due_ms=%lu next_topology_sample_ms=%lu current_interval_ms=%lu liveness_floor_ms=%lu liveness_timeout_ms=%lu direct_one_hop_count=%u next_node_sequence=%u due=%lu enqueued=%lu busy=%lu rx_unique=%lu rx_duplicate=%lu rx_rejected=%lu dedupe_capacity=%lu interval_advanced=%lu interval_snapped=%lu local_broadcast_suppressed=%lu topology_reset=%lu topology_unchanged=%lu liveness_sample=%lu liveness_floor_decayed=%lu targeted_owner_status=%u targeted_action_enqueued=%u targeted_action_token=%u targeted_action_context=%u targeted_owner_ticks=%lu targeted_owner_enqueued=%lu targeted_rx=%lu targeted_scheduler_events=%lu targeted_invalid=%lu\n",
               (UINT)state->maintenance.armed, (UINT)state->maintenance.pending,
               (UW)state->maintenance.next_hello_due_ms,
               (UW)state->maintenance.next_topology_sample_ms,
               (UW)state->maintenance.current_interval_ms,
               (UW)state->maintenance.liveness_floor_ms,
               (UW)state->maintenance.liveness_timeout_ms,
               (UINT)state->maintenance.direct_one_hop_count,
               (UINT)state->maintenance.next_node_sequence,
              (UW)state->maintenance_counters.due,
              (UW)state->maintenance_counters.enqueued,
              (UW)state->maintenance_counters.busy,
               (UW)state->maintenance_counters.rx_unique,
               (UW)state->maintenance_counters.rx_duplicate,
               (UW)state->maintenance_counters.rx_rejected,
               (UW)state->maintenance_counters.dedupe_capacity,
               (UW)state->maintenance_counters.interval_advanced,
               (UW)state->maintenance_counters.interval_snapped,
               (UW)state->maintenance_counters.local_broadcast_suppressed,
               (UW)state->maintenance_counters.topology_reset,
                (UW)state->maintenance_counters.topology_unchanged,
                (UW)state->maintenance_counters.liveness_sample,
                (UW)state->maintenance_counters.liveness_floor_decayed,
                (UINT)state->targeted_owner_status,
                (UINT)state->targeted_owner_action.enqueued,
                (UINT)state->targeted_owner_action.token,
                (UINT)state->targeted_owner_action.context_index,
                (UW)state->maintenance_counters.targeted_owner_ticks,
                (UW)state->maintenance_counters.targeted_owner_enqueued,
                (UW)state->maintenance_counters.targeted_rx,
                 (UW)state->maintenance_counters.targeted_scheduler_events,
                 (UW)state->maintenance_counters.targeted_invalid);
    tm_printf((UB *)"routed tc_metadata origin_q=%u relay_q=%u origin_busy=%lu relay_busy=%lu current_seq=%u current_event=%u current_sid8=%u last_seq=%u last_event=%u last_sid8=%u apply=%lu duplicate=%lu relay=%lu admitted=%lu metadata_base=%u metadata_count=%u metadata_pdu_len=%u metadata_active=%u completion_commit=%lu completion_retry=%lu completion_failure=%lu\n",
              (UINT)state->tc_metadata.origin_queue_depth,
              (UINT)state->tc_metadata.relay_queue_depth,
              (UW)state->tc_metadata.tc_origin_busy_count,
              (UW)state->tc_metadata.tc_relay_busy_count,
              (UINT)state->tc_metadata.current_tc_sequence,
              (UINT)state->tc_metadata.current_tc_event,
              (UINT)state->tc_metadata.current_subject_sid8,
              (UINT)state->tc_metadata.last_tc_sequence,
              (UINT)state->tc_metadata.last_tc_event,
              (UINT)state->tc_metadata.last_subject_sid8,
              (UW)state->tc_metadata.tc_apply_count,
              (UW)state->tc_metadata.tc_duplicate_count,
              (UW)state->tc_metadata.tc_relay_count,
              (UW)state->tc_metadata.tc_admission_count,
              (UINT)state->tc_metadata.metadata_base_type,
              (UINT)state->tc_metadata.metadata_emitted_count,
              (UINT)state->tc_metadata.metadata_pdu_len,
              (UINT)state->tc_metadata.metadata_transaction_active,
              (UW)state->tc_metadata.metadata_completion_commit_count,
              (UW)state->tc_metadata.metadata_completion_retry_count,
              (UW)state->tc_metadata.metadata_completion_failure_count);
    if (queue_guard_begin()) {
        routed_full_logger_summary_active = 0u;
        queue_guard_end();
    }
#endif
}
#endif

#if TRON_BUILD_BENCHMARK_MODE
static int routed_benchmark_logger_record(uint32_t *session_out,
                                           uint64_t *record_id_out,
                                           uint32_t *started_at_ms_out)
{
    int result = 0;

    if (session_out == NULL || record_id_out == NULL) {
        routed_benchmark_record_snapshot_fault();
        return 0;
    }
    if (!queue_guard_begin()) {
        routed_benchmark_record_guard_fault();
        return 0;
    }
    *session_out = routed_benchmark_state.session_id;
    if (started_at_ms_out != NULL) {
        *started_at_ms_out = routed_benchmark_state.started_at_ms;
    }
    result = routed_benchmark_next_record_id(&routed_benchmark_state, record_id_out);
    queue_guard_end();
    return result;
}

static void log_benchmark_boot(void)
{
    uint32_t session;
    uint32_t started_at_ms;
    uint32_t emission_now;
    uint64_t record_id;

    if (routed_benchmark_logger_record(&session, &record_id, &started_at_ms)) {
        emission_now = now_ms();
        tm_printf((UB *)"obs_boot schema=observer-v2 now=%lu started_at_ms=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu feature=%s\n",
                    (UW)emission_now, (UW)started_at_ms,
                    (UINT)TRON_BUILD_BENCH_ROLE_NUMBER, (UW)session,
                    (UW)(record_id >> 32),
                    (UW)record_id,
                    (UB *)(TRON_BUILD_ROUTED_FULL_TAVRN != 0 ? "FULL_TAVRN" :
                                                          "AODV_ONLY"));
    }
}

static void log_benchmark_clock(void)
{
    uint32_t session;
    uint32_t emission_now;
    uint64_t record_id;

    if (routed_benchmark_logger_record(&session, &record_id, NULL)) {
        emission_now = now_ms();
        tm_printf((UB *)"obs_clock schema=observer-v2 now=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu\n",
                   (UW)emission_now, (UINT)TRON_BUILD_BENCH_ROLE_NUMBER,
                   (UW)session,
                   (UW)(record_id >> 32), (UW)record_id);
    }
}

static void log_benchmark_attempt(const routed_benchmark_attempt_t *attempt)
{
    uint32_t session;
    uint32_t emission_now;
    uint64_t record_id;

    if (attempt == NULL) {
        return;
    }
    if (!routed_benchmark_logger_record(&session, &record_id, NULL)) {
        return;
    }
    if (attempt->kind == ROUTED_BENCHMARK_RECORD_OFFER) {
        routed_benchmark_logged_offer_record_id = record_id;
        routed_benchmark_logged_offer_identity = attempt->identity;
        routed_benchmark_logged_offer_valid = 1u;
        emission_now = now_ms();
        tm_printf((UB *)"obs_offer schema=observer-v2 now=%lu event_at_ms=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu deadline_ms=%lu workload=%u burst=%lu sequence=%u payload_id=%u identity=0x%06lx offer=%lu attempted=%u accepted=0 destination=0x%04x width=%u status=%lu\n",
                   (UW)emission_now, (UW)attempt->event_at_ms,
                   (UINT)TRON_BUILD_BENCH_ROLE_NUMBER,
                   (UW)session, (UW)(record_id >> 32), (UW)record_id,
                  (UW)attempt->deadline_ms,
                   (UINT)attempt->workload, (UW)attempt->burst,
                   (UINT)attempt->sequence, (UINT)attempt->sequence,
                   (UW)attempt->identity, (UW)record_id,
                   (UINT)attempt->attempted,
                   (UINT)attempt->destination, (UINT)attempt->width,
                   (UW)attempt->status);
    } else if (routed_benchmark_logged_offer_valid != 0u &&
               routed_benchmark_logged_offer_identity == attempt->identity) {
        emission_now = now_ms();
        tm_printf((UB *)"obs_app schema=observer-v2 now=%lu event_at_ms=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu offer_record_id_hi=%lu offer_record_id_lo=%lu deadline_ms=%lu workload=%u burst=%lu sequence=%u payload_id=%u identity=0x%06lx attempted=%u accepted=%u status=%lu destination=0x%04x width=%u\n",
                   (UW)emission_now, (UW)attempt->event_at_ms,
                   (UINT)TRON_BUILD_BENCH_ROLE_NUMBER,
                   (UW)session, (UW)(record_id >> 32), (UW)record_id,
                  (UW)(routed_benchmark_logged_offer_record_id >> 32),
                  (UW)routed_benchmark_logged_offer_record_id,
                  (UW)attempt->deadline_ms,
                  (UINT)attempt->workload, (UW)attempt->burst,
                   (UINT)attempt->sequence, (UINT)attempt->sequence,
                   (UW)attempt->identity,
                   (UINT)attempt->attempted,
                   (UINT)attempt->accepted, (UW)attempt->status,
                  (UINT)attempt->destination, (UINT)attempt->width);
        routed_benchmark_logged_offer_valid = 0u;
    } else {
        emission_now = now_ms();
        tm_printf((UB *)"obs_app schema=observer-v2 now=%lu event_at_ms=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu offer_record_id_hi=0 offer_record_id_lo=0 deadline_ms=%lu workload=%u burst=%lu sequence=%u payload_id=%u identity=0x%06lx attempted=%u accepted=%u status=%lu destination=0x%04x width=%u\n",
                   (UW)emission_now, (UW)attempt->event_at_ms,
                   (UINT)TRON_BUILD_BENCH_ROLE_NUMBER,
                   (UW)session, (UW)(record_id >> 32), (UW)record_id,
                  (UW)attempt->deadline_ms, (UINT)attempt->workload,
                   (UW)attempt->burst, (UINT)attempt->sequence,
                   (UINT)attempt->sequence, (UW)attempt->identity,
                   (UINT)attempt->attempted,
                   (UINT)attempt->accepted, (UW)attempt->status,
                  (UINT)attempt->destination, (UINT)attempt->width);
    }
}

static void log_benchmark_control(void)
{
    routed_benchmark_logger_storage_t *storage = &routed_benchmark_logger_storage;
    const tavrn_link_counters_t *link_source;
    const aodv_counters_t *aodv_source;
    const tavrn_router_counters_t *router_source;
    uint32_t session;
    uint32_t emission_now;
    uint64_t record_id;

    memset(&storage->observer, 0, sizeof(storage->observer));
    memset(&storage->link, 0, sizeof(storage->link));
    memset(&storage->aodv, 0, sizeof(storage->aodv));
    memset(&storage->router, 0, sizeof(storage->router));
    if (!queue_guard_begin()) {
        routed_benchmark_record_guard_fault();
        return;
    }
    storage->observer = routed_benchmark_observer;
    link_source = tavrn_link_v2_counters(&routed_link);
    aodv_source = aodv_core_counters(&routed_aodv);
    router_source = tavrn_router_counters(&routed_router);
    if (link_source != NULL) {
        storage->link = *link_source;
    }
    if (aodv_source != NULL) {
        storage->aodv = *aodv_source;
    }
    if (router_source != NULL) {
        storage->router = *router_source;
    }
    queue_guard_end();
    if (!routed_benchmark_logger_record(&session, &record_id, NULL)) {
        return;
    }
    emission_now = now_ms();
    tm_printf((UB *)"obs_control schema=observer-v2 now=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu control_tx_scope=AODV_DISPATCH_PROXY_ONLY rx_control_proxy_rreq=%lu rx_control_proxy_rrep=%lu rx_control_proxy_rerr=%lu rx_control_proxy_hello=%lu rx_control_proxy_sync_offer=%lu rx_control_proxy_sync_pull=%lu rx_control_proxy_sync_data=%lu rx_control_proxy_tc_update=%lu rx_control_proxy_rrep_ack=%lu rx_control_proxy_bytes_rreq=%lu rx_control_proxy_bytes_rrep=%lu rx_control_proxy_bytes_rerr=%lu rx_control_proxy_bytes_hello=%lu rx_control_proxy_bytes_sync_offer=%lu rx_control_proxy_bytes_sync_pull=%lu rx_control_proxy_bytes_sync_data=%lu rx_control_proxy_bytes_tc_update=%lu rx_control_proxy_bytes_rrep_ack=%lu aodv_dispatch_enqueue_proxy_rreq=%lu aodv_dispatch_enqueue_proxy_rrep=%lu aodv_dispatch_enqueue_proxy_rerr=%lu aodv_dispatch_enqueue_proxy_hello=%lu aodv_dispatch_enqueue_proxy_sync_offer=%lu aodv_dispatch_enqueue_proxy_sync_pull=%lu aodv_dispatch_enqueue_proxy_sync_data=%lu aodv_dispatch_enqueue_proxy_tc_update=%lu aodv_dispatch_enqueue_proxy_rrep_ack=%lu aodv_dispatch_enqueue_proxy_bytes_rreq=%lu aodv_dispatch_enqueue_proxy_bytes_rrep=%lu aodv_dispatch_enqueue_proxy_bytes_rerr=%lu aodv_dispatch_enqueue_proxy_bytes_hello=%lu aodv_dispatch_enqueue_proxy_bytes_sync_offer=%lu aodv_dispatch_enqueue_proxy_bytes_sync_pull=%lu aodv_dispatch_enqueue_proxy_bytes_sync_data=%lu aodv_dispatch_enqueue_proxy_bytes_tc_update=%lu aodv_dispatch_enqueue_proxy_bytes_rrep_ack=%lu scheduler_rx_adv_proxy=%lu scheduler_tx_done_proxy=%lu scheduler_tx_failed_proxy=%lu scheduler_fault_proxy=%lu scheduler_rx_ch37_proxy=%lu scheduler_rx_ch38_proxy=%lu scheduler_rx_ch39_proxy=%lu link_tx_admitted_proxy=%lu link_tx_done_proxy=%lu link_tx_failed_proxy=%lu retry_due=%lu retry_exhausted=%lu aodv_rreq_duplicate=%lu aodv_rreq_rate_limited=%lu aodv_action_backpressure=%lu router_failure_invariant=%lu\n",
               (UW)emission_now, (UINT)TRON_BUILD_BENCH_ROLE_NUMBER,
               (UW)session,
               (UW)(record_id >> 32), (UW)record_id,
                (UW)storage->observer.rx_control_proxy_count[TAVRN_WIRE_E_RREQ],
                (UW)storage->observer.rx_control_proxy_count[TAVRN_WIRE_E_RREP],
                (UW)storage->observer.rx_control_proxy_count[TAVRN_WIRE_E_RERR],
                (UW)storage->observer.rx_control_proxy_count[TAVRN_WIRE_HELLO],
                (UW)storage->observer.rx_control_proxy_count[TAVRN_WIRE_SYNC_OFFER],
                (UW)storage->observer.rx_control_proxy_count[TAVRN_WIRE_SYNC_PULL],
                (UW)storage->observer.rx_control_proxy_count[TAVRN_WIRE_SYNC_DATA],
                (UW)storage->observer.rx_control_proxy_count[TAVRN_WIRE_TC_UPDATE],
                (UW)storage->observer.rx_control_proxy_count[TAVRN_WIRE_E_RREP_ACK],
                (UW)storage->observer.rx_control_proxy_bytes[TAVRN_WIRE_E_RREQ],
                (UW)storage->observer.rx_control_proxy_bytes[TAVRN_WIRE_E_RREP],
                (UW)storage->observer.rx_control_proxy_bytes[TAVRN_WIRE_E_RERR],
                (UW)storage->observer.rx_control_proxy_bytes[TAVRN_WIRE_HELLO],
                (UW)storage->observer.rx_control_proxy_bytes[TAVRN_WIRE_SYNC_OFFER],
                (UW)storage->observer.rx_control_proxy_bytes[TAVRN_WIRE_SYNC_PULL],
                (UW)storage->observer.rx_control_proxy_bytes[TAVRN_WIRE_SYNC_DATA],
                (UW)storage->observer.rx_control_proxy_bytes[TAVRN_WIRE_TC_UPDATE],
                (UW)storage->observer.rx_control_proxy_bytes[TAVRN_WIRE_E_RREP_ACK],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_count[
                   TAVRN_WIRE_E_RREQ],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_count[
                   TAVRN_WIRE_E_RREP],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_count[
                   TAVRN_WIRE_E_RERR],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_count[
                   TAVRN_WIRE_HELLO],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_count[
                   TAVRN_WIRE_SYNC_OFFER],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_count[
                   TAVRN_WIRE_SYNC_PULL],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_count[
                   TAVRN_WIRE_SYNC_DATA],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_count[
                   TAVRN_WIRE_TC_UPDATE],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_count[
                   TAVRN_WIRE_E_RREP_ACK],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_bytes[
                   TAVRN_WIRE_E_RREQ],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_bytes[
                   TAVRN_WIRE_E_RREP],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_bytes[
                   TAVRN_WIRE_E_RERR],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_bytes[
                   TAVRN_WIRE_HELLO],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_bytes[
                   TAVRN_WIRE_SYNC_OFFER],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_bytes[
                   TAVRN_WIRE_SYNC_PULL],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_bytes[
                   TAVRN_WIRE_SYNC_DATA],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_bytes[
                   TAVRN_WIRE_TC_UPDATE],
                (UW)storage->observer.aodv_dispatch_control_enqueue_proxy_bytes[
                    TAVRN_WIRE_E_RREP_ACK],
                (UW)storage->observer.scheduler_rx_adv_proxy,
                (UW)storage->observer.scheduler_tx_done_proxy,
                (UW)storage->observer.scheduler_tx_failed_proxy,
                (UW)storage->observer.scheduler_fault_proxy,
                (UW)storage->observer.scheduler_rx_channel_proxy[0],
                (UW)storage->observer.scheduler_rx_channel_proxy[1],
                (UW)storage->observer.scheduler_rx_channel_proxy[2],
                (UW)storage->link.tx_admitted,
               (UW)storage->link.tx_done, (UW)storage->link.tx_failed,
               (UW)storage->link.retry_due, (UW)storage->link.retry_exhausted,
               (UW)storage->aodv.rreq_duplicate,
               (UW)storage->aodv.rreq_rate_limited,
               (UW)storage->aodv.action_backpressure,
               (UW)storage->router.failure_invariant);
}

static void log_benchmark_health(void)
{
    routed_benchmark_logger_storage_t *storage = &routed_benchmark_logger_storage;
    uint32_t session;
    uint32_t emission_now;
    uint64_t record_id;

    (void)routed_benchmark_snapshot(&storage->health);
    if (!queue_guard_begin()) {
        routed_benchmark_record_guard_fault();
        return;
    }
    storage->observer = routed_benchmark_observer;
    queue_guard_end();
    if (!routed_benchmark_logger_record(&session, &record_id, NULL)) {
        return;
    }
    emission_now = now_ms();
    tm_printf((UB *)"obs_health schema=observer-v2 now=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu record_seq=%lu offered=%lu accepted=%lu rejected=%lu not_ready=%lu skipped=%lu heartbeat_skipped=%lu throughput_skipped=%lu event_q=%u event_q_high_water=%u event_q_dropped=%lu attempt_queue_faults=%lu guard_faults=%lu snapshot_faults=%lu final_q=%u diagnostic_dropped=%lu rreq_dropped=%lu retry_log_dropped=%lu trace_over_budget=%lu trace_fault_latched=%lu mesh_fault=%u router_fault=%u\n",
               (UW)emission_now, (UINT)TRON_BUILD_BENCH_ROLE_NUMBER,
               (UW)session,
              (UW)(record_id >> 32), (UW)record_id, (UW)record_id,
               (UW)storage->health.state.offered,
               (UW)storage->health.state.accepted,
               (UW)storage->health.state.rejected,
               (UW)storage->health.state.not_ready,
               (UW)storage->health.state.skipped,
               (UW)storage->health.state.heartbeat_skipped,
                (UW)storage->health.state.throughput_skipped,
                (UINT)storage->health.attempts.count,
                (UINT)storage->health.attempts.high_water,
                (UW)storage->health.attempts.dropped_count,
                (UW)storage->health.faults.attempt_queue_faults,
                (UW)storage->health.faults.guard_faults,
                (UW)storage->health.faults.snapshot_faults,
                (UINT)storage->health.final_queue_count,
               (UW)storage->health.diagnostic_dropped,
               (UW)storage->health.rreq_dropped,
               (UW)storage->health.retry_log_dropped,
               (UW)storage->observer.trace_over_budget,
               (UW)storage->observer.trace_fault_latched,
               (UINT)storage->health.mesh_fault_latched,
               (UINT)storage->health.router_fault);
}

#if TRON_BUILD_ROUTED_FULL_TAVRN
static void log_benchmark_gtt_next(void)
{
    const routed_cycle_gtt_snapshot_entry_t *entry;
    uint32_t session;
    uint32_t emission_now;
    uint64_t record_id;

    if (routed_benchmark_gtt_emission.active == 0u) {
        return;
    }
    if (routed_benchmark_gtt_emission.entry_cursor == 0u) {
        if (!routed_benchmark_logger_record(&session, &record_id, NULL)) {
            return;
        }
        routed_benchmark_gtt_emission.entry_cursor = 1u;
        emission_now = now_ms();
        tm_printf((UB *)"obs_gtt_begin schema=observer-v2 now=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu query_at_ms=%lu slot_capacity=%u retained_entry_count=%u\n",
                    (UW)emission_now,
                   (UINT)TRON_BUILD_BENCH_ROLE_NUMBER, (UW)session,
                   (UW)(record_id >> 32), (UW)record_id,
                   (UW)routed_benchmark_gtt_emission.query_at_ms,
                   (UINT)TAVRN_GTT_CAPACITY,
                   (UINT)routed_benchmark_gtt_snapshot.entry_count);
        return;
    }
    if (routed_benchmark_gtt_emission.invalid != 0u) {
        if (!routed_benchmark_logger_record(&session, &record_id, NULL)) {
            return;
        }
        routed_benchmark_gtt_emission.active = 0u;
        emission_now = now_ms();
        tm_printf((UB *)"obs_gtt_end schema=observer-v2 now=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu query_at_ms=%lu status=INVALID entry_count=0 nondeparted_count=0\n",
                   (UW)emission_now, (UINT)TRON_BUILD_BENCH_ROLE_NUMBER,
                  (UW)session, (UW)(record_id >> 32), (UW)record_id,
                  (UW)routed_benchmark_gtt_emission.query_at_ms);
        return;
    }
    if (routed_benchmark_gtt_emission.entry_cursor <=
        routed_benchmark_gtt_snapshot.entry_count) {
        uint8_t entry_index = (uint8_t)(
            routed_benchmark_gtt_emission.entry_cursor - 1u);

        entry = &routed_benchmark_gtt_snapshot.entries[entry_index];
        if (!routed_benchmark_logger_record(&session, &record_id, NULL)) {
            return;
        }
        routed_benchmark_gtt_emission.entry_cursor++;
        emission_now = now_ms();
        tm_printf((UB *)"obs_gtt_entry schema=observer-v2 now=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu query_at_ms=%lu slot=%u adva=%02x:%02x:%02x:%02x:%02x:%02x serial=%u serial_state=%u hop_count=%u hop_state=%u freshness=%u departed=%u last_evidence_ms=%lu\n",
                   (UW)emission_now, (UINT)TRON_BUILD_BENCH_ROLE_NUMBER,
                  (UW)session, (UW)(record_id >> 32), (UW)record_id,
                  (UW)routed_benchmark_gtt_snapshot.query_at_ms,
                  (UINT)entry_index,
                  (UINT)entry->canonical_adva.bytes[0],
                  (UINT)entry->canonical_adva.bytes[1],
                  (UINT)entry->canonical_adva.bytes[2],
                  (UINT)entry->canonical_adva.bytes[3],
                  (UINT)entry->canonical_adva.bytes[4],
                  (UINT)entry->canonical_adva.bytes[5], (UINT)entry->serial,
                  (UINT)entry->serial_state, (UINT)entry->hop_count,
                  (UINT)entry->hop_state, (UINT)entry->freshness,
                  (UINT)entry->departed, (UW)entry->last_evidence_ms);
        return;
    }
    if (!routed_benchmark_logger_record(&session, &record_id, NULL)) {
        return;
    }
    routed_benchmark_gtt_emission.active = 0u;
    emission_now = now_ms();
    tm_printf((UB *)"obs_gtt_end schema=observer-v2 now=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu query_at_ms=%lu status=OK entry_count=%u nondeparted_count=%u\n",
                (UW)emission_now, (UINT)TRON_BUILD_BENCH_ROLE_NUMBER,
                (UW)session,
               (UW)(record_id >> 32), (UW)record_id,
               (UW)routed_benchmark_gtt_snapshot.query_at_ms,
               (UINT)routed_benchmark_gtt_snapshot.entry_count,
               (UINT)routed_benchmark_gtt_snapshot.nondeparted_count);
}

static void start_benchmark_gtt_emission(void)
{
    routed_full_telemetry_status_t status = ROUTED_FULL_TELEMETRY_INVALID;
    uint32_t snapshot_now;

    if (routed_benchmark_gtt_emission.active != 0u) {
        return;
    }
    memset(&routed_benchmark_gtt_emission, 0,
           sizeof(routed_benchmark_gtt_emission));
    snapshot_now = now_ms();
    routed_benchmark_gtt_emission.query_at_ms = snapshot_now;
    if (!queue_guard_begin()) {
        routed_benchmark_record_guard_fault();
        routed_benchmark_record_snapshot_fault();
        routed_benchmark_gtt_emission.invalid = 1u;
        routed_benchmark_gtt_emission.active = 1u;
        return;
    }
    status = routed_full_telemetry_snapshot_gtt(
        &routed_gtt, snapshot_now, &routed_benchmark_gtt_snapshot);
    queue_guard_end();
    routed_benchmark_gtt_emission.active = 1u;
    if (status != ROUTED_FULL_TELEMETRY_OK) {
        routed_benchmark_record_snapshot_fault();
        routed_benchmark_gtt_emission.invalid = 1u;
    }
}
#else
static void log_benchmark_gtt_not_implemented(uint32_t now)
{
    uint32_t session;
    uint32_t emission_now;
    uint64_t record_id;

    if (routed_benchmark_logger_record(&session, &record_id, NULL)) {
        emission_now = now_ms();
        tm_printf((UB *)"obs_gtt_begin schema=observer-v2 now=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu query_at_ms=%lu slot_capacity=0 status=NOT_IMPLEMENTED\n",
                   (UW)emission_now, (UINT)TRON_BUILD_BENCH_ROLE_NUMBER,
                   (UW)session,
                   (UW)(record_id >> 32), (UW)record_id, (UW)now);
    }
    if (routed_benchmark_logger_record(&session, &record_id, NULL)) {
        emission_now = now_ms();
        tm_printf((UB *)"obs_gtt_end schema=observer-v2 now=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu query_at_ms=%lu status=NOT_IMPLEMENTED entry_count=0 nondeparted_count=0\n",
                   (UW)emission_now, (UINT)TRON_BUILD_BENCH_ROLE_NUMBER,
                   (UW)session,
                  (UW)(record_id >> 32), (UW)record_id, (UW)now);
    }
}
#endif
#endif

static void record_router_fault(tavrn_router_fault_reason_t reason)
{
    if (reason == TAVRN_ROUTER_FAULT_NONE) {
        return;
    }
    if (pending_router_fault != reason && logged_router_fault != reason) {
        pending_router_fault = reason;
        routed_counters.router_fault++;
    }
}

static void record_router_status(tavrn_router_event_status_t status)
{
    tavrn_router_fault_reason_t reason;

    if (status != TAVRN_ROUTER_EVENT_INVALID) {
        return;
    }
    reason = tavrn_router_fault_reason(&routed_router);
    if (reason == TAVRN_ROUTER_FAULT_NONE) {
        routed_counters.local_custody_fault++;
        return;
    }
    record_router_fault(reason);
}

static void record_router_terminal(const tavrn_router_phase_trace_t *trace)
{
    if (trace != NULL &&
        trace->terminal_fault_present == TAVRN_ROUTER_TRACE_PRESENT) {
        record_router_fault(trace->terminal_fault);
    }
}

static void handle_dispatch_event(const tavrn_router_dispatch_event_t *event)
{
    if (event == NULL) {
        return;
    }
    if (event->type == TAVRN_ROUTER_DISPATCH_EVENT_NONE) {
        return;
    } else if (event->type == TAVRN_ROUTER_DISPATCH_EVENT_PENDING_DATA_FAILED) {
        routed_counters.pending_failed++;
    } else if (event->type == TAVRN_ROUTER_DISPATCH_EVENT_BLACKLIST_NEIGHBOR) {
        routed_counters.blacklisted++;
    } else {
        routed_counters.local_custody_fault++;
        mesh_fault_latched = 1u;
        return;
    }
    if (!local_event_push(event)) {
        routed_counters.local_custody_fault++;
        mesh_fault_latched = 1u;
    }
}

static int trace_has_router_terminal(const routed_cycle_trace_t *trace)
{
    const tavrn_router_phase_trace_t *router_trace = NULL;

    if (trace == NULL) {
        return 0;
    }
    if (trace->phase == ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT) {
        if (trace->detail.application_submit.submit_present !=
            TAVRN_ROUTER_TRACE_PRESENT) {
            return 0;
        }
        router_trace = &trace->detail.application_submit.submit;
    } else if (trace->phase == ROUTED_CYCLE_PHASE_SCHEDULER_EVENT ||
               trace->phase == ROUTED_CYCLE_PHASE_ROUTER_TICK ||
               trace->phase == ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH ||
               trace->phase == ROUTED_CYCLE_PHASE_LINK_SERVICE) {
        router_trace = &trace->detail.router;
    }
    return router_trace != NULL &&
        router_trace->terminal_fault_present == TAVRN_ROUTER_TRACE_PRESENT;
}

static int trace_is_scheduler_fault(const routed_cycle_trace_t *trace)
{
    if (trace == NULL || trace->phase != ROUTED_CYCLE_PHASE_SCHEDULER_POLL) {
        return 0;
    }
    return trace->detail.scheduler_poll.event.fault != BLE_MESH_SCHED_FAULT_NONE ||
           trace->detail.scheduler_poll.event.type ==
               BLE_MESH_SCHED_EVENT_RADIO_FAULT ||
           trace->detail.scheduler_poll.event.type ==
               BLE_MESH_SCHED_EVENT_SERVICE_FAULT;
}

static int trace_needs_diagnostic(const routed_cycle_trace_t *trace)
{
    return trace != NULL &&
        (trace->fault_latched == ROUTED_CYCLE_BOOLEAN_TRUE ||
          trace->timing.over_budget == ROUTED_CYCLE_BOOLEAN_TRUE ||
          trace_is_scheduler_fault(trace) || trace_has_router_terminal(trace));
}

static int trace_retry_exhausted_tick(const routed_cycle_trace_t *trace)
{
    return trace != NULL && trace->phase == ROUTED_CYCLE_PHASE_ROUTER_TICK &&
        trace->detail.router.detail.tick.link_step_present ==
            TAVRN_ROUTER_TRACE_PRESENT &&
        trace->detail.router.detail.tick.link_step_status == TAVRN_LINK_STEP_EVENT &&
        trace->detail.router.detail.tick.link_event_present ==
            TAVRN_ROUTER_TRACE_PRESENT &&
        trace->detail.router.detail.tick.link_event.type ==
            TAVRN_LINK_EVENT_RETRY_EXHAUSTED;
}

static void routed_cycle_trace_sink(void *context, const routed_cycle_trace_t *trace)
{
    uint32_t *maximum;
    int retry_tick;
    int diagnostic;

    (void)context;
    if (trace == NULL) {
        return;
    }
#if TRON_BUILD_BENCHMARK_MODE
    routed_benchmark_observer_observe_trace(&routed_benchmark_observer, trace);
#endif
#if TRON_BUILD_ROUTED_FULL_TAVRN
    routed_max_scheduler_gap_ms = routed_full_telemetry_max_scheduler_gap(
        routed_max_scheduler_gap_ms,
        trace->timing.elapsed_since_scheduler_return_ms);
#endif
    if (trace->fault_latched == ROUTED_CYCLE_BOOLEAN_FALSE &&
        trace->phase > ROUTED_CYCLE_PHASE_NOT_APPLICABLE &&
        trace->phase < ROUTED_CYCLE_PHASE_UNKNOWN) {
        maximum = &routed_phase_max_elapsed_ms[trace->phase];
        if (trace->timing.phase_elapsed_ms > *maximum) {
            *maximum = trace->timing.phase_elapsed_ms;
        }
    }
    retry_tick = trace_retry_exhausted_tick(trace);
    diagnostic = trace_needs_diagnostic(trace);
    if ((retry_tick != 0 || diagnostic != 0) && queue_guard_begin()) {
        if (retry_tick != 0) {
            (void)routed_cycle_retry_log_offer(
                &routed_retry_log, &trace->detail.router.detail.tick.link_event);
        }
        if (diagnostic != 0) {
            (void)routed_cycle_diagnostic_enqueue(&routed_diagnostic_queue, trace);
        }
        queue_guard_end();
    }
}

static void routed_rreq_lifecycle(void *context,
                                  const aodv_rreq_lifecycle_record_t *record)
{
    (void)context;
    if (record != NULL && queue_guard_begin()) {
        (void)routed_cycle_rreq_enqueue(&routed_rreq_queue, record);
        queue_guard_end();
    }
}

static int routed_diagnostic_pop(void)
{
    routed_cycle_result_t status;

    if (!queue_guard_begin()) {
        return 0;
    }
    status = routed_cycle_diagnostic_dequeue(&routed_diagnostic_queue,
                                              &routed_logger_record.diagnostic);
    if (status == ROUTED_CYCLE_RESULT_OK) {
        routed_logger_progress_epoch++;
    }
    queue_guard_end();
    return status == ROUTED_CYCLE_RESULT_OK;
}

static int routed_retry_log_pop(void)
{
    routed_cycle_retry_log_status_t status;

    if (!queue_guard_begin()) {
        return 0;
    }
    status = routed_cycle_retry_log_take(&routed_retry_log,
                                         &routed_logger_record.retry_event);
    queue_guard_end();
    return status == ROUTED_CYCLE_RETRY_LOG_OK;
}

static int routed_diagnostic_pending(uint32_t *progress_epoch_out)
{
    int pending;

    if (progress_epoch_out == NULL || !queue_guard_begin()) {
        return -1;
    }
    pending = routed_diagnostic_queue.count != 0u;
    *progress_epoch_out = routed_logger_progress_epoch;
    queue_guard_end();
    return pending;
}

static int routed_rreq_pop(void)
{
    routed_cycle_result_t status;

    if (!queue_guard_begin()) {
        return 0;
    }
    status = routed_cycle_rreq_dequeue(&routed_rreq_queue, &routed_logged_rreq);
    queue_guard_end();
    return status == ROUTED_CYCLE_RESULT_OK;
}

static void latch_scheduler_fault(uint32_t now)
{
    ble_mesh_sched_event_t fault;

    if (mesh_fault_latched != 0u) {
        return;
    }
    memset(&fault, 0, sizeof(fault));
    fault.type = BLE_MESH_SCHED_EVENT_SERVICE_FAULT;
    fault.fault = BLE_MESH_SCHED_FAULT_POLL_OVERRUN;
    mesh_fault_latched = 1u;
    routed_counters.scheduler_fault++;
    (void)tavrn_router_handle_scheduler_event_ex(&routed_router, &fault, now,
                                                   &routed_scheduler_fault_trace);
#if TRON_BUILD_ROUTED_FULL_TAVRN
    if (tavrn_maintenance_high_token_scheduler_event(
            &routed_maintenance, &routed_router, &routed_link, &fault, now,
            &routed_high_token_dispatch) == TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID) {
        routed_scheduler_fault_trace.detail.scheduler_event.status =
            TAVRN_ROUTER_EVENT_INVALID;
    }
#endif
    routed_scheduler_fault_trace.completed_at_ms = now_ms();
    record_router_status(routed_scheduler_fault_trace.detail.scheduler_event.status);
    record_router_terminal(&routed_scheduler_fault_trace);
}

#if TRON_BUILD_ROUTED_FULL_TAVRN
static tavrn_router_control_intercept_status_t routed_verification_rrep_receive(
    void *context, const tavrn_rx_control_event_t *control_event, uint32_t now)
{
    tavrn_maintenance_t *maintenance = context;
    tavrn_rreq_verification_status_t status;

    if (maintenance == NULL || control_event == NULL) {
        return TAVRN_ROUTER_CONTROL_INTERCEPT_INVALID;
    }
    if (control_event->control.type != TAVRN_WIRE_E_RREP) {
        return TAVRN_ROUTER_CONTROL_INTERCEPT_IGNORED;
    }
#if TRON_BUILD_LOCAL_REPAIR
    {
        tavrn_router_control_intercept_status_t repair_status =
            tavrn_full_repair_binding_receive_rrep(&routed_repair_binding,
                                                   control_event, now);

        if (repair_status != TAVRN_ROUTER_CONTROL_INTERCEPT_IGNORED) {
            return repair_status;
        }
    }
#endif
    status = tavrn_maintenance_verification_receive_rrep(
        maintenance, &routed_router, &routed_link, &routed_gtt, control_event, now);
    if (status == TAVRN_RREQ_VERIFICATION_CANCELED ||
        status == TAVRN_RREQ_VERIFICATION_OK) {
        return TAVRN_ROUTER_CONTROL_INTERCEPT_CONSUMED;
    }
    if (status == TAVRN_RREQ_VERIFICATION_BUSY) {
        return TAVRN_ROUTER_CONTROL_INTERCEPT_BUSY;
    }
    if (status == TAVRN_RREQ_VERIFICATION_INVALID) {
        return TAVRN_ROUTER_CONTROL_INTERCEPT_INVALID;
    }
    return TAVRN_ROUTER_CONTROL_INTERCEPT_IGNORED;
}

#if !TRON_BUILD_BENCHMARK_MODE
static int routed_expiry_sweep_enqueue(
    uint32_t now, const tavrn_maintenance_expiry_sweep_snapshot_t *sweep)
{
    routed_expiry_sweep_record_t *record;

    if (sweep == NULL || sweep->pass_performed == 0u || !queue_guard_begin()) {
        return 0;
    }
    if (routed_expiry_sweep_queue.count == ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY) {
        queue_guard_end();
        return 0;
    }
    routed_expiry_sweep_passes++;
    if (routed_expiry_sweep_passes == 0u) {
        routed_expiry_sweep_passes = 1u;
    }
    if (sweep->trace_count == TAVRN_GTT_CAPACITY) {
        routed_expiry_complete_passes++;
        if (routed_expiry_complete_passes == 0u) {
            routed_expiry_complete_passes = 1u;
        }
    }
    record = &routed_expiry_sweep_queue.records[routed_expiry_sweep_queue.tail];
    memset(record, 0, sizeof(*record));
    record->now_ms = now;
    record->received_evidence_count = sweep->received_evidence_count;
    record->pass = routed_expiry_sweep_passes;
    record->complete_passes = routed_expiry_complete_passes;
    record->max_scheduler_gap_ms = routed_max_scheduler_gap_ms;
    record->scheduler_fault_count = routed_counters.scheduler_fault;
    record->router_fault = tavrn_router_fault_reason(&routed_router);
    record->trace_count = sweep->trace_count;
    record->cursor = sweep->next_cursor;
    record->soft_selected_count = sweep->soft_selected_count;
    record->hard_selected_count = sweep->hard_selected_count;
    record->demand_deferred_count = sweep->demand_deferred_count;
    record->unavailable_count = sweep->unavailable_count;
    record->departed_count = sweep->departed_count;
    record->purged_count = sweep->purged_count;
    record->maximum_copied_candidates = sweep->maximum_copied_candidates;
    record->targeted_control_count = sweep->targeted_control_count;
    record->rreq_control_count = sweep->rreq_control_count;
    record->expiry_tc_control_count = sweep->expiry_tc_control_count;
    record->mesh_fault_latched = mesh_fault_latched;
    routed_expiry_sweep_queue.tail = (uint8_t)(
        (routed_expiry_sweep_queue.tail + 1u) % ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY);
    routed_expiry_sweep_queue.count++;
    queue_guard_end();
    return 1;
}

static int routed_expiry_sweep_dequeue(routed_expiry_sweep_record_t *record_out)
{
    if (record_out == NULL || !queue_guard_begin()) {
        return 0;
    }
    if (routed_expiry_sweep_queue.count == 0u) {
        queue_guard_end();
        return 0;
    }
    *record_out = routed_expiry_sweep_queue.records[routed_expiry_sweep_queue.head];
    routed_expiry_sweep_queue.head = (uint8_t)(
        (routed_expiry_sweep_queue.head + 1u) % ROUTED_EXPIRY_SWEEP_TELEMETRY_CAPACITY);
    routed_expiry_sweep_queue.count--;
    queue_guard_end();
    return 1;
}
#endif

#if TRON_BUILD_TEST_EXPIRY_FULL_TABLE
static int routed_expiry_table_has_identity(const tavrn_adva_t *identity)
{
    uint8_t index;

    if (identity == NULL) {
        return 0;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &routed_gtt_storage.entries[index];

        if (entry->occupied != 0u &&
            memcmp(entry->identity.bytes, identity->bytes, TAVRN_ADVA_LEN) == 0) {
            return 1;
        }
    }
    return 0;
}

static tavrn_adva_t routed_expiry_table_synthetic_identity(uint8_t ordinal)
{
    tavrn_adva_t identity = {
        .bytes = { ordinal, 0x5eu, 0x54u, 0x46u, 0x54u,
                   (uint8_t)(0xc0u | (ordinal & 0x3fu)) },
    };

    return identity;
}

/* The hook seeds once after all FULL owner state is initialized.  It never
 * refreshes entries: each synthetic member must naturally progress through
 * soft and hard expiry on every later due sweep. */
static int routed_expiry_full_table_seed(uint32_t now)
{
    uint8_t seeded = 0u;
    uint8_t ordinal = 1u;

    if (!routed_expiry_table_has_identity(&routed_gtt.config.local_identity)) {
        return 0;
    }
    while (seeded < TAVRN_GTT_CAPACITY - 1u) {
        tavrn_gtt_evidence_t evidence;
        tavrn_gtt_expiry_observe_status_t status;
        tavrn_esc_context_match_t context;
        tavrn_logical_id_t resolved;
        tavrn_adva_t identity;

        if (ordinal == 0u) {
            return 0;
        }
        identity = routed_expiry_table_synthetic_identity(ordinal);
        ordinal++;
        /* Reject every full AdvA collision first, then use the real ESC
         * mapping to reject local, existing, or already-seeded SID8 aliases. */
        if (routed_expiry_table_has_identity(&identity) ||
            tavrn_esc_resolve_sid8(&routed_gtt, identity.bytes[0], now,
                                    &context) != TAVRN_ESC_CONTEXT_UNKNOWN) {
            continue;
        }
        memset(&evidence, 0, sizeof(evidence));
        evidence.identity = identity;
        evidence.serial = identity.bytes[0];
        evidence.serial_present = 1u;
        evidence.hop_count = 2u;
        evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
        status = tavrn_gtt_observe_with_provenance(
            &routed_gtt, &evidence, TAVRN_GTT_PROVENANCE_IMPORTED_HOP_ONE, now, NULL);
        if (status != TAVRN_GTT_EXPIRY_OBSERVE_ADDED ||
            tavrn_full_resolve_unique_sid8(&routed_full, &identity, &resolved) !=
                TAVRN_ESC_CONTEXT_UNIQUE ||
            resolved.width != TAVRN_IDENTITY_SID8 ||
            resolved.value != identity.bytes[0] ||
            tavrn_gtt_application_request(
                &routed_gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER, &identity, now,
                NULL) != TAVRN_GTT_APPLICATION_CHANGED) {
            return 0;
        }
        seeded++;
    }
    return seeded == TAVRN_GTT_CAPACITY - 1u;
}
#endif

#if !TRON_BUILD_BENCHMARK_MODE
static tavrn_maintenance_status_t routed_observe_local_broadcast(void)
{
    tavrn_router_local_broadcast_snapshot_t broadcast;
    tavrn_maintenance_status_t status;

    if (tavrn_router_local_broadcast_snapshot(&routed_router, &broadcast) !=
        TAVRN_ROUTER_LOCAL_BROADCAST_OK) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (broadcast.generation == routed_local_broadcast_generation_seen) {
        return TAVRN_MAINTENANCE_OK;
    }
    routed_local_broadcast_generation_seen = broadcast.generation;
    status = tavrn_maintenance_observe_local_broadcast(
        &routed_maintenance, broadcast.accepted_at_ms);
    return status == TAVRN_MAINTENANCE_GATED ? TAVRN_MAINTENANCE_OK : status;
}
#endif

#if !TRON_BUILD_BENCHMARK_MODE
static void routed_full_snapshot_request(void)
{
    if (!queue_guard_begin()) {
        return;
    }
    if (routed_gtt_snapshot_request_pending != 0u ||
        routed_gtt_snapshot_ready != 0u) {
        routed_gtt_snapshot_request_dropped++;
    } else {
        routed_gtt_snapshot_request_pending = 1u;
    }
    queue_guard_end();
}

static void routed_full_snapshot_service(void)
{
    routed_full_telemetry_status_t status;
    uint8_t requested = 0u;

    if (queue_guard_begin()) {
        requested = routed_gtt_snapshot_request_pending != 0u &&
                    routed_full_logger_summary_active == 0u;
        queue_guard_end();
    }
    if (requested == 0u) {
        return;
    }
    if (queue_guard_begin()) {
        if (routed_full_logger_summary_active != 0u) {
            queue_guard_end();
            return;
        }
        status = routed_full_telemetry_snapshot_gtt(
            &routed_gtt, now_ms(), &routed_full_logger_storage.gtt_snapshot);
        routed_gtt_snapshot_request_pending = 0u;
        if (status == ROUTED_FULL_TELEMETRY_OK) {
            routed_gtt_snapshot_ready = 1u;
        } else {
            routed_gtt_snapshot_failed++;
        }
        queue_guard_end();
    }
}

static int routed_full_snapshot_is_ready(void)
{
    int ready = 0;

    if (queue_guard_begin()) {
        ready = routed_gtt_snapshot_ready != 0u;
        queue_guard_end();
    }
    return ready;
}

static void routed_full_snapshot_clear_ready(void)
{
    if (queue_guard_begin()) {
        routed_gtt_snapshot_ready = 0u;
        queue_guard_end();
    }
}
#endif /* !TRON_BUILD_BENCHMARK_MODE */
#endif /* TRON_BUILD_ROUTED_FULL_TAVRN */

static uint32_t routed_cycle_now_ms(void *context)
{
    (void)context;
    return now_ms();
}

static routed_cycle_start_rx_result_t routed_cycle_start_rx(void *context)
{
    routed_cycle_start_rx_result_t result;

    (void)context;
    ble_mesh_scheduler_start_rx(&routed_scheduler, now_ms());
    result.returned_at_ms = now_ms();
    result.status = ROUTED_CYCLE_START_RX_OK;
    return result;
}

static routed_cycle_scheduler_poll_result_t routed_cycle_scheduler_poll(
    void *context, uint32_t poll_started_at_ms)
{
    routed_cycle_scheduler_poll_result_t result;

    (void)context;
    memset(&result, 0, sizeof(result));
    (void)ble_mesh_scheduler_poll(&routed_scheduler, poll_started_at_ms,
                                  &result.event);
    result.returned_at_ms = now_ms();
    result.status = ROUTED_CYCLE_SCHEDULER_POLL_OK;
    return result;
}

static void routed_cycle_capture_scheduler_input(
    tavrn_router_phase_trace_t *trace, const ble_mesh_sched_event_t *event)
{
    tavrn_router_scheduler_trace_t *scheduler_trace;

    if (trace == NULL || event == NULL) {
        return;
    }
    scheduler_trace = &trace->detail.scheduler_event;
    scheduler_trace->input_event_type = event->type;
    scheduler_trace->input_fault = event->fault;
    scheduler_trace->input_channel = event->channel;
    scheduler_trace->input_rssi_magnitude_db = event->rssi_magnitude_db;
    memcpy(scheduler_trace->input_advertiser.bytes, event->adv_addr,
           sizeof(scheduler_trace->input_advertiser.bytes));
    scheduler_trace->input_adv_len = event->adv_len;
    scheduler_trace->input_present = TAVRN_ROUTER_TRACE_PRESENT;
}

static void routed_cycle_mark_predecode_gate(tavrn_router_phase_trace_t *trace,
                                             tavrn_router_event_status_t status)
{
    tavrn_router_scheduler_trace_t *scheduler_trace;

    if (trace == NULL) {
        return;
    }
    scheduler_trace = &trace->detail.scheduler_event;
    scheduler_trace->status = status;
    scheduler_trace->wire_decode_present = TAVRN_ROUTER_TRACE_NOT_PRESENT;
    scheduler_trace->decoded_frame_present = TAVRN_ROUTER_TRACE_NOT_PRESENT;
    scheduler_trace->link_step_present = TAVRN_ROUTER_TRACE_NOT_PRESENT;
    scheduler_trace->link_event_present = TAVRN_ROUTER_TRACE_NOT_PRESENT;
    scheduler_trace->rx_control_present = TAVRN_ROUTER_TRACE_NOT_PRESENT;
}

static tavrn_router_phase_trace_t routed_cycle_router_scheduler_event(
    void *context, const ble_mesh_sched_event_t *event, uint32_t now)
{
    tavrn_router_phase_trace_t trace;

    (void)context;
    memset(&trace, 0, sizeof(trace));
    trace.phase = TAVRN_ROUTER_TRACE_SCHEDULER_EVENT;
    routed_cycle_capture_scheduler_input(&trace, event);
    if (TRON_BUILD_TEST_HOOKS != 0 && TRON_BUILD_RX_BLOCK_ADVA_ENABLED != 0 &&
        event != NULL && event->type == BLE_MESH_SCHED_EVENT_RX_ADV &&
        adva_equal(event->adv_addr, configured_rx_block_adva)) {
        routed_counters.rx_blocked++;
        trace.completed_at_ms = now_ms();
        routed_cycle_mark_predecode_gate(&trace, TAVRN_ROUTER_EVENT_IGNORED);
        return trace;
    }
#if TRON_BUILD_ROUTED_FULL_TAVRN
    {
        tavrn_mentorship_event_trace_t mentorship_trace;
        tavrn_mentorship_status_t mentorship_status;

        memset(&mentorship_trace, 0, sizeof(mentorship_trace));
        mentorship_status = tavrn_mentorship_handle_scheduler_event(
            &routed_mentorship, event, now, &mentorship_trace);
        if (mentorship_status == TAVRN_MENTORSHIP_GATED) {
            routed_cycle_mark_predecode_gate(&trace, TAVRN_ROUTER_EVENT_REJOINING);
        } else {
            trace.detail.scheduler_event.status = mentorship_trace.router_result;
            trace.detail.scheduler_event.wire_decode_result =
                mentorship_trace.wire_decode_result;
            trace.detail.scheduler_event.decoded_frame_type = mentorship_trace.wire_type;
            trace.detail.scheduler_event.link_event_type = mentorship_trace.link_event_type;
            if (mentorship_trace.reached_wire_link_router != 0u) {
                trace.detail.scheduler_event.wire_decode_present =
                    TAVRN_ROUTER_TRACE_PRESENT;
                trace.detail.scheduler_event.link_step_present =
                    TAVRN_ROUTER_TRACE_PRESENT;
                trace.detail.scheduler_event.link_event_present =
                    TAVRN_ROUTER_TRACE_PRESENT;
            }
            if (mentorship_trace.rx_control_present != 0u) {
                trace.detail.scheduler_event.rx_control = mentorship_trace.rx_control;
                trace.detail.scheduler_event.rx_control_present =
                    TAVRN_ROUTER_TRACE_PRESENT;
            }
            if (mentorship_trace.rx_control_present != 0u &&
                mentorship_status != TAVRN_MENTORSHIP_INVALID) {
                const tavrn_rx_control_event_t *rx_control =
                    &mentorship_trace.rx_control;

                if (tavrn_maintenance_is_targeted_control(
                        rx_control)) {
                    memset(&routed_targeted_rx_input_storage, 0,
                           sizeof(routed_targeted_rx_input_storage));
                    memset(&routed_targeted_rx_action_storage, 0,
                           sizeof(routed_targeted_rx_action_storage));
                    routed_targeted_rx_input_storage.control_event =
                        *rx_control;
                    routed_targeted_rx_status = tavrn_maintenance_targeted_receive(
                        &routed_maintenance, &routed_router, &routed_link,
                        &routed_gtt, &routed_targeted_rx_input_storage, now,
                        &routed_targeted_rx_action_storage);
                    if (routed_targeted_rx_status ==
                        TAVRN_TARGETED_FRESHNESS_INVALID) {
                        trace.detail.scheduler_event.status =
                            TAVRN_ROUTER_EVENT_INVALID;
                    }
                } else if (mentorship_trace.rx_control.control.type ==
                                TAVRN_WIRE_HELLO &&
                            mentorship_trace.rx_control.control.pdu_len >= 6u &&
                            (mentorship_trace.rx_control.control.pdu[5] == 0x80u ||
                             (mentorship_trace.rx_control.control.pdu[5] & 0x40u) != 0u)) {
                    if (tavrn_maintenance_handle_rx_control(
                            &routed_maintenance, rx_control,
                            now) == TAVRN_MAINTENANCE_INVALID) {
                        trace.detail.scheduler_event.status =
                            TAVRN_ROUTER_EVENT_INVALID;
                    }
                }
            }
        }
        if (mentorship_status == TAVRN_MENTORSHIP_INVALID) {
            trace.detail.scheduler_event.status = TAVRN_ROUTER_EVENT_INVALID;
        }
        if (tavrn_maintenance_high_token_scheduler_event(
                &routed_maintenance, &routed_router, &routed_link, event, now,
                &routed_high_token_dispatch) == TAVRN_MAINTENANCE_HIGH_TOKEN_INVALID) {
            trace.detail.scheduler_event.status = TAVRN_ROUTER_EVENT_INVALID;
        }
    }
#else
    (void)tavrn_router_handle_scheduler_event_ex(&routed_router, event, now, &trace);
#endif
    trace.completed_at_ms = now_ms();
    record_router_status(trace.detail.scheduler_event.status);
    record_router_terminal(&trace);
    if (event != NULL && (event->type == BLE_MESH_SCHED_EVENT_RADIO_FAULT ||
                          event->type == BLE_MESH_SCHED_EVENT_SERVICE_FAULT)) {
        mesh_fault_latched = 1u;
        routed_counters.scheduler_fault++;
    }
    return trace;
}

static tavrn_router_phase_trace_t routed_cycle_router_tick(void *context,
                                                               uint32_t now)
{
    aodv_status_t status;

    (void)context;
#if TRON_BUILD_ROUTED_FULL_TAVRN
    {
        tavrn_router_phase_trace_t *trace = &routed_binding_result_storage.router_trace;
        tavrn_full_maintenance_binding_status_t binding_status;
        tavrn_full_application_mailbox_status_t mailbox_status;
        uint8_t application_present = 0u;
        uint8_t scheduler_return_requested = 0u;

        memset(&routed_binding_input_storage, 0, sizeof(routed_binding_input_storage));
        memset(&routed_binding_command_storage, 0, sizeof(routed_binding_command_storage));
        if (!queue_guard_begin()) {
            status = AODV_STATUS_INVALID;
            memset(trace, 0, sizeof(*trace));
            trace->phase = TAVRN_ROUTER_TRACE_TICK;
            trace->detail.tick.status = AODV_STATUS_INVALID;
        } else if ((mailbox_status = tavrn_full_application_mailbox_owner_take(
                        &routed_application_mailbox, &routed_binding_command_storage)) ==
                       TAVRN_FULL_APPLICATION_MAILBOX_PROCESSING ||
                   mailbox_status == TAVRN_FULL_APPLICATION_MAILBOX_EMPTY) {
            queue_guard_end();
            if (mailbox_status == TAVRN_FULL_APPLICATION_MAILBOX_PROCESSING) {
                application_present = 1u;
                routed_binding_command_storage.now_ms = now;
                routed_binding_command_storage.query_time_ms = now;
            }
            routed_binding_input_storage.router = &routed_router;
            routed_binding_input_storage.mentorship = &routed_mentorship;
            routed_binding_input_storage.maintenance = &routed_maintenance;
            routed_binding_input_storage.application_command = routed_binding_command_storage;
            routed_binding_input_storage.application_present = application_present;
            binding_status = tavrn_full_maintenance_binding_tick(
                routed_binding_input_storage, now, &routed_binding_result_storage);
            if (binding_status == TAVRN_FULL_MAINTENANCE_BINDING_OK) {
                status = routed_binding_result_storage.router_status;
            } else {
                status = AODV_STATUS_INVALID;
            }
#if TRON_BUILD_LOCAL_REPAIR
            if (status != AODV_STATUS_INVALID &&
                tavrn_full_repair_binding_tick(&routed_repair_binding, now,
                                               &routed_repair_tick_result) ==
                    TAVRN_FULL_REPAIR_BINDING_INVALID) {
                status = AODV_STATUS_INVALID;
                trace->detail.tick.status = AODV_STATUS_INVALID;
            }
#endif
#if !TRON_BUILD_BENCHMARK_MODE
            if (routed_binding_result_storage.post_tick.sweep_status ==
                    TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK &&
                routed_binding_result_storage.post_tick.sweep.pass_performed != 0u) {
                if (!routed_expiry_sweep_enqueue(
                        now, &routed_binding_result_storage.post_tick.sweep)) {
                    status = AODV_STATUS_INVALID;
                    trace->detail.tick.status = AODV_STATUS_INVALID;
                } else {
                    scheduler_return_requested = 1u;
                }
            }
#endif
            if (application_present != 0u &&
                routed_binding_result_storage.application_result.status <=
                    TAVRN_GTT_APPLICATION_UNAVAILABLE) {
                if (!queue_guard_begin()) {
                    status = AODV_STATUS_INVALID;
                    trace->detail.tick.status = AODV_STATUS_INVALID;
                } else {
                    mailbox_status = tavrn_full_application_mailbox_owner_publish(
                        &routed_application_mailbox,
                        routed_binding_result_storage.application_result);
                    queue_guard_end();
                    if (mailbox_status != TAVRN_FULL_APPLICATION_MAILBOX_READY) {
                        status = AODV_STATUS_INVALID;
                        trace->detail.tick.status = AODV_STATUS_INVALID;
                    }
                }
            } else if (application_present != 0u) {
                status = AODV_STATUS_INVALID;
                trace->phase = TAVRN_ROUTER_TRACE_TICK;
                trace->detail.tick.status = AODV_STATUS_INVALID;
            }
            if (routed_binding_result_storage.status != TAVRN_FULL_MAINTENANCE_BINDING_OK) {
                status = AODV_STATUS_INVALID;
                trace->detail.tick.status = AODV_STATUS_INVALID;
            }
        } else {
            queue_guard_end();
            status = AODV_STATUS_INVALID;
            memset(trace, 0, sizeof(*trace));
            trace->phase = TAVRN_ROUTER_TRACE_TICK;
            trace->detail.tick.status = AODV_STATUS_INVALID;
        }
        if (status == AODV_STATUS_INVALID) {
            record_router_status(TAVRN_ROUTER_EVENT_INVALID);
        } else if (scheduler_return_requested != 0u) {
            routed_router_tick_scheduler_return_requested = 1u;
        }
        record_router_terminal(trace);
        return *trace;
    }
#else
    {
        tavrn_router_phase_trace_t trace;

        memset(&trace, 0, sizeof(trace));
    status = tavrn_router_tick_ex(&routed_router, now, &trace);
        if (status == AODV_STATUS_INVALID) {
            record_router_status(TAVRN_ROUTER_EVENT_INVALID);
        }
        record_router_terminal(&trace);
        return trace;
    }
#endif
}

#if TRON_BUILD_ROUTED_FULL_TAVRN
static uint8_t routed_cycle_router_tick_take_scheduler_return(void *context)
{
    uint8_t requested;

    (void)context;
    requested = routed_router_tick_scheduler_return_requested;
    routed_router_tick_scheduler_return_requested = 0u;
    return requested;
}
#endif

static routed_cycle_application_request_t routed_cycle_application_prepare(
    void *context, uint32_t now)
{
    routed_cycle_application_request_t request;

    (void)context;
    memset(&request, 0, sizeof(request));
#if TRON_BUILD_ROUTED_FULL_TAVRN && !TRON_BUILD_BENCHMARK_MODE
    routed_full_snapshot_service();
#endif
#if TRON_BUILD_BENCHMARK_MODE
    {
        routed_benchmark_slot_t slot;
        routed_benchmark_schedule_status_t schedule_status;

        routed_benchmark_pending_attempt_valid = 0u;
        if (TRON_BUILD_BENCH_ROLE_NUMBER != 1u) {
            request.status = ROUTED_CYCLE_APPLICATION_DISABLED;
            request.prepared_at_ms = now_ms();
            return request;
        }
        if (mesh_fault_latched != 0u ||
            tavrn_router_fault_reason(&routed_router) != TAVRN_ROUTER_FAULT_NONE) {
            request.status = ROUTED_CYCLE_APPLICATION_DISABLED;
            request.prepared_at_ms = now_ms();
            return request;
        }
        schedule_status = routed_benchmark_schedule(now, &slot);
        if (schedule_status == ROUTED_BENCHMARK_SCHEDULE_NONE) {
            request.status = ROUTED_CYCLE_APPLICATION_DISABLED;
            request.prepared_at_ms = now_ms();
            return request;
        }
        if (schedule_status != ROUTED_BENCHMARK_SCHEDULE_DUE) {
            request.status = ROUTED_CYCLE_APPLICATION_INVALID;
            request.prepared_at_ms = now_ms();
            return request;
        }
        memset(&routed_benchmark_pending_attempt, 0,
               sizeof(routed_benchmark_pending_attempt));
        routed_benchmark_pending_attempt.event_at_ms = now;
        routed_benchmark_pending_attempt.deadline_ms = slot.deadline_ms;
        routed_benchmark_pending_attempt.identity = slot.identity;
        routed_benchmark_pending_attempt.burst = slot.burst;
        routed_benchmark_pending_attempt.session_id = slot.session_id;
        routed_benchmark_pending_attempt.sequence = slot.sequence;
        routed_benchmark_pending_attempt.workload = slot.workload;
        routed_benchmark_pending_attempt.kind = ROUTED_BENCHMARK_RECORD_OFFER;
#if TRON_BUILD_ROUTED_FULL_TAVRN
        {
            tavrn_mentorship_state_snapshot_t mentorship_state;
            tavrn_logical_id_t resolved_destination;

            if (tavrn_mentorship_state_snapshot(&routed_mentorship,
                                                &mentorship_state) !=
                    TAVRN_MENTORSHIP_OK ||
                mentorship_state.state != TAVRN_MENTORSHIP_SID8_ACTIVE ||
                mentorship_state.active_width != TAVRN_IDENTITY_SID8 ||
                routed_benchmark_destination_ready(
                    &routed_full, &configured_destination.adva, now,
                    &resolved_destination) != ROUTED_BENCHMARK_DESTINATION_READY) {
                routed_benchmark_pending_attempt.destination = 0u;
                routed_benchmark_pending_attempt.width = TAVRN_IDENTITY_SID8;
                routed_benchmark_pending_attempt.status =
                    ROUTED_BENCHMARK_STATUS_NOT_READY;
                routed_benchmark_pending_attempt.attempted = 0u;
                routed_benchmark_offer_attempt(&routed_benchmark_pending_attempt);
                routed_benchmark_pending_attempt.kind =
                    ROUTED_BENCHMARK_RECORD_APPLICATION;
                routed_benchmark_offer_attempt(&routed_benchmark_pending_attempt);
                routed_benchmark_record_not_ready_status();
                request.status = ROUTED_CYCLE_APPLICATION_DISABLED;
                request.prepared_at_ms = now_ms();
                return request;
            }
            request.data.final_destination = resolved_destination;
        }
#else
        request.data.final_destination = configured_destination.logical_id;
#endif
        routed_benchmark_pending_attempt.destination =
            request.data.final_destination.value;
        routed_benchmark_pending_attempt.width =
            (uint8_t)request.data.final_destination.width;
        routed_benchmark_pending_attempt.status = 0u;
        routed_benchmark_pending_attempt.attempted = 1u;
        routed_benchmark_offer_attempt(&routed_benchmark_pending_attempt);
        routed_benchmark_pending_attempt.kind = ROUTED_BENCHMARK_RECORD_APPLICATION;
        request.data.app_kind = ROUTED_BENCHMARK_APP_KIND;
        request.data.app_source = 0u;
        request.data.app_len = ROUTED_BENCHMARK_APP_PAYLOAD_BYTES;
        request.data.app_bytes[0] = (uint8_t)(slot.session_id & 0xffu);
        request.data.app_bytes[1] = (uint8_t)((slot.session_id >> 8) & 0xffu);
        request.data.app_bytes[2] = (uint8_t)((slot.session_id >> 16) & 0xffu);
        request.data.app_bytes[3] = (uint8_t)((slot.session_id >> 24) & 0xffu);
        request.data.app_bytes[4] = (uint8_t)(slot.identity & 0xffu);
        request.data.app_bytes[5] = (uint8_t)((slot.identity >> 8) & 0xffu);
        request.data.app_bytes[6] = (uint8_t)((slot.identity >> 16) & 0xffu);
        routed_benchmark_pending_attempt_valid = 1u;
        request.status = ROUTED_CYCLE_APPLICATION_READY;
        request.prepared_at_ms = now_ms();
        return request;
    }
#else
    request.status = ROUTED_CYCLE_APPLICATION_DISABLED;
    if (TRON_BUILD_LINK_INITIATOR == 0 ||
        TRON_BUILD_LINK_PEER_ADVA_ENABLED == 0 || mesh_fault_latched != 0u ||
        tavrn_router_fault_reason(&routed_router) != TAVRN_ROUTER_FAULT_NONE ||
        routed_counters.submitted >= TRON_BUILD_LINK_TRANSACTION_TARGET ||
        (int32_t)(now - routed_next_submit_at) < 0) {
        request.prepared_at_ms = now_ms();
        return request;
    }
#if TRON_BUILD_ROUTED_FULL_TAVRN
    {
        tavrn_mentorship_state_snapshot_t mentorship_state;

        if (tavrn_mentorship_state_snapshot(&routed_mentorship, &mentorship_state) !=
            TAVRN_MENTORSHIP_OK) {
            request.prepared_at_ms = now_ms();
            return request;
        }
        request.data.final_destination = configured_destination.logical_id;
        if (mentorship_state.active_width == TAVRN_IDENTITY_SID8) {
            request.data.final_destination.width = TAVRN_IDENTITY_SID8;
            request.data.final_destination.value = configured_destination.adva.bytes[0];
        }
    }
#else
    request.data.final_destination = configured_destination.logical_id;
#endif
    request.data.app_kind = 0x7fu;
    request.data.app_source = 0u;
    request.data.app_len = 4u;
    request.data.app_bytes[0] = (uint8_t)(routed_counters.submitted & 0xffu);
    request.data.app_bytes[1] =
        (uint8_t)((routed_counters.submitted >> 8) & 0xffu);
    request.data.app_bytes[2] =
        (uint8_t)((routed_counters.submitted >> 16) & 0xffu);
    request.data.app_bytes[3] =
        (uint8_t)((routed_counters.submitted >> 24) & 0xffu);
    request.status = ROUTED_CYCLE_APPLICATION_READY;
    request.prepared_at_ms = now_ms();
    return request;
#endif
}

static tavrn_router_phase_trace_t routed_cycle_router_submit(
    void *context, const tron_application_data_t *data, uint32_t now)
{
    tavrn_router_phase_trace_t trace;
    aodv_status_t status;

    (void)context;
    memset(&trace, 0, sizeof(trace));
#if TRON_BUILD_ROUTED_FULL_TAVRN
    status = tavrn_mentorship_submit_application(&routed_mentorship, data, now);
    trace.phase = TAVRN_ROUTER_TRACE_APPLICATION_SUBMIT;
    trace.detail.submit.status = status;
#else
    status = tavrn_router_submit_application_ex(&routed_router, data, now, &trace);
#endif
    trace.completed_at_ms = now_ms();
#if TRON_BUILD_BENCHMARK_MODE
    if (routed_benchmark_pending_attempt_valid != 0u) {
        uint8_t accepted = status == AODV_STATUS_OK || status == AODV_STATUS_QUEUED;

        routed_benchmark_record_submission_status(accepted);
        if (accepted != 0u) {
            routed_counters.submitted++;
        }
        routed_benchmark_pending_attempt.event_at_ms = trace.completed_at_ms;
        routed_benchmark_pending_attempt.accepted = accepted;
        routed_benchmark_pending_attempt.status = (uint32_t)status;
        routed_benchmark_offer_attempt(&routed_benchmark_pending_attempt);
        routed_benchmark_pending_attempt_valid = 0u;
    }
#else
    if (status == AODV_STATUS_OK || status == AODV_STATUS_QUEUED) {
        routed_counters.submitted++;
    }
    routed_next_submit_at = trace.completed_at_ms + TRON_BUILD_LINK_TX_INTERVAL_MS;
#endif
    record_router_terminal(&trace);
    return trace;
}

static tavrn_router_phase_trace_t routed_cycle_router_dispatch(void *context,
                                                                uint32_t now)
{
    tavrn_router_phase_trace_t trace;
    tavrn_router_event_status_t status;

    (void)context;
    memset(&trace, 0, sizeof(trace));
#if TRON_BUILD_ROUTED_FULL_TAVRN
    if (tavrn_mentorship_dispatch(&routed_mentorship, now, &trace) ==
        TAVRN_MENTORSHIP_INVALID) {
        status = TAVRN_ROUTER_EVENT_INVALID;
    } else {
        status = trace.detail.dispatch.status;
    }
#else
    status = tavrn_router_dispatch_trace_ex(&routed_router, now, &trace);
#endif
    trace.completed_at_ms = now_ms();
    record_router_status(status);
    record_router_terminal(&trace);
    handle_dispatch_event(&trace.detail.dispatch.dispatch_event);
    return trace;
}

static tavrn_router_phase_trace_t routed_cycle_router_link_service(void *context,
                                                                     uint32_t now)
{
    tavrn_router_phase_trace_t trace;
    tavrn_router_event_status_t status;

    (void)context;
    memset(&trace, 0, sizeof(trace));
    status = tavrn_router_service_link_ex(&routed_router, now, &trace);
    trace.completed_at_ms = now_ms();
    record_router_status(status);
    record_router_terminal(&trace);
    return trace;
}

static ER routed_wait_for_release(void)
{
    UINT pattern;
    uint32_t progress_epoch;
    int diagnostic_pending;

    diagnostic_pending = routed_diagnostic_pending(&progress_epoch);
    if (diagnostic_pending < 0) {
        return E_CTX;
    }
    for (;;) {
        ER wait_status = tk_wai_flg(routed_release_flag_id, ROUTED_RELEASE_BIT,
                                    TWF_ORW | TWF_BITCLR, &pattern, TMO_FEVR);

        if (wait_status != E_OK) {
            return wait_status;
        }
        if (diagnostic_pending == 0 ||
            routed_logger_progress_epoch != progress_epoch) {
            return E_OK;
        }
        if ((uint32_t)(now_ms() - routed_cycle_state.last_scheduler_return_ms) >=
            tron_timer_config.scheduler_poll_max_ms) {
            return E_TMOUT;
        }
    }
}

static void routed_release_cyclic(void *exinf)
{
    (void)exinf;
    (void)tk_set_flg(routed_release_flag_id, ROUTED_RELEASE_BIT);
}

static routed_cycle_yield_result_t routed_cycle_healthy_yield(
    void *context, uint32_t requested_slack_ms, uint32_t now)
{
    routed_cycle_yield_result_t result;

    (void)context;
    (void)now;
    (void)requested_slack_ms;
    result.status = routed_wait_for_release() == E_OK ? ROUTED_CYCLE_YIELD_OK :
                                                        ROUTED_CYCLE_YIELD_INVALID;
    result.completed_at_ms = now_ms();
    return result;
}

static routed_cycle_fault_idle_result_t routed_cycle_fault_idle(void *context,
                                                                  uint32_t now)
{
    routed_cycle_fault_idle_result_t result;

    (void)context;
    if (routed_cycle_state.first_over_budget_phase !=
        ROUTED_CYCLE_PHASE_NOT_APPLICABLE) {
        latch_scheduler_fault(now);
    }
    result.status = routed_wait_for_release() == E_OK ?
        ROUTED_CYCLE_FAULT_IDLE_OK : ROUTED_CYCLE_FAULT_IDLE_INVALID;
    result.completed_at_ms = now_ms();
    return result;
}

LOCAL void routed_mesh_task(INT stacd, void *exinf)
{
    (void)stacd;
    (void)exinf;
    (void)routed_cycle_run_task(&routed_cycle_state, &routed_cycle_operations,
                                 0u, &routed_cycle_result_storage);
}

static void log_retry_exhausted_event(const tavrn_link_event_t *event)
{
    const tavrn_owned_data_event_t *owned;

    if (event == NULL || event->type != TAVRN_LINK_EVENT_RETRY_EXHAUSTED) {
        return;
    }
    owned = &event->detail.owned_data;
    tm_printf((UB *)"routed retry_exhausted event_type=%u next_hop_adva=%02x:%02x:%02x:%02x:%02x:%02x next_hop_width=%u next_hop_value=0x%04x origin_width=%u origin_value=0x%04x final_destination_width=%u final_destination_value=0x%04x data_seq=%u ttl=%u hops=%u urgent=%u app_kind=0x%02x app_source=0x%02x app_len=%u app_bytes=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x ownership=%u attempts=%u busy_responses=%u requested_mask=0x%02x completed_mask=0x%02x first_tx_ms=%lu last_tx_ms=%lu final_deadline_ms=%lu local_reason=%u mesh_fault_reason=%u\n",
              (UINT)event->type,
              (UINT)owned->next_hop.adva.bytes[0],
              (UINT)owned->next_hop.adva.bytes[1],
              (UINT)owned->next_hop.adva.bytes[2],
              (UINT)owned->next_hop.adva.bytes[3],
              (UINT)owned->next_hop.adva.bytes[4],
              (UINT)owned->next_hop.adva.bytes[5],
              (UINT)owned->next_hop.logical_id.width,
              (UINT)owned->next_hop.logical_id.value,
              (UINT)owned->data.origin.width,
              (UINT)owned->data.origin.value,
              (UINT)owned->data.final_destination.width,
              (UINT)owned->data.final_destination.value,
              (UINT)owned->data.data_seq,
              (UINT)owned->data.ttl,
              (UINT)owned->data.hops,
              (UINT)owned->data.urgent,
              (UINT)owned->data.app_kind,
              (UINT)owned->data.app_source,
              (UINT)owned->data.app_len,
              (UINT)owned->data.app_bytes[0],
              (UINT)owned->data.app_bytes[1],
              (UINT)owned->data.app_bytes[2],
              (UINT)owned->data.app_bytes[3],
              (UINT)owned->data.app_bytes[4],
              (UINT)owned->data.app_bytes[5],
              (UINT)owned->data.app_bytes[6],
              (UINT)owned->data.app_bytes[7],
              (UINT)owned->data.app_bytes[8],
              (UINT)owned->data.app_bytes[9],
              (UINT)owned->data.ownership,
              (UINT)owned->attempt_count,
              (UINT)owned->busy_response_count,
              (UINT)owned->requested_channel_mask,
              (UINT)owned->completed_channel_mask,
              (UW)owned->first_tx_ms,
              (UW)owned->last_tx_ms,
              (UW)owned->final_deadline_ms,
              (UINT)owned->local_reason,
              (UINT)owned->mesh_fault_reason);
}

static void log_cycle_diagnostic(const routed_cycle_trace_t *trace)
{
    if (trace == NULL) {
        return;
    }
    tm_printf((UB *)"routed cycle_diagnostic phase=%u cycle_started_ms=%lu phase_started_ms=%lu phase_completed_ms=%lu phase_elapsed_ms=%lu scheduler_return_ms=%lu scheduler_gap_ms=%lu poll_bound_ms=%lu over_budget=%u over_budget_phase=%u over_budget_source=%u fault_latched=%u\n",
              (UINT)trace->phase, (UW)trace->timing.cycle_started_ms,
              (UW)trace->timing.phase_started_ms,
              (UW)trace->timing.phase_completed_ms,
              (UW)trace->timing.phase_elapsed_ms,
              (UW)trace->timing.scheduler_return_ms,
              (UW)trace->timing.elapsed_since_scheduler_return_ms,
              (UW)trace->timing.poll_bound_ms, (UINT)trace->timing.over_budget,
              (UINT)trace->timing.over_budget_phase,
              (UINT)trace->timing.over_budget_source,
              (UINT)trace->fault_latched);
    switch (trace->phase) {
    case ROUTED_CYCLE_PHASE_PRE_POLL_BOUND:
        tm_printf((UB *)"routed cycle_diagnostic_detail kind=pre_poll status=%u\n",
                  (UINT)trace->detail.pre_poll.status);
        break;
    case ROUTED_CYCLE_PHASE_SCHEDULER_POLL:
        tm_printf((UB *)"routed cycle_diagnostic_detail kind=scheduler_poll status=%u event_type=%u event_fault=%u\n",
                  (UINT)trace->detail.scheduler_poll.status,
                  (UINT)trace->detail.scheduler_poll.event.type,
                  (UINT)trace->detail.scheduler_poll.event.fault);
        break;
    case ROUTED_CYCLE_PHASE_SCHEDULER_EVENT:
        tm_printf((UB *)"routed cycle_diagnostic_detail kind=scheduler_event status=%u input_present=%u input_type=%u input_fault=%u input_channel=%u input_rssi=%u input_advertiser=%02x:%02x:%02x:%02x:%02x:%02x input_adv_len=%u wire_decode_present=%u wire_decode_result=%u decoded_frame_present=%u decoded_frame_type=%u link_step_present=%u link_step_status=%u link_event_present=%u link_event_type=%u rx_control_present=%u control_transmitter=%02x:%02x:%02x:%02x:%02x:%02x control_rssi=%u control_type=%u control_pdu_len=%u control_pdu=%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x terminal_present=%u terminal_fault=%u\n",
                   (UINT)trace->detail.router.detail.scheduler_event.status,
                   (UINT)trace->detail.router.detail.scheduler_event.input_present,
                   (UINT)trace->detail.router.detail.scheduler_event.input_event_type,
                   (UINT)trace->detail.router.detail.scheduler_event.input_fault,
                   (UINT)trace->detail.router.detail.scheduler_event.input_channel,
                   (UINT)trace->detail.router.detail.scheduler_event.input_rssi_magnitude_db,
                   (UINT)trace->detail.router.detail.scheduler_event.input_advertiser.bytes[0],
                   (UINT)trace->detail.router.detail.scheduler_event.input_advertiser.bytes[1],
                   (UINT)trace->detail.router.detail.scheduler_event.input_advertiser.bytes[2],
                   (UINT)trace->detail.router.detail.scheduler_event.input_advertiser.bytes[3],
                   (UINT)trace->detail.router.detail.scheduler_event.input_advertiser.bytes[4],
                   (UINT)trace->detail.router.detail.scheduler_event.input_advertiser.bytes[5],
                   (UINT)trace->detail.router.detail.scheduler_event.input_adv_len,
                   (UINT)trace->detail.router.detail.scheduler_event.wire_decode_present,
                   (UINT)trace->detail.router.detail.scheduler_event.wire_decode_result,
                   (UINT)trace->detail.router.detail.scheduler_event.decoded_frame_present,
                   (UINT)trace->detail.router.detail.scheduler_event.decoded_frame_type,
                   (UINT)trace->detail.router.detail.scheduler_event.link_step_present,
                   (UINT)trace->detail.router.detail.scheduler_event.link_step_status,
                   (UINT)trace->detail.router.detail.scheduler_event.link_event_present,
                   (UINT)trace->detail.router.detail.scheduler_event.link_event_type,
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control_present,
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.transmitter.adva.bytes[0],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.transmitter.adva.bytes[1],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.transmitter.adva.bytes[2],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.transmitter.adva.bytes[3],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.transmitter.adva.bytes[4],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.transmitter.adva.bytes[5],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.rssi_magnitude_db,
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.type,
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu_len,
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[0],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[1],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[2],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[3],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[4],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[5],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[6],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[7],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[8],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[9],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[10],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[11],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[12],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[13],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[14],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[15],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[16],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[17],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[18],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[19],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[20],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[21],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[22],
                   (UINT)trace->detail.router.detail.scheduler_event.rx_control.control.pdu[23],
                   (UINT)trace->detail.router.terminal_fault_present,
                   (UINT)trace->detail.router.terminal_fault);
        break;
    case ROUTED_CYCLE_PHASE_ROUTER_TICK:
        tm_printf((UB *)"routed cycle_diagnostic_detail kind=router_tick status=%u terminal_present=%u terminal_fault=%u\n",
                  (UINT)trace->detail.router.detail.tick.status,
                  (UINT)trace->detail.router.terminal_fault_present,
                  (UINT)trace->detail.router.terminal_fault);
        break;
    case ROUTED_CYCLE_PHASE_APPLICATION_SUBMIT:
        tm_printf((UB *)"routed cycle_diagnostic_detail kind=application_submit request_status=%u prepared_at_ms=%lu submit_present=%u submit_status=%u terminal_present=%u terminal_fault=%u\n",
                  (UINT)trace->detail.application_submit.request.status,
                  (UW)trace->detail.application_submit.request.prepared_at_ms,
                  (UINT)trace->detail.application_submit.submit_present,
                  (UINT)trace->detail.application_submit.submit.detail.submit.status,
                  (UINT)trace->detail.application_submit.submit.terminal_fault_present,
                  (UINT)trace->detail.application_submit.submit.terminal_fault);
        break;
    case ROUTED_CYCLE_PHASE_AODV_ACTION_DISPATCH:
        tm_printf((UB *)"routed cycle_diagnostic_detail kind=dispatch status=%u action_present=%u action_type=%u rreq_enqueue_present=%u rreq_enqueue_outcome=%u link_send_present=%u link_send_status=%u link_event_present=%u link_event_type=%u terminal_present=%u terminal_fault=%u\n",
                  (UINT)trace->detail.router.detail.dispatch.status,
                  (UINT)trace->detail.router.detail.dispatch.action_present,
                  (UINT)trace->detail.router.detail.dispatch.action.type,
                  (UINT)trace->detail.router.detail.dispatch.rreq_enqueue_present,
                  (UINT)trace->detail.router.detail.dispatch.rreq_enqueue.outcome,
                  (UINT)trace->detail.router.detail.dispatch.link_send_present,
                  (UINT)trace->detail.router.detail.dispatch.link_send_status,
                  (UINT)trace->detail.router.detail.dispatch.link_event_present,
                  (UINT)trace->detail.router.detail.dispatch.link_event.type,
                  (UINT)trace->detail.router.terminal_fault_present,
                  (UINT)trace->detail.router.terminal_fault);
        break;
    case ROUTED_CYCLE_PHASE_LINK_SERVICE:
        tm_printf((UB *)"routed cycle_diagnostic_detail kind=link_service status=%u link_step_status=%u link_event_present=%u link_event_type=%u terminal_present=%u terminal_fault=%u\n",
                  (UINT)trace->detail.router.detail.link_service.status,
                  (UINT)trace->detail.router.detail.link_service.link_step_status,
                  (UINT)trace->detail.router.detail.link_service.link_event_present,
                  (UINT)trace->detail.router.detail.link_service.link_event.type,
                  (UINT)trace->detail.router.terminal_fault_present,
                  (UINT)trace->detail.router.terminal_fault);
        break;
    case ROUTED_CYCLE_PHASE_HEALTHY_YIELD:
        tm_printf((UB *)"routed cycle_diagnostic_detail kind=healthy_yield requested_slack_ms=%lu status=%u completed_at_ms=%lu\n",
                  (UW)trace->detail.healthy_yield.requested_slack_ms,
                  (UINT)trace->detail.healthy_yield.result.status,
                  (UW)trace->detail.healthy_yield.result.completed_at_ms);
        break;
    case ROUTED_CYCLE_PHASE_FAULT_IDLE:
        tm_printf((UB *)"routed cycle_diagnostic_detail kind=fault_idle status=%u completed_at_ms=%lu\n",
                  (UINT)trace->detail.fault_idle.status,
                  (UW)trace->detail.fault_idle.completed_at_ms);
        break;
    default:
        break;
    }
}

static void log_rreq_lifecycle(const aodv_rreq_lifecycle_record_t *record)
{
    if (record == NULL) {
        return;
    }
    tm_printf((UB *)"routed rreq_lifecycle stage=%u discovery_correlation=%lu origin=0x%04x destination=0x%04x request_id=%u initial_scope=%u current_scope=%u ring_ordinal=%u action_created_at_ms=%lu action_created_state=%u link_enqueue_at_ms=%lu link_enqueue_state=%u scope_source=%u ordinary=%u scoped=%u hint_used=%u fallback=%u full_diameter=%u action_outcome=%u enqueue_outcome=%u\n",
              (UINT)record->stage,
              (UW)record->attempt.discovery_correlation,
              (UINT)record->attempt.origin.value,
              (UINT)record->attempt.destination.value,
              (UINT)record->attempt.request_id,
              (UINT)record->attempt.initial_scope,
              (UINT)record->attempt.current_scope,
              (UINT)record->attempt.ring_ordinal,
              (UW)record->action_created_at_ms,
              (UINT)record->action_created_at_state,
              (UW)record->link_enqueue_at_ms,
              (UINT)record->link_enqueue_at_state,
              (UINT)record->scope_source, (UINT)record->ordinary,
              (UINT)record->scoped, (UINT)record->hint_used,
              (UINT)record->fallback, (UINT)record->full_diameter,
              (UINT)record->action_outcome, (UINT)record->enqueue_outcome);
}

#if TRON_BUILD_ROUTED_FULL_TAVRN
#if !TRON_BUILD_BENCHMARK_MODE
static void log_expiry_sweep(const routed_expiry_sweep_record_t *record)
{
    if (record == NULL) {
        return;
    }
    tm_printf((UB *)"routed expiry_sweep now_ms=%lu pass=%lu trace_count=%u cursor=%u soft_selected=%u hard_selected=%u demand_deferred=%u unavailable_count=%u departed=%u purged=%u max_copied=%u targeted_controls=%u rreq_controls=%u expiry_tc_controls=%u received_evidence=%lu complete_passes=%lu max_scheduler_gap_ms=%lu scheduler_fault=%lu mesh_fault=%u router_fault=%u\n",
               (UW)record->now_ms, (UW)record->pass, (UINT)record->trace_count,
               (UINT)record->cursor, (UINT)record->soft_selected_count,
               (UINT)record->hard_selected_count,
               (UINT)record->demand_deferred_count,
               (UINT)record->unavailable_count, (UINT)record->departed_count,
               (UINT)record->purged_count,
              (UINT)record->maximum_copied_candidates,
              (UINT)record->targeted_control_count,
              (UINT)record->rreq_control_count,
              (UINT)record->expiry_tc_control_count,
              (UW)record->received_evidence_count, (UW)record->complete_passes,
              (UW)record->max_scheduler_gap_ms,
               (UW)record->scheduler_fault_count,
               (UINT)record->mesh_fault_latched, (UINT)record->router_fault);
}

static void log_full_snapshot(void)
{
    uint8_t index;

    tm_printf((UB *)"routed gtt_snapshot query_at_ms=%lu entry_count=%u nondeparted_count=%u snapshot_q_pending=%u snapshot_ready=%u snapshot_request_dropped=%lu snapshot_failed=%lu\n",
              (UW)routed_full_logger_storage.gtt_snapshot.query_at_ms,
              (UINT)routed_full_logger_storage.gtt_snapshot.entry_count,
              (UINT)routed_full_logger_storage.gtt_snapshot.nondeparted_count,
              (UINT)routed_gtt_snapshot_request_pending,
              (UINT)routed_gtt_snapshot_ready,
              (UW)routed_gtt_snapshot_request_dropped,
              (UW)routed_gtt_snapshot_failed);
    for (index = 0u; index < routed_full_logger_storage.gtt_snapshot.entry_count; index++) {
        const routed_cycle_gtt_snapshot_entry_t *entry =
            &routed_full_logger_storage.gtt_snapshot.entries[index];

        tm_printf((UB *)"routed gtt_snapshot_entry index=%u adva=%02x:%02x:%02x:%02x:%02x:%02x last_evidence_ms=%lu soft_deadline_ms=%lu hard_deadline_ms=%lu departed_deadline_ms=%lu serial=%u serial_state=%u hop_count=%u hop_state=%u freshness=%u departed=%u\n",
                  (UINT)index, (UINT)entry->canonical_adva.bytes[0],
                  (UINT)entry->canonical_adva.bytes[1],
                  (UINT)entry->canonical_adva.bytes[2],
                  (UINT)entry->canonical_adva.bytes[3],
                  (UINT)entry->canonical_adva.bytes[4],
                  (UINT)entry->canonical_adva.bytes[5],
                  (UW)entry->last_evidence_ms, (UW)entry->soft_deadline_ms,
                  (UW)entry->hard_deadline_ms, (UW)entry->departed_deadline_ms,
                  (UINT)entry->serial, (UINT)entry->serial_state,
                  (UINT)entry->hop_count, (UINT)entry->hop_state,
                  (UINT)entry->freshness, (UINT)entry->departed);
    }
}
#endif
#endif

LOCAL void routed_logger_task(INT stacd, void *exinf)
{
#if TRON_BUILD_BENCHMARK_MODE
    routed_delivery_t *delivery = &routed_benchmark_logger_storage.delivery;
    routed_local_event_t *local_event = &routed_benchmark_logger_storage.local_event;
#else
    routed_delivery_t delivery_storage;
    routed_local_event_t local_event_storage;
    routed_delivery_t *delivery = &delivery_storage;
    routed_local_event_t *local_event = &local_event_storage;
#endif
#if !TRON_BUILD_BENCHMARK_MODE
    uint32_t next_summary_at = now_ms() + tron_timer_config.stats_ms;
#endif
#if TRON_BUILD_BENCHMARK_MODE
    uint32_t next_clock_at = now_ms();
    uint32_t next_observation_at = now_ms();
#endif

    (void)stacd;
    (void)exinf;
    tm_printf((UB *)"%s\n", (UB *)tron_build_info_line());
    log_startup();
#if TRON_BUILD_BENCHMARK_MODE
    log_benchmark_boot();
#endif
    while (1) {
        uint32_t now = now_ms();

        if (pending_router_fault != TAVRN_ROUTER_FAULT_NONE &&
            pending_router_fault != logged_router_fault) {
            tm_printf((UB *)"routed router_fault reason=%u\n",
                      (UINT)pending_router_fault);
            logged_router_fault = pending_router_fault;
        }
#if TRON_BUILD_ROUTED_FULL_TAVRN && !TRON_BUILD_BENCHMARK_MODE
        if (routed_full_snapshot_is_ready()) {
            log_full_snapshot();
            routed_full_snapshot_clear_ready();
        } else if (routed_expiry_sweep_dequeue(&routed_logged_expiry_sweep)) {
            log_expiry_sweep(&routed_logged_expiry_sweep);
        } else
#endif
        if (routed_diagnostic_pop()) {
            log_cycle_diagnostic(&routed_logger_record.diagnostic);
        } else if (routed_retry_log_pop()) {
            log_retry_exhausted_event(&routed_logger_record.retry_event);
        } else if (routed_rreq_pop()) {
            log_rreq_lifecycle(&routed_logged_rreq);
        } else if (local_event_pop(local_event)) {
            if (local_event->event.type ==
                TAVRN_ROUTER_DISPATCH_EVENT_PENDING_DATA_FAILED) {
                tm_printf((UB *)"routed pending_data_failed destination=0x%04x\n",
                          (UINT)local_event->event.destination.value);
            } else if (local_event->event.type ==
                       TAVRN_ROUTER_DISPATCH_EVENT_BLACKLIST_NEIGHBOR) {
                tm_printf((UB *)"routed blacklist_neighbor destination=0x%04x peer=%02x:%02x:%02x:%02x:%02x:%02x\n",
                          (UINT)local_event->event.destination.value,
                          (UINT)local_event->event.peer.adva.bytes[0],
                          (UINT)local_event->event.peer.adva.bytes[1],
                          (UINT)local_event->event.peer.adva.bytes[2],
                          (UINT)local_event->event.peer.adva.bytes[3],
                          (UINT)local_event->event.peer.adva.bytes[4],
                          (UINT)local_event->event.peer.adva.bytes[5]);
            }
        } else if (delivery_pop(delivery)) {
#if TRON_BUILD_BENCHMARK_MODE
            routed_benchmark_workload_t workload =
                ROUTED_BENCHMARK_WORKLOAD_HEARTBEAT;
            uint32_t identity = 0u;
            uint32_t origin_session = 0u;
            uint32_t burst = 0u;
            uint16_t sequence = 0u;
            uint32_t session;
            uint32_t emission_now;
            uint64_t record_id;
            int identity_valid = routed_benchmark_decode_payload(
                delivery->data.app_kind, delivery->data.app_len,
                delivery->data.app_bytes, &origin_session, &identity) &&
                routed_benchmark_identity_decode(identity, &workload, &burst,
                                                  &sequence);

            if (routed_benchmark_logger_record(&session, &record_id, NULL)) {
                if (identity_valid != 0) {
                    emission_now = now_ms();
                    tm_printf((UB *)"obs_final schema=observer-v2 now=%lu event_at_ms=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu origin=0x%04x destination=0x%04x identity_valid=1 origin_session=%lu identity=0x%06lx workload=%u burst=%lu sequence=%u payload_id=%u app_kind=%u app_len=%u peer=%02x:%02x:%02x:%02x:%02x:%02x\n",
                                (UW)emission_now, (UW)delivery->delivered_at_ms,
                               (UINT)TRON_BUILD_BENCH_ROLE_NUMBER, (UW)session,
                              (UW)(record_id >> 32), (UW)record_id,
                               (UINT)delivery->data.origin.value,
                               (UINT)delivery->data.final_destination.value,
                               (UW)origin_session, (UW)identity,
                               (UINT)workload, (UW)burst, (UINT)sequence,
                               (UINT)sequence,
                                (UINT)delivery->data.app_kind,
                                (UINT)delivery->data.app_len,
                               (UINT)delivery->transmitter.adva.bytes[0],
                               (UINT)delivery->transmitter.adva.bytes[1],
                               (UINT)delivery->transmitter.adva.bytes[2],
                               (UINT)delivery->transmitter.adva.bytes[3],
                               (UINT)delivery->transmitter.adva.bytes[4],
                                (UINT)delivery->transmitter.adva.bytes[5]);
                } else {
                    emission_now = now_ms();
                    tm_printf((UB *)"obs_final schema=observer-v2 now=%lu event_at_ms=%lu role=%u session=%lu record_id_hi=%lu record_id_lo=%lu origin=0x%04x destination=0x%04x identity_valid=0 origin_session=0 identity=0x%06lx workload=%u burst=%lu sequence=%u payload_id=%u app_kind=%u app_len=%u peer=%02x:%02x:%02x:%02x:%02x:%02x\n",
                                (UW)emission_now, (UW)delivery->delivered_at_ms,
                               (UINT)TRON_BUILD_BENCH_ROLE_NUMBER, (UW)session,
                              (UW)(record_id >> 32), (UW)record_id,
                               (UINT)delivery->data.origin.value,
                               (UINT)delivery->data.final_destination.value,
                               (UW)identity, (UINT)workload, (UW)burst,
                               (UINT)sequence, (UINT)sequence,
                                (UINT)delivery->data.app_kind,
                                (UINT)delivery->data.app_len,
                               (UINT)delivery->transmitter.adva.bytes[0],
                               (UINT)delivery->transmitter.adva.bytes[1],
                               (UINT)delivery->transmitter.adva.bytes[2],
                               (UINT)delivery->transmitter.adva.bytes[3],
                               (UINT)delivery->transmitter.adva.bytes[4],
                                (UINT)delivery->transmitter.adva.bytes[5]);
                }
            }
#else
            tm_printf((UB *)"routed final_data now=%lu origin=0x%04x destination=0x%04x seq=%u app_kind=0x%02x app_len=%u peer=%02x:%02x:%02x:%02x:%02x:%02x\n",
                       (UW)delivery->delivered_at_ms,
                       (UINT)delivery->data.origin.value,
                       (UINT)delivery->data.final_destination.value,
                       (UINT)delivery->data.data_seq, (UINT)delivery->data.app_kind,
                       (UINT)delivery->data.app_len,
                       (UINT)delivery->transmitter.adva.bytes[0],
                       (UINT)delivery->transmitter.adva.bytes[1],
                       (UINT)delivery->transmitter.adva.bytes[2],
                       (UINT)delivery->transmitter.adva.bytes[3],
                        (UINT)delivery->transmitter.adva.bytes[4],
                        (UINT)delivery->transmitter.adva.bytes[5]);
#endif
#if TRON_BUILD_BENCHMARK_MODE
        } else if (routed_benchmark_attempt_pop()) {
            log_benchmark_attempt(&routed_benchmark_logged_attempt);
#endif
#if TRON_BUILD_BENCHMARK_MODE
        }
        /* Drain at most one copied event above, then give due periodic
         * observer records a bounded one-turn latency under sustained load. */
        if ((int32_t)(now - next_clock_at) >= 0) {
            log_benchmark_clock();
            next_clock_at += ((now - next_clock_at) /
                              ROUTED_BENCHMARK_HEARTBEAT_INTERVAL_MS + 1u) *
                ROUTED_BENCHMARK_HEARTBEAT_INTERVAL_MS;
        } else if ((int32_t)(now - next_observation_at) >= 0) {
            log_benchmark_control();
            log_benchmark_health();
#if TRON_BUILD_ROUTED_FULL_TAVRN
            start_benchmark_gtt_emission();
#else
            log_benchmark_gtt_not_implemented(now);
#endif
            next_observation_at += ((now - next_observation_at) / 10000u + 1u) *
                10000u;
#if TRON_BUILD_ROUTED_FULL_TAVRN
        } else if (routed_benchmark_gtt_emission.active != 0u) {
            log_benchmark_gtt_next();
#endif
#else
        } else if ((int32_t)(now - next_summary_at) >= 0) {
            log_summary(now);
#if TRON_BUILD_ROUTED_FULL_TAVRN
            routed_full_snapshot_request();
#endif
            next_summary_at = now + tron_timer_config.stats_ms;
#endif
        }
        (void)tk_dly_tsk(1u);
    }
}

EXPORT INT usermain(void)
{
    T_CTSK mesh_task = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG3,
                          .task = (FP)routed_mesh_task,
                          .itskpri = ROUTED_MESH_TASK_PRIORITY,
                          .stksz = ROUTED_MESH_TASK_STACK_BYTES };
    T_CTSK logger_task = { .exinf = NULL, .tskatr = TA_HLNG | TA_RNG3,
                             .task = (FP)routed_logger_task,
                             .itskpri = ROUTED_LOGGER_TASK_PRIORITY,
                             .stksz = ROUTED_LOGGER_TASK_STACK_BYTES };
    T_CFLG release_flag = { .exinf = NULL, .flgatr = TA_TFIFO,
                             .iflgptn = 0u };
    T_CCYC release_cyclic = { .exinf = NULL, .cycatr = TA_HLNG | TA_PHS,
                               .cychdr = (FP)routed_release_cyclic,
                               .cyctim = 0u, .cycphs = 0u };
    tavrn_link_config_t link_config;
    aodv_core_config_t aodv_config;
    tavrn_router_incarnation_config_t incarnation_config;
    aodv_rreq_telemetry_config_t rreq_telemetry;
    tavrn_router_application_hooks_t application_hooks;
    uint16_t boot_nonce;
#if TRON_BUILD_BENCHMARK_MODE
    uint16_t benchmark_session_high;
    uint32_t benchmark_session;
#endif
    uint32_t release_period_ms;
    ID mesh_id;
    ID logger_id;
    ID release_cyclic_id;

    ring_init(&delivery_state.published);
    ring_init(&local_event_ring);
#if TRON_BUILD_BENCHMARK_MODE
    /* This is intentionally before radio initialization.  It never starts the
     * continuous display task: after exactly 1500 ms of digit scanning the
     * matrix is fully off. */
    display_show_benchmark_role(TRON_BUILD_BENCH_ROLE_NUMBER, 1500u);
#endif
    if (!tron_timer_config_is_valid(&tron_timer_config) ||
        ble_radio_try_init(tron_timer_config.radio_state_timeout_ms) != BLE_RADIO_OP_OK ||
        ble_radio_read_default_adva(runtime_ficr_adva) != BLE_RADIO_OP_OK ||
        !routed_boot_nonce(&boot_nonce)
#if TRON_BUILD_BENCHMARK_MODE
        || !routed_boot_nonce(&benchmark_session_high)
#endif
        ) {
        tm_printf((UB *)"routed bounded radio/FICR/RNG/timer initialization failed\n");
        return 1;
    }
#if TRON_BUILD_BENCHMARK_MODE
    /* The router keeps the low sample as its 16-bit incarnation nonce; the
     * benchmark payload session combines it with a second nonzero sample. */
    benchmark_session = ((uint32_t)benchmark_session_high << 16) | boot_nonce;
#endif
    tm_printf((UB *)"routed boot_nonce=%u\n", (UINT)boot_nonce);
#if TRON_BUILD_BENCH_IDENTIFY_DISPLAY && !TRON_BUILD_BENCHMARK_MODE
    display_init();
    display_show_digit(TRON_BUILD_BENCH_ROLE_NUMBER);
#endif
    if (tron_timer_config.scheduler_poll_max_ms <= 1u) {
        tm_printf((UB *)"routed scheduler poll bound cannot release mesh waits\n");
        return 1;
    }
    release_period_ms = tron_timer_config.scheduler_poll_max_ms - 1u;
    release_cyclic.cyctim = release_period_ms;
    if (TRON_BUILD_LOCAL_ADVA_ENABLED != 0) {
        memcpy(local_adva, configured_local_adva, sizeof(local_adva));
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
    memset(&incarnation_config, 0, sizeof(incarnation_config));
    incarnation_config.boot_nonce = boot_nonce;
    incarnation_config.reboot_announce_ms = tron_timer_config.router_reboot_announce_ms;
    if (tavrn_link_v2_init(&routed_link, &routed_scheduler, &link_config, now_ms()) !=
            TAVRN_LINK_INIT_OK ||
        aodv_core_init(&routed_aodv, &aodv_config, now_ms()) != AODV_INIT_OK) {
        tm_printf((UB *)"routed generated link/AODV configuration rejected\n");
        return 1;
    }
#if TRON_BUILD_ROUTED_FULL_TAVRN
    {
        tavrn_gtt_config_t gtt_config;
        tavrn_router_augmentation_hooks_t full_hooks;

        memset(&gtt_config, 0, sizeof(gtt_config));
        gtt_config.local_identity = local_peer.adva;
        gtt_config.soft_expiry_ms = tron_timer_config.gtt_soft_expiry_ms;
        gtt_config.hard_expiry_ms = tron_timer_config.gtt_hard_expiry_ms;
        gtt_config.departed_retention_ms = tron_timer_config.gtt_departed_ms;
        if (tavrn_gtt_init(&routed_gtt, &routed_gtt_storage, &gtt_config, now_ms()) !=
                TAVRN_GTT_INIT_OK ||
            tavrn_full_init(&routed_full, &routed_gtt) != TAVRN_FULL_INIT_OK) {
            tm_printf((UB *)"routed FULL_TAVRN mentorship initialization rejected\n");
            return 1;
        }
        full_hooks = tavrn_full_router_hooks(&routed_full);
        incarnation_config.feature_level = TAVRN_ROUTER_FEATURE_FULL_TAVRN;
        if (tavrn_router_init_with_incarnation(&routed_router, &routed_link,
                                               &routed_aodv, &full_hooks,
                                               &incarnation_config, now_ms()) !=
            TAVRN_ROUTER_INCARNATION_OK) {
            tm_printf((UB *)"routed FULL_TAVRN router initialization rejected\n");
            return 1;
        }
    }
#else
    incarnation_config.feature_level = TAVRN_ROUTER_FEATURE_AODV_ONLY;
    if (tavrn_router_init_with_incarnation(&routed_router, &routed_link,
                                           &routed_aodv, NULL,
                                           &incarnation_config, now_ms()) !=
        TAVRN_ROUTER_INCARNATION_OK) {
        tm_printf((UB *)"routed AODV_ONLY router initialization rejected\n");
        return 1;
    }
#endif
    memset(&application_hooks, 0, sizeof(application_hooks));
    application_hooks.reserve = router_delivery_reserve;
    application_hooks.commit = router_delivery_commit;
    application_hooks.cancel = router_delivery_cancel;
    if (tavrn_router_set_application_hooks(&routed_router, &application_hooks) !=
        TAVRN_ROUTER_APPLICATION_HOOK_OK) {
        tm_printf((UB *)"routed delivery binding rejected\n");
        return 1;
    }
#if TRON_BUILD_ROUTED_FULL_TAVRN
    {
        tavrn_mentorship_config_t mentorship_config;
        tavrn_maintenance_config_t maintenance_config;
        tavrn_router_control_interceptor_t rrep_interceptor;
#if TRON_BUILD_LOCAL_REPAIR
        tavrn_full_repair_binding_config_t repair_config;
#endif

        memset(&mentorship_config, 0, sizeof(mentorship_config));
        mentorship_config.offer_window_ms = tron_timer_config.mentor_offer_window_ms;
        mentorship_config.page_timeout_ms = tron_timer_config.mentor_page_timeout_ms;
        mentorship_config.self_bootstrap_ms =
            tron_timer_config.mentor_self_bootstrap_ms;
        mentorship_config.offer_suppression_ms =
            tron_timer_config.mentor_offer_suppression_ms;
        mentorship_config.join_dedupe_ms = tron_timer_config.tc_uuid_ms;
        mentorship_config.sync_dedupe_ms = tron_timer_config.mentor_sync_dedupe_ms;
        mentorship_config.rssi_weak_magnitude_db =
            (uint16_t)tron_timer_config.mentor_rssi_weak_magnitude_db;
        mentorship_config.rssi_strong_magnitude_db =
            (uint16_t)tron_timer_config.mentor_rssi_strong_magnitude_db;
        mentorship_config.rssi_weak_delay_ms =
            tron_timer_config.mentor_rssi_weak_delay_ms;
        mentorship_config.rssi_strong_delay_ms =
            tron_timer_config.mentor_rssi_strong_delay_ms;
        mentorship_config.jitter_min_ms = tron_timer_config.mentor_jitter_min_ms;
        mentorship_config.jitter_max_ms = tron_timer_config.mentor_jitter_max_ms;
        mentorship_config.page_attempts =
            (uint8_t)tron_timer_config.mentor_page_attempts;
        if (tavrn_mentorship_init(&routed_mentorship, &routed_router,
                                   &routed_gtt, &mentorship_config, now_ms()) !=
            TAVRN_MENTORSHIP_OK) {
            tm_printf((UB *)"routed FULL_TAVRN mentorship initialization rejected\n");
            return 1;
        }
        memset(&maintenance_config, 0, sizeof(maintenance_config));
        maintenance_config.hello_change_ms = tron_timer_config.hello_change_ms;
        maintenance_config.hello_stable_ms = tron_timer_config.hello_stable_ms;
        maintenance_config.hello_alpha_permille =
            ratio_to_permille(tron_timer_config.hello_alpha);
        maintenance_config.hello_snap_permille =
            ratio_to_permille(tron_timer_config.hello_snap_ratio);
        maintenance_config.topology_sample_ms = tron_timer_config.gtt_maintenance_ms;
        maintenance_config.hello_dedupe_ms = tron_timer_config.hello_dedupe_ms;
        maintenance_config.initial_node_sequence = 1u;
        maintenance_config.aodv_net_traversal_ms =
            tron_timer_config.aodv_net_traversal_ms;
        maintenance_config.freshness_response_min_ms =
            tron_timer_config.freshness_response_min_ms;
        maintenance_config.freshness_response_max_ms =
            tron_timer_config.freshness_response_max_ms;
        maintenance_config.metadata_cooldown_ms = tron_timer_config.metadata_cooldown_ms;
        maintenance_config.tc_uuid_ms = tron_timer_config.tc_uuid_ms;
        maintenance_config.tc_subject_ms = tron_timer_config.tc_subject_ms;
        if (tavrn_maintenance_init(&routed_maintenance, &routed_router,
                                      &routed_gtt, &maintenance_config) !=
                TAVRN_MAINTENANCE_OK ||
            tavrn_full_application_mailbox_init(&routed_application_mailbox) !=
                TAVRN_FULL_APPLICATION_MAILBOX_EMPTY ||
            tavrn_full_maintenance_binding_install(
                &routed_router, &routed_mentorship, &routed_maintenance) !=
                TAVRN_FULL_MAINTENANCE_BINDING_OK) {
            tm_printf((UB *)"routed FULL_TAVRN maintenance initialization rejected\n");
            return 1;
        }
#if TRON_BUILD_LOCAL_REPAIR
        memset(&repair_config, 0, sizeof(repair_config));
        repair_config.repair = &routed_repair;
        repair_config.router = &routed_router;
        repair_config.link = &routed_link;
        repair_config.aodv = &routed_aodv;
        repair_config.maintenance = &routed_maintenance;
        repair_config.gtt = &routed_gtt;
        repair_config.net_diameter = (uint8_t)tron_timer_config.aodv_net_diameter;
        repair_config.path_discovery_ms = tron_timer_config.aodv_path_discovery_ms;
        repair_config.repair_timeout_ms = tron_timer_config.repair_timeout_ms;
        repair_config.repair_cooldown_ms = tron_timer_config.repair_cooldown_ms;
        if (tavrn_full_repair_binding_init(&routed_repair_binding, &repair_config) !=
            TAVRN_FULL_REPAIR_BINDING_OK) {
            tm_printf((UB *)"routed FULL_TAVRN repair initialization rejected\n");
            return 1;
        }
#endif
        memset(&rrep_interceptor, 0, sizeof(rrep_interceptor));
        rrep_interceptor.context = &routed_maintenance;
        rrep_interceptor.receive = routed_verification_rrep_receive;
        if (tavrn_router_set_control_interceptor(&routed_router,
                                                 &rrep_interceptor) !=
            TAVRN_ROUTER_EVENT_OK) {
            tm_printf((UB *)"routed FULL_TAVRN RREP verification binding rejected\n");
            return 1;
        }
#if TRON_BUILD_TEST_EXPIRY_FULL_TABLE
        if (!routed_expiry_full_table_seed(now_ms())) {
            tm_printf((UB *)"routed expiry full-table hook seed failed\n");
            return 1;
        }
#endif
#if !TRON_BUILD_BENCHMARK_MODE
        {
            tavrn_router_local_broadcast_snapshot_t broadcast;

            if (tavrn_router_local_broadcast_snapshot(&routed_router, &broadcast) !=
                TAVRN_ROUTER_LOCAL_BROADCAST_OK) {
                tm_printf((UB *)"routed FULL_TAVRN broadcast observation binding rejected\n");
                return 1;
            }
            routed_local_broadcast_generation_seen = broadcast.generation;
        }
#endif
    }
#endif
    routed_cycle_diagnostic_queue_init(&routed_diagnostic_queue);
    routed_cycle_retry_log_init(&routed_retry_log);
    routed_cycle_rreq_queue_init(&routed_rreq_queue);
    memset(&routed_cycle_operations, 0, sizeof(routed_cycle_operations));
    routed_cycle_operations.context = NULL;
    routed_cycle_operations.start_rx = routed_cycle_start_rx;
    routed_cycle_operations.scheduler_poll = routed_cycle_scheduler_poll;
    routed_cycle_operations.router_scheduler_event =
        routed_cycle_router_scheduler_event;
    routed_cycle_operations.router_tick = routed_cycle_router_tick;
#if TRON_BUILD_ROUTED_FULL_TAVRN
    routed_cycle_operations.take_scheduler_return_after_tick =
        routed_cycle_router_tick_take_scheduler_return;
#endif
    routed_cycle_operations.application_prepare = routed_cycle_application_prepare;
    routed_cycle_operations.router_submit = routed_cycle_router_submit;
    routed_cycle_operations.router_dispatch = routed_cycle_router_dispatch;
    routed_cycle_operations.router_link_service = routed_cycle_router_link_service;
    routed_cycle_operations.healthy_yield = routed_cycle_healthy_yield;
    routed_cycle_operations.fault_idle = routed_cycle_fault_idle;
    routed_cycle_operations.now_ms = routed_cycle_now_ms;
    routed_cycle_operations.trace_sink = routed_cycle_trace_sink;
    routed_cycle_operations.healthy_wait_completion_guard_ms = 0u;
    memset(&routed_cycle_result_storage, 0, sizeof(routed_cycle_result_storage));
    memset(&rreq_telemetry, 0, sizeof(rreq_telemetry));
    rreq_telemetry.context = NULL;
    rreq_telemetry.on_lifecycle = routed_rreq_lifecycle;
#if TRON_BUILD_BENCHMARK_MODE
    routed_benchmark_attempt_queue_init(&routed_benchmark_attempt_queue);
    routed_benchmark_observer_init(&routed_benchmark_observer);
    if (!routed_benchmark_init(&routed_benchmark_state, now_ms(),
                                 benchmark_session)) {
        tm_printf((UB *)"routed benchmark configuration rejected\n");
        return 1;
    }
#else
    routed_next_submit_at = now_ms();
#endif
    if (routed_cycle_init(&routed_cycle_state,
                          tron_timer_config.scheduler_poll_max_ms) !=
            ROUTED_CYCLE_RESULT_OK ||
        aodv_core_set_rreq_telemetry(&routed_aodv, &rreq_telemetry) !=
            AODV_RREQ_TELEMETRY_OK) {
        tm_printf((UB *)"routed runtime observability binding rejected\n");
        return 1;
    }
    routed_release_flag_id = tk_cre_flg(&release_flag);
    if (routed_release_flag_id <= 0) {
        tm_printf((UB *)"routed release flag creation failed id=%d\n",
                  routed_release_flag_id);
        return 1;
    }
    release_cyclic_id = tk_cre_cyc(&release_cyclic);
    if (release_cyclic_id <= 0) {
        tm_printf((UB *)"routed release cyclic creation failed id=%d\n",
                  release_cyclic_id);
        return 1;
    }
    mesh_id = tk_cre_tsk(&mesh_task);
    if (mesh_id <= 0) {
        tm_printf((UB *)"routed mesh task creation failed id=%d\n", mesh_id);
        return 1;
    }
    logger_id = tk_cre_tsk(&logger_task);
    if (logger_id <= 0) {
        tm_printf((UB *)"routed logger task creation failed id=%d\n", logger_id);
        return 1;
    }
    if (tk_sta_cyc(release_cyclic_id) != E_OK) {
        tm_printf((UB *)"routed release cyclic start failed\n");
        return 1;
    }
    if (tk_sta_tsk(mesh_id, 0) != E_OK) {
        tm_printf((UB *)"routed mesh task start failed\n");
        return 1;
    }
    if (tk_sta_tsk(logger_id, 0) != E_OK) {
        tm_printf((UB *)"routed logger task start failed\n");
        (void)tk_stp_cyc(release_cyclic_id);
        (void)tk_ter_tsk(mesh_id);
        return 1;
    }
    tk_slp_tsk(TMO_FEVR);
    return 0;
}
