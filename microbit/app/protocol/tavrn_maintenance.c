#include "tavrn_maintenance.h"

#include <string.h>

#define TAVRN_MAINTENANCE_HALF_RANGE 0x80000000u
#define TAVRN_MAINTENANCE_PERMILLE 1000u
#define TAVRN_MAINTENANCE_LIVENESS_FACTOR 3u
#define TAVRN_MAINTENANCE_LIVENESS_SNAP_PERMILLE 1050u

static int time_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

static int adva_equal(const tavrn_adva_t *left, const tavrn_adva_t *right)
{
    return left != NULL && right != NULL &&
        memcmp(left->bytes, right->bytes, TAVRN_ADVA_LEN) == 0;
}

static int config_is_valid(const tavrn_maintenance_config_t *config)
{
    uint8_t adaptive_all_zero;

    if (config == NULL || config->hello_change_ms == 0u ||
        config->hello_change_ms >= TAVRN_MAINTENANCE_HALF_RANGE ||
        config->hello_dedupe_ms == 0u ||
        config->hello_dedupe_ms >= TAVRN_MAINTENANCE_HALF_RANGE) {
        return 0;
    }
    adaptive_all_zero = config->hello_stable_ms == 0u &&
        config->hello_alpha_permille == 0u &&
        config->hello_snap_permille == 0u &&
        config->topology_sample_ms == 0u;
    if (adaptive_all_zero != 0u) {
        return 1;
    }
    return config->hello_stable_ms >= config->hello_change_ms &&
        config->hello_stable_ms < TAVRN_MAINTENANCE_HALF_RANGE &&
        config->hello_alpha_permille != 0u &&
        config->hello_alpha_permille <= TAVRN_MAINTENANCE_PERMILLE &&
        config->hello_snap_permille != 0u &&
        config->hello_snap_permille <= TAVRN_MAINTENANCE_PERMILLE &&
        config->topology_sample_ms != 0u &&
        config->topology_sample_ms < TAVRN_MAINTENANCE_HALF_RANGE;
}

static int adaptive_enabled(const tavrn_maintenance_t *maintenance)
{
    return maintenance != NULL && maintenance->config.hello_stable_ms != 0u;
}

static uint32_t saturating_multiply(uint32_t value, uint32_t factor)
{
    if (factor != 0u && value > UINT32_MAX / factor) {
        return UINT32_MAX;
    }
    return value * factor;
}

static uint32_t liveness_threshold(uint32_t interval_ms)
{
    return saturating_multiply(interval_ms, TAVRN_MAINTENANCE_LIVENESS_FACTOR);
}

static uint32_t liveness_snap_threshold(uint32_t threshold_ms)
{
    uint64_t scaled = (uint64_t)threshold_ms *
        TAVRN_MAINTENANCE_LIVENESS_SNAP_PERMILLE;

    return scaled >= (uint64_t)UINT32_MAX * TAVRN_MAINTENANCE_PERMILLE ?
        UINT32_MAX : (uint32_t)(scaled / TAVRN_MAINTENANCE_PERMILLE);
}

static void update_liveness_timeout(tavrn_maintenance_t *maintenance,
                                    uint8_t decay_floor)
{
    uint32_t current_threshold;
    uint32_t floor;

    if (maintenance == NULL || adaptive_enabled(maintenance) == 0) {
        return;
    }
    current_threshold = liveness_threshold(maintenance->snapshot.current_interval_ms);
    floor = maintenance->snapshot.liveness_floor_ms;
    maintenance->snapshot.liveness_timeout_ms =
        floor > current_threshold ? floor : current_threshold;
    if (decay_floor == 0u || floor <= current_threshold) {
        return;
    }
    floor -= (floor - current_threshold) / 2u;
    if (floor <= liveness_snap_threshold(current_threshold)) {
        floor = 0u;
    }
    maintenance->snapshot.liveness_floor_ms = floor;
    maintenance->counters.liveness_floor_decayed++;
}

static uint8_t direct_one_hop_count(const tavrn_maintenance_t *maintenance,
                                    uint32_t now_ms)
{
    uint8_t index;
    uint8_t count = 0u;

    (void)now_ms;
    if (maintenance == NULL || maintenance->gtt == NULL ||
        maintenance->gtt->storage == NULL) {
        return 0u;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *entry = &maintenance->gtt->storage->entries[index];

        if (entry->occupied == 0u || entry->departed != 0u ||
            entry->hop_count != 1u ||
            adva_equal(&entry->identity, &maintenance->gtt->config.local_identity)) {
            continue;
        }
        count++;
    }
    return count;
}

static uint32_t adaptive_next_interval(const tavrn_maintenance_t *maintenance,
                                       uint8_t *snapped_out)
{
    uint64_t numerator;
    uint32_t interval;
    uint32_t snap_at;

    *snapped_out = 0u;
    numerator = (uint64_t)maintenance->config.hello_alpha_permille *
        maintenance->snapshot.current_interval_ms +
        (uint64_t)(TAVRN_MAINTENANCE_PERMILLE -
                   maintenance->config.hello_alpha_permille) *
            maintenance->config.hello_stable_ms +
        TAVRN_MAINTENANCE_PERMILLE / 2u;
    interval = (uint32_t)(numerator / TAVRN_MAINTENANCE_PERMILLE);
    if (interval > maintenance->config.hello_stable_ms) {
        interval = maintenance->config.hello_stable_ms;
    }
    if (interval <= maintenance->snapshot.current_interval_ms &&
        maintenance->snapshot.current_interval_ms <
            maintenance->config.hello_stable_ms) {
        interval = maintenance->snapshot.current_interval_ms + 1u;
    }
    snap_at = (uint32_t)(((uint64_t)maintenance->config.hello_snap_permille *
                           maintenance->config.hello_stable_ms) /
                          TAVRN_MAINTENANCE_PERMILLE);
    if (interval >= snap_at && interval != maintenance->config.hello_stable_ms) {
        interval = maintenance->config.hello_stable_ms;
        *snapped_out = 1u;
    }
    return interval;
}

static void advance_adaptive_interval(tavrn_maintenance_t *maintenance)
{
    uint8_t snapped;

    if (adaptive_enabled(maintenance) == 0) {
        return;
    }
    maintenance->snapshot.current_interval_ms =
        adaptive_next_interval(maintenance, &snapped);
    maintenance->counters.interval_advanced++;
    if (snapped != 0u) {
        maintenance->counters.interval_snapped++;
    }
    update_liveness_timeout(maintenance, 0u);
}

/* Sampling uses only retained active direct GTT evidence.  It runs before a
 * due HELLO so a topology change cannot emit one stale-cadence HELLO first. */
static uint8_t sample_topology(tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    uint8_t direct_count;

    direct_count = direct_one_hop_count(maintenance, now_ms);
    maintenance->counters.liveness_sample++;
    maintenance->snapshot.next_topology_sample_ms = now_ms +
        maintenance->config.topology_sample_ms;
    if (direct_count != maintenance->snapshot.direct_one_hop_count) {
        uint32_t old_threshold =
            liveness_threshold(maintenance->snapshot.current_interval_ms);

        if (old_threshold > maintenance->snapshot.liveness_floor_ms) {
            maintenance->snapshot.liveness_floor_ms = old_threshold;
        }
        maintenance->snapshot.direct_one_hop_count = direct_count;
        maintenance->snapshot.current_interval_ms =
            maintenance->config.hello_change_ms;
        maintenance->snapshot.next_hello_due_ms = now_ms +
            maintenance->snapshot.current_interval_ms;
        maintenance->counters.topology_reset++;
        /* Retain the pre-change liveness floor through this newly observed
         * membership epoch. The next unchanged sample performs its bounded
         * decay; expiry gating therefore cannot shorten a direct lifetime at
         * the same transition that reset the HELLO cadence. */
        update_liveness_timeout(maintenance, 0u);
        return 1u;
    }
    maintenance->counters.topology_unchanged++;
    update_liveness_timeout(maintenance, 1u);
    return 0u;
}

static int maintenance_is_initialized(const tavrn_maintenance_t *maintenance)
{
    return maintenance != NULL && maintenance->router != NULL &&
        maintenance->gtt != NULL && maintenance->gtt->storage != NULL &&
        config_is_valid(&maintenance->config);
}

static int sid8_active(const tavrn_maintenance_t *maintenance)
{
    const tavrn_router_t *router;

    if (!maintenance_is_initialized(maintenance)) {
        return 0;
    }
    router = maintenance->router;
    return router->link != NULL && router->incarnation.enabled != 0u &&
        router->incarnation.config.feature_level ==
            TAVRN_ROUTER_FEATURE_FULL_TAVRN &&
        router->incarnation.snapshot.state ==
            TAVRN_ROUTER_INCARNATION_ESTABLISHED &&
        router->link->config.local_peer.logical_id.width == TAVRN_IDENTITY_SID8;
}

static void clear_pending(tavrn_maintenance_t *maintenance)
{
    if (maintenance == NULL) {
        return;
    }
    memset(&maintenance->snapshot.pending_hello, 0,
           sizeof(maintenance->snapshot.pending_hello));
    maintenance->snapshot.pending = 0u;
    maintenance->counters.pending = 0u;
}

static void disarm(tavrn_maintenance_t *maintenance)
{
    if (maintenance == NULL) {
        return;
    }
    if (maintenance->queued_hello_valid != 0u) {
        (void)tavrn_router_cancel_ordinary_hello(maintenance->router,
                                                  &maintenance->queued_hello);
    }
    clear_pending(maintenance);
    memset(&maintenance->queued_hello, 0, sizeof(maintenance->queued_hello));
    maintenance->queued_hello_valid = 0u;
    maintenance->local_broadcast_suppression_active = 0u;
    maintenance->snapshot.armed = 0u;
    maintenance->snapshot.next_hello_due_ms = 0u;
    maintenance->snapshot.next_topology_sample_ms = 0u;
    maintenance->snapshot.current_interval_ms = 0u;
    maintenance->snapshot.liveness_floor_ms = 0u;
    maintenance->snapshot.liveness_timeout_ms = 0u;
    maintenance->snapshot.direct_one_hop_count = 0u;
    memset(maintenance->dedupe, 0, sizeof(maintenance->dedupe));
}

static void purge_dedupe(tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_MAINTENANCE_DEDUPE_CAPACITY; index++) {
        tavrn_maintenance_dedupe_entry_t *entry = &maintenance->dedupe[index];

        if (entry->valid != 0u && time_due(now_ms, entry->expires_at_ms)) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static void clear_dedupe_for_origin(tavrn_maintenance_t *maintenance,
                                    const tavrn_adva_t *origin)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_MAINTENANCE_DEDUPE_CAPACITY; index++) {
        tavrn_maintenance_dedupe_entry_t *entry = &maintenance->dedupe[index];

        if (entry->valid != 0u && adva_equal(&entry->origin, origin)) {
            memset(entry, 0, sizeof(*entry));
        }
    }
}

static tavrn_maintenance_epoch_entry_t *epoch_slot(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *origin)
{
    uint8_t index;

    for (index = 0u; index < TAVRN_MAINTENANCE_EPOCH_CAPACITY; index++) {
        tavrn_maintenance_epoch_entry_t *entry = &maintenance->epochs[index];

        if (entry->valid != 0u && adva_equal(&entry->origin, origin)) {
            return entry;
        }
    }
    for (index = 0u; index < TAVRN_MAINTENANCE_EPOCH_CAPACITY; index++) {
        if (maintenance->epochs[index].valid == 0u) {
            return &maintenance->epochs[index];
        }
    }
    return NULL;
}

/* A reset that is still pending keeps the old tuple visible in router-common.
 * Do not advance this local epoch until the shared reset has committed. */
static tavrn_maintenance_status_t synchronize_peer_epoch(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *origin,
    uint32_t now_ms)
{
    tavrn_router_incarnation_peer_snapshot_t peer;
    tavrn_router_incarnation_status_t peer_status;
    tavrn_maintenance_epoch_entry_t *entry;
    tavrn_gtt_serial_clear_status_t clear_status;

    peer_status = tavrn_router_incarnation_peer_snapshot(
        maintenance->router, origin, &peer);
    if (peer_status == TAVRN_ROUTER_INCARNATION_NOT_FOUND) {
        return TAVRN_MAINTENANCE_OK;
    }
    if (peer_status != TAVRN_ROUTER_INCARNATION_OK) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (peer.direct_binding_valid == 0u || peer.route_barred != 0u ||
        peer.reset_pending != 0u) {
        return TAVRN_MAINTENANCE_OK;
    }
    entry = epoch_slot(maintenance, origin);
    if (entry == NULL) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (entry->valid != 0u && entry->boot_nonce == peer.boot_nonce) {
        return TAVRN_MAINTENANCE_OK;
    }
    clear_status = tavrn_gtt_clear_serial(maintenance->gtt, origin, now_ms);
    if (clear_status == TAVRN_GTT_SERIAL_CLEAR_INVALID) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    clear_dedupe_for_origin(maintenance, origin);
    memset(entry, 0, sizeof(*entry));
    entry->origin = *origin;
    entry->boot_nonce = peer.boot_nonce;
    entry->valid = 1u;
    return TAVRN_MAINTENANCE_OK;
}

static tavrn_maintenance_dedupe_entry_t *dedupe_slot(
    tavrn_maintenance_t *maintenance, const tavrn_adva_t *origin,
    uint16_t node_sequence, uint32_t now_ms, uint8_t *duplicate_out)
{
    uint8_t index;

    *duplicate_out = 0u;
    purge_dedupe(maintenance, now_ms);
    for (index = 0u; index < TAVRN_MAINTENANCE_DEDUPE_CAPACITY; index++) {
        tavrn_maintenance_dedupe_entry_t *entry = &maintenance->dedupe[index];

        if (entry->valid != 0u && entry->node_sequence == node_sequence &&
            adva_equal(&entry->origin, origin)) {
            *duplicate_out = 1u;
            return entry;
        }
    }
    for (index = 0u; index < TAVRN_MAINTENANCE_DEDUPE_CAPACITY; index++) {
        if (maintenance->dedupe[index].valid == 0u) {
            return &maintenance->dedupe[index];
        }
    }
    return NULL;
}

static void remember_dedupe(tavrn_maintenance_dedupe_entry_t *entry,
                            const tavrn_adva_t *origin,
                            uint16_t node_sequence,
                            const tavrn_maintenance_config_t *config,
                            uint32_t now_ms)
{
    memset(entry, 0, sizeof(*entry));
    entry->origin = *origin;
    entry->node_sequence = node_sequence;
    entry->expires_at_ms = now_ms + config->hello_dedupe_ms;
    entry->valid = 1u;
}

static int ordinary_hello_event(const tavrn_maintenance_t *maintenance,
                                const tavrn_rx_control_event_t *event,
                                tavrn_adva_t *origin_out,
                                uint16_t *node_sequence_out)
{
    const tavrn_validated_control_t *control;
    const tavrn_direct_peer_t *transmitter;
    tavrn_adva_t origin;

    if (maintenance == NULL || event == NULL || origin_out == NULL ||
        node_sequence_out == NULL) {
        return 0;
    }
    control = &event->control;
    transmitter = &event->transmitter;
    if (control->type != TAVRN_WIRE_HELLO || control->pdu_len != 17u ||
        control->pdu[0] != 0x54u || control->pdu[1] != 0x52u ||
        control->pdu[2] != 0x02u ||
        control->pdu[3] != maintenance->router->link->config.network_id ||
        control->pdu[4] != TAVRN_WIRE_HELLO || control->pdu[5] != 0x80u ||
        control->pdu[6] != 0x10u || control->pdu[7] != 0xffu ||
        control->pdu[8] != 0xffu ||
        transmitter->logical_id.width != TAVRN_IDENTITY_SID8) {
        return 0;
    }
    memcpy(origin.bytes, &control->pdu[9], TAVRN_ADVA_LEN);
    if (!adva_equal(&origin, &transmitter->adva) ||
        transmitter->logical_id.value != origin.bytes[0] ||
        adva_equal(&origin, &maintenance->gtt->config.local_identity)) {
        return 0;
    }
    *origin_out = origin;
    *node_sequence_out = (uint16_t)control->pdu[15] |
        ((uint16_t)control->pdu[16] << 8);
    return 1;
}

static int ordinary_n0_sid8_hello(const tavrn_rx_control_event_t *event)
{
    return event != NULL && event->control.type == TAVRN_WIRE_HELLO &&
        (event->control.pdu[5] & 0xc0u) == 0x80u;
}

static tavrn_maintenance_status_t enqueue_pending(tavrn_maintenance_t *maintenance,
                                                   uint32_t now_ms)
{
    tavrn_router_hello_status_t status;

    status = tavrn_router_enqueue_ordinary_hello(
        maintenance->router, &maintenance->snapshot.pending_hello, now_ms);
    if (status == TAVRN_ROUTER_HELLO_BUSY) {
        maintenance->counters.busy++;
        return TAVRN_MAINTENANCE_BUSY;
    }
    if (status != TAVRN_ROUTER_HELLO_OK) {
        clear_pending(maintenance);
        return TAVRN_MAINTENANCE_INVALID;
    }
    maintenance->queued_hello = maintenance->snapshot.pending_hello;
    maintenance->queued_hello_valid = 1u;
    clear_pending(maintenance);
    maintenance->counters.enqueued++;
    maintenance->snapshot.next_node_sequence++;
    advance_adaptive_interval(maintenance);
    maintenance->snapshot.next_hello_due_ms = now_ms +
        maintenance->snapshot.current_interval_ms;
    maintenance->local_broadcast_suppression_active = 0u;
    return TAVRN_MAINTENANCE_OK;
}

tavrn_maintenance_status_t tavrn_maintenance_init(
    tavrn_maintenance_t *maintenance, tavrn_router_t *router, tavrn_gtt_t *gtt,
    const tavrn_maintenance_config_t *config)
{
    if (maintenance == NULL || router == NULL || gtt == NULL || gtt->storage == NULL ||
        !config_is_valid(config)) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    memset(maintenance, 0, sizeof(*maintenance));
    maintenance->router = router;
    maintenance->gtt = gtt;
    maintenance->config = *config;
    maintenance->snapshot.current_interval_ms = config->hello_change_ms;
    maintenance->snapshot.next_node_sequence = config->initial_node_sequence;
    maintenance->expiry_schedule_initialized = 0u;
    return TAVRN_MAINTENANCE_OK;
}

tavrn_maintenance_status_t tavrn_maintenance_activate(
    tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    if (!maintenance_is_initialized(maintenance)) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (!sid8_active(maintenance)) {
        disarm(maintenance);
        return TAVRN_MAINTENANCE_GATED;
    }
    if (maintenance->snapshot.armed != 0u) {
        return TAVRN_MAINTENANCE_OK;
    }
    maintenance->snapshot.armed = 1u;
    maintenance->snapshot.current_interval_ms = maintenance->config.hello_change_ms;
    maintenance->snapshot.next_hello_due_ms = now_ms +
        maintenance->snapshot.current_interval_ms;
    maintenance->expiry_next_due_ms = 0u;
    maintenance->expiry_cursor = 0u;
    maintenance->expiry_schedule_initialized = 0u;
    if (adaptive_enabled(maintenance) != 0) {
        maintenance->snapshot.direct_one_hop_count =
            direct_one_hop_count(maintenance, now_ms);
        maintenance->snapshot.next_topology_sample_ms = now_ms +
            maintenance->config.topology_sample_ms;
        maintenance->snapshot.liveness_floor_ms = 0u;
        update_liveness_timeout(maintenance, 0u);
    }
    return TAVRN_MAINTENANCE_OK;
}

tavrn_maintenance_status_t tavrn_maintenance_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    tavrn_router_hello_status_t build_status;
    uint8_t topology_changed = 0u;

    if (!maintenance_is_initialized(maintenance)) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (!sid8_active(maintenance) || maintenance->snapshot.armed == 0u) {
        disarm(maintenance);
        return TAVRN_MAINTENANCE_GATED;
    }
    if (adaptive_enabled(maintenance) != 0 &&
        time_due(now_ms, maintenance->snapshot.next_topology_sample_ms)) {
        topology_changed = sample_topology(maintenance, now_ms);
    }
    if (maintenance->snapshot.pending != 0u) {
        return enqueue_pending(maintenance, now_ms);
    }
    if (topology_changed != 0u) {
        return TAVRN_MAINTENANCE_OK;
    }
    if (!time_due(now_ms, maintenance->snapshot.next_hello_due_ms)) {
        return TAVRN_MAINTENANCE_OK;
    }
    memset(&maintenance->snapshot.pending_hello, 0,
           sizeof(maintenance->snapshot.pending_hello));
    build_status = tavrn_router_build_ordinary_hello(
        maintenance->router, maintenance->snapshot.next_node_sequence,
        &maintenance->snapshot.pending_hello);
    if (build_status != TAVRN_ROUTER_HELLO_OK) {
        return build_status == TAVRN_ROUTER_HELLO_GATED ?
            TAVRN_MAINTENANCE_GATED : TAVRN_MAINTENANCE_INVALID;
    }
    maintenance->snapshot.pending = 1u;
    maintenance->counters.pending = 1u;
    maintenance->counters.due++;
    return enqueue_pending(maintenance, now_ms);
}

tavrn_maintenance_status_t tavrn_maintenance_handle_rx_control(
    tavrn_maintenance_t *maintenance,
    const tavrn_rx_control_event_t *control_event, uint32_t now_ms)
{
    tavrn_adva_t origin;
    tavrn_maintenance_dedupe_entry_t *entry;
    tavrn_gtt_evidence_t evidence;
    tavrn_gtt_expiry_observe_status_t observe_status;
    uint16_t node_sequence;
    uint8_t duplicate;

    if (!maintenance_is_initialized(maintenance) || control_event == NULL) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    /* The copied N=1 passes through this function after router-common has
     * committed it.  A delayed reset is reconciled on the next copied HELLO,
     * before its ordinary N=0 serial is evaluated below. */
    if (control_event->control.type == TAVRN_WIRE_HELLO &&
        synchronize_peer_epoch(maintenance, &control_event->transmitter.adva,
                               now_ms) != TAVRN_MAINTENANCE_OK) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (!sid8_active(maintenance)) {
        return TAVRN_MAINTENANCE_IGNORED;
    }
    if (!ordinary_n0_sid8_hello(control_event)) {
        return TAVRN_MAINTENANCE_IGNORED;
    }
    if (!ordinary_hello_event(maintenance, control_event, &origin,
                              &node_sequence)) {
        maintenance->counters.rx_rejected++;
        return TAVRN_MAINTENANCE_RX_REJECTED;
    }
    entry = dedupe_slot(maintenance, &origin, node_sequence, now_ms, &duplicate);
    if (duplicate != 0u) {
        maintenance->counters.rx_duplicate++;
        return TAVRN_MAINTENANCE_RX_DUPLICATE;
    }
    if (entry == NULL) {
        maintenance->counters.rx_rejected++;
        maintenance->counters.dedupe_capacity++;
        return TAVRN_MAINTENANCE_RX_REJECTED;
    }
    memset(&evidence, 0, sizeof(evidence));
    evidence.identity = origin;
    evidence.serial = node_sequence;
    evidence.serial_present = 1u;
    evidence.hop_count = 1u;
    evidence.kind = TAVRN_GTT_EVIDENCE_LIVENESS;
    observe_status = tavrn_gtt_observe_with_provenance(
        maintenance->gtt, &evidence, TAVRN_GTT_PROVENANCE_DIRECT_HELLO, now_ms,
        NULL);
    if (observe_status != TAVRN_GTT_EXPIRY_OBSERVE_ADDED &&
        observe_status != TAVRN_GTT_EXPIRY_OBSERVE_REFRESHED) {
        maintenance->counters.rx_rejected++;
        return TAVRN_MAINTENANCE_RX_REJECTED;
    }
    remember_dedupe(entry, &origin, node_sequence, &maintenance->config, now_ms);
    maintenance->counters.rx_unique++;
    return TAVRN_MAINTENANCE_RX_UNIQUE;
}

tavrn_maintenance_status_t tavrn_maintenance_observe_local_broadcast(
    tavrn_maintenance_t *maintenance, uint32_t accepted_at_ms)
{
    if (!maintenance_is_initialized(maintenance)) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    if (!sid8_active(maintenance) || maintenance->snapshot.armed == 0u) {
        return TAVRN_MAINTENANCE_GATED;
    }
    if (adaptive_enabled(maintenance) == 0) {
        return TAVRN_MAINTENANCE_IGNORED;
    }
    if (maintenance->snapshot.pending != 0u) {
        /* This HELLO has not entered the link queue.  The accepted broadcast
         * satisfies this cadence cycle, so discard only the retained local
         * copy without consuming its node sequence. */
        clear_pending(maintenance);
    }
    maintenance->counters.local_broadcast_suppressed++;
    if (maintenance->local_broadcast_suppression_active != 0u) {
        maintenance->snapshot.next_hello_due_ms = accepted_at_ms +
            maintenance->snapshot.current_interval_ms;
        return TAVRN_MAINTENANCE_OK;
    }
    advance_adaptive_interval(maintenance);
    maintenance->snapshot.next_hello_due_ms = accepted_at_ms +
        maintenance->snapshot.current_interval_ms;
    maintenance->local_broadcast_suppression_active = 1u;
    return TAVRN_MAINTENANCE_OK;
}

tavrn_maintenance_status_t tavrn_maintenance_snapshot(
    const tavrn_maintenance_t *maintenance,
    tavrn_maintenance_snapshot_t *snapshot_out)
{
    if (!maintenance_is_initialized(maintenance) || snapshot_out == NULL) {
        return TAVRN_MAINTENANCE_INVALID;
    }
    *snapshot_out = maintenance->snapshot;
    return TAVRN_MAINTENANCE_OK;
}

const tavrn_maintenance_counters_t *tavrn_maintenance_counters(
    const tavrn_maintenance_t *maintenance)
{
    return maintenance_is_initialized(maintenance) ? &maintenance->counters : NULL;
}

static tavrn_gtt_entry_t *maintenance_entry(tavrn_maintenance_t *maintenance,
                                            const tavrn_adva_t *identity)
{
    uint8_t index;

    if (!maintenance_is_initialized(maintenance) || identity == NULL) {
        return NULL;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        tavrn_gtt_entry_t *entry = &maintenance->gtt->storage->entries[index];

        if (entry->occupied != 0u && adva_equal(&entry->identity, identity)) {
            return entry;
        }
    }
    return NULL;
}

static const tavrn_gtt_entry_t *maintenance_const_entry(
    const tavrn_maintenance_t *maintenance, const tavrn_adva_t *identity)
{
    return maintenance_entry((tavrn_maintenance_t *)(void *)maintenance, identity);
}

static uint32_t expiry_interval(const tavrn_maintenance_t *maintenance)
{
    return maintenance->config.topology_sample_ms != 0u ?
        maintenance->config.topology_sample_ms : maintenance->config.hello_change_ms;
}

static int logical_subject_for_identity(const tavrn_maintenance_t *maintenance,
                                        const tavrn_adva_t *identity,
                                        const tavrn_gtt_entry_t *entry,
                                        tavrn_logical_id_t *subject_out)
{
    uint8_t matches = 0u;
    uint8_t index;
    tavrn_identity_width_t width;

    if (!maintenance_is_initialized(maintenance) || identity == NULL ||
        subject_out == NULL) {
        return 0;
    }
    width = maintenance->router->link->config.local_peer.logical_id.width;
    memset(subject_out, 0, sizeof(*subject_out));
    subject_out->width = width;
    if (width == TAVRN_IDENTITY_SID16) {
        subject_out->value = (uint16_t)identity->bytes[0] |
            ((uint16_t)identity->bytes[1] << 8);
        return subject_out->value != 0u && subject_out->value != 0xffffu;
    }
    if (width != TAVRN_IDENTITY_SID8 || identity->bytes[0] == 0u ||
        identity->bytes[0] == 0xffu) {
        return 0;
    }
    /* A departed subject has no live compressed context, but it also has no
     * demand; permit its zero-demand departure/purge bookkeeping. */
    if (entry != NULL && entry->departed != 0u) {
        subject_out->value = identity->bytes[0];
        return 1;
    }
    for (index = 0u; index < TAVRN_GTT_CAPACITY; index++) {
        const tavrn_gtt_entry_t *candidate =
            &maintenance->gtt->storage->entries[index];

        if (candidate->occupied != 0u && candidate->departed == 0u &&
            candidate->identity.bytes[0] == identity->bytes[0]) {
            matches++;
        }
    }
    if (matches != 1u) {
        return 0;
    }
    subject_out->value = identity->bytes[0];
    return 1;
}

tavrn_maintenance_demand_status_t tavrn_maintenance_demand_snapshot(
    const tavrn_maintenance_t *maintenance, const tavrn_adva_t *identity,
    uint32_t now_ms, tavrn_maintenance_demand_snapshot_t *snapshot_out)
{
    tavrn_maintenance_demand_snapshot_t snapshot;
    const tavrn_gtt_entry_t *entry;
    tavrn_logical_id_t subject;
    tavrn_aodv_subject_demand_snapshot_t aodv;
    tavrn_link_subject_demand_snapshot_t link;
    tavrn_router_subject_demand_snapshot_t router;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (!maintenance_is_initialized(maintenance) || identity == NULL ||
        snapshot_out == NULL) {
        return TAVRN_MAINTENANCE_DEMAND_INVALID;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.identity = *identity;
    entry = maintenance_const_entry(maintenance, identity);
    if (entry == NULL) {
        snapshot.snapshot_available = 1u;
        *snapshot_out = snapshot;
        return TAVRN_MAINTENANCE_DEMAND_OK;
    }
    snapshot.gtt_revision = entry->revision;
    if (!logical_subject_for_identity(maintenance, identity, entry, &subject)) {
        return TAVRN_MAINTENANCE_DEMAND_UNAVAILABLE;
    }
    aodv = aodv_core_subject_demand_snapshot(maintenance->router->aodv, &subject,
                                              identity, now_ms);
    link = tavrn_link_v2_subject_demand_snapshot(maintenance->router->link, &subject,
                                                 identity, now_ms);
    router = tavrn_router_subject_demand_snapshot(maintenance->router, &subject,
                                                  identity, now_ms);
    if (aodv.snapshot_available == 0u || link.snapshot_available == 0u ||
        router.snapshot_available == 0u) {
        return TAVRN_MAINTENANCE_DEMAND_UNAVAILABLE;
    }
    snapshot.reason_mask = (uint16_t)(aodv.reason_mask | link.reason_mask |
                                      router.reason_mask);
    if (entry->application_requested != 0u) {
        snapshot.reason_mask |= TAVRN_MAINT_DEMAND_APPLICATION_REQUEST;
    }
    snapshot.valid_route_to_subject = aodv.valid_route_to_subject;
    snapshot.stage_zero_eligible = aodv.valid_route_to_subject;
    snapshot.snapshot_available = 1u;
    *snapshot_out = snapshot;
    return TAVRN_MAINTENANCE_DEMAND_OK;
}

static int direct_deadline_for(const tavrn_maintenance_t *maintenance,
                               const tavrn_gtt_entry_t *entry,
                               uint32_t *deadline_out)
{
    uint32_t interval_limit;
    uint32_t duration;

    if (maintenance == NULL || entry == NULL || deadline_out == NULL) {
        return 0;
    }
    interval_limit = saturating_multiply(maintenance->snapshot.current_interval_ms,
                                         TAVRN_MAINTENANCE_LIVENESS_FACTOR);
    duration = maintenance->snapshot.liveness_floor_ms > interval_limit ?
        maintenance->snapshot.liveness_floor_ms : interval_limit;
    if (duration == 0u || duration >= TAVRN_MAINTENANCE_HALF_RANGE) {
        return 0;
    }
    *deadline_out = entry->last_direct_evidence_ms + duration;
    return 1;
}

static uint32_t maintenance_next_generation(tavrn_maintenance_t *maintenance)
{
    uint32_t generation = maintenance->gtt->storage->next_generation + 1u;

    if (generation == 0u) {
        generation = 1u;
    }
    maintenance->gtt->storage->next_generation = generation;
    return generation;
}

tavrn_maintenance_checked_departure_status_t tavrn_maintenance_checked_local_departure(
    tavrn_maintenance_t *maintenance,
    const tavrn_gtt_departure_candidate_t *candidate, uint32_t now_ms,
    tavrn_gtt_expiry_snapshot_t *snapshot_out)
{
    tavrn_gtt_entry_t *entry;
    tavrn_gtt_expiry_snapshot_t snapshot;
    tavrn_maintenance_demand_snapshot_t demand;
    tavrn_maintenance_demand_status_t demand_status;
    uint32_t direct_deadline;
    uint32_t generation;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (!maintenance_is_initialized(maintenance) || candidate == NULL) {
        return TAVRN_GTT_CHECKED_INVALID;
    }
    entry = maintenance_entry(maintenance, &candidate->identity);
    if (entry == NULL) {
        return TAVRN_GTT_CHECKED_NOT_FOUND;
    }
    if (tavrn_gtt_expiry_snapshot(maintenance->gtt, &candidate->identity, now_ms,
                                  &snapshot) != TAVRN_GTT_EXPIRY_QUERY_FOUND) {
        return TAVRN_GTT_CHECKED_INVALID;
    }
    if (snapshot_out != NULL) {
        *snapshot_out = snapshot;
    }
    if (candidate->revision != entry->revision) {
        snapshot.freshness = TAVRN_GTT_FRESHNESS_ACTIVE;
        if (snapshot_out != NULL) {
            *snapshot_out = snapshot;
        }
        return TAVRN_GTT_CHECKED_STALE_REVISION;
    }
    if (candidate->hard_deadline_ms != entry->hard_deadline_ms) {
        snapshot.freshness = TAVRN_GTT_FRESHNESS_ACTIVE;
        if (snapshot_out != NULL) {
            *snapshot_out = snapshot;
        }
        return TAVRN_GTT_CHECKED_INVALID;
    }
    if (adva_equal(&candidate->identity, &maintenance->gtt->config.local_identity)) {
        return TAVRN_GTT_CHECKED_SELF;
    }
    if (entry->departed != 0u) {
        return TAVRN_GTT_CHECKED_ALREADY_DEPARTED;
    }
    if (!time_due(now_ms, entry->hard_deadline_ms)) {
        /* A copied departure candidate represents the still-live membership
         * observation, not a request to expose an intermediate soft-stale
         * classification.  Rejection must leave that candidate value intact. */
        snapshot.freshness = TAVRN_GTT_FRESHNESS_ACTIVE;
        if (snapshot_out != NULL) {
            *snapshot_out = snapshot;
        }
        return TAVRN_GTT_CHECKED_NOT_HARD_EXPIRED;
    }
    demand_status = tavrn_maintenance_demand_snapshot(maintenance,
                                                       &candidate->identity,
                                                       now_ms, &demand);
    if (demand_status != TAVRN_MAINTENANCE_DEMAND_OK ||
        demand.snapshot_available == 0u) {
        return TAVRN_GTT_CHECKED_UNAVAILABLE;
    }
    if (demand.reason_mask != 0u) {
        return TAVRN_GTT_CHECKED_DEMAND_DEFERRED;
    }
    if (entry->direct != 0u) {
        if (!direct_deadline_for(maintenance, entry, &direct_deadline)) {
            if (snapshot_out != NULL) {
                memset(snapshot_out, 0, sizeof(*snapshot_out));
            }
            return TAVRN_GTT_CHECKED_INVALID;
        }
        if (!time_due(now_ms, direct_deadline)) {
            snapshot.freshness = TAVRN_GTT_FRESHNESS_ACTIVE;
            if (snapshot_out != NULL) {
                *snapshot_out = snapshot;
            }
            return TAVRN_GTT_CHECKED_DIRECT_DEADLINE_DEFERRED;
        }
    }
    entry->departed = 1u;
    entry->departed_deadline_ms = now_ms + maintenance->gtt->config.departed_retention_ms;
    entry->direct = 0u;
    entry->last_direct_evidence_ms = 0u;
    entry->application_requested = 0u;
    generation = maintenance_next_generation(maintenance);
    entry->revision = generation;
    entry->storage_generation = generation;
    maintenance->gtt->counters.departed++;
    if (tavrn_gtt_expiry_snapshot(maintenance->gtt, &candidate->identity, now_ms,
                                  &snapshot) != TAVRN_GTT_EXPIRY_QUERY_FOUND) {
        return TAVRN_GTT_CHECKED_INVALID;
    }
    if (snapshot_out != NULL) {
        *snapshot_out = snapshot;
    }
    return TAVRN_GTT_CHECKED_DEPARTED;
}

tavrn_maintenance_expiry_sweep_status_t tavrn_maintenance_sweep_expiry(
    tavrn_maintenance_t *maintenance, uint32_t now_ms,
    tavrn_maintenance_expiry_sweep_snapshot_t *snapshot_out)
{
    tavrn_maintenance_expiry_sweep_snapshot_t snapshot;
    uint8_t visited;
    uint8_t start_cursor;

    if (snapshot_out != NULL) {
        memset(snapshot_out, 0, sizeof(*snapshot_out));
    }
    if (!maintenance_is_initialized(maintenance) || snapshot_out == NULL ||
        expiry_interval(maintenance) == 0u) {
        return TAVRN_MAINTENANCE_EXPIRY_SWEEP_INVALID;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    if (maintenance->expiry_schedule_initialized == 0u) {
        maintenance->expiry_next_due_ms = now_ms + expiry_interval(maintenance);
        maintenance->expiry_schedule_initialized = 1u;
    }
    snapshot.next_due_ms = maintenance->expiry_next_due_ms;
    snapshot.next_cursor = maintenance->expiry_cursor;
    if (!time_due(now_ms, maintenance->expiry_next_due_ms)) {
        *snapshot_out = snapshot;
        return TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK;
    }
    snapshot.pass_performed = 1u;
    snapshot.received_evidence_count = maintenance->expiry_received_evidence_count;
    maintenance->expiry_received_evidence_count = 0u;
    start_cursor = maintenance->expiry_cursor;
    for (visited = 0u; visited < TAVRN_GTT_CAPACITY; visited++) {
        uint8_t slot = (uint8_t)((start_cursor + visited) %
                                  TAVRN_GTT_CAPACITY);
        tavrn_gtt_entry_t *entry = &maintenance->gtt->storage->entries[slot];

        if (entry->occupied == 0u) {
            continue;
        }
        if (entry->departed != 0u) {
            if (time_due(now_ms, entry->departed_deadline_ms)) {
                memset(entry, 0, sizeof(*entry));
                snapshot.purged_count++;
                continue;
            }
        }
        snapshot.trace_slots[snapshot.trace_count++] = slot;
        snapshot.maximum_copied_candidates = 1u;
        maintenance->expiry_cursor = (uint8_t)((slot + 1u) % TAVRN_GTT_CAPACITY);
        if (entry->departed != 0u) {
            continue;
        }
        if (adva_equal(&entry->identity, &maintenance->gtt->config.local_identity)) {
            continue;
        }
        if (time_due(now_ms, entry->hard_deadline_ms)) {
            tavrn_gtt_departure_candidate_t candidate;
            tavrn_maintenance_checked_departure_status_t status;

            snapshot.hard_selected_count++;
            candidate.identity = entry->identity;
            candidate.revision = entry->revision;
            candidate.hard_deadline_ms = entry->hard_deadline_ms;
            status = tavrn_maintenance_checked_local_departure(maintenance,
                                                                &candidate, now_ms,
                                                                NULL);
            if (status == TAVRN_GTT_CHECKED_DEPARTED) {
                snapshot.departed_count++;
            } else if (status == TAVRN_GTT_CHECKED_DEMAND_DEFERRED ||
                       status == TAVRN_GTT_CHECKED_DIRECT_DEADLINE_DEFERRED) {
                snapshot.demand_deferred_count++;
            } else if (status == TAVRN_GTT_CHECKED_UNAVAILABLE ||
                       status == TAVRN_GTT_CHECKED_INVALID) {
                snapshot.unavailable_count++;
            }
        } else if (time_due(now_ms, entry->soft_deadline_ms)) {
            snapshot.soft_selected_count++;
        }
    }
    maintenance->expiry_next_due_ms = now_ms + expiry_interval(maintenance);
    snapshot.next_due_ms = maintenance->expiry_next_due_ms;
    snapshot.next_cursor = maintenance->expiry_cursor;
    *snapshot_out = snapshot;
    return TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK;
}

tavrn_maintenance_owner_pre_tick_result_t tavrn_maintenance_owner_pre_tick(
    tavrn_maintenance_t *maintenance, tavrn_maintenance_owner_pre_tick_input_t input,
    uint32_t now_ms)
{
    tavrn_maintenance_owner_pre_tick_result_t result;

    memset(&result, 0, sizeof(result));
    result.observe_status = TAVRN_GTT_EXPIRY_OBSERVE_INVALID;
    result.application_result.status = TAVRN_GTT_APPLICATION_INVALID;
    result.application_result.query_status = TAVRN_GTT_EXPIRY_QUERY_INVALID;
    if (!maintenance_is_initialized(maintenance) || input.evidence_present > 1u ||
        input.application_present > 1u) {
        return result;
    }
    if (input.evidence_present != 0u) {
        result.observe_present = 1u;
        result.observe_status = tavrn_gtt_observe_with_provenance(
            maintenance->gtt, &input.evidence, input.provenance, now_ms, NULL);
        if (result.observe_status != TAVRN_GTT_EXPIRY_OBSERVE_INVALID) {
            maintenance->expiry_received_evidence_count++;
        }
    }
    if (input.application_present != 0u) {
        result.application_present = 1u;
        input.application_command.now_ms = now_ms;
        input.application_command.query_time_ms = now_ms;
        result.application_result = tavrn_gtt_apply_application_command(
            maintenance->gtt, TAVRN_PHASE5_EXPIRY_LANE_MESH_OWNER,
            input.application_command);
    }
    return result;
}

tavrn_maintenance_owner_post_tick_result_t tavrn_maintenance_owner_post_tick(
    tavrn_maintenance_t *maintenance, uint32_t now_ms)
{
    tavrn_maintenance_owner_post_tick_result_t result;

    memset(&result, 0, sizeof(result));
    result.sweep_status = TAVRN_MAINTENANCE_EXPIRY_SWEEP_INVALID;
    result.maintenance_status = TAVRN_MAINTENANCE_INVALID;
    if (!maintenance_is_initialized(maintenance)) {
        return result;
    }
    if (!sid8_active(maintenance) || maintenance->snapshot.armed == 0u) {
        result.sweep_status = TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK;
        result.maintenance_status = TAVRN_MAINTENANCE_GATED;
        result.maintenance = maintenance->snapshot;
        return result;
    }
    result.sweep_status = tavrn_maintenance_sweep_expiry(maintenance, now_ms,
                                                          &result.sweep);
    if (result.sweep_status != TAVRN_MAINTENANCE_EXPIRY_SWEEP_OK) {
        result.maintenance_status = TAVRN_MAINTENANCE_INVALID;
        return result;
    }
    result.maintenance_status = tavrn_maintenance_tick(maintenance, now_ms);
    if (tavrn_maintenance_snapshot(maintenance, &result.maintenance) !=
        TAVRN_MAINTENANCE_OK) {
        result.maintenance_status = TAVRN_MAINTENANCE_INVALID;
    }
    return result;
}
